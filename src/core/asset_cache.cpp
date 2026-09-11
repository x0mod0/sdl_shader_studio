#include "ssstudio/asset_cache.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "ssstudio/cache.h"
#include "ssstudio/hash.h"
#include "ssstudio/process.h"

namespace ssstudio {
namespace {

Diagnostic note(Severity severity, std::string message) {
    Diagnostic d;
    d.severity = severity;
    d.code = "SSSTUDIO-ASSET";
    d.message = std::move(message);
    return d;
}

/// An extension for a cached file, guessed from what the server said and then
/// from the address. Only cosmetic: the decoder sniffs the content, so a wrong
/// guess costs nothing but a confusing name in the cache directory.
std::string extension_for(std::string_view content_type, std::string_view url) {
    struct Known {
        std::string_view type;
        std::string_view extension;
    };
    static constexpr Known kTypes[] = {
        {"image/png", ".png"},   {"image/jpeg", ".jpg"}, {"image/webp", ".webp"},
        {"image/avif", ".avif"}, {"image/gif", ".gif"},  {"image/bmp", ".bmp"},
        {"image/tga", ".tga"},   {"image/svg+xml", ".svg"},
    };
    for (const Known& known : kTypes) {
        if (content_type.find(known.type) != std::string_view::npos) {
            return std::string(known.extension);
        }
    }

    // Back to the address, minus any query string.
    std::string_view path = url.substr(0, url.find('?'));
    const std::size_t slash = path.find_last_of('/');
    if (slash != std::string_view::npos) path = path.substr(slash + 1);
    const std::size_t dot = path.find_last_of('.');
    if (dot != std::string_view::npos && dot + 1 < path.size() && path.size() - dot <= 6) {
        std::string extension(path.substr(dot));
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return extension;
    }
    return ".bin";
}

bool read_whole_file(const std::filesystem::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return true;
}

/// Trims whitespace from both ends, which is all the meta file's values need.
std::string_view trimmed(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

/// The sidecar is `key: value` a line at a time. Hand-rolled rather than TOML
/// because it is written and read only here, and because a cache file that
/// needs a parser to be readable defeats the point of keeping it as a file.
std::map<std::string, std::string> read_meta(const std::filesystem::path& path) {
    std::map<std::string, std::string> fields;
    std::ifstream in(path);
    if (!in) return fields;
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        fields[std::string(trimmed(std::string_view(line).substr(0, colon)))] =
            std::string(trimmed(std::string_view(line).substr(colon + 1)));
    }
    return fields;
}

std::int64_t now_unix() {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

/// Finds curl.
///
/// run_process spawns an exact path rather than searching, which is right for
/// the shader toolchain - it is looked up once and configured - but wrong for a
/// tool that is simply expected to be on the machine. So the search happens
/// here: the configured path first, then the places it actually lives, then
/// whatever PATH says. Returns empty when there is no curl to run, which is
/// reported as ToolMissing rather than as a failure to reach the address.
std::filesystem::path resolve_curl(const std::filesystem::path& configured) {
    std::error_code ec;
    if (!configured.empty() && std::filesystem::exists(configured, ec)) return configured;

#if defined(_WIN32)
    static constexpr const char* kExecutable = "curl.exe";
    static constexpr const char* kWellKnown[] = {"C:\\Windows\\System32\\curl.exe"};
    static constexpr char kSeparator = ';';
#else
    static constexpr const char* kExecutable = "curl";
    static constexpr const char* kWellKnown[] = {"/usr/bin/curl", "/usr/local/bin/curl",
                                                 "/opt/homebrew/bin/curl"};
    static constexpr char kSeparator = ':';
#endif

    for (const char* candidate : kWellKnown) {
        if (std::filesystem::exists(candidate, ec)) return candidate;
    }

    const char* path_env = std::getenv("PATH");
    if (!path_env) return {};
    std::string_view rest(path_env);
    while (!rest.empty()) {
        const std::size_t split = rest.find(kSeparator);
        const std::string_view piece = rest.substr(0, split);
        if (!piece.empty()) {
            const std::filesystem::path candidate = std::filesystem::path(piece) / kExecutable;
            if (std::filesystem::exists(candidate, ec)) return candidate;
        }
        if (split == std::string_view::npos) break;
        rest.remove_prefix(split + 1);
    }
    return {};
}

/// Only https, and nothing that could redirect out of it. A URL is data from
/// somewhere else - it may have arrived in a file someone sent - so what it is
/// allowed to reach is decided here rather than by whatever it happens to say.
bool is_acceptable(std::string_view url) {
    return url.rfind("https://", 0) == 0 && url.size() > 8;
}

}  // namespace

std::string_view to_string(FetchStatus s) {
    switch (s) {
        case FetchStatus::Ready: return "ready";
        case FetchStatus::InFlight: return "downloading";
        case FetchStatus::ToolMissing: return "curl not found";
        case FetchStatus::Offline: return "could not be reached";
        case FetchStatus::HttpError: return "the server refused it";
        case FetchStatus::TooLarge: return "too large";
        case FetchStatus::Refused: return "not an https address";
    }
    return "ready";
}

AssetCache::~AssetCache() { stop_worker(); }

void AssetCache::open(std::filesystem::path dir, std::uint64_t budget_bytes) {
    stop_worker();
    std::lock_guard<std::mutex> lock(mutex_);
    dir_ = std::move(dir);
    budget_ = budget_bytes;
    if (dir_.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    running_ = true;
    worker_ = std::thread(&AssetCache::worker_loop, this);
}

void AssetCache::stop_worker() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) return;
        running_ = false;
        queue_.clear();
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void AssetCache::set_tool_path(std::filesystem::path curl) {
    std::lock_guard<std::mutex> lock(mutex_);
    curl_ = std::move(curl);
}

std::filesystem::path AssetCache::default_directory() {
    return CompileCache::default_directory() / "assets";
}

std::filesystem::path AssetCache::path_for(std::string_view url,
                                           std::string_view extension) const {
    return dir_ / (to_hex(fnv1a64(url)) + std::string(extension));
}

std::filesystem::path AssetCache::meta_path_for(std::string_view url) const {
    return dir_ / (to_hex(fnv1a64(url)) + ".meta");
}

std::optional<AssetEntry> AssetCache::lookup(std::string_view url) const {
    if (dir_.empty() || url.empty()) return std::nullopt;

    const auto meta = read_meta(meta_path_for(url));
    // A missing sidecar means a miss even when the bytes are there. It is
    // written after the file is renamed into place, so its absence is exactly
    // how an interrupted download tells you not to trust what it left behind.
    if (meta.empty()) return std::nullopt;

    const auto url_field = meta.find("url");
    // The key is a hash, and a hash is not a promise. Two addresses landing on
    // one key would otherwise serve each other's pixels, which is a very
    // confusing bug to look at.
    if (url_field == meta.end() || url_field->second != url) return std::nullopt;

    AssetEntry entry;
    entry.url = std::string(url);
    const auto get = [&meta](const char* key) -> std::string {
        const auto it = meta.find(key);
        return it == meta.end() ? std::string() : it->second;
    };
    entry.content_type = get("content_type");
    entry.path = path_for(url, extension_for(entry.content_type, url));

    std::error_code ec;
    if (!std::filesystem::exists(entry.path, ec)) return std::nullopt;
    entry.bytes = static_cast<std::uint64_t>(std::filesystem::file_size(entry.path, ec));
    if (ec) return std::nullopt;

    const std::string hash = get("content_hash");
    if (!hash.empty()) entry.content_hash = std::stoull(hash, nullptr, 16);
    const std::string fetched = get("fetched");
    if (!fetched.empty()) entry.fetched_unix = std::stoll(fetched);
    return entry;
}

bool AssetCache::in_flight(std::string_view url) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::find(active_.begin(), active_.end(), url) != active_.end()) return true;
    return std::any_of(queue_.begin(), queue_.end(),
                       [&url](const Job& job) { return job.url == url; });
}

void AssetCache::fetch(std::string url, Callback on_done) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) return;
        const bool queued = std::any_of(queue_.begin(), queue_.end(),
                                        [&url](const Job& job) { return job.url == url; });
        const bool running = std::find(active_.begin(), active_.end(), url) != active_.end();
        // Asking twice for the same address while it is on its way is a click,
        // not a second download.
        if (queued || running) return;
        queue_.push_back(Job{std::move(url), std::move(on_done)});
    }
    cv_.notify_one();
}

void AssetCache::worker_loop() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return !running_ || !queue_.empty(); });
            if (!running_) return;
            job = std::move(queue_.front());
            queue_.pop_front();
            active_.push_back(job.url);
        }

        AssetEntry entry;
        const FetchStatus status = download(job.url, entry);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            active_.erase(std::remove(active_.begin(), active_.end(), job.url), active_.end());
            if (!running_) return;
            finished_.emplace_back(std::move(job), std::make_pair(status, std::move(entry)));
        }
    }
}

FetchStatus AssetCache::download(const std::string& url, AssetEntry& out) const {
    if (!is_acceptable(url)) return FetchStatus::Refused;

    std::filesystem::path configured;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        configured = curl_;
    }
    const std::filesystem::path curl = resolve_curl(configured);
    if (curl.empty()) return FetchStatus::ToolMissing;

    // Written beside the final name and moved into place only once it is whole,
    // so an interrupted download never leaves something that looks cached.
    const std::filesystem::path partial = dir_ / (to_hex(fnv1a64(url)) + ".part");
    std::error_code ec;
    std::filesystem::remove(partial, ec);

    const std::vector<std::string> args = {
        "--fail",
        "--silent",
        "--show-error",
        "--location",
        // https only, and a redirect cannot walk out of it into some other
        // scheme on the way.
        "--proto", "=https",
        "--proto-redir", "=https",
        "--max-time", "30",
        "--max-filesize", std::to_string(kMaxBytes),
        "--write-out", "%{content_type}",
        "--output", partial.string(),
        url,
    };

    const ProcessResult result = run_process(curl, args);
    if (!result.started()) {
        std::filesystem::remove(partial, ec);
        return FetchStatus::ToolMissing;
    }
    if (!result.ok()) {
        std::filesystem::remove(partial, ec);
        // curl says 22 for an HTTP status it was told to fail on, and 63 when
        // the response ran past --max-filesize. Everything else is treated as
        // not having reached the far end.
        if (result.exit_code == 22) return FetchStatus::HttpError;
        if (result.exit_code == 63) return FetchStatus::TooLarge;
        return FetchStatus::Offline;
    }

    // --max-filesize is advisory: a server that sends no Content-Length can run
    // past it, so the size is checked again now that the bytes are here.
    const auto size = static_cast<std::uint64_t>(std::filesystem::file_size(partial, ec));
    if (ec || size > kMaxBytes) {
        std::filesystem::remove(partial, ec);
        return FetchStatus::TooLarge;
    }

    std::string bytes;
    if (!read_whole_file(partial, bytes)) {
        std::filesystem::remove(partial, ec);
        return FetchStatus::Offline;
    }

    out.url = url;
    out.content_type = std::string(trimmed(result.out));
    out.bytes = size;
    out.content_hash = fnv1a64(bytes);
    out.fetched_unix = now_unix();
    out.path = path_for(url, extension_for(out.content_type, url));

    std::filesystem::rename(partial, out.path, ec);
    if (ec) {
        std::filesystem::remove(partial, ec);
        return FetchStatus::Offline;
    }

    // The sidecar goes last, on purpose: it is what lookup() treats as proof
    // that the bytes beside it are complete.
    std::ofstream meta(meta_path_for(url), std::ios::trunc);
    meta << "url: " << url << "\n";
    meta << "content_type: " << out.content_type << "\n";
    meta << "content_hash: " << to_hex(out.content_hash) << "\n";
    meta << "fetched: " << out.fetched_unix << "\n";
    return FetchStatus::Ready;
}

void AssetCache::poll() {
    std::deque<std::pair<Job, std::pair<FetchStatus, AssetEntry>>> done;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        done.swap(finished_);
    }
    // Outside the lock: a callback may well ask the cache something.
    for (auto& [job, result] : done) {
        if (job.on_done) job.on_done(result.first, result.second);
    }
    if (!done.empty()) trim();
}

bool AssetCache::localise(const AssetEntry& entry, const std::filesystem::path& destination,
                          Diagnostics& out_diags) const {
    std::error_code ec;
    if (entry.path.empty() || !std::filesystem::exists(entry.path, ec)) {
        out_diags.push_back(note(Severity::Error,
                                 "nothing cached for " + entry.url + " to copy into the project"));
        return false;
    }
    if (std::filesystem::exists(destination, ec)) {
        out_diags.push_back(
            note(Severity::Error, "refusing to overwrite " + destination.string()));
        return false;
    }
    std::filesystem::create_directories(destination.parent_path(), ec);
    std::filesystem::copy_file(entry.path, destination, ec);
    if (ec) {
        out_diags.push_back(note(Severity::Error, "could not write " + destination.string() +
                                                      ": " + ec.message()));
        return false;
    }
    return true;
}

void AssetCache::trim() {
    if (dir_.empty()) return;

    struct Candidate {
        std::filesystem::path meta;
        std::filesystem::path payload;
        std::int64_t fetched = 0;
        std::uint64_t bytes = 0;
    };
    std::vector<Candidate> entries;
    std::uint64_t total = 0;

    std::error_code ec;
    for (const auto& item : std::filesystem::directory_iterator(dir_, ec)) {
        if (ec) break;
        if (item.path().extension() != ".meta") continue;
        const auto fields = read_meta(item.path());
        const auto url = fields.find("url");
        if (url == fields.end()) continue;

        Candidate candidate;
        candidate.meta = item.path();
        candidate.payload =
            path_for(url->second, extension_for(fields.count("content_type")
                                                    ? fields.at("content_type")
                                                    : std::string(),
                                                url->second));
        candidate.bytes =
            static_cast<std::uint64_t>(std::filesystem::file_size(candidate.payload, ec));
        if (ec) {
            ec.clear();
            candidate.bytes = 0;
        }
        const auto fetched = fields.find("fetched");
        if (fetched != fields.end() && !fetched->second.empty()) {
            candidate.fetched = std::stoll(fetched->second);
        }
        total += candidate.bytes;
        entries.push_back(std::move(candidate));
    }

    if (total <= budget_) return;
    std::sort(entries.begin(), entries.end(),
              [](const Candidate& a, const Candidate& b) { return a.fetched < b.fetched; });

    for (const Candidate& candidate : entries) {
        if (total <= budget_) break;
        // The sidecar goes first. If anything interrupts this, what is left
        // behind reads as a miss rather than as an entry with no bytes.
        std::filesystem::remove(candidate.meta, ec);
        std::filesystem::remove(candidate.payload, ec);
        total -= std::min(total, candidate.bytes);
    }
}

void AssetCache::clear() {
    if (dir_.empty()) return;
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
    std::filesystem::create_directories(dir_, ec);
}

AssetCacheStats AssetCache::stats() const {
    AssetCacheStats out;
    if (dir_.empty()) return out;
    std::error_code ec;
    for (const auto& item : std::filesystem::directory_iterator(dir_, ec)) {
        if (ec) break;
        if (item.path().extension() == ".meta") {
            ++out.entries;
            continue;
        }
        out.bytes += static_cast<std::uint64_t>(item.file_size(ec));
        if (ec) ec.clear();
    }
    return out;
}

}  // namespace ssstudio
