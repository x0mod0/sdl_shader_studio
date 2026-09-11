// The on-disk cache for fetched files.
//
// Nothing here touches the network. What is worth testing is the bookkeeping
// around a download rather than the download: which files count as a hit, what
// an interrupted transfer leaves behind, and whether a key collision can serve
// one address's bytes for another's.
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "ssstudio/asset_cache.h"
#include "ssstudio/hash.h"
#include "test.h"

using namespace ssstudio;

namespace {

std::filesystem::path cache_dir(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() / "ssstudio-tests" / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void write_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

/// Fakes what a completed download leaves behind: the bytes, then the sidecar.
void plant(const std::filesystem::path& dir, const std::string& url, const std::string& bytes,
           const char* extension = ".png", const char* content_type = "image/png",
           std::int64_t fetched = 1000) {
    const std::string key = to_hex(fnv1a64(url));
    write_file(dir / (key + extension), bytes);
    std::ofstream meta(dir / (key + ".meta"), std::ios::trunc);
    meta << "url: " << url << "\n";
    meta << "content_type: " << content_type << "\n";
    meta << "etag: \n";
    meta << "content_hash: " << to_hex(fnv1a64(bytes)) << "\n";
    meta << "fetched: " << fetched << "\n";
}

}  // namespace

TEST(asset_cache_finds_what_was_planted) {
    const auto dir = cache_dir("asset_hit");
    AssetCache cache;
    cache.open(dir, 1024 * 1024);
    plant(dir, "https://example.invalid/a.png", "pixels");

    const auto entry = cache.lookup("https://example.invalid/a.png");
    CHECK(entry.has_value());
    CHECK_STREQ(entry->url, "https://example.invalid/a.png");
    CHECK_EQ(entry->bytes, static_cast<std::uint64_t>(6));
    CHECK_EQ(entry->content_hash, fnv1a64(std::string("pixels")));
    CHECK(std::filesystem::exists(entry->path));
}

TEST(asset_cache_misses_when_the_sidecar_is_absent) {
    const auto dir = cache_dir("asset_partial");
    AssetCache cache;
    cache.open(dir, 1024 * 1024);

    // Bytes with no sidecar are what an interrupted download leaves. Treating
    // them as a hit would serve half a file as if it were whole.
    const std::string url = "https://example.invalid/b.png";
    write_file(dir / (to_hex(fnv1a64(url)) + ".png"), "half a fi");
    CHECK(!cache.lookup(url).has_value());
}

TEST(asset_cache_misses_when_the_sidecar_names_another_address) {
    const auto dir = cache_dir("asset_collision");
    AssetCache cache;
    cache.open(dir, 1024 * 1024);

    // The key is a hash, and a hash is not a promise. Without this check two
    // addresses that collided would quietly serve each other's pixels.
    const std::string url = "https://example.invalid/c.png";
    const std::string key = to_hex(fnv1a64(url));
    write_file(dir / (key + ".png"), "pixels");
    std::ofstream meta(dir / (key + ".meta"), std::ios::trunc);
    meta << "url: https://example.invalid/somewhere-else.png\n";
    meta << "content_type: image/png\n";
    meta.close();

    CHECK(!cache.lookup(url).has_value());
}

TEST(asset_cache_misses_when_the_bytes_are_gone) {
    const auto dir = cache_dir("asset_no_bytes");
    AssetCache cache;
    cache.open(dir, 1024 * 1024);
    plant(dir, "https://example.invalid/d.png", "pixels");

    // Someone emptied the cache directory by hand, which is meant to be safe.
    std::error_code ec;
    std::filesystem::remove(dir / (to_hex(fnv1a64(std::string("https://example.invalid/d.png"))) +
                                   ".png"),
                            ec);
    CHECK(!cache.lookup("https://example.invalid/d.png").has_value());
}

TEST(asset_cache_never_fetches_a_non_https_address) {
    const auto dir = cache_dir("asset_scheme");
    AssetCache cache;
    cache.open(dir, 1024 * 1024);

    // A URL may have arrived inside a file someone else wrote, so what it is
    // allowed to reach is decided here rather than by what it says.
    for (const char* url : {"http://example.invalid/a.png", "file:///etc/passwd",
                            "ftp://example.invalid/a.png", ""}) {
        FetchStatus status = FetchStatus::Ready;
        bool called = false;
        cache.fetch(url, [&](FetchStatus s, const AssetEntry&) {
            status = s;
            called = true;
        });
        // The answer comes back through poll() from a worker, so this waits for
        // it rather than spinning: polling in a tight loop simply outruns the
        // thread it is waiting on.
        for (int spin = 0; spin < 400 && !called; ++spin) {
            cache.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(called);
        CHECK(status != FetchStatus::Ready);
    }
}

TEST(asset_cache_trims_the_oldest_first) {
    const auto dir = cache_dir("asset_trim");
    AssetCache cache;
    // Room for roughly two of the three below.
    cache.open(dir, 24);
    plant(dir, "https://example.invalid/old.png", "0123456789", ".png", "image/png", 100);
    plant(dir, "https://example.invalid/mid.png", "0123456789", ".png", "image/png", 200);
    plant(dir, "https://example.invalid/new.png", "0123456789", ".png", "image/png", 300);

    cache.trim();
    CHECK(!cache.lookup("https://example.invalid/old.png").has_value());
    CHECK(cache.lookup("https://example.invalid/new.png").has_value());
}

TEST(asset_cache_localise_copies_and_refuses_to_overwrite) {
    const auto dir = cache_dir("asset_localise");
    AssetCache cache;
    cache.open(dir, 1024 * 1024);
    plant(dir, "https://example.invalid/e.png", "pixels");

    const auto entry = cache.lookup("https://example.invalid/e.png");
    CHECK(entry.has_value());

    const auto destination = dir / "project" / "assets" / "e.png";
    Diagnostics diags;
    CHECK(cache.localise(*entry, destination, diags));
    CHECK(std::filesystem::exists(destination));
    CHECK(!has_errors(diags));

    // Twice is a refusal, not a silent replacement: the file may have been
    // edited since it was copied.
    Diagnostics second;
    CHECK(!cache.localise(*entry, destination, second));
    CHECK(has_errors(second));
}

TEST(asset_cache_clear_leaves_a_usable_directory) {
    const auto dir = cache_dir("asset_clear");
    AssetCache cache;
    cache.open(dir, 1024 * 1024);
    plant(dir, "https://example.invalid/f.png", "pixels");
    CHECK_EQ(cache.stats().entries, static_cast<std::uint64_t>(1));

    cache.clear();
    CHECK_EQ(cache.stats().entries, static_cast<std::uint64_t>(0));
    CHECK(std::filesystem::is_directory(dir));
    CHECK(!cache.lookup("https://example.invalid/f.png").has_value());
}

TEST(asset_cache_sits_beside_the_compile_cache) {
    // Same parent, different directory: the two share nothing but a location,
    // and either can be deleted without touching the other.
    const auto assets = AssetCache::default_directory();
    CHECK_STREQ(assets.filename().string(), "assets");
}
