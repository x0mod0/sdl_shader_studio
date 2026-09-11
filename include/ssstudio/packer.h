// Writes and reads the shader pack container described by PackLayout.
#ifndef SSSTUDIO_PACKER_H
#define SSSTUDIO_PACKER_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "ssstudio/pack_format.h"

namespace ssstudio {

struct PackOptions {
    PackLayout layout;
    Compression compression = Compression::LZ4;
    std::uint32_t compression_threshold = 1024;  // blobs below this stay raw
    bool strip_names = false;
    bool strip_reflection = true;
    std::string user_section;  // free-form; written when layout.include_user_section
};

struct PackStats {
    std::uint32_t shader_count = 0;
    std::uint32_t blob_count = 0;
    std::uint64_t raw_bytes = 0;
    std::uint64_t stored_bytes = 0;
    std::uint64_t total_bytes = 0;
    std::uint32_t format_mask = 0;
    std::uint32_t stage_mask = 0;
    bool compute_only = false;
};

class PackBuilder {
public:
    void add(PackShader shader);
    bool empty() const { return shaders_.empty(); }
    const std::vector<PackShader>& shaders() const { return shaders_; }

    // Returns the serialized pack. Errors (key collisions, unsupported layout,
    // oversized blobs) are appended to `out_diags` and an empty vector returned.
    std::vector<std::uint8_t> build(const PackOptions& options,
                                    Diagnostics& out_diags,
                                    PackStats* out_stats = nullptr) const;

private:
    std::vector<PackShader> shaders_;
};

// Read-side mirror, used by `ssstudio inspect`, the build verifier and the tests.
// Deliberately independent of the writer so a format bug shows up as a
// round-trip failure rather than being cancelled out by shared code.
class PackReader {
public:
    struct Entry {
        std::uint32_t key = 0;
        Stage stage = Stage::Fragment;
        Language language = Language::HLSL;
        std::string name;        // empty when names were stripped
        std::string entry_point;
        std::uint32_t num_samplers = 0;
        std::uint32_t num_storage_textures = 0;
        std::uint32_t num_storage_buffers = 0;
        std::uint32_t num_uniform_buffers = 0;
        std::uint32_t num_readwrite_storage_textures = 0;
        std::uint32_t num_readwrite_storage_buffers = 0;
        std::uint32_t compute_threads[3] = {1, 1, 1};
        std::uint32_t format_mask = 0;
        std::vector<BlobRef> blobs;
        std::string reflection_json;  // empty when stripped
    };

    bool open(const std::vector<std::uint8_t>& bytes, Diagnostics& out_diags);

    const PackLayout& layout() const { return layout_; }
    std::uint32_t flags() const { return flags_; }
    std::uint32_t format_mask() const { return format_mask_; }
    std::uint32_t stage_mask() const { return stage_mask_; }
    const std::string& user_section() const { return user_section_; }
    Compression compression() const { return compression_; }
    const std::vector<Entry>& entries() const { return entries_; }

    const Entry* find(std::uint32_t key) const;
    // Returns the decompressed blob for `key` in `format`, or empty on failure.
    std::vector<std::uint8_t> blob(std::uint32_t key, ShaderFormat format,
                                   Diagnostics& out_diags) const;

private:
    PackLayout layout_;
    Compression compression_ = Compression::None;
    std::uint32_t flags_ = 0;
    std::uint32_t format_mask_ = 0;
    std::uint32_t stage_mask_ = 0;
    std::string user_section_;
    std::vector<Entry> entries_;
    std::vector<std::uint8_t> bytes_;
};

}  // namespace ssstudio

#endif  // SSSTUDIO_PACKER_H
