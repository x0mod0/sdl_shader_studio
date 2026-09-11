// Textures for the preview: what a binding is sampled with, and what decodes it.
//
// Everything here is GUI-side. The core never sees a texture, in the same way it
// never sees a shader module - it deals in paths and reflection, and this is what
// turns those into something the GPU can bind.
#ifndef SSSTUDIO_GUI_TEXTURE_REGISTRY_H
#define SSSTUDIO_GUI_TEXTURE_REGISTRY_H

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

#include "ssstudio/sampler_state.h"

namespace ssstudio::gui {

/// The image formats this build can actually decode, as a comma-separated list
/// for the log and the settings panel.
///
/// Worth showing rather than assuming: which decoders a build ends up with
/// depends on what was found when it was configured, and a format quietly
/// missing is otherwise only discovered by the person whose image will not open.
std::string supported_image_formats();

/// True when the build has any image support at all.
bool image_loading_available();

// ---------------------------------------------------------------------------
// The registry
// ---------------------------------------------------------------------------

/// Why a binding is showing what it is showing, for the row that describes it.
enum class TextureStatus : std::uint8_t {
    /// The requested image is on the GPU and bound.
    Ready,
    /// Nothing is bound here; the white stand-in is.
    Unbound,
    /// Being decoded. The stand-in is bound until it arrives.
    Loading,
    /// The path does not exist.
    NotFound,
    /// A URL binding whose bytes have not been downloaded. Not an error: it is
    /// the resting state of a project opened on a machine that has never
    /// fetched them, and it is cleared by asking for the download.
    NotCached,
    /// The file exists but could not be decoded.
    DecodeFailed,
    /// Nothing here can serve this slot - a cube or 3D sampler, or a build with
    /// no image support at all.
    Unsupported,
};

std::string_view to_string(TextureStatus s);

/// The name a pass's render target is registered under.
///
/// The renderer binds pass targets by index, straight out of the schedule, so
/// this is not on the drawing path at all - it exists so the I/O panel can ask
/// what a `pass output` binding currently resolves to and show its size.
std::string pass_target_key(const std::string& pass, std::uint32_t copy);

/// One texture ready to bind. Non-owning: the registry owns what this points at
/// and outlives any resolution handed out during a frame.
struct ResolvedTexture {
    SDL_GPUTexture* texture = nullptr;
    SDL_GPUSampler* sampler = nullptr;
    std::uint32_t width = 1;
    std::uint32_t height = 1;
    /// True when this is the white stand-in rather than what was asked for.
    bool is_fallback = true;
};

/// Releases a GPU texture back to the device that made it.
///
/// At namespace scope rather than nested in the registry on purpose: a deleter
/// nested inside a class has its default member initializer parsed only once the
/// enclosing class is complete, so any member declared before that point cannot
/// see that the deleter is default-constructible and quietly loses its own
/// default constructor.
struct GpuTextureDeleter {
    SDL_GPUDevice* device = nullptr;
    void operator()(SDL_GPUTexture* texture) const noexcept;
};

/// The same, for a sampler.
struct GpuSamplerDeleter {
    SDL_GPUDevice* device = nullptr;
    void operator()(SDL_GPUSampler* sampler) const noexcept;
};

using GpuTexturePtr = std::unique_ptr<SDL_GPUTexture, GpuTextureDeleter>;
using GpuSamplerPtr = std::unique_ptr<SDL_GPUSampler, GpuSamplerDeleter>;

/// Owns every texture the preview samples, and every sampler it samples them
/// with.
///
/// Decoding happens on a worker and uploading happens on the thread that owns
/// the GPU device. That split is not an optimisation: a large image takes long
/// enough to decode to drop frames if done inline, and SDL GPU resources want
/// to be created on one thread.
class TextureRegistry {
public:
    TextureRegistry() = default;
    ~TextureRegistry();

    TextureRegistry(const TextureRegistry&) = delete;
    TextureRegistry& operator=(const TextureRegistry&) = delete;

    bool init(SDL_GPUDevice* device);
    void shutdown();
    bool ready() const { return device_ != nullptr; }

    /// The 1x1 white stand-in, bound wherever the real thing is not available.
    /// Always valid once init() has succeeded.
    ResolvedTexture fallback(const SamplerState& state = {});

    /// Resolves the image at `path`, decoding it if this is the first time it
    /// has been asked for.
    ///
    /// Never blocks: a texture that is not ready yet resolves to the stand-in
    /// with a status of Loading and becomes itself on a later frame.
    ResolvedTexture file(const std::filesystem::path& path, const SamplerState& state,
                         TextureStatus& out_status);

    /// Registers a texture the registry does not own the pixels of, under a
    /// name callers can resolve it by.
    ///
    /// This is how a pass's render target becomes bindable: it is created and
    /// owned by the pass pool, but resolving it has to go through the same path
    /// as an image from disk, or every caller would need to know which kind of
    /// texture it was looking at. Passing null forgets the name.
    void set_external(const std::string& key, SDL_GPUTexture* texture, std::uint32_t width,
                      std::uint32_t height);

    /// Resolves a texture registered with set_external. Falls back to the
    /// stand-in, with a status of NotFound, for a name nobody registered.
    ResolvedTexture external(const std::string& key, const SamplerState& state,
                             TextureStatus& out_status);

    /// Reads `path` again, keeping what is already on the GPU until the new
    /// bytes arrive. What hot reload calls when a file changes underneath.
    void invalidate(const std::filesystem::path& path);

    /// Re-reads every decoded file whose timestamp has moved since it was read,
    /// and says how many that was. Cheaper than the caller keeping its own list
    /// of paths: the registry already knows what it decoded and when.
    std::size_t refresh_changed();

    /// Takes delivery of anything the worker finished and releases textures that
    /// have not been asked for lately. Call once per frame, on the device's
    /// thread, before resolving anything.
    void tick(std::uint64_t frame, std::uint64_t budget_bytes);

    /// Bytes of GPU memory the decoded textures currently occupy.
    std::uint64_t bytes_used() const { return bytes_used_; }

    const std::string& last_error() const { return last_error_; }

private:
    /// What distinguishes one uploaded texture from another.
    ///
    /// Filter and wrap are not in here: they live in the sampler and cost
    /// nothing to vary. A flip, an sRGB format and a mip chain are all decided
    /// when the pixels are uploaded, so two bindings on one file that disagree
    /// about those genuinely need two textures.
    struct Key {
        std::string source;
        bool vflip = false;
        bool srgb = false;
        bool mipmap = false;

        bool operator<(const Key& other) const;
    };

    /// Pixels the worker has decoded, waiting to become a texture.
    struct Decoded {
        Key key;
        /// Tightly packed RGBA8, `width * height * 4` bytes.
        std::vector<std::uint8_t> pixels;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        /// Empty when the decode worked; what went wrong otherwise.
        std::string error;
        TextureStatus status = TextureStatus::Ready;
    };

    struct Entry {
        GpuTexturePtr texture;
        std::uint32_t width = 1;
        std::uint32_t height = 1;
        std::uint64_t bytes = 0;
        /// The frame this was last resolved on, which is what eviction orders by.
        std::uint64_t last_used = 0;
        TextureStatus status = TextureStatus::Loading;
        /// What the file's timestamp was when it was read, so a change on disk
        /// can be noticed without decoding it again.
        std::filesystem::file_time_type mtime{};
    };

    SDL_GPUSampler* sampler_for(const SamplerState& state);
    void enqueue(Key key);
    void worker_loop();
    /// Turns decoded pixels into a texture. Device thread only.
    void upload(Decoded decoded);
    void evict(std::uint64_t budget_bytes);

    SDL_GPUDevice* device_ = nullptr;
    GpuTexturePtr white_;
    std::map<SamplerState, GpuSamplerPtr> samplers_;
    std::map<Key, Entry> entries_;

    /// Textures owned elsewhere, by name. Non-owning on purpose: the pass pool
    /// creates and destroys these, and a second owner would be a second chance
    /// to release them.
    struct External {
        SDL_GPUTexture* texture = nullptr;
        std::uint32_t width = 1;
        std::uint32_t height = 1;
    };
    std::map<std::string, External> externals_;
    std::uint64_t bytes_used_ = 0;
    std::uint64_t frame_ = 0;
    std::string last_error_;

    // --- the decode worker -------------------------------------------------
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Key> pending_;
    std::deque<Decoded> finished_;
    std::thread worker_;
    bool running_ = false;
};

}  // namespace ssstudio::gui

#endif  // SSSTUDIO_GUI_TEXTURE_REGISTRY_H
