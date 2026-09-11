#include <algorithm>
#include <cstring>

#include "ssstudio/reflection.h"
#include "ssstudio/spirv_reflect.h"
#include "test.h"

using namespace ssstudio;

namespace {

Reflection graphics_reflection(Stage stage, std::uint32_t resource_space,
                               std::uint32_t uniform_space) {
    Reflection r;
    r.stage = stage;
    Resource tex;
    tex.name = "albedo";
    tex.kind = ResourceKind::SampledTexture;
    tex.set = resource_space;
    r.resources.push_back(tex);

    UniformBlock b;
    b.name = "Frame";
    b.set = uniform_space;
    b.size = 16;
    r.uniform_blocks.push_back(b);
    return r;
}

}  // namespace

TEST(binding_model_accepts_sdl_register_spaces) {
    CHECK(!has_errors(graphics_reflection(Stage::Vertex, 0, 1).validate_binding_model("x")));
    CHECK(!has_errors(graphics_reflection(Stage::Fragment, 2, 3).validate_binding_model("x")));
}

TEST(binding_model_rejects_wrong_spaces) {
    CHECK(has_errors(graphics_reflection(Stage::Fragment, 0, 1).validate_binding_model("x")));
    CHECK(has_errors(graphics_reflection(Stage::Vertex, 2, 3).validate_binding_model("x")));
}

TEST(compute_read_write_split_matches_sdl_expectations) {
    Reflection r;
    r.stage = Stage::Compute;

    Resource ro;
    ro.name = "src";
    ro.kind = ResourceKind::StorageTexture;
    ro.set = 0;
    ro.writable = false;
    r.resources.push_back(ro);

    Resource rw;
    rw.name = "dst";
    rw.kind = ResourceKind::StorageTexture;
    rw.set = 1;
    rw.writable = true;
    r.resources.push_back(rw);

    CHECK(!has_errors(r.validate_binding_model("x")));
    CHECK_EQ(r.num_storage_textures(), 2u);
    CHECK_EQ(r.num_readonly_storage_textures(), 1u);
    CHECK_EQ(r.num_readwrite_storage_textures(), 1u);
}

TEST(compute_rejects_readwrite_in_space0) {
    Reflection r;
    r.stage = Stage::Compute;
    Resource rw;
    rw.name = "dst";
    rw.kind = ResourceKind::StorageTexture;
    rw.set = 0;
    rw.writable = true;
    r.resources.push_back(rw);
    CHECK(has_errors(r.validate_binding_model("x")));
}

TEST(sampler_counts_include_arrays) {
    Reflection r;
    Resource tex;
    tex.kind = ResourceKind::SampledTexture;
    tex.array_size = 4;
    r.resources.push_back(tex);
    CHECK_EQ(r.num_samplers(), 4u);
}

TEST(vertex_formats_map_to_sdl_enums) {
    VertexInput v;
    v.type = ScalarType::Float;
    v.components = 3;
    CHECK_STREQ(v.sdl_vertex_format(), "SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3");
    v.components = 1;
    CHECK_STREQ(v.sdl_vertex_format(), "SDL_GPU_VERTEXELEMENTFORMAT_FLOAT");
    v.type = ScalarType::UInt;
    v.components = 4;
    CHECK_STREQ(v.sdl_vertex_format(), "SDL_GPU_VERTEXELEMENTFORMAT_UINT4");
}

TEST(reflection_json_contains_the_essentials) {
    Reflection r = graphics_reflection(Stage::Fragment, 2, 3);
    const std::string json = reflection_to_json(r);
    CHECK(json.find("\"stage\":\"fragment\"") != std::string::npos);
    CHECK(json.find("albedo") != std::string::npos);
    CHECK(json.find("\"uniform_buffers\":1") != std::string::npos);
}

TEST(spirv_reflector_rejects_garbage_without_crashing) {
    Diagnostics d;
    const std::vector<std::uint8_t> junk(64, 0xAB);
    const Reflection r = reflect_spirv(junk, Stage::Fragment, "main", d);
    CHECK(!d.empty());
    CHECK(r.resources.empty());
}

TEST(spirv_reflector_reads_a_minimal_module) {
    // Hand-assembled SPIR-V: one uniform-constant sampled image at set 2, binding 0.
    const std::uint32_t words[] = {
        0x07230203u, 0x00010000u, 0x00080001u, 20u, 0u,            // header
        (4u << 16) | 5u,  1u, 0x65706f68u, 0u,                      // OpName %1 "hope"
        (4u << 16) | 71u, 1u, 34u, 2u,                              // OpDecorate %1 DescriptorSet 2
        (4u << 16) | 71u, 1u, 33u, 0u,                              // OpDecorate %1 Binding 0
        (3u << 16) | 22u, 10u, 32u,                                 // OpTypeFloat %10 32
        (9u << 16) | 25u, 11u, 10u, 1u, 0u, 0u, 0u, 1u, 0u,         // OpTypeImage %11 (2D, sampled)
        (3u << 16) | 27u, 12u, 11u,                                 // OpTypeSampledImage %12 %11
        (4u << 16) | 32u, 13u, 0u, 12u,                             // OpTypePointer %13 UniformConstant %12
        (4u << 16) | 59u, 13u, 1u, 0u,                              // OpVariable %13 %1 UniformConstant
    };
    std::vector<std::uint8_t> spirv(sizeof(words));
    std::memcpy(spirv.data(), words, sizeof(words));

    Diagnostics d;
    const Reflection r = reflect_spirv(spirv, Stage::Fragment, "main", d);
    CHECK_EQ(r.resources.size(), std::size_t{1});
    if (r.resources.empty()) return;
    CHECK_STREQ(r.resources[0].name, "hope");
    CHECK_EQ(r.resources[0].set, 2u);
    CHECK_EQ(r.resources[0].binding, 0u);
    CHECK(r.resources[0].kind == ResourceKind::SampledTexture);
}

// ---------------------------------------------------------------------------
// Binding keys for resources that occupy more than one slot
// ---------------------------------------------------------------------------

namespace {

Resource sampled(std::string name, std::uint32_t binding, std::uint32_t array_size = 0) {
    Resource r;
    r.name = std::move(name);
    r.kind = ResourceKind::SampledTexture;
    r.binding = binding;
    r.array_size = array_size;
    return r;
}

}  // namespace

TEST(resource_slot_count_treats_zero_as_one) {
    CHECK_EQ(resource_slot_count(sampled("tex", 0)), 1u);
    CHECK_EQ(resource_slot_count(sampled("channels", 0, 3)), 3u);
}

TEST(resource_binding_key_leaves_a_plain_name_alone) {
    // Nothing already written into a manifest changes shape.
    CHECK_STREQ(resource_binding_key(sampled("albedo", 0), 0), "albedo");
}

TEST(resource_binding_key_subscripts_an_array) {
    const Resource channels = sampled("channels", 0, 3);
    CHECK_STREQ(resource_binding_key(channels, 0), "channels[0]");
    CHECK_STREQ(resource_binding_key(channels, 2), "channels[2]");
}

TEST(resource_binding_key_agrees_across_the_two_front_ends) {
    // The front ends disagree about arrays, and this is where they are made to
    // agree. glslang gives one resource with an array size; DXC gives resources
    // already named with the subscript and an array size of zero. Both have to
    // produce the same keys, or a manifest written against one language would
    // silently bind nothing against the other.
    const Resource from_glsl = sampled("channels", 0, 3);
    const Resource from_hlsl_element_0 = sampled("channels[0]", 0);
    const Resource from_hlsl_element_2 = sampled("channels[2]", 2);

    CHECK_STREQ(resource_binding_key(from_glsl, 0), resource_binding_key(from_hlsl_element_0, 0));
    CHECK_STREQ(resource_binding_key(from_glsl, 2), resource_binding_key(from_hlsl_element_2, 0));
}

TEST(resource_slots_do_not_overlap_across_an_array) {
    // What the renderer sizes its bind table from: the last slot anything
    // reaches, counting an array as the several slots it occupies.
    const Resource channels = sampled("channels", 0, 3);
    const Resource lone = sampled("lone", 3);

    std::uint32_t highest = 0;
    for (const Resource* r : {&channels, &lone}) {
        highest = std::max(highest, r->binding + resource_slot_count(*r));
    }
    CHECK_EQ(highest, 4u);
}
