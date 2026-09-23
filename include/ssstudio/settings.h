// App-scope settings, stored as TOML next to the platform config dir.
// Anything that can be presented or done in more than one way lives here.
#ifndef SSSTUDIO_SETTINGS_H
#define SSSTUDIO_SETTINGS_H

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "ssstudio/pack_format.h"
#include "ssstudio/syntax.h"
#include "ssstudio/types.h"

namespace ssstudio {

/// Editor syntax colors as 0xRRGGBB, indexed by TokenKind rather than named one
/// field at a time: a token kind added to the lexer then cannot compile without
/// a color, and the settings UI can walk the kinds instead of listing them.
struct SyntaxColors {
    std::array<std::uint32_t, kTokenKindCount> rgb{};

    std::uint32_t& operator[](TokenKind k) { return rgb[static_cast<std::size_t>(k)]; }
    std::uint32_t operator[](TokenKind k) const { return rgb[static_cast<std::size_t>(k)]; }
};

/// Walks up from `start` looking for a directory named `examples`, at most
/// `max_levels` levels, and returns the first hit or an empty path.
///
/// Separated from the caller that supplies `start` because that part is the
/// application's business - a development build sits in build/bin and a macOS
/// bundle buries it three levels deeper again - while the walk itself is
/// ordinary path arithmetic that a test can check. Stops at the first hit, so a
/// parent directory that happens to contain an examples/ cannot win over a
/// nearer one.
std::filesystem::path find_examples_dir(const std::filesystem::path& start, int max_levels = 8);

/// The palette a theme name stands for. An unknown name gives the dark one, so a
/// hand-edited settings file with a typo in it still opens.
SyntaxColors syntax_palette(std::string_view theme);

/// The names syntax_palette() knows, in the order Settings should offer them.
const std::vector<std::string>& syntax_palette_names();

/// The editor's text size: its default, and the range the zoom shortcuts and the
/// Settings slider both work within. One set of numbers so the two cannot
/// disagree about how far out the editor can be zoomed.
inline constexpr float kDefaultEditorFontSize = 15.0f;
inline constexpr float kMinEditorFontSize = 8.0f;
inline constexpr float kMaxEditorFontSize = 40.0f;

struct EditorSettings {
    std::string font_path;          // empty == bundled font
    float font_size = kDefaultEditorFontSize;
    int tab_width = 4;
    bool insert_spaces = true;
    bool word_wrap = false;
    bool show_line_numbers = true;
    bool show_whitespace = false;
    bool minimap = false;
    bool error_lens = true;         // inline diagnostics next to the line
    bool auto_indent = true;
    bool compile_on_type = true;
    int compile_debounce_ms = 300;
    /// While typing, ask the compiler for diagnostics only and skip code
    /// generation. Faster feedback on a large shader, at the cost of the preview
    /// and the reflection in the I/O panel updating on save rather than on every
    /// keystroke - which is why it is off by default rather than on.
    bool check_only_while_typing = false;
    bool compile_on_save = true;
    bool warnings_as_errors = false;
    /// "dark", "light", "classic", or the id of an installed theme pack. An id
    /// that is not installed opens as dark, so a settings file that travelled
    /// from a machine with more themes on it still opens.
    ///
    /// A fresh install starts on Tide Dark, the pack the application ships as
    /// its own look. It is a pack rather than a built-in so that it stays one
    /// more theme anybody can copy and change; a copy of the app that has lost
    /// its themes directory falls back to dark by the rule above.
    std::string color_theme = "tide-dark";

    /// Re-read the active theme pack when the window regains focus, if its file
    /// has changed. Off by default: it is a theme author's convenience, and
    /// everyone else would be paying a file check for a file they never edit.
    bool reload_theme_on_focus = false;
    bool syntax_highlight = true;
    /// Offer the completion list while typing. Ctrl+Space still opens it when
    /// this is off, so it reads as "stop interrupting" rather than "no
    /// completion".
    bool autocomplete = true;
    /// The palette a token kind falls back to when the active theme supplies no
    /// colour for it. A name from syntax_palette_names(); "custom" is no longer
    /// one of them, because customness is recorded per kind by syntax_pinned
    /// rather than by a flag over all ten.
    std::string syntax_theme = "default";

    /// What actually draws: the theme's colours, with every pinned kind
    /// replaced by the user's own.
    SyntaxColors syntax_colors = syntax_palette("default");

    /// Which kinds the user has edited themselves. A pinned kind survives a
    /// theme change; an unpinned one follows whatever theme is active.
    ///
    /// This is not stored as a list of its own: a kind is pinned exactly when
    /// [editor.syntax] holds a key for it, so presence in the file *is* the
    /// pin. Storing the deviation rather than the state is the same shape
    /// ProjectEditorState uses for closed tabs, and it buys the same property -
    /// something that arrives later, a new theme or a new TokenKind, behaves
    /// sensibly for everyone who never expressed an opinion about it.
    std::array<bool, kTokenKindCount> syntax_pinned{};
    int snapshot_history = 20;      // per-shader undo snapshots kept on disk
};

struct PreviewDefaults {
    std::string backend;            // "", "vulkan", "direct3d12", "metal"
    bool vsync = true;
    int target_fps = 0;             // 0 == uncapped/vsync
    bool separate_window = false;
    bool show_stats = true;
    bool pixel_inspector = true;
    std::string background = "checkerboard";  // checkerboard | color | transparent
};

struct FileSettings {
    /// Where new projects are put, and where the Open dialog starts when there
    /// is no better guess. Empty means the built-in default, which is the
    /// examples/ directory shipped alongside the application when one can be
    /// found next to it, and the user's Documents folder otherwise. Kept empty
    /// rather than resolved at load time so that a settings file stays portable
    /// between machines, and so the default can change without every existing
    /// settings file pinning the old one.
    std::filesystem::path projects_dir;

    // Project container
    std::string project_extension = ".ssstudio";
    std::string project_archive_extension = ".sdlshz";
    std::string manifest_filename = "project.toml";

    // Recognized source extensions per stage/language. Editable so a user can
    // keep their existing naming scheme (.vs.hlsl, .frag.glsl, ...).
    std::vector<std::string> vertex_extensions{".vert.hlsl", ".vs.hlsl", ".vert.glsl"};
    std::vector<std::string> fragment_extensions{".frag.hlsl", ".ps.hlsl", ".frag.glsl"};
    std::vector<std::string> compute_extensions{".comp.hlsl", ".cs.hlsl", ".comp.glsl"};
    bool ask_when_ambiguous = true;

    std::string autosave_suffix = ".autosave";
    int autosave_interval_s = 60;
    bool watch_external_changes = true;
};

struct ToolSettings {
    std::filesystem::path shadercross_dir;  // overrides bundled binaries
    std::filesystem::path dxc_path;
    std::filesystem::path glslang_path;     // empty == the built-in glslang
    std::filesystem::path xcrun_path;       // metallib on macOS
    /// Where curl lives, when it is not simply on the path. Downloading is done
    /// by running it rather than by linking an HTTP client, the same way the
    /// shader toolchain is reached.
    std::filesystem::path curl_path;
    int compile_threads = 2;
    bool shared_compile_cache = true;
    std::filesystem::path shared_cache_dir;  // empty == platform default
    /// Budget for the on-disk cache of compiled shader blobs. Named for assets
    /// for historical reasons; it has never held anything but compiles.
    std::uint64_t asset_cache_bytes = 512ull * 1024 * 1024;
    /// Budget for decoded textures held on the GPU. Counted in GPU bytes, which
    /// bear no relation to file sizes: a 200 KB photograph is 32 MB once it is
    /// four bytes per pixel.
    std::uint64_t texture_cache_bytes = 256ull * 1024 * 1024;
    /// Budget for files downloaded from a URL and kept on disk. Separate from
    /// the compile cache, which shares nothing with it but a parent directory.
    std::uint64_t download_cache_bytes = 512ull * 1024 * 1024;
};

struct LanguageSettings {
    bool hlsl_enabled = true;
    bool glsl_enabled = true;
    Language default_language = Language::HLSL;
    std::string hlsl_vertex_profile = "vs_6_0";
    std::string hlsl_fragment_profile = "ps_6_0";
    std::string hlsl_compute_profile = "cs_6_0";
    std::vector<std::string> include_dirs;
};

struct UiSettings {
    /// Nothing about the docking lives here. The window layout is kept in
    /// layout.ini next to this file, written whenever the user rearranges the
    /// panels and read back at startup; deleting that file brings the app's own
    /// default arrangement back.
    bool multi_viewport = true;
    /// Reopen the projects that were open when the app last closed. A project
    /// path on the command line takes precedence: an explicit argument is a
    /// request for that project, not for the last session as well.
    bool restore_session = true;
    bool show_register_hints = true;
    bool confirm_on_close_dirty = true;
    float ui_scale = 1.0f;
    std::map<std::string, std::string> shortcuts;  // action -> chord
};

/// How one project's editor was left: which shader tabs were where, and which
/// were closed.
///
/// Both lists name shaders that existed at the time. A shader appearing in the
/// project since - added by a colleague, or by `ssstudio import` - is in neither,
/// and both rules are written so that such a shader behaves sensibly: it opens
/// (because what is stored is which tabs are *closed*, not which are open) and
/// it takes its place after the remembered ones.
struct ProjectEditorState {
    /// The shader ids in the order their tabs were shown, left to right. Ids not
    /// listed follow them in project order.
    std::vector<std::string> tab_order;

    /// The shaders whose tabs were closed. Anything not named here opens.
    std::vector<std::string> closed_tabs;

    /// Nothing worth writing down: the editor was left in its default state.
    bool empty() const { return tab_order.empty() && closed_tabs.empty(); }
};

struct AppSettings {
    EditorSettings editor;
    PreviewDefaults preview;
    FileSettings files;
    ToolSettings tools;
    LanguageSettings languages;
    UiSettings ui;

    // Default container layout for new projects and for profiles that do not
    // override it (see PackLayout for what is customizable).
    PackLayout pack_layout;

    std::vector<std::filesystem::path> recent_projects;

    /// The projects that were open at the end of the last session, in tab order,
    /// and which of them was in front. Written whenever a project is opened,
    /// closed or brought forward, so an unclean exit still leaves a usable
    /// record. Not the same list as recent_projects: a project can be recent
    /// without having been left open.
    std::vector<std::filesystem::path> open_projects;
    int active_project = 0;

    /// What the editor remembers about each project between sessions, by project
    /// manifest path.
    ///
    /// This is editor state, not project state, so it lives here beside the
    /// docking layout rather than in project.toml - where arranging your own
    /// tabs would show up as a change on everybody else's checkout. Bounded by
    /// pruning to the projects that are still recent or open, so it cannot grow
    /// forever.
    std::map<std::string, ProjectEditorState> project_editor_state;

    static std::filesystem::path default_path();
    static AppSettings defaults() { return AppSettings{}; }
};

bool load_settings(const std::filesystem::path& path, AppSettings& out, Diagnostics& out_diags);
bool save_settings(const std::filesystem::path& path, const AppSettings& in, Diagnostics& out_diags);

// Guesses stage and language from a filename using FileSettings. Returns false
// when ambiguous, so the UI can ask instead of picking silently.
bool classify_source_file(const FileSettings& fs, const std::filesystem::path& p,
                          Stage& out_stage, Language& out_language);

}  // namespace ssstudio

#endif  // SSSTUDIO_SETTINGS_H
