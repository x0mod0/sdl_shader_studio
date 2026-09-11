#include "ssstudio/compress.h"

#include <cstring>

#if defined(SSSTUDIO_HAVE_LZ4)
#include <lz4.h>
#endif
#if defined(SSSTUDIO_HAVE_ZSTD)
#include <zstd.h>
#endif

namespace ssstudio {

bool compression_available(Compression c) {
    switch (c) {
        case Compression::None:
            return true;
        case Compression::LZ4:
#if defined(SSSTUDIO_HAVE_LZ4)
            return true;
#else
            return false;
#endif
        case Compression::Zstd:
#if defined(SSSTUDIO_HAVE_ZSTD)
            return true;
#else
            return false;
#endif
    }
    return false;
}

std::vector<std::uint8_t> compress(const std::vector<std::uint8_t>& in, Compression c,
                                   Compression* out_used) {
    auto store_raw = [&]() {
        if (out_used) *out_used = Compression::None;
        return in;
    };

    if (in.empty() || c == Compression::None || !compression_available(c)) return store_raw();

#if defined(SSSTUDIO_HAVE_LZ4)
    if (c == Compression::LZ4) {
        const int bound = LZ4_compressBound(static_cast<int>(in.size()));
        std::vector<std::uint8_t> out(static_cast<std::size_t>(bound));
        const int n = LZ4_compress_default(reinterpret_cast<const char*>(in.data()),
                                           reinterpret_cast<char*>(out.data()),
                                           static_cast<int>(in.size()), bound);
        if (n <= 0 || static_cast<std::size_t>(n) >= in.size()) return store_raw();
        out.resize(static_cast<std::size_t>(n));
        if (out_used) *out_used = Compression::LZ4;
        return out;
    }
#endif
#if defined(SSSTUDIO_HAVE_ZSTD)
    if (c == Compression::Zstd) {
        const std::size_t bound = ZSTD_compressBound(in.size());
        std::vector<std::uint8_t> out(bound);
        const std::size_t n = ZSTD_compress(out.data(), bound, in.data(), in.size(), 9);
        if (ZSTD_isError(n) || n >= in.size()) return store_raw();
        out.resize(n);
        if (out_used) *out_used = Compression::Zstd;
        return out;
    }
#endif
    return store_raw();
}

std::vector<std::uint8_t> decompress(const std::uint8_t* data, std::uint32_t size,
                                     std::uint32_t expected_size, Compression c) {
    if (size == expected_size || c == Compression::None) {
        return std::vector<std::uint8_t>(data, data + size);
    }
#if defined(SSSTUDIO_HAVE_LZ4)
    if (c == Compression::LZ4) {
        std::vector<std::uint8_t> out(expected_size);
        const int n = LZ4_decompress_safe(reinterpret_cast<const char*>(data),
                                          reinterpret_cast<char*>(out.data()),
                                          static_cast<int>(size),
                                          static_cast<int>(expected_size));
        if (n < 0 || static_cast<std::uint32_t>(n) != expected_size) return {};
        return out;
    }
#endif
#if defined(SSSTUDIO_HAVE_ZSTD)
    if (c == Compression::Zstd) {
        std::vector<std::uint8_t> out(expected_size);
        const std::size_t n = ZSTD_decompress(out.data(), expected_size, data, size);
        if (ZSTD_isError(n) || n != expected_size) return {};
        return out;
    }
#endif
    return {};
}

}  // namespace ssstudio
