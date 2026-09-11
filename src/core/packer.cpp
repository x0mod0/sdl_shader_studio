#include "ssstudio/packer.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

#include "ssstudio/compress.h"
#include "ssstudio/hash.h"

namespace ssstudio {
namespace {

// Field width under each header preset. Compact packs 4-byte fields; Padded64
// widens every field to 8 bytes so the header can be memcpy'd into a struct on
// any ABI without packing pragmas.
std::uint32_t field_size(HeaderPreset p) { return p == HeaderPreset::Padded64 ? 8u : 4u; }

void put_u8(std::vector<std::uint8_t>& b, std::uint8_t v) { b.push_back(v); }

void put_u16(std::vector<std::uint8_t>& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

void put_u32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFF));
}

// Writes a field respecting the preset width (high half is zero padding).
void put_field(std::vector<std::uint8_t>& b, std::uint32_t v, HeaderPreset p) {
    put_u32(b, v);
    if (p == HeaderPreset::Padded64) put_u32(b, 0);
}

void pad_to(std::vector<std::uint8_t>& b, std::uint32_t alignment) {
    while (b.size() % alignment != 0) b.push_back(0);
}

Diagnostic error(std::string msg, std::string code = "SSSTUDIO-PACK") {
    Diagnostic d;
    d.severity = Severity::Error;
    d.message = std::move(msg);
    d.code = std::move(code);
    return d;
}

// A string table with deduplication; offsets are 1-based so 0 can mean "absent".
class StringTable {
public:
    std::uint32_t add(const std::string& s) {
        if (s.empty()) return 0;
        auto it = offsets_.find(s);
        if (it != offsets_.end()) return it->second;
        const std::uint32_t off = static_cast<std::uint32_t>(bytes_.size()) + 1;
        bytes_.insert(bytes_.end(), s.begin(), s.end());
        bytes_.push_back(0);
        offsets_.emplace(s, off);
        return off;
    }
    const std::vector<std::uint8_t>& bytes() const { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_{0};  // leading NUL so offset 0 is reserved
    std::map<std::string, std::uint32_t> offsets_;
};

}  // namespace

// ---------------------------------------------------------------------------
// PackLayout
// ---------------------------------------------------------------------------
void PackLayout::set_magic(std::string_view s) {
    for (int i = 0; i < 4; ++i) {
        magic[i] = i < static_cast<int>(s.size()) ? s[static_cast<std::size_t>(i)] : ' ';
    }
}

std::uint32_t PackLayout::header_size() const {
    // magic(4) + version(2+2) + 11 fields
    const std::uint32_t f = field_size(header_preset);
    const std::uint32_t base = 4 + 2 + 2 + 11 * f;
    const std::uint32_t align = header_preset == HeaderPreset::Padded64 ? 8u : 4u;
    return ((base + align - 1) / align) * align;
}

std::uint32_t PackLayout::entry_size(std::uint32_t formats_per_entry) const {
    const std::uint32_t f = field_size(header_preset);
    // key + 10 packed bytes + 4 offset fields + 3 thread fields + format_mask
    std::uint32_t size = key_size() + 10 + f * 4 + f * 3 + f;
    size += formats_per_entry * (f * 3);
    const std::uint32_t align = header_preset == HeaderPreset::Padded64 ? 8u : 4u;
    return ((size + align - 1) / align) * align;
}

Diagnostics PackLayout::validate() const {
    Diagnostics out;
    if (alignment != 16 && alignment != 64 && alignment != 256) {
        out.push_back(error("pack alignment must be 16, 64 or 256 (got " +
                            std::to_string(alignment) + ")", "SSSTUDIO-LAYOUT"));
    }
    for (char c : magic) {
        if (c < 32 || c > 126) {
            out.push_back(error("pack magic must be four printable ASCII characters",
                                "SSSTUDIO-LAYOUT"));
            break;
        }
    }
    if (extension.empty() || extension[0] != '.') {
        out.push_back(error("pack extension must start with '.'", "SSSTUDIO-LAYOUT"));
    }
    if (include_reflection && !include_names) {
        Diagnostic d;
        d.severity = Severity::Warning;
        d.code = "SSSTUDIO-LAYOUT";
        d.message = "reflection is kept but names are stripped; tooling will show unnamed shaders";
        out.push_back(std::move(d));
    }
    return out;
}

std::string PackLayout::signature() const {
    // Compact, human-readable and stable: baked into generated headers so a
    // loader built for one layout refuses a pack built with another.
    std::string s = magic_string();
    s += "/v" + std::to_string(kPackVersionMajor) + "." + std::to_string(kPackVersionMinor);
    s += "/" + std::string(to_string(header_preset));
    s += "/" + std::string(to_string(entry_sort));
    s += blobs_before_table ? "/blobs_first" : "/table_first";
    s += key16 ? "/key16" : "/key32";
    s += "/align" + std::to_string(alignment);
    if (include_names) s += "/names";
    if (include_reflection) s += "/refl";
    if (include_user_section) s += "/user";
    return s;
}

// ---------------------------------------------------------------------------
// PackBuilder
// ---------------------------------------------------------------------------
void PackBuilder::add(PackShader shader) { shaders_.push_back(std::move(shader)); }

std::vector<std::uint8_t> PackBuilder::build(const PackOptions& options, Diagnostics& out_diags,
                                             PackStats* out_stats) const {
    const PackLayout& layout = options.layout;

    Diagnostics layout_diags = layout.validate();
    out_diags.insert(out_diags.end(), layout_diags.begin(), layout_diags.end());
    if (has_errors(layout_diags)) return {};

    if (shaders_.empty()) {
        out_diags.push_back(error("nothing to pack: no shaders marked include_in_pack"));
        return {};
    }
    if (shaders_.size() > 0xFFFF && layout.key16) {
        out_diags.push_back(error("key16 layout cannot address more than 65535 shaders"));
        return {};
    }

    // --- order entries -----------------------------------------------------
    std::vector<const PackShader*> order;
    order.reserve(shaders_.size());
    for (const auto& s : shaders_) order.push_back(&s);

    switch (layout.entry_sort) {
        case EntrySort::ByKey:
            std::stable_sort(order.begin(), order.end(),
                             [](const PackShader* a, const PackShader* b) { return a->key < b->key; });
            break;
        case EntrySort::ByStageThenKey:
            std::stable_sort(order.begin(), order.end(), [](const PackShader* a, const PackShader* b) {
                if (a->stage != b->stage) return a->stage < b->stage;
                return a->key < b->key;
            });
            break;
        case EntrySort::ManifestOrder:
            break;
    }

    // --- validate keys -----------------------------------------------------
    {
        std::set<std::uint32_t> seen;
        for (const auto* s : order) {
            if (layout.key16 && s->key > 0xFFFF) {
                out_diags.push_back(error("shader '" + s->id + "' key " + std::to_string(s->key) +
                                          " does not fit the 16-bit key layout"));
                return {};
            }
            if (!seen.insert(s->key).second) {
                out_diags.push_back(error("key collision on " + std::to_string(s->key) +
                                          " (shader '" + s->id + "')", "SSSTUDIO-KEY"));
                return {};
            }
        }
    }

    // --- collect blobs -----------------------------------------------------
    std::uint32_t format_mask = 0;
    std::uint32_t stage_mask = 0;
    std::uint32_t max_formats = 0;
    for (const auto* s : order) {
        stage_mask |= stage_bit(s->stage);
        max_formats = std::max<std::uint32_t>(max_formats, static_cast<std::uint32_t>(s->blobs.size()));
        for (const auto& [fmt, bytes] : s->blobs) {
            if (bytes.empty()) {
                out_diags.push_back(error("shader '" + s->id + "' has an empty " +
                                          std::string(to_string(fmt)) + " blob"));
                return {};
            }
            format_mask |= fmt;
        }
    }

    const bool compute_only = stage_mask == stage_bit(Stage::Compute);

    // Encode blobs first so their sizes are known before the table is laid out.
    struct EncodedBlob {
        ShaderFormat format;
        std::vector<std::uint8_t> data;
        std::uint32_t uncompressed_size;
    };
    std::vector<std::vector<EncodedBlob>> encoded(order.size());

    bool any_compressed = false;
    std::uint64_t raw_bytes = 0;
    std::uint32_t blob_count = 0;

    for (std::size_t i = 0; i < order.size(); ++i) {
        auto blobs = order[i]->blobs;
        std::sort(blobs.begin(), blobs.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& [fmt, bytes] : blobs) {
            raw_bytes += bytes.size();
            ++blob_count;
            EncodedBlob e;
            e.format = fmt;
            e.uncompressed_size = static_cast<std::uint32_t>(bytes.size());
            if (options.compression != Compression::None &&
                bytes.size() >= options.compression_threshold) {
                Compression used = Compression::None;
                e.data = compress(bytes, options.compression, &used);
                if (used == Compression::None) {
                    e.data = bytes;
                } else {
                    any_compressed = true;
                }
            } else {
                e.data = bytes;
            }
            encoded[i].push_back(std::move(e));
        }
    }

    if (options.compression != Compression::None && !compression_available(options.compression)) {
        Diagnostic d;
        d.severity = Severity::Warning;
        d.code = "SSSTUDIO-COMPRESS";
        d.message = std::string(to_string(options.compression)) +
                    " codec is not available in this build; blobs were stored uncompressed";
        out_diags.push_back(std::move(d));
    }

    // --- strings and reflection -------------------------------------------
    const bool keep_names = layout.include_names && !options.strip_names;
    const bool keep_reflection = layout.include_reflection && !options.strip_reflection;

    StringTable strings;
    std::vector<std::uint32_t> entry_name_off(order.size(), 0);
    std::vector<std::uint32_t> entry_entrypoint_off(order.size(), 0);
    std::vector<std::string> reflection_blobs(order.size());

    for (std::size_t i = 0; i < order.size(); ++i) {
        entry_entrypoint_off[i] = strings.add(order[i]->entry_point);
        if (keep_names) {
            entry_name_off[i] = strings.add(order[i]->name.empty() ? order[i]->id : order[i]->name);
        }
        if (keep_reflection) reflection_blobs[i] = reflection_to_json(order[i]->reflection);
    }

    // --- section sizes -----------------------------------------------------
    const std::uint32_t header_size = layout.header_size();
    std::uint32_t table_size = 0;
    for (std::size_t i = 0; i < order.size(); ++i) {
        table_size += layout.entry_size(static_cast<std::uint32_t>(encoded[i].size()));
    }

    // Layout plan: header, then (table, blobs) or (blobs, table), then strings,
    // then reflection, then the optional user section.
    std::vector<std::uint8_t> out;
    out.reserve(static_cast<std::size_t>(raw_bytes + table_size + 4096));

    auto aligned = [&](std::uint32_t v) {
        return ((v + layout.alignment - 1) / layout.alignment) * layout.alignment;
    };

    const std::uint32_t table_offset =
        layout.blobs_before_table ? 0 : aligned(header_size);
    std::uint32_t blob_offset = layout.blobs_before_table
                                    ? aligned(header_size)
                                    : aligned(table_offset + table_size);

    // Assign blob offsets.
    std::vector<std::vector<BlobRef>> refs(order.size());
    std::uint32_t cursor = blob_offset;
    for (std::size_t i = 0; i < order.size(); ++i) {
        for (const auto& e : encoded[i]) {
            BlobRef r;
            r.format = e.format;
            r.offset = cursor;
            r.size = static_cast<std::uint32_t>(e.data.size());
            r.uncompressed_size = e.uncompressed_size;
            refs[i].push_back(r);
            cursor = aligned(cursor + r.size);
        }
    }
    const std::uint32_t blob_end = cursor;
    const std::uint32_t real_table_offset =
        layout.blobs_before_table ? aligned(blob_end) : table_offset;
    const std::uint32_t string_offset =
        aligned(layout.blobs_before_table ? real_table_offset + table_size : blob_end);
    const std::uint32_t string_size = static_cast<std::uint32_t>(strings.bytes().size());
    std::uint32_t reflection_offset = aligned(string_offset + string_size);

    std::vector<std::pair<std::uint32_t, std::uint32_t>> reflection_slots(order.size(), {0, 0});
    std::uint32_t rcursor = reflection_offset;
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (reflection_blobs[i].empty()) continue;
        reflection_slots[i] = {rcursor, static_cast<std::uint32_t>(reflection_blobs[i].size())};
        rcursor = aligned(rcursor + reflection_slots[i].second);
    }
    const std::uint32_t reflection_end = rcursor;
    const bool has_user = layout.include_user_section && !options.user_section.empty();
    const std::uint32_t user_offset = has_user ? aligned(reflection_end) : 0;
    const std::uint32_t user_size =
        has_user ? static_cast<std::uint32_t>(options.user_section.size()) : 0;

    // --- flags -------------------------------------------------------------
    std::uint32_t flags = 0;
    if (any_compressed) flags |= PACK_FLAG_COMPRESSED;
    if (keep_reflection) flags |= PACK_FLAG_HAS_REFLECTION;
    if (keep_names) flags |= PACK_FLAG_HAS_NAMES;
    if (compute_only) flags |= PACK_FLAG_COMPUTE_ONLY;
    if (has_user) flags |= PACK_FLAG_HAS_USER_SECTION;
    if (layout.key16) flags |= PACK_FLAG_KEY16;
    if (layout.entry_sort == EntrySort::ManifestOrder) flags |= PACK_FLAG_UNSORTED;
    if (layout.blobs_before_table) flags |= PACK_FLAG_BLOBS_FIRST;
    if (layout.header_preset == HeaderPreset::Padded64) flags |= PACK_FLAG_PADDED64;

    const HeaderPreset hp = layout.header_preset;

    // --- header ------------------------------------------------------------
    out.insert(out.end(), layout.magic, layout.magic + 4);
    put_u16(out, kPackVersionMajor);
    put_u16(out, kPackVersionMinor);
    put_field(out, flags, hp);
    put_field(out, static_cast<std::uint32_t>(order.size()), hp);
    put_field(out, format_mask, hp);
    put_field(out, stage_mask, hp);
    put_field(out, static_cast<std::uint32_t>(options.compression), hp);
    put_field(out, layout.alignment, hp);
    put_field(out, real_table_offset, hp);
    put_field(out, blob_offset, hp);
    put_field(out, string_offset, hp);
    put_field(out, user_offset, hp);
    put_field(out, user_size, hp);
    out.resize(header_size, 0);

    auto write_table = [&]() {
        for (std::size_t i = 0; i < order.size(); ++i) {
            const PackShader& s = *order[i];
            const std::size_t entry_begin = out.size();
            if (layout.key16) {
                put_u16(out, static_cast<std::uint16_t>(s.key));
            } else {
                put_u32(out, s.key);
            }
            // Graphics shaders record every storage binding in the read-only
            // fields (SDL has no read-write bindings there). Compute shaders
            // split them, which is exactly what SDL_GPUComputePipelineCreateInfo
            // wants, so the loader never has to consult reflection.
            const bool is_compute = s.stage == Stage::Compute;
            const std::uint32_t ro_tex = is_compute ? s.reflection.num_readonly_storage_textures()
                                                    : s.reflection.num_storage_textures();
            const std::uint32_t ro_buf = is_compute ? s.reflection.num_readonly_storage_buffers()
                                                    : s.reflection.num_storage_buffers();
            const std::uint32_t rw_tex = is_compute ? s.reflection.num_readwrite_storage_textures() : 0;
            const std::uint32_t rw_buf = is_compute ? s.reflection.num_readwrite_storage_buffers() : 0;
            auto clamp8 = [](std::uint32_t v) {
                return static_cast<std::uint8_t>(std::min<std::uint32_t>(v, 255));
            };

            put_u8(out, static_cast<std::uint8_t>(s.stage));
            put_u8(out, static_cast<std::uint8_t>(s.language));
            put_u8(out, static_cast<std::uint8_t>(refs[i].size()));
            put_u8(out, clamp8(s.reflection.num_samplers()));
            put_u8(out, clamp8(ro_tex));
            put_u8(out, clamp8(ro_buf));
            put_u8(out, clamp8(s.reflection.num_uniform_buffers()));
            put_u8(out, clamp8(rw_tex));
            put_u8(out, clamp8(rw_buf));
            put_u8(out, 0);  // reserved

            put_field(out, entry_entrypoint_off[i], hp);
            put_field(out, entry_name_off[i], hp);
            put_field(out, reflection_slots[i].first, hp);
            put_field(out, reflection_slots[i].second, hp);
            for (int t = 0; t < 3; ++t) put_field(out, s.reflection.compute_threads[t], hp);

            std::uint32_t entry_format_mask = 0;
            for (const auto& r : refs[i]) entry_format_mask |= r.format;
            put_field(out, entry_format_mask, hp);

            for (const auto& r : refs[i]) {
                put_field(out, r.offset, hp);
                put_field(out, r.size, hp);
                put_field(out, r.uncompressed_size, hp);
            }
            const std::uint32_t expect = layout.entry_size(static_cast<std::uint32_t>(refs[i].size()));
            out.resize(entry_begin + expect, 0);
        }
    };

    auto write_blobs = [&]() {
        for (std::size_t i = 0; i < order.size(); ++i) {
            for (std::size_t j = 0; j < encoded[i].size(); ++j) {
                out.resize(refs[i][j].offset, 0);
                out.insert(out.end(), encoded[i][j].data.begin(), encoded[i][j].data.end());
            }
        }
    };

    if (layout.blobs_before_table) {
        write_blobs();
        out.resize(real_table_offset, 0);
        write_table();
    } else {
        out.resize(real_table_offset, 0);
        write_table();
        write_blobs();
    }

    out.resize(string_offset, 0);
    out.insert(out.end(), strings.bytes().begin(), strings.bytes().end());

    for (std::size_t i = 0; i < order.size(); ++i) {
        if (reflection_blobs[i].empty()) continue;
        out.resize(reflection_slots[i].first, 0);
        out.insert(out.end(), reflection_blobs[i].begin(), reflection_blobs[i].end());
    }

    if (has_user) {
        out.resize(user_offset, 0);
        out.insert(out.end(), options.user_section.begin(), options.user_section.end());
    }
    pad_to(out, layout.alignment);

    if (out_stats) {
        out_stats->shader_count = static_cast<std::uint32_t>(order.size());
        out_stats->blob_count = blob_count;
        out_stats->raw_bytes = raw_bytes;
        std::uint64_t stored = 0;
        for (const auto& v : encoded)
            for (const auto& e : v) stored += e.data.size();
        out_stats->stored_bytes = stored;
        out_stats->total_bytes = out.size();
        out_stats->format_mask = format_mask;
        out_stats->stage_mask = stage_mask;
        out_stats->compute_only = compute_only;
    }
    return out;
}

}  // namespace ssstudio
