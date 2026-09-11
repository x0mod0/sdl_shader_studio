#include "ssstudio/sampler_state.h"

#include <optional>
#include <tuple>

namespace ssstudio {
namespace {

/// Looks a key up, or returns an empty view when it is absent. Keeps the
/// parsing below free of repeated find/end dances.
std::string_view lookup(const std::map<std::string, std::string>& extra, const char* key) {
    const auto it = extra.find(key);
    return it == extra.end() ? std::string_view{} : std::string_view(it->second);
}

/// The spellings written by sampler_state_to_extra, read back. Each returns
/// nothing for a value it does not know, which is what lets the caller keep the
/// default rather than having to invent one here.
std::optional<SamplerState::Filter> parse_filter(std::string_view text) {
    if (text == "nearest") return SamplerState::Filter::Nearest;
    if (text == "linear") return SamplerState::Filter::Linear;
    if (text == "mipmap") return SamplerState::Filter::Mipmap;
    return std::nullopt;
}

std::optional<SamplerState::Wrap> parse_wrap(std::string_view text) {
    if (text == "clamp") return SamplerState::Wrap::Clamp;
    if (text == "repeat") return SamplerState::Wrap::Repeat;
    return std::nullopt;
}

/// Booleans arrive as the strings "true" and "false" rather than as TOML
/// booleans, because `extra` is a map of strings and everything in it is text.
std::optional<bool> parse_bool(std::string_view text) {
    if (text == "true") return true;
    if (text == "false") return false;
    return std::nullopt;
}

}  // namespace

std::string_view to_string(SamplerState::Filter f) {
    switch (f) {
        case SamplerState::Filter::Nearest: return "nearest";
        case SamplerState::Filter::Linear: return "linear";
        case SamplerState::Filter::Mipmap: return "mipmap";
    }
    return "linear";
}

std::string_view to_string(SamplerState::Wrap w) {
    switch (w) {
        case SamplerState::Wrap::Clamp: return "clamp";
        case SamplerState::Wrap::Repeat: return "repeat";
    }
    return "clamp";
}

bool SamplerState::operator<(const SamplerState& other) const {
    return std::tie(filter, wrap, vflip, srgb) <
           std::tie(other.filter, other.wrap, other.vflip, other.srgb);
}

bool SamplerState::operator==(const SamplerState& other) const {
    return std::tie(filter, wrap, vflip, srgb) ==
           std::tie(other.filter, other.wrap, other.vflip, other.srgb);
}

SamplerState sampler_state_from_extra(const std::map<std::string, std::string>& extra) {
    SamplerState state;
    // Absent and unrecognised are treated the same way: the field keeps its
    // default and nothing is said about it. The map is hand-editable text that
    // may also have been written by a version knowing more spellings than this
    // one, and neither a typo nor a value from the future is worth refusing to
    // open a project over. The cost is that a misspelled setting looks like one
    // that was never set - visible in the panel, which shows what was understood.
    state.filter = parse_filter(lookup(extra, "filter")).value_or(state.filter);
    state.wrap = parse_wrap(lookup(extra, "wrap")).value_or(state.wrap);
    state.vflip = parse_bool(lookup(extra, "vflip")).value_or(state.vflip);
    state.srgb = parse_bool(lookup(extra, "srgb")).value_or(state.srgb);
    return state;
}

void sampler_state_to_extra(const SamplerState& state, std::map<std::string, std::string>& extra) {
    const SamplerState defaults;

    // Only what differs from the default is written, which is the rule the rest
    // of the manifest follows: a project file should say what someone chose, not
    // restate everything that was left alone. Erasing rather than writing the
    // default keeps a file from growing a line every time a row is touched.
    const auto put = [&extra](const char* key, bool differs, std::string value) {
        if (differs) {
            extra[key] = std::move(value);
        } else {
            extra.erase(key);
        }
    };

    put("filter", state.filter != defaults.filter, std::string(to_string(state.filter)));
    put("wrap", state.wrap != defaults.wrap, std::string(to_string(state.wrap)));
    put("vflip", state.vflip != defaults.vflip, state.vflip ? "true" : "false");
    put("srgb", state.srgb != defaults.srgb, state.srgb ? "true" : "false");
}



}  // namespace ssstudio
