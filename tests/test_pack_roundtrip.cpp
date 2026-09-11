// Writer/reader round trips across every customizable layout dimension.
#include <numeric>

#include "ssstudio/packer.h"
#include "test.h"

using namespace ssstudio;

namespace {

std::vector<std::uint8_t> fake_blob(std::size_t size, std::uint8_t seed) {
    std::vector<std::uint8_t> out(size);
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<std::uint8_t>((i * 31 + seed) & 0xFF);
    }
    return out;
}

PackShader make_graphics(const std::string& id, std::uint32_t key, Stage stage) {
    PackShader s;
    s.id = id;
    s.name = id;
    s.entry_point = "main";
    s.stage = stage;
    s.key = key;
    s.reflection.stage = stage;

    Resource tex;
    tex.name = "albedo";
    tex.kind = ResourceKind::SampledTexture;
    tex.set = stage == Stage::Vertex ? 0 : 2;
    tex.binding = 0;
    s.reflection.resources.push_back(tex);

    UniformBlock block;
    block.name = "Frame";
    block.set = stage == Stage::Vertex ? 1 : 3;
    block.binding = 0;
    block.size = 32;
    s.reflection.uniform_blocks.push_back(block);

    s.blobs.emplace_back(FORMAT_SPIRV, fake_blob(2048, static_cast<std::uint8_t>(key)));
    s.blobs.emplace_back(FORMAT_DXIL, fake_blob(1024, static_cast<std::uint8_t>(key + 7)));
    return s;
}

PackShader make_compute(const std::string& id, std::uint32_t key) {
    PackShader s;
    s.id = id;
    s.name = id;
    s.entry_point = "cs_main";
    s.stage = Stage::Compute;
    s.key = key;
    s.reflection.stage = Stage::Compute;
    s.reflection.compute_threads[0] = 8;
    s.reflection.compute_threads[1] = 8;
    s.reflection.compute_threads[2] = 1;

    Resource in_tex;
    in_tex.name = "source";
    in_tex.kind = ResourceKind::StorageTexture;
    in_tex.set = 0;
    in_tex.binding = 0;
    in_tex.writable = false;
    s.reflection.resources.push_back(in_tex);

    Resource out_tex;
    out_tex.name = "destination";
    out_tex.kind = ResourceKind::StorageTexture;
    out_tex.set = 1;
    out_tex.binding = 0;
    out_tex.writable = true;
    s.reflection.resources.push_back(out_tex);

    Resource out_buf;
    out_buf.name = "histogram";
    out_buf.kind = ResourceKind::StorageBuffer;
    out_buf.set = 1;
    out_buf.binding = 1;
    out_buf.writable = true;
    s.reflection.resources.push_back(out_buf);

    s.blobs.emplace_back(FORMAT_SPIRV, fake_blob(3000, static_cast<std::uint8_t>(key)));
    return s;
}

// Builds, reads back and compares every blob byte for byte.
void round_trip(const PackOptions& options, const std::vector<PackShader>& shaders) {
    PackBuilder builder;
    for (const auto& s : shaders) builder.add(s);

    Diagnostics diags;
    PackStats stats;
    const auto bytes = builder.build(options, diags, &stats);
    CHECK(!has_errors(diags));
    CHECK(!bytes.empty());
    CHECK_EQ(stats.shader_count, static_cast<std::uint32_t>(shaders.size()));

    PackReader reader;
    Diagnostics read_diags;
    CHECK(reader.open(bytes, read_diags));
    CHECK(!has_errors(read_diags));
    CHECK_EQ(reader.entries().size(), shaders.size());

    for (const auto& s : shaders) {
        const auto* entry = reader.find(s.key);
        CHECK(entry != nullptr);
        if (!entry) continue;
        CHECK_EQ(static_cast<int>(entry->stage), static_cast<int>(s.stage));
        CHECK_STREQ(entry->entry_point, s.entry_point);
        if (options.layout.include_names && !options.strip_names) {
            CHECK_STREQ(entry->name, s.name);
        } else {
            CHECK(entry->name.empty());
        }
        for (const auto& [fmt, blob] : s.blobs) {
            Diagnostics bd;
            const auto got = reader.blob(s.key, fmt, bd);
            CHECK(!has_errors(bd));
            CHECK(got == blob);
        }
    }
}

}  // namespace

TEST(pack_roundtrip_default_layout) {
    PackOptions options;
    options.compression = Compression::None;
    round_trip(options, {make_graphics("sprite_vert", 10, Stage::Vertex),
                         make_graphics("sprite_frag", 20, Stage::Fragment)});
}

TEST(pack_roundtrip_every_layout_permutation) {
    const std::uint32_t alignments[] = {16, 64, 256};
    for (std::uint32_t alignment : alignments) {
        for (int preset = 0; preset < 2; ++preset) {
            for (int blobs_first = 0; blobs_first < 2; ++blobs_first) {
                for (int key16 = 0; key16 < 2; ++key16) {
                    PackOptions options;
                    options.compression = Compression::None;
                    options.layout.alignment = alignment;
                    options.layout.header_preset =
                        preset ? HeaderPreset::Padded64 : HeaderPreset::Compact;
                    options.layout.blobs_before_table = blobs_first != 0;
                    options.layout.key16 = key16 != 0;
                    round_trip(options, {make_graphics("a_vert", 1, Stage::Vertex),
                                         make_graphics("b_frag", 2, Stage::Fragment),
                                         make_compute("c_comp", 3)});
                }
            }
        }
    }
}

TEST(pack_roundtrip_custom_magic_and_sort) {
    PackOptions options;
    options.compression = Compression::None;
    options.layout.set_magic("GAME");
    options.layout.entry_sort = EntrySort::ManifestOrder;
    round_trip(options, {make_graphics("z_frag", 900, Stage::Fragment),
                         make_graphics("a_vert", 5, Stage::Vertex)});

    // Unsorted packs must still be findable (linear scan path in the reader).
    PackBuilder builder;
    builder.add(make_graphics("z_frag", 900, Stage::Fragment));
    builder.add(make_graphics("a_vert", 5, Stage::Vertex));
    Diagnostics diags;
    const auto bytes = builder.build(options, diags, nullptr);
    PackReader reader;
    Diagnostics rd;
    CHECK(reader.open(bytes, rd));
    CHECK(reader.find(900) != nullptr);
    CHECK(reader.find(5) != nullptr);
    CHECK(reader.find(4242) == nullptr);
    CHECK_STREQ(reader.layout().magic_string(), "GAME");
}

TEST(pack_stores_compute_readwrite_counts_separately) {
    PackOptions options;
    options.compression = Compression::None;
    PackBuilder builder;
    builder.add(make_compute("blur", 77));

    Diagnostics diags;
    const auto bytes = builder.build(options, diags, nullptr);
    CHECK(!has_errors(diags));

    PackReader reader;
    Diagnostics rd;
    CHECK(reader.open(bytes, rd));
    const auto* e = reader.find(77);
    CHECK(e != nullptr);
    if (!e) return;
    // One read-only texture; one read-write texture and one read-write buffer.
    CHECK_EQ(e->num_storage_textures, 1u);
    CHECK_EQ(e->num_storage_buffers, 0u);
    CHECK_EQ(e->num_readwrite_storage_textures, 1u);
    CHECK_EQ(e->num_readwrite_storage_buffers, 1u);
    CHECK_EQ(e->compute_threads[0], 8u);
    CHECK_EQ(e->compute_threads[1], 8u);
}

TEST(pack_marks_compute_only_packs) {
    PackOptions options;
    options.compression = Compression::None;
    PackBuilder builder;
    builder.add(make_compute("a", 1));
    builder.add(make_compute("b", 2));

    Diagnostics diags;
    PackStats stats;
    const auto bytes = builder.build(options, diags, &stats);
    CHECK(stats.compute_only);

    PackReader reader;
    Diagnostics rd;
    CHECK(reader.open(bytes, rd));
    CHECK((reader.flags() & PACK_FLAG_COMPUTE_ONLY) != 0);
}

TEST(pack_rejects_key_collisions) {
    PackOptions options;
    PackBuilder builder;
    builder.add(make_graphics("a", 42, Stage::Vertex));
    builder.add(make_graphics("b", 42, Stage::Fragment));

    Diagnostics diags;
    const auto bytes = builder.build(options, diags, nullptr);
    CHECK(bytes.empty());
    CHECK(has_errors(diags));
}

TEST(pack_rejects_key_too_wide_for_key16) {
    PackOptions options;
    options.layout.key16 = true;
    PackBuilder builder;
    builder.add(make_graphics("a", 0x1FFFF, Stage::Vertex));

    Diagnostics diags;
    CHECK(builder.build(options, diags, nullptr).empty());
    CHECK(has_errors(diags));
}

TEST(pack_strips_names_when_asked) {
    PackOptions options;
    options.compression = Compression::None;
    options.strip_names = true;
    options.layout.include_names = false;
    round_trip(options, {make_graphics("secret_vert", 1, Stage::Vertex)});
}

TEST(pack_keeps_reflection_when_requested) {
    PackOptions options;
    options.compression = Compression::None;
    options.layout.include_reflection = true;
    options.strip_reflection = false;

    PackBuilder builder;
    builder.add(make_graphics("sprite_frag", 3, Stage::Fragment));
    Diagnostics diags;
    const auto bytes = builder.build(options, diags, nullptr);
    CHECK(!has_errors(diags));

    PackReader reader;
    Diagnostics rd;
    CHECK(reader.open(bytes, rd));
    const auto* e = reader.find(3);
    CHECK(e != nullptr);
    if (e) CHECK(e->reflection_json.find("albedo") != std::string::npos);
}

TEST(pack_user_section_round_trips) {
    PackOptions options;
    options.compression = Compression::None;
    options.layout.include_user_section = true;
    options.user_section = "build=ci-4711";

    PackBuilder builder;
    builder.add(make_graphics("a", 1, Stage::Vertex));
    Diagnostics diags;
    const auto bytes = builder.build(options, diags, nullptr);
    CHECK(!has_errors(diags));

    PackReader reader;
    Diagnostics rd;
    CHECK(reader.open(bytes, rd));
    CHECK_STREQ(reader.user_section(), "build=ci-4711");
}

TEST(pack_compression_is_transparent_to_the_reader) {
    PackOptions options;
    options.compression = Compression::LZ4;  // falls back to raw when unavailable
    options.compression_threshold = 16;
    round_trip(options, {make_graphics("sprite_vert", 1, Stage::Vertex),
                         make_compute("blur", 2)});
}

TEST(pack_rejects_truncated_data) {
    PackOptions options;
    options.compression = Compression::None;
    PackBuilder builder;
    builder.add(make_graphics("a", 1, Stage::Vertex));
    Diagnostics diags;
    auto bytes = builder.build(options, diags, nullptr);
    CHECK(!bytes.empty());

    bytes.resize(bytes.size() / 3);
    PackReader reader;
    Diagnostics rd;
    CHECK(!reader.open(bytes, rd));
    CHECK(has_errors(rd));
}

TEST(pack_rejects_foreign_magic) {
    std::vector<std::uint8_t> bytes(64, 0);
    const char* magic = "NOPE";
    for (int i = 0; i < 4; ++i) bytes[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(magic[i]);
    bytes[4] = 99;  // bogus version

    PackReader reader;
    Diagnostics rd;
    CHECK(!reader.open(bytes, rd));
    CHECK(has_errors(rd));
}
