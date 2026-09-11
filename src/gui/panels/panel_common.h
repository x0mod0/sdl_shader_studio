/// Small helpers shared by the panels.
#ifndef SSSTUDIO_GUI_PANEL_COMMON_H
#define SSSTUDIO_GUI_PANEL_COMMON_H

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <string>
#include <string_view>

#include <imgui.h>

#include "ssstudio/color.h"

#include "ssstudio/types.h"

namespace ssstudio::gui {

/// A theme colour as ImGui wants it. Two spellings because ImGui wants both: a
/// draw list takes packed ImU32, and PushStyleColor takes floats.
///
/// The packing differs as well as the type - Rgba is 0xRRGGBBAA, matching how
/// hex reads, while IM_COL32 is ABGR in memory - so these two functions are the
/// only place that byte order needs thinking about.
inline ImU32 theme_u32(Rgba color) {
    return IM_COL32((color >> 24) & 0xFF, (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);
}

inline ImVec4 theme_vec4(Rgba color) {
    return ImVec4(static_cast<float>((color >> 24) & 0xFF) / 255.0f,
                  static_cast<float>((color >> 16) & 0xFF) / 255.0f,
                  static_cast<float>((color >> 8) & 0xFF) / 255.0f,
                  static_cast<float>(color & 0xFF) / 255.0f);
}

/// What a shader is called in the interface: the base of its filename, which is
/// exactly what was typed into the New shader form.
///
/// The id is a different string - it carries the stage and the language too, so
/// that test.vert.hlsl and test.frag.hlsl can both be called "test" - and that
/// spelling belongs to the manifest and the generated header rather than to
/// anything a person reads. A file with no extension at all falls back to the
/// id, because a blank label would be worse than a technical one.
///
/// Takes the two fields rather than a Document so this header does not have to
/// know what one is; it is shared because a shader called one thing in a tab and
/// another in a window title is a shader nobody can find.
inline std::string shader_display_name(const std::filesystem::path& path,
                                       const std::string& id) {
    const std::string filename = path.filename().string();
    const std::size_t dot = filename.find('.');
    if (dot == std::string::npos) return filename.empty() ? id : filename;
    return dot == 0 ? filename : filename.substr(0, dot);
}

/// "vert" / "frag" / "comp", the way the tabs and the file extensions spell it.
inline const char* stage_suffix(Stage stage) {
    switch (stage) {
        case Stage::Vertex: return "vert";
        case Stage::Fragment: return "frag";
        case Stage::Compute: return "comp";
    }
    return "?";
}

/// Places widgets left to right and starts a new row when the next one would
/// not fit, so a toolbar folds instead of running off the edge of a narrowed
/// panel.
///
/// ImGui lays out a row with SameLine(), which has no idea how wide the row is
/// allowed to get; a panel dragged narrow simply clips whatever hangs over the
/// right edge. This asks the opposite question - "does the next widget still
/// fit?" - and only calls SameLine() when it does.
///
/// The caller has to say how wide the widget it is about to place will be,
/// which is what button_width() and the helpers below are for. Widths that
/// depend on the panel (fitted_width) must all be computed before the first
/// next() call, while GetContentRegionAvail() still describes the whole row.
///
/// Usage:
///     const float speed_w = fitted_width(110.0f);
///     FlowLayout row;
///     row.next(button_width("Pause"));
///     ImGui::Button("Pause");
///     row.next(labeled_width("speed", speed_w));
///     ImGui::SetNextItemWidth(speed_w);
///     ImGui::DragFloat("speed", &speed);
class FlowLayout {
public:
    FlowLayout()
        : right_edge_(ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x),
          spacing_(ImGui::GetStyle().ItemSpacing.x) {}

    /// Call immediately before each widget. The first call places the widget on
    /// the current line; later ones continue the row while `item_width` still
    /// fits, and let the widget fall to the next row when it does not.
    void next(float item_width) {
        if (first_) {
            first_ = false;
            return;
        }
        if (ImGui::GetItemRectMax().x + spacing_ + item_width <= right_edge_) ImGui::SameLine();
    }

    /// Forces the next widget onto a new row - for a group that belongs
    /// together and should not be split across the fold.
    void wrap() { first_ = true; }

private:
    float right_edge_ = 0.0f;
    float spacing_ = 0.0f;
    bool first_ = true;
};

/// Width a button with this label will occupy. SmallButton keeps the horizontal
/// frame padding of a regular button and only drops the vertical, so this
/// covers both.
inline float button_width(const char* label) {
    return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

/// Width a checkbox or a radio button with this label will occupy: the box, the
/// gap, and the text.
inline float checkbox_width(const char* label) {
    const ImGuiStyle& style = ImGui::GetStyle();
    return ImGui::GetFrameHeight() + style.ItemInnerSpacing.x +
           ImGui::CalcTextSize(label, nullptr, true).x;
}

/// Width of a plain run of text.
inline float text_width(const char* text) { return ImGui::CalcTextSize(text).x; }

/// Width of an input widget plus the label ImGui draws to its right.
inline float labeled_width(const char* label, float item_width) {
    const float text = ImGui::CalcTextSize(label, nullptr, true).x;
    return item_width + (text > 0.0f ? ImGui::GetStyle().ItemInnerSpacing.x + text : 0.0f);
}

/// A width that gives way as the panel narrows: `preferred` while there is room
/// for it, whatever room is left when there is not. Never below `minimum`
/// unless the panel itself is narrower than that.
///
/// Call it before laying the row out, while the available region is still the
/// full width of the row rather than what a SameLine() has left over.
inline float fitted_width(float preferred, float minimum = 70.0f) {
    const float room = ImGui::GetContentRegionAvail().x;
    return std::max(std::min(preferred, room), std::min(minimum, room));
}

/// Width for a side pane that keeps its share of a narrowed panel instead of
/// squeezing the main area to nothing: `preferred` while the panel is wide,
/// then a fraction of it, and never more than `max_fraction` of the whole.
inline float side_pane_width(float preferred, float max_fraction = 0.4f) {
    const float room = ImGui::GetContentRegionAvail().x;
    return std::max(60.0f, std::min(preferred, room * max_fraction));
}

/// A filesystem path as the path component of a file:// URL.
///
/// Everything outside the unreserved set is percent-encoded, because
/// SDL_OpenURL is handed to a platform URL parser rather than to a shell: a
/// project directory called "My Shaders" would otherwise be rejected outright on
/// macOS, and a '#' in a directory name would truncate the path.
inline std::string file_url(const std::filesystem::path& directory) {
    static const char* hex = "0123456789ABCDEF";
    std::string out = "file://";
    for (const char c : directory.generic_string()) {
        const auto byte = static_cast<unsigned char>(c);
        if (std::isalnum(byte) != 0 || byte == '/' || byte == '-' || byte == '_' || byte == '.' ||
            byte == '~') {
            out.push_back(c);
            continue;
        }
        out.push_back('%');
        out.push_back(hex[byte >> 4]);
        out.push_back(hex[byte & 0x0F]);
    }
    return out;
}

/// A path as something outside this process can use: absolute, with any '..'
/// and symlinks resolved. Everything in the app carries absolute paths already,
/// but this is where one leaves the process - as a file:// URL or on the
/// clipboard - and a relative path is silently useless in both.
inline std::filesystem::path external_path(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path resolved = std::filesystem::weakly_canonical(path, ec);
    if (!ec && !resolved.empty()) return resolved;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    return ec ? path : absolute;
}

/// A stable identifier written the way a heading is written: the same string
/// with its first letter capitalized.
///
/// For values that are spellings in a file as well as words on screen - a node
/// category in a graph, a stage in a manifest. The file keeps the spelling it
/// has always had, and the interface gets one that matches every other heading
/// beside it.
inline std::string display_case(std::string_view name) {
    std::string out(name);
    if (!out.empty()) out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

/// Draws an input's name before the input rather than after it, and returns the
/// id to hand the widget itself.
///
/// ImGui puts a label to the right of the control it belongs to, so a form reads
/// as "[field] name" - the value arrives before the word that says what it is.
/// Every input in this app is drawn the other way round, which is how a form is
/// read everywhere else.
///
/// The width has to be set here rather than by the caller: SetNextItemWidth is
/// consumed by the next item added, and the label is an item, so a width set
/// before it would be spent on the text.
///
/// Checkboxes and radio buttons are left alone. Their label belongs after the
/// box - it is the thing being ticked, not a name for a value.
///
/// Usage:
///     ImGui::DragFloat(left_label("speed", speed_w).c_str(), &speed, 0.01f);
inline std::string left_label(const char* label, float item_width = 0.0f) {
    // The visible half only: a label of "name##unique" shows "name" and keeps
    // its uniqueness in the id returned below.
    const char* end = std::strstr(label, "##");
    // A label that is entirely hidden - "##v" in a table whose column header
    // already names it - draws nothing at all. Emitting an empty text item would
    // still cost the spacing that follows it, and indent the field by a gap with
    // nothing in it.
    if (end != label) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label, end);
        ImGui::SameLine();
    }
    if (item_width != 0.0f) ImGui::SetNextItemWidth(item_width);
    // The whole original after "##", so two fields of one name stay as distinct
    // as they were before the label moved.
    return std::string("##") + label;
}

/// A colour field: a box you can type a hex value into, and a swatch that opens
/// the picker, each showing what the other did.
///
/// One control rather than two, because a colour is one value. ImGui's own
/// ColorEdit has a text mode, but it is behind a right-click on the swatch and
/// spells its value without the '#' - so the number you can read off the screen
/// is not the one you can paste into a stylesheet, a shader, or this field.
///
/// `components` is 3 or 4; with 4 the picker gains an alpha bar and the text
/// gains two more digits. Returns true when the colour changed this frame.
inline bool color_field(const char* label, float* rgba, int components,
                        float hex_width = 96.0f) {
    IM_ASSERT(components == 3 || components == 4);

    // What is in the box, per field, so that a half-typed value survives to the
    // next frame. Rewriting the text from the colour every frame would make the
    // field impossible to type into: the first keystroke would be replaced
    // before the second arrived.
    static std::map<ImGuiID, std::string> typing;

    const std::string id = left_label(label, hex_width);
    const ImGuiID key = ImGui::GetID(id.c_str());

    auto entry = typing.find(key);
    if (entry == typing.end()) {
        entry = typing.emplace(key, color_to_hex(rgba, components)).first;
    }

    bool changed = false;
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%s", entry->second.c_str());
    if (ImGui::InputText(id.c_str(), buffer, sizeof(buffer))) {
        entry->second = buffer;
        // Only a complete colour is applied. Everything else is left in the box
        // as typed, so the field can be cleared and retyped without the swatch
        // flickering through whatever the half-finished text happened to name.
        changed = color_from_hex(entry->second, rgba, components);
    }
    const bool typing_here = ImGui::IsItemActive();

    ImGui::SameLine();
    ImGuiColorEditFlags flags = ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel;
    if (components == 4) {
        flags |= ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf;
    }
    const std::string swatch = id + "_swatch";
    const bool picked = components == 4 ? ImGui::ColorEdit4(swatch.c_str(), rgba, flags)
                                        : ImGui::ColorEdit3(swatch.c_str(), rgba, flags);
    changed |= picked;

    // Written back from the colour whenever the box is not being typed into,
    // which is what makes the picker show up in the text.
    if (!typing_here) entry->second = color_to_hex(rgba, components);
    return changed;
}

/// A draggable divider between two panes. ImGui has no splitter of its own, so
/// it is an invisible button that applies its own drag to the width it is given.
///
/// `direction` is +1 when the pane being resized is to the left of the divider
/// and -1 when it is to the right, which is the whole difference between the two
/// sides of a three-pane layout: dragging left grows the right-hand pane.
///
/// Drawn as a faint line rather than nothing at all - a divider you can only
/// find by hovering everything is a divider nobody finds.
inline void vertical_splitter(const char* id, float height, float& width, float min_width,
                              float max_width, float direction) {
    // Eight pixels rather than the four a drawn line needs: the target has to be
    // findable with a pointer, not just visible.
    constexpr float kThickness = 8.0f;
    const ImVec2 top_left = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(kThickness, std::max(1.0f, height)));

    const bool active = ImGui::IsItemActive();
    if (active || ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (active) {
        width = std::clamp(width + ImGui::GetIO().MouseDelta.x * direction, min_width, max_width);
    }

    const ImU32 color = ImGui::GetColorU32(active      ? ImGuiCol_SeparatorActive
                                           : ImGui::IsItemHovered() ? ImGuiCol_SeparatorHovered
                                                                    : ImGuiCol_Separator);
    // Thicker while it is being used, so the drag reads as deliberate rather
    // than as something that might not have taken.
    const float x = top_left.x + kThickness * 0.5f;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(x, top_left.y), ImVec2(x, top_left.y + height),
                                        color, active ? 3.0f : 2.0f);
}

/// Right-aligns the next widget on the current line, against the right edge of
/// the content region rather than against whatever was drawn before it.
///
/// SameLine's own offset is measured from the end of the previous item, which
/// is no help when that item is a Selectable filling the whole row: the offset
/// then lands a further row's width past the edge and the widget disappears off
/// the side of the panel. Computing the target from the right edge instead
/// gives the same answer whatever preceded it, wide or narrow.
///
/// Overlapping the previous item is the ordinary case here rather than a
/// mistake - a row-wide Selectable with a small button in its right end is the
/// reason this exists, and that Selectable needs
/// ImGuiSelectableFlags_AllowOverlap so the button can still take the click. On
/// a panel too narrow to hold the widget it starts at the row's left edge and
/// clips there, which at least leaves the near end of it on screen.
inline void same_line_right_aligned(float item_width) {
    // Both read before SameLine moves the cursor: it is sitting at the start of
    // the following line here, so its x is the row's left edge and the region
    // ahead of it is the width of the row.
    const float left = ImGui::GetCursorScreenPos().x;
    const float right = left + ImGui::GetContentRegionAvail().x;

    ImGui::SameLine();
    // SameLine has put the y back on the previous line; only the x is wrong.
    ImVec2 pos = ImGui::GetCursorScreenPos();
    pos.x = std::max(left, right - item_width);
    ImGui::SetCursorScreenPos(pos);
}

/// A path shown to be read and then taken somewhere else - a terminal, a file
/// manager, a bug report. Clicking it copies it.
///
/// A plain label rather than a Selectable or an InputText: labels wrap at the
/// panel edge, and a path is exactly the kind of text that has to. The cost is
/// that a label offers nothing to say it is clickable, which is what the hand
/// cursor and the tooltip are for - and the tooltip says "Copied" for a moment
/// afterwards, because a copy with no visible effect is one you do again to be
/// sure it happened.
inline void copyable_path(const std::string& text, bool dim = true) {
    // What the last click copied, and when. One pair for the whole interface:
    // only one path can have been the last one clicked.
    static std::string copied;
    static double copied_at = 0.0;

    if (dim) {
        ImGui::TextDisabled("%s", text.c_str());
    } else {
        ImGui::TextUnformatted(text.c_str());
    }

    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::IsItemClicked()) {
            ImGui::SetClipboardText(text.c_str());
            copied = text;
            copied_at = ImGui::GetTime();
        }
    }
    // On a delay - SetItemTooltip honours the style's hover delay - because a
    // tooltip that fires every time the pointer crosses a row on its way down a
    // list is a tooltip you learn to read past.
    const bool just_copied = copied == text && ImGui::GetTime() - copied_at < 1.5;
    ImGui::SetItemTooltip("%s", just_copied ? "Copied" : "Click to copy");
}

/// Wraps every Text* call in the enclosing window - or, inside a table, in the
/// enclosing column - at its right edge, so long paths, shader ids and compiler
/// messages fold onto a second line instead of running past the panel.
///
/// ImGui keeps the wrap position per window, so a child window opened inside the
/// scope needs one of its own.
class TextWrapScope {
public:
    TextWrapScope() { ImGui::PushTextWrapPos(0.0f); }
    ~TextWrapScope() { ImGui::PopTextWrapPos(); }

    TextWrapScope(const TextWrapScope&) = delete;
    TextWrapScope& operator=(const TextWrapScope&) = delete;
};

/// One panel window: Begin on construction, End on destruction, with text
/// wrapping enabled in between.
///
/// The panels return early from several places, and the wrap position has to be
/// popped before End or it would unbalance the parent window's stack. Tying both
/// to a scope makes that ordering impossible to get wrong, and removes the
/// End()-before-every-return that the panels used to repeat.
///
/// Usage:
///     PanelScope panel("Editor", &app.show_editor);
///     if (!panel) return;
class PanelScope {
public:
    PanelScope(const char* name, bool* p_open, ImGuiWindowFlags flags = 0)
        : visible_(ImGui::Begin(name, p_open, flags)) {
        if (visible_) ImGui::PushTextWrapPos(0.0f);
    }

    ~PanelScope() {
        if (visible_) ImGui::PopTextWrapPos();
        // ImGui wants End() even when Begin() returned false (collapsed window).
        ImGui::End();
    }

    /// False when the window is collapsed or clipped and its contents should be
    /// skipped entirely.
    explicit operator bool() const { return visible_; }

    PanelScope(const PanelScope&) = delete;
    PanelScope& operator=(const PanelScope&) = delete;

private:
    bool visible_ = false;
};

}  // namespace ssstudio::gui

#endif  // SSSTUDIO_GUI_PANEL_COMMON_H
