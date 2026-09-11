// Pass 3 of 3 - the pass that draws what you see.
//
// Named as the pipeline's `fragment` rather than as one of its `passes`, which
// is how a chain says which pass is final: exactly once, so it can be neither
// missing nor doubled.
//
// It reads both earlier passes - the sharp accumulation buffer and the blurred
// copy of it - which is the other half of why they are separate targets.
Texture2D<float4> scene : register(t0, space2);
SamplerState scene_sampler : register(s0, space2);
Texture2D<float4> bloom : register(t1, space2);
SamplerState bloom_sampler : register(s1, space2);

cbuffer Frame : register(b0, space3) {
    float2 resolution;
    float bloom_strength;
    float exposure;
};

float4 main(float4 position : SV_Position) : SV_Target0 {
    const float2 uv = position.xy / resolution;

    float3 color = scene.Sample(scene_sampler, uv).rgb;
    color += bloom.Sample(bloom_sampler, uv).rgb * bloom_strength;
    color *= exposure;

    // The presented target is always rgba8 - it is shown rather than sampled.
    // Everything upstream is rgba16f and holds values well above 1, so the
    // range has to be brought down somewhere, and this is the only pass that
    // knows it is the last one. Reinhard: cheap, and it never clips.
    color = color / (1.0 + color);

    // A little vignette, purely so the corners do not compete with the trails.
    const float2 centered = uv * 2.0 - 1.0;
    color *= 1.0 - 0.25 * dot(centered, centered);

    // The swapchain is not sRGB, so the curve is applied here rather than by
    // the format. Doing both is the classic way to end up washed out.
    color = pow(max(color, 0.0), 1.0 / 2.2);

    return float4(color, 1.0);
}
