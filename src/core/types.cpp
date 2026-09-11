#include "ssstudio/types.h"

#include <algorithm>
#include <array>
#include <cstdio>

#include "ssstudio/hash.h"
#include "ssstudio/pack_format.h"

namespace ssstudio {
namespace {

struct NameRow {
    const char* name;
    int value;
};

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
std::string_view to_string(Stage s) {
    switch (s) {
        case Stage::Vertex: return "vertex";
        case Stage::Fragment: return "fragment";
        case Stage::Compute: return "compute";
    }
    return "unknown";
}

std::optional<Stage> stage_from_string(std::string_view s) {
    const std::string v = lower(s);
    if (v == "vertex" || v == "vert" || v == "vs") return Stage::Vertex;
    if (v == "fragment" || v == "frag" || v == "pixel" || v == "ps") return Stage::Fragment;
    if (v == "compute" || v == "comp" || v == "cs") return Stage::Compute;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
std::string_view to_string(Language l) {
    switch (l) {
        case Language::HLSL: return "hlsl";
        case Language::GLSL: return "glsl";
    }
    return "unknown";
}

std::optional<Language> language_from_string(std::string_view s) {
    const std::string v = lower(s);
    if (v == "hlsl") return Language::HLSL;
    if (v == "glsl") return Language::GLSL;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
std::string_view to_string(ShaderFormat f) {
    switch (f) {
        case FORMAT_NONE: return "none";
        case FORMAT_PRIVATE: return "private";
        case FORMAT_SPIRV: return "spirv";
        case FORMAT_DXBC: return "dxbc";
        case FORMAT_DXIL: return "dxil";
        case FORMAT_MSL: return "msl";
        case FORMAT_METALLIB: return "metallib";
    }
    return "unknown";
}

std::optional<ShaderFormat> format_from_string(std::string_view s) {
    const std::string v = lower(s);
    if (v == "spirv" || v == "spv") return FORMAT_SPIRV;
    if (v == "dxbc") return FORMAT_DXBC;
    if (v == "dxil") return FORMAT_DXIL;
    if (v == "msl") return FORMAT_MSL;
    if (v == "metallib") return FORMAT_METALLIB;
    if (v == "private") return FORMAT_PRIVATE;
    if (v == "none") return FORMAT_NONE;
    return std::nullopt;
}

std::vector<ShaderFormat> formats_in_mask(std::uint32_t mask) {
    static constexpr ShaderFormat kAll[] = {FORMAT_PRIVATE, FORMAT_SPIRV, FORMAT_DXBC,
                                            FORMAT_DXIL,    FORMAT_MSL,   FORMAT_METALLIB};
    std::vector<ShaderFormat> out;
    for (ShaderFormat f : kAll) {
        if (mask & f) out.push_back(f);
    }
    return out;
}

std::string format_mask_to_string(std::uint32_t mask) {
    std::string out;
    for (ShaderFormat f : formats_in_mask(mask)) {
        if (!out.empty()) out += ", ";
        out += to_string(f);
    }
    return out.empty() ? "none" : out;
}

// ---------------------------------------------------------------------------
std::string_view to_string(KeyStrategy k) {
    switch (k) {
        case KeyStrategy::Enum: return "enum";
        case KeyStrategy::Hash: return "hash";
        case KeyStrategy::Explicit: return "explicit";
    }
    return "explicit";
}

std::optional<KeyStrategy> key_strategy_from_string(std::string_view s) {
    const std::string v = lower(s);
    if (v == "enum" || v == "sequential") return KeyStrategy::Enum;
    if (v == "hash" || v == "fnv1a") return KeyStrategy::Hash;
    if (v == "explicit" || v == "pinned") return KeyStrategy::Explicit;
    return std::nullopt;
}

std::string_view to_string(Compression c) {
    switch (c) {
        case Compression::None: return "none";
        case Compression::LZ4: return "lz4";
        case Compression::Zstd: return "zstd";
    }
    return "none";
}

std::optional<Compression> compression_from_string(std::string_view s) {
    const std::string v = lower(s);
    if (v == "none" || v == "raw" || v == "off") return Compression::None;
    if (v == "lz4") return Compression::LZ4;
    if (v == "zstd" || v == "zst") return Compression::Zstd;
    return std::nullopt;
}

std::string_view to_string(Severity s) {
    switch (s) {
        case Severity::Info: return "info";
        case Severity::Warning: return "warning";
        case Severity::Error: return "error";
    }
    return "error";
}

std::string Diagnostic::format() const {
    std::string out;
    if (!file.empty()) {
        out += file;
        if (line > 0) {
            out += ":" + std::to_string(line);
            if (column > 0) out += ":" + std::to_string(column);
        }
        out += ": ";
    }
    out += std::string(to_string(severity));
    out += ": ";
    out += message;
    if (!code.empty()) out += " [" + code + "]";
    return out;
}

bool has_errors(const Diagnostics& d) {
    return std::any_of(d.begin(), d.end(),
                       [](const Diagnostic& x) { return x.severity == Severity::Error; });
}

// ---------------------------------------------------------------------------
// pack_format enum strings live here to keep the small translation units few.
// ---------------------------------------------------------------------------
std::string_view to_string(EntrySort s) {
    switch (s) {
        case EntrySort::ByKey: return "by_key";
        case EntrySort::ByStageThenKey: return "by_stage_then_key";
        case EntrySort::ManifestOrder: return "manifest_order";
    }
    return "by_key";
}

std::optional<EntrySort> entry_sort_from_string(std::string_view s) {
    const std::string v = lower(s);
    if (v == "by_key" || v == "key") return EntrySort::ByKey;
    if (v == "by_stage_then_key" || v == "stage") return EntrySort::ByStageThenKey;
    if (v == "manifest_order" || v == "manifest") return EntrySort::ManifestOrder;
    return std::nullopt;
}

std::string_view to_string(HeaderPreset p) {
    switch (p) {
        case HeaderPreset::Compact: return "compact";
        case HeaderPreset::Padded64: return "padded64";
    }
    return "compact";
}

std::optional<HeaderPreset> header_preset_from_string(std::string_view s) {
    const std::string v = lower(s);
    if (v == "compact") return HeaderPreset::Compact;
    if (v == "padded64" || v == "padded") return HeaderPreset::Padded64;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
std::uint32_t fnv1a32(std::string_view s) {
    std::uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

std::uint64_t fnv1a64(std::string_view s) {
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

std::uint64_t fnv1a64(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t h = 1469598103934665603ull;
    for (std::uint8_t c : bytes) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

std::string to_hex(std::uint64_t v, int digits) {
    static const char* kHex = "0123456789abcdef";
    std::string out(static_cast<std::size_t>(digits), '0');
    for (int i = digits - 1; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kHex[v & 0xF];
        v >>= 4;
    }
    return out;
}

}  // namespace ssstudio
