// The pieces of the interface's look that ImGui has no widget for: the filled
// primary button, filter chips, a segmented control, status dots, bordered
// sections, small-capital labels, and the font sizes they are set in.
//
// Every colour comes from the active theme's roles, never from a literal, so a
// pack that changes its accent or its surfaces changes these with it - and the
// three built-in themes, which have roles too, get the same shapes in their own
// colours. Behaviour stays in the panels; nothing here decides what a click
// means, only how the thing being clicked looks.
#ifndef SSSTUDIO_GUI_WIDGETS_H
#define SSSTUDIO_GUI_WIDGETS_H

#include <string>
#include <string_view>

#include <imgui.h>

#include "ssstudio/theme_pack.h"
#include "ssstudio/types.h"

namespace ssstudio::gui {

/// The theme's roles as packed ImGui colours, looked up once.
///
/// A panel builds one of these at the top of its draw function rather than
/// asking the theme for a role by name at every use: the lookups are map
/// searches, and the names would otherwise be string literals scattered through
/// every panel, each one a typo waiting to fall back to black.
struct ThemeInk {
    explicit ThemeInk(const ResolvedTheme& theme);

    /// The gutter between panels, the panels themselves, and input fields.
    ImU32 base = 0;
    ImU32 raised = 0;
    ImU32 sunken = 0;

    /// Body text, secondary text, and text set on the accent.
    ImU32 text = 0;
    ImU32 muted = 0;
    ImU32 inverted = 0;
    /// Halfway between text and muted: values that are read but not edited,
    /// such as a type name or a size.
    ImU32 soft = 0;

    /// Hairlines: `subtle` between rows, `strong` around things that are
    /// meant to be noticed, such as an unticked checkbox.
    ImU32 subtle = 0;
    ImU32 strong = 0;

    /// The accent as a fill: the primary button, a progress bar. Not for text
    /// or thin marks - a theme may make it dark, the way the built-in dark
    /// theme does; accent_ink is the accent for those.
    ImU32 accent = 0;
    ImU32 accent_hover = 0;
    ImU32 accent_active = 0;
    /// The accent at low opacity: selected rows, "locked", active chips.
    ImU32 accent_muted = 0;
    /// The accent as ink: accent-coloured text, dots, outlines and bars,
    /// readable against the panels.
    ImU32 accent_ink = 0;

    /// A selection: the fill behind a selected entry, and the text on it. The
    /// selected page in Settings is drawn in these, so a theme sets how it
    /// looks with the select.bg and select.ink roles.
    ImU32 select_bg = 0;
    ImU32 select_ink = 0;

    /// Compile and diagnostic states. Taken from the theme's [diagnostics]
    /// table rather than the status roles, so they agree with the colours the
    /// editor and the Diagnostics panel have always used.
    ImU32 ok = 0;
    ImU32 warn = 0;
    ImU32 error = 0;
    ImU32 info = 0;

    ImU32 severity(Severity s) const;
};

/// A colour with its opacity replaced. `alpha` is 0 to 1, like the `alpha()`
/// transform in a theme pack.
ImU32 with_alpha(ImU32 color, float alpha);

/// Records the interface's base font size for this frame. Called once, at the
/// top of the frame, before anything pushes a font: PushFont() rewrites the
/// style's base size while it is in effect, so reading it later from inside the
/// editor (which pushes its own size) would scale every caption by the code
/// zoom as well.
void begin_type_frame();

/// A size from the design, which is drawn at a 13px base, scaled to the base
/// size the interface is actually using. A pack that asks for a 15px interface
/// gets every caption and label enlarged in proportion, rather than a larger
/// body with the same tiny metadata beside it.
///
/// Unscaled, as PushFont() wants it: ui_scale is applied by ImGui afterwards.
float type_size(float design_px);

/// The same, in screen pixels with ui_scale applied: what ImDrawList::AddText
/// wants when it is handed a font and a size explicitly.
float type_pixels(float design_px);

/// A length from the design in pixels, scaled the way ImGui scaled the style:
/// by the interface's font size relative to the design's 13px, with ui_scale
/// already in it. For paddings and gaps drawn by hand.
float design_px(float px);

/// Pushes a font and a size for the lifetime of the scope. `font` may be null
/// for "the current font"; `design_px` goes through type_size().
class FontScope {
public:
    FontScope(ImFont* font, float design_px);
    ~FontScope();
    FontScope(const FontScope&) = delete;
    FontScope& operator=(const FontScope&) = delete;
};

/// The code font at a design size: identifiers, paths, numbers.
class MonoScope : public FontScope {
public:
    explicit MonoScope(float design_px = 13.0f);
};

/// One run of text in a colour, as an item.
void colored_text(std::string_view text, ImU32 color);

/// One run of text in the code font, as an item.
void mono_text(std::string_view text, ImU32 color, float design_px = 12.0f);

/// A section label in small capitals and the muted ink: "PIPELINE",
/// "VALIDATION", "ROLES". Given the words in any case; they are shown upper.
void caps_label(std::string_view text, const ThemeInk& ink);

/// A filled dot sitting on the current text line, as an item: compile state
/// in a status bar, a project's health on its pill, a severity in a list.
void status_dot(ImU32 color, float design_px = 6.0f);

/// A short vertical rule between groups of toolbar controls, as an item the
/// height of a frame.
void toolbar_divider(const ThemeInk& ink);

/// The one filled button a view has: the accent, with inverted text. Used for
/// Build and nothing else, which is what keeps it meaning "the main thing".
bool primary_button(const char* label, const ThemeInk& ink, const ImVec2& size = ImVec2(0, 0));

/// A primary button that also shows its keyboard shortcut, dimmed, after the
/// label.
bool primary_button_with_hint(const char* label, const char* hint, const ThemeInk& ink);

/// A ghost button with a red outline and red text, for the destructive action
/// on a form ("Delete node"). Outlined rather than filled, so it is findable
/// without being the loudest thing on the screen.
bool danger_button(const char* label, const ThemeInk& ink);

/// A button with no fill until it is hovered: secondary actions that sit
/// beside a real button ("Copy all").
bool ghost_button(const char* label, const ThemeInk& ink);

/// A button with a dimmed hint after the label, such as a shortcut: "Recompile
/// all  F7". Otherwise an ordinary button.
bool button_with_hint(const char* label, const char* hint, const ThemeInk& ink);

/// A rounded filter chip: a coloured dot, a label and a count. Filled while
/// on, outlined while off; a click flips `on`. Returns true when it did.
bool toggle_chip(const char* label, int count, ImU32 dot, bool& on, const ThemeInk& ink);

/// Width toggle_chip() will take, for laying a row of them out.
float toggle_chip_width(const char* label, int count);

/// A row of mutually exclusive options in one sunken track - the shape a tab
/// bar has when the tabs are a view setting rather than documents. Returns
/// true when `current` changed.
bool segmented_control(const char* id, const char* const* labels, int count, int& current,
                       const ThemeInk& ink);

/// Width segmented_control() will take for these labels.
float segmented_control_width(const char* const* labels, int count);

/// A fully rounded label: a remark rather than a control ("Shader file is
/// generated from this graph").
void pill_label(std::string_view text, ImU32 text_color, ImU32 fill);

/// A small square-cornered tag in the code font ("cbuffer", "SV_Target0").
void tag_label(std::string_view text, const ThemeInk& ink);

/// Width tag_label() will take.
float tag_label_width(std::string_view text);

/// The header of a bordered section: what the section is, what it is made of,
/// and what kind of thing it is.
struct SectionHeader {
    std::string title;
    /// Shown after the title in the code font: "space3 · b0 · 16 B".
    std::string meta;
    /// Shown right-aligned as a tag: "cbuffer", "2 bound".
    std::string tag;
    bool default_open = true;
};

/// Opens a bordered, collapsible section: a header you click to fold, and a
/// body beneath it indented to the header's padding. Returns whether the body
/// should be drawn. section_end() must follow whatever this returns.
///
/// The open state is remembered per id, as a CollapsingHeader's is.
bool section_begin(const char* id, const SectionHeader& header, const ThemeInk& ink);

/// Closes what section_begin() opened.
void section_end();

/// Width a table inside a section should take, leaving the same margin on the
/// right as the section's indent leaves on the left.
float section_content_width();

/// The column headers of the current table in small capitals and the muted
/// ink, taken from the names given to TableSetupColumn(). Replaces
/// TableHeadersRow() for tables that are not sortable. A column whose bit is
/// set in `right_aligned` has its header against the column's right edge, over
/// numbers that are.
void caps_headers_row(const ThemeInk& ink, unsigned right_aligned = 0);

/// A rounded rectangle as the design's overlay chips draw it: nearly opaque
/// base colour and a hairline, over an image or a canvas.
void overlay_frame(ImDrawList* draw_list, const ImVec2& min, const ImVec2& max,
                   const ThemeInk& ink, ImU32 border = 0);

/// A string ending in "..." when it would not fit `max_width`, measured in the
/// current font.
std::string fit_text(std::string_view text, float max_width);

/// The same, cut from the front: "...projects/clean-bloom". For paths, whose
/// end is the part that says which one it is.
std::string fit_text_left(std::string_view text, float max_width);

/// A right arrow in the current font when it has one, and "->" when it does
/// not - the built-in font stops at Latin-1, and a missing glyph draws as a
/// box exactly where the eye is reading.
const char* arrow_glyph();

/// A count with its thousands grouped: "36,688".
std::string grouped(unsigned long long value);

/// "1.9 KB", "28.4 KB", "3.1 MB". Exact byte counts are for tooltips.
std::string human_bytes(unsigned long long bytes);

}  // namespace ssstudio::gui

#endif  // SSSTUDIO_GUI_WIDGETS_H
