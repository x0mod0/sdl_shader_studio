// The two typefaces the interface draws with: one for words, one for code.
//
// A theme pack may name a font file for each ([font] ui and editor), and the
// user's own editor font setting outranks the pack's. Everything that cannot be
// found or read falls back to ImGui's built-in font, so a missing file costs the
// look and never the session.
#ifndef SSSTUDIO_GUI_FONTS_H
#define SSSTUDIO_GUI_FONTS_H

#include <filesystem>
#include <map>
#include <string>
#include <vector>

struct ImFont;

namespace ssstudio::gui {

/// Which font files the interface should be using. An empty path means the
/// built-in font for that role.
struct FontChoice {
    /// Labels, buttons, menus - everything that is prose.
    std::filesystem::path ui;
    /// Code, identifiers, paths and numbers: the editor, and every place the
    /// interface shows something the user would type or copy.
    std::filesystem::path mono;
    /// The interface text size in pixels, or zero for the font's own. Matters
    /// only to the built-in font: ImGui has it as a pixel font drawn for 13px,
    /// which blurs at any other size, and as a scalable version of the same
    /// design that does not - so a size other than 13 gets the scalable one.
    float size = 0.0f;

    bool operator==(const FontChoice&) const = default;
};

/// Owns the loaded fonts and swaps them between frames.
///
/// A choice is recorded by request() and only acted on by apply(). The split
/// exists because a theme is usually changed from inside a frame - the Settings
/// panel is drawing when the user picks one - and the font atlas is best left
/// alone while a frame is being built. The main loop calls apply() before
/// NewFrame(), where changing io.FontDefault takes effect cleanly.
///
/// Files are loaded once and kept: switching between two themes and back costs
/// nothing the second time, and a handful of font sources is not memory worth
/// reclaiming.
class FontLibrary {
public:
    /// Records the fonts wanted from now on. Cheap, and safe to call mid-frame.
    void request(const FontChoice& choice);

    /// Whether a request is waiting for apply().
    bool pending() const { return pending_; }

    /// Loads and installs whatever the last request() asked for. Returns one
    /// message per file that could not be used, for the caller to log - the
    /// library has no opinion about where messages go.
    ///
    /// Must be called outside a frame: before the first NewFrame(), or between
    /// Render() and the next NewFrame().
    std::vector<std::string> apply();

    /// The interface font now installed as io.FontDefault.
    ImFont* ui() const { return ui_; }

    /// The code font, or the built-in one when no file was given.
    ImFont* mono() const { return mono_; }

private:
    /// A file as a font, or null. Remembers failures as well as successes, so a
    /// missing file is reported once rather than on every theme change.
    ImFont* load(const std::filesystem::path& path, std::vector<std::string>& problems);

    /// ImGui's own font for a text size, added on first use. The pixel font
    /// is always added first, whichever is asked for, so it is the atlas's
    /// first entry - the one ImGui itself falls back to.
    ImFont* builtin(float size);

    FontChoice wanted_;
    bool pending_ = false;

    /// Every file tried so far, keyed by its path as given. Null for a file
    /// that could not be read.
    std::map<std::filesystem::path, ImFont*> loaded_;
    /// The pixel font drawn for 13px, and the scalable one for other sizes.
    ImFont* builtin_ = nullptr;
    ImFont* builtin_scalable_ = nullptr;
    ImFont* ui_ = nullptr;
    ImFont* mono_ = nullptr;
};

/// The font code, identifiers, paths and numbers are drawn in.
///
/// Null until the first FontLibrary::apply(), and PushFont() reads null as
/// "keep the current font" - so a caller never has to test it before use.
ImFont* mono_font();

}  // namespace ssstudio::gui

#endif  // SSSTUDIO_GUI_FONTS_H
