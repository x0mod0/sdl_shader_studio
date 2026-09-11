#include "ssstudio/cache.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "ssstudio/hash.h"
#include "ssstudio/spirv_reflect.h"

namespace ssstudio {
namespace {

// Entry file: "SDSC" + version, then reflection JSON, diagnostics and blobs.
// Deliberately its own tiny format rather than the pack format, because the two
// have nothing to do with each other and coupling them would be a trap later.
constexpr char kMagic[4] = {'S', 'D', 'S', 'C'};
constexpr std::uint32_t kVersion = 2;

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFF));
}

void put_bytes(std::vector<std::uint8_t>& out, const void* data, std::size_t size) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    out.insert(out.end(), p, p + size);
}

void put_string(std::vector<std::uint8_t>& out, const std::string& s) {
    put_u32(out, static_cast<std::uint32_t>(s.size()));
    put_bytes(out, s.data(), s.size());
}

struct Reader {
    const std::uint8_t* data;
    std::size_t size;
    std::size_t pos = 0;
    bool ok = true;

    std::uint32_t u32() {
        if (pos + 4 > size) {
            ok = false;
            return 0;
        }
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(data[pos + i]) << (i * 8);
        pos += 4;
        return v;
    }
    std::string str() {
        const std::uint32_t n = u32();
        if (!ok || pos + n > size) {
            ok = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(data + pos), n);
        pos += n;
        return s;
    }
    std::vector<std::uint8_t> bytes() {
        const std::uint32_t n = u32();
        if (!ok || pos + n > size) {
            ok = false;
            return {};
        }
        std::vector<std::uint8_t> v(data + pos, data + pos + n);
        pos += n;
        return v;
    }
};

std::vector<std::uint8_t> encode(const CompileResult& result) {
    std::vector<std::uint8_t> out;
    put_bytes(out, kMagic, 4);
    put_u32(out, kVersion);

    put_string(out, reflection_to_json(result.reflection));
    put_u32(out, static_cast<std::uint32_t>(result.reflection.stage));
    put_string(out, result.reflection.entry_point);

    put_u32(out, static_cast<std::uint32_t>(result.diagnostics.size()));
    for (const auto& d : result.diagnostics) {
        put_u32(out, static_cast<std::uint32_t>(d.severity));
        put_u32(out, static_cast<std::uint32_t>(d.line));
        put_u32(out, static_cast<std::uint32_t>(d.column));
        put_string(out, d.file);
        put_string(out, d.message);
        put_string(out, d.code);
    }

    put_u32(out, static_cast<std::uint32_t>(result.blobs.size()));
    for (const auto& [format, blob] : result.blobs) {
        put_u32(out, static_cast<std::uint32_t>(format));
        put_u32(out, static_cast<std::uint32_t>(blob.size()));
        put_bytes(out, blob.data(), blob.size());
    }
    return out;
}

// The cached reflection is stored as JSON for inspectability, but the fields the
// packer needs are re-derived from the blobs on load, so a cache hit and a cold
// compile produce identical packs. Only the cheap fields are restored here.
bool decode(const std::vector<std::uint8_t>& bytes, CompileResult& out,
            std::string& out_reflection_json) {
    if (bytes.size() < 8 || std::memcmp(bytes.data(), kMagic, 4) != 0) return false;

    Reader r{bytes.data(), bytes.size(), 4};
    if (r.u32() != kVersion) return false;

    out_reflection_json = r.str();
    out.reflection.stage = static_cast<Stage>(r.u32());
    out.reflection.entry_point = r.str();

    const std::uint32_t diagnostic_count = r.u32();
    for (std::uint32_t i = 0; i < diagnostic_count && r.ok; ++i) {
        Diagnostic d;
        d.severity = static_cast<Severity>(r.u32());
        d.line = static_cast<int>(r.u32());
        d.column = static_cast<int>(r.u32());
        d.file = r.str();
        d.message = r.str();
        d.code = r.str();
        out.diagnostics.push_back(std::move(d));
    }

    const std::uint32_t blob_count = r.u32();
    for (std::uint32_t i = 0; i < blob_count && r.ok; ++i) {
        const auto format = static_cast<ShaderFormat>(r.u32());
        out.blobs[format] = r.bytes();
    }
    return r.ok && !out.blobs.empty();
}

}  // namespace

CompileCache::CompileCache(std::filesystem::path dir, std::uint64_t budget_bytes) {
    open(std::move(dir), budget_bytes);
}

void CompileCache::open(std::filesystem::path dir, std::uint64_t budget_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    dir_ = std::move(dir);
    budget_ = budget_bytes;
    stats_ = CacheStats{};
    if (dir_.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    if (ec) dir_.clear();  // an unusable cache is simply no cache
}

std::filesystem::path CompileCache::default_directory() {
#if defined(_WIN32)
    if (const char* appdata = std::getenv("LOCALAPPDATA")) {
        return std::filesystem::path(appdata) / "sdl-shader-studio" / "cache";
    }
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home) / "Library" / "Caches" / "sdl-shader-studio";
    }
#else
    if (const char* xdg = std::getenv("XDG_CACHE_HOME")) {
        return std::filesystem::path(xdg) / "sdl-shader-studio";
    }
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home) / ".cache" / "sdl-shader-studio";
    }
#endif
    return std::filesystem::temp_directory_path() / "sdl-shader-studio-cache";
}

std::filesystem::path CompileCache::path_for(std::uint64_t key) const {
    // Two hex characters of prefix keeps directories small on filesystems that
    // dislike thousands of entries in one folder.
    const std::string hex = to_hex(key, 16);
    return dir_ / hex.substr(0, 2) / (hex + ".sdsc");
}

std::optional<CompileResult> CompileCache::lookup(std::uint64_t key) {
    if (!enabled()) return std::nullopt;

    const std::filesystem::path path = path_for(key);
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.misses;
        return std::nullopt;
    }

    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                    std::istreambuf_iterator<char>());
    CompileResult result;
    std::string reflection_json;
    if (!decode(bytes, result, reflection_json)) {
        std::error_code ec;
        std::filesystem::remove(path, ec);  // corrupt entry: drop it and recompile
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.misses;
        return std::nullopt;
    }

    // Reflection is re-derived from the cached SPIR-V rather than trusted from
    // JSON, so a cache hit and a fresh compile cannot diverge.
    auto spirv = result.blobs.find(FORMAT_SPIRV);
    if (spirv != result.blobs.end()) {
        Diagnostics ignored;
        Reflection fresh = reflect_spirv(spirv->second, result.reflection.stage,
                                         result.reflection.entry_point, ignored);
        fresh.entry_point = result.reflection.entry_point;
        result.reflection = std::move(fresh);
    }

    result.ok = true;
    result.from_cache = true;

    // Touch the file so trim() evicts genuinely cold entries.
    std::error_code ec;
    std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(), ec);

    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.hits;
    return result;
}

bool CompileCache::store(std::uint64_t key, const CompileResult& result) {
    if (!enabled() || !result.ok || result.blobs.empty()) return false;

    const std::filesystem::path path = path_for(key);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    const std::vector<std::uint8_t> bytes = encode(result);

    // Write and rename so a crash mid-write cannot leave a half entry that would
    // be read back as a corrupt shader.
    const std::filesystem::path temp = path.string() + ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

void CompileCache::trim() {
    if (!enabled()) return;

    struct Entry {
        std::filesystem::path path;
        std::uint64_t size = 0;
        std::filesystem::file_time_type time{};
    };
    std::vector<Entry> entries;
    std::uint64_t total = 0;

    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(dir_, ec);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        if (it->path().extension() != ".sdsc") continue;
        Entry e;
        e.path = it->path();
        e.size = static_cast<std::uint64_t>(it->file_size(ec));
        e.time = it->last_write_time(ec);
        total += e.size;
        entries.push_back(std::move(e));
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.entries = entries.size();
        stats_.bytes = total;
    }
    if (total <= budget_) return;

    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.time < b.time; });

    for (const auto& e : entries) {
        if (total <= budget_) break;
        std::filesystem::remove(e.path, ec);
        if (ec) continue;
        total -= e.size;
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.evictions;
        if (stats_.entries) --stats_.entries;
        stats_.bytes = total;
    }
}

void CompileCache::clear() {
    if (!enabled()) return;
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
    std::filesystem::create_directories(dir_, ec);
    std::lock_guard<std::mutex> lock(mutex_);
    stats_ = CacheStats{};
}

CacheStats CompileCache::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void CompileCache::set_budget(std::uint64_t bytes) {
    budget_ = bytes;
    trim();
}

}  // namespace ssstudio
