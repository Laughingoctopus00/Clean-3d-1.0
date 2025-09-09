// PixelShader.hlsl
Texture2D LeftEyeTex : register(t0);
Texture2D RightEyeTex : register(t1);
Texture2D<float4> FogScatteringTex : register(t2);
SamplerState Sampler : register(s0);

cbuffer IllusionConfig : register(b0)
{
    float depth_intensity;
    float parallax_strength;
    float alpha;
    float edge_depth_influence;
    float color_separation;
    float perspective_strength;
    uint enable_gpu;
    int processing_quality;
    uint enable_chromatic;
    uint enable_parallax;
    uint enable_dof;
    float time;
    float occlusion_strength;
    float wiggle_frequency;

    // Volumetric fog params
    float fog_density;
    float fog_color_r;
    float fog_color_g;
    float fog_color_b;
    float fog_scatter;
    float fog_anisotropy;
    float fog_height_falloff;
    float temporal_blend;

    // New outline controls
    float outline_width;
    float outline_intensity;
    uint enable_parallax_barrier;
    uint enable_lenticular;
    uint enable_volumetric_fog;
    float strip_width;
    float barrier_width;
    float lens_width;
    float eye_separation;
    float screen_width;
    float screen_height;
    float head_offset_x;

    // Test pattern toggle
    uint enable_test_pattern; // 0 = normal, 1 = test pattern

    // Parallax-barrier specific controls
    float barrier_opacity; // 0.0 = no darkening, 1.0 = full black
    float pixel_pitch_mm; // physical pixel pitch in mm

    // Software stripe controls (pixels)
    float stripe_width_px;   // width of one stripe in pixels
    float stripe_offset_px;  // horizontal offset of stripes in pixels
};

struct PSInput
{
    float4 Position : SV_POSITION;
    float2 UV : TEXCOORD0;
};

// Simple HSV -> RGB helper
float3 HSVtoRGB(float h, float s, float v)
{
    float3 k = float3(6.0, 2.0, 4.0);
    float3 p = abs(frac(h + k / 6.0) * 6.0 - 3.0) - 1.0;
    p = saturate(p);
    return v * lerp(float3(1,1,1), p, s);
}

// Gamma correct blend (src over dest with gamma-aware linear blending)
float3 GammaBlend(float3 src, float srcA, float3 dest)
{
    // convert to linear
    float3 s = pow(src, 2.2);
    float3 d = pow(dest, 2.2);
    float3 outL = s * srcA + d * (1.0 - srcA);
    return pow(outL, 1.0/2.2);
}

// Sobel edge detection using reduced samples
float EdgeSobel(float2 uv, float2 texel)
{
    // Sample a 3x3 neighborhood (9 samples)
    float3 c00 = LeftEyeTex.Sample(Sampler, uv + texel * float2(-1, -1)).rgb;
    float3 c10 = LeftEyeTex.Sample(Sampler, uv + texel * float2(0, -1)).rgb;
    float3 c20 = LeftEyeTex.Sample(Sampler, uv + texel * float2(1, -1)).rgb;

    float3 c01 = LeftEyeTex.Sample(Sampler, uv + texel * float2(-1, 0)).rgb;
    float3 c11 = LeftEyeTex.Sample(Sampler, uv).rgb;
    float3 c21 = LeftEyeTex.Sample(Sampler, uv + texel * float2(1, 0)).rgb;

    float3 c02 = LeftEyeTex.Sample(Sampler, uv + texel * float2(-1, 1)).rgb;
    float3 c12 = LeftEyeTex.Sample(Sampler, uv + texel * float2(0, 1)).rgb;
    float3 c22 = LeftEyeTex.Sample(Sampler, uv + texel * float2(1, 1)).rgb;

    float gx = -dot(c00, float3(0.299,0.587,0.114)) - 2.0*dot(c01, float3(0.299,0.587,0.114)) - dot(c02, float3(0.299,0.587,0.114))
               + dot(c20, float3(0.299,0.587,0.114)) + 2.0*dot(c21, float3(0.299,0.587,0.114)) + dot(c22, float3(0.299,0.587,0.114));
    float gy = -dot(c00, float3(0.299,0.587,0.114)) - 2.0*dot(c10, float3(0.299,0.587,0.114)) - dot(c20, float3(0.299,0.587,0.114))
               + dot(c02, float3(0.299,0.587,0.114)) + 2.0*dot(c12, float3(0.299,0.587,0.114)) + dot(c22, float3(0.299,0.587,0.114));

    return sqrt(gx*gx + gy*gy);
}

// Overlay-only pixel shader. Enhanced parallax barrier effect for superior 3D depth.
float4 PSMain(PSInput input) : SV_TARGET
{
    float2 uv = input.UV;
    float px = uv.x * screen_width + head_offset_x + stripe_offset_px;
    float w = max(0.1f, stripe_width_px);
    int bandIndex = (int)floor(px / w);

    // Enhanced branchless selection with subpixel accuracy
    bool isLeft = ((bandIndex & 1) == 0); // true for left, false for right

    // Sample left and right eye textures with enhanced filtering
    float3 leftCol = LeftEyeTex.Sample(Sampler, uv).rgb;
    float3 rightCol = RightEyeTex.Sample(Sampler, uv).rgb;

    // Advanced color processing for better 3D perception
    // Enhance contrast slightly for better depth perception
    leftCol = pow(leftCol, 0.95f);
    rightCol = pow(rightCol, 0.95f);
    
    // Choose color based on stripe parity
    float3 outCol = isLeft ? leftCol : rightCol;

    // Enhanced stripe blending with smooth transitions
    float stripePhase = frac(px / w); // 0 to 1 within stripe
    float edgeSoftness = 0.1f; // Soften stripe edges slightly
    float stripeBlend = smoothstep(edgeSoftness, 1.0f - edgeSoftness, 
                                  isLeft ? stripePhase : (1.0f - stripePhase));
    
    // Apply enhanced gamma correction for barrier opacity
    float gamma = 2.2f;
    float effective_opacity = pow(barrier_opacity, gamma);
    
    // Advanced stripe alpha calculation with smooth falloff
    float baseAlpha = lerp(0.15f, 1.0f, effective_opacity);
    float stripeAlpha = baseAlpha * stripeBlend;
    
    // Enhanced depth-based color adjustment
    float depth = dot(outCol, float3(0.299f, 0.587f, 0.114f)); // Luminance
    float depthBoost = 1.0f + (depth - 0.5f) * 0.1f; // Slight boost for brighter areas
    outCol *= depthBoost;

    // --- Iridescent outline based on Sobel edge detection and outline params ---
    float2 texel = float2(1.0 / max(1.0, screen_width), 1.0 / max(1.0, screen_height));
    // scale texel by outline_width (in pixels)
    float2 scaledTexel = texel * max(1.0f, outline_width);

    float edgeStrength = EdgeSobel(uv, scaledTexel);

    // Map edgeStrength through edge_depth_influence and outline_intensity
    float mask = saturate(edgeStrength * (edge_depth_influence * 0.01f) );
    mask = smoothstep(0.02f, 0.8f, mask);
    float hue = frac(time * 0.05 + sin((uv.x + uv.y) * 10.0 + time * wiggle_frequency) * 0.1);
    float3 outlineColor = HSVtoRGB(hue, 0.85f, 0.9f);
    float outlineAlpha = mask * outline_intensity;

    // Composite outline using gamma-correct blend
    float3 finalCol = GammaBlend(outlineColor, outlineAlpha * 0.6f, outCol);

    // --- Cheap luminance-based volumetric fog (screen-space proxy) ---
    if (enable_volumetric_fog != 0) {
        // Sample precomputed FogScatteringTexture. It may be at a lower resolution, so sample using uv directly (sampler will handle it)
        float4 fogSample = FogScatteringTex.Sample(Sampler, uv);
        float scatter = fogSample.w; // compute wrote scatter in alpha
        float3 fogColorFromTex = fogSample.rgb;

        // Blend between pixel fog approximation and compute fog for higher quality
        float3 fogColor = float3(fog_color_r, fog_color_g, fog_color_b);

        // If compute returned a value, use it
        float useComputeBlend = saturate(temporal_blend); // temporal_blend used as a weight here
        float3 computeFog = fogColorFromTex * fog_scatter;
        float3 fallbackFog = fogColor * (1.0 - exp(-fog_density * (1.0 - depth) * 20.0));

        float3 chosenFog = lerp(fallbackFog, computeFog, useComputeBlend);

        finalCol = GammaBlend(chosenFog, saturate(scatter), finalCol);
    }

    // Final alpha calculation with improved blending
    float outA = saturate(stripeAlpha * alpha);
    finalCol = finalCol * outA;

    return float4(finalCol, outA);
}