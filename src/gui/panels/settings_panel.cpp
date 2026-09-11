// Every option in AppSettings is reachable here, grouped the way the plan
// describes: nothing is settable only by editing the TOML by hand.
#include <algorithm>
#include <cfloat>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <imgui.h>

#include "app.h"
#include "panel_common.h"

namespace ssstudio::gui {
namespace {

bool input_string(const char* label, std::string& value, const char* hint = nullptr) {
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer), "%s", value.c_str());
    const std::string id = left_label(label);
    const bool changed = hint
                             ? ImGui::InputTextWithHint(id.c_str(), hint, buffer, sizeof(buffer))
                             : ImGui::InputText(id.c_str(), buffer, sizeof(buffer));
    if (changed) value = buffer;
    return changed;
}

bool input_path(const char* label, std::filesystem::path& value, const char* hint) {
    std::string text = value.generic_string();
    if (!input_string(label, text, hint)) return false;
    value = text;
    return true;
}

bool string_list(const char* label, std::vector<std::string>& values) {
    bool changed = false;
    if (!ImGui::TreeNode(label)) return false;
    for (std::size_t i = 0; i < values.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        // The field takes the row minus the remove button, so it narrows with
        // the panel instead of pushing the button off the edge.
        const float remove = button_width("x") + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetNextItemWidth(std::max(40.0f, ImGui::GetContentRegionAvail().x - remove));
        changed |= input_string("##entry", values[i]);
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            values.erase(values.begin() + static_cast<long>(i));
            ImGui::PopID();
            ImGui::TreePop();
            return true;
        }
        ImGui::PopID();
    }
    if (ImGui::SmallButton("add")) {
        values.emplace_back();
        changed = true;
    }
    ImGui::TreePop();
    return changed;
}

/// Hands a directory to the desktop's file manager.
///
/// Works for a directory inside the application bundle as well as one in the
/// user's own space: a file manager given an exact path opens it, even where it
/// would not show the enclosing folder by default.
void open_directory(App& app, const std::filesystem::path& path) {
    const std::filesystem::path directory = external_path(path);
    if (directory.empty() || !directory.is_absolute()) {
        app.log(Severity::Warning, "could not work out where that folder lives on disk");
        return;
    }
    if (!SDL_OpenURL(file_url(directory).c_str())) {
        // The path goes in the message, so a desktop with no file manager
        // registered still leaves somewhere to copy from.
        app.log(Severity::Warning,
                "could not open " + directory.string() + ": " + SDL_GetError());
    }
}

/// Opens the themes/ directory beside settings.toml, filling it in the first
/// time with the packs that shipped inside the application.
///
/// Copied rather than pointed at. The packs the application ships with live
/// inside it - inside the .app on macOS - where a file manager will not show
/// them and a text editor should not be asked to save into them. An empty
/// folder with a note explaining where the real files are is an explanation
/// where a person wanted a file, so this puts the files there: a working theme
/// to read, a reference listing every option, and the README describing both.
///
/// Only when there is nothing here yet, and never over the top of a file of the
/// same name. Once a copy is yours, it is yours - including when a later
/// version of the application ships a newer one, because overwriting an edit
/// nobody asked to lose is worse than shipping a stale sample.
void open_themes_directory(App& app) {
    const std::filesystem::path directory = external_path(app.user_themes_dir());
    if (directory.empty() || !directory.is_absolute()) {
        app.log(Severity::Warning, "could not work out where the themes directory lives on disk");
        return;
    }

    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec && !std::filesystem::is_directory(directory, ec)) {
        app.log(Severity::Warning,
                "could not create " + directory.string() + ": " + ec.message());
        return;
    }

    // "No packs here yet", not "we just made it" and not "the folder is empty".
    // A folder opened once already exists, and a file manager leaves a dotfile
    // of its own in it, so creation is the wrong moment to test for; a stray
    // note or a leftover README is not a theme either. What decides this is
    // whether the folder does the job it is for.
    bool has_a_pack = false;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (!theme_pack_id(entry.path()).empty()) {
            has_a_pack = true;
            break;
        }
    }

    const std::filesystem::path shipped = app.shipped_themes_dir();
    if (!has_a_pack && !shipped.empty()) {
        int copied = 0;
        for (const auto& entry : std::filesystem::directory_iterator(shipped, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            const std::filesystem::path destination = directory / entry.path().filename();
            if (std::filesystem::exists(destination, ec)) continue;
            std::error_code copy_ec;
            std::filesystem::copy_file(entry.path(), destination, copy_ec);
            if (copy_ec) {
                app.log(Severity::Warning, "could not copy " + entry.path().filename().string() +
                                               ": " + copy_ec.message());
                continue;
            }
            ++copied;
        }
        if (copied > 0) {
            // An earlier version of this button left a note here instead of the
            // files. The files say everything it said, so it goes.
            std::error_code note_ec;
            std::filesystem::remove(directory / "README.txt", note_ec);
            app.log(Severity::Info, "put " + std::to_string(copied) +
                                        " file(s) in your themes folder to start from");
            // The copies are packs in their own right now, under a root that
            // outranks the application's, so the list has to be rebuilt before
            // anything reads it again.
            app.reload_theme_packs();
        }
    }

    open_directory(app, directory);
}

/// The fallback palette, and the ten color swatches behind it.
///
/// Picking a palette overwrites the kinds the user has not pinned; touching any
/// one swatch pins that kind. A pinned kind survives a theme change and an
/// unpinned one follows the theme, so the marker and the revert button beside
/// each swatch are not decoration: without them a theme that applies to eight
/// of ten kinds reads as a bug rather than as a choice the user made.
bool draw_syntax_colors(EditorSettings& e, const ResolvedTheme& theme) {
    bool changed = false;

    const auto& names = syntax_palette_names();
    // No "custom" any more: customness is per kind, recorded by the pins, so a
    // name over all ten has nothing left to say.
    //
    // Shown capitalized like the theme names and the token kinds beside them;
    // `names` keeps the spelling the settings file uses. `shown` has to outlive
    // the Combo below, because what is handed over is pointers into it.
    std::vector<std::string> shown;
    for (const auto& name : names) shown.push_back(display_case(name));
    std::vector<const char*> labels;
    for (const auto& label : shown) labels.push_back(label.c_str());

    int current = 0;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i] == e.syntax_theme) current = static_cast<int>(i);
    }
    if (ImGui::Combo(left_label("Fallback palette").c_str(), &current, labels.data(),
                     static_cast<int>(labels.size()))) {
        e.syntax_theme = names[static_cast<std::size_t>(current)];
        const SyntaxColors palette = syntax_palette(e.syntax_theme);
        for (std::size_t i = 0; i < kTokenKindCount; ++i) {
            if (!e.syntax_pinned[i]) e.syntax_colors.rgb[i] = palette.rgb[i];
        }
        changed = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Used for the kinds the current theme does not set.");
    }

    if (!ImGui::TreeNode("Colors")) return changed;

    const bool any_pinned =
        std::any_of(e.syntax_pinned.begin(), e.syntax_pinned.end(), [](bool p) { return p; });
    ImGui::BeginDisabled(!any_pinned);
    if (ImGui::SmallButton("Reset all to theme")) {
        for (std::size_t i = 0; i < kTokenKindCount; ++i) {
            e.syntax_pinned[i] = false;
            e.syntax_colors.rgb[i] = theme.syntax[i] >> 8;
        }
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled(any_pinned ? "* marks a color you set yourself"
                                   : "every color follows the theme");

    for (std::size_t i = 0; i < kTokenKindCount; ++i) {
        const auto kind = static_cast<TokenKind>(i);
        const std::uint32_t rgb = e.syntax_colors[kind];
        float color[3] = {static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
                          static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                          static_cast<float>(rgb & 0xFF) / 255.0f};
        // Capitalized for display only, like every other heading and label; the
        // lower-case spelling is what the settings file keys on. The asterisk
        // says this one is the user's rather than the theme's.
        const std::string label =
            display_case(to_string(kind)) + (e.syntax_pinned[i] ? " *" : "");
        ImGui::PushID(static_cast<int>(i));
        if (color_field(label.c_str(), color, 3)) {
            const auto channel = [](float v) {
                return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
            };
            e.syntax_colors[kind] =
                (channel(color[0]) << 16) | (channel(color[1]) << 8) | channel(color[2]);
            e.syntax_pinned[i] = true;
            changed = true;
        }
        if (e.syntax_pinned[i]) {
            ImGui::SameLine();
            if (ImGui::SmallButton("revert")) {
                e.syntax_pinned[i] = false;
                e.syntax_colors.rgb[i] = theme.syntax[i] >> 8;
                changed = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Follow the theme again.");
        }
        ImGui::PopID();
    }
    ImGui::TreePop();
    return changed;
}

/// The theme picker: the three built-in themes, then every installed pack.
///
/// Ids rather than display names in the settings file, so the list shows the
/// name and records the id. A pack that is not installed still shows, marked,
/// rather than silently becoming dark - a settings file that travelled from
/// another machine should say what it is missing.
bool draw_theme_picker(EditorSettings& e, const std::map<std::string, ThemePack>& packs,
                       std::filesystem::path& show_pack_folder) {
    // Two lists, deliberately: `ids` is what the settings file records and
    // `labels` is what the list reads as. Every name goes through display_case,
    // so the built-in ids and a pack whose author wrote its name in lower case
    // both sit under the same capital as every other heading in this panel.
    std::vector<std::string> ids = {"dark", "light", "classic"};
    std::vector<std::string> labels;
    for (const auto& id : ids) labels.push_back(display_case(id));
    for (const auto& [id, pack] : packs) {
        ids.push_back(id);
        labels.push_back(display_case(pack.name.empty() ? id : pack.name));
    }
    bool installed = true;
    if (std::find(ids.begin(), ids.end(), e.color_theme) == ids.end()) {
        installed = false;
        ids.push_back(e.color_theme);
        labels.push_back(display_case(e.color_theme) + " (not installed)");
    }

    std::vector<const char*> items;
    for (const auto& label : labels) items.push_back(label.c_str());
    int current = 0;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (ids[i] == e.color_theme) current = static_cast<int>(i);
    }

    bool changed = false;
    if (ImGui::Combo(left_label("Theme").c_str(), &current, items.data(),
                     static_cast<int>(items.size()))) {
        e.color_theme = ids[static_cast<std::size_t>(current)];
        changed = true;
    }

    if (const auto it = packs.find(e.color_theme); it != packs.end()) {
        ImGui::Indent();
        if (!it->second.description.empty()) {
            ImGui::TextWrapped("%s", it->second.description.c_str());
        }
        std::string credit = it->second.author;
        if (!it->second.version.empty()) {
            credit += credit.empty() ? it->second.version : " - " + it->second.version;
        }
        if (!credit.empty()) ImGui::TextDisabled("%s", credit.c_str());
        // Where it came from, click to copy. Three directories are searched and
        // only one of them is the folder the button above opens, so "which file
        // am I actually looking at" is a question this panel should answer
        // rather than leave to be guessed.
        const std::filesystem::path folder = it->second.file.parent_path();
        copyable_path(folder.generic_string());
        ImGui::SameLine();
        if (ImGui::SmallButton("Show")) show_pack_folder = folder;
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Opens the folder this theme was read from.");
        }
        ImGui::Unindent();
    } else if (!installed) {
        ImGui::Indent();
        ImGui::TextDisabled("Showing dark until this theme is installed.");
        ImGui::Unindent();
    }
    return changed;
}

bool draw_editor_settings(EditorSettings& e, const ResolvedTheme& theme,
                          const std::map<std::string, ThemePack>& packs, bool& reload_themes,
                          bool& open_folder, std::filesystem::path& show_pack_folder) {
    bool changed = false;
    changed |= ImGui::SliderFloat(left_label("Font size").c_str(), &e.font_size, kMinEditorFontSize,
                                  kMaxEditorFontSize, "%.0f");
    ImGui::Indent();
#if defined(__APPLE__)
    ImGui::TextDisabled("Cmd = / Cmd - zooms the editor, Cmd 0 resets.");
#else
    ImGui::TextDisabled("Ctrl = / Ctrl - zooms the editor, Ctrl 0 resets.");
#endif
    ImGui::Unindent();
    changed |= input_string("Font file", e.font_path, "empty = bundled font");
    changed |= ImGui::SliderInt(left_label("Tab width").c_str(), &e.tab_width, 1, 8);
    changed |= ImGui::Checkbox("Insert spaces", &e.insert_spaces);
    changed |= ImGui::Checkbox("Word wrap", &e.word_wrap);
    changed |= ImGui::Checkbox("Line numbers", &e.show_line_numbers);
    changed |= ImGui::Checkbox("Show whitespace", &e.show_whitespace);
    changed |= ImGui::Checkbox("Inline diagnostics", &e.error_lens);
    changed |= ImGui::Checkbox("Auto indent", &e.auto_indent);
    ImGui::Separator();
    changed |= ImGui::Checkbox("Compile while typing", &e.compile_on_type);
    ImGui::BeginDisabled(!e.compile_on_type);
    changed |= ImGui::SliderInt(left_label("Debounce (ms)").c_str(), &e.compile_debounce_ms, 0, 2000);
    changed |= ImGui::Checkbox("While typing, check for errors only",
                               &e.check_only_while_typing);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Skips code generation, so errors arrive sooner on a large shader.\n"
            "The preview and the I/O panel then update when you save, not as you type.");
    }
    ImGui::EndDisabled();
    changed |= ImGui::Checkbox("Compile on save", &e.compile_on_save);
    changed |= ImGui::Checkbox("Suggest completions while typing", &e.autocomplete);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Ctrl+Space opens the list whatever this is set to.\n"
            "Up and Down choose, Tab or Enter inserts, Esc dismisses.");
    }
    changed |= ImGui::Checkbox("Treat warnings as errors", &e.warnings_as_errors);
    changed |= ImGui::SliderInt(left_label("Snapshot history").c_str(), &e.snapshot_history, 0, 200);

    changed |= draw_theme_picker(e, packs, show_pack_folder);
    if (ImGui::SmallButton("Reload themes")) reload_themes = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("Open folder")) open_folder = true;
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Opens your themes folder, beside your settings file.\n"
                          "The first time, the themes that came with the app are copied "
                          "in for you\nto read and edit - including showcase.s3theme, "
                          "which lists every option.\n"
                          "Copy one, rename it, then press Reload themes. The file name "
                          "is the\ntheme's name.");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu pack(s) installed", packs.size());

    changed |= ImGui::Checkbox("Reload the theme when the window regains focus",
                               &e.reload_theme_on_focus);
    if (ImGui::IsItemHovered()) {
        // Four lines because each one answers a question this setting actually
        // raises - when it acts, what it costs, what it misses, what happens
        // when the edit is wrong. Why it is off by default belongs next to the
        // field in settings.h, not here.
        ImGui::SetTooltip(
            "Applies edits you save to the active theme pack when you switch back to "
            "the app.\n"
            "One file, checked on focus - nothing runs while you work, or while a "
            "built-in theme is selected.\n"
            "A pack added to themes/, or one the active pack inherits from, still needs "
            "Reload themes.\n"
            "A broken edit is reported in Diagnostics; the theme on screen is left "
            "alone.");
    }

    ImGui::SeparatorText("Syntax colors");
    changed |= ImGui::Checkbox("Color keywords and types", &e.syntax_highlight);
    ImGui::BeginDisabled(!e.syntax_highlight);
    changed |= draw_syntax_colors(e, theme);
    ImGui::EndDisabled();
    return changed;
}

bool draw_preview_settings(PreviewDefaults& p) {
    bool changed = false;
    changed |= input_string("Backend", p.backend, "empty = let SDL choose");
    changed |= ImGui::Checkbox("Vsync", &p.vsync);
    changed |= ImGui::SliderInt(left_label("Target fps (0 = uncapped)").c_str(), &p.target_fps, 0, 240);
    changed |= ImGui::Checkbox("Preview in a separate window", &p.separate_window);
    changed |= ImGui::Checkbox("Show stats", &p.show_stats);
    changed |= ImGui::Checkbox("Pixel inspector", &p.pixel_inspector);

    const char* backgrounds[] = {"checkerboard", "color", "transparent"};
    int current = p.background == "color" ? 1 : (p.background == "transparent" ? 2 : 0);
    if (ImGui::Combo(left_label("Background").c_str(), &current, backgrounds, IM_ARRAYSIZE(backgrounds))) {
        p.background = backgrounds[current];
        changed = true;
    }
    return changed;
}

/// `resolved_projects_dir` is what the app would use right now, so the field can
/// say what leaving it empty actually means on this machine rather than making
/// the user find out by opening the New Project dialog.
bool draw_file_settings(FileSettings& f, const std::string& resolved_projects_dir) {
    bool changed = false;

    ImGui::SeparatorText("Projects");
    changed |= input_path("Projects folder", f.projects_dir, "empty = the default below");
    ImGui::Indent();
    ImGui::TextDisabled("New projects start here, and Open falls back to it.");
    ImGui::TextWrapped("Currently: %s", resolved_projects_dir.c_str());
    ImGui::Unindent();
    ImGui::Spacing();

    ImGui::SeparatorText("Naming");
    ImGui::TextDisabled(
        "Extensions are yours to choose; the app matches the longest one, so .vert.hlsl wins "
        "over .hlsl.");
    changed |= input_string("Project extension", f.project_extension);
    changed |= input_string("Archive extension", f.project_archive_extension);
    changed |= input_string("Manifest filename", f.manifest_filename);
    changed |= string_list("vertex extensions", f.vertex_extensions);
    changed |= string_list("fragment extensions", f.fragment_extensions);
    changed |= string_list("compute extensions", f.compute_extensions);
    changed |= ImGui::Checkbox("Ask when a filename is ambiguous", &f.ask_when_ambiguous);
    ImGui::Separator();
    changed |= input_string("Autosave suffix", f.autosave_suffix);
    changed |= ImGui::SliderInt(left_label("Autosave interval (s)").c_str(), &f.autosave_interval_s, 0, 600);
    changed |= ImGui::Checkbox("Watch for external changes", &f.watch_external_changes);
    return changed;
}

bool draw_tool_settings(ToolSettings& t) {
    bool changed = false;
    ImGui::TextDisabled(
        "The app ships with its own toolchain. These override it, in case you\n"
        "want a newer one or one built for a different target.");
    ImGui::Spacing();
    changed |= input_path("shadercross dir", t.shadercross_dir, "empty = the bundled copy");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "The directory holding the shadercross tool, or the prefix above its bin/.\n"
            "Left empty, the copy shipped inside the application is used, and failing\n"
            "that whatever is on PATH.");
    }
    changed |= input_path("dxc", t.dxc_path, "empty = bundled");
    changed |= input_path("glslang", t.glslang_path, "empty = built in");
    changed |= input_path("xcrun", t.xcrun_path, "macOS metallib");
    changed |= ImGui::SliderInt(left_label("Compile threads").c_str(), &t.compile_threads, 1, 16);
    changed |= ImGui::Checkbox("Share the compile cache between projects",
                               &t.shared_compile_cache);
    changed |= input_path("Shared cache dir", t.shared_cache_dir, "empty = platform default");

    int cache_mb = static_cast<int>(t.asset_cache_bytes / (1024 * 1024));
    if (ImGui::SliderInt(left_label("Asset cache (MB)").c_str(), &cache_mb, 16, 4096)) {
        t.asset_cache_bytes = static_cast<std::uint64_t>(cache_mb) * 1024ull * 1024ull;
        changed = true;
    }
    return changed;
}

bool draw_language_settings(LanguageSettings& l) {
    bool changed = false;
    FlowLayout row;
    row.next(checkbox_width("HLSL"));
    changed |= ImGui::Checkbox("HLSL", &l.hlsl_enabled);
    row.next(checkbox_width("GLSL"));
    changed |= ImGui::Checkbox("GLSL", &l.glsl_enabled);

    const char* languages[] = {"hlsl", "glsl"};
    int current = l.default_language == Language::GLSL ? 1 : 0;
    if (ImGui::Combo(left_label("Default language").c_str(), &current, languages, IM_ARRAYSIZE(languages))) {
        l.default_language = current == 1 ? Language::GLSL : Language::HLSL;
        changed = true;
    }
    changed |= input_string("Vertex profile", l.hlsl_vertex_profile);
    changed |= input_string("Fragment profile", l.hlsl_fragment_profile);
    changed |= input_string("Compute profile", l.hlsl_compute_profile);
    changed |= string_list("include directories", l.include_dirs);
    return changed;
}

bool draw_pack_layout_settings(PackLayout& layout, const ResolvedTheme& theme) {
    bool changed = false;
    ImGui::TextDisabled(
        "The container format is yours. Whatever you pick is recorded in the pack header and "
        "baked into the generated loader, which refuses a pack that does not match.");

    std::string magic = layout.magic_string();
    if (input_string("Magic (4 chars)", magic)) {
        layout.set_magic(magic);
        changed = true;
    }
    changed |= input_string("Pack extension", layout.extension);

    const char* presets[] = {"compact", "padded64"};
    int preset = layout.header_preset == HeaderPreset::Padded64 ? 1 : 0;
    if (ImGui::Combo(left_label("Header preset").c_str(), &preset, presets, IM_ARRAYSIZE(presets))) {
        layout.header_preset = preset == 1 ? HeaderPreset::Padded64 : HeaderPreset::Compact;
        changed = true;
    }

    const char* sorts[] = {"by key (binary search)", "by stage then key", "manifest order (scan)"};
    int sort = static_cast<int>(layout.entry_sort);
    if (ImGui::Combo(left_label("Entry order").c_str(), &sort, sorts, IM_ARRAYSIZE(sorts))) {
        layout.entry_sort = static_cast<EntrySort>(sort);
        changed = true;
    }

    const std::array<std::uint32_t, 3> alignments{16, 64, 256};
    int alignment_index = layout.alignment == 256 ? 2 : (layout.alignment == 64 ? 1 : 0);
    const char* alignment_labels[] = {"16", "64", "256"};
    if (ImGui::Combo(left_label("Blob alignment").c_str(), &alignment_index, alignment_labels,
                     IM_ARRAYSIZE(alignment_labels))) {
        layout.alignment = alignments[static_cast<std::size_t>(alignment_index)];
        changed = true;
    }

    changed |= ImGui::Checkbox("16-bit keys", &layout.key16);
    changed |= ImGui::Checkbox("Blobs before the entry table", &layout.blobs_before_table);
    changed |= ImGui::Checkbox("Store shader names", &layout.include_names);
    changed |= ImGui::Checkbox("Store reflection", &layout.include_reflection);
    changed |= ImGui::Checkbox("Reserve a user section", &layout.include_user_section);

    ImGui::Separator();
    ImGui::TextDisabled("signature: %s", layout.signature().c_str());

    Diagnostics diags = layout.validate();
    for (const auto& d : diags) {
        const ImVec4 color = theme_vec4(theme.diagnostic(d.severity));
        ImGui::TextColored(color, "%s", d.message.c_str());
    }
    return changed;
}

}  // namespace

void draw_settings_panel(App& app) {
    // Settings is a window you open, change something in and close again, not a
    // panel that lives in the layout. Docking it into the workspace would put it
    // permanently in the way of the thing it is configuring.
    //
    // NoAutoMerge asks the platform for a real window rather than one drawn
    // inside the main one, so it appears where the desktop puts windows and can
    // be reached the way every other window can. It has to be set through a
    // window class: whether a viewport is created at all is decided before the
    // window's own flags are read.
    ImGuiWindowClass standalone;
    standalone.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
    ImGui::SetNextWindowClass(&standalone);

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(std::min(560.0f, viewport->WorkSize.x * 0.5f),
                                    std::min(680.0f, viewport->WorkSize.y * 0.8f)),
                             ImGuiCond_FirstUseEver);
    // A fifth of the screen at the smallest, and no ceiling. The same floor the
    // graph window has: these settings are two columns of labelled controls, and
    // below that the labels start eating the fields they belong to.
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(viewport->WorkSize.x * 0.2f, viewport->WorkSize.y * 0.2f),
        ImVec2(FLT_MAX, FLT_MAX));
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
               viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
        ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));

    PanelScope panel("Settings", &app.show_settings, ImGuiWindowFlags_NoDocking);
    if (!panel) return;

    AppSettings& settings = app.settings();
    bool changed = false;
    bool theme_changed = false;
    bool reload_themes = false;
    bool open_folder = false;
    std::filesystem::path show_pack_folder;

    if (ImGui::BeginTabBar("##settings")) {
        if (ImGui::BeginTabItem("Editor")) {
            const std::string before = settings.editor.color_theme;
            changed |= draw_editor_settings(settings.editor, app.theme(), app.theme_packs(),
                                            reload_themes, open_folder, show_pack_folder);
            theme_changed = settings.editor.color_theme != before;
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Preview")) {
            changed |= draw_preview_settings(settings.preview);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Files")) {
            changed |= draw_file_settings(settings.files, app.projects_dir().generic_string());
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Tools")) {
            ImGui::TextDisabled("current backend: %s", app.backend_name().c_str());
            changed |= draw_tool_settings(settings.tools);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Languages")) {
            changed |= draw_language_settings(settings.languages);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Formats")) {
            changed |= draw_pack_layout_settings(settings.pack_layout, app.theme());
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Interface")) {
            // The scale lives in the style, which is only rebuilt by
            // apply_theme - so moving this has to ask for that rebuild, the
            // same as changing the theme does.
            if (ImGui::SliderFloat(left_label("UI scale").c_str(), &settings.ui.ui_scale, 0.75f,
                                   2.0f, "%.2f")) {
                changed = true;
                theme_changed = true;
            }
            changed |= ImGui::Checkbox("Multi-viewport windows", &settings.ui.multi_viewport);
            changed |= ImGui::Checkbox("Show register hints", &settings.ui.show_register_hints);
            changed |= ImGui::Checkbox("Confirm when closing with unsaved edits",
                                       &settings.ui.confirm_on_close_dirty);
            changed |= ImGui::Checkbox("Reopen last session's projects at startup",
                                       &settings.ui.restore_session);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Opens the projects that were open when you last quit.\n"
                    "A project path on the command line takes precedence.");
            }
            ImGui::SeparatorText("Shortcuts");
            for (const auto& action : app.actions()) {
                if (action.shortcut.empty()) continue;
                ImGui::BulletText("%-28s %s", action.label.c_str(), action.shortcut.c_str());
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    // Both after the tab bar, like every other action this panel collects: the
    // one opens a window of someone else's, the other re-reads files, and
    // neither belongs in the middle of drawing a form.
    if (open_folder) open_themes_directory(app);
    if (!show_pack_folder.empty()) open_directory(app, show_pack_folder);
    // Reloading first, so picking a theme in the same frame a pack was added
    // finds it.
    if (reload_themes) {
        app.reload_theme_packs();
        theme_changed = true;
    }
    if (theme_changed) app.apply_current_theme();
    if (changed) app.save_settings_now();

}

}  // namespace ssstudio::gui
