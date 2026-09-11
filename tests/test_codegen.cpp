#include "ssstudio/build.h"
#include "ssstudio/codegen.h"
#include "test.h"

using namespace ssstudio;

namespace {

GenInfo info_for(PackLayout layout = PackLayout{}) {
    GenInfo info;
    info.project_name = "Test";
    info.profile_name = "release";
    info.tool_version = "SDL Shader Studio test";
    info.pack_filename = "shaders" + layout.extension;
    info.layout = layout;
    info.build_time = "2026-01-01 00:00:00Z";
    return info;
}

PackShader fragment_shader() {
    PackShader s;
    s.id = "sprite_frag";
    s.name = "sprite_frag";
    s.stage = Stage::Fragment;
    s.key = 0xABCD1234u;
    s.reflection.stage = Stage::Fragment;

    UniformBlock b;
    b.name = "Frame";
    b.set = 3;
    b.binding = 0;
    b.size = 32;
    UniformMember time;
    time.name = "time";
    time.type = ScalarType::Float;
    time.offset = 0;
    time.size = 4;
    UniformMember tint;
    tint.name = "tint";
    tint.type = ScalarType::Float;
    tint.cols = 4;
    tint.offset = 16;
    tint.size = 16;
    b.members = {time, tint};
    s.reflection.uniform_blocks.push_back(b);

    Resource tex;
    tex.name = "albedo";
    tex.kind = ResourceKind::SampledTexture;
    tex.set = 2;
    tex.binding = 0;
    s.reflection.resources.push_back(tex);

    s.blobs.emplace_back(FORMAT_SPIRV, std::vector<std::uint8_t>(16, 1));
    return s;
}

PackShader vertex_shader() {
    PackShader s;
    s.id = "sprite_vert";
    s.stage = Stage::Vertex;
    s.key = 1;
    s.reflection.stage = Stage::Vertex;

    VertexInput pos;
    pos.name = "position";
    pos.semantic = "POSITION";
    pos.location = 0;
    pos.components = 3;
    VertexInput uv;
    uv.name = "uv";
    uv.semantic = "TEXCOORD0";
    uv.location = 1;
    uv.components = 2;
    s.reflection.vertex_inputs = {pos, uv};
    s.blobs.emplace_back(FORMAT_SPIRV, std::vector<std::uint8_t>(16, 2));
    return s;
}

}  // namespace

TEST(id_header_contains_keys_and_counts) {
    const auto header = generate_id_header({fragment_shader()}, info_for());
    CHECK(header.find("SHADER_SPRITE_FRAG") != std::string::npos);
    CHECK(header.find("0xABCD1234u") != std::string::npos);
    CHECK(header.find("SHADER_SPRITE_FRAG_NUM_SAMPLERS 1") != std::string::npos);
    CHECK(header.find("SHADER_SPRITE_FRAG_NUM_UNIFORM_BUFFERS 1") != std::string::npos);
}

TEST(loader_header_has_no_unsubstituted_placeholders) {
    const auto loader = generate_loader_header(info_for());
    CHECK(loader.find("@TOOL_VERSION@") == std::string::npos);
    CHECK(loader.find("@MAGIC@") == std::string::npos);
    CHECK(loader.find("@LAYOUT_SIGNATURE@") == std::string::npos);
    CHECK(loader.find("@PACK_EXT@") == std::string::npos);
    CHECK(loader.find("@VERSION_MAJOR@") == std::string::npos);
    CHECK(loader.find("S3PACK_CreateShader") != std::string::npos);
    CHECK(loader.find("namespace s3pack") != std::string::npos);
}

TEST(loader_header_bakes_in_the_custom_layout) {
    PackLayout layout;
    layout.set_magic("GAME");
    layout.extension = ".pak";
    layout.key16 = true;
    const auto loader = generate_loader_header(info_for(layout));
    CHECK(loader.find("\"GAME\"") != std::string::npos);
    CHECK(loader.find("key16") != std::string::npos);
    CHECK(loader.find(".pak") != std::string::npos);
}

TEST(uniform_struct_snippet_pads_to_the_declared_size) {
    const auto snippet = snippet_uniform_struct(fragment_shader());
    CHECK(snippet.find("typedef struct Frame") != std::string::npos);
    CHECK(snippet.find("float time") != std::string::npos);
    CHECK(snippet.find("float tint[4]") != std::string::npos);
    CHECK(snippet.find("_pad0[12]") != std::string::npos);   // 4 -> 16
    CHECK(snippet.find("sizeof(Frame) == 32") != std::string::npos);
}

TEST(vertex_input_snippet_orders_by_location_and_computes_pitch) {
    const auto snippet = snippet_vertex_input_state(vertex_shader(), info_for());
    CHECK(snippet.find("SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3") != std::string::npos);
    CHECK(snippet.find("SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2") != std::string::npos);
    CHECK(snippet.find(".offset = 12") != std::string::npos);
    CHECK(snippet.find(".pitch = 20") != std::string::npos);
}

TEST(cpp_snippet_uses_raii_and_c_snippet_does_not) {
    const auto c = snippet_create_shader(fragment_shader(), info_for(), SnippetFlavor::C);
    const auto cpp = snippet_create_shader(fragment_shader(), info_for(), SnippetFlavor::CppRaii);
    CHECK(c.find("S3PACK_CreateShader") != std::string::npos);
    CHECK(c.find("SDL_ReleaseGPUShader") != std::string::npos);
    CHECK(cpp.find("s3pack::Shader") != std::string::npos);
    CHECK(cpp.find("SDL_ReleaseGPUShader") == std::string::npos);
}

TEST(docs_cover_every_shader_and_stay_compute_aware) {
    GenInfo info = info_for();
    info.stats.compute_only = false;
    const auto docs = generate_docs({vertex_shader(), fragment_shader()}, info);
    CHECK(docs.find("### sprite_frag") != std::string::npos);
    CHECK(docs.find("### sprite_vert") != std::string::npos);
    CHECK(docs.find("Pipeline example") != std::string::npos);

    PackShader compute;
    compute.id = "blur";
    compute.stage = Stage::Compute;
    compute.reflection.stage = Stage::Compute;
    compute.reflection.compute_threads[0] = 8;
    GenInfo compute_info = info_for();
    compute_info.stats.compute_only = true;
    const auto compute_docs = generate_docs({compute}, compute_info);
    CHECK(compute_docs.find("compute-only pack") != std::string::npos);
    CHECK(compute_docs.find("Pipeline example") == std::string::npos);
    CHECK(compute_docs.find("SDL_DispatchGPUCompute") != std::string::npos);
}

TEST(meta_toml_records_the_layout) {
    PackLayout layout;
    layout.alignment = 256;
    const auto meta = generate_meta_toml({fragment_shader()}, info_for(layout));
    CHECK(meta.find("alignment = 256") != std::string::npos);
    CHECK(meta.find("id = \"sprite_frag\"") != std::string::npos);
}

// --- per-shader binaries -------------------------------------------------

TEST(a_shader_binary_is_named_after_its_source_without_the_language) {
    CHECK_STREQ(shader_binary_stem("test.frag.hlsl"), "test.frag");
    CHECK_STREQ(shader_binary_stem("shaders/sprites/blur.vert.glsl"), "blur.vert");
    // A source with no stage segment keeps its whole name.
    CHECK_STREQ(shader_binary_stem("post.hlsl"), "post");
    // And one with no extension at all is left alone rather than emptied.
    CHECK_STREQ(shader_binary_stem("plain"), "plain");
    CHECK_STREQ(shader_binary_stem(".hidden"), ".hidden");
}

TEST(one_format_leaves_the_format_out_of_the_filename) {
    CHECK_STREQ(shader_binary_filename("test.frag", FORMAT_SPIRV, true, ".bin"),
                "test.frag.bin");
    CHECK_STREQ(shader_binary_filename("test.frag", FORMAT_DXIL, true, ".bin"),
                "test.frag.bin");
}

TEST(several_formats_put_the_format_in_so_they_do_not_collide) {
    CHECK_STREQ(shader_binary_filename("test.frag", FORMAT_SPIRV, false, ".bin"),
                "test.frag.spirv.bin");
    CHECK_STREQ(shader_binary_filename("test.frag", FORMAT_MSL, false, ".bin"),
                "test.frag.msl.bin");
    // Which is the point: two formats of one shader are two different names.
    CHECK(shader_binary_filename("test.frag", FORMAT_SPIRV, false, ".bin") !=
          shader_binary_filename("test.frag", FORMAT_DXIL, false, ".bin"));
}

TEST(the_extension_is_taken_with_or_without_its_dot) {
    CHECK_STREQ(shader_binary_filename("test.frag", FORMAT_SPIRV, true, "bin"), "test.frag.bin");
    CHECK_STREQ(shader_binary_filename("test.frag", FORMAT_SPIRV, true, ".spv"), "test.frag.spv");
    CHECK_STREQ(shader_binary_filename("test.frag", FORMAT_SPIRV, true, ".shader"),
                "test.frag.shader");
    // Empty means no extension, which is the "test.frag" spelling.
    CHECK_STREQ(shader_binary_filename("test.frag", FORMAT_SPIRV, true, ""), "test.frag");
}
