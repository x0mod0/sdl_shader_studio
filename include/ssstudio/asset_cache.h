// On-disk cache for files fetched from a URL.
//
// Sits beside the compile cache and keeps its guarantees deliberately: entries
// are plain files, so the cache is inspectable, deletable, and safe to lose at
// any moment. Nothing here is required for a project to open - a cold cache with
// no network resolves to nothing and says so, rather than blocking.
//
// Fetching is done by running curl rather than by linking an HTTP client, which
// is the same choice this project already makes for the shader toolchain: a tool
// that is present on every platform this runs on, invoked out of process, beats
// a library that has to be built for each of them.
#ifndef SSSTUDIO_ASSET_CACHE_H
#define SSSTUDIO_ASSET_CACHE_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "ssstudio/types.h"

namespace ssstudio {

/// One file that has been fetched and kept.
struct AssetEntry {
    /// Where the bytes are. Valid only until the entry is evicted.
    std::filesystem::path path;
    /// The address it came from, stored so a key collision can be spotted.
    std::string url;
    /// What the server said it was, when it said anything.
    std::string content_type;
    std::uint64_t bytes = 0;
    /// FNV-1a of the file's own bytes, so a project can notice that a remote
    /// asset is not what it was.
    std::uint64_t content_hash = 0;
    /// Seconds since the epoch, for showing how stale an entry is.
    std::int64_t fetched_unix = 0;
};

/// How a fetch ended. Everything except Ready leaves the cache as it was.
enum class FetchStatus : std::uint8_t {
    /// The bytes are on disk and the entry is usable.
    Ready,
    /// Queued or in flight; ask again later.
    InFlight,
    /// curl could not be found. The one failure that is about this machine
    /// rather than about the address.
    ToolMissing,
    /// The transfer did not complete - no network, DNS, a refused connection.
    Offline,
    /// The server answered, and the answer was not the file.
    HttpError,
    /// The response was larger than the cap.
    TooLarge,
    /// Refused before anything was sent: not an https address, or empty.
    Refused,
};

std::string_view to_string(FetchStatus s);

struct AssetCacheStats {
    std::uint64_t entries = 0;
    std::uint64_t bytes = 0;
};

/// Fetches URLs once and keeps them.
///
/// Nothing is ever fetched implicitly. `lookup` only ever reads what is already
/// on disk; `fetch` is the only thing that touches the network, and it exists to
/// be called from a button rather than from a resolution path. That separation
/// is the whole reason opening a project someone sent you cannot make this
/// machine talk to an address they chose.
class AssetCache {
public:
    AssetCache() = default;
    ~AssetCache();

    AssetCache(const AssetCache&) = delete;
    AssetCache& operator=(const AssetCache&) = delete;

    /// Points the cache at a directory, creating it if needed. Safe to call
    /// again to move it, which is what a settings change does.
    void open(std::filesystem::path dir, std::uint64_t budget_bytes);
    bool enabled() const { return !dir_.empty(); }
    const std::filesystem::path& directory() const { return dir_; }

    /// What is already on disk for this address, if anything. Never fetches.
    std::optional<AssetEntry> lookup(std::string_view url) const;

    /// Queues a download. Returns at once; `on_done` runs from poll(), on the
    /// thread that calls it. Asking for something already in flight does not
    /// queue it twice.
    using Callback = std::function<void(FetchStatus, AssetEntry)>;
    void fetch(std::string url, Callback on_done);

    /// True while that address is being fetched.
    bool in_flight(std::string_view url) const;

    /// Runs the callbacks of anything that has finished. Call once per frame.
    void poll();

    /// Where curl lives, when it is not simply on the path.
    void set_tool_path(std::filesystem::path curl);

    /// Copies a cached file into the project so the project stops depending on
    /// this machine's cache. What turns a URL binding into a file one.
    bool localise(const AssetEntry& entry, const std::filesystem::path& destination,
                  Diagnostics& out_diags) const;

    /// Releases least-recently-fetched entries until the cache fits its budget.
    void trim();
    void clear();
    AssetCacheStats stats() const;

    /// Beside the compile cache, under `assets/`.
    static std::filesystem::path default_directory();

    /// The largest response that will be accepted, in bytes. A texture larger
    /// than this is a mistake rather than a texture.
    static constexpr std::uint64_t kMaxBytes = 64ull * 1024 * 1024;

private:
    struct Job {
        std::string url;
        Callback on_done;
    };

    /// Runs curl for one address and writes the entry out. Worker thread only.
    FetchStatus download(const std::string& url, AssetEntry& out) const;
    std::filesystem::path path_for(std::string_view url, std::string_view extension) const;
    std::filesystem::path meta_path_for(std::string_view url) const;
    void worker_loop();
    void stop_worker();

    std::filesystem::path dir_;
    std::filesystem::path curl_;
    std::uint64_t budget_ = 512ull << 20;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    std::vector<std::string> active_;
    std::deque<std::pair<Job, std::pair<FetchStatus, AssetEntry>>> finished_;
    std::thread worker_;
    bool running_ = false;
};

}  // namespace ssstudio

#endif  // SSSTUDIO_ASSET_CACHE_H
