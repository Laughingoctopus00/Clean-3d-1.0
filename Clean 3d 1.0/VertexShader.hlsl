// VertexShader.hlsl
struct VSInput
{
    float3 position : POSITION;
    float2 uv : TEXCOORD;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.position = float4(input.position, 1.0);
    output.uv = input.uv;
    return output;
}