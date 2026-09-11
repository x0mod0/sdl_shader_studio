// Small hashing helpers. FNV-1a is used for shader keys (documented, stable and
// reproducible outside this tool) and for cache invalidation.
#ifndef SSSTUDIO_HASH_H
#define SSSTUDIO_HASH_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ssstudio {

std::uint32_t fnv1a32(std::string_view s);
std::uint64_t fnv1a64(std::string_view s);
std::uint64_t fnv1a64(const std::vector<std::uint8_t>& bytes);

// Lowercase hex, used for cache keys and asset hashes in the manifest.
std::string to_hex(std::uint64_t v, int digits = 16);

}  // namespace ssstudio

#endif  // SSSTUDIO_HASH_H
