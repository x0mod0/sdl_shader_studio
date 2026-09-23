#include "ssstudio/theme_pack.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

#include <toml++/toml.hpp>

#include "ssstudio/settings.h"

namespace ssstudio {
namespace {

Diagnostic problem(Severity severity, std::string message, std::string file) {
    Diagnostic d;
    d.severity = severity;
    d.message = std::move(message);
    d.file = std::move(file);
    return d;
}

std::string lowered(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string_view trimmed(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

// ---------------------------------------------------------------------------
// The tables
//
// These are the tables docs/THEME_PACK_FORMAT.md documents, written as the
// expressions that document prints. Keeping them as text rather than as code
// means the derivation a pack author reads and the one that runs cannot drift
// apart: there is one spelling of "a hovered button is the accent, lightened".
// ---------------------------------------------------------------------------

struct Entry {
    std::string_view name;
    std::string_view expression;
};

/// The twenty semantic roles, in the order `ssstudio theme --resolve` lists them,
/// each with the expression docs/THEME_PACK_FORMAT.md suggests writing for it.
/// Only the names are read at runtime (theme_role_names()). A role a pack leaves
/// out comes from its base theme - in the end the fixed values in
/// builtin_theme_roles() - and is never evaluated from these expressions; they
/// are kept so the suggestion the document prints has one spelling. `ink.inverted`
/// has no expression because, when absent, it is a choice between two colours
/// rather than a formula - see inverted_ink().
constexpr std::array<Entry, 20> kRoleDefaults = {{
    {"surface.base", "#17171a"},
    {"surface.raised", "lighten($surface.base, 4%)"},
    {"surface.sunken", "darken($surface.base, 3%)"},
    {"surface.overlay", "alpha($surface.base, 60%)"},
    {"ink.primary", "#f2f2f2"},
    {"ink.muted", "mix($ink.primary, $surface.base, 55%)"},
    {"ink.inverted", ""},
    {"line.subtle", "mix($surface.base, $ink.primary, 14%)"},
    {"line.strong", "mix($surface.base, $ink.primary, 28%)"},
    {"accent", "#33384a"},
    {"accent.hover", "lighten($accent, 8%)"},
    {"accent.active", "darken($accent, 6%)"},
    {"accent.muted", "alpha($accent, 25%)"},
    {"accent.ink", "$ink.primary"},
    {"select.bg", "alpha($accent, 35%)"},
    {"select.ink", "$ink.primary"},
    {"status.ok", "#6fbf73"},
    {"status.warn", "#e0af68"},
    {"status.error", "#eb6a6a"},
    {"status.info", "$accent"},
}};

/// Every widget colour, and what it is when a pack does not name it. Spelled
/// as ImGui spells it, prefix dropped: this is a foreign vocabulary, and
/// renaming it to snake_case would break the one thing an author does when
/// stuck, which is search ImGui's own documentation for the name.
constexpr std::array<Entry, 63> kUiDerivations = {{
    {"Text", "$ink.primary"},
    {"TextDisabled", "$ink.muted"},
    {"WindowBg", "$surface.base"},
    {"ChildBg", "$surface.raised"},
    {"PopupBg", "$surface.raised"},
    {"Border", "$line.subtle"},
    {"BorderShadow", "alpha($surface.base, 0%)"},
    {"FrameBg", "$surface.sunken"},
    {"FrameBgHovered", "lighten($surface.sunken, 4%)"},
    {"FrameBgActive", "lighten($surface.sunken, 7%)"},
    {"TitleBg", "$surface.base"},
    {"TitleBgActive", "$surface.raised"},
    {"TitleBgCollapsed", "alpha($surface.base, 75%)"},
    {"MenuBarBg", "$surface.raised"},
    {"ScrollbarBg", "alpha($surface.sunken, 60%)"},
    {"ScrollbarGrab", "$line.strong"},
    {"ScrollbarGrabHovered", "lighten($line.strong, 8%)"},
    {"ScrollbarGrabActive", "$accent"},
    {"CheckMark", "$accent.ink"},
    {"CheckboxSelectedBg", "$accent"},
    {"SliderGrab", "$accent"},
    {"SliderGrabActive", "$accent.active"},
    {"Button", "$accent"},
    {"ButtonHovered", "$accent.hover"},
    {"ButtonActive", "$accent.active"},
    {"Header", "$accent"},
    {"HeaderHovered", "$accent.hover"},
    {"HeaderActive", "$accent.active"},
    {"Separator", "$line.subtle"},
    {"SeparatorHovered", "$line.strong"},
    {"SeparatorActive", "$accent"},
    {"ResizeGrip", "alpha($line.strong, 40%)"},
    {"ResizeGripHovered", "alpha($accent, 70%)"},
    {"ResizeGripActive", "$accent"},
    {"InputTextCursor", "$ink.primary"},
    {"Tab", "mix($surface.base, $surface.raised, 50%)"},
    {"TabHovered", "$accent.hover"},
    {"TabSelected", "$accent"},
    {"TabSelectedOverline", "$accent.ink"},
    {"TabDimmed", "darken($surface.base, 2%)"},
    {"TabDimmedSelected", "mix($surface.raised, $accent, 30%)"},
    {"TabDimmedSelectedOverline", "$accent.muted"},
    {"DockingPreview", "$accent.muted"},
    {"DockingEmptyBg", "darken($surface.base, 2%)"},
    {"PlotLines", "$status.info"},
    {"PlotLinesHovered", "$status.warn"},
    {"PlotHistogram", "$accent"},
    {"PlotHistogramHovered", "$accent.hover"},
    {"TableHeaderBg", "$surface.raised"},
    {"TableBorderStrong", "$line.strong"},
    {"TableBorderLight", "$line.subtle"},
    {"TableRowBg", "alpha($surface.base, 0%)"},
    {"TableRowBgAlt", "alpha($ink.primary, 4%)"},
    {"TextLink", "$accent.ink"},
    {"TextSelectedBg", "$select.bg"},
    {"TreeLines", "$line.subtle"},
    {"DragDropTarget", "$status.warn"},
    {"DragDropTargetBg", "alpha($status.warn, 15%)"},
    {"UnsavedMarker", "$accent.muted"},
    {"NavCursor", "$accent"},
    {"NavWindowingHighlight", "alpha($ink.primary, 70%)"},
    {"NavWindowingDimBg", "$surface.overlay"},
    {"ModalWindowDimBg", "$surface.overlay"},
}};

/// Names ImGui has retired. A pack written against an older build keeps
/// working, because the colour it means still exists under a new spelling.
constexpr std::array<Entry, 4> kUiAliases = {{
    {"TabActive", "TabSelected"},
    {"TabUnfocused", "TabDimmed"},
    {"TabUnfocusedActive", "TabDimmedSelected"},
    {"NavHighlight", "NavCursor"},
}};

constexpr std::array<Entry, 4> kDiagnosticDefaults = {{
    {"error", "$status.error"},
    {"warning", "$status.warn"},
    {"info", "$status.info"},
    {"success", "$status.ok"},
}};

/// The node editor draws with its own list, so its colours are its own. The
/// literals are what graph_panel.cpp drew before packs existed, so the default
/// theme is the look the graph already had.
constexpr std::array<Entry, 20> kGraphDefaults = {{
    {"canvas", "darken($surface.base, 2%)"},
    {"grid", "alpha($ink.primary, 5%)"},
    {"node_bg", "#1e2026eb"},
    {"node_border", "#000000a0"},
    {"node_border_selected", "#ffd278"},
    {"node_title", "#ebebf0"},
    {"node_label", "#c8c8ce"},
    {"wire", "$line.strong"},
    {"wire_active", "$accent"},
    {"pin_unconnected", "#5a5a60"},
    {"pin.float", "#a0c8ff"},
    {"pin.float2", "#8ce6be"},
    {"pin.float3", "#e6c882"},
    {"pin.float4", "#f0a0aa"},
    {"pin.int", "#bebebe"},
    {"pin.bool", "#c8a0f0"},
    {"pin.float4x4", "#b4b4b4"},
    {"pin.texture2d", "#78dcf0"},
    {"pin.sampler", "#bebebe"},
    {"pin.any", "#bebebe"},
}};

constexpr std::array<Entry, 8> kGraphNodeDefaults = {{
    {"node.input", "#345476"},
    {"node.math", "#3a4a60"},
    {"node.utility", "#3e5c4c"},
    {"node.color", "#684454"},
    {"node.control", "#605434"},
    {"node.custom", "#563e6c"},
    {"node.output", "#6e4a38"},
    {"node.unknown", "#404048"},
}};

constexpr std::array<Entry, 4> kPreviewDefaults = {{
    {"checker_a", "#2a2a30"},
    {"checker_b", "#22222a"},
    {"border", "$line.subtle"},
    {"stats_ink", "$ink.muted"},
}};

/// The metrics whitelist. A value outside its range is clamped and warned
/// about, because a window_rounding of 400 is not a look anyone chose.
enum class StyleKind { Scalar, Vec2, Direction };

struct StyleField {
    std::string_view name;
    StyleKind kind;
    float min;
    float max;
};

constexpr std::array<StyleField, 37> kStyleFields = {{
    {"alpha", StyleKind::Scalar, 0.2f, 1.0f},
    {"disabled_alpha", StyleKind::Scalar, 0.1f, 1.0f},
    {"window_padding", StyleKind::Vec2, 0.0f, 32.0f},
    {"window_rounding", StyleKind::Scalar, 0.0f, 16.0f},
    {"window_border_size", StyleKind::Scalar, 0.0f, 3.0f},
    {"window_min_size", StyleKind::Vec2, 32.0f, 512.0f},
    {"window_title_align", StyleKind::Vec2, 0.0f, 1.0f},
    {"window_menu_button_position", StyleKind::Direction, 0.0f, 0.0f},
    {"child_rounding", StyleKind::Scalar, 0.0f, 16.0f},
    {"child_border_size", StyleKind::Scalar, 0.0f, 3.0f},
    {"popup_rounding", StyleKind::Scalar, 0.0f, 16.0f},
    {"popup_border_size", StyleKind::Scalar, 0.0f, 3.0f},
    {"frame_padding", StyleKind::Vec2, 0.0f, 24.0f},
    {"frame_rounding", StyleKind::Scalar, 0.0f, 16.0f},
    {"frame_border_size", StyleKind::Scalar, 0.0f, 3.0f},
    {"item_spacing", StyleKind::Vec2, 0.0f, 24.0f},
    {"item_inner_spacing", StyleKind::Vec2, 0.0f, 24.0f},
    {"cell_padding", StyleKind::Vec2, 0.0f, 24.0f},
    {"indent_spacing", StyleKind::Scalar, 0.0f, 48.0f},
    {"scrollbar_size", StyleKind::Scalar, 6.0f, 24.0f},
    {"scrollbar_rounding", StyleKind::Scalar, 0.0f, 16.0f},
    {"grab_min_size", StyleKind::Scalar, 4.0f, 32.0f},
    {"grab_rounding", StyleKind::Scalar, 0.0f, 16.0f},
    {"image_rounding", StyleKind::Scalar, 0.0f, 16.0f},
    {"tab_rounding", StyleKind::Scalar, 0.0f, 16.0f},
    {"tab_border_size", StyleKind::Scalar, 0.0f, 3.0f},
    {"tab_bar_border_size", StyleKind::Scalar, 0.0f, 4.0f},
    {"tab_bar_overline_size", StyleKind::Scalar, 0.0f, 4.0f},
    {"menu_item_rounding", StyleKind::Scalar, 0.0f, 16.0f},
    {"separator_size", StyleKind::Scalar, 1.0f, 4.0f},
    {"separator_text_border_size", StyleKind::Scalar, 0.0f, 4.0f},
    {"tree_lines_size", StyleKind::Scalar, 0.0f, 3.0f},
    {"tree_lines_rounding", StyleKind::Scalar, 0.0f, 16.0f},
    {"button_text_align", StyleKind::Vec2, 0.0f, 1.0f},
    {"selectable_text_align", StyleKind::Vec2, 0.0f, 1.0f},
    {"input_text_cursor_size", StyleKind::Scalar, 1.0f, 4.0f},
    {"color_button_position", StyleKind::Direction, 0.0f, 0.0f},
}};

const StyleField* style_field(std::string_view name) {
    for (const auto& field : kStyleFields) {
        if (field.name == name) return &field;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// The value grammar
// ---------------------------------------------------------------------------

/// Splits on commas that are not inside parentheses, so mix($a, alpha($b, 20%),
/// 30%) reads as three arguments rather than four.
std::vector<std::string_view> split_arguments(std::string_view text) {
    std::vector<std::string_view> out;
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '(') ++depth;
        else if (text[i] == ')') --depth;
        else if (text[i] == ',' && depth == 0) {
            out.push_back(trimmed(text.substr(start, i - start)));
            start = i + 1;
        }
    }
    out.push_back(trimmed(text.substr(start)));
    return out;
}

bool parse_percent(std::string_view text, float& out) {
    text = trimmed(text);
    if (text.empty()) return false;
    const bool percent = text.back() == '%';
    if (percent) text.remove_suffix(1);
    const std::string number(trimmed(text));
    if (number.empty()) return false;
    char* end = nullptr;
    const double value = std::strtod(number.c_str(), &end);
    if (end != number.c_str() + number.size()) return false;
    out = static_cast<float>(percent ? value / 100.0 : value);
    return true;
}

bool eval_value(std::string_view expression, const std::map<std::string, Rgba>& roles,
                const std::map<std::string, Rgba>& palette, int depth, Rgba& out,
                std::string& error);

bool eval_reference(std::string_view name, const std::map<std::string, Rgba>& roles,
                    const std::map<std::string, Rgba>& palette, Rgba& out, std::string& error) {
    // "$roles.x" and "$palette.x" say which table to look in; a bare name looks
    // in roles first, because that is the layer a pack is expected to work in.
    if (name.rfind("roles.", 0) == 0) {
        const auto it = roles.find(std::string(name.substr(6)));
        if (it != roles.end()) {
            out = it->second;
            return true;
        }
        error = "no role named '" + std::string(name.substr(6)) + "'";
        return false;
    }
    if (name.rfind("palette.", 0) == 0) {
        const auto it = palette.find(std::string(name.substr(8)));
        if (it != palette.end()) {
            out = it->second;
            return true;
        }
        error = "no palette colour named '" + std::string(name.substr(8)) + "'";
        return false;
    }
    const std::string key(name);
    if (const auto it = roles.find(key); it != roles.end()) {
        out = it->second;
        return true;
    }
    if (const auto it = palette.find(key); it != palette.end()) {
        out = it->second;
        return true;
    }
    error = "'" + key + "' is not a role or a palette colour";
    return false;
}

bool eval_call(std::string_view name, std::string_view body,
               const std::map<std::string, Rgba>& roles,
               const std::map<std::string, Rgba>& palette, int depth, Rgba& out,
               std::string& error) {
    const std::vector<std::string_view> args = split_arguments(body);
    const auto colour = [&](std::size_t index, Rgba& value) {
        return index < args.size() &&
               eval_value(args[index], roles, palette, depth + 1, value, error);
    };
    const auto amount = [&](std::size_t index, float& value) {
        if (index < args.size() && parse_percent(args[index], value)) return true;
        error = "'" + std::string(name) + "' needs an amount like \"40%\"";
        return false;
    };

    Rgba a = 0, b = 0;
    float k = 0.0f;
    if (name == "alpha") {
        if (args.size() != 2 || !colour(0, a) || !amount(1, k)) return false;
        out = color_alpha(a, k);
        return true;
    }
    if (name == "lighten" || name == "darken") {
        if (args.size() != 2 || !colour(0, a) || !amount(1, k)) return false;
        out = name == "lighten" ? color_lighten(a, k) : color_darken(a, k);
        return true;
    }
    if (name == "mix") {
        if (args.size() != 3 || !colour(0, a) || !colour(1, b) || !amount(2, k)) return false;
        out = color_mix(a, b, k);
        return true;
    }
    error = "unknown transform '" + std::string(name) + "'";
    return false;
}

bool eval_value(std::string_view expression, const std::map<std::string, Rgba>& roles,
                const std::map<std::string, Rgba>& palette, int depth, Rgba& out,
                std::string& error) {
    // Four is deep enough for mix(alpha($a, 50%), lighten($b, 5%), 30%) and
    // shallow enough that a pack cannot make resolving expensive.
    if (depth > 4) {
        error = "expression nested more than four deep";
        return false;
    }
    const std::string_view text = trimmed(expression);
    if (text.empty()) {
        error = "empty value";
        return false;
    }
    if (text.front() == '$') return eval_reference(text.substr(1), roles, palette, out, error);
    if (const std::size_t open = text.find('('); open != std::string_view::npos) {
        if (text.back() != ')') {
            error = "unbalanced parentheses in '" + std::string(text) + "'";
            return false;
        }
        return eval_call(trimmed(text.substr(0, open)),
                         text.substr(open + 1, text.size() - open - 2), roles, palette, depth, out,
                         error);
    }
    if (color_rgba_from_hex(text, out)) return true;
    error = "'" + std::string(text) + "' is not a colour, a $reference or a transform";
    return false;
}

/// Evaluates a table of expressions into a table of colours, warning about the
/// ones that cannot be evaluated and leaving those out - a pack with one bad
/// colour is a pack with one derived colour, not a broken theme.
void eval_table(const std::map<std::string, std::string>& expressions,
                const std::map<std::string, Rgba>& roles,
                const std::map<std::string, Rgba>& palette, const std::string& file,
                std::string_view section, std::map<std::string, Rgba>& out, Diagnostics& diags) {
    for (const auto& [key, expression] : expressions) {
        Rgba value = 0;
        std::string error;
        if (eval_value(expression, roles, palette, 0, value, error)) {
            out[key] = value;
        } else {
            diags.push_back(problem(Severity::Warning,
                                    std::string(section) + "." + key + ": " + error, file));
        }
    }
}

// ---------------------------------------------------------------------------
// Built-in role sets
//
// The built-in themes are still applied through ImGui's own styles, so these
// numbers do not decide what dark, light and classic look like. They decide
// what a pack starts from when it inherits one, and they are read off the
// colours those themes actually use so that inheriting `dark` and setting
// nothing looks like dark.
// ---------------------------------------------------------------------------

constexpr std::array<Entry, 19> kDarkRoles = {{
    {"surface.base", "#17171a"},
    {"surface.raised", "#1c1c1f"},
    {"surface.sunken", "#29292e"},
    {"surface.overlay", "#17171a99"},
    {"ink.primary", "#f2f2f2"},
    {"ink.muted", "#808080"},
    {"line.subtle", "#3a3a42"},
    {"line.strong", "#55555f"},
    {"accent", "#33384a"},
    {"accent.hover", "#3f4558"},
    {"accent.active", "#4a5166"},
    {"accent.muted", "#5980bfb3"},
    {"accent.ink", "#4296fa"},
    {"select.bg", "#4296fa59"},
    {"select.ink", "#f2f2f2"},
    {"status.ok", "#6fbf73"},
    {"status.warn", "#e0af68"},
    {"status.error", "#eb6a6a"},
    {"status.info", "#78aae6"},
}};

constexpr std::array<Entry, 19> kLightRoles = {{
    {"surface.base", "#f0f0f0"},
    {"surface.raised", "#ffffff"},
    {"surface.sunken", "#ffffff"},
    {"surface.overlay", "#c8c8c899"},
    {"ink.primary", "#000000"},
    {"ink.muted", "#656d76"},
    {"line.subtle", "#d9d9d9"},
    {"line.strong", "#bfbfbf"},
    {"accent", "#4296fa"},
    {"accent.hover", "#5aa4ff"},
    {"accent.active", "#2b7fe0"},
    {"accent.muted", "#4296fa40"},
    {"accent.ink", "#ffffff"},
    {"select.bg", "#4296fa59"},
    {"select.ink", "#000000"},
    {"status.ok", "#146c2e"},
    {"status.warn", "#8f5b00"},
    {"status.error", "#cf222e"},
    {"status.info", "#0550ae"},
}};

constexpr std::array<Entry, 19> kClassicRoles = {{
    {"surface.base", "#14141c"},
    {"surface.raised", "#23232e"},
    {"surface.sunken", "#2b2b38"},
    {"surface.overlay", "#14141c99"},
    {"ink.primary", "#e6e6e6"},
    {"ink.muted", "#999999"},
    {"line.subtle", "#4a4a5c"},
    {"line.strong", "#64647a"},
    {"accent", "#4b5688"},
    {"accent.hover", "#5c6899"},
    {"accent.active", "#6d7aab"},
    {"accent.muted", "#4b568859"},
    {"accent.ink", "#e6e6e6"},
    {"select.bg", "#59669c73"},
    {"select.ink", "#e6e6e6"},
    {"status.ok", "#6fbf73"},
    {"status.warn", "#e0af68"},
    {"status.error", "#eb6a6a"},
    {"status.info", "#8f9fd9"},
}};

/// The palette a kind falls back to: what the user picked when they picked one,
/// otherwise what the theme's appearance implies. One rule covers a partial
/// [syntax] block and a missing one.
std::array<Rgba, kTokenKindCount> fallback_syntax(const std::string& palette_name,
                                                  std::string_view appearance) {
    const std::vector<std::string>& known = syntax_palette_names();
    std::string chosen = palette_name;
    if (chosen.empty() || std::find(known.begin(), known.end(), chosen) == known.end()) {
        chosen = appearance == "light" ? "light" : "default";
    }
    const SyntaxColors colors = syntax_palette(chosen);
    std::array<Rgba, kTokenKindCount> out{};
    for (std::size_t i = 0; i < kTokenKindCount; ++i) {
        out[i] = (colors.rgb[i] << 8) | 0xFFu;  // stored as 0xRRGGBB, painted opaque
    }
    return out;
}

/// Whether a colour is closer to white than to black, used to infer
/// `appearance` for a pack that does not declare one.
bool looks_light(Rgba surface) {
    return color_contrast(0xFFFFFFFFu, surface) < color_contrast(0x000000FFu, surface);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public tables
// ---------------------------------------------------------------------------

bool theme_is_builtin(std::string_view id) {
    return id == "dark" || id == "light" || id == "classic";
}

const std::vector<std::string>& theme_role_names() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> v;
        for (const auto& entry : kRoleDefaults) v.emplace_back(entry.name);
        return v;
    }();
    return names;
}

const std::vector<std::string>& theme_ui_color_names() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> v;
        for (const auto& entry : kUiDerivations) v.emplace_back(entry.name);
        return v;
    }();
    return names;
}

std::string_view theme_ui_color_alias(std::string_view name) {
    for (const auto& entry : kUiAliases) {
        if (entry.name == name) return entry.expression;
    }
    return {};
}

std::map<std::string, Rgba> builtin_theme_roles(std::string_view id) {
    const std::array<Entry, 19>* table = nullptr;
    if (id == "dark") table = &kDarkRoles;
    else if (id == "light") table = &kLightRoles;
    else if (id == "classic") table = &kClassicRoles;
    if (table == nullptr) return {};

    std::map<std::string, Rgba> out;
    for (const auto& entry : *table) {
        Rgba value = 0;
        if (color_rgba_from_hex(entry.expression, value)) out[std::string(entry.name)] = value;
    }
    return out;
}

bool eval_theme_color(std::string_view expression, const std::map<std::string, Rgba>& roles,
                      const std::map<std::string, Rgba>& palette, Rgba& out, std::string& error) {
    return eval_value(expression, roles, palette, 0, out, error);
}

// ---------------------------------------------------------------------------
// ResolvedTheme lookups
// ---------------------------------------------------------------------------

namespace {

Rgba lookup(const std::map<std::string, Rgba>& table, std::string_view name, Rgba fallback) {
    const auto it = table.find(std::string(name));
    return it == table.end() ? fallback : it->second;
}

}  // namespace

Rgba ResolvedTheme::role(std::string_view name, Rgba fallback) const {
    return lookup(roles, name, fallback);
}

Rgba ResolvedTheme::ui_color(std::string_view name, Rgba fallback) const {
    return lookup(ui, name, fallback);
}

Rgba ResolvedTheme::graph_color(std::string_view name, Rgba fallback) const {
    return lookup(graph, name, fallback);
}

Rgba ResolvedTheme::preview_color(std::string_view name, Rgba fallback) const {
    return lookup(preview, name, fallback);
}

Rgba ResolvedTheme::diagnostic(Severity severity) const {
    switch (severity) {
        case Severity::Error: return lookup(diagnostics, "error", 0xeb6a6aFFu);
        case Severity::Warning: return lookup(diagnostics, "warning", 0xe0af68FFu);
        case Severity::Info: return lookup(diagnostics, "info", 0x78aae6FFu);
    }
    return lookup(diagnostics, "info", 0x78aae6FFu);
}

Rgba ResolvedTheme::success() const { return lookup(diagnostics, "success", 0x6fbf73FFu); }

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

namespace {

/// Reads a table of string values, warning about anything that is not one.
/// `allowed` empty means any key is allowed, which is what the palette and the
/// graph's per-type tables need: their key sets grow with the shader language,
/// not with this format.
void read_string_table(const toml::table* table, const std::vector<std::string>& allowed,
                       const std::string& file, std::string_view section,
                       std::map<std::string, std::string>& out, Diagnostics& diags) {
    if (table == nullptr) return;
    for (const auto& [key, node] : *table) {
        const std::string name(key.str());
        const auto value = node.value<std::string>();
        if (!value) {
            diags.push_back(problem(Severity::Warning,
                                    std::string(section) + "." + name + " is not a string", file));
            continue;
        }
        if (!allowed.empty() &&
            std::find(allowed.begin(), allowed.end(), name) == allowed.end()) {
            diags.push_back(problem(Severity::Warning,
                                    "unknown key " + std::string(section) + "." + name, file));
            continue;
        }
        out[name] = *value;
    }
}

void read_style(const toml::table* table, const std::string& file, ThemeStyle& out,
                Diagnostics& diags) {
    if (table == nullptr) return;
    for (const auto& [key, node] : *table) {
        const std::string name(key.str());
        const StyleField* field = style_field(name);
        if (field == nullptr) {
            diags.push_back(problem(Severity::Warning, "unknown key style." + name, file));
            continue;
        }
        switch (field->kind) {
            case StyleKind::Scalar: {
                const auto value = node.value<double>();
                if (!value) {
                    diags.push_back(
                        problem(Severity::Warning, "style." + name + " is not a number", file));
                    break;
                }
                const float clamped =
                    std::clamp(static_cast<float>(*value), field->min, field->max);
                if (clamped != static_cast<float>(*value)) {
                    diags.push_back(problem(Severity::Warning,
                                            "style." + name + " clamped to " +
                                                std::to_string(clamped),
                                            file));
                }
                out.scalar[name] = clamped;
                break;
            }
            case StyleKind::Vec2: {
                const auto* pair = node.as_array();
                if (pair == nullptr || pair->size() != 2) {
                    diags.push_back(problem(Severity::Warning,
                                            "style." + name + " is not a pair like [10, 10]",
                                            file));
                    break;
                }
                std::array<float, 2> values{};
                bool ok = true;
                for (std::size_t i = 0; i < 2; ++i) {
                    const auto value = (*pair)[i].value<double>();
                    if (!value) {
                        ok = false;
                        break;
                    }
                    values[i] = std::clamp(static_cast<float>(*value), field->min, field->max);
                }
                if (!ok) {
                    diags.push_back(problem(Severity::Warning,
                                            "style." + name + " holds something that is not a "
                                                              "number",
                                            file));
                    break;
                }
                out.vec2[name] = values;
                break;
            }
            case StyleKind::Direction: {
                const auto value = node.value<std::string>();
                static const std::array<std::string_view, 5> kDirections = {"none", "left", "right",
                                                                            "up", "down"};
                if (!value || std::find(kDirections.begin(), kDirections.end(), *value) ==
                                  kDirections.end()) {
                    diags.push_back(problem(Severity::Warning,
                                            "style." + name +
                                                " is not one of none, left, right, up, down",
                                            file));
                    break;
                }
                out.direction[name] = *value;
                break;
            }
        }
    }
}

/// A font path may not leave the pack. Checked before the file is opened rather
/// than after: a pack that reads someone's home directory works on exactly one
/// machine, and finding that out at load time is the point.
bool font_path_is_contained(std::string_view value) {
    if (value.empty()) return false;
    const std::filesystem::path path(value);
    if (path.is_absolute() || path.has_root_name()) return false;
    for (const auto& part : path) {
        if (part == "..") return false;
    }
    return true;
}

}  // namespace

std::string theme_pack_id(const std::filesystem::path& path) {
    std::string stem = path.filename().string();
    // A single-file pack is "<id>.s3theme"; a directory pack is the same name
    // without a file inside it having to repeat the id.
    const std::string suffix(kThemePackExtension);
    if (stem.size() <= suffix.size()) return {};
    if (lowered(stem.substr(stem.size() - suffix.size())) != suffix) return {};
    stem = lowered(stem.substr(0, stem.size() - suffix.size()));

    if (stem.empty() || !(std::isalnum(static_cast<unsigned char>(stem.front())))) return {};
    for (char c : stem) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
        if (!ok) return {};
    }
    return stem;
}

bool parse_theme_pack_text(std::string_view text, std::string id, ThemePack& out,
                           Diagnostics& diags) {
    out = ThemePack{};
    out.id = lowered(id);

    toml::table tbl;
    try {
        tbl = toml::parse(text);
    } catch (const toml::parse_error& e) {
        Diagnostic d = problem(Severity::Error, std::string(e.description()), out.id);
        d.line = static_cast<int>(e.source().begin.line);
        d.column = static_cast<int>(e.source().begin.column);
        diags.push_back(std::move(d));
        return false;
    }

    const auto* pack = tbl["pack"].as_table();
    if (pack == nullptr) {
        diags.push_back(problem(Severity::Error, "no [pack] table", out.id));
        return false;
    }
    const auto format = (*pack)["format"].value<std::int64_t>();
    if (!format) {
        diags.push_back(problem(Severity::Error, "[pack] has no format", out.id));
        return false;
    }
    if (*format != kThemePackFormat) {
        diags.push_back(problem(Severity::Error,
                                "theme pack format " + std::to_string(*format) +
                                    " is not readable by this build, which reads format " +
                                    std::to_string(kThemePackFormat),
                                out.id));
        return false;
    }
    out.format = static_cast<int>(*format);

    out.name = (*pack)["name"].value_or(out.id);
    out.author = (*pack)["author"].value_or(std::string{});
    out.version = (*pack)["version"].value_or(std::string{});
    out.description = (*pack)["description"].value_or(std::string{});
    out.homepage = (*pack)["homepage"].value_or(std::string{});
    out.appearance = lowered((*pack)["appearance"].value_or(std::string{}));
    out.inherit = lowered((*pack)["inherit"].value_or(std::string{"dark"}));

    if (!out.appearance.empty() && out.appearance != "dark" && out.appearance != "light") {
        diags.push_back(problem(Severity::Warning,
                                "pack.appearance is not \"dark\" or \"light\"; inferring it",
                                out.id));
        out.appearance.clear();
    }
    if (theme_is_builtin(out.id)) {
        diags.push_back(problem(Severity::Error,
                                "a pack may not use the id '" + out.id +
                                    "', which is a built-in theme; inherit from it instead",
                                out.id));
        return false;
    }

    read_string_table(tbl["palette"].as_table(), {}, out.id, "palette", out.palette, diags);
    read_string_table(tbl["roles"].as_table(), theme_role_names(), out.id, "roles", out.roles,
                      diags);

    // Colours carry ImGui's own spelling, and a name it has retired still means
    // the colour it always meant.
    if (const auto* colors = tbl["colors"].as_table()) {
        std::map<std::string, std::string> raw;
        read_string_table(colors, {}, out.id, "colors", raw, diags);
        for (const auto& [key, value] : raw) {
            std::string name = key;
            if (const std::string_view now = theme_ui_color_alias(name); !now.empty()) {
                name = std::string(now);
            }
            const std::vector<std::string>& known = theme_ui_color_names();
            if (std::find(known.begin(), known.end(), name) == known.end()) {
                diags.push_back(problem(Severity::Warning, "unknown key colors." + key, out.id));
                continue;
            }
            out.ui[name] = value;
        }
    }

    std::vector<std::string> kinds;
    for (std::size_t i = 0; i < kTokenKindCount; ++i) {
        kinds.emplace_back(to_string(static_cast<TokenKind>(i)));
    }
    read_string_table(tbl["syntax"].as_table(), kinds, out.id, "syntax", out.syntax, diags);
    read_string_table(tbl["diagnostics"].as_table(), {"error", "warning", "info", "success"},
                      out.id, "diagnostics", out.diagnostics, diags);

    if (const auto* graph = tbl["graph"].as_table()) {
        for (const auto& [key, node] : *graph) {
            const std::string name(key.str());
            // [graph.pins] and [graph.nodes] are sub-tables; everything else in
            // [graph] is a colour of the canvas itself.
            if (name == "pins" || name == "nodes") {
                const std::string prefix = name == "pins" ? "pin." : "node.";
                std::map<std::string, std::string> raw;
                read_string_table(node.as_table(), {}, out.id, "graph." + name, raw, diags);
                for (const auto& [type, value] : raw) out.graph[prefix + lowered(type)] = value;
                continue;
            }
            const auto value = node.value<std::string>();
            if (!value) {
                diags.push_back(
                    problem(Severity::Warning, "graph." + name + " is not a string", out.id));
                continue;
            }
            out.graph[name] = *value;
        }
    }

    if (const auto* preview = tbl["preview"].as_table()) {
        for (const auto& [key, node] : *preview) {
            const std::string name(key.str());
            if (name == "checker_size") {
                const auto value = node.value<double>();
                if (!value) {
                    diags.push_back(problem(Severity::Warning,
                                            "preview.checker_size is not a number", out.id));
                    continue;
                }
                out.checker_size = std::clamp(static_cast<float>(*value), 4.0f, 32.0f);
                continue;
            }
            const auto value = node.value<std::string>();
            if (!value) {
                diags.push_back(
                    problem(Severity::Warning, "preview." + name + " is not a string", out.id));
                continue;
            }
            out.preview[name] = *value;
        }
    }

    read_style(tbl["style"].as_table(), out.id, out.style, diags);

    if (const auto* font = tbl["font"].as_table()) {
        out.font_ui = (*font)["ui"].value_or(std::string{});
        out.font_editor = (*font)["editor"].value_or(std::string{});
        out.font_ui_size = static_cast<float>((*font)["ui_size"].value_or(0.0));
        out.font_editor_size = static_cast<float>((*font)["editor_size"].value_or(0.0));
        for (const std::string* path : {&out.font_ui, &out.font_editor}) {
            if (!path->empty() && !font_path_is_contained(*path)) {
                diags.push_back(problem(Severity::Error,
                                        "font path '" + *path +
                                            "' leaves the pack directory, which would make the "
                                            "pack work on one machine only",
                                        out.id));
                return false;
            }
        }
    }
    return true;
}

bool parse_theme_pack(const std::filesystem::path& path, ThemePack& out, Diagnostics& diags) {
    const std::string id = theme_pack_id(path);
    if (id.empty()) {
        diags.push_back(problem(Severity::Error,
                                "'" + path.filename().string() + "' is not a theme pack name",
                                path.string()));
        return false;
    }

    std::error_code ec;
    std::filesystem::path file = path;
    std::filesystem::path dir;
    if (std::filesystem::is_directory(path, ec)) {
        dir = path;
        file = path / "theme.toml";
    }

    std::ifstream in(file, std::ios::binary);
    if (!in) {
        diags.push_back(problem(Severity::Error, "cannot open " + file.string(), path.string()));
        return false;
    }
    std::ostringstream text;
    text << in.rdbuf();

    if (!parse_theme_pack_text(text.str(), id, out, diags)) {
        // The diagnostics carry the id; give them the path as well, now that
        // there is one, so the message names a file the user can open.
        for (auto& d : diags) {
            if (d.file == id) d.file = file.string();
        }
        return false;
    }
    for (auto& d : diags) {
        if (d.file == id) d.file = file.string();
    }
    out.file = file;
    out.dir = dir;
    if (!dir.empty()) {
        if (!out.font_ui.empty()) out.font_ui = (dir / out.font_ui).string();
        if (!out.font_editor.empty()) out.font_editor = (dir / out.font_editor).string();
    } else if (!out.font_ui.empty() || !out.font_editor.empty()) {
        diags.push_back(problem(Severity::Warning,
                                "[font] needs a pack directory for its paths to be relative to; "
                                "ignoring it",
                                file.string()));
        out.font_ui.clear();
        out.font_editor.clear();
    }
    return true;
}

// ---------------------------------------------------------------------------
// Resolving
// ---------------------------------------------------------------------------

namespace {

/// Evaluates role expressions against the roles resolved so far, retrying the
/// ones that referred forwards until nothing moves. A role may therefore
/// reference another role written below it; a set that still cannot be resolved
/// after a pass that changed nothing is a cycle.
///
/// One subtlety decides two behaviours at once. The roles map arrives holding
/// the inherited theme's values, so a name this pack also redefines could be
/// read either as "the parent's" or as "mine", and which one won would depend
/// on the order the keys happened to be visited. So while evaluating a role,
/// every *other* role this pack redefines is hidden:
///
///   - `accent = "lighten($accent, 5%)"` means the inherited accent, lightened,
///     which is how a pack tweaks its parent rather than restating it;
///   - `accent = "$accent.hover"` with `accent.hover = "$accent"` cannot see
///     each other's inherited values, makes no progress, and is reported as the
///     cycle it is instead of resolving to whichever was visited first.
bool resolve_roles(const std::map<std::string, std::string>& expressions,
                   const std::map<std::string, Rgba>& palette, const std::string& file,
                   std::map<std::string, Rgba>& roles, Diagnostics& diags) {
    std::map<std::string, std::string> pending = expressions;
    while (!pending.empty()) {
        std::map<std::string, std::string> retry;
        std::string last_error;
        for (const auto& [name, expression] : pending) {
            std::map<std::string, Rgba> visible = roles;
            for (const auto& [other, unused] : pending) {
                if (other != name) visible.erase(other);
            }
            Rgba value = 0;
            std::string error;
            if (eval_value(expression, visible, palette, 0, value, error)) {
                roles[name] = value;
            } else {
                retry.emplace(name, expression);
                last_error = "roles." + name + ": " + error;
            }
        }
        if (retry.size() == pending.size()) {
            std::string names;
            for (const auto& [name, expression] : retry) {
                if (!names.empty()) names += ", ";
                names += name;
            }
            diags.push_back(problem(Severity::Error,
                                    "roles cannot be resolved (a cycle, or a name that does not "
                                    "exist) among: " +
                                        names + " - " + last_error,
                                    file));
            return false;
        }
        pending = std::move(retry);
    }
    return true;
}

/// Fills a table from its defaults for every key the pack did not set.
void apply_defaults(const std::vector<Entry>& defaults, const std::map<std::string, Rgba>& roles,
                    const std::map<std::string, Rgba>& palette, const std::string& file,
                    std::map<std::string, Rgba>& out, Diagnostics& diags) {
    for (const auto& entry : defaults) {
        const std::string name(entry.name);
        if (out.count(name) != 0) continue;
        Rgba value = 0;
        std::string error;
        if (eval_value(entry.expression, roles, palette, 0, value, error)) {
            out[name] = value;
        } else {
            // A derivation that cannot be evaluated is this module's bug, not
            // the pack's, so it is worth saying out loud rather than silently
            // leaving a colour black.
            diags.push_back(problem(Severity::Warning,
                                    "derivation for '" + name + "' failed: " + error, file));
        }
    }
}

template <std::size_t N>
void append(std::vector<Entry>& out, const std::array<Entry, N>& table) {
    out.insert(out.end(), table.begin(), table.end());
}

const std::vector<Entry>& graph_defaults() {
    static const std::vector<Entry> table = [] {
        std::vector<Entry> v;
        append(v, kGraphDefaults);
        append(v, kGraphNodeDefaults);
        return v;
    }();
    return table;
}

const std::vector<Entry>& ui_defaults() {
    static const std::vector<Entry> table = [] {
        std::vector<Entry> v;
        append(v, kUiDerivations);
        return v;
    }();
    return table;
}

const std::vector<Entry>& diagnostic_defaults() {
    static const std::vector<Entry> table = [] {
        std::vector<Entry> v;
        append(v, kDiagnosticDefaults);
        return v;
    }();
    return table;
}

const std::vector<Entry>& preview_defaults() {
    static const std::vector<Entry> table = [] {
        std::vector<Entry> v;
        append(v, kPreviewDefaults);
        return v;
    }();
    return table;
}

/// `ink.inverted` is a choice between two colours the theme already declares
/// rather than a formula: a pale accent needs dark text on it and a deep one
/// needs light text, and picking the higher-contrast of the surface and the ink
/// gets that right without the pack having to think about it.
Rgba inverted_ink(const std::map<std::string, Rgba>& roles) {
    const Rgba surface = lookup(roles, "surface.base", 0x000000FFu);
    const Rgba ink = lookup(roles, "ink.primary", 0xFFFFFFFFu);
    const Rgba accent = lookup(roles, "accent", 0x000000FFu);
    return color_contrast(surface, accent) >= color_contrast(ink, accent) ? surface : ink;
}

}  // namespace

ResolvedTheme builtin_resolved_theme(std::string_view id,
                                     const std::string& fallback_syntax_palette) {
    ResolvedTheme out;
    out.id = std::string(id.empty() ? std::string_view("dark") : id);
    out.name = out.id;
    out.builtin = true;
    out.roles = builtin_theme_roles(out.id);
    if (out.roles.empty()) {
        out.id = "dark";
        out.name = "dark";
        out.roles = builtin_theme_roles("dark");
    }
    out.appearance = out.id == "light" ? "light" : "dark";
    out.roles["ink.inverted"] = inverted_ink(out.roles);
    out.syntax = fallback_syntax(fallback_syntax_palette, out.appearance);

    Diagnostics ignored;
    const std::map<std::string, Rgba> no_palette;
    apply_defaults(diagnostic_defaults(), out.roles, no_palette, out.id, out.diagnostics, ignored);
    apply_defaults(graph_defaults(), out.roles, no_palette, out.id, out.graph, ignored);
    apply_defaults(preview_defaults(), out.roles, no_palette, out.id, out.preview, ignored);
    out.checker_size = 8.0f;
    return out;
}

bool resolve_theme(const ThemePack& pack, const ThemeResolveContext& context, ResolvedTheme& out,
                   Diagnostics& diags) {
    const std::string file = pack.file.empty() ? pack.id : pack.file.string();

    // Walk `inherit` to a built-in, collecting the chain. Newest first here;
    // applied oldest first below, so a child overrides its parent.
    std::vector<const ThemePack*> chain{&pack};
    std::set<std::string> seen{pack.id};
    std::string base = "dark";
    for (int depth = 0; depth < 8; ++depth) {
        const ThemePack* current = chain.back();
        if (theme_is_builtin(current->inherit)) {
            base = current->inherit;
            break;
        }
        if (!seen.insert(current->inherit).second) {
            diags.push_back(problem(Severity::Error,
                                    "inherit cycle: '" + current->id + "' inherits '" +
                                        current->inherit + "', which is already in the chain",
                                    file));
            out = builtin_resolved_theme("dark", context.fallback_syntax_palette);
            return false;
        }
        const auto it = context.available.find(current->inherit);
        if (it == context.available.end()) {
            diags.push_back(problem(Severity::Warning,
                                    "'" + current->id + "' inherits '" + current->inherit +
                                        "', which is not installed; using dark",
                                    file));
            break;
        }
        chain.push_back(&it->second);
        if (depth == 7) {
            diags.push_back(problem(Severity::Error,
                                    "inherit chain deeper than eight packs", file));
            out = builtin_resolved_theme("dark", context.fallback_syntax_palette);
            return false;
        }
    }

    // Merge every layer's text, oldest first, so that resolving happens once
    // against the final set rather than once per layer. That is what lets a
    // child's role change re-derive a parent's widget colours instead of
    // leaving them where the parent computed them.
    std::map<std::string, std::string> palette_text, role_text, ui_text, syntax_text;
    std::map<std::string, std::string> diagnostic_text, graph_text, preview_text;
    ThemeStyle style;
    std::string appearance;
    float checker_size = 0.0f;
    std::filesystem::path font_ui, font_editor;
    float font_ui_size = 0.0f, font_editor_size = 0.0f;

    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const ThemePack& layer = **it;
        for (const auto& [k, v] : layer.palette) palette_text[k] = v;
        for (const auto& [k, v] : layer.roles) role_text[k] = v;
        for (const auto& [k, v] : layer.ui) ui_text[k] = v;
        for (const auto& [k, v] : layer.syntax) syntax_text[k] = v;
        for (const auto& [k, v] : layer.diagnostics) diagnostic_text[k] = v;
        for (const auto& [k, v] : layer.graph) graph_text[k] = v;
        for (const auto& [k, v] : layer.preview) preview_text[k] = v;
        for (const auto& [k, v] : layer.style.scalar) style.scalar[k] = v;
        for (const auto& [k, v] : layer.style.vec2) style.vec2[k] = v;
        for (const auto& [k, v] : layer.style.direction) style.direction[k] = v;
        if (!layer.appearance.empty()) appearance = layer.appearance;
        if (layer.checker_size > 0.0f) checker_size = layer.checker_size;
        if (!layer.font_ui.empty()) font_ui = layer.font_ui;
        if (!layer.font_editor.empty()) font_editor = layer.font_editor;
        if (layer.font_ui_size > 0.0f) font_ui_size = layer.font_ui_size;
        if (layer.font_editor_size > 0.0f) font_editor_size = layer.font_editor_size;
    }

    std::map<std::string, Rgba> palette;
    for (const auto& [name, text] : palette_text) {
        Rgba value = 0;
        if (color_rgba_from_hex(text, value)) {
            palette[name] = value;
        } else {
            diags.push_back(problem(Severity::Warning,
                                    "palette." + name + ": '" + text +
                                        "' is not a hex colour (the palette is the ground the "
                                        "rest of the cascade stands on, so it may only be hex)",
                                    file));
        }
    }

    out = ResolvedTheme{};
    out.id = pack.id;
    out.name = pack.name.empty() ? pack.id : pack.name;
    out.builtin = false;
    out.roles = builtin_theme_roles(base);
    out.style = style;

    if (!resolve_roles(role_text, palette, file, out.roles, diags)) {
        out = builtin_resolved_theme(base, context.fallback_syntax_palette);
        return false;
    }
    // Computed rather than inherited whenever the pack does not name it: the
    // right answer depends on the accent, and a pack that changed its accent
    // would otherwise keep an inverted ink chosen for a different one.
    if (role_text.count("ink.inverted") == 0) out.roles["ink.inverted"] = inverted_ink(out.roles);

    out.appearance =
        !appearance.empty() ? appearance
                            : (looks_light(out.role("surface.base")) ? "light" : "dark");

    eval_table(ui_text, out.roles, palette, file, "colors", out.ui, diags);
    apply_defaults(ui_defaults(), out.roles, palette, file, out.ui, diags);

    out.syntax = fallback_syntax(context.fallback_syntax_palette, out.appearance);
    for (const auto& [name, expression] : syntax_text) {
        TokenKind kind{};
        if (!token_kind_from_string(name, kind)) continue;  // the reader already warned
        Rgba value = 0;
        std::string error;
        if (eval_value(expression, out.roles, palette, 0, value, error)) {
            // Alpha is ignored: the editor draws text, and a half-transparent
            // keyword reads as a rendering bug rather than as a style.
            out.syntax[static_cast<std::size_t>(kind)] = color_alpha(value, 1.0f);
        } else {
            diags.push_back(problem(Severity::Warning, "syntax." + name + ": " + error, file));
        }
    }

    eval_table(diagnostic_text, out.roles, palette, file, "diagnostics", out.diagnostics, diags);
    apply_defaults(diagnostic_defaults(), out.roles, palette, file, out.diagnostics, diags);
    eval_table(graph_text, out.roles, palette, file, "graph", out.graph, diags);
    apply_defaults(graph_defaults(), out.roles, palette, file, out.graph, diags);
    eval_table(preview_text, out.roles, palette, file, "preview", out.preview, diags);
    apply_defaults(preview_defaults(), out.roles, palette, file, out.preview, diags);

    out.checker_size = checker_size > 0.0f ? checker_size : 8.0f;
    out.font_ui = font_ui;
    out.font_editor = font_editor;
    out.font_ui_size = font_ui_size;
    out.font_editor_size = font_editor_size;
    return true;
}

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

std::vector<std::filesystem::path> theme_pack_paths(
    const std::vector<std::filesystem::path>& roots) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    // The same directory can be named twice - a macOS bundle reports its
    // Resources directory as the executable's location, so "beside the
    // executable" and "the bundle's Resources" are one place. Reading it twice
    // would parse every pack twice and report any warning in one of them twice.
    std::set<std::filesystem::path> visited;
    for (const auto& root : roots) {
        if (root.empty() || !std::filesystem::is_directory(root, ec)) continue;
        std::filesystem::path key = std::filesystem::weakly_canonical(root, ec);
        if (ec) key = root;
        if (!visited.insert(key).second) continue;
        std::vector<std::filesystem::path> found;
        for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
            const std::filesystem::path& path = entry.path();
            if (theme_pack_id(path).empty()) continue;
            if (entry.is_directory(ec)) {
                if (!std::filesystem::exists(path / "theme.toml", ec)) continue;
            } else if (!entry.is_regular_file(ec)) {
                continue;
            }
            found.push_back(path);
        }
        // Directory order is whatever the filesystem says; sorting makes the
        // list the same on every machine, which matters because the id
        // collision rule is "later wins" and a stable order is what makes that
        // predictable.
        std::sort(found.begin(), found.end());
        out.insert(out.end(), found.begin(), found.end());
    }
    return out;
}

// ---------------------------------------------------------------------------
// Watching
// ---------------------------------------------------------------------------

void ThemeWatch::reset(const std::filesystem::path& path) {
    file.clear();
    stamp = {};
    watching = false;
    if (path.empty()) return;

    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    if (ec) return;
    file = path;
    stamp = time;
    watching = true;
}

bool ThemeWatch::changed() {
    if (!watching) return false;
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(file, ec);
    // A file that cannot be read right now is not a change: it is a file being
    // written, or one that has been moved away. Either way the answer is "not
    // yet", and the recorded time stays where it was so the next check still
    // has something to compare against.
    if (ec) return false;
    if (time == stamp) return false;
    stamp = time;
    return true;
}

// ---------------------------------------------------------------------------
// Lint
// ---------------------------------------------------------------------------

Diagnostics lint_theme(const ResolvedTheme& theme) {
    Diagnostics out;
    const std::string file = theme.id;

    const auto check = [&](std::string_view what, Rgba fg, Rgba bg, float least) {
        const float ratio = color_contrast(fg, bg);
        if (ratio >= least) return;
        std::ostringstream message;
        message.precision(2);
        message << std::fixed << what << " contrast is " << ratio << ":1, below the " << least
                << ":1 this check wants";
        out.push_back(problem(Severity::Warning, message.str(), file));
    };

    const Rgba base = theme.role("surface.base");
    const Rgba sunken = theme.role("surface.sunken");
    check("ink.primary on surface.base", theme.role("ink.primary"), base, 7.0f);
    check("ink.muted on surface.base", theme.role("ink.muted"), base, 4.5f);
    check("ink.inverted on accent", theme.role("ink.inverted"), theme.role("accent"), 4.5f);
    // A button has to be findable rather than legible, so this threshold is far
    // below the ones for text. Calibrated so the built-in dark theme - whose
    // accent is a dark blue-grey a shade off its own panels - passes: a check
    // the shipped themes fail is a miscalibrated check, not a discovery.
    check("accent against surface.raised", theme.role("accent"), theme.role("surface.raised"),
          1.2f);

    for (std::size_t i = 0; i < kTokenKindCount; ++i) {
        const auto kind = static_cast<TokenKind>(i);
        // Comments are meant to recede - every palette in this project dims
        // them on purpose - so they are held to the large-text threshold while
        // the kinds that carry meaning are held to the body-text one.
        const float least = kind == TokenKind::Comment ? 3.0f : 4.5f;
        check(std::string("syntax.") + std::string(to_string(kind)) + " on surface.sunken",
              theme.syntax[i], sunken, least);
    }

    for (const char* name : {"error", "warning", "info", "success"}) {
        const auto it = theme.diagnostics.find(name);
        if (it == theme.diagnostics.end()) continue;
        check(std::string("diagnostics.") + name + " on surface.base", it->second, base, 4.5f);
    }
    return out;
}

}  // namespace ssstudio
