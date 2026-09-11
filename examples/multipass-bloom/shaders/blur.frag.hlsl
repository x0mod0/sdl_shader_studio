// Pass 2 of 3 - the bloom blur.
//
// Reads pass 1's target with source = "pass_output". That pass ran earlier this
// frame, so this is a plain read of what it just wrote: no second copy, no
// parity, nothing alternating. Only the backwards read in pass 1 costs that.
//
// This is the pass that justifies the chain. A blur needs every neighbouring
// pixel's finished value, and a fragment shader cannot see what the rest of its
// own draw produced - so the thing being blurred has to be a target somebody
// else already finished writing.
Texture2D<float4> source : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

cbuffer Blur : register(b0, space3) {
    float2 resolution;
    /// Tap spacing in pixels. Bound manually in project.toml, so the bloom can
    /// be widened without touching this file.
    float radius;
    /// Brightness a pixel has to exceed before it blooms at all. The trails
    /// buffer holds values above 1 on purpose; this is what separates the cores
    /// from the tails they leave behind.
    float threshold;
};

// A 1D gaussian, applied as an outer product below. Sums to 1, so the 2D kernel
// does too and the blur neither brightens nor dims what it spreads.
static const float kWeight[5] = {0.0625, 0.25, 0.375, 0.25, 0.0625};

float4 main(float4 position : SV_Position) : SV_Target0 {
    float3 sum = 0.0;
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            const float2 uv = (position.xy + float2(x, y) * radius) / resolution;
            // Thresholded per tap rather than after the sum: subtracting from
            // the blurred total would let a wide field of dim pixels add up to a
            // highlight that is not in the image.
            const float3 tap = max(source.Sample(source_sampler, uv).rgb - threshold, 0.0);
            sum += tap * (kWeight[x + 2] * kWeight[y + 2]);
        }
    }
    return float4(sum, 1.0);
}
