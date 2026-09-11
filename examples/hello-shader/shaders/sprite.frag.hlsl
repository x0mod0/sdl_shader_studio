// Fragment shader.
// SDL GPU expects resources in space2 and uniform buffers in space3 for this stage.
//
// No texture is declared on purpose: nothing in project.toml binds one, and a
// sampler slot with no source behind it is a preview that renders nothing. The
// colour is procedural, from the varying and the frame uniforms.
cbuffer Frame : register(b0, space3) {
    float time;
    float2 resolution;
    float _pad;
    float4 tint;
};

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
    // 0..1 and back, twice a second. Bound to the "pulse" macro in project.toml.
    float pulse = sin(time * 2.0) * 0.5 + 0.5;

    // A plain uv gradient: red across, green down, and the pulse in blue so the
    // time uniform is visibly live. If the gradient ever comes out flat, the
    // varying at TEXCOORD0 is not arriving.
    float3 color = float3(uv, pulse);

    return float4(color * tint.rgb, tint.a);
}
