// On-disk shader pack format and its customization descriptor.
//
// The layout is not fixed: magic, alignment, key width, entry ordering and the
// presence of optional sections are all configurable (App settings -> Formats,
// overridable per build profile). Whatever is chosen is recorded in the header
// and mirrored into the generated loader and docs, so a pack always describes
// itself well enough for the reference loader to reject a mismatch loudly.
#ifndef SSSTUDIO_PACK_FORMAT_H
#define SSSTUDIO_PACK_FORMAT_H

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ssstudio/reflection.h"
#include "ssstudio/types.h"

namespace ssstudio {

inline constexpr std::uint16_t kPackVersionMajor = 1;
inline constexpr std::uint16_t kPackVersionMinor = 0;

// Header flags.
enum PackFlags : std::uint32_t {
    PACK_FLAG_COMPRESSED = 1u << 0,
    PACK_FLAG_HAS_REFLECTION = 1u << 1,
    PACK_FLAG_HAS_NAMES = 1u << 2,
    PACK_FLAG_COMPUTE_ONLY = 1u << 3,
    PACK_FLAG_HAS_USER_SECTION = 1u << 4,
    PACK_FLAG_KEY16 = 1u << 5,       // keys are 16-bit (stored widened)
    PACK_FLAG_UNSORTED = 1u << 6,    // entries in manifest order: linear scan
    PACK_FLAG_BLOBS_FIRST = 1u << 7, // blob section precedes the entry table
    PACK_FLAG_PADDED64 = 1u << 8,    // 8-byte aligned header fields
};

enum class EntrySort : std::uint8_t {
    ByKey = 0,        // binary search in the loader (default)
    ByStageThenKey = 1,
    ManifestOrder = 2,  // linear scan
};

std::string_view to_string(EntrySort s);
std::optional<EntrySort> entry_sort_from_string(std::string_view s);

enum class HeaderPreset : std::uint8_t {
    Compact = 0,   // 48-byte header, 4-byte fields
    Padded64 = 1,  // 8-byte aligned fields, trivially castable on every ABI
};

std::string_view to_string(HeaderPreset p);
std::optional<HeaderPreset> header_preset_from_string(std::string_view s);

// Everything a user may change about the container.
struct PackLayout {
    char magic[4] = {'S', '3', 'P', 'K'};
    std::string extension = ".s3pack";
    HeaderPreset header_preset = HeaderPreset::Compact;
    EntrySort entry_sort = EntrySort::ByKey;
    bool blobs_before_table = false;
    bool key16 = false;
    std::uint32_t alignment = 16;  // 16 / 64 / 256
    bool include_names = true;
    bool include_reflection = false;
    bool include_user_section = false;

    std::string magic_string() const { return std::string(magic, magic + 4); }
    void set_magic(std::string_view s);
    std::uint32_t key_size() const { return key16 ? 2u : 4u; }

    // Byte size of the header and of one entry under the current preset.
    std::uint32_t header_size() const;
    std::uint32_t entry_size(std::uint32_t formats_per_entry) const;

    Diagnostics validate() const;

    // A short stable descriptor string, embedded in generated code so the
    // loader can assert at compile time that it matches the pack it ships with.
    std::string signature() const;
};

// One blob reference inside an entry.
struct BlobRef {
    ShaderFormat format = FORMAT_SPIRV;
    std::uint32_t offset = 0;
    std::uint32_t size = 0;              // stored size (compressed if flagged)
    std::uint32_t uncompressed_size = 0; // == size when not compressed
};

// Input to the packer: one shader, already compiled for every target format.
struct PackShader {
    std::string id;         // stable manifest id, e.g. "sprite_frag"
    std::string name;       // display name (may equal id)
    std::string entry_point = "main";
    Stage stage = Stage::Fragment;
    Language language = Language::HLSL;
    std::uint32_t key = 0;
    Reflection reflection;
    std::vector<std::pair<ShaderFormat, std::vector<std::uint8_t>>> blobs;
};

}  // namespace ssstudio

#endif  // SSSTUDIO_PACK_FORMAT_H
