Texture2D LeftTex    : register(t0);
Texture2D RightTex   : register(t1);
SamplerState LinearSampler : register(s0);

cbuffer BarrierParams : register(b1)
{
    float2 invResolution; // 8 bytes
    int bandPixels;       // 4
    float phaseOffset;    // 4
    float darkness;       // 4
    int subpixelMode;     // 4
    int temporalFlip;     // 4
    int frameIndex;       // 4
    float padding0;       // make size multiple of 16
};

struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float3 ApplySubpixel(float2 uv, float2 px, Texture2D tex)
{
    // sample per-channel offsets (0,1,2) pixels if subpixelMode enabled
    float3 col = 0;
    if (subpixelMode == 0) {
        col = tex.Sample(LinearSampler, uv).rgb;
    } else {
        float3 sampleOffsets = float3(0.0, 1.0, 2.0);
        for (int i = 0; i < 3; ++i) {
            float2 uvo = uv;
            uvo.x = (px.x + sampleOffsets[i]) * invResolution.x; // add pixel offset
            col[i] = tex.Sample(LinearSampler, uvo).rgb[i];
        }
    }
    return col;
}

float4 PSMain(VSOut input) : SV_TARGET
{
    float2 uv = input.uv;
    // Correct pixel coordinate calculation
    float2 pixelPos = uv / invResolution; // uv * (width, height)
    float x = pixelPos.x + phaseOffset;

    int band = max(1, bandPixels);
    int idx = (int)floor(x / band);
    bool showLeft = (idx % 2) == 0;

    // temporal flip
    if (temporalFlip != 0) {
        showLeft = (showLeft != ((frameIndex & 1) != 0));
    }

    float3 leftCol = LeftTex.Sample(LinearSampler, uv).rgb;
    float3 rightCol = RightTex.Sample(LinearSampler, uv).rgb;

    float3 outCol = showLeft ? leftCol : rightCol;

    // darkness: 0 = fully black blocked stripes, 1 = no block
    // Use gamma correction for darkness for perceptual steps
    float gamma = 2.2f;
    float effective_darkness = pow(darkness, gamma);
    float3 blocked = float3(0,0,0);
    float blend = showLeft ? 1.0 : effective_darkness;
    outCol = lerp(blocked, outCol, blend);

    return float4(outCol, 1.0);
}