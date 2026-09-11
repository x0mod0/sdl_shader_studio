#include "ssstudio/color.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdio>

namespace ssstudio {
namespace {

int to_byte(float value) {
    const float clamped = std::clamp(value, 0.0f, 1.0f);
    return static_cast<int>(std::lround(clamped * 255.0f));
}

/// The value of one hex digit, or -1. Both cases, because a colour copied from
/// somewhere else arrives in whichever the author of that used.
int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::string color_to_hex(const float* components, int count) {
    if (components == nullptr || count <= 0) return {};
    const int used = std::min(count, 4);
    char buffer[16];
    if (used >= 4) {
        std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X%02X", to_byte(components[0]),
                      to_byte(components[1]), to_byte(components[2]), to_byte(components[3]));
    } else {
        // Fewer than three components still produce a colour; the missing ones
        // read as zero, which is what the picker beside it shows.
        const float zero = 0.0f;
        const float* g = used > 1 ? &components[1] : &zero;
        const float* b = used > 2 ? &components[2] : &zero;
        std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X", to_byte(components[0]), to_byte(*g),
                      to_byte(*b));
    }
    return buffer;
}

bool color_from_hex(std::string_view text, float* components, int count) {
    if (components == nullptr || count <= 0) return false;

    // Leading and trailing space, and the '#', are all optional: a colour
    // pasted from anywhere arrives with some combination of them.
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    if (!text.empty() && text.front() == '#') text.remove_prefix(1);

    if (text.size() != 3 && text.size() != 6 && text.size() != 8) return false;

    std::array<int, 8> digits{};
    for (std::size_t i = 0; i < text.size(); ++i) {
        const int digit = hex_digit(text[i]);
        if (digit < 0) return false;
        digits[i] = digit;
    }

    // Parsed in full before anything is written, so a string that turns out to
    // be malformed half way through leaves the colour untouched.
    std::array<float, 4> parsed{0.0f, 0.0f, 0.0f, 1.0f};
    if (text.size() == 3) {
        // "#abc" is "#aabbcc": each digit stands for both halves of its byte.
        for (int i = 0; i < 3; ++i) {
            parsed[static_cast<std::size_t>(i)] =
                static_cast<float>(digits[static_cast<std::size_t>(i)] * 17) / 255.0f;
        }
    } else {
        for (int i = 0; i < static_cast<int>(text.size()) / 2; ++i) {
            const int value = digits[static_cast<std::size_t>(i * 2)] * 16 +
                              digits[static_cast<std::size_t>(i * 2 + 1)];
            parsed[static_cast<std::size_t>(i)] = static_cast<float>(value) / 255.0f;
        }
    }

    const int written = std::min(count, 4);
    for (int i = 0; i < written && i < 3; ++i) {
        components[i] = parsed[static_cast<std::size_t>(i)];
    }
    // Six digits into a four-component colour says nothing about the alpha, so
    // the alpha it already had is what it keeps.
    if (written >= 4 && text.size() == 8) components[3] = parsed[3];
    return true;
}

bool names_a_color(std::string_view name) {
    // The words a shader author actually uses. Kept short on purpose: every
    // addition is another chance to show a picker for something that is not a
    // colour, and the ones missing from it cost only a number field.
    static constexpr std::array<std::string_view, 10> kWords = {
        "color", "colour", "tint", "albedo", "rgb",
        "emissive", "diffuse", "specular", "ambient", "background"};

    std::string lowered;
    lowered.reserve(name.size());
    for (char c : name) {
        lowered += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    for (std::string_view word : kWords) {
        if (lowered.find(word) != std::string::npos) return true;
    }
    return false;
}


// ---------------------------------------------------------------------------
// Colours as numbers
// ---------------------------------------------------------------------------
namespace {

struct Oklab {
    float L, a, b;
};

float srgb_to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float linear_to_srgb(float c) {
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

Oklab to_oklab(float r, float g, float b) {
    const float lr = srgb_to_linear(r), lg = srgb_to_linear(g), lb = srgb_to_linear(b);
    const float l = 0.4122214708f * lr + 0.5363325363f * lg + 0.0514459929f * lb;
    const float m = 0.2119034982f * lr + 0.6806995451f * lg + 0.1073969566f * lb;
    const float s = 0.0883024619f * lr + 0.2817188376f * lg + 0.6299787005f * lb;
    const float l_ = std::cbrt(l), m_ = std::cbrt(m), s_ = std::cbrt(s);
    return {0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_,
            1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_,
            0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_};
}

/// The inverse chain, clamped to the sRGB cube: a transform that leaves the
/// gamut comes back as the nearest paintable colour rather than as an error, so
/// no argument a pack can write makes a transform fail.
void from_oklab(const Oklab& c, float& r, float& g, float& b) {
    const float l_ = c.L + 0.3963377774f * c.a + 0.2158037573f * c.b;
    const float m_ = c.L - 0.1055613458f * c.a - 0.0638541728f * c.b;
    const float s_ = c.L - 0.0894841775f * c.a - 1.2914855480f * c.b;
    const float l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;
    const float lr = 4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
    const float lg = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
    const float lb = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;
    r = std::clamp(linear_to_srgb(lr), 0.0f, 1.0f);
    g = std::clamp(linear_to_srgb(lg), 0.0f, 1.0f);
    b = std::clamp(linear_to_srgb(lb), 0.0f, 1.0f);
}

void unpack(Rgba color, float* out) {
    out[0] = static_cast<float>((color >> 24) & 0xFFu) / 255.0f;
    out[1] = static_cast<float>((color >> 16) & 0xFFu) / 255.0f;
    out[2] = static_cast<float>((color >> 8) & 0xFFu) / 255.0f;
    out[3] = static_cast<float>(color & 0xFFu) / 255.0f;
}

Rgba pack(const float* c) {
    return (static_cast<Rgba>(to_byte(c[0])) << 24) | (static_cast<Rgba>(to_byte(c[1])) << 16) |
           (static_cast<Rgba>(to_byte(c[2])) << 8) | static_cast<Rgba>(to_byte(c[3]));
}

/// Relative luminance per WCAG, on an already-opaque colour.
float luminance(const float* c) {
    return 0.2126f * srgb_to_linear(c[0]) + 0.7152f * srgb_to_linear(c[1]) +
           0.0722f * srgb_to_linear(c[2]);
}

Rgba shift_lightness(Rgba color, float amount) {
    float c[4];
    unpack(color, c);
    Oklab lab = to_oklab(c[0], c[1], c[2]);
    lab.L = std::clamp(lab.L + amount, 0.0f, 1.0f);
    from_oklab(lab, c[0], c[1], c[2]);
    return pack(c);
}

}  // namespace

std::string color_rgba_to_hex(Rgba color) {
    float c[4];
    unpack(color, c);
    return color_to_hex(c, (color & 0xFFu) == 0xFFu ? 3 : 4);
}

bool color_rgba_from_hex(std::string_view text, Rgba& out) {
    float c[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    if (!color_from_hex(text, c, 4)) return false;
    out = pack(c);
    return true;
}

Rgba color_lighten(Rgba color, float amount) { return shift_lightness(color, amount); }

Rgba color_darken(Rgba color, float amount) { return shift_lightness(color, -amount); }

Rgba color_mix(Rgba a, Rgba b, float t) {
    const float k = std::clamp(t, 0.0f, 1.0f);
    float ca[4], cb[4];
    unpack(a, ca);
    unpack(b, cb);
    const Oklab la = to_oklab(ca[0], ca[1], ca[2]);
    const Oklab lb = to_oklab(cb[0], cb[1], cb[2]);
    const Oklab mixed{la.L + (lb.L - la.L) * k, la.a + (lb.a - la.a) * k,
                      la.b + (lb.b - la.b) * k};
    float out[4] = {0.0f, 0.0f, 0.0f, ca[3] + (cb[3] - ca[3]) * k};
    from_oklab(mixed, out[0], out[1], out[2]);
    return pack(out);
}

Rgba color_alpha(Rgba color, float alpha) {
    return (color & 0xFFFFFF00u) | static_cast<Rgba>(to_byte(alpha));
}

Rgba color_over(Rgba fg, Rgba bg) {
    float f[4], b[4];
    unpack(fg, f);
    unpack(bg, b);
    const float a = f[3];
    float out[4] = {f[0] * a + b[0] * (1.0f - a), f[1] * a + b[1] * (1.0f - a),
                    f[2] * a + b[2] * (1.0f - a), b[3]};
    return pack(out);
}

float color_contrast(Rgba fg, Rgba bg) {
    float f[4], b[4];
    unpack(color_over(fg, color_alpha(bg, 1.0f)), f);
    unpack(color_alpha(bg, 1.0f), b);
    const float lf = luminance(f), lb = luminance(b);
    const float hi = std::max(lf, lb), lo = std::min(lf, lb);
    return (hi + 0.05f) / (lo + 0.05f);
}

}  // namespace ssstudio
