// Starter sources and cross-stage validation.
//
// Templates matter more than they look: they are where a new user learns the
// SDL register-space rules, so each one is correct by construction and says why.
#include "ssstudio/templates.h"

#include <algorithm>
#include <map>
#include <sstream>

namespace ssstudio {
namespace {

const char* hlsl_vertex = R"(// Vertex shader.
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
)";

const char* hlsl_fragment = R"(// Fragment shader.
// SDL GPU expects resources in space2 and uniform buffers in space3 for this stage.
Texture2D<float4> albedo : register(t0, space2);
SamplerState albedo_sampler : register(s0, space2);

cbuffer Frame : register(b0, space3) {
    float time;
    float2 resolution;
    float _pad;
    float4 tint;
};

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
    float4 color = albedo.Sample(albedo_sampler, uv);
    float pulse = sin(time * 2.0) * 0.5 + 0.5;
    return color * tint * lerp(0.75, 1.0, pulse);
}
)";

const char* hlsl_compute = R"(// Compute shader.
// Read-only resources go in space0, read-write in space1, uniforms in space2.
Texture2D<float4> source : register(t0, space0);
RWTexture2D<float4> destination : register(u0, space1);

cbuffer Params : register(b0, space2) {
    float2 texel_size;
    float strength;
    float _pad;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    float4 color = source.Load(int3(id.xy, 0));
    destination[id.xy] = lerp(color, 1.0 - color, strength);
}
)";

const char* glsl_vertex = R"(#version 450
// Vertex shader (Vulkan GLSL).
// The set numbers below are what SDL GPU expects: set 0 for resources, set 1 for
// uniform buffers in the vertex stage.
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec2 in_uv;

layout(location = 0) out vec2 v_uv;

layout(set = 1, binding = 0) uniform Transform {
    mat4 mvp;
} transform;

void main() {
    gl_Position = transform.mvp * vec4(in_position, 1.0);
    v_uv = in_uv;
}
)";

const char* glsl_fragment = R"(#version 450
// Fragment shader (Vulkan GLSL).
// Set 2 for resources, set 3 for uniform buffers.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D albedo;

layout(set = 3, binding = 0) uniform Frame {
    float time;
    vec2 resolution;
    float _pad;
    vec4 tint;
} frame;

void main() {
    vec4 color = texture(albedo, v_uv);
    float pulse = sin(frame.time * 2.0) * 0.5 + 0.5;
    out_color = color * frame.tint * mix(0.75, 1.0, pulse);
}
)";

const char* glsl_compute = R"(#version 450
// Compute shader (Vulkan GLSL).
// Read-only in set 0, read-write in set 1, uniforms in set 2.
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source;
layout(set = 1, binding = 0, rgba8) uniform writeonly image2D destination;

layout(set = 2, binding = 0) uniform Params {
    vec2 texel_size;
    float strength;
    float _pad;
} params;

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    vec4 color = texelFetch(source, id, 0);
    imageStore(destination, id, mix(color, 1.0 - color, params.strength));
}
)";

const char* hlsl_fullscreen_vertex = R"(// Covers the screen with a single triangle built from the vertex id, so the
// draw needs no vertex buffer and no input layout. Writes no varyings: the
// fragment shader paired with this one reads SV_Position instead.
float4 main(uint vertex_id : SV_VertexID) : SV_Position {
    // (0,0) (2,0) (0,2) in UV space covers the screen with one triangle.
    float2 uv = float2((vertex_id << 1) & 2, vertex_id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
)";

const char* glsl_fullscreen_vertex = R"(#version 450
// Covers the screen with a single triangle built from the vertex index, so the
// draw needs no vertex buffer and no input layout. Writes no varyings: the
// fragment shader paired with this one reads gl_FragCoord instead.
void main() {
    // (0,0) (2,0) (0,2) in UV space covers the screen with one triangle.
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);
}
)";

Diagnostic mismatch(std::string message) {
    Diagnostic d;
    d.severity = Severity::Error;
    d.code = "SSSTUDIO-VARYING";
    d.message = std::move(message);
    return d;
}

}  // namespace

std::string default_source(Stage stage, Language language) {
    if (language == Language::GLSL) {
        switch (stage) {
            case Stage::Vertex: return glsl_vertex;
            case Stage::Fragment: return glsl_fragment;
            case Stage::Compute: return glsl_compute;
        }
    }
    switch (stage) {
        case Stage::Vertex: return hlsl_vertex;
        case Stage::Fragment: return hlsl_fragment;
        case Stage::Compute: return hlsl_compute;
    }
    return hlsl_fragment;
}

std::string fullscreen_vertex_source(Language language) {
    return language == Language::GLSL ? glsl_fullscreen_vertex : hlsl_fullscreen_vertex;
}

std::string stage_suffix(Stage stage) {
    switch (stage) {
        case Stage::Vertex: return "_vert";
        case Stage::Fragment: return "_frag";
        case Stage::Compute: return "_comp";
    }
    return "_frag";
}

std::string id_suffix(Stage stage, Language language) {
    return stage_suffix(stage) + (language == Language::GLSL ? "_glsl" : "_hlsl");
}

namespace {

/// Whether `id` ends in `tail`. Strictly longer, never equal: an id that is
/// nothing but the suffix carries no name of its own, and treating it as
/// already-suffixed would leave an empty name behind.
bool ends_with(const std::string& id, const std::string& tail) {
    return id.size() > tail.size() && id.compare(id.size() - tail.size(), tail.size(), tail) == 0;
}

}  // namespace

std::string shader_id_for(const std::string& name, Stage stage, Language language) {
    if (name.empty()) return name;
    const std::string tail = id_suffix(stage, language);
    return ends_with(name, tail) ? name : name + tail;
}

std::string shader_basename(const std::string& id, Stage stage, Language language) {
    const std::string tail = id_suffix(stage, language);
    if (ends_with(id, tail)) return id.substr(0, id.size() - tail.size());
    // Ids from before the language joined the convention, and the ones
    // scaffold_project() still writes, carry the stage alone.
    const std::string stage_only = stage_suffix(stage);
    if (ends_with(id, stage_only)) return id.substr(0, id.size() - stage_only.size());
    return id;
}

std::string default_extension(Stage stage, Language language) {
    const char* suffix = language == Language::GLSL ? ".glsl" : ".hlsl";
    switch (stage) {
        case Stage::Vertex: return std::string(".vert") + suffix;
        case Stage::Fragment: return std::string(".frag") + suffix;
        case Stage::Compute: return std::string(".comp") + suffix;
    }
    return std::string(".frag") + suffix;
}

Diagnostics validate_varyings(const Reflection& vertex, const Reflection& fragment,
                              const std::string& file) {
    Diagnostics out;
    if (vertex.stage != Stage::Vertex || fragment.stage != Stage::Fragment) return out;

    // Cross-language projects are allowed, so varyings are matched by location
    // rather than by name: HLSL semantics and GLSL names will not agree, and the
    // hardware only cares about the location anyway.
    std::map<std::uint32_t, const VertexInput*> produced;
    for (const auto& v : vertex.outputs_as_varyings) produced[v.location] = &v;

    for (const auto& consumed : fragment.inputs_as_varyings) {
        auto it = produced.find(consumed.location);
        if (it == produced.end()) {
            out.push_back(mismatch("the fragment shader reads varying location " +
                                   std::to_string(consumed.location) + " ('" + consumed.name +
                                   "'), which the vertex shader does not write"));
            continue;
        }
        if (it->second->components != consumed.components) {
            out.push_back(mismatch("varying location " + std::to_string(consumed.location) +
                                   " is written as " + std::to_string(it->second->components) +
                                   " component(s) but read as " +
                                   std::to_string(consumed.components)));
        }
        if (it->second->type != consumed.type) {
            out.push_back(mismatch("varying location " + std::to_string(consumed.location) +
                                   " changes scalar type between stages (" +
                                   std::string(to_string(it->second->type)) + " -> " +
                                   std::string(to_string(consumed.type)) + ")"));
        }
    }

    for (const auto& [location, v] : produced) {
        const bool consumed =
            std::any_of(fragment.inputs_as_varyings.begin(), fragment.inputs_as_varyings.end(),
                        [&](const VertexInput& in) { return in.location == location; });
        if (consumed) continue;
        Diagnostic d;
        d.severity = Severity::Warning;
        d.code = "SSSTUDIO-VARYING";
        d.file = file;
        d.message = "the vertex shader writes varying location " + std::to_string(location) +
                    " ('" + v->name + "') that no fragment shader reads";
        out.push_back(std::move(d));
    }

    for (auto& d : out) {
        if (d.file.empty()) d.file = file;
    }
    return out;
}

}  // namespace ssstudio
