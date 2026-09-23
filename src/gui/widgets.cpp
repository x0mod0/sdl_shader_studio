#include "widgets.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <imgui_internal.h>  // item submission for the drawn-by-hand items below

#include "fonts.h"
#include "panels/panel_common.h"

namespace ssstudio::gui {
namespace {

/// The design is drawn at a 13px interface font; every size in it is relative
/// to that.
constexpr float kDesignBase = 13.0f;

/// The interface's unscaled base size, and the same after ui_scale, both as
/// they stood at the top of the frame. See begin_type_frame().
float g_base_size = kDesignBase;
float g_base_pixels = kDesignBase;

ImU32 mix(ImU32 a, ImU32 b, float t) {
    const ImVec4 x = ImGui::ColorConvertU32ToFloat4(a);
    const ImVec4 y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t,
                                                 x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t));
}

/// The visible half of an ImGui label: "Build##top" shows "Build".
std::string_view visible_label(const char* label) {
    const char* end = std::strstr(label, "##");
    return end == nullptr ? std::string_view(label) : std::string_view(label, end - label);
}

/// Reserves a rectangle in the layout without making it interactive, sitting
/// on the text line the way ImGui's own text does: after AlignTextToFramePadding
/// it lines up with the buttons beside it, and without it with plain text.
ImRect text_line_item(float width) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const float height = ImGui::GetTextLineHeight();
    const ImVec2 pos(window->DC.CursorPos.x, window->DC.CursorPos.y + window->DC.CurrLineTextBaseOffset);
    const ImRect bb(pos, ImVec2(pos.x + width, pos.y + height));
    ImGui::ItemSize(ImVec2(width, height), 0.0f);
    ImGui::ItemAdd(bb, 0);
    return bb;
}

/// The same, a frame tall: for things that sit among buttons and fields.
ImRect frame_item(float width) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const float height = ImGui::GetFrameHeight();
    const ImRect bb(window->DC.CursorPos,
                    ImVec2(window->DC.CursorPos.x + width, window->DC.CursorPos.y + height));
    ImGui::ItemSize(bb, ImGui::GetStyle().FramePadding.y);
    ImGui::ItemAdd(bb, 0);
    return bb;
}

/// Pushes the colours of a button that is not the theme's ordinary one, and
/// pops them again when the scope ends.
class ButtonColors {
public:
    ButtonColors(ImU32 fill, ImU32 hovered, ImU32 active, ImU32 text, ImU32 border) {
        ImGui::PushStyleColor(ImGuiCol_Button, fill);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hovered);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, active);
        ImGui::PushStyleColor(ImGuiCol_Text, text);
        ImGui::PushStyleColor(ImGuiCol_Border, border);
    }
    ~ButtonColors() { ImGui::PopStyleColor(5); }
    ButtonColors(const ButtonColors&) = delete;
    ButtonColors& operator=(const ButtonColors&) = delete;
};

/// A button whose label is followed by a dimmed hint, drawn in the button's
/// own text colour at `hint_alpha`.
bool button_with_trailing_hint(const char* label, const char* hint, float hint_alpha) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float label_width = ImGui::CalcTextSize(label, nullptr, true).x;
    float hint_width = 0.0f;
    {
        FontScope small(nullptr, 11.0f);
        hint_width = ImGui::CalcTextSize(hint).x;
    }
    const float gap = design_px(8.0f);
    const ImVec2 size(style.FramePadding.x * 2.0f + label_width + gap + hint_width,
                      ImGui::GetFrameHeight());

    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleVar();

    const ImVec2 max = ImGui::GetItemRectMax();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text, hint_alpha);
    FontScope small(nullptr, 11.0f);
    const float y = min.y + (max.y - min.y - ImGui::GetTextLineHeight()) * 0.5f;
    ImGui::GetWindowDrawList()->AddText(ImVec2(max.x - style.FramePadding.x - hint_width, y), ink,
                                        hint);
    return pressed;
}

}  // namespace

// ---------------------------------------------------------------------------
// Colours
// ---------------------------------------------------------------------------
ThemeInk::ThemeInk(const ResolvedTheme& theme) {
    const auto role = [&theme](const char* name) { return theme_u32(theme.role(name)); };
    base = role("surface.base");
    raised = role("surface.raised");
    sunken = role("surface.sunken");
    text = role("ink.primary");
    muted = role("ink.muted");
    inverted = role("ink.inverted");
    soft = mix(text, muted, 0.45f);
    subtle = role("line.subtle");
    strong = role("line.strong");
    accent = role("accent");
    accent_hover = role("accent.hover");
    accent_active = role("accent.active");
    accent_muted = role("accent.muted");
    ok = theme_u32(theme.success());
    warn = theme_u32(theme.diagnostic(Severity::Warning));
    error = theme_u32(theme.diagnostic(Severity::Error));
    info = theme_u32(theme.diagnostic(Severity::Info));
}

ImU32 ThemeInk::severity(Severity s) const {
    switch (s) {
        case Severity::Error: return error;
        case Severity::Warning: return warn;
        case Severity::Info: return info;
    }
    return muted;
}

ImU32 with_alpha(ImU32 color, float alpha) {
    const auto a = static_cast<ImU32>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    return (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

// ---------------------------------------------------------------------------
// Type
// ---------------------------------------------------------------------------
void begin_type_frame() {
    const float base = ImGui::GetStyle().FontSizeBase;
    g_base_size = base > 0.0f ? base : kDesignBase;
    g_base_pixels = ImGui::GetFontSize() > 0.0f ? ImGui::GetFontSize() : g_base_size;
}

float type_size(float design) { return g_base_size * design / kDesignBase; }

float type_pixels(float design) { return g_base_pixels * design / kDesignBase; }

float design_px(float px) { return std::round(px * g_base_pixels / kDesignBase); }

FontScope::FontScope(ImFont* font, float design) { ImGui::PushFont(font, type_size(design)); }

FontScope::~FontScope() { ImGui::PopFont(); }

MonoScope::MonoScope(float design) : FontScope(mono_font(), design) {}

void colored_text(std::string_view text, ImU32 color) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopStyleColor();
}

void mono_text(std::string_view text, ImU32 color, float design) {
    MonoScope mono(design);
    colored_text(text, color);
}

void caps_label(std::string_view text, const ThemeInk& ink) {
    std::string upper(text);
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    FontScope small(nullptr, 10.5f);
    colored_text(upper, ink.muted);
}

// ---------------------------------------------------------------------------
// Marks
// ---------------------------------------------------------------------------
void status_dot(ImU32 color, float design) {
    const float diameter = design_px(design);
    const ImRect bb = text_line_item(diameter);
    ImGui::GetWindowDrawList()->AddCircleFilled(bb.GetCenter(), diameter * 0.5f, color);
}

void toolbar_divider(const ThemeInk& ink) {
    const float margin = design_px(4.0f);
    const ImRect bb = frame_item(margin * 2.0f + 1.0f);
    const float inset = bb.GetHeight() * 0.16f;
    const float x = std::floor(bb.Min.x + margin) + 0.5f;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(x, bb.Min.y + inset), ImVec2(x, bb.Max.y - inset),
                                        ink.subtle);
}

// ---------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------
bool primary_button(const char* label, const ThemeInk& ink, const ImVec2& size) {
    ButtonColors colors(ink.accent, ink.accent_hover, ink.accent_active, ink.inverted,
                        with_alpha(ink.accent, 0.0f));
    return ImGui::Button(label, size);
}

bool primary_button_with_hint(const char* label, const char* hint, const ThemeInk& ink) {
    ButtonColors colors(ink.accent, ink.accent_hover, ink.accent_active, ink.inverted,
                        with_alpha(ink.accent, 0.0f));
    return button_with_trailing_hint(label, hint, 0.7f);
}

bool danger_button(const char* label, const ThemeInk& ink) {
    ButtonColors colors(with_alpha(ink.error, 0.0f), with_alpha(ink.error, 0.12f),
                        with_alpha(ink.error, 0.2f), ink.error, with_alpha(ink.error, 0.45f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    const bool pressed = ImGui::Button(label);
    ImGui::PopStyleVar();
    return pressed;
}

bool ghost_button(const char* label, const ThemeInk& ink) {
    // The text brightens under the pointer, which has to be known before the
    // button is drawn: measured the way Button() will measure itself.
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::CalcTextSize(label, nullptr, true).x + style.FramePadding.x * 2.0f,
                      ImGui::GetFrameHeight());
    const bool hovered = !ImGui::GetCurrentContext()->DisabledStackSize &&
                         ImGui::IsWindowHovered() &&
                         ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    ButtonColors colors(with_alpha(ink.raised, 0.0f), ImGui::GetColorU32(ImGuiCol_HeaderHovered),
                        ImGui::GetColorU32(ImGuiCol_HeaderActive), hovered ? ink.text : ink.muted,
                        with_alpha(ink.raised, 0.0f));
    return ImGui::Button(label);
}

bool button_with_hint(const char* label, const char* hint, const ThemeInk& ink) {
    ImGui::PushStyleColor(ImGuiCol_Text, ink.text);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, ink.muted);
    const bool pressed = button_with_trailing_hint(label, hint, 0.6f);
    ImGui::PopStyleColor(2);
    return pressed;
}

// ---------------------------------------------------------------------------
// Chips and controls
// ---------------------------------------------------------------------------
float toggle_chip_width(const char* label, int count) {
    FontScope small(nullptr, 12.0f);
    const std::string number = std::to_string(count);
    const std::string_view shown = visible_label(label);
    return design_px(9.0f) * 2.0f + design_px(6.0f) * 3.0f +
           ImGui::CalcTextSize(shown.data(), shown.data() + shown.size()).x +
           ImGui::CalcTextSize(number.c_str()).x;
}

bool toggle_chip(const char* label, int count, ImU32 dot, bool& on, const ThemeInk& ink) {
    // Measured at the row's own size, so the chip lines up with the buttons
    // beside it even though its text is a size smaller.
    const float height = ImGui::GetFrameHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();

    FontScope small(nullptr, 12.0f);
    const std::string number = std::to_string(count);
    const std::string_view shown = visible_label(label);
    const float pad = design_px(9.0f);
    const float gap = design_px(6.0f);
    const float dot_size = design_px(6.0f);
    const float label_width =
        ImGui::CalcTextSize(shown.data(), shown.data() + shown.size()).x;
    const float number_width = ImGui::CalcTextSize(number.c_str()).x;
    const ImVec2 size(pad + dot_size + gap + label_width + gap + number_width + pad, height);

    ImGui::PushID(label);
    const bool pressed = ImGui::InvisibleButton("##chip", size);
    ImGui::PopID();
    const bool hovered = ImGui::IsItemHovered();
    if (pressed) on = !on;

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImVec2 max(pos.x + size.x, pos.y + size.y);
    const float rounding = size.y * 0.5f;
    if (on) {
        draw_list->AddRectFilled(pos, max,
                                 ImGui::GetColorU32(hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button),
                                 rounding);
        draw_list->AddRect(pos, max, ink.strong, rounding);
    } else {
        if (hovered) {
            draw_list->AddRectFilled(pos, max, ImGui::GetColorU32(ImGuiCol_HeaderHovered), rounding);
        }
        draw_list->AddRect(pos, max, ink.subtle, rounding);
    }

    const float center_y = pos.y + size.y * 0.5f;
    const float text_y = center_y - ImGui::GetTextLineHeight() * 0.5f;
    float x = pos.x + pad;
    draw_list->AddCircleFilled(ImVec2(x + dot_size * 0.5f, center_y), dot_size * 0.5f,
                               on ? dot : with_alpha(dot, 0.55f));
    x += dot_size + gap;
    draw_list->AddText(ImVec2(x, text_y), on ? ink.text : ink.muted, shown.data(),
                       shown.data() + shown.size());
    x += label_width + gap;
    draw_list->AddText(ImVec2(x, text_y), on ? ink.soft : ink.muted, number.c_str());
    return pressed;
}

float segmented_control_width(const char* const* labels, int count) {
    FontScope small(nullptr, 12.0f);
    const float pad = design_px(10.0f);
    float width = design_px(2.0f) * 2.0f;
    for (int i = 0; i < count; ++i) width += ImGui::CalcTextSize(labels[i]).x + pad * 2.0f;
    return width + design_px(2.0f) * static_cast<float>(std::max(0, count - 1));
}

bool segmented_control(const char* id, const char* const* labels, int count, int& current,
                       const ThemeInk& ink) {
    const float height = ImGui::GetFrameHeight();
    const float width = segmented_control_width(labels, count);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 track_max(origin.x + width, origin.y + height);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const float rounding = std::max(ImGui::GetStyle().FrameRounding, design_px(6.0f));
    draw_list->AddRectFilled(origin, track_max, ink.sunken, rounding);
    draw_list->AddRect(origin, track_max, ink.subtle, rounding);

    FontScope small(nullptr, 12.0f);
    const float inset = design_px(2.0f);
    const float pad = design_px(10.0f);
    bool changed = false;

    ImGui::PushID(id);
    float x = origin.x + inset;
    for (int i = 0; i < count; ++i) {
        const float item_width = ImGui::CalcTextSize(labels[i]).x + pad * 2.0f;
        const ImVec2 min(x, origin.y + inset);
        const ImVec2 max(x + item_width, track_max.y - inset);
        ImGui::SetCursorScreenPos(min);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##segment", ImVec2(max.x - min.x, max.y - min.y)) &&
            current != i) {
            current = i;
            changed = true;
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();

        const bool selected = current == i;
        if (selected || hovered) {
            draw_list->AddRectFilled(
                min, max,
                ImGui::GetColorU32(selected ? ImGuiCol_Button : ImGuiCol_HeaderHovered),
                std::max(0.0f, rounding - inset));
        }
        const float text_y = min.y + (max.y - min.y - ImGui::GetTextLineHeight()) * 0.5f;
        draw_list->AddText(ImVec2(min.x + pad, text_y), selected || hovered ? ink.text : ink.muted,
                           labels[i]);
        x = max.x + inset;
    }
    ImGui::PopID();

    // The segments were placed by hand; this is the one item the layout sees,
    // so whatever follows lines up after the whole track.
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, height));
    return changed;
}

void pill_label(std::string_view text, ImU32 text_color, ImU32 fill) {
    const float height = ImGui::GetFrameHeight() - design_px(2.0f);
    FontScope small(nullptr, 12.0f);
    const float pad = design_px(9.0f);
    const float text_width = ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;

    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImVec2 pos(window->DC.CursorPos.x, window->DC.CursorPos.y + design_px(1.0f));
    const ImRect bb(pos, ImVec2(pos.x + text_width + pad * 2.0f, pos.y + height));
    ImGui::ItemSize(ImVec2(bb.GetWidth(), ImGui::GetFrameHeight()),
                    ImGui::GetStyle().FramePadding.y);
    ImGui::ItemAdd(bb, 0);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(bb.Min, bb.Max, fill, height * 0.5f);
    draw_list->AddText(ImVec2(bb.Min.x + pad, bb.Min.y + (height - ImGui::GetTextLineHeight()) * 0.5f),
                       text_color, text.data(), text.data() + text.size());
}

float tag_label_width(std::string_view text) {
    MonoScope mono(10.5f);
    return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x + design_px(6.0f) * 2.0f;
}

void tag_label(std::string_view text, const ThemeInk& ink) {
    const float width = tag_label_width(text);
    MonoScope mono(10.5f);
    const ImRect bb = text_line_item(width);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(ImVec2(bb.Min.x, bb.Min.y - design_px(1.0f)),
                             ImVec2(bb.Max.x, bb.Max.y + design_px(1.0f)),
                             ImGui::GetColorU32(ImGuiCol_Button), design_px(4.0f));
    draw_list->AddText(ImVec2(bb.Min.x + design_px(6.0f), bb.Min.y), ink.soft, text.data(),
                       text.data() + text.size());
}

// ---------------------------------------------------------------------------
// Sections and tables
// ---------------------------------------------------------------------------
bool section_begin(const char* id, const SectionHeader& header, const ThemeInk& ink) {
    const ImGuiStyle& style = ImGui::GetStyle();
    // The theme's child rounding when it has one, and the frame rounding when
    // it does not: the built-in themes leave children square, and a bordered
    // block with square corners among rounded fields reads as a mistake.
    const float rounding = style.ChildRounding > 0.0f ? style.ChildRounding : style.FrameRounding;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, rounding);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, ink.subtle);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, with_alpha(ink.raised, 0.0f));
    // Auto-sized rather than scrolling: a section is as tall as what it holds,
    // and the panel around it is what scrolls. A child that cannot scroll
    // hands the mouse wheel to its parent by itself.
    ImGui::BeginChild(id, ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float height = design_px(34.0f);
    const float pad = design_px(10.0f);

    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID open_id = ImGui::GetID("##open");
    bool open = storage->GetBool(open_id, header.default_open);
    if (ImGui::InvisibleButton("##header", ImVec2(width, height))) {
        open = !open;
        storage->SetBool(open_id, open);
    }
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 max(min.x + width, min.y + height);

    draw_list->AddRectFilled(min, max,
                             ImGui::GetColorU32(hovered ? ImGuiCol_HeaderHovered : ImGuiCol_Header),
                             rounding, open ? ImDrawFlags_RoundCornersTop : ImDrawFlags_RoundCornersAll);
    if (open) draw_list->AddLine(ImVec2(min.x, max.y - 0.5f), ImVec2(max.x, max.y - 0.5f), ink.subtle);

    const float center_y = min.y + height * 0.5f;
    float x = min.x + pad;
    const float arrow = ImGui::GetFontSize() * 0.7f;
    ImGui::RenderArrow(draw_list, ImVec2(x, center_y - arrow * 0.5f), ink.muted,
                       open ? ImGuiDir_Down : ImGuiDir_Right, 0.7f);
    x += arrow + design_px(8.0f);

    const float text_y = center_y - ImGui::GetTextLineHeight() * 0.5f;
    draw_list->AddText(ImVec2(x, text_y), ink.text, header.title.c_str());
    x += ImGui::CalcTextSize(header.title.c_str()).x + design_px(8.0f);

    float tag_left = max.x - pad;
    if (!header.tag.empty()) {
        const float tag_width = tag_label_width(header.tag);
        tag_left -= tag_width;
        MonoScope mono(10.5f);
        const float tag_height = ImGui::GetTextLineHeight() + design_px(4.0f);
        const ImVec2 tag_min(tag_left, center_y - tag_height * 0.5f);
        draw_list->AddRectFilled(tag_min, ImVec2(tag_left + tag_width, tag_min.y + tag_height),
                                 ImGui::GetColorU32(ImGuiCol_Button), design_px(4.0f));
        draw_list->AddText(ImVec2(tag_left + design_px(6.0f), tag_min.y + design_px(2.0f)), ink.soft,
                           header.tag.c_str());
    }
    if (!header.meta.empty()) {
        MonoScope mono(11.0f);
        const std::string meta = fit_text(header.meta, tag_left - design_px(8.0f) - x);
        draw_list->AddText(ImVec2(x, center_y - ImGui::GetTextLineHeight() * 0.5f), ink.muted,
                           meta.c_str());
    }

    // Always indented, whatever `open` says, so section_end() has exactly one
    // thing to undo and cannot be called wrongly.
    ImGui::Indent(pad);
    if (open) ImGui::Dummy(ImVec2(0.0f, design_px(2.0f)));
    return open;
}

void section_end() {
    ImGui::Unindent(design_px(10.0f));
    ImGui::EndChild();
}

float section_content_width() {
    return std::max(1.0f, ImGui::GetContentRegionAvail().x - design_px(10.0f));
}

void caps_headers_row(const ThemeInk& ink, unsigned right_aligned) {
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    FontScope small(nullptr, 10.5f);
    ImGui::PushStyleColor(ImGuiCol_Text, ink.muted);
    const int columns = ImGui::TableGetColumnCount();
    for (int column = 0; column < columns; ++column) {
        if (!ImGui::TableSetColumnIndex(column)) continue;
        const char* name = ImGui::TableGetColumnName(column);
        if (name == nullptr) continue;
        std::string upper(visible_label(name));
        for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (upper.empty()) continue;
        if ((right_aligned & (1u << column)) != 0) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                 std::max(0.0f, ImGui::GetContentRegionAvail().x -
                                                    ImGui::CalcTextSize(upper.c_str()).x));
        }
        ImGui::TextUnformatted(upper.c_str());
    }
    ImGui::PopStyleColor();
}

void overlay_frame(ImDrawList* draw_list, const ImVec2& min, const ImVec2& max,
                   const ThemeInk& ink, ImU32 border) {
    const float rounding = std::max(ImGui::GetStyle().FrameRounding, design_px(4.0f));
    draw_list->AddRectFilled(min, max, with_alpha(ink.base, 0.86f), rounding);
    draw_list->AddRect(min, max, border != 0 ? border : ink.subtle, rounding);
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------
std::string fit_text(std::string_view text, float max_width) {
    if (max_width <= 0.0f) return {};
    std::string out(text);
    if (ImGui::CalcTextSize(out.c_str()).x <= max_width) return out;
    const float dots = ImGui::CalcTextSize("...").x;
    while (!out.empty()) {
        // Whole UTF-8 sequences only: dropping half of one would draw a
        // replacement glyph at exactly the place the eye is reading.
        do {
            out.pop_back();
        } while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80);
        if (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0xC0) out.pop_back();
        if (ImGui::CalcTextSize(out.c_str()).x + dots <= max_width) break;
    }
    return out + "...";
}

const char* arrow_glyph() {
    ImFont* font = ImGui::GetFont();
    return font != nullptr && font->IsGlyphInFont(0x2192) ? "\xE2\x86\x92" : "->";
}

std::string grouped(unsigned long long value) {
    std::string digits = std::to_string(value);
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) {
        digits.insert(static_cast<std::size_t>(i), ",");
    }
    return digits;
}

std::string human_bytes(unsigned long long bytes) {
    char buffer[32];
    if (bytes < 1024ull) {
        std::snprintf(buffer, sizeof(buffer), "%llu B", bytes);
    } else if (bytes < 1024ull * 1024ull) {
        std::snprintf(buffer, sizeof(buffer), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.1f MB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    return buffer;
}

}  // namespace ssstudio::gui
