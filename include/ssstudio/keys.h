// Shader key assignment. Keys are always numeric; strings never appear on the
// runtime path.
#ifndef SSSTUDIO_KEYS_H
#define SSSTUDIO_KEYS_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "ssstudio/types.h"

namespace ssstudio {

struct KeyRequest {
    std::string id;                       // manifest id
    std::optional<std::uint32_t> pinned;  // explicit key from project.toml
};

// Assigns a key per request according to `strategy`. Collisions and, for the
// Explicit strategy, unpinned ids are reported as diagnostics. With Explicit,
// newly assigned keys are returned in `out_new_pins` so the caller can write
// them back into the manifest and keep them stable forever after.
std::vector<std::uint32_t> assign_keys(const std::vector<KeyRequest>& requests,
                                       KeyStrategy strategy, bool key16,
                                       Diagnostics& out_diags,
                                       std::map<std::string, std::uint32_t>* out_new_pins);

// C identifier for a shader id, e.g. "sprite_frag" -> "SHADER_SPRITE_FRAG".
std::string enum_name(const std::string& prefix, const std::string& id);

}  // namespace ssstudio

#endif  // SSSTUDIO_KEYS_H
