// How a bound texture is sampled, and how it is read on the way in.
//
// Pure data, in the core rather than beside the renderer, because this is what a
// texture binding stores in `project.toml` - it is part of the manifest before
// it is anything to do with a GPU.
#ifndef SSSTUDIO_SAMPLER_STATE_H
#define SSSTUDIO_SAMPLER_STATE_H

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace ssstudio {

/// How a bound texture is sampled, and how it is read on the way in.
///
/// This is the whole of what a texture binding can say beyond where its pixels
/// come from. It is deliberately small: every field here has to survive a round
/// trip through the manifest as text, and each one that exists is one more thing
/// a project file can disagree with a future version about.
struct SamplerState {
    /// How the texture is filtered. Mipmap implies a full chain is built at
    /// upload, which the other two do not need.
    enum class Filter : std::uint8_t { Nearest, Linear, Mipmap };
    /// What happens outside the 0..1 range, applied to both axes.
    enum class Wrap : std::uint8_t { Clamp, Repeat };

    Filter filter = Filter::Linear;
    Wrap wrap = Wrap::Clamp;
    /// Flip vertically at upload time. The default for an imported channel,
    /// whose convention puts the origin at the bottom left, and doing it here
    /// keeps the flip out of every place the texture is sampled.
    bool vflip = false;
    /// Treat the file's values as sRGB and linearise them at upload.
    ///
    /// Done on the way in rather than by asking the GPU for an sRGB format, so
    /// that there is exactly one place it can happen. Doing both is the classic
    /// way to end up with a washed-out image and no idea which half to remove.
    bool srgb = false;

    /// Ordering so a cache can key on the state. Samplers are few - there are
    /// six combinations of filter and wrap - so they are made once and shared.
    bool operator<(const SamplerState& other) const;
    bool operator==(const SamplerState& other) const;
};

std::string_view to_string(SamplerState::Filter f);
std::string_view to_string(SamplerState::Wrap w);

/// Reads sampler state out of a binding's `extra` map.
///
/// The map is free-form text from `project.toml`, so anything may be missing,
/// misspelled or written by a version that knew more than this one.
SamplerState sampler_state_from_extra(const std::map<std::string, std::string>& extra);

/// Writes sampler state into a binding's `extra` map, leaving unrelated keys
/// alone. Only values that differ from the default are stored, which is the rule
/// the rest of the manifest follows.
void sampler_state_to_extra(const SamplerState& state, std::map<std::string, std::string>& extra);

}  // namespace ssstudio

#endif  // SSSTUDIO_SAMPLER_STATE_H
