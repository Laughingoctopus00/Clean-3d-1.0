// Effects.hlsl
cbuffer ConfigBuffer : register(b0)
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
};

Texture2D<float4> ScreenTexture : register(t0);
Texture2D<float> DepthTexture : register(t1);
SamplerState LinearSampler : register(s0);

struct PSInput
{
    float4 Position : SV_POSITION;
    float2 UV : TEXCOORD0;
};

float4 PSMain(PSInput input) : SV_TARGET
{
    float4 color = ScreenTexture.Sample(LinearSampler, input.UV);
    float depth = DepthTexture.Sample(LinearSampler, input.UV);
    
    float4 finalColor = color;
    
    // Apply parallax effect
    if (enable_parallax)
    {
        float2 parallaxOffset = float2(depth - 0.5, depth - 0.5) * parallax_strength * 0.01;
        finalColor = ScreenTexture.Sample(LinearSampler, input.UV + parallaxOffset);
    }
    
    // Apply chromatic aberration
    if (enable_chromatic)
    {
        float2 caOffset = float2(depth - 0.5, depth - 0.5) * color_separation * 0.005;
        finalColor.r = ScreenTexture.Sample(LinearSampler, input.UV + caOffset).r;
        finalColor.g = ScreenTexture.Sample(LinearSampler, input.UV).g;
        finalColor.b = ScreenTexture.Sample(LinearSampler, input.UV - caOffset).b;
    }
    
    // Apply depth of field
    if (enable_dof)
    {
        float blurAmount = abs(depth - 0.5) * processing_quality;
        float4 blurred = 0;
        int samples = processing_quality;
        for (int i = -samples; i <= samples; i++)
        {
            for (int j = -samples; j <= samples; j++)
            {
                float2 offset = float2(i, j) * blurAmount * 0.001;
                blurred += ScreenTexture.Sample(LinearSampler, input.UV + offset);
            }
        }
        blurred /= (2 * samples + 1) * (2 * samples + 1);
        finalColor = lerp(finalColor, blurred, saturate(abs(depth - 0.5) * 2));
    }
    
    // Apply alpha
    finalColor.a *= saturate(alpha / 255.0);
    
    return finalColor;
}