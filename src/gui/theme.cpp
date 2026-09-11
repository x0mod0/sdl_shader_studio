// Visual styling. Kept apart from panel code so a theme change never touches
// behaviour.
#include <array>
#include <string>
#include <unordered_map>

#include <imgui.h>

#include "app.h"
#include "panels/panel_common.h"

namespace ssstudio::gui {
namespace {

/// ImGui's colour index for a name, or -1.
///
/// Built by asking ImGui for its own names rather than from a table of our own:
/// GetStyleColorName() comes from the ImGui this build links, so the mapping
/// cannot disagree with it, and a colour added or renamed upstream needs no
/// change here.
int imgui_color_index(const std::string& name) {
    static const std::unordered_map<std::string, int> kIndex = [] {
        std::unordered_map<std::string, int> map;
        for (int i = 0; i < ImGuiCol_COUNT; ++i) {
            if (const char* label = ImGui::GetStyleColorName(i)) map.emplace(label, i);
        }
        return map;
    }();
    const auto it = kIndex.find(name);
    return it == kIndex.end() ? -1 : it->second;
}

ImGuiDir direction_from(const std::string& name, ImGuiDir fallback) {
    if (name == "none") return ImGuiDir_None;
    if (name == "left") return ImGuiDir_Left;
    if (name == "right") return ImGuiDir_Right;
    if (name == "up") return ImGuiDir_Up;
    if (name == "down") return ImGuiDir_Down;
    return fallback;
}

/// The app's own geometry, applied to every theme. A pack overrides any of it
/// through [style]; these are what it starts from.
void apply_default_metrics(ImGuiStyle& style) {
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.TabRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.ScrollbarRounding = 3.0f;
    style.WindowPadding = ImVec2(10, 10);
    style.FramePadding = ImVec2(7, 4);
    style.ItemSpacing = ImVec2(8, 6);
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.SeparatorTextBorderSize = 1.0f;
}

/// The dark theme's own colours, which are ImGui's dark style with the handful
/// of changes the app has always made to it.
void apply_builtin_dark(ImGuiStyle& style) {
    ImGui::StyleColorsDark();
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.09f, 0.10f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.11f, 0.11f, 0.12f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.17f, 0.20f, 1.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.16f, 0.18f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.20f, 0.22f, 0.27f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.20f, 0.22f, 0.27f, 1.00f);
    colors[ImGuiCol_Tab] = ImVec4(0.13f, 0.14f, 0.16f, 1.00f);
    colors[ImGuiCol_TabSelected] = ImVec4(0.22f, 0.24f, 0.30f, 1.00f);
    colors[ImGuiCol_DockingPreview] = ImVec4(0.35f, 0.50f, 0.75f, 0.70f);
}

/// A pack's sixty-three colours, straight onto the style.
///
/// ImGui's dark style goes down first so that a colour this build knows about
/// and the resolver does not still has a value - a new ImGuiCol_ arriving
/// upstream leaves a gap here, not a black rectangle.
void apply_pack_colors(ImGuiStyle& style, const ResolvedTheme& theme) {
    ImGui::StyleColorsDark();
    for (const auto& [name, color] : theme.ui) {
        const int index = imgui_color_index("ImGuiCol_" + name);
        if (index >= 0) style.Colors[index] = theme_vec4(color);
    }
}

void apply_pack_metrics(ImGuiStyle& style, const ThemeStyle& metrics) {
    const auto scalar = [&](const char* name, float& field) {
        const auto it = metrics.scalar.find(name);
        if (it != metrics.scalar.end()) field = it->second;
    };
    const auto pair = [&](const char* name, ImVec2& field) {
        const auto it = metrics.vec2.find(name);
        if (it != metrics.vec2.end()) field = ImVec2(it->second[0], it->second[1]);
    };
    const auto dir = [&](const char* name, ImGuiDir& field) {
        const auto it = metrics.direction.find(name);
        if (it != metrics.direction.end()) field = direction_from(it->second, field);
    };

    scalar("alpha", style.Alpha);
    scalar("disabled_alpha", style.DisabledAlpha);
    pair("window_padding", style.WindowPadding);
    scalar("window_rounding", style.WindowRounding);
    scalar("window_border_size", style.WindowBorderSize);
    pair("window_min_size", style.WindowMinSize);
    pair("window_title_align", style.WindowTitleAlign);
    dir("window_menu_button_position", style.WindowMenuButtonPosition);
    scalar("child_rounding", style.ChildRounding);
    scalar("child_border_size", style.ChildBorderSize);
    scalar("popup_rounding", style.PopupRounding);
    scalar("popup_border_size", style.PopupBorderSize);
    pair("frame_padding", style.FramePadding);
    scalar("frame_rounding", style.FrameRounding);
    scalar("frame_border_size", style.FrameBorderSize);
    pair("item_spacing", style.ItemSpacing);
    pair("item_inner_spacing", style.ItemInnerSpacing);
    pair("cell_padding", style.CellPadding);
    scalar("indent_spacing", style.IndentSpacing);
    scalar("scrollbar_size", style.ScrollbarSize);
    scalar("scrollbar_rounding", style.ScrollbarRounding);
    scalar("grab_min_size", style.GrabMinSize);
    scalar("grab_rounding", style.GrabRounding);
    scalar("image_rounding", style.ImageRounding);
    scalar("tab_rounding", style.TabRounding);
    scalar("tab_border_size", style.TabBorderSize);
    scalar("tab_bar_border_size", style.TabBarBorderSize);
    scalar("tab_bar_overline_size", style.TabBarOverlineSize);
    scalar("menu_item_rounding", style.MenuItemRounding);
    scalar("separator_size", style.SeparatorSize);
    scalar("separator_text_border_size", style.SeparatorTextBorderSize);
    scalar("tree_lines_size", style.TreeLinesSize);
    scalar("tree_lines_rounding", style.TreeLinesRounding);
    pair("button_text_align", style.ButtonTextAlign);
    pair("selectable_text_align", style.SelectableTextAlign);
    scalar("input_text_cursor_size", style.InputTextCursorSize);
    dir("color_button_position", style.ColorButtonPosition);
}

}  // namespace

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------
void apply_theme(const ResolvedTheme& theme, float ui_scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    // Back to ImGui's defaults first. ScaleAllSizes() below multiplies whatever
    // is already there, so re-applying a theme without this would compound the
    // scale - and the scale slider re-applies it on every frame it is dragged.
    style = ImGuiStyle();

    // The three built-in themes are still applied the way they always were,
    // through ImGui's own styles. A pack's colours are derived from its roles
    // instead; keeping the two paths apart is what lets packs exist without
    // changing what dark, light and classic look like.
    if (theme.builtin) {
        if (theme.id == "light") {
            ImGui::StyleColorsLight();
        } else if (theme.id == "classic") {
            ImGui::StyleColorsClassic();
        } else {
            apply_builtin_dark(style);
        }
    } else {
        apply_pack_colors(style, theme);
    }

    apply_default_metrics(style);
    if (!theme.builtin) apply_pack_metrics(style, theme.style);

    // A dock node draws its own close button at the right of the tab bar, on top
    // of the close button each tab already has. Two buttons a few pixels apart
    // that close different amounts - the node one closes every tab in the node -
    // is a trap, so only the per-tab one stays. Not a look, so not something a
    // pack may set.
    style.DockingNodeHasCloseButton = false;

    if (ui_scale > 0.0f && ui_scale != 1.0f) {
        style.ScaleAllSizes(ui_scale);
        // The text as well as the spacing around it. ScaleAllSizes only touches
        // sizes, so on its own a "UI scale" moved the padding and left every
        // word on screen exactly as small as it was.
        style.FontScaleMain = ui_scale;
    }
}

}  // namespace ssstudio::gui
