// The window's chrome: the bar across the top and the bar along the bottom.
//
// Both are viewport side bars rather than parts of the dock host. ImGui takes a
// side bar's height out of the viewport's work area, so the host - and every
// panel docked in it - is laid out in what is left, and neither bar can ever be
// covered by a panel or docked into.
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>  // BeginViewportSideBar, and the menu bar's rectangle

#include "app.h"
#include "native_menu.h"
#include "panels/panel_common.h"
#include "preview/renderer.h"
#include "widgets.h"

namespace ssstudio::gui {
namespace {

/// The application's mark, drawn rather than loaded: three overlapping discs on
/// a dark tile, as the icon has them. Drawing it costs a few triangles; loading
/// the PNG would mean shipping it beside the executable and keeping a texture
/// alive for eighteen pixels of chrome. Its colours are the icon's, not the
/// theme's - it is a logo, and a logo does not change with the theme.
void draw_app_mark(ImDrawList* draw_list, const ImVec2& min, float size) {
    draw_list->AddRectFilled(min, ImVec2(min.x + size, min.y + size), IM_COL32(38, 27, 30, 255),
                             size * 0.24f);
    const float radius = size * 0.27f;
    const auto disc = [&](float fx, float fy, ImU32 color) {
        draw_list->AddCircleFilled(ImVec2(min.x + size * fx, min.y + size * fy), radius, color, 24);
    };
    disc(0.37f, 0.40f, IM_COL32(255, 170, 72, 220));
    disc(0.63f, 0.40f, IM_COL32(246, 108, 204, 200));
    disc(0.50f, 0.63f, IM_COL32(255, 112, 106, 200));
}

/// A cross drawn with two lines, for the close button on a pill. Drawn rather
/// than typed so it looks the same in every font, including the built-in one.
void draw_cross(ImDrawList* draw_list, const ImVec2& center, float half, ImU32 color) {
    draw_list->AddLine(ImVec2(center.x - half, center.y - half),
                       ImVec2(center.x + half, center.y + half), color, 1.2f);
    draw_list->AddLine(ImVec2(center.x + half, center.y - half),
                       ImVec2(center.x - half, center.y + half), color, 1.2f);
}

/// A plus drawn with two lines, for the add button after the pills.
void draw_plus(ImDrawList* draw_list, const ImVec2& center, float half, ImU32 color) {
    draw_list->AddLine(ImVec2(center.x - half, center.y), ImVec2(center.x + half, center.y), color,
                       1.2f);
    draw_list->AddLine(ImVec2(center.x, center.y - half), ImVec2(center.x, center.y + half), color,
                       1.2f);
}

/// How a project is doing, as far as its pill's dot is concerned.
enum class ProjectHealth {
    /// Nothing has compiled yet, or there is nothing to compile.
    Idle,
    /// A compile is in flight; the last answer is about to be replaced.
    Compiling,
    /// Every shader compiled and nothing was said about any of them.
    Clean,
    /// Everything compiled, with warnings.
    Warnings,
    /// At least one shader does not compile.
    Errors,
};

/// Reads a project's shaders and says which of the states above it is in.
///
/// Each Document in session.documents carries what its last compile said:
/// `compiling`, `compiled_ok`, and `diagnostics` (each with a `severity`).
/// The pill shows the dot for the project in front always, and for a project
/// behind it only when this returns Warnings or Errors.
ProjectHealth project_health(const ProjectSession& session) {
    bool compiling = false;
    bool warnings = false;
    bool all_compiled = !session.documents.empty();
    for (const Document& doc : session.documents) {
        // A shader that failed outranks everything, including another one
        // still compiling: it is the one state that will not clear up on its
        // own, and the one a project behind the front one must still show.
        // Only a finished compile counts - one in flight still carries the
        // previous attempt's diagnostics, which are about to be replaced.
        if (!doc.compiling && !doc.compiled_ok && has_errors(doc.diagnostics)) {
            return ProjectHealth::Errors;
        }
        compiling = compiling || doc.compiling;
        all_compiled = all_compiled && doc.compiled_ok;
        warnings = warnings || std::any_of(doc.diagnostics.begin(), doc.diagnostics.end(),
                                           [](const Diagnostic& d) {
                                               return d.severity == Severity::Warning;
                                           });
    }
    if (compiling) return ProjectHealth::Compiling;
    if (warnings) return ProjectHealth::Warnings;
    // Clean only once every shader has compiled: a project half of whose
    // shaders have never been tried has not earned the green.
    return all_compiled ? ProjectHealth::Clean : ProjectHealth::Idle;
}

ImU32 health_color(ProjectHealth health, const ThemeInk& ink) {
    switch (health) {
        case ProjectHealth::Idle: return ink.muted;
        case ProjectHealth::Compiling: return ink.accent_ink;
        case ProjectHealth::Clean: return ink.ok;
        case ProjectHealth::Warnings: return ink.warn;
        case ProjectHealth::Errors: return ink.error;
    }
    return ink.muted;
}

/// "1 error", "2 errors".
std::string counted(int count, const char* singular, const char* plural) {
    return std::to_string(count) + " " + (count == 1 ? singular : plural);
}

/// The preview's binary format, the way people write it.
const char* format_display_name(ShaderFormat format) {
    switch (format) {
        case FORMAT_SPIRV: return "SPIR-V";
        case FORMAT_MSL: return "MSL";
        case FORMAT_METALLIB: return "Metal";
        case FORMAT_DXIL: return "DXIL";
        case FORMAT_DXBC: return "DXBC";
        case FORMAT_PRIVATE: return "private";
        case FORMAT_NONE: break;
    }
    return "none";
}

/// A path as it reads best in a narrow bar: under the home directory it is
/// spelled from "~", which is usually most of what an absolute path is.
std::string short_path(const std::filesystem::path& path) {
    const std::string full = path.generic_string();
    const char* home = SDL_getenv("HOME");
    if (home == nullptr || *home == '\0') return full;
    const std::string prefix = std::filesystem::path(home).generic_string();
    if (full.size() > prefix.size() && full.compare(0, prefix.size(), prefix) == 0 &&
        full[prefix.size()] == '/') {
        return "~" + full.substr(prefix.size());
    }
    return full;
}

}  // namespace

// ---------------------------------------------------------------------------
// Top bar
// ---------------------------------------------------------------------------
void App::draw_top_bar(const ThemeInk& ink) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float height = design_px(40.0f);
    const ImGuiStyle& style = ImGui::GetStyle();

    // A menu bar is as tall as the frame padding makes it: measured at Begin,
    // and used again by BeginMenuBar to centre the menu titles. The bar's own
    // padding is pushed across both, and popped before any menu can open, so
    // the menus themselves keep the theme's padding.
    const float pad_y = std::max(0.0f, std::floor((height - ImGui::GetFontSize()) * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, pad_y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(design_px(10.0f), 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ink.base);
    ImGui::PushStyleColor(ImGuiCol_MenuBarBg, ink.base);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
    const bool open = ImGui::BeginViewportSideBar("##topbar", viewport, ImGuiDir_Up, height, flags);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
    if (!open || !ImGui::BeginMenuBar()) {
        ImGui::PopStyleVar();
        ImGui::End();
        return;
    }
    ImGui::PopStyleVar();

    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImRect bar = window->MenuBarRect();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddLine(ImVec2(bar.Min.x, bar.Max.y - 0.5f), ImVec2(bar.Max.x, bar.Max.y - 0.5f),
                       with_alpha(ink.subtle, 0.8f));

    const float mark = design_px(18.0f);
    draw_app_mark(draw_list,
                  ImVec2(ImGui::GetCursorScreenPos().x, bar.Min.y + (bar.GetHeight() - mark) * 0.5f),
                  mark);
    ImGui::Dummy(ImVec2(mark, 0.0f));

    if (native_menu::available()) {
        // The menus are in the system menu bar (App::frame), so the pills
        // follow the mark directly.
        ImGui::Dummy(ImVec2(design_px(4.0f), 0.0f));
    } else {
        draw_menu_bar();

        // The rule between "what the app can do" and "what is open in it".
        const float x = std::floor(ImGui::GetCursorScreenPos().x + design_px(4.0f)) + 0.5f;
        const float rule = design_px(18.0f);
        const float y = bar.Min.y + (bar.GetHeight() - rule) * 0.5f;
        draw_list->AddLine(ImVec2(x, y), ImVec2(x, y + rule), ink.subtle);
        ImGui::Dummy(ImVec2(design_px(10.0f), 0.0f));
    }

    // The profile picker and the Build button are measured first, so the
    // pills know how far they may run before they have to fold into a list.
    const float gap = design_px(6.0f);
    float build_width = 0.0f;
    {
        const float build_label = ImGui::CalcTextSize("Build").x;
        float hint = 0.0f;
        {
            FontScope small(nullptr, 11.0f);
            hint = ImGui::CalcTextSize("Ctrl+B").x;
        }
        build_width = style.FramePadding.x * 2.0f + build_label + design_px(8.0f) + hint;
        if (project_open() && !project().profiles.empty()) {
            FontScope small(nullptr, 12.0f);
            float widest = ImGui::CalcTextSize("profile").x;
            for (const auto& profile : project().profiles) {
                widest = std::max(widest, ImGui::CalcTextSize(profile.name.c_str()).x);
            }
            build_width += gap + std::min(widest, design_px(140.0f)) + style.FramePadding.x * 2.0f +
                           ImGui::GetFrameHeight();
        }
    }
    const float build_left = bar.Max.x - design_px(10.0f) - build_width;

    draw_project_tabs(ink, build_left - design_px(12.0f));
    draw_top_bar_build(ink, build_left, bar.GetHeight());

    ImGui::EndMenuBar();
    ImGui::End();
}

void App::draw_top_bar_build(const ThemeInk& ink, float left, float height) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const float top = window->MenuBarRect().Min.y;
    const float gap = design_px(6.0f);
    float x = left;

    if (project_open() && !project().profiles.empty()) {
        // The same repair the Build panel makes: a profile named in one project
        // rarely exists in the next, and the choice is kept per project.
        std::string& profile_choice = build_profile_choice();
        auto& profiles = project().profiles;
        const bool known =
            std::any_of(profiles.begin(), profiles.end(),
                        [&](const BuildProfile& p) { return p.name == profile_choice; });
        if (!known) profile_choice = profiles.front().name;

        FontScope small(nullptr, 12.0f);
        float widest = ImGui::CalcTextSize("profile").x;
        for (const auto& profile : profiles) {
            widest = std::max(widest, ImGui::CalcTextSize(profile.name.c_str()).x);
        }
        const float combo_width = std::min(widest, design_px(140.0f)) +
                                  ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetFrameHeight();
        ImGui::SetCursorScreenPos(ImVec2(x, top + (height - ImGui::GetFrameHeight()) * 0.5f));
        ImGui::SetNextItemWidth(combo_width);
        ImGui::PushStyleColor(ImGuiCol_Text, ink.soft);
        const bool combo = ImGui::BeginCombo("##top_profile", profile_choice.c_str());
        ImGui::PopStyleColor();
        if (combo) {
            for (const auto& profile : profiles) {
                if (ImGui::Selectable(profile.name.c_str(), profile.name == profile_choice)) {
                    profile_choice = profile.name;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("Build profile");
        x = ImGui::GetItemRectMax().x + gap;
    }

    // Exactly what Ctrl+B does, so the button and the shortcut cannot come to
    // mean different builds.
    const bool can_build = project_open() && !build_busy_;
    ImGui::SetCursorScreenPos(ImVec2(x, top + (height - ImGui::GetFrameHeight()) * 0.5f));
    ImGui::BeginDisabled(!can_build);
    if (primary_button_with_hint("Build##top", "Ctrl+B", ink)) run_action("build.run");
    ImGui::EndDisabled();
    if (build_busy_ && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        // A build started from another tab says whose it is, so a busy Build
        // button explains itself.
        const std::string who = building_project_name();
        ImGui::SetTooltip("%s", who.empty() ? "Building..." : ("Building " + who + "...").c_str());
    }
}

void App::draw_project_tabs(const ThemeInk& ink, float right_edge) {
    if (sessions_.empty()) {
        pending_activate_key_.clear();
        return;
    }

    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImRect bar = window->MenuBarRect();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    const float pill_height = ImGui::GetFrameHeight() + design_px(2.0f);
    const float pad = design_px(10.0f);
    const float gap = design_px(8.0f);
    const float dot = design_px(6.0f);
    const float spacing = design_px(2.0f);
    const float close_size = std::round(ImGui::GetFontSize() * 0.8f);
    const float top = bar.Min.y + (bar.GetHeight() - pill_height) * 0.5f;
    const float button_size = pill_height;

    // The name is the label; the key is the identity. A project whose name
    // changes keeps its pill, and two projects that share a name still get one
    // each.
    struct Pill {
        int index = 0;
        std::string label;
        float width = 0.0f;
    };
    std::vector<Pill> pills;
    float total = 0.0f;
    for (std::size_t i = 0; i < sessions_.size(); ++i) {
        const ProjectSession& session = *sessions_[i];
        Pill pill;
        pill.index = static_cast<int>(i);
        pill.label = session.project.name.empty() ? "Untitled" : session.project.name;
        if (session.has_unsaved_changes()) pill.label += " *";
        // The close button's room is kept on every pill, shown or not, so a
        // pill does not change width - and push its neighbours about - as the
        // pointer crosses it.
        pill.width = pad + dot + gap + ImGui::CalcTextSize(pill.label.c_str()).x + gap +
                     close_size + pad * 0.6f;
        total += pill.width + spacing;
        pills.push_back(std::move(pill));
    }

    // What fits, in order, with the "+" always kept. When the row overflows,
    // a list button takes the place of the pills that did not fit - the same
    // job the tab bar's list button did - and the active project is always one
    // of the pills left showing.
    const float start = ImGui::GetCursorScreenPos().x;
    float room = right_edge - start - (button_size + spacing);
    std::vector<const Pill*> visible;
    bool overflow = false;
    if (total > room) {
        overflow = true;
        room -= button_size + spacing;
        float used = 0.0f;
        for (const Pill& pill : pills) {
            if (used + pill.width > room) break;
            visible.push_back(&pill);
            used += pill.width + spacing;
        }
        const Pill& active_pill = pills[static_cast<std::size_t>(std::max(0, active_session_))];
        const bool shown = std::any_of(visible.begin(), visible.end(),
                                       [&](const Pill* p) { return p == &active_pill; });
        if (!shown) {
            while (!visible.empty() && used + active_pill.width > room) {
                used -= visible.back()->width + spacing;
                visible.pop_back();
            }
            visible.push_back(&active_pill);
        }
    } else {
        for (const Pill& pill : pills) visible.push_back(&pill);
    }

    // Applied after the row is drawn: closing a session mid-loop would pull the
    // vector out from under it.
    int activate = -1;
    int close = -1;

    float x = start;
    for (const Pill* pill : visible) {
        const ProjectSession& session = *sessions_[static_cast<std::size_t>(pill->index)];
        const bool active = pill->index == active_session_;
        const ImVec2 min(x, top);
        const ImVec2 max(x + pill->width, top + pill_height);

        ImGui::SetCursorScreenPos(min);
        ImGui::PushID(session.key.c_str());
        const bool clicked = ImGui::InvisibleButton("##pill", ImVec2(pill->width, pill_height));
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 close_center(max.x - pad * 0.6f - close_size * 0.5f, min.y + pill_height * 0.5f);
        const bool show_close = active || hovered;
        const bool over_close =
            show_close && hovered &&
            ImGui::IsMouseHoveringRect(
                ImVec2(close_center.x - close_size * 0.5f, close_center.y - close_size * 0.5f),
                ImVec2(close_center.x + close_size * 0.5f, close_center.y + close_size * 0.5f));
        if (clicked) {
            if (over_close) {
                close = pill->index;
            } else {
                // Clicking a pill is how the UI changes project: everything the
                // panels draw comes from the active session.
                activate = pill->index;
            }
        }
        // Middle-click closes, as it did on the tabs these replace.
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) close = pill->index;
        if (hovered && !over_close) {
            ImGui::SetItemTooltip("%s", session.project.root.string().c_str());
        } else if (over_close) {
            ImGui::SetItemTooltip("Close %s", session.project.name.c_str());
        }
        ImGui::PopID();

        const float rounding = design_px(6.0f);
        if (active) {
            draw_list->AddRectFilled(min, max, ink.raised, rounding);
            draw_list->AddRect(min, max, ink.subtle, rounding);
        } else if (hovered) {
            draw_list->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_HeaderHovered), rounding);
        }

        const float center_y = min.y + pill_height * 0.5f;
        float cursor = min.x + pad;
        // A project in front always says how it is; one behind only speaks up
        // when something needs looking at.
        const ProjectHealth health = project_health(session);
        const bool loud = health == ProjectHealth::Errors || health == ProjectHealth::Warnings;
        if (active || loud) {
            draw_list->AddCircleFilled(ImVec2(cursor + dot * 0.5f, center_y), dot * 0.5f,
                                       health_color(health, ink));
        }
        cursor += dot + gap;
        draw_list->AddText(ImVec2(cursor, center_y - ImGui::GetTextLineHeight() * 0.5f),
                           active ? ink.text : ink.muted, pill->label.c_str());

        if (show_close) {
            if (over_close) {
                draw_list->AddCircleFilled(close_center, close_size * 0.55f,
                                           ImGui::GetColorU32(ImGuiCol_ButtonHovered));
            }
            draw_cross(draw_list, close_center, close_size * 0.22f, over_close ? ink.text : ink.muted);
        }
        x = max.x + spacing;
    }

    // The projects that did not fit, and the way to reach them.
    if (overflow) {
        const ImVec2 min(x, top);
        ImGui::SetCursorScreenPos(min);
        if (ImGui::InvisibleButton("##project_list", ImVec2(button_size, pill_height))) {
            ImGui::OpenPopup("##project_list_menu");
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::SetItemTooltip("All open projects");
        if (hovered) {
            draw_list->AddRectFilled(min, ImVec2(min.x + button_size, min.y + pill_height),
                                     ImGui::GetColorU32(ImGuiCol_HeaderHovered), design_px(6.0f));
        }
        const float arrow = ImGui::GetFontSize() * 0.6f;
        ImGui::RenderArrow(draw_list,
                           ImVec2(min.x + (button_size - arrow) * 0.5f,
                                  min.y + (pill_height - arrow) * 0.5f),
                           hovered ? ink.text : ink.muted, ImGuiDir_Down, 0.6f);
        if (ImGui::BeginPopup("##project_list_menu")) {
            for (const Pill& pill : pills) {
                ImGui::PushID(pill.index);
                if (ImGui::Selectable(pill.label.c_str(), pill.index == active_session_)) {
                    activate = pill.index;
                }
                ImGui::PopID();
            }
            ImGui::EndPopup();
        }
        x += button_size + spacing;
    }

    // Another project: the same two ways in as the File menu.
    {
        const ImVec2 min(x, top);
        ImGui::SetCursorScreenPos(min);
        if (ImGui::InvisibleButton("##add_project", ImVec2(button_size, pill_height))) {
            ImGui::OpenPopup("##add_project_menu");
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::SetItemTooltip("New or open a project");
        if (hovered) {
            draw_list->AddRectFilled(min, ImVec2(min.x + button_size, min.y + pill_height),
                                     ImGui::GetColorU32(ImGuiCol_HeaderHovered), design_px(6.0f));
        }
        draw_plus(draw_list, ImVec2(min.x + button_size * 0.5f, min.y + pill_height * 0.5f),
                  ImGui::GetFontSize() * 0.3f, hovered ? ink.text : ink.muted);
        if (ImGui::BeginPopup("##add_project_menu")) {
            if (ImGui::MenuItem("New project...", "Ctrl+N")) prompt_new_project();
            if (ImGui::MenuItem("Open project...", "Ctrl+O")) prompt_open_project();
            ImGui::EndPopup();
        }
    }

    // Nothing reads this any more - the pills show the active session directly
    // rather than being told which one to select - but the places that set it
    // still do, so it is emptied where the tab bar used to empty it.
    pending_activate_key_.clear();

    if (activate >= 0) activate_session(activate);
    if (close >= 0) request_close_session(close);
}

// ---------------------------------------------------------------------------
// Status bar
// ---------------------------------------------------------------------------
void App::draw_status_bar(const ThemeInk& ink) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float height = design_px(26.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(design_px(12.0f), 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ink.base);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav;
    const bool open =
        ImGui::BeginViewportSideBar("##statusbar", viewport, ImGuiDir_Down, height, flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    if (!open) {
        ImGui::End();
        return;
    }

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetWindowPos();
    const float width = ImGui::GetWindowWidth();
    draw_list->AddLine(ImVec2(origin.x, origin.y + 0.5f), ImVec2(origin.x + width, origin.y + 0.5f),
                       with_alpha(ink.subtle, 0.8f));

    // Scoped to end before End(): a font pushed in a window has to be popped
    // in that window.
    {
        FontScope small(nullptr, 12.0f);
        const float group_gap = design_px(18.0f);
        const float dot_gap = design_px(6.0f);
        ImGui::SetCursorPosY(std::floor((height - ImGui::GetTextLineHeight()) * 0.5f));

        // --- right: where the caret is, what is being drawn, and with what -----
        // Measured before the left half is drawn, so the project path there
        // knows how much room it may take.
        struct Part {
            std::string text;
            ImU32 color = 0;
        };
        std::vector<Part> parts;
        if (build_busy_) {
            const std::string who = building_project_name();
            parts.push_back({who.empty() ? "Building..." : "Building " + who + "...", ink.accent_ink});
        }
        if (project_open()) {
            if (const Document* doc = active_document(); doc != nullptr && show_editor) {
                parts.push_back({"Ln " + std::to_string(doc->cursor_line) + ", Col " +
                                     std::to_string(doc->cursor_column),
                                 ink.muted});
            }
            if (const PreviewPipeline* pipeline = project().active_pipeline()) {
                // The image pass counts: "3 passes" for two buffers and the image.
                const std::size_t passes = pipeline->passes.size() + 1;
                parts.push_back({pipeline->name + " \xC2\xB7 " +
                                     counted(static_cast<int>(passes), "pass", "passes"),
                                 ink.muted});
            }
        }
        if (preview_ && preview_->ready()) {
            parts.push_back({format_display_name(preview_->preview_format()), ink.muted});
        }
        parts.push_back({backend_name_, ink.muted});

        float right_width = 0.0f;
        for (const Part& part : parts) right_width += ImGui::CalcTextSize(part.text.c_str()).x;
        right_width += group_gap * static_cast<float>(parts.size() - 1);

        // --- left: what state the project is in, and which project it is ------
        const auto dotted = [&](ImU32 color, const std::string& text) {
            status_dot(color);
            ImGui::SameLine(0.0f, dot_gap);
            colored_text(text, color);
        };

        if (!project_open()) {
            colored_text("No project open", ink.muted);
        } else {
            int errors = 0;
            int warnings = 0;
            bool compiling = false;
            bool all_ok = !documents().empty();
            double milliseconds = 0.0;
            for (const Document& doc : documents()) {
                compiling = compiling || doc.compiling;
                all_ok = all_ok && doc.compiled_ok;
                milliseconds += doc.last_compile_ms;
                for (const Diagnostic& d : doc.diagnostics) {
                    if (d.severity == Severity::Error) ++errors;
                    if (d.severity == Severity::Warning) ++warnings;
                }
            }

            if (compiling) {
                dotted(ink.accent_ink, "Compiling...");
            } else if (errors > 0 || warnings > 0) {
                if (errors > 0) dotted(ink.error, counted(errors, "error", "errors"));
                if (errors > 0 && warnings > 0) ImGui::SameLine(0.0f, group_gap);
                if (warnings > 0) dotted(ink.warn, counted(warnings, "warning", "warnings"));
            } else if (all_ok) {
                char text[64];
                std::snprintf(text, sizeof(text), "Compiled \xC2\xB7 %.0f ms", milliseconds);
                dotted(ink.ok, text);
            } else {
                dotted(ink.muted, "Not compiled yet");
            }

            ImGui::SameLine(0.0f, group_gap);
            MonoScope mono(11.0f);
            // Aligned to the 12px line it sits on rather than to its own smaller
            // one, so the baselines agree.
            ImGui::SetCursorPosY(std::floor((height - ImGui::GetTextLineHeight()) * 0.5f));
            // Shortened from the front when it would run into the right half:
            // the end of a path is what says which project it is. A click
            // still copies all of it.
            const std::string path = short_path(project().root);
            const float room =
                width - design_px(12.0f) - right_width - group_gap - ImGui::GetCursorPosX();
            copyable_path(path, true, fit_text_left(path, std::max(room, text_width("...") * 2.0f)));
        }

        // Right-aligned, but never drawn back over the left half: on a window too
        // narrow for both, the right half follows the left and is clipped at the
        // edge instead.
        const float padding = design_px(12.0f);
        const float right_x = width - padding - right_width;
        ImGui::SameLine(0.0f, group_gap);
        if (right_x > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(right_x);
        ImGui::SetCursorPosY(std::floor((height - ImGui::GetTextLineHeight()) * 0.5f));
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i > 0) ImGui::SameLine(0.0f, group_gap);
            colored_text(parts[i].text, parts[i].color);
        }
    }

    ImGui::End();
}

}  // namespace ssstudio::gui
