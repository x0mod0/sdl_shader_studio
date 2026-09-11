// Read side of the pack container. Written independently of packer.cpp on
// purpose: a format bug then shows up as a round-trip failure instead of being
// cancelled out by shared helpers.
#include <algorithm>
#include <cstring>

#include "ssstudio/compress.h"
#include "ssstudio/packer.h"

namespace ssstudio {
namespace {

Diagnostic error(std::string msg) {
    Diagnostic d;
    d.severity = Severity::Error;
    d.code = "SSSTUDIO-PACK";
    d.message = std::move(msg);
    return d;
}

class Cursor {
public:
    Cursor(const std::uint8_t* data, std::size_t size, bool padded)
        : data_(data), size_(size), padded_(padded) {}

    bool seek(std::size_t pos) {
        if (pos > size_) return false;
        pos_ = pos;
        return true;
    }
    std::size_t pos() const { return pos_; }
    bool ok() const { return ok_; }

    std::uint8_t u8() {
        if (pos_ + 1 > size_) { ok_ = false; return 0; }
        return data_[pos_++];
    }
    std::uint16_t u16() {
        if (pos_ + 2 > size_) { ok_ = false; return 0; }
        std::uint16_t v = static_cast<std::uint16_t>(data_[pos_] | (data_[pos_ + 1] << 8));
        pos_ += 2;
        return v;
    }
    std::uint32_t u32() {
        if (pos_ + 4 > size_) { ok_ = false; return 0; }
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(data_[pos_ + i]) << (i * 8);
        pos_ += 4;
        return v;
    }
    // Field respecting the header preset (Padded64 stores 8 bytes per field).
    std::uint32_t field() {
        const std::uint32_t v = u32();
        if (padded_) u32();
        return v;
    }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    bool padded_ = false;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

std::string read_cstring(const std::vector<std::uint8_t>& bytes, std::size_t base,
                         std::uint32_t offset) {
    if (offset == 0) return {};
    const std::size_t start = base + offset - 1;
    if (start >= bytes.size()) return {};
    const auto* p = reinterpret_cast<const char*>(bytes.data() + start);
    const std::size_t max = bytes.size() - start;
    const std::size_t len = ::strnlen(p, max);
    return std::string(p, len);
}

}  // namespace

bool PackReader::open(const std::vector<std::uint8_t>& bytes, Diagnostics& out_diags) {
    entries_.clear();
    bytes_ = bytes;

    if (bytes_.size() < 32) {
        out_diags.push_back(error("pack is too small to contain a header"));
        return false;
    }

    // Peek the flags to learn the header preset before reading the rest.
    const bool padded = (bytes_[8] | (bytes_[9] << 8) | (bytes_[10] << 16) |
                         (bytes_[11] << 24)) & PACK_FLAG_PADDED64;

    Cursor c(bytes_.data(), bytes_.size(), padded);
    char magic[4];
    for (int i = 0; i < 4; ++i) magic[i] = static_cast<char>(c.u8());
    const std::uint16_t vmaj = c.u16();
    c.u16();  // minor version: forward-compatible within a major version
    if (vmaj != kPackVersionMajor) {
        out_diags.push_back(error("pack major version " + std::to_string(vmaj) +
                                  " is not supported by this build (expected " +
                                  std::to_string(kPackVersionMajor) + ")"));
        return false;
    }

    flags_ = c.field();
    const std::uint32_t shader_count = c.field();
    format_mask_ = c.field();
    stage_mask_ = c.field();
    const std::uint32_t compression_id = c.field();
    const std::uint32_t alignment = c.field();
    const std::uint32_t table_offset = c.field();
    const std::uint32_t blob_offset = c.field();
    const std::uint32_t string_offset = c.field();
    const std::uint32_t user_offset = c.field();
    const std::uint32_t user_size = c.field();
    (void)blob_offset;

    if (!c.ok()) {
        out_diags.push_back(error("truncated pack header"));
        return false;
    }

    layout_ = PackLayout{};
    std::memcpy(layout_.magic, magic, 4);
    layout_.header_preset = padded ? HeaderPreset::Padded64 : HeaderPreset::Compact;
    layout_.entry_sort = (flags_ & PACK_FLAG_UNSORTED) ? EntrySort::ManifestOrder : EntrySort::ByKey;
    layout_.blobs_before_table = (flags_ & PACK_FLAG_BLOBS_FIRST) != 0;
    layout_.key16 = (flags_ & PACK_FLAG_KEY16) != 0;
    layout_.alignment = alignment ? alignment : 16;
    layout_.include_names = (flags_ & PACK_FLAG_HAS_NAMES) != 0;
    layout_.include_reflection = (flags_ & PACK_FLAG_HAS_REFLECTION) != 0;
    layout_.include_user_section = (flags_ & PACK_FLAG_HAS_USER_SECTION) != 0;

    const auto compression = static_cast<Compression>(compression_id);
    if (compression_id > static_cast<std::uint32_t>(Compression::Zstd)) {
        out_diags.push_back(error("unknown compression id in pack header"));
        return false;
    }
    compression_ = compression;

    if (!c.seek(table_offset)) {
        out_diags.push_back(error("entry table offset lies outside the pack"));
        return false;
    }

    entries_.reserve(shader_count);
    for (std::uint32_t i = 0; i < shader_count; ++i) {
        const std::size_t entry_begin = c.pos();
        Entry e;
        e.key = layout_.key16 ? c.u16() : c.u32();
        e.stage = static_cast<Stage>(c.u8());
        e.language = static_cast<Language>(c.u8());
        const std::uint8_t blob_count = c.u8();
        e.num_samplers = c.u8();
        e.num_storage_textures = c.u8();
        e.num_storage_buffers = c.u8();
        e.num_uniform_buffers = c.u8();
        e.num_readwrite_storage_textures = c.u8();
        e.num_readwrite_storage_buffers = c.u8();
        c.u8();  // reserved

        const std::uint32_t entrypoint_off = c.field();
        const std::uint32_t name_off = c.field();
        const std::uint32_t refl_off = c.field();
        const std::uint32_t refl_size = c.field();
        for (int t = 0; t < 3; ++t) e.compute_threads[t] = c.field();
        e.format_mask = c.field();

        for (std::uint8_t b = 0; b < blob_count; ++b) {
            BlobRef r;
            r.offset = c.field();
            r.size = c.field();
            r.uncompressed_size = c.field();
            e.blobs.push_back(r);
        }
        if (!c.ok()) {
            out_diags.push_back(error("truncated entry table at entry " + std::to_string(i)));
            return false;
        }

        // Recover the per-blob format from the entry mask: blobs are written in
        // ascending format order, which the writer guarantees.
        {
            const auto fmts = formats_in_mask(e.format_mask);
            if (fmts.size() != e.blobs.size()) {
                out_diags.push_back(error("entry " + std::to_string(i) +
                                          " format mask does not match its blob count"));
                return false;
            }
            for (std::size_t k = 0; k < fmts.size(); ++k) e.blobs[k].format = fmts[k];
        }

        e.entry_point = read_cstring(bytes_, string_offset, entrypoint_off);
        e.name = read_cstring(bytes_, string_offset, name_off);
        if (refl_off && refl_size && refl_off + refl_size <= bytes_.size()) {
            e.reflection_json.assign(reinterpret_cast<const char*>(bytes_.data() + refl_off),
                                     refl_size);
        }

        for (const auto& r : e.blobs) {
            if (static_cast<std::size_t>(r.offset) + r.size > bytes_.size()) {
                out_diags.push_back(error("blob for key " + std::to_string(e.key) +
                                          " points outside the pack"));
                return false;
            }
        }

        entries_.push_back(std::move(e));
        const std::uint32_t stride = layout_.entry_size(blob_count);
        if (!c.seek(entry_begin + stride)) {
            out_diags.push_back(error("entry stride runs past the end of the pack"));
            return false;
        }
    }

    if (user_offset && user_size && user_offset + user_size <= bytes_.size()) {
        user_section_.assign(reinterpret_cast<const char*>(bytes_.data() + user_offset), user_size);
    }

    if (!(flags_ & PACK_FLAG_UNSORTED)) {
        const bool sorted = std::is_sorted(entries_.begin(), entries_.end(),
                                           [](const Entry& a, const Entry& b) { return a.key < b.key; });
        if (!sorted) {
            Diagnostic d;
            d.severity = Severity::Warning;
            d.code = "SSSTUDIO-PACK";
            d.message = "pack claims sorted entries but the table is not ordered by key; "
                        "loaders using binary search may miss shaders";
            out_diags.push_back(std::move(d));
        }
    }
    return true;
}

const PackReader::Entry* PackReader::find(std::uint32_t key) const {
    if (flags_ & PACK_FLAG_UNSORTED) {
        for (const auto& e : entries_)
            if (e.key == key) return &e;
        return nullptr;
    }
    auto it = std::lower_bound(entries_.begin(), entries_.end(), key,
                               [](const Entry& e, std::uint32_t k) { return e.key < k; });
    if (it == entries_.end() || it->key != key) return nullptr;
    return &*it;
}

std::vector<std::uint8_t> PackReader::blob(std::uint32_t key, ShaderFormat format,
                                           Diagnostics& out_diags) const {
    const Entry* e = find(key);
    if (!e) {
        out_diags.push_back(error("no shader with key " + std::to_string(key) + " in this pack"));
        return {};
    }
    for (const auto& r : e->blobs) {
        if (r.format != format) continue;
        const std::uint8_t* p = bytes_.data() + r.offset;
        if (r.size == r.uncompressed_size || !(flags_ & PACK_FLAG_COMPRESSED)) {
            return std::vector<std::uint8_t>(p, p + r.size);
        }
        auto out = decompress(p, r.size, r.uncompressed_size, compression_);
        if (out.empty()) {
            out_diags.push_back(error("failed to decompress blob for key " + std::to_string(key) +
                                      " (" + std::string(to_string(format)) + ")"));
        }
        return out;
    }
    out_diags.push_back(error("shader " + std::to_string(key) + " has no " +
                              std::string(to_string(format)) + " blob"));
    return {};
}

}  // namespace ssstudio
