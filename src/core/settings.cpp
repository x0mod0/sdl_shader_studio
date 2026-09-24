#include "ssstudio/settings.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <toml++/toml.hpp>

namespace ssstudio {
namespace {

Diagnostic error(std::string msg, std::string file = {}) {
    Diagnostic d;
    d.severity = Severity::Error;
    d.code = "SSSTUDIO-SETTINGS";
    d.message = std::move(msg);
    d.file = std::move(file);
    return d;
}

std::string escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    return out;
}

void write_string_array(std::ostream& os, const char* key, const std::vector<std::string>& v) {
    os << key << " = [";
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) os << ", ";
        os << "\"" << escape(v[i]) << "\"";
    }
    os << "]\n";
}

std::vector<std::string> read_string_array(const toml::node_view<const toml::node>& n,
                                           std::vector<std::string> fallback) {
    const auto* arr = n.as_array();
    if (!arr) return fallback;
    std::vector<std::string> out;
    for (const auto& e : *arr) out.push_back(e.value_or(std::string()));
    return out;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Colors travel as "#rrggbb": a settings file is meant to be readable, and a
// decimal 13748182 is not. A malformed value leaves the palette's color alone
// rather than painting something black.
bool parse_hex_color(std::string_view text, std::uint32_t& out) {
    if (!text.empty() && text.front() == '#') text.remove_prefix(1);
    if (text.size() != 6) return false;
    std::uint32_t value = 0;
    for (char c : text) {
        value <<= 4;
        if (c >= '0' && c <= '9') value |= static_cast<std::uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') value |= static_cast<std::uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') value |= static_cast<std::uint32_t>(c - 'A' + 10);
        else return false;
    }
    out = value;
    return true;
}

std::string format_hex_color(std::uint32_t rgb) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out = "#______";
    for (int i = 0; i < 6; ++i) {
        out[static_cast<std::size_t>(6 - i)] = kDigits[(rgb >> (i * 4)) & 0xF];
    }
    return out;
}

// Indexed the same way SyntaxColors is, so the two cannot drift apart:
// plain, comment, preprocessor, keyword, type, intrinsic, number, string,
// operator, identifier.
struct Palette {
    const char* name;
    std::array<std::uint32_t, kTokenKindCount> rgb;
};

constexpr std::array<Palette, 3> kPalettes = {{
    // Tuned against the dark theme's 0x171719 editor background.
    {"default", {{0xd8dee9, 0x6b7a8f, 0xe0af68, 0xc792ea, 0x82aaff, 0x7fdbca, 0xf78c6c,
                  0xc3e88d, 0x99a7bd, 0xd8dee9}}},
    // For the light and classic ImGui themes, where the editor ground is pale.
    {"light", {{0x24292f, 0x6e7781, 0x8250df, 0xcf222e, 0x0550ae, 0x1a7f37, 0x953800,
                0x0a3069, 0x57606a, 0x24292f}}},
    // Structure only: comments recede, everything else is one ink. For anyone
    // who finds a full palette noisier than it is useful.
    {"mono", {{0xcfd3da, 0x6b7280, 0xa1a8b3, 0xffffff, 0xffffff, 0xcfd3da, 0xcfd3da,
               0xcfd3da, 0x9aa1ad, 0xcfd3da}}},
}};

/// Undoes the "all ten, every time" spelling an earlier build wrote.
///
/// Before theme packs, [editor.syntax] always held all ten kinds, so reading
/// presence as a pin would pin all ten the first time anyone opened the app -
/// freezing every existing user out of every theme's syntax colours, which is
/// precisely what per-key pinning exists to avoid. A file with all ten present
/// is therefore compared against the palette it names, and only the kinds that
/// differ stay pinned.
///
/// "custom" said the name no longer described the colours but not what they had
/// started from, so it is resolved to whichever palette matches most kinds -
/// which is what "custom" meant in practice: a stock palette with two or three
/// colours changed.
void migrate_syntax_pins(EditorSettings& e) {
    const std::vector<std::string>& names = syntax_palette_names();
    std::string base = e.syntax_theme;
    if (std::find(names.begin(), names.end(), base) == names.end()) {
        std::size_t best = 0;
        base = names.empty() ? std::string("default") : names.front();
        for (const auto& name : names) {
            const SyntaxColors candidate = syntax_palette(name);
            std::size_t matches = 0;
            for (std::size_t i = 0; i < kTokenKindCount; ++i) {
                if (candidate.rgb[i] == e.syntax_colors.rgb[i]) ++matches;
            }
            // Strictly greater, so a tie keeps the earlier palette - and
            // syntax_palette_names() lists "default" first.
            if (matches > best) {
                best = matches;
                base = name;
            }
        }
        e.syntax_theme = base;
    }

    const SyntaxColors palette = syntax_palette(base);
    for (std::size_t i = 0; i < kTokenKindCount; ++i) {
        if (e.syntax_colors.rgb[i] == palette.rgb[i]) e.syntax_pinned[i] = false;
    }
}

}  // namespace

std::filesystem::path find_examples_dir(const std::filesystem::path& start, int max_levels) {
    std::error_code ec;
    std::filesystem::path dir = start;
    for (int level = 0; level < max_levels && !dir.empty(); ++level) {
        const std::filesystem::path candidate = dir / "examples";
        if (std::filesystem::is_directory(candidate, ec)) return candidate;
        const std::filesystem::path parent = dir.parent_path();
        if (parent == dir) break;  // reached the filesystem root
        dir = parent;
    }
    return {};
}

SyntaxColors syntax_palette(std::string_view theme) {
    SyntaxColors out;
    out.rgb = kPalettes[0].rgb;
    for (const auto& palette : kPalettes) {
        if (theme == palette.name) out.rgb = palette.rgb;
    }
    return out;
}

const std::vector<std::string>& syntax_palette_names() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> v;
        for (const auto& palette : kPalettes) v.emplace_back(palette.name);
        return v;
    }();
    return names;
}

std::filesystem::path AppSettings::default_path() {
#if defined(_WIN32)
    if (const char* appdata = std::getenv("APPDATA")) {
        return std::filesystem::path(appdata) / "sdl-shader-studio" / "settings.toml";
    }
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home) / "Library" / "Application Support" /
               "sdl-shader-studio" / "settings.toml";
    }
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
        return std::filesystem::path(xdg) / "sdl-shader-studio" / "settings.toml";
    }
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home) / ".config" / "sdl-shader-studio" / "settings.toml";
    }
#endif
    return std::filesystem::path("ssstudio-settings.toml");
}

bool load_settings(const std::filesystem::path& path, AppSettings& out, Diagnostics& out_diags) {
    out = AppSettings::defaults();
    if (!std::filesystem::exists(path)) return true;  // defaults are a valid state

    toml::table tbl;
    try {
        tbl = toml::parse_file(path.string());
    } catch (const toml::parse_error& e) {
        Diagnostic d = error(std::string(e.description()), path.string());
        d.line = static_cast<int>(e.source().begin.line);
        d.column = static_cast<int>(e.source().begin.column);
        out_diags.push_back(std::move(d));
        return false;
    }

    if (const auto* t = tbl["editor"].as_table()) {
        auto& e = out.editor;
        e.font_path = (*t)["font_path"].value_or(e.font_path);
        e.font_size = static_cast<float>((*t)["font_size"].value_or(e.font_size));
        e.tab_width = static_cast<int>((*t)["tab_width"].value_or(e.tab_width));
        e.insert_spaces = (*t)["insert_spaces"].value_or(e.insert_spaces);
        e.word_wrap = (*t)["word_wrap"].value_or(e.word_wrap);
        e.show_line_numbers = (*t)["show_line_numbers"].value_or(e.show_line_numbers);
        e.show_whitespace = (*t)["show_whitespace"].value_or(e.show_whitespace);
        e.minimap = (*t)["minimap"].value_or(e.minimap);
        e.error_lens = (*t)["error_lens"].value_or(e.error_lens);
        e.auto_indent = (*t)["auto_indent"].value_or(e.auto_indent);
        e.compile_on_type = (*t)["compile_on_type"].value_or(e.compile_on_type);
        e.compile_debounce_ms = static_cast<int>((*t)["compile_debounce_ms"].value_or(e.compile_debounce_ms));
        e.check_only_while_typing =
            (*t)["check_only_while_typing"].value_or(e.check_only_while_typing);
        e.compile_on_save = (*t)["compile_on_save"].value_or(e.compile_on_save);
        e.warnings_as_errors = (*t)["warnings_as_errors"].value_or(e.warnings_as_errors);
        e.color_theme = (*t)["color_theme"].value_or(e.color_theme);
        e.reload_theme_on_focus =
            (*t)["reload_theme_on_focus"].value_or(e.reload_theme_on_focus);
        e.syntax_highlight = (*t)["syntax_highlight"].value_or(e.syntax_highlight);
        e.autocomplete = (*t)["autocomplete"].value_or(e.autocomplete);
        e.syntax_theme = (*t)["syntax_theme"].value_or(e.syntax_theme);
        e.snapshot_history = static_cast<int>((*t)["snapshot_history"].value_or(e.snapshot_history));

        // The named palette is the starting point and [editor.syntax] overrides
        // it per kind, which is what lets "default plus a different comment
        // color" be expressible without spelling out all ten. A key that is
        // there is a kind the user pinned; a kind with no key follows the
        // active theme.
        e.syntax_colors = syntax_palette(e.syntax_theme);
        e.syntax_pinned.fill(false);
        std::size_t present = 0;
        if (const auto* colors = (*t)["syntax"].as_table()) {
            for (std::size_t i = 0; i < kTokenKindCount; ++i) {
                const auto kind = static_cast<TokenKind>(i);
                const auto value = (*colors)[to_string(kind)].value<std::string>();
                std::uint32_t rgb = 0;
                if (value && parse_hex_color(*value, rgb)) {
                    e.syntax_colors[kind] = rgb;
                    e.syntax_pinned[i] = true;
                    ++present;
                }
            }
        }
        if (present == kTokenKindCount) migrate_syntax_pins(e);
    }

    if (const auto* t = tbl["preview"].as_table()) {
        auto& p = out.preview;
        p.backend = (*t)["backend"].value_or(p.backend);
        p.vsync = (*t)["vsync"].value_or(p.vsync);
        p.target_fps = static_cast<int>((*t)["target_fps"].value_or(p.target_fps));
        p.separate_window = (*t)["separate_window"].value_or(p.separate_window);
        p.show_stats = (*t)["show_stats"].value_or(p.show_stats);
        p.pixel_inspector = (*t)["pixel_inspector"].value_or(p.pixel_inspector);
        p.background = (*t)["background"].value_or(p.background);
    }

    if (const auto* t = tbl["files"].as_table()) {
        auto& f = out.files;
        f.projects_dir = (*t)["projects_dir"].value_or(std::string());
        f.project_extension = (*t)["project_extension"].value_or(f.project_extension);
        f.project_archive_extension =
            (*t)["project_archive_extension"].value_or(f.project_archive_extension);
        f.manifest_filename = (*t)["manifest_filename"].value_or(f.manifest_filename);
        f.vertex_extensions = read_string_array((*t)["vertex_extensions"], f.vertex_extensions);
        f.fragment_extensions = read_string_array((*t)["fragment_extensions"], f.fragment_extensions);
        f.compute_extensions = read_string_array((*t)["compute_extensions"], f.compute_extensions);
        f.ask_when_ambiguous = (*t)["ask_when_ambiguous"].value_or(f.ask_when_ambiguous);
        f.autosave_suffix = (*t)["autosave_suffix"].value_or(f.autosave_suffix);
        f.autosave_interval_s = static_cast<int>((*t)["autosave_interval_s"].value_or(f.autosave_interval_s));
        f.watch_external_changes = (*t)["watch_external_changes"].value_or(f.watch_external_changes);
    }

    if (const auto* t = tbl["tools"].as_table()) {
        auto& x = out.tools;
        x.shadercross_dir = (*t)["shadercross_dir"].value_or(std::string());
        x.dxc_path = (*t)["dxc_path"].value_or(std::string());
        x.glslang_path = (*t)["glslang_path"].value_or(std::string());
        x.xcrun_path = (*t)["xcrun_path"].value_or(std::string());
        x.compile_threads = static_cast<int>((*t)["compile_threads"].value_or(x.compile_threads));
        x.shared_compile_cache = (*t)["shared_compile_cache"].value_or(x.shared_compile_cache);
        x.shared_cache_dir = (*t)["shared_cache_dir"].value_or(std::string());
        if (auto v = (*t)["asset_cache_mb"].value<std::int64_t>()) {
            x.asset_cache_bytes = static_cast<std::uint64_t>(*v) * 1024ull * 1024ull;
        }
        if (auto v = (*t)["texture_cache_mb"].value<std::int64_t>()) {
            x.texture_cache_bytes = static_cast<std::uint64_t>(*v) * 1024ull * 1024ull;
        }
        if (auto v = (*t)["download_cache_mb"].value<std::int64_t>()) {
            x.download_cache_bytes = static_cast<std::uint64_t>(*v) * 1024ull * 1024ull;
        }
        x.curl_path = (*t)["curl_path"].value_or(std::string());
    }

    if (const auto* t = tbl["languages"].as_table()) {
        auto& l = out.languages;
        l.hlsl_enabled = (*t)["hlsl_enabled"].value_or(l.hlsl_enabled);
        l.glsl_enabled = (*t)["glsl_enabled"].value_or(l.glsl_enabled);
        if (auto s = (*t)["default_language"].value<std::string>()) {
            l.default_language = language_from_string(*s).value_or(l.default_language);
        }
        l.hlsl_vertex_profile = (*t)["hlsl_vertex_profile"].value_or(l.hlsl_vertex_profile);
        l.hlsl_fragment_profile = (*t)["hlsl_fragment_profile"].value_or(l.hlsl_fragment_profile);
        l.hlsl_compute_profile = (*t)["hlsl_compute_profile"].value_or(l.hlsl_compute_profile);
        l.include_dirs = read_string_array((*t)["include_dirs"], l.include_dirs);
    }

    if (const auto* t = tbl["ui"].as_table()) {
        auto& u = out.ui;
        u.multi_viewport = (*t)["multi_viewport"].value_or(u.multi_viewport);
        u.restore_session = (*t)["restore_session"].value_or(u.restore_session);
        u.show_register_hints = (*t)["show_register_hints"].value_or(u.show_register_hints);
        u.confirm_on_close_dirty = (*t)["confirm_on_close_dirty"].value_or(u.confirm_on_close_dirty);
        u.ui_scale = static_cast<float>((*t)["ui_scale"].value_or(u.ui_scale));
        u.font_size = static_cast<float>((*t)["font_size"].value_or(u.font_size));
        // Out of range reads as unset rather than as the nearest end: a size
        // nobody could have picked in the app is more likely a typo than a wish.
        if (u.font_size != 0.0f && (u.font_size < kMinUiFontSize || u.font_size > kMaxUiFontSize)) {
            u.font_size = 0.0f;
        }
        if (const auto* sc = (*t)["shortcuts"].as_table()) {
            for (const auto& [k, v] : *sc) {
                u.shortcuts[std::string(k.str())] = v.value_or(std::string());
            }
        }
    }

    if (const auto* t = tbl["pack_layout"].as_table()) {
        auto& l = out.pack_layout;
        if (auto s = (*t)["magic"].value<std::string>()) l.set_magic(*s);
        l.extension = (*t)["extension"].value_or(l.extension);
        if (auto s = (*t)["header_preset"].value<std::string>()) {
            l.header_preset = header_preset_from_string(*s).value_or(l.header_preset);
        }
        if (auto s = (*t)["entry_sort"].value<std::string>()) {
            l.entry_sort = entry_sort_from_string(*s).value_or(l.entry_sort);
        }
        l.blobs_before_table = (*t)["blobs_before_table"].value_or(l.blobs_before_table);
        l.key16 = (*t)["key16"].value_or(l.key16);
        l.alignment = static_cast<std::uint32_t>((*t)["alignment"].value_or(l.alignment));
        l.include_names = (*t)["include_names"].value_or(l.include_names);
        l.include_reflection = (*t)["include_reflection"].value_or(l.include_reflection);
        l.include_user_section = (*t)["include_user_section"].value_or(l.include_user_section);
    }

    if (const auto* arr = tbl["recent_projects"].as_array()) {
        for (const auto& n : *arr) out.recent_projects.emplace_back(n.value_or(std::string()));
    } else if (const auto* t = tbl["pack_layout"].as_table()) {
        // Files written before the key was moved have it here, because it was
        // emitted after the last table header and TOML read it as part of that
        // table. Recovering it costs three lines and saves the list the user
        // actually has on disk; the next save writes it where it belongs.
        if (const auto* moved = (*t)["recent_projects"].as_array()) {
            for (const auto& n : *moved) out.recent_projects.emplace_back(n.value_or(std::string()));
        }
    }

    if (const auto* t = tbl["session"].as_table()) {
        if (const auto* arr = (*t)["open_projects"].as_array()) {
            for (const auto& n : *arr) out.open_projects.emplace_back(n.value_or(std::string()));
        }
        out.active_project = static_cast<int>((*t)["active_project"].value_or(out.active_project));

        if (const auto* arr = (*t)["projects"].as_array()) {
            const auto ids_of = [](const toml::table& entry, const char* key) {
                std::vector<std::string> out_ids;
                if (const auto* list = entry[key].as_array()) {
                    for (const auto& id : *list) {
                        std::string one = id.value_or(std::string());
                        if (!one.empty()) out_ids.push_back(std::move(one));
                    }
                }
                return out_ids;
            };
            for (const auto& node : *arr) {
                const auto* entry = node.as_table();
                if (!entry) continue;
                const std::string path = (*entry)["path"].value_or(std::string());
                if (path.empty()) continue;
                ProjectEditorState state;
                state.tab_order = ids_of(*entry, "tab_order");
                state.closed_tabs = ids_of(*entry, "closed_tabs");
                // An entry recording nothing is the same as no entry, and
                // keeping it would only grow the file.
                if (!state.empty()) out.project_editor_state[path] = std::move(state);
            }
        }
    }

    Diagnostics ld = out.pack_layout.validate();
    out_diags.insert(out_diags.end(), ld.begin(), ld.end());
    return !has_errors(ld);
}

bool save_settings(const std::filesystem::path& path, const AppSettings& in,
                   Diagnostics& out_diags) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ostringstream os;
    os << "# SDL Shader Studio - application settings.\n"
       << "# Every option here is also reachable from Settings in the app.\n\n";

    // Before every table header, because that is what makes it a top-level key.
    // Written at the end of the file it looked top-level but belonged to
    // whichever table came last, and load_settings never found it again.
    if (!in.recent_projects.empty()) {
        os << "recent_projects = [";
        for (std::size_t i = 0; i < in.recent_projects.size(); ++i) {
            if (i) os << ", ";
            os << "\"" << escape(in.recent_projects[i].generic_string()) << "\"";
        }
        os << "]\n\n";
    }

    const auto& e = in.editor;
    os << "[editor]\n";
    if (!e.font_path.empty()) os << "font_path = \"" << escape(e.font_path) << "\"\n";
    os << "font_size = " << e.font_size << "\n";
    os << "tab_width = " << e.tab_width << "\n";
    os << "insert_spaces = " << (e.insert_spaces ? "true" : "false") << "\n";
    os << "word_wrap = " << (e.word_wrap ? "true" : "false") << "\n";
    os << "show_line_numbers = " << (e.show_line_numbers ? "true" : "false") << "\n";
    os << "show_whitespace = " << (e.show_whitespace ? "true" : "false") << "\n";
    os << "minimap = " << (e.minimap ? "true" : "false") << "\n";
    os << "error_lens = " << (e.error_lens ? "true" : "false") << "\n";
    os << "auto_indent = " << (e.auto_indent ? "true" : "false") << "\n";
    os << "compile_on_type = " << (e.compile_on_type ? "true" : "false") << "\n";
    os << "compile_debounce_ms = " << e.compile_debounce_ms << "\n";
    os << "check_only_while_typing = " << (e.check_only_while_typing ? "true" : "false")
       << "  # diagnostics only; preview and reflection then update on save\n";
    os << "compile_on_save = " << (e.compile_on_save ? "true" : "false") << "\n";
    os << "warnings_as_errors = " << (e.warnings_as_errors ? "true" : "false") << "\n";
    os << "color_theme = \"" << escape(e.color_theme) << "\"\n";
    os << "reload_theme_on_focus = " << (e.reload_theme_on_focus ? "true" : "false")
       << "  # re-read the theme pack when the window regains focus\n";
    os << "syntax_highlight = " << (e.syntax_highlight ? "true" : "false") << "\n";
    os << "autocomplete = " << (e.autocomplete ? "true" : "false") << "\n";
    os << "syntax_theme = \"" << escape(e.syntax_theme) << "\"  # "
       << "used when the theme sets no syntax colors; one of: ";
    for (std::size_t i = 0; i < syntax_palette_names().size(); ++i) {
        if (i) os << ", ";
        os << syntax_palette_names()[i];
    }
    os << "\n";
    os << "snapshot_history = " << e.snapshot_history << "\n";

    // Only the kinds the user pinned. Writing all ten would say that all ten
    // were chosen by hand, and the next theme would then apply to none of them.
    //
    // Last in the [editor] block on purpose: a TOML sub-table swallows every
    // bare key that follows it, so anything written after this would land in
    // [editor.syntax] instead of [editor].
    if (std::any_of(e.syntax_pinned.begin(), e.syntax_pinned.end(), [](bool p) { return p; })) {
        os << "\n[editor.syntax]  # colors you changed yourself; the theme supplies the rest\n";
        for (std::size_t i = 0; i < kTokenKindCount; ++i) {
            if (!e.syntax_pinned[i]) continue;
            const auto kind = static_cast<TokenKind>(i);
            os << to_string(kind) << " = \"" << format_hex_color(e.syntax_colors[kind]) << "\"\n";
        }
    }

    const auto& p = in.preview;
    os << "\n[preview]\n";
    os << "backend = \"" << escape(p.backend) << "\"  # \"\" = let SDL choose\n";
    os << "vsync = " << (p.vsync ? "true" : "false") << "\n";
    os << "target_fps = " << p.target_fps << "\n";
    os << "separate_window = " << (p.separate_window ? "true" : "false") << "\n";
    os << "show_stats = " << (p.show_stats ? "true" : "false") << "\n";
    os << "pixel_inspector = " << (p.pixel_inspector ? "true" : "false") << "\n";
    os << "background = \"" << escape(p.background) << "\"\n";

    const auto& f = in.files;
    os << "\n[files]\n";
    os << "projects_dir = \"" << escape(f.projects_dir.generic_string())
       << "\"  # \"\" = alongside the app, or Documents\n";
    os << "project_extension = \"" << escape(f.project_extension) << "\"\n";
    os << "project_archive_extension = \"" << escape(f.project_archive_extension) << "\"\n";
    os << "manifest_filename = \"" << escape(f.manifest_filename) << "\"\n";
    write_string_array(os, "vertex_extensions", f.vertex_extensions);
    write_string_array(os, "fragment_extensions", f.fragment_extensions);
    write_string_array(os, "compute_extensions", f.compute_extensions);
    os << "ask_when_ambiguous = " << (f.ask_when_ambiguous ? "true" : "false") << "\n";
    os << "autosave_suffix = \"" << escape(f.autosave_suffix) << "\"\n";
    os << "autosave_interval_s = " << f.autosave_interval_s << "\n";
    os << "watch_external_changes = " << (f.watch_external_changes ? "true" : "false") << "\n";

    const auto& x = in.tools;
    os << "\n[tools]\n";
    os << "shadercross_dir = \"" << escape(x.shadercross_dir.generic_string()) << "\"\n";
    os << "dxc_path = \"" << escape(x.dxc_path.generic_string()) << "\"\n";
    os << "glslang_path = \"" << escape(x.glslang_path.generic_string()) << "\"\n";
    os << "xcrun_path = \"" << escape(x.xcrun_path.generic_string()) << "\"\n";
    os << "compile_threads = " << x.compile_threads << "\n";
    os << "shared_compile_cache = " << (x.shared_compile_cache ? "true" : "false") << "\n";
    os << "shared_cache_dir = \"" << escape(x.shared_cache_dir.generic_string()) << "\"\n";
    os << "asset_cache_mb = " << (x.asset_cache_bytes / (1024 * 1024)) << "\n";
    os << "texture_cache_mb = " << (x.texture_cache_bytes / (1024 * 1024)) << "\n";
    os << "download_cache_mb = " << (x.download_cache_bytes / (1024 * 1024)) << "\n";
    if (!x.curl_path.empty()) os << "curl_path = \"" << escape(x.curl_path.string()) << "\"\n";

    const auto& l = in.languages;
    os << "\n[languages]\n";
    os << "hlsl_enabled = " << (l.hlsl_enabled ? "true" : "false") << "\n";
    os << "glsl_enabled = " << (l.glsl_enabled ? "true" : "false") << "\n";
    os << "default_language = \"" << to_string(l.default_language) << "\"\n";
    os << "hlsl_vertex_profile = \"" << escape(l.hlsl_vertex_profile) << "\"\n";
    os << "hlsl_fragment_profile = \"" << escape(l.hlsl_fragment_profile) << "\"\n";
    os << "hlsl_compute_profile = \"" << escape(l.hlsl_compute_profile) << "\"\n";
    write_string_array(os, "include_dirs", l.include_dirs);

    const auto& u = in.ui;
    os << "\n[ui]\n";
    os << "multi_viewport = " << (u.multi_viewport ? "true" : "false") << "\n";
    os << "restore_session = " << (u.restore_session ? "true" : "false")
       << "  # reopen the projects that were open last time\n";
    os << "show_register_hints = " << (u.show_register_hints ? "true" : "false") << "\n";
    os << "confirm_on_close_dirty = " << (u.confirm_on_close_dirty ? "true" : "false") << "\n";
    os << "ui_scale = " << u.ui_scale << "\n";
    os << "font_size = " << u.font_size << "  # interface text in px; 0 = the theme's size\n";
    if (!u.shortcuts.empty()) {
        os << "\n[ui.shortcuts]\n";
        for (const auto& [k, v] : u.shortcuts) os << k << " = \"" << escape(v) << "\"\n";
    }

    const auto& pl = in.pack_layout;
    os << "\n[pack_layout]  # default container format for new projects\n";
    os << "magic = \"" << pl.magic_string() << "\"\n";
    os << "extension = \"" << escape(pl.extension) << "\"\n";
    os << "header_preset = \"" << to_string(pl.header_preset) << "\"  # compact | padded64\n";
    os << "entry_sort = \"" << to_string(pl.entry_sort)
       << "\"  # by_key | by_stage_then_key | manifest_order\n";
    os << "blobs_before_table = " << (pl.blobs_before_table ? "true" : "false") << "\n";
    os << "key16 = " << (pl.key16 ? "true" : "false") << "\n";
    os << "alignment = " << pl.alignment << "  # 16 | 64 | 256\n";
    os << "include_names = " << (pl.include_names ? "true" : "false") << "\n";
    os << "include_reflection = " << (pl.include_reflection ? "true" : "false") << "\n";
    os << "include_user_section = " << (pl.include_user_section ? "true" : "false") << "\n";

    // Last, and in its own table: this is the one part of the file the app
    // rewrites on its own rather than because the user changed a setting.
    os << "\n[session]  # what was open last time; see ui.restore_session\n";
    os << "open_projects = [";
    for (std::size_t i = 0; i < in.open_projects.size(); ++i) {
        if (i) os << ", ";
        os << "\"" << escape(in.open_projects[i].generic_string()) << "\"";
    }
    os << "]\n";
    os << "active_project = " << in.active_project << "\n";

    // Last of all: every key after an array-of-tables header belongs to it, so
    // nothing scalar may follow. One table per project keeps the paths out of
    // TOML key position, where they would need quoting and would read badly.
    if (!in.project_editor_state.empty()) {
        const auto write_ids = [&os](const char* key, const std::vector<std::string>& ids) {
            if (ids.empty()) return;
            os << key << " = [";
            for (std::size_t i = 0; i < ids.size(); ++i) {
                if (i) os << ", ";
                os << "\"" << escape(ids[i]) << "\"";
            }
            os << "]\n";
        };
        os << "\n# How the editor was left in each project: where the shader tabs were and\n";
        os << "# which were closed. Editor state, not project state - a shader listed as\n";
        os << "# closed is still fully part of its project.\n";
        for (const auto& [path, state] : in.project_editor_state) {
            if (state.empty()) continue;
            os << "[[session.projects]]\n";
            os << "path = \"" << escape(path) << "\"\n";
            write_ids("tab_order", state.tab_order);
            write_ids("closed_tabs", state.closed_tabs);
        }
    }

    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file) {
            out_diags.push_back(error("cannot write " + tmp.string()));
            return false;
        }
        const std::string text = os.str();
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        out_diags.push_back(error("cannot replace " + path.string() + ": " + ec.message()));
        return false;
    }
    return true;
}

bool classify_source_file(const FileSettings& fs, const std::filesystem::path& p, Stage& out_stage,
                          Language& out_language) {
    const std::string name = lower(p.filename().string());

    struct Group {
        const std::vector<std::string>* list;
        Stage stage;
    };
    const Group groups[] = {{&fs.vertex_extensions, Stage::Vertex},
                            {&fs.fragment_extensions, Stage::Fragment},
                            {&fs.compute_extensions, Stage::Compute}};

    // Longest match wins, so ".vert.hlsl" beats a bare ".hlsl" rule.
    std::size_t best = 0;
    bool found = false;
    for (const auto& g : groups) {
        for (const auto& ext : *g.list) {
            const std::string e = lower(ext);
            if (!ends_with(name, e) || e.size() <= best) continue;
            best = e.size();
            out_stage = g.stage;
            out_language = e.find("glsl") != std::string::npos ? Language::GLSL : Language::HLSL;
            found = true;
        }
    }
    return found;
}

}  // namespace ssstudio
