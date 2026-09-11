// Shared compile cache.
//
// Keyed by (source, language, stage, entry point, defines, formats, optimization
// level) via CompileRequest::cache_key(), so two projects that contain the same
// shader compile it once between them. Entries are plain files, which means the
// cache is inspectable, deletable, and safe to lose at any moment.
#ifndef SSSTUDIO_CACHE_H
#define SSSTUDIO_CACHE_H

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "ssstudio/compiler.h"

namespace ssstudio {

struct CacheStats {
    std::uint64_t entries = 0;
    std::uint64_t bytes = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t evictions = 0;
};

class CompileCache {
public:
    CompileCache() = default;
    explicit CompileCache(std::filesystem::path dir, std::uint64_t budget_bytes = 512ull << 20);

    /// Points an existing cache at a directory, creating it if needed, and
    /// resets the hit and miss counters. The mutex the cache owns makes the
    /// class non-assignable, so a cache whose location is only known after
    /// settings are loaded is re-opened in place rather than replaced.
    void open(std::filesystem::path dir, std::uint64_t budget_bytes = 512ull << 20);

    bool enabled() const { return !dir_.empty(); }
    const std::filesystem::path& directory() const { return dir_; }

    // Returns a previously stored result, or nullopt. A hit carries the blobs,
    // the reflection and the diagnostics the original compile produced, because
    // a warning that only appears on a cold cache is a trap.
    std::optional<CompileResult> lookup(std::uint64_t key);

    // Stores a successful result. Failures are never cached: a compile error
    // usually means the user is mid-edit, and stale errors are worse than a
    // recompile.
    bool store(std::uint64_t key, const CompileResult& result);

    // Removes least-recently-used entries until the cache fits its budget.
    void trim();
    void clear();

    CacheStats stats() const;
    void set_budget(std::uint64_t bytes);

    // Platform default: alongside the app's settings, under `cache/`.
    static std::filesystem::path default_directory();

private:
    std::filesystem::path path_for(std::uint64_t key) const;

    std::filesystem::path dir_;
    std::uint64_t budget_ = 512ull << 20;
    mutable std::mutex mutex_;
    mutable CacheStats stats_;
};

}  // namespace ssstudio

#endif  // SSSTUDIO_CACHE_H
