// SPIR-V -> Reflection. Self-contained: no SPIRV-Cross dependency.
#ifndef SSSTUDIO_SPIRV_REFLECT_H
#define SSSTUDIO_SPIRV_REFLECT_H

#include <cstdint>
#include <string>
#include <vector>

#include "ssstudio/reflection.h"

namespace ssstudio {

// Parses `spirv` and fills the fields the tool needs. Anything unrecognized is
// skipped rather than fatal; problems are reported as warnings so a shader that
// compiles still previews even if reflection is partial.
Reflection reflect_spirv(const std::vector<std::uint8_t>& spirv, Stage stage,
                         const std::string& entry_point, Diagnostics& out_diags);

}  // namespace ssstudio

#endif  // SSSTUDIO_SPIRV_REFLECT_H
