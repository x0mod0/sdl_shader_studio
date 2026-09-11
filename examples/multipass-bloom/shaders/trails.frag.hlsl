// Pass 1 of 3 - the accumulation buffer.
//
// SDL GPU expects resources in space2 and uniform buffers in space3 for this
// stage.
//
// `feedback` is bound to this very pass with source = "previous_frame". That is
// not a trick: a pass cannot sample the target it is drawing into, so a read of
// itself can only mean the frame before. The scheduler spots it, gives this pass
// two targets instead of one, and alternates which is written by frame parity -
// so the sample below is always last frame's result, and nothing aliases.
//
// The target is rgba16f, so the values written here are free to go well above 1.
// The blur pass downstream wants a real highlight to spread, not a clipped one.
Texture2D<float4> feedback : register(t0, space2);
SamplerState feedback_sampler : register(s0, space2);

cbuffer Frame : register(b0, space3) {
    float2 resolution;
    float time;
    float delta_time;
};

static const uint kEmitters = 3;

/// Seconds for a trail to fade to half its brightness. Time rather than a
/// per-frame factor, so the trail looks the same at 30fps and at 144.
static const float kHalfLife = 0.45;

// Where emitter `i` is this frame, in 0..1 across the buffer. Three orbits at
// different rates, so they cross each other rather than travel together.
float2 emitter_position(uint i, float t) {
    const float phase = float(i) * 2.399963;  // golden angle, keeps them apart
    const float rate = 0.6 + 0.17 * float(i);
    return float2(0.5 + 0.34 * sin(t * rate + phase),
                  0.5 + 0.28 * cos(t * rate * 1.37 + phase * 1.7));
}

// Far enough apart in hue that two overlapping trails read as a third colour
// rather than as one brighter blob.
float3 emitter_color(uint i) {
    const float3 palette[3] = {
        float3(1.00, 0.35, 0.12),
        float3(0.15, 0.50, 1.00),
        float3(0.30, 1.00, 0.45),
    };
    return palette[i];
}

float4 main(float4 position : SV_Position) : SV_Target0 {
    const float2 uv = position.xy / resolution;
    const float aspect = resolution.x / resolution.y;

    // This frame's contribution: a tight, bright core per emitter. Corrected for
    // aspect so the cores stay round when the preview panel is not square.
    float3 emitted = 0.0;
    for (uint i = 0; i < kEmitters; ++i) {
        const float2 delta = (uv - emitter_position(i, time)) * float2(aspect, 1.0);
        emitted += emitter_color(i) * exp(-dot(delta, delta) * 900.0) * 6.0;
    }

    const float3 history = feedback.Sample(feedback_sampler, uv).rgb;

    // Half the trail every kHalfLife seconds, whatever the frame rate. exp2 of
    // a negative ratio is the frame-rate-independent form: two half-length
    // frames fade by exactly as much as one whole one, which a constant
    // per-frame factor does not manage.
    const float decay = exp2(-delta_time / kHalfLife);

    // A purely multiplicative fade approaches zero without ever reaching it, so
    // the oldest trail would linger as a dim smear indefinitely. The floor is
    // per-second for the same reason the decay is, and max() keeps it from
    // pulling a channel negative once it has arrived.
    const float3 faded = max(history * decay - 0.02 * delta_time, 0.0);

    // Additive rather than a lerp: the emitters are light being added to what is
    // already there, which is what lets a crossing point read brighter than
    // either trail alone. Alpha is data nobody downstream reads, so it stays 1
    // rather than accumulating.
    return float4(faded + emitted, 1.0);
}
