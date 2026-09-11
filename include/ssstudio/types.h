// SDL Shader Studio - core value types.
// The core library deliberately does not include SDL. Only the GUI and the
// generated loader header depend on SDL, which keeps the core testable and
// buildable in isolation (see docs/ARCHITECTURE.md).
#ifndef SSSTUDIO_TYPES_H
#define SSSTUDIO_TYPES_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssstudio {

// ---------------------------------------------------------------------------
// Stages
// ---------------------------------------------------------------------------
enum class Stage : std::uint8_t {
    Vertex = 0,
    Fragment = 1,
    Compute = 2,
};

inline constexpr std::uint32_t stage_bit(Stage s) {
    return 1u << static_cast<std::uint32_t>(s);
}

std::string_view to_string(Stage s);
std::optional<Stage> stage_from_string(std::string_view s);

// ---------------------------------------------------------------------------
// Source languages
// ---------------------------------------------------------------------------
enum class Language : std::uint8_t {
    HLSL = 0,
    GLSL = 1,  // compiled through glslang; the pack carries the provenance
               // byte either way.
};

std::string_view to_string(Language l);
std::optional<Language> language_from_string(std::string_view s);

// ---------------------------------------------------------------------------
// Shader binary formats. Values mirror SDL_GPU_SHADERFORMAT_* so a consumer can
// mask directly against SDL_GetGPUShaderFormats() without a translation table.
// ---------------------------------------------------------------------------
enum ShaderFormat : std::uint32_t {
    FORMAT_NONE = 0u,
    FORMAT_PRIVATE = 1u << 0,
    FORMAT_SPIRV = 1u << 1,
    FORMAT_DXBC = 1u << 2,
    FORMAT_DXIL = 1u << 3,
    FORMAT_MSL = 1u << 4,
    FORMAT_METALLIB = 1u << 5,
};

std::string_view to_string(ShaderFormat f);
std::optional<ShaderFormat> format_from_string(std::string_view s);
std::vector<ShaderFormat> formats_in_mask(std::uint32_t mask);
std::string format_mask_to_string(std::uint32_t mask);

// ---------------------------------------------------------------------------
// Build knobs
// ---------------------------------------------------------------------------
enum class KeyStrategy : std::uint8_t {
    Enum = 0,      // sequential, assigned in manifest order
    Hash = 1,      // FNV-1a 32 of the shader id, collision-checked
    Explicit = 2,  // stable ids stored in project.toml (default)
};

std::string_view to_string(KeyStrategy k);
std::optional<KeyStrategy> key_strategy_from_string(std::string_view s);

enum class Compression : std::uint8_t {
    None = 0,
    LZ4 = 1,
    Zstd = 2,
};

std::string_view to_string(Compression c);
std::optional<Compression> compression_from_string(std::string_view s);

enum class Severity : std::uint8_t { Info, Warning, Error };
std::string_view to_string(Severity s);

struct Diagnostic {
    Severity severity = Severity::Error;
    std::string file;
    int line = 0;
    int column = 0;
    std::string message;
    std::string code;  // compiler-specific code, may be empty

    std::string format() const;
};

using Diagnostics = std::vector<Diagnostic>;

bool has_errors(const Diagnostics& d);

}  // namespace ssstudio

#endif  // SSSTUDIO_TYPES_H
