// Cross-check: the pack is produced by PackBuilder and then read by the
// *generated loader* (compiled against the SDL stub), not by PackReader. A
// format change that updates only one side fails here.
#include <cstring>
#include <vector>

#define S3PACK_IMPLEMENTATION
#include "generated_s3pack.h"  // configured from templates/s3pack.h at build time

#include "ssstudio/packer.h"
#include "test.h"

using namespace ssstudio;

namespace {

std::vector<std::uint8_t> blob(std::size_t n, std::uint8_t seed) {
    std::vector<std::uint8_t> out(n);
    for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<std::uint8_t>((i * 17 + seed) & 0xFF);
    return out;
}

PackShader fragment(std::uint32_t key) {
    PackShader s;
    s.id = "sprite_frag";
    s.name = "sprite_frag";
    s.entry_point = "main";
    s.stage = Stage::Fragment;
    s.key = key;
    s.reflection.stage = Stage::Fragment;

    Resource tex;
    tex.name = "albedo";
    tex.kind = ResourceKind::SampledTexture;
    tex.set = 2;
    s.reflection.resources.push_back(tex);

    UniformBlock b;
    b.name = "Frame";
    b.set = 3;
    b.size = 16;
    s.reflection.uniform_blocks.push_back(b);

    s.blobs.emplace_back(FORMAT_SPIRV, blob(1500, 3));
    return s;
}

PackShader compute(std::uint32_t key) {
    PackShader s;
    s.id = "blur";
    s.name = "blur";
    s.entry_point = "cs_main";
    s.stage = Stage::Compute;
    s.key = key;
    s.reflection.stage = Stage::Compute;
    s.reflection.compute_threads[0] = 16;
    s.reflection.compute_threads[1] = 4;
    s.reflection.compute_threads[2] = 1;

    Resource ro;
    ro.name = "src";
    ro.kind = ResourceKind::StorageTexture;
    ro.set = 0;
    ro.writable = false;
    s.reflection.resources.push_back(ro);

    Resource rw;
    rw.name = "dst";
    rw.kind = ResourceKind::StorageTexture;
    rw.set = 1;
    rw.writable = true;
    s.reflection.resources.push_back(rw);

    s.blobs.emplace_back(FORMAT_SPIRV, blob(900, 9));
    return s;
}

std::vector<std::uint8_t> build_pack(const std::vector<PackShader>& shaders,
                                     PackLayout layout = PackLayout{}) {
    PackOptions options;
    options.layout = layout;
    options.compression = Compression::None;
    PackBuilder builder;
    for (const auto& s : shaders) builder.add(s);
    Diagnostics diags;
    return builder.build(options, diags, nullptr);
}

}  // namespace

TEST(loader_opens_a_pack_written_by_the_builder) {
    const auto bytes = build_pack({fragment(100), compute(200)});
    CHECK(!bytes.empty());

    S3PACK_Pack pack;
    CHECK(S3PACK_OpenMemory(&pack, bytes.data(), bytes.size(), false));
    CHECK_EQ(pack.shader_count, 2u);

    const S3PACK_Entry* frag = S3PACK_Find(&pack, 100);
    CHECK(frag != nullptr);
    if (frag) {
        CHECK_STREQ(frag->name ? frag->name : "", "sprite_frag");
        CHECK_STREQ(frag->entry_point ? frag->entry_point : "", "main");
        CHECK_EQ(static_cast<int>(frag->num_samplers), 1);
        CHECK_EQ(static_cast<int>(frag->num_uniform_buffers), 1);
    }
    CHECK(S3PACK_Find(&pack, 4242) == nullptr);
    S3PACK_Close(&pack);
}

TEST(loader_returns_the_exact_blob_bytes) {
    const auto shader = fragment(7);
    const auto bytes = build_pack({shader});

    S3PACK_Pack pack;
    CHECK(S3PACK_OpenMemory(&pack, bytes.data(), bytes.size(), false));

    const std::size_t needed = S3PACK_ReadBlob(&pack, 7, SDL_GPU_SHADERFORMAT_SPIRV, nullptr, 0);
    CHECK_EQ(needed, shader.blobs[0].second.size());

    std::vector<std::uint8_t> out(needed);
    CHECK_EQ(S3PACK_ReadBlob(&pack, 7, SDL_GPU_SHADERFORMAT_SPIRV, out.data(), out.size()), needed);
    CHECK(out == shader.blobs[0].second);
    S3PACK_Close(&pack);
}

TEST(loader_fills_create_info_from_the_entry) {
    const auto bytes = build_pack({fragment(11)});
    S3PACK_Pack pack;
    CHECK(S3PACK_OpenMemory(&pack, bytes.data(), bytes.size(), false));

    SSSTUDIO_StubSetSupportedFormats(SDL_GPU_SHADERFORMAT_SPIRV);
    SDL_GPUShader* shader = S3PACK_CreateShader(nullptr, &pack, 11);
    CHECK(shader != nullptr);

    const SDL_GPUShaderCreateInfo* info = SSSTUDIO_StubLastShaderInfo();
    CHECK_EQ(static_cast<int>(info->stage), static_cast<int>(SDL_GPU_SHADERSTAGE_FRAGMENT));
    CHECK_EQ(info->num_samplers, 1u);
    CHECK_EQ(info->num_uniform_buffers, 1u);
    CHECK_EQ(info->code_size, std::size_t{1500});
    CHECK_STREQ(info->entrypoint, "main");
    S3PACK_Close(&pack);
}

TEST(loader_splits_compute_storage_counts) {
    const auto bytes = build_pack({compute(21)});
    S3PACK_Pack pack;
    CHECK(S3PACK_OpenMemory(&pack, bytes.data(), bytes.size(), false));

    Uint32 threads[3] = {0, 0, 0};
    SDL_GPUComputePipeline* pipeline = S3PACK_CreateComputePipeline(nullptr, &pack, 21, threads);
    CHECK(pipeline != nullptr);
    CHECK_EQ(threads[0], 16u);
    CHECK_EQ(threads[1], 4u);

    const SDL_GPUComputePipelineCreateInfo* info = SSSTUDIO_StubLastComputeInfo();
    CHECK_EQ(info->num_readonly_storage_textures, 1u);
    CHECK_EQ(info->num_readwrite_storage_textures, 1u);
    CHECK_EQ(info->threadcount_x, 16u);
    CHECK_EQ(info->threadcount_y, 4u);
    S3PACK_Close(&pack);
}

TEST(loader_refuses_a_graphics_call_on_a_compute_shader) {
    const auto bytes = build_pack({compute(31)});
    S3PACK_Pack pack;
    CHECK(S3PACK_OpenMemory(&pack, bytes.data(), bytes.size(), false));
    CHECK(S3PACK_CreateShader(nullptr, &pack, 31) == nullptr);
    CHECK(std::strstr(S3PACK_GetError(), "compute") != nullptr);
    S3PACK_Close(&pack);
}

TEST(loader_reports_when_the_device_supports_no_stored_format) {
    const auto bytes = build_pack({fragment(41)});
    S3PACK_Pack pack;
    CHECK(S3PACK_OpenMemory(&pack, bytes.data(), bytes.size(), false));

    SSSTUDIO_StubSetSupportedFormats(SDL_GPU_SHADERFORMAT_METALLIB);
    CHECK(S3PACK_CreateShader(nullptr, &pack, 41) == nullptr);
    CHECK(std::strstr(S3PACK_GetError(), "device") != nullptr);

    SSSTUDIO_StubSetSupportedFormats(SDL_GPU_SHADERFORMAT_SPIRV);
    S3PACK_Close(&pack);
}

TEST(loader_handles_every_layout_permutation) {
    const std::uint32_t alignments[] = {16, 64, 256};
    for (std::uint32_t alignment : alignments) {
        for (int preset = 0; preset < 2; ++preset) {
            for (int blobs_first = 0; blobs_first < 2; ++blobs_first) {
                for (int key16 = 0; key16 < 2; ++key16) {
                    PackLayout layout;
                    layout.alignment = alignment;
                    layout.header_preset =
                        preset ? HeaderPreset::Padded64 : HeaderPreset::Compact;
                    layout.blobs_before_table = blobs_first != 0;
                    layout.key16 = key16 != 0;

                    const auto shader = fragment(55);
                    const auto bytes = build_pack({shader, compute(66)}, layout);
                    CHECK(!bytes.empty());

                    S3PACK_Pack pack;
                    CHECK(S3PACK_OpenMemory(&pack, bytes.data(), bytes.size(), false));
                    CHECK_EQ(pack.shader_count, 2u);

                    std::vector<std::uint8_t> out(shader.blobs[0].second.size());
                    CHECK_EQ(S3PACK_ReadBlob(&pack, 55, SDL_GPU_SHADERFORMAT_SPIRV, out.data(),
                                             out.size()),
                             out.size());
                    CHECK(out == shader.blobs[0].second);
                    S3PACK_Close(&pack);
                }
            }
        }
    }
}

TEST(loader_rejects_a_pack_with_foreign_magic) {
    auto bytes = build_pack({fragment(1)});
    bytes[0] = 'X';
    S3PACK_Pack pack;
    CHECK(!S3PACK_OpenMemory(&pack, bytes.data(), bytes.size(), false));
    CHECK(std::strstr(S3PACK_GetError(), "magic") != nullptr);
}

TEST(cpp_wrapper_moves_and_releases) {
    const auto bytes = build_pack({fragment(90)});
    SSSTUDIO_StubSetSupportedFormats(SDL_GPU_SHADERFORMAT_SPIRV);

    auto pack = s3pack::Pack::open_memory(bytes.data(), bytes.size());
    CHECK(pack.good());
    CHECK(pack->contains(90));

    s3pack::Shader shader = pack->shader(nullptr, 90);
    CHECK(static_cast<bool>(shader));

    s3pack::Shader moved = std::move(shader);
    CHECK(static_cast<bool>(moved));
    CHECK(!static_cast<bool>(shader));  // moved-from is empty, not double-released

    auto moved_pack = std::move(pack);
    CHECK(moved_pack.good());
}

TEST(cpp_compute_wrapper_computes_group_counts) {
    const auto bytes = build_pack({compute(91)});
    auto pack = s3pack::Pack::open_memory(bytes.data(), bytes.size());
    CHECK(pack.good());

    s3pack::ComputePipeline pipeline = pack->compute(nullptr, 91);
    CHECK(static_cast<bool>(pipeline));
    CHECK_EQ(pipeline.thread_count(0), 16u);
    CHECK_EQ(pipeline.groups_for(1920, 0), 120u);
    CHECK_EQ(pipeline.groups_for(1080, 1), 270u);
    CHECK_EQ(pipeline.groups_for(1, 0), 1u);
}
