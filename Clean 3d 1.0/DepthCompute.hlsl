// DepthCompute.hlsl
#define THREAD_GROUP_SIZE_X 16
#define THREAD_GROUP_SIZE_Y 16

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
RWTexture2D<float> DepthTexture : register(u0);
SamplerState LinearSampler : register(s0);

[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 pixelCoord = dispatchThreadID.xy;
    
    // Get texture dimensions
    uint width, height;
    ScreenTexture.GetDimensions(width, height);
    
    if (pixelCoord.x >= width || pixelCoord.y >= height)
        return;

    // Sample color from screen texture
    float4 color = ScreenTexture.SampleLevel(LinearSampler, float2(pixelCoord.x / (float) width, pixelCoord.y / (float) height), 0);
    
    // Simple depth calculation based on luminance
    float luminance = dot(color.rgb, float3(0.299, 0.587, 0.114));
    float depth = 1.0 - luminance; // Invert so brighter = closer
    
    // Apply depth intensity
    depth = pow(depth, depth_intensity);
    
    // Edge detection (simple sobel filter)
    float edge = 0;
    if (edge_depth_influence > 0)
    {
        float4 left = ScreenTexture.SampleLevel(LinearSampler, float2((pixelCoord.x - 1) / (float) width, pixelCoord.y / (float) height), 0);
        float4 right = ScreenTexture.SampleLevel(LinearSampler, float2((pixelCoord.x + 1) / (float) width, pixelCoord.y / (float) height), 0);
        float4 up = ScreenTexture.SampleLevel(LinearSampler, float2(pixelCoord.x / (float) width, (pixelCoord.y - 1) / (float) height), 0);
        float4 down = ScreenTexture.SampleLevel(LinearSampler, float2(pixelCoord.x / (float) width, (pixelCoord.y + 1) / (float) height), 0);
        
        edge = abs(luminance - dot(left.rgb, float3(0.299, 0.587, 0.114))) +
               abs(luminance - dot(right.rgb, float3(0.299, 0.587, 0.114))) +
               abs(luminance - dot(up.rgb, float3(0.299, 0.587, 0.114))) +
               abs(luminance - dot(down.rgb, float3(0.299, 0.587, 0.114)));
        edge = saturate(edge * edge_depth_influence);
    }
    
    // Combine depth and edge information
    DepthTexture[pixelCoord] = lerp(depth, edge, 0.5);
}