// Vertex shader.
// SDL GPU expects resources in space0 and uniform buffers in space1 for this stage.
struct Input {
    float3 position : POSITION;
    float2 uv       : TEXCOORD0;
};

struct Output {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

cbuffer Transform : register(b0, space1) {
    float4x4 mvp;
};

Output main(Input input) {
    Output output;
    output.position = mul(mvp, float4(input.position, 1.0));
    output.uv = input.uv;
    return output;
}
