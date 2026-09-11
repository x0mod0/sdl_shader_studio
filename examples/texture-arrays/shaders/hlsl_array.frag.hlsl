// Three textures under one name, in HLSL.
//
// DXC flattens this: the compiled module declares channels[0], channels[1] and
// channels[2] as separate resources at registers t0, t1 and t2, which is why
// the Inputs & Outputs panel shows three rows rather than one.
//
// The screen is split into three bands, one per element. Bound correctly you get
// red, green, blue from left to right, with one, two and three white bars. Any
// other order means a slot went to the wrong place; three identical bands mean
// only the first element is being bound at all.
Texture2D<float4> channels[3] : register(t0, space2);
SamplerState channel_samplers[3] : register(s0, space2);

cbuffer Frame : register(b0, space3) {
    // Picked up from the `resolution` macro by name, with nothing to configure.
    float2 resolution;
};

float4 main(float4 position : SV_Position) : SV_Target0 {
    float2 uv = position.xy / max(resolution, float2(1.0, 1.0));
    float2 local = float2(frac(uv.x * 3.0), uv.y);

    // Indexed explicitly rather than dynamically: a dynamic index into a
    // texture array needs descriptor indexing, which is a bigger ask than this
    // is trying to demonstrate.
    int band = clamp(int(uv.x * 3.0), 0, 2);
    if (band == 0) return channels[0].Sample(channel_samplers[0], local);
    if (band == 1) return channels[1].Sample(channel_samplers[1], local);
    return channels[2].Sample(channel_samplers[2], local);
}
