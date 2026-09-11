// Colours as text.
//
// A colour is picked with a pointer and written down as hex, and the two have to
// agree. The conversion lives here rather than in the widget that uses it so it
// can be tested by writing down a string and reading back the numbers - and so
// that anything else needing a colour on the clipboard or in a file gets the
// same spelling.
#ifndef SSSTUDIO_COLOR_H
#define SSSTUDIO_COLOR_H

#include <cstdint>
#include <string>
#include <string_view>

namespace ssstudio {

/// A colour as "#RRGGBB", or "#RRGGBBAA" when `count` is 4. Components are 0..1
/// and are clamped, so a value out of range is written as the nearest colour
/// rather than as nonsense.
std::string color_to_hex(const float* components, int count);

/// Reads a colour into `components`, and says whether it could.
///
/// Accepts "#RGB", "#RRGGBB" and "#RRGGBBAA", with or without the '#' and in
/// either case. Nothing is written unless the whole string parses, so a field
/// half way through being typed leaves the colour it names alone.
///
/// A six-digit value written into a four-component colour leaves the alpha as it
/// was: dropping to opaque would be a change nobody asked for, and the two
/// spellings are how you say "this colour" and "this colour, this transparent".
bool color_from_hex(std::string_view text, float* components, int count);

/// Whether a value called `name` is meant to be a colour.
///
/// Three or four components is a colour or it is a direction, a size, a pair of
/// coordinates - and the count says nothing about which. The name usually does,
/// so that is what this asks. Judged on a substring so that "base_color",
/// "tintColour" and "emissive_rgb" all read the same way.
///
/// Wrong occasionally by construction: a value called "colorspace_scale" is not
/// a colour. The cost is a picker where a number field belonged, which is a
/// worse control rather than a wrong value.
bool names_a_color(std::string_view name);

// ---------------------------------------------------------------------------
// Colours as numbers
//
// Theme packs need to compute with colours, not only spell them: a hover state
// is the accent one step lighter, a docking preview is the accent at a quarter
// alpha. These work on a colour packed as 0xRRGGBBAA - one value to copy
// around, and the same byte order the hex spelling reads in.
// ---------------------------------------------------------------------------

/// A colour packed as 0xRRGGBBAA.
using Rgba = std::uint32_t;

/// "#rrggbb", or "#rrggbbaa" when the colour is not opaque. Round-trips through
/// color_rgba_from_hex().
std::string color_rgba_to_hex(Rgba color);

/// Reads "#rgb", "#rrggbb" or "#rrggbbaa", with or without the '#'. A
/// six-digit value is opaque. Nothing is written unless the whole string
/// parses.
bool color_rgba_from_hex(std::string_view text, Rgba& out);

/// `amount` added to (lighten) or subtracted from (darken) the colour's Oklab
/// L, where 1.0 is the whole lightness axis. Alpha is untouched.
///
/// Oklab rather than sRGB because scaling sRGB channels desaturates as it
/// brightens: a saturated accent lightened in sRGB comes back grey, which is
/// the one thing a hover state must not do. L is perceptual, so an 8% step is
/// the same visible step whether the colour is dark blue or pale yellow - which
/// is what lets one derivation serve every theme instead of only the theme it
/// was tuned against.
Rgba color_lighten(Rgba color, float amount);
Rgba color_darken(Rgba color, float amount);

/// Interpolates in Oklab; `t` of 0 is `a` and 1 is `b`. Alpha interpolates
/// linearly, because alpha is not a perceptual quantity.
Rgba color_mix(Rgba a, Rgba b, float t);

/// The colour with its alpha replaced. `alpha` is 0..1 and is clamped.
Rgba color_alpha(Rgba color, float alpha);

/// `fg` composited over `bg`, source-over. The result is as opaque as `bg`.
///
/// Contrast is a property of what reaches the eye, so a translucent colour has
/// to be flattened before it can be measured - a ratio computed on unblended
/// alpha describes a colour nobody sees.
Rgba color_over(Rgba fg, Rgba bg);

/// The WCAG relative-luminance contrast ratio between two colours, 1.0 to 21.0.
/// `fg` is composited over `bg` first.
float color_contrast(Rgba fg, Rgba bg);

}  // namespace ssstudio

#endif  // SSSTUDIO_COLOR_H
