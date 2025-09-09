#define THREAD_GROUP_SIZE_X 16
#define THREAD_GROUP_SIZE_Y 16

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
    uint enable_parallax_barrier;
    uint enable_lenticular;
    float strip_width;
    float barrier_width;
    float lens_width;
    float eye_separation;
    float screen_width;
    float screen_height;
    float head_offset_x;
    float fog_density;
    float fog_color_r;
    float fog_color_g;
    float fog_color_b;
    float fog_scatter;
    uint enable_volumetric_fog;
    uint enable_raytracing;
};

Texture2D<float> DepthTexture : register(t0);
RWTexture2D<float4> FogScatteringTexture : register(u0);
SamplerState LinearSampler : register(s0);

[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, 1)]
void FogCSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 pixelCoord = dispatchThreadID.xy;
    
    uint width, height;
    DepthTexture.GetDimensions(width, height);
    
    if (pixelCoord.x >= width || pixelCoord.y >= height)
        return;

    float2 uv = float2(pixelCoord.x / (float) width, pixelCoord.y / (float) height);
    float depth = DepthTexture.SampleLevel(LinearSampler, uv, 0);
    float3 fog_color = float3(fog_color_r, fog_color_g, fog_color_b);

    float scatter = 0.0;
    int numSteps = clamp(8 + processing_quality * 4, 8, 24);
    float stepSize = 1.0 / numSteps;
    float3 pos = float3(uv, 0.0);
    float3 lightDir = normalize(float3(0.0, 0.0, -1.0));

    [unroll(24)]
    for (int i = 0; i < numSteps; i++)
    {
        float t = i * stepSize;
        if (t >= depth)
            break;

        pos.xy = clamp(pos.xy, 0.0, 1.0);
        float sampleDepth = DepthTexture.SampleLevel(LinearSampler, pos.xy, 0);
        if (sampleDepth < pos.z)
            break;

        scatter += fog_density * exp(-sampleDepth * 2.0) * stepSize;
        pos += lightDir * stepSize;
    }

    FogScatteringTexture[pixelCoord] = float4(fog_color * fog_scatter * scatter, scatter);
}