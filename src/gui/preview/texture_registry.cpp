#include "preview/texture_registry.h"

#include <algorithm>
#include <cstring>
#include <tuple>

#if defined(SSSTUDIO_HAVE_SDL_IMAGE)
#include <SDL3_image/SDL_image.h>
#endif

namespace ssstudio::gui {

std::string supported_image_formats() {
#if defined(SSSTUDIO_HAVE_SDL_IMAGE)
    // Read off the build's own switches rather than probed: there is no runtime
    // "could you decode this" short of handing it a file. The formats below the
    // conditional are the ones this project always turns on.
    std::string formats = "png, jpeg, webp, bmp, gif, tga, qoi, pnm, svg";
#if defined(SSSTUDIO_IMAGE_AVIF)
    formats += ", avif";
#endif
    return formats;
#else
    return {};
#endif
}

bool image_loading_available() {
#if defined(SSSTUDIO_HAVE_SDL_IMAGE)
    return true;
#else
    return false;
#endif
}

// ---------------------------------------------------------------------------
// The registry
// ---------------------------------------------------------------------------
namespace {

constexpr SDL_GPUTextureFormat kLinearFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
constexpr SDL_GPUTextureFormat kSrgbFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;

/// Mip levels for an image of this size, counting the full-size one.
std::uint32_t mip_levels(std::uint32_t width, std::uint32_t height) {
    std::uint32_t levels = 1;
    std::uint32_t extent = std::max(width, height);
    while (extent > 1) {
        extent /= 2;
        ++levels;
    }
    return levels;
}

/// What a texture costs on the GPU. A mip chain adds a third again, and the
/// file's own size says nothing about any of it: a 200 KB photograph is 32 MB
/// once it is four bytes per pixel.
std::uint64_t gpu_bytes(std::uint32_t width, std::uint32_t height, bool mipmap) {
    const std::uint64_t base = static_cast<std::uint64_t>(width) * height * 4ull;
    return mipmap ? base + base / 3ull : base;
}

}  // namespace

std::string pass_target_key(const std::string& pass, std::uint32_t copy) {
    return copy == 0 ? pass : pass + "#" + std::to_string(copy);
}

std::string_view to_string(TextureStatus s) {
    switch (s) {
        case TextureStatus::Ready: return "ready";
        case TextureStatus::Unbound: return "unbound";
        case TextureStatus::Loading: return "loading";
        case TextureStatus::NotFound: return "not found";
        case TextureStatus::NotCached: return "not downloaded";
        case TextureStatus::DecodeFailed: return "could not be decoded";
        case TextureStatus::Unsupported: return "unsupported";
    }
    return "unbound";
}

void GpuTextureDeleter::operator()(SDL_GPUTexture* texture) const noexcept {
    if (device && texture) SDL_ReleaseGPUTexture(device, texture);
}

void GpuSamplerDeleter::operator()(SDL_GPUSampler* sampler) const noexcept {
    if (device && sampler) SDL_ReleaseGPUSampler(device, sampler);
}

bool TextureRegistry::Key::operator<(const Key& other) const {
    return std::tie(source, vflip, srgb, mipmap) <
           std::tie(other.source, other.vflip, other.srgb, other.mipmap);
}

TextureRegistry::~TextureRegistry() { shutdown(); }

bool TextureRegistry::init(SDL_GPUDevice* device) {
    if (!device) {
        last_error_ = "no GPU device";
        return false;
    }
    device_ = device;

    // A 1x1 white texture stands in for anything not bound or not ready, so an
    // unfinished shader still draws instead of failing to build a pipeline.
    SDL_GPUTextureCreateInfo info;
    SDL_zero(info);
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = kLinearFormat;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = 1;
    info.height = 1;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    white_ = GpuTexturePtr(SDL_CreateGPUTexture(device_, &info), GpuTextureDeleter{device_});
    if (!white_) {
        last_error_ = SDL_GetError();
        device_ = nullptr;
        return false;
    }

    const std::uint8_t pixel[4] = {255, 255, 255, 255};
    SDL_GPUTransferBufferCreateInfo transfer_info;
    SDL_zero(transfer_info);
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size = sizeof(pixel);
    if (SDL_GPUTransferBuffer* staging = SDL_CreateGPUTransferBuffer(device_, &transfer_info)) {
        if (void* mapped = SDL_MapGPUTransferBuffer(device_, staging, false)) {
            SDL_memcpy(mapped, pixel, sizeof(pixel));
            SDL_UnmapGPUTransferBuffer(device_, staging);

            SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
            SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTextureTransferInfo source;
            SDL_zero(source);
            source.transfer_buffer = staging;
            SDL_GPUTextureRegion destination;
            SDL_zero(destination);
            destination.texture = white_.get();
            destination.w = 1;
            destination.h = 1;
            destination.d = 1;
            SDL_UploadToGPUTexture(pass, &source, &destination, false);
            SDL_EndGPUCopyPass(pass);
            SDL_SubmitGPUCommandBuffer(cmd);
        }
        SDL_ReleaseGPUTransferBuffer(device_, staging);
    }

    running_ = true;
    worker_ = std::thread(&TextureRegistry::worker_loop, this);
    return true;
}

void TextureRegistry::shutdown() {
    if (running_) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
            pending_.clear();
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }
    // Every GPU object here owns itself, so releasing is a matter of letting go
    // in an order that cannot outlive the device.
    entries_.clear();
    externals_.clear();
    samplers_.clear();
    white_.reset();
    finished_.clear();
    bytes_used_ = 0;
    device_ = nullptr;
}

SDL_GPUSampler* TextureRegistry::sampler_for(const SamplerState& state) {
    if (!device_) return nullptr;
    if (const auto it = samplers_.find(state); it != samplers_.end()) return it->second.get();

    const bool nearest = state.filter == SamplerState::Filter::Nearest;
    const bool mipmap = state.filter == SamplerState::Filter::Mipmap;
    const SDL_GPUSamplerAddressMode address = state.wrap == SamplerState::Wrap::Repeat
                                                  ? SDL_GPU_SAMPLERADDRESSMODE_REPEAT
                                                  : SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;

    SDL_GPUSamplerCreateInfo info;
    SDL_zero(info);
    info.min_filter = nearest ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
    info.mag_filter = info.min_filter;
    info.mipmap_mode = nearest ? SDL_GPU_SAMPLERMIPMAPMODE_NEAREST
                               : SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    info.address_mode_u = address;
    info.address_mode_v = address;
    info.address_mode_w = address;
    // Without a chain there is only level zero to sample, so the range is capped
    // rather than left open: an open range over a single-level texture is how
    // some drivers end up sampling nothing.
    info.min_lod = 0.0f;
    info.max_lod = mipmap ? 1000.0f : 0.0f;

    GpuSamplerPtr sampler(SDL_CreateGPUSampler(device_, &info), GpuSamplerDeleter{device_});
    if (!sampler) {
        last_error_ = SDL_GetError();
        return nullptr;
    }
    SDL_GPUSampler* raw = sampler.get();
    samplers_.emplace(state, std::move(sampler));
    return raw;
}

ResolvedTexture TextureRegistry::fallback(const SamplerState& state) {
    ResolvedTexture resolved;
    resolved.texture = white_.get();
    resolved.sampler = sampler_for(state);
    resolved.width = 1;
    resolved.height = 1;
    resolved.is_fallback = true;
    return resolved;
}

ResolvedTexture TextureRegistry::file(const std::filesystem::path& path,
                                      const SamplerState& state, TextureStatus& out_status) {
    if (!device_) {
        out_status = TextureStatus::Unsupported;
        return fallback(state);
    }
    if (path.empty()) {
        out_status = TextureStatus::Unbound;
        return fallback(state);
    }
    if (!image_loading_available()) {
        out_status = TextureStatus::Unsupported;
        return fallback(state);
    }

    Key key;
    key.source = path.generic_string();
    key.vflip = state.vflip;
    key.srgb = state.srgb;
    key.mipmap = state.filter == SamplerState::Filter::Mipmap;

    const auto it = entries_.find(key);
    if (it == entries_.end()) {
        Entry entry;
        entry.status = TextureStatus::Loading;
        entry.last_used = frame_;
        std::error_code ec;
        entry.mtime = std::filesystem::last_write_time(path, ec);
        entries_.emplace(key, std::move(entry));
        enqueue(key);
        out_status = TextureStatus::Loading;
        return fallback(state);
    }

    it->second.last_used = frame_;
    out_status = it->second.status;
    if (it->second.status != TextureStatus::Ready || !it->second.texture) {
        return fallback(state);
    }

    ResolvedTexture resolved;
    resolved.texture = it->second.texture.get();
    resolved.sampler = sampler_for(state);
    resolved.width = it->second.width;
    resolved.height = it->second.height;
    resolved.is_fallback = false;
    return resolved;
}

void TextureRegistry::set_external(const std::string& key, SDL_GPUTexture* texture,
                                  std::uint32_t width, std::uint32_t height) {
    if (!texture) {
        externals_.erase(key);
        return;
    }
    External& entry = externals_[key];
    entry.texture = texture;
    entry.width = std::max(width, 1u);
    entry.height = std::max(height, 1u);
}

ResolvedTexture TextureRegistry::external(const std::string& key, const SamplerState& state,
                                          TextureStatus& out_status) {
    const auto it = externals_.find(key);
    if (it == externals_.end() || !it->second.texture) {
        out_status = TextureStatus::NotFound;
        return fallback(state);
    }

    ResolvedTexture resolved;
    resolved.texture = it->second.texture;
    resolved.sampler = sampler_for(state);
    resolved.width = it->second.width;
    resolved.height = it->second.height;
    resolved.is_fallback = false;
    out_status = TextureStatus::Ready;
    return resolved;
}

void TextureRegistry::invalidate(const std::filesystem::path& path) {
    const std::string source = path.generic_string();
    // Every upload variant of the same file is re-read, since the bytes behind
    // all of them changed. What is already on the GPU stays bound until the new
    // pixels arrive: an image being written by another program is readable half
    // way through, and flashing to white every time someone saves in their
    // editor is worse than showing the previous version for another second.
    for (auto& [key, entry] : entries_) {
        if (key.source != source) continue;
        std::error_code ec;
        entry.mtime = std::filesystem::last_write_time(path, ec);
        enqueue(key);
    }
}

std::size_t TextureRegistry::refresh_changed() {
    std::vector<std::filesystem::path> changed;
    for (const auto& [key, entry] : entries_) {
        std::error_code ec;
        const auto now = std::filesystem::last_write_time(key.source, ec);
        if (ec || now == entry.mtime) continue;
        changed.emplace_back(key.source);
    }
    // Collected first: invalidate() writes to the same map it would be iterating.
    for (const auto& path : changed) invalidate(path);
    return changed.size();
}

void TextureRegistry::enqueue(Key key) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.push_back(std::move(key));
    }
    cv_.notify_one();
}

void TextureRegistry::tick(std::uint64_t frame, std::uint64_t budget_bytes) {
    frame_ = frame;
    if (!device_) return;

    std::deque<Decoded> ready;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ready.swap(finished_);
    }
    for (Decoded& decoded : ready) upload(std::move(decoded));

    evict(budget_bytes);
}

void TextureRegistry::evict(std::uint64_t budget_bytes) {
    if (bytes_used_ <= budget_bytes) return;

    // Oldest first, and never anything resolved this frame: dropping a texture
    // that is about to be bound would only decode it again next frame.
    std::vector<std::pair<std::uint64_t, Key>> candidates;
    for (const auto& [key, entry] : entries_) {
        if (entry.last_used >= frame_) continue;
        if (!entry.texture) continue;
        candidates.emplace_back(entry.last_used, key);
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    for (const auto& [used, key] : candidates) {
        if (bytes_used_ <= budget_bytes) break;
        const auto it = entries_.find(key);
        if (it == entries_.end()) continue;
        bytes_used_ -= std::min(bytes_used_, it->second.bytes);
        entries_.erase(it);
    }
}

void TextureRegistry::worker_loop() {
    for (;;) {
        Key key;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return !running_ || !pending_.empty(); });
            if (!running_) return;
            key = std::move(pending_.front());
            pending_.pop_front();
        }

        Decoded decoded;
        decoded.key = key;

#if defined(SSSTUDIO_HAVE_SDL_IMAGE)
        struct SurfaceDeleter {
            void operator()(SDL_Surface* s) const noexcept {
                if (s) SDL_DestroySurface(s);
            }
        };
        using SurfacePtr = std::unique_ptr<SDL_Surface, SurfaceDeleter>;

        SurfacePtr loaded(IMG_Load(key.source.c_str()));
        if (!loaded) {
            decoded.error = SDL_GetError();
            decoded.status = std::filesystem::exists(key.source) ? TextureStatus::DecodeFailed
                                                                 : TextureStatus::NotFound;
        } else {
            // Converted unconditionally rather than only when the format looks
            // wrong. What comes back depends on the file - an indexed PNG, a
            // 24-bit BMP, 16 bits per channel - and the conversion is a no-op
            // when it is already what was asked for. RGBA32 is the byte-order
            // spelling, so it is RGBA in memory on either endianness.
            SurfacePtr rgba(SDL_ConvertSurface(loaded.get(), SDL_PIXELFORMAT_RGBA32));
            if (!rgba) {
                decoded.error = SDL_GetError();
                decoded.status = TextureStatus::DecodeFailed;
            } else {
                decoded.width = static_cast<std::uint32_t>(rgba->w);
                decoded.height = static_cast<std::uint32_t>(rgba->h);
                const std::size_t row_bytes = static_cast<std::size_t>(rgba->w) * 4u;
                decoded.pixels.resize(row_bytes * static_cast<std::size_t>(rgba->h));

                // Copied row by row using the surface's own pitch, which is not
                // always the width times four. Assuming it is produces an image
                // sheared diagonally, which reads as a decoder bug and is not.
                const auto* source = static_cast<const std::uint8_t*>(rgba->pixels);
                for (int y = 0; y < rgba->h; ++y) {
                    const int destination_row = key.vflip ? (rgba->h - 1 - y) : y;
                    std::memcpy(decoded.pixels.data() + destination_row * row_bytes,
                                source + static_cast<std::size_t>(y) * rgba->pitch, row_bytes);
                }
                decoded.status = TextureStatus::Ready;
            }
        }
#else
        decoded.status = TextureStatus::Unsupported;
        decoded.error = "this build has no image support";
#endif

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) return;
            finished_.push_back(std::move(decoded));
        }
    }
}

void TextureRegistry::upload(Decoded decoded) {
    const auto it = entries_.find(decoded.key);
    // The entry can be gone: the binding may have been changed or the project
    // closed while the decode was in flight. Nothing to put the pixels in.
    if (it == entries_.end()) return;

    Entry& entry = it->second;
    if (decoded.status != TextureStatus::Ready) {
        if (!decoded.error.empty()) last_error_ = decoded.error;
        // A re-read that failed leaves standing whatever was already there. Only
        // a first attempt has nothing better to show than the reason it failed.
        if (!entry.texture) entry.status = decoded.status;
        return;
    }
    if (decoded.width == 0 || decoded.height == 0) {
        entry.status = TextureStatus::DecodeFailed;
        return;
    }

    const bool mipmap = decoded.key.mipmap;
    SDL_GPUTextureFormat format = decoded.key.srgb ? kSrgbFormat : kLinearFormat;
    // The conversion is asked of the hardware rather than done to the pixels:
    // an 8-bit image linearised on the CPU loses exactly the precision in the
    // darks that sRGB encoding existed to keep.
    SDL_GPUTextureUsageFlags usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    if (mipmap) usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    if (!SDL_GPUTextureSupportsFormat(device_, format, SDL_GPU_TEXTURETYPE_2D, usage)) {
        format = kLinearFormat;
    }

    SDL_GPUTextureCreateInfo info;
    SDL_zero(info);
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = usage;
    info.width = decoded.width;
    info.height = decoded.height;
    info.layer_count_or_depth = 1;
    info.num_levels = mipmap ? mip_levels(decoded.width, decoded.height) : 1;

    GpuTexturePtr texture(SDL_CreateGPUTexture(device_, &info), GpuTextureDeleter{device_});
    if (!texture) {
        last_error_ = SDL_GetError();
        entry.status = TextureStatus::DecodeFailed;
        return;
    }

    SDL_GPUTransferBufferCreateInfo transfer_info;
    SDL_zero(transfer_info);
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size = static_cast<Uint32>(decoded.pixels.size());
    SDL_GPUTransferBuffer* staging = SDL_CreateGPUTransferBuffer(device_, &transfer_info);
    if (!staging) {
        last_error_ = SDL_GetError();
        entry.status = TextureStatus::DecodeFailed;
        return;
    }

    bool uploaded = false;
    if (void* mapped = SDL_MapGPUTransferBuffer(device_, staging, false)) {
        SDL_memcpy(mapped, decoded.pixels.data(), decoded.pixels.size());
        SDL_UnmapGPUTransferBuffer(device_, staging);

        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
        SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureTransferInfo source;
        SDL_zero(source);
        source.transfer_buffer = staging;
        SDL_GPUTextureRegion destination;
        SDL_zero(destination);
        destination.texture = texture.get();
        destination.w = decoded.width;
        destination.h = decoded.height;
        destination.d = 1;
        SDL_UploadToGPUTexture(pass, &source, &destination, false);
        SDL_EndGPUCopyPass(pass);
        if (mipmap) SDL_GenerateMipmapsForGPUTexture(cmd, texture.get());
        SDL_SubmitGPUCommandBuffer(cmd);
        uploaded = true;
    } else {
        last_error_ = SDL_GetError();
    }
    // Released on every path out, including the one where mapping failed.
    SDL_ReleaseGPUTransferBuffer(device_, staging);

    if (!uploaded) {
        entry.status = TextureStatus::DecodeFailed;
        return;
    }

    bytes_used_ -= std::min(bytes_used_, entry.bytes);
    entry.texture = std::move(texture);
    entry.width = decoded.width;
    entry.height = decoded.height;
    entry.bytes = gpu_bytes(decoded.width, decoded.height, mipmap);
    entry.status = TextureStatus::Ready;
    bytes_used_ += entry.bytes;
}

}  // namespace ssstudio::gui
