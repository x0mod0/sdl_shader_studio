// Compression shim. LZ4 and zstd are optional at build time; when a codec is
// missing the packer falls back to storing raw and says so in the diagnostics
// rather than silently producing a pack the loader cannot read.
#ifndef SSSTUDIO_COMPRESS_H
#define SSSTUDIO_COMPRESS_H

#include <cstdint>
#include <vector>

#include "ssstudio/types.h"

namespace ssstudio {

bool compression_available(Compression c);

// Returns the encoded bytes. `out_used` reports what was actually applied,
// which may be Compression::None if the codec is unavailable or the result grew.
std::vector<std::uint8_t> compress(const std::vector<std::uint8_t>& in, Compression c,
                                   Compression* out_used);

// `expected_size` is the recorded uncompressed size; returns empty on failure.
std::vector<std::uint8_t> decompress(const std::uint8_t* data, std::uint32_t size,
                                     std::uint32_t expected_size, Compression c);

}  // namespace ssstudio

#endif  // SSSTUDIO_COMPRESS_H
