// Loadable theme packs: parse, resolve, discover, lint.
//
// A pack is TOML on disk and a struct of numbers in memory. Nothing here knows
// what ImGui is - resolving a pack produces colours keyed by name, and only
// src/gui turns those into an ImGuiStyle. That is what lets a theme be tested
// end to end on a machine with no display.
//
// See docs/THEME_PACK_FORMAT.md for the format itself; the tables in this
// module are the same tables that document describes.
#ifndef SSSTUDIO_THEME_PACK_H
#define SSSTUDIO_THEME_PACK_H

#include <array>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "ssstudio/color.h"
#include "ssstudio/syntax.h"
#include "ssstudio/types.h"

namespace ssstudio {

/// The schema version this build reads. A pack declaring anything else is
/// refused rather than half-read, for the reason the shader pack header carries
/// a signature: misreading a file is worse than not reading it.
inline constexpr int kThemePackFormat = 1;

/// The file extension, and the stem suffix that makes a directory a pack.
inline constexpr std::string_view kThemePackExtension = ".s3theme";

/// The three built-in themes. A pack may not claim these ids; it inherits from
/// them instead.
bool theme_is_builtin(std::string_view id);

/// The twenty semantic roles, in the order the settings panel and
/// `ssstudio theme` should list them.
const std::vector<std::string>& theme_role_names();

/// The widget colours a pack may set, spelled as ImGui spells them with the
/// `ImGuiCol_` prefix dropped. This is also the set the derivation table covers,
/// so a name absent from here has no derivation and cannot be resolved.
const std::vector<std::string>& theme_ui_color_names();

/// The name a retired ImGui colour has now, or an empty view when `name` is not
/// an alias. Packs written against an older build keep working.
std::string_view theme_ui_color_alias(std::string_view name);

/// Metrics, kept as three maps rather than a struct because the set is a
/// whitelist that the GUI walks - a key nobody sets should not become a field
/// that has to be given a value.
struct ThemeStyle {
    std::map<std::string, float> scalar;
    std::map<std::string, std::array<float, 2>> vec2;
    std::map<std::string, std::string> direction;  // none | left | right | up | down
};

/// One pack as it was written: expressions, not colours. Kept separate from the
/// resolved form so that `inherit` can resolve a chain of packs without any of
/// them having been evaluated yet.
struct ThemePack {
    std::string id;                 // the filename stem, lowercased
    std::filesystem::path file;     // the .s3theme file, or the theme.toml inside a directory
    std::filesystem::path dir;      // the pack directory, or empty for a single-file pack

    int format = kThemePackFormat;
    std::string name;
    std::string author;
    std::string version;
    std::string description;
    std::string homepage;
    std::string appearance;         // "dark" | "light" | "" == infer
    std::string inherit = "dark";

    std::map<std::string, std::string> palette;      // name -> hex only
    std::map<std::string, std::string> roles;        // role -> expression
    std::map<std::string, std::string> ui;           // ImGui colour name -> expression
    std::map<std::string, std::string> syntax;       // token kind -> expression
    std::map<std::string, std::string> diagnostics;  // error|warning|info|success
    std::map<std::string, std::string> graph;        // canvas, grid, pin.float, node.math, ...
    std::map<std::string, std::string> preview;      // checker_a, checker_b, border, ...
    ThemeStyle style;

    float checker_size = 0.0f;      // 0 == unset
    std::string font_ui;            // relative to dir
    std::string font_editor;
    float font_ui_size = 0.0f;      // 0 == unset
    float font_editor_size = 0.0f;
};

/// A pack with every value evaluated to a colour or a number. This is what the
/// GUI paints from and what `ssstudio theme` prints.
struct ResolvedTheme {
    std::string id;
    std::string name;
    std::string appearance = "dark";
    /// True for `dark`, `light` and `classic`. Their widget colours are still
    /// applied the way they always were - ImGui's own style plus the app's
    /// overrides - so `ui` is empty for them and the GUI knows not to look.
    /// Everything else in here (syntax, diagnostics, graph, preview) is filled
    /// either way, which is what gives the app one code path for its own
    /// colours without changing how the built-in themes look.
    bool builtin = false;

    std::map<std::string, Rgba> roles;
    std::map<std::string, Rgba> ui;
    std::array<Rgba, kTokenKindCount> syntax{};
    std::map<std::string, Rgba> diagnostics;
    std::map<std::string, Rgba> graph;
    std::map<std::string, Rgba> preview;
    ThemeStyle style;
    float checker_size = 8.0f;

    std::filesystem::path font_ui;
    std::filesystem::path font_editor;
    float font_ui_size = 0.0f;
    float font_editor_size = 0.0f;

    /// Lookups that answer for a name nobody set, so a caller painting one
    /// widget does not have to check first. `fallback` is what comes back.
    Rgba role(std::string_view name, Rgba fallback = 0x000000FFu) const;
    Rgba ui_color(std::string_view name, Rgba fallback = 0x000000FFu) const;
    Rgba graph_color(std::string_view name, Rgba fallback = 0x000000FFu) const;
    Rgba preview_color(std::string_view name, Rgba fallback = 0x000000FFu) const;
    Rgba diagnostic(Severity severity) const;
    /// The "compiled cleanly" ink, which is not a Severity.
    Rgba success() const;
};

/// The roles a built-in theme stands for. Empty for an unknown id.
///
/// The built-in themes themselves are still applied the way they always were -
/// ImGui's own styles plus the app's overrides - so their appearance does not
/// depend on this table. It exists for `inherit`: a pack that inherits `dark`
/// and names no roles of its own gets these.
std::map<std::string, Rgba> builtin_theme_roles(std::string_view id);

/// Evaluates one value from the pack grammar: hex, `$ref`, or a transform.
/// `error` is set and false returned when it cannot be evaluated.
bool eval_theme_color(std::string_view expression, const std::map<std::string, Rgba>& roles,
                      const std::map<std::string, Rgba>& palette, Rgba& out, std::string& error);

/// Reads a pack. False means refused - unknown format, unparseable TOML, an id
/// collision with a built-in, or a font path that escapes the pack directory.
/// Warnings for unknown or ill-typed keys are appended either way.
bool parse_theme_pack(const std::filesystem::path& path, ThemePack& out, Diagnostics& diags);

/// The same, for text that is not on disk. `id` is what the filename would have
/// supplied. Used by the tests, and by anything holding a pack in memory.
bool parse_theme_pack_text(std::string_view text, std::string id, ThemePack& out,
                           Diagnostics& diags);

/// What resolving needs beyond the pack itself.
struct ThemeResolveContext {
    /// Packs by id, so `inherit` can name another pack. The pack being resolved
    /// need not be in here.
    std::map<std::string, ThemePack> available;

    /// `editor.syntax_theme`: the palette a kind falls back to when the pack
    /// does not name it. Empty means use the palette the appearance implies.
    std::string fallback_syntax_palette;
};

/// Resolves the cascade. False means the pack was refused and `out` holds the
/// base theme instead, which is why a cosmetic mistake cannot stop the app.
bool resolve_theme(const ThemePack& pack, const ThemeResolveContext& context, ResolvedTheme& out,
                   Diagnostics& diags);

/// A resolved built-in theme, for when no pack is active.
ResolvedTheme builtin_resolved_theme(std::string_view id,
                                     const std::string& fallback_syntax_palette = {});

/// Every pack under `roots`, in discovery order. Later roots win an id
/// collision, so the caller passes bundled, then user, then project.
std::vector<std::filesystem::path> theme_pack_paths(const std::vector<std::filesystem::path>& roots);

/// The pack id a path stands for: the stem, lowercased, with the `.s3theme`
/// suffix removed. Empty when the path is not a pack.
std::string theme_pack_id(const std::filesystem::path& path);

/// Remembers the file a theme was read from and whether it has changed since.
///
/// The rule that matters is in changed(): it answers true at most once per
/// modification time, and records the attempt whether or not the caller then
/// manages to read the file. A text editor saving in two steps leaves a
/// truncated file behind for a moment, and without that rule a pack caught
/// mid-save would be retried - and complained about - on every check until it
/// was next touched. Recording the attempt means the completed save, which
/// carries a newer time, is what gets retried.
struct ThemeWatch {
    std::filesystem::path file;
    std::filesystem::file_time_type stamp{};
    bool watching = false;

    /// Starts watching `path` from its current state. An empty path, or one
    /// that cannot be read, stops the watch instead - which is what a built-in
    /// theme wants, having no file behind it.
    void reset(const std::filesystem::path& path);

    /// Whether the file has been modified since the last call (or since
    /// reset()). Records the new time before answering, so two calls in a row
    /// cannot both say yes.
    bool changed();
};

/// Contrast advice. Never fatal - these are findings about a theme, not
/// problems with the file - so they come back as Info and Warning only.
Diagnostics lint_theme(const ResolvedTheme& theme);

}  // namespace ssstudio

#endif  // SSSTUDIO_THEME_PACK_H
