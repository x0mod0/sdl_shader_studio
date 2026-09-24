// Every option in AppSettings is reachable here, grouped the way the plan
// describes: nothing is settable only by editing the TOML by hand.
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <array>
#include <iterator>
#include <string_view>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <imgui.h>

#include "app.h"
#include "panel_common.h"
#include "widgets.h"

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

/// A slider whose value takes effect when it is let go, rather than on every
/// frame it is dragged.
///
/// For the settings that resize the interface - this window included. Applied
/// while dragging, each new size moves the slider under the pointer, the
/// pointer then reads a different value off it, that value resizes everything
/// again, and the window flickers between two sizes without settling on either
/// - nor saving either. So the slider shows the value being dragged the whole
/// time, and `value` changes only on the frame it is released, or a value typed
/// into it with Ctrl+click is entered. Returns true on exactly that frame.
///
/// `slider` draws the widget itself, given the id to use and the value to show,
/// so one helper serves float and integer sliders alike. The label is drawn in
/// a column `column` wide and the slider is `width` wide, so a group of these
/// line up with each other rather than each starting wherever its label ends.
template <typename T, typename Slider>
bool slider_applied_on_release(const char* label, float column, float width, T& value,
                               Slider&& slider) {
    // The value being dragged, per slider, while it is held. One entry at most
    // in practice: only one slider can be held at a time.
    static std::map<ImGuiID, T> held;
    const float start = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(start + column);
    ImGui::SetNextItemWidth(width);
    const std::string id = std::string("##") + label;
    const ImGuiID key = ImGui::GetID(id.c_str());
    const auto it = held.find(key);
    T shown = it != held.end() ? it->second : value;
    slider(id.c_str(), &shown);
    const bool released = ImGui::IsItemDeactivatedAfterEdit();
    if (ImGui::IsItemActive()) {
        held[key] = shown;
    } else {
        held.erase(key);
    }
    if (released) value = shown;
    return released;
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
            // A pack can be a directory - Tide is, so that it can carry its
            // fonts - and those are copied whole. Any other directory is not a
            // theme and stays where it is.
            const bool pack_directory =
                entry.is_directory(ec) && !theme_pack_id(entry.path()).empty();
            if (!entry.is_regular_file(ec) && !pack_directory) continue;
            const std::filesystem::path destination = directory / entry.path().filename();
            if (std::filesystem::exists(destination, ec)) continue;
            std::error_code copy_ec;
            if (pack_directory) {
                std::filesystem::copy(entry.path(), destination,
                                      std::filesystem::copy_options::recursive, copy_ec);
            } else {
                std::filesystem::copy_file(entry.path(), destination, copy_ec);
            }
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
bool draw_syntax_colors(EditorSettings& e, const ResolvedTheme& theme, const ThemeInk& ink) {
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

    const bool any_pinned =
        std::any_of(e.syntax_pinned.begin(), e.syntax_pinned.end(), [](bool p) { return p; });
    {
        FontScope small(nullptr, 12.0f);
        ImGui::TextDisabled(any_pinned ? "* marks a color you set yourself"
                                       : "every color follows the theme");
    }

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
        // Each kind's name in its own colour, the way it will read in the
        // editor, so the list doubles as a sample of the palette.
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF,
                                                      rgb & 0xFF, 0xFF));
        const std::string id = left_label(label.c_str(), design_px(96.0f));
        ImGui::PopStyleColor();
        bool edited = false;
        {
            MonoScope mono(12.0f);
            edited = color_field(id.c_str(), color, 3);
        }
        if (edited) {
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
            if (ghost_button("revert", ink)) {
                e.syntax_pinned[i] = false;
                e.syntax_colors.rgb[i] = theme.syntax[i] >> 8;
                changed = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Follow the theme again.");
        }
        ImGui::PopID();
    }
    return changed;
}

/// "Reset all to theme", as the accent-coloured text link it is in the syntax
/// heading. Separate from the list so the heading can carry it.
bool draw_reset_syntax(EditorSettings& e, const ResolvedTheme& theme, const ThemeInk& ink) {
    const bool any_pinned =
        std::any_of(e.syntax_pinned.begin(), e.syntax_pinned.end(), [](bool p) { return p; });
    ImGui::BeginDisabled(!any_pinned);
    ImGui::PushStyleColor(ImGuiCol_Button, with_alpha(ink.raised, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ink.accent_muted);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ink.accent_muted);
    ImGui::PushStyleColor(ImGuiCol_Text, ink.accent_ink);
    FontScope small(nullptr, 12.0f);
    const bool pressed = ImGui::SmallButton("Reset all to theme");
    ImGui::PopStyleColor(4);
    ImGui::EndDisabled();
    if (!pressed) return false;
    for (std::size_t i = 0; i < kTokenKindCount; ++i) {
        e.syntax_pinned[i] = false;
        e.syntax_colors.rgb[i] = theme.syntax[i] >> 8;
    }
    return true;
}

/// One theme as a card: a small drawing of it - its ground, two panels on it, a
/// few lines of text and an accent button - over its name and where it comes
/// from. Drawn from the theme's own resolved roles, so a pack needs no picture
/// of itself to be shown here. Returns true when the card is clicked.
bool theme_card(const ResolvedTheme& preview, const std::string& meta, bool selected, float width,
                const ThemeInk& ink) {
    const float pad = design_px(6.0f);
    const float picture_height = design_px(70.0f);
    float name_height = 0.0f;
    float meta_height = 0.0f;
    {
        FontScope name_font(nullptr, 12.5f);
        name_height = ImGui::GetTextLineHeight();
    }
    {
        FontScope meta_font(nullptr, 11.0f);
        meta_height = ImGui::GetTextLineHeight();
    }
    const float height = pad + picture_height + design_px(8.0f) + name_height + meta_height +
                         design_px(10.0f);

    const ImVec2 min = ImGui::GetCursorScreenPos();
    ImGui::PushID(preview.id.c_str());
    const bool clicked = ImGui::InvisibleButton("##card", ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    const ImVec2 max(min.x + width, min.y + height);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const float rounding = design_px(8.0f);
    draw_list->AddRectFilled(min, max,
                             ImGui::GetColorU32(hovered ? ImGuiCol_HeaderHovered : ImGuiCol_Header),
                             rounding);
    if (selected) {
        draw_list->AddRect(min, max, ink.accent_ink, rounding, 0, 2.0f);
    } else {
        draw_list->AddRect(min, max, hovered ? ink.strong : ink.subtle, rounding);
    }

    // The miniature: the ground, a wide panel with text and a button on it,
    // and a narrow one beside it.
    const auto role = [&preview](const char* name) { return theme_u32(preview.role(name)); };
    const ImVec2 picture_min(min.x + pad, min.y + pad);
    const ImVec2 picture_max(max.x - pad, picture_min.y + picture_height);
    draw_list->AddRectFilled(picture_min, picture_max, role("surface.base"), design_px(5.0f));
    const float inset = design_px(4.0f);
    const float gap = design_px(3.0f);
    const float inner_width = picture_max.x - picture_min.x - inset * 2.0f - gap;
    const ImVec2 wide_min(picture_min.x + inset, picture_min.y + inset);
    const ImVec2 wide_max(wide_min.x + inner_width * 2.0f / 3.0f, picture_max.y - inset);
    const ImVec2 narrow_min(wide_max.x + gap, wide_min.y);
    const ImVec2 narrow_max(picture_max.x - inset, wide_max.y);
    draw_list->AddRectFilled(wide_min, wide_max, role("surface.raised"), design_px(3.0f));
    draw_list->AddRectFilled(narrow_min, narrow_max, role("surface.raised"), design_px(3.0f));
    const ImU32 ink_color = role("ink.primary");
    const float line = design_px(3.0f);
    const float panel_width = wide_max.x - wide_min.x - design_px(12.0f);
    const float fractions[3] = {0.6f, 0.8f, 0.4f};
    for (int i = 0; i < 3; ++i) {
        const float y = wide_min.y + design_px(6.0f) + static_cast<float>(i) * (line + design_px(4.0f));
        draw_list->AddRectFilled(ImVec2(wide_min.x + design_px(6.0f), y),
                                 ImVec2(wide_min.x + design_px(6.0f) + panel_width * fractions[i], y + line),
                                 i == 0 ? ink_color : with_alpha(ink_color, 0.5f), design_px(2.0f));
    }
    draw_list->AddRectFilled(ImVec2(wide_min.x + design_px(6.0f), wide_max.y - design_px(14.0f)),
                             ImVec2(wide_min.x + design_px(34.0f), wide_max.y - design_px(6.0f)),
                             role("accent"), design_px(2.0f));

    // The name and where the theme comes from.
    const float text_x = min.x + pad + design_px(4.0f);
    const float text_width_max = width - (pad + design_px(4.0f)) * 2.0f;
    float y = picture_max.y + design_px(8.0f);
    {
        FontScope name_font(nullptr, 12.5f);
        const std::string name = fit_text(display_case(preview.name), text_width_max);
        draw_list->AddText(ImVec2(text_x, y), ink.text, name.c_str());
        y += ImGui::GetTextLineHeight();
    }
    {
        FontScope meta_font(nullptr, 11.0f);
        const std::string line_text = fit_text(meta, text_width_max);
        draw_list->AddText(ImVec2(text_x, y), ink.muted, line_text.c_str());
    }
    return clicked;
}

/// The Theme page: every installed theme as a card, the active one's details,
/// its roles, and the syntax colours - the user's own over the theme's.
///
/// Ids rather than display names in the settings file, so the cards show the
/// name and record the id. A theme that is not installed still says so, rather
/// than silently becoming dark - a settings file that travelled from another
/// machine should say what it is missing.
bool draw_theme_page(App& app, EditorSettings& e, bool& reload_themes, bool& open_folder,
                     std::filesystem::path& show_pack_folder, const ThemeInk& ink) {
    bool changed = false;
    const auto& packs = app.theme_packs();
    const std::vector<ResolvedTheme>& previews = app.theme_previews();
    const ImGuiStyle& style = ImGui::GetStyle();

    // --- the heading, and the two things done to the whole list ----------
    {
        FontScope heading(nullptr, 16.0f);
        ImGui::AlignTextToFramePadding();
        colored_text("Theme", ink.text);
    }
    {
        FontScope small(nullptr, 12.0f);
        char count[64];
        std::snprintf(count, sizeof(count), "%zu themes \xC2\xB7 %zu pack%s", previews.size(),
                      packs.size(), packs.size() == 1 ? "" : "s");
        const float actions = text_width(count) + style.ItemSpacing.x + button_width("Reload themes") +
                              style.ItemSpacing.x + button_width("Open folder");
        same_line_right_aligned(actions);
        ImGui::AlignTextToFramePadding();
        colored_text(count, ink.muted);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload themes")) reload_themes = true;
    ImGui::SameLine();
    if (ImGui::Button("Open folder")) open_folder = true;
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Opens your themes folder, beside your settings file.\n"
                          "The first time, the themes that came with the app are copied "
                          "in for you\nto read and edit - including showcase.s3theme, "
                          "which lists every option.\n"
                          "Copy one, rename it, then press Reload themes. The file name "
                          "is the\ntheme's name.");
    }
    ImGui::Spacing();

    // --- the cards ---------------------------------------------------------
    const float gap = design_px(10.0f);
    const float room = ImGui::GetContentRegionAvail().x;
    const int columns =
        std::clamp(static_cast<int>((room + gap) / (design_px(132.0f) + gap)), 1, 6);
    const float card_width = (room - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    bool installed = false;
    for (std::size_t i = 0; i < previews.size(); ++i) {
        const ResolvedTheme& preview = previews[i];
        if (i % static_cast<std::size_t>(columns) != 0) ImGui::SameLine(0.0f, gap);
        std::string meta = "built-in";
        std::string description;
        if (const auto it = packs.find(preview.id); it != packs.end()) {
            meta = "pack \xC2\xB7 inherits " + it->second.inherit;
            description = it->second.description;
        }
        const bool selected = preview.id == e.color_theme;
        installed = installed || selected;
        if (theme_card(preview, meta, selected, card_width, ink) && !selected) {
            e.color_theme = preview.id;
            changed = true;
        }
        if (!description.empty()) ImGui::SetItemTooltip("%s", description.c_str());
    }
    if (!installed) {
        ImGui::TextDisabled("'%s' is not installed; showing dark until it is.",
                            display_case(e.color_theme).c_str());
    }

    // Where the active pack came from, click to copy. Three directories are
    // searched and only one of them is the folder the button above opens, so
    // "which file am I actually looking at" is a question this page should
    // answer rather than leave to be guessed.
    if (const auto it = packs.find(e.color_theme); it != packs.end()) {
        std::string credit = it->second.author;
        if (!it->second.version.empty()) {
            credit += credit.empty() ? it->second.version : " - " + it->second.version;
        }
        FontScope small(nullptr, 12.0f);
        if (!it->second.description.empty()) ImGui::TextWrapped("%s", it->second.description.c_str());
        if (!credit.empty()) ImGui::TextDisabled("%s", credit.c_str());
        const std::filesystem::path folder = it->second.file.parent_path();
        {
            MonoScope mono(11.0f);
            copyable_path(folder.generic_string());
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Show")) show_pack_folder = folder;
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Opens the folder this theme was read from.");
        }
    }
    ImGui::Spacing();

    // --- the reload option and the lint verdict, in one bordered row --------
    {
        const ImVec2 box_min = ImGui::GetCursorScreenPos();
        const float box_width = ImGui::GetContentRegionAvail().x;
        const float pad = design_px(10.0f);
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        draw_list->ChannelsSplit(2);
        draw_list->ChannelsSetCurrent(1);
        ImGui::SetCursorScreenPos(ImVec2(box_min.x + pad, box_min.y + pad));
        changed |= ImGui::Checkbox("Reload the theme when the window regains focus",
                                   &e.reload_theme_on_focus);
        if (ImGui::IsItemHovered()) {
            // Four lines because each one answers a question this setting
            // actually raises - when it acts, what it costs, what it misses,
            // what happens when the edit is wrong. Why it is off by default
            // belongs next to the field in settings.h, not here.
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

        // The contrast advice the command line's --lint gives, for the theme
        // on screen - the moment someone editing a pack wants it.
        const Diagnostics findings = lint_theme(app.theme());
        FontScope small(nullptr, 12.0f);
        std::string verdict = findings.empty()
                                  ? std::string("Lint clean \xC2\xB7 0 findings")
                                  : std::to_string(findings.size()) +
                                        (findings.size() == 1 ? " lint finding" : " lint findings");
        std::string file;
        if (const auto it = packs.find(app.theme().id); it != packs.end()) {
            file = (it->second.dir.empty() ? it->second.file : it->second.dir).filename().string();
        }
        float right = design_px(6.0f) + design_px(6.0f) + text_width(verdict.c_str());
        float file_width = 0.0f;
        if (!file.empty()) {
            MonoScope mono(11.0f);
            file_width = text_width(file.c_str());
            right += design_px(16.0f) + file_width;
        }
        ImGui::SameLine();
        const float right_x = box_min.x + box_width - pad - right;
        if (right_x > ImGui::GetCursorScreenPos().x) {
            ImGui::SetCursorScreenPos(ImVec2(right_x, ImGui::GetCursorScreenPos().y));
        }
        ImGui::AlignTextToFramePadding();
        const ImU32 verdict_color = findings.empty() ? ink.ok : ink.warn;
        status_dot(verdict_color);
        ImGui::SameLine(0.0f, design_px(6.0f));
        colored_text(verdict, verdict_color);
        if (!findings.empty() && ImGui::IsItemHovered()) {
            std::string list;
            for (const auto& finding : findings) list += finding.message + "\n";
            ImGui::SetTooltip("%s", list.c_str());
        }
        if (!file.empty()) {
            ImGui::SameLine(0.0f, design_px(16.0f));
            MonoScope mono(11.0f);
            colored_text(file, ink.muted);
        }
        const ImVec2 box_max(box_min.x + box_width, ImGui::GetItemRectMax().y + pad);
        draw_list->ChannelsSetCurrent(0);
        draw_list->AddRect(box_min, box_max, ink.subtle, design_px(6.0f));
        draw_list->ChannelsMerge();
        ImGui::SetCursorScreenPos(ImVec2(box_min.x, box_max.y));
        ImGui::Dummy(ImVec2(box_width, 0.0f));
    }
    ImGui::Spacing();

    // --- roles and syntax, side by side when there is room ----------------
    const bool side_by_side = ImGui::GetContentRegionAvail().x > design_px(620.0f);
    if (ImGui::BeginTable("##theme_columns", side_by_side ? 2 : 1,
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("##roles", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        if (side_by_side) ImGui::TableSetupColumn("##syntax", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();

        // The twenty roles, as this theme resolved them.
        {
            caps_label("Roles", ink);
            std::string set = "built-in";
            if (const auto it = packs.find(app.theme().id); it != packs.end()) {
                set = std::to_string(it->second.roles.size()) + " of " +
                      std::to_string(theme_role_names().size()) + " set by pack";
            }
            FontScope small(nullptr, 12.0f);
            same_line_right_aligned(text_width(set.c_str()));
            colored_text(set, ink.muted);
        }
        ImGui::PushStyleColor(ImGuiCol_TableBorderLight, ink.subtle);
        if (ImGui::BeginTable("##roles", 2,
                              ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerH)) {
            for (const std::string& name : theme_role_names()) {
                ImGui::TableNextColumn();
                const Rgba value = app.theme().role(name);
                const float swatch = design_px(16.0f);
                const ImVec2 at = ImGui::GetCursorScreenPos();
                ImDrawList* draw_list = ImGui::GetWindowDrawList();
                draw_list->AddRectFilled(at, ImVec2(at.x + swatch, at.y + swatch), theme_u32(value),
                                         design_px(4.0f));
                draw_list->AddRect(at, ImVec2(at.x + swatch, at.y + swatch),
                                   with_alpha(ink.text, 0.12f), design_px(4.0f));
                ImGui::Dummy(ImVec2(swatch, swatch));
                ImGui::SameLine(0.0f, design_px(10.0f));
                MonoScope mono(12.0f);
                colored_text(name, ink.text);
                const std::string hex = color_rgba_to_hex(value);
                MonoScope small(11.0f);
                same_line_right_aligned(text_width(hex.c_str()) + design_px(4.0f));
                colored_text(hex, ink.muted);
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleColor();

        // The syntax colours: which the theme sets, which the user pinned.
        ImGui::TableNextColumn();
        if (!side_by_side) ImGui::Spacing();
        caps_label("Syntax", ink);
        {
            FontScope small(nullptr, 12.0f);
            same_line_right_aligned(button_width("Reset all to theme"));
        }
        changed |= draw_reset_syntax(e, app.theme(), ink);
        changed |= ImGui::Checkbox("Color keywords and types", &e.syntax_highlight);
        ImGui::BeginDisabled(!e.syntax_highlight);
        changed |= draw_syntax_colors(e, app.theme(), ink);
        ImGui::EndDisabled();
        ImGui::EndTable();
    }
    return changed;
}

bool draw_editor_settings(EditorSettings& e) {
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

namespace {

/// One entry in the settings list: its name, and the words someone might type
/// looking for something on it.
struct SettingsPage {
    const char* name;
    const char* keywords;
};

constexpr SettingsPage kPages[] = {
    {"Editor", "font size zoom file tab width spaces wrap line numbers whitespace inline "
               "diagnostics indent compile typing debounce errors save completions warnings "
               "snapshot history"},
    {"Preview", "backend vsync fps frame rate window stats pixel inspector background "
                "checkerboard color transparent"},
    {"Files", "projects folder naming extensions manifest ambiguous autosave interval watch "
              "external changes"},
    {"Tools", "toolchain shadercross dxc glslang xcrun metallib threads cache shared asset "
              "backend"},
    {"Languages", "hlsl glsl default language profile vertex fragment compute include "
                  "directories"},
    {"Formats", "pack container magic extension header preset entry order alignment keys "
                "blobs table names reflection user section signature"},
    {"UI", "interface scale text size font larger smaller zoom multi viewport windows "
           "register hints confirm closing unsaved session reopen shortcuts"},
    {"Theme", "theme colors colours packs reload folder focus lint roles syntax palette "
              "keywords highlight pinned reset"},
};

/// Positions in kPages, for the page switch below.
enum SettingsPageIndex : int {
    kEditorPage,
    kPreviewPage,
    kFilesPage,
    kToolsPage,
    kLanguagesPage,
    kFormatsPage,
    kUiPage,
    kThemePage,
};
static_assert(std::string_view(kPages[kThemePage].name) == "Theme",
              "SettingsPageIndex has to follow the order of kPages");

/// Whether a page answers what was typed into the search field: its name or
/// one of its keywords contains it, ignoring case.
bool page_matches(const SettingsPage& page, const char* query) {
    if (query[0] == '\0') return true;
    std::string needle(query);
    for (char& c : needle) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string haystack = std::string(page.name) + " " + page.keywords;
    for (char& c : haystack) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return haystack.find(needle) != std::string::npos;
}

/// One row of the settings list. The selected page is drawn as a selection -
/// the theme's select.bg behind it and its select.ink for the name, so a theme
/// decides how it reads - with a bar of the accent's ink down its left edge.
/// Returns true when clicked.
bool settings_nav_item(const char* name, bool selected, const ThemeInk& ink) {
    const float height = design_px(30.0f);
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(name, ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 max(min.x + width, min.y + height);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const float rounding = design_px(5.0f);
    if (selected) {
        draw_list->AddRectFilled(min, max, ink.select_bg, rounding);
        draw_list->AddRectFilled(min, ImVec2(min.x + design_px(2.0f), max.y), ink.accent_ink);
    } else if (hovered) {
        draw_list->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_HeaderHovered), rounding);
    }
    draw_list->AddText(ImVec2(min.x + design_px(12.0f), min.y + (height - ImGui::GetTextLineHeight()) * 0.5f),
                       selected ? ink.select_ink : (hovered ? ink.text : ink.soft), name);
    return clicked;
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

    // Wide enough for the page list and a page beside it the first time it
    // opens; a size the user chose afterwards is kept.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(std::min(960.0f, viewport->WorkSize.x * 0.7f),
                                    std::min(700.0f, viewport->WorkSize.y * 0.8f)),
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

    // No padding of its own: the page list runs to the window's edges, and the
    // page beside it keeps its own margins.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    PanelScope panel("Settings", &app.show_settings, ImGuiWindowFlags_NoDocking);
    ImGui::PopStyleVar();
    if (!panel) return;

    const ThemeInk ink(app.theme());
    AppSettings& settings = app.settings();
    bool changed = false;
    bool theme_changed = false;
    bool reload_themes = false;
    bool open_folder = false;
    std::filesystem::path show_pack_folder;

    // Which page is showing. Kept for the session, like the page a tab bar
    // would have remembered.
    static int page = 0;
    static char search[64] = "";

    // --- the page list ---------------------------------------------------
    const float nav_width = std::min(design_px(190.0f), ImGui::GetContentRegionAvail().x * 0.35f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetColorU32(ImGuiCol_TitleBg));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(design_px(8.0f), design_px(12.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(ImGui::GetStyle().ItemSpacing.x, design_px(2.0f)));
    ImGui::BeginChild("##settings_nav", ImVec2(nav_width, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##settings_search", "Search settings", search, sizeof(search));
    ImGui::Dummy(ImVec2(0.0f, design_px(6.0f)));
    int shown = 0;
    for (int i = 0; i < static_cast<int>(std::size(kPages)); ++i) {
        if (!page_matches(kPages[i], search)) continue;
        ++shown;
        if (settings_nav_item(kPages[i].name, page == i, ink)) page = i;
    }
    if (shown == 0) {
        FontScope small(nullptr, 12.0f);
        ImGui::TextDisabled("Nothing matches.");
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    // The hairline between the list and the page.
    {
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(max.x, min.y), ImVec2(max.x, max.y), ink.subtle);
    }

    // --- the page --------------------------------------------------------
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(design_px(22.0f), design_px(18.0f)));
    ImGui::BeginChild("##settings_page", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    // A child window keeps its own wrap position; the panel's does not reach
    // in here. Pushed and popped by hand rather than by a scope, because the
    // pop has to happen inside the child, before EndChild().
    ImGui::PushTextWrapPos(0.0f);

    // Every page but Theme opens with its name, the way Theme's own heading
    // does; Theme draws its heading itself, with its actions beside it.
    if (page != kThemePage) {
        FontScope heading(nullptr, 16.0f);
        colored_text(kPages[page].name, ink.text);
        ImGui::Spacing();
    }

    switch (page) {
        case kEditorPage: {
            const std::string font_before = settings.editor.font_path;
            changed |= draw_editor_settings(settings.editor);
            // A new font file is loaded between frames, the way a theme's is.
            if (settings.editor.font_path != font_before) app.request_fonts();
            break;
        }
        case kPreviewPage:
            changed |= draw_preview_settings(settings.preview);
            break;
        case kFilesPage:
            changed |= draw_file_settings(settings.files, app.projects_dir().generic_string());
            break;
        case kToolsPage:
            ImGui::TextDisabled("current backend: %s", app.backend_name().c_str());
            changed |= draw_tool_settings(settings.tools);
            break;
        case kLanguagesPage:
            changed |= draw_language_settings(settings.languages);
            break;
        case kFormatsPage:
            changed |= draw_pack_layout_settings(settings.pack_layout, app.theme());
            break;
        case kUiPage: {
            // The scale lives in the style, which is only rebuilt by
            // apply_theme - so a new scale has to ask for that rebuild, the
            // same as changing the theme does. Both sliders on this page resize
            // the window they are in, so both apply when they are let go (see
            // slider_applied_on_release).
            //
            // Both sliders share a label column and a width, measured from the
            // room there is: wide enough to drag with precision, and never so
            // wide that the button beside the second one is pushed out of the
            // window.
            const ImGuiStyle& style = ImGui::GetStyle();
            const char* reset_label = "Use the theme's size";
            const float column =
                std::max(text_width("UI scale"), text_width("Text size")) + style.ItemSpacing.x * 2.0f;
            const float reset_width = button_width(reset_label);
            const float room = ImGui::GetContentRegionAvail().x - column;
            const float slider_width = std::clamp(room - style.ItemSpacing.x - reset_width,
                                                  std::min(room, design_px(140.0f)),
                                                  design_px(420.0f));
            const bool reset_fits = slider_width + style.ItemSpacing.x + reset_width <= room;
            const float page_left = ImGui::GetCursorPosX();

            if (slider_applied_on_release("UI scale", column, slider_width, settings.ui.ui_scale,
                                          [](const char* id, float* v) {
                                              ImGui::SliderFloat(id, v, 0.75f, 2.0f, "%.2f");
                                          })) {
                changed = true;
                theme_changed = true;
            }
            ImGui::SetItemTooltip(
                "Everything larger or smaller: text, spacing and controls.\n"
                "Takes effect when you let go of the slider.");

            // The text alone. Shown at the size in effect, so the slider starts
            // where the text already is; moving it makes the size the user's own,
            // which then outlasts a change of theme, and the button beside it
            // hands the choice back to the theme.
            // What the theme would give with no size of the user's own: its
            // suggestion, or the default when it makes none.
            const float theme_size = ui_font_size_in_effect(0.0f, app.theme().font_ui_size);
            //
            // A float slider shown in whole pixels rather than an integer one:
            // ImGui makes an integer slider's grab one step wide, which over a
            // range this short covers the number it is meant to be showing. The
            // value is rounded when it is applied.
            float text_size = settings.ui.font_size > 0.0f ? settings.ui.font_size : theme_size;
            if (slider_applied_on_release("Text size", column, slider_width, text_size,
                                          [](const char* id, float* v) {
                                              ImGui::SliderFloat(id, v, kMinUiFontSize,
                                                                 kMaxUiFontSize, "%.0f px");
                                          })) {
                settings.ui.font_size = std::round(text_size);
                changed = true;
                theme_changed = true;
            }
            ImGui::SetItemTooltip(
                "The size of the interface's text, without enlarging anything else.\n"
                "UI scale above still multiplies it. The editor has a size of its own,\n"
                "on the Editor page. Takes effect when you let go of the slider.");
            // Beside the slider while there is room for it, under it when the
            // window is too narrow - never cut off at the window's edge.
            if (reset_fits) {
                ImGui::SameLine();
            } else {
                ImGui::SetCursorPosX(page_left + column);
            }
            ImGui::BeginDisabled(settings.ui.font_size <= 0.0f);
            if (ImGui::SmallButton(reset_label)) {
                settings.ui.font_size = 0.0f;
                changed = true;
                theme_changed = true;
            }
            ImGui::EndDisabled();
            {
                // Under the slider it describes, not under its label.
                FontScope small(nullptr, 12.0f);
                ImGui::SetCursorPosX(page_left + column);
                if (settings.ui.font_size > 0.0f) {
                    ImGui::TextDisabled("Your own size. The theme suggests %.0f px.", theme_size);
                } else {
                    ImGui::TextDisabled("The theme's size. Move the slider to choose your own.");
                }
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
            // A table rather than padded text: the interface font is
            // proportional, so spaces cannot line the keys up in a column. No
            // wrapping inside it - the page wraps at its edge, and a column
            // sized to fit its text would otherwise be measured from text
            // already wrapped to that column, and shrink to a letter wide.
            ImGui::PushTextWrapPos(-1.0f);
            if (ImGui::BeginTable("##shortcuts", 2, ImGuiTableFlags_SizingFixedFit)) {
                for (const auto& action : app.actions()) {
                    if (action.shortcut.empty()) continue;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(action.label.c_str());
                    ImGui::TableNextColumn();
                    mono_text(action.shortcut, ink.soft, 12.0f);
                }
                ImGui::EndTable();
            }
            ImGui::PopTextWrapPos();
            break;
        }
        case kThemePage:
        default: {
            const std::string before = settings.editor.color_theme;
            changed |= draw_theme_page(app, settings.editor, reload_themes, open_folder,
                                       show_pack_folder, ink);
            theme_changed = settings.editor.color_theme != before;
            break;
        }
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();

    // All after the page, like every other action this panel collects: the
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
