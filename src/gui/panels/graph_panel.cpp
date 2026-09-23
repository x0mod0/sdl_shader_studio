// Graph editor.
//
// The canvas is drawn with ImGui primitives rather than a node-editor
// dependency, which keeps the build simple and the interaction obvious: drag a
// node body to move it, drag from an output pin to an input pin to link, click a
// link to remove it.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <imgui.h>

#include "app.h"
#include "fonts.h"
#include "panel_common.h"
#include "ssstudio/graph.h"
#include "widgets.h"

namespace ssstudio::gui {
namespace {

constexpr float kNodeWidth = 168.0f;
constexpr float kRowHeight = 22.0f;
constexpr float kHeaderHeight = 30.0f;
constexpr float kPinRadius = 5.0f;
/// A node's corner radius, and the height of the category stripe along the top
/// of its header.
constexpr float kNodeRounding = 8.0f;
constexpr float kStripeHeight = 3.0f;

struct PinLocation {
    ImVec2 position;
    PinType type = PinType::Float;
};

/// A pin's colour, and a node header's, from the theme.
///
/// Keyed by the name to_string() gives the enum rather than by a switch, so a
/// pin type or a node category added to graph.h needs no change here: it lands
/// on the neutral colour it lands on today until a theme names it.
ImU32 color_for(PinType type, const ResolvedTheme& theme) {
    const std::string key = "pin." + std::string(to_string(type));
    return theme_u32(theme.graph_color(key, theme.graph_color("pin.any", 0xbebebeFFu)));
}

ImU32 header_color(NodeCategory category, const ResolvedTheme& theme) {
    const std::string key = "node." + std::string(to_string(category));
    return theme_u32(theme.graph_color(key, theme.graph_color("node.unknown", 0x404048FFu)));
}

float node_height(const NodeDef& def) {
    const std::size_t rows = std::max(def.inputs.size(), def.outputs.size());
    return kHeaderHeight + static_cast<float>(rows) * kRowHeight + 8.0f;
}

/// How far the canvas has been panned, per graph. View state rather than
/// document state: it is not worth writing into the graph file, but it does have
/// to survive a tab switch, so it is keyed the way the editor states are - by
/// session and shader, so two projects each holding a "plasma" do not share a
/// scroll position.
ImVec2& canvas_pan(const std::string& key) {
    static std::map<std::string, ImVec2> pans;
    return pans[key];
}

/// Removes a node and every link that touched it. Shared by the inspector's
/// button and the Delete key, so the two cannot come to mean different things.
void remove_node(Graph& graph, const std::string& id) {
    graph.links.erase(std::remove_if(graph.links.begin(), graph.links.end(),
                                     [&](const Link& l) {
                                         return l.from_node == id || l.to_node == id;
                                     }),
                      graph.links.end());
    graph.nodes.erase(std::remove_if(graph.nodes.begin(), graph.nodes.end(),
                                     [&](const Node& n) { return n.id == id; }),
                      graph.nodes.end());
}

/// Where a node of this type would land if it were dropped at the pointer: the
/// grid position under the cursor, moved off anything already there.
///
/// One function so the ghost and the placement cannot disagree - a preview drawn
/// somewhere other than where the node appears is worse than no preview.
NodeRect placement_rect(const Graph& graph, const NodeDef& def, const ImVec2& mouse,
                        const ImVec2& origin, bool snap) {
    constexpr float kSnap = 12.0f;
    // A grid step even when snapping is off: the search has to move in steps of
    // something, and a coarse one clears a neighbouring node in fewer of them.
    constexpr float kSearchStep = 12.0f;
    constexpr float kGap = 12.0f;

    NodeRect rect;
    rect.x = mouse.x - origin.x;
    rect.y = mouse.y - origin.y;
    if (snap) {
        rect.x = std::round(rect.x / kSnap) * kSnap;
        rect.y = std::round(rect.y / kSnap) * kSnap;
    }
    rect.width = kNodeWidth;
    rect.height = node_height(def);

    std::vector<NodeRect> occupied;
    occupied.reserve(graph.nodes.size());
    for (const auto& node : graph.nodes) {
        const NodeDef* other = resolve_node_def(graph, node.type);
        if (!other) continue;
        occupied.push_back({node.x, node.y, kNodeWidth, node_height(*other)});
    }
    place_without_overlap(occupied, rect, kSearchStep, kGap);
    return rect;
}

std::string unique_id(const Graph& graph, const std::string& base) {
    if (!graph.find_node(base)) return base;
    for (int i = 2; i < 1000; ++i) {
        const std::string candidate = base + "_" + std::to_string(i);
        if (!graph.find_node(candidate)) return candidate;
    }
    return base + "_x";
}

/// The node library. Picking an entry arms it rather than creating it: the node
/// appears where the next click on the canvas lands, because a node dropped at a
/// position nobody chose is a node that has to be dragged somewhere immediately.
void draw_library(App& app, Graph& graph, std::string& armed, bool& changed) {
    const ThemeInk ink(app.theme());
    static char filter[64] = "";
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##nodefilter", "Filter nodes", filter, sizeof(filter));
    const std::string needle = filter;

    // What is armed, and the way out of it. Escape does the same thing, but a
    // mode with no visible way to leave it is a trap.
    if (!armed.empty()) {
        const NodeDef* def = resolve_node_def(graph, armed);
        ImGui::TextWrapped("Click the canvas to place %s.",
                           def ? def->label.c_str() : armed.c_str());
        if (ImGui::SmallButton("Cancel")) armed.clear();
        ImGui::SameLine();
        ImGui::TextDisabled("or Esc");
        ImGui::Separator();
    }

    // Two separate facts, and conflating them was a bug: `have_section` says a
    // header has been drawn for `current`, `section_open` says that header is
    // expanded. Testing only the second one drew the header again for every
    // entry of a collapsed category - the same label, and so the same ImGui id,
    // a dozen times over, which is both the repeated rows and the id-conflict
    // error that hovering one of them reported.
    NodeCategory current = NodeCategory::Input;
    bool have_section = false;
    bool section_open = false;
    auto flush = [&] {
        if (have_section && section_open) ImGui::TreePop();
        have_section = false;
        section_open = false;
    };

    std::vector<const NodeDef*> definitions;
    for (const auto& def : builtin_nodes()) definitions.push_back(&def);
    for (const auto& def : graph.custom_nodes) definitions.push_back(&def);

    std::stable_sort(definitions.begin(), definitions.end(),
                     [](const NodeDef* a, const NodeDef* b) {
                         if (a->category != b->category) return a->category < b->category;
                         return a->label < b->label;
                     });

    // How many of each category the filter lets through, for the count at the
    // end of each heading - which is what says a folded category is not empty.
    std::map<NodeCategory, int> counts;
    for (const NodeDef* def : definitions) {
        if (!needle.empty() && def->label.find(needle) == std::string::npos &&
            def->type.find(needle) == std::string::npos) {
            continue;
        }
        ++counts[def->category];
    }

    for (const NodeDef* def : definitions) {
        if (!needle.empty() && def->label.find(needle) == std::string::npos &&
            def->type.find(needle) == std::string::npos) {
            continue;
        }
        if (!have_section || def->category != current) {
            flush();
            current = def->category;
            have_section = true;
            // Capitalized for display only: to_string(NodeCategory) is the
            // spelling a graph file carries, and changing that would rewrite
            // every saved graph to make a heading look right.
            //
            // The tree node carries only the arrow; the category's colour, its
            // name and its count are laid out after it on the same line, so the
            // swatch sits between the arrow and the word the way the canvas
            // shows the same colour on the node's header.
            const std::string name = display_case(to_string(current));
            section_open = ImGui::TreeNodeEx(("##category_" + name).c_str(),
                                             ImGuiTreeNodeFlags_DefaultOpen |
                                                 ImGuiTreeNodeFlags_SpanAvailWidth |
                                                 ImGuiTreeNodeFlags_AllowOverlap);
            ImGui::SameLine(0.0f, 0.0f);
            {
                const float swatch = design_px(8.0f);
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const float y = at.y + (ImGui::GetTextLineHeight() - swatch) * 0.5f;
                ImGui::GetWindowDrawList()->AddRectFilled(
                    ImVec2(at.x, y), ImVec2(at.x + swatch, y + swatch),
                    header_color(current, app.theme()), design_px(2.0f));
                ImGui::Dummy(ImVec2(swatch, ImGui::GetTextLineHeight()));
            }
            ImGui::SameLine(0.0f, design_px(8.0f));
            ImGui::TextUnformatted(name.c_str());
            const std::string count = std::to_string(counts[current]);
            FontScope small(nullptr, 11.0f);
            same_line_right_aligned(text_width(count.c_str()) + design_px(4.0f));
            colored_text(count, ink.muted);
        }
        if (!section_open) continue;

        // Keyed by type rather than by label: two categories may reasonably
        // offer a "Multiply", and two rows with one id is the same fault as
        // above in a quieter place.
        ImGui::PushID(def->type.c_str());
        // Picking the armed entry again puts the tool down, which is what a
        // second click on a pressed button means everywhere else.
        if (ImGui::Selectable(def->label.c_str(), def->type == armed)) {
            armed = (def->type == armed) ? std::string() : def->type;
        }
        if (ImGui::IsItemHovered() && !def->documentation.empty()) {
            ImGui::SetTooltip("%s", def->documentation.c_str());
        }
        ImGui::PopID();
    }
    flush();
    (void)app;
    (void)changed;
}

void draw_inspector(Graph& graph, const std::string& selected, bool& changed,
                    const ResolvedTheme& theme) {
    const ThemeInk ink(theme);
    Node* node = graph.find_node(selected);
    if (!node) {
        ImGui::TextDisabled("Select a node to edit it.");
        return;
    }
    const NodeDef* def = resolve_node_def(graph, node->type);
    if (!def) return;

    // What kind of node this is: its category's colour and its type, as the
    // library and the node's own header show them.
    {
        const float swatch = design_px(8.0f);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float y = at.y + (ImGui::GetTextLineHeight() - swatch) * 0.5f;
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(at.x, y),
                                                  ImVec2(at.x + swatch, y + swatch),
                                                  header_color(def->category, theme),
                                                  design_px(2.0f));
        ImGui::Dummy(ImVec2(swatch, ImGui::GetTextLineHeight()));
        ImGui::SameLine(0.0f, design_px(8.0f));
        mono_text(def->type, ink.muted, 12.0f);
    }

    char name[96];
    std::snprintf(name, sizeof(name), "%s", node->name.c_str());
    if (ImGui::InputText(left_label("Name").c_str(), name, sizeof(name))) {
        node->name = name;
        changed = true;
    }

    // Properties the node types understand. Shown only where they apply, so the
    // inspector never asks for something meaningless.
    auto property = [&](const char* key, const char* label, const char* hint) {
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer), "%s", node->properties[key].c_str());
        if (ImGui::InputTextWithHint(left_label(label).c_str(), hint, buffer, sizeof(buffer))) {
            node->properties[key] = buffer;
            changed = true;
        }
    };

    if (node->type == "input.uniform" || node->type == "input.macro" ||
        node->type == "input.vertex_attribute" || node->type == "output.varying") {
        property("name", "Declared name", "becomes a uniform / attribute name");
        property("type", "Type", "float, float2, float3, float4, float4x4");
    }
    if (node->type == "input.vertex_attribute") property("semantic", "Semantic", "POSITION");
    if (node->type == "input.constant") {
        ImGui::SeparatorText("Constant value");
        // A type and a value, because the node's output type is whatever its
        // literal is. Only the float widths: the emitter writes every number in
        // fixed-point form, so an "int" constant would generate `int x = 1.0;`,
        // which GLSL rejects outright.
        static constexpr std::array<const char*, 4> kTypes = {"float", "float2", "float3",
                                                              "float4"};
        std::string& type_name = node->properties["type"];
        std::vector<double>& stored = node->values["value"];
        if (type_name.empty()) {
            // A node saved before this editor existed has a value and no type,
            // or neither. Either way the value's own width is the better guess.
            type_name = std::string(to_string(
                vector_of(static_cast<std::uint32_t>(std::max<std::size_t>(1, stored.size())))));
        }

        int current = 0;
        for (int i = 0; i < static_cast<int>(kTypes.size()); ++i) {
            if (type_name == kTypes[static_cast<std::size_t>(i)]) current = i;
        }
        if (ImGui::Combo(left_label("Type").c_str(), &current, kTypes.data(), static_cast<int>(kTypes.size()))) {
            type_name = kTypes[static_cast<std::size_t>(current)];
            changed = true;
        }

        // Resized rather than rebuilt, so narrowing a float4 to a float2 and
        // widening it again does not silently lose what was typed in between -
        // it does, but only past the width that was actually shown.
        const std::size_t components = static_cast<std::size_t>(current) + 1;
        stored.resize(components, 0.0);

        std::vector<float> temp(stored.begin(), stored.end());
        bool edited = false;
        // Plain number fields rather than a colour picker: a colour has its own
        // node now, and a Constant is for the values that are not one.
        if (components == 1) edited = ImGui::DragFloat(left_label("Value").c_str(), temp.data(), 0.01f);
        else if (components == 2) edited = ImGui::DragFloat2(left_label("Value").c_str(), temp.data(), 0.01f);
        else if (components == 3) edited = ImGui::DragFloat3(left_label("Value").c_str(), temp.data(), 0.01f);
        else edited = ImGui::DragFloat4(left_label("Value").c_str(), temp.data(), 0.01f);
        if (edited) {
            for (std::size_t i = 0; i < stored.size(); ++i) stored[i] = temp[i];
            changed = true;
        }
        ImGui::TextDisabled("baked into the source as a literal");
    }
    if (node->type == "input.color") {
        // Stored against the output pin's name, which is where code generation
        // looks for it. Normalised to four components first so a file written by
        // hand with three cannot make the picker read past its own vector.
        std::vector<double>& stored = node->values["color"];
        if (stored.size() != 4) {
            std::vector<double> rgba = {0.0, 0.0, 0.0, 1.0};
            for (std::size_t i = 0; i < rgba.size() && i < stored.size(); ++i) {
                rgba[i] = stored[i];
            }
            if (stored.empty() && !def->outputs.empty() &&
                def->outputs.front().default_value.size() == 4) {
                rgba = def->outputs.front().default_value;
            }
            stored = rgba;
        }
        float temp[4] = {static_cast<float>(stored[0]), static_cast<float>(stored[1]),
                         static_cast<float>(stored[2]), static_cast<float>(stored[3])};
        if (color_field("Colour", temp, 4)) {
            for (std::size_t i = 0; i < stored.size(); ++i) stored[i] = temp[i];
            changed = true;
        }
    }
    if (node->type == "utility.swizzle") {
        std::string& stored = node->properties["swizzle"];
        if (stored.empty()) stored = "xy";
        const PinType source = resolved_pin_type(graph, *node, *def, "value", false);
        const std::uint32_t available = std::max<std::uint32_t>(1, component_count(source));

        char buffer[8];
        std::snprintf(buffer, sizeof(buffer), "%s", stored.c_str());
        if (ImGui::InputTextWithHint(left_label("Swizzle").c_str(), "xy, xyz, bgr, xxxx", buffer, sizeof(buffer))) {
            stored = buffer;
            changed = true;
        }
        // The reason it is wrong, under the field it is wrong in. The same text
        // the validation list shows, so there is one explanation rather than two
        // that could drift.
        const std::string problem = swizzle_error(stored, available);
        if (problem.empty()) {
            ImGui::TextDisabled("%s in, %s out", std::string(to_string(source)).c_str(),
                                std::string(to_string(vector_of(static_cast<std::uint32_t>(
                                                std::clamp<std::size_t>(stored.size(), 1, 4)))))
                                    .c_str());
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ink.error);
            ImGui::TextUnformatted(problem.c_str());
            ImGui::PopStyleColor();
        }
    }
    if (node->type == "utility.component") {
        // A list rather than a text field: there are exactly four answers, and
        // three of them are wrong for any given input width. The ones the
        // connected value does not have are shown disabled rather than hidden,
        // so the reason a component cannot be picked is visible.
        std::string& stored = node->properties["component"];
        if (stored.empty()) stored = "x";
        const std::uint32_t chosen = component_index(stored).value_or(0);
        const std::uint32_t available =
            std::max<std::uint32_t>(1, component_count(resolved_pin_type(graph, *node, *def, "value", false)));

        if (ImGui::BeginCombo(left_label("Component").c_str(), std::string(component_name(chosen)).c_str())) {
            for (std::uint32_t i = 0; i < 4; ++i) {
                const std::string label(component_name(i));
                ImGui::BeginDisabled(i >= available);
                if (ImGui::Selectable(label.c_str(), i == chosen)) {
                    stored = label;
                    changed = true;
                }
                ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("the input has %u component%s", available,
                            available == 1 ? "" : "s");
    }
    if (node->type == "input.texture_sample") property("name", "Texture name", "albedo");
    if (node->type == "custom.code") {
        property("return_type", "Return type", "float4");
        std::string& code = node->properties["code"];
        char buffer[2048];
        std::snprintf(buffer, sizeof(buffer), "%s", code.c_str());
        if (ImGui::InputTextMultiline(left_label("Code").c_str(), buffer, sizeof(buffer), ImVec2(-FLT_MIN, 140))) {
            code = buffer;
            changed = true;
        }
        ImGui::TextDisabled("$0, $1 are the input expressions; use return to produce the result.");
    }

    // Literal values for unconnected inputs.
    for (const auto& pin : def->inputs) {
        if (graph.incoming(node->id, pin.name)) continue;
        const std::uint32_t components = std::max<std::uint32_t>(1, component_count(pin.type));
        std::vector<double>& stored = node->values[pin.name];
        if (stored.empty()) stored = pin.default_value;
        stored.resize(components, 0.0);

        std::vector<float> temp(stored.begin(), stored.end());
        bool edited = false;
        // A picker for the pins that hold a colour, number fields for the rest.
        // These used to be told apart by component count, so a float3 called
        // "normal" got a colour picker and there was no way to type a value into
        // it. The name is the only thing that actually knows.
        if ((components == 3 || components == 4) && names_a_color(pin.name)) {
            edited = color_field(pin.name.c_str(), temp.data(), static_cast<int>(components));
        } else {
            const std::string id = left_label(pin.name.c_str());
            if (components == 1) edited = ImGui::DragFloat(id.c_str(), temp.data(), 0.01f);
            else if (components == 2) edited = ImGui::DragFloat2(id.c_str(), temp.data(), 0.01f);
            else if (components == 3) edited = ImGui::DragFloat3(id.c_str(), temp.data(), 0.01f);
            else if (components == 4) edited = ImGui::DragFloat4(id.c_str(), temp.data(), 0.01f);
        }
        if (edited) {
            for (std::size_t i = 0; i < stored.size(); ++i) stored[i] = temp[i];
            changed = true;
        }
    }

    ImGui::Spacing();
    if (danger_button("Delete node", ink)) {
        remove_node(graph, node->id);
        changed = true;
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    FontScope small(nullptr, 12.0f);
    ImGui::TextDisabled("or press Delete");
}

}  // namespace

void draw_graph_panel(App& app) {
    const ResolvedTheme& theme = app.theme();
    // A window of its own rather than a panel in the layout. A graph is worked on
    // beside the shader it generates, not in a tab that hides it, and the canvas
    // wants more room than a dock node is willing to give it.
    //
    // NoAutoMerge asks the platform for a real window, which is also what makes
    // it reachable the way every other window on the desktop is. It goes through
    // a window class because ImGui decides whether to create a viewport before
    // it reads the window's own flags.
    ImGuiWindowClass standalone;
    standalone.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
    ImGui::SetNextWindowClass(&standalone);

    // Half the screen when it first opens, and free to be dragged to any size
    // from a fifth of the screen upwards. A window ImGui has no size for is sized
    // to its contents, and this one's contents are a canvas - so without the
    // default it opens as tall as the graph is, which is taller than the display.
    // Only the default is bounded; a ceiling on the size would stop the one panel
    // that most wants to be made large from being made large.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 half(viewport->WorkSize.x * 0.5f, viewport->WorkSize.y * 0.5f);
    ImGui::SetNextWindowSize(half, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
               viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
        ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(viewport->WorkSize.x * 0.2f, viewport->WorkSize.y * 0.2f),
        ImVec2(FLT_MAX, FLT_MAX));

    // The shader in the title, and the window's identity after "###" so it stays
    // the same window while that name changes. Everything that reaches for this
    // window by name - the focus request, the layout file - keys on "Graph"; a
    // title that was also the id would make it a different window every time the
    // active shader changed, losing its size and position with it.
    Document* doc = app.active_document();
    std::string title = "Graph";
    if (doc != nullptr) {
        // The stage as well as the name, because a project may hold a vertex
        // and a fragment shader called the same thing and the window would
        // otherwise not say which of them it is showing.
        title += " - " + shader_display_name(doc->path, doc->id) + " (" +
                 stage_suffix(doc->stage) + ")";
    }
    title += "###Graph";

    PanelScope panel(title.c_str(), &app.show_graph, ImGuiWindowFlags_NoDocking);
    if (!panel) return;

    if (!doc) {
        ImGui::TextDisabled("Select a shader in the Editor.");
        return;
    }

    Graph* graph = app.graph_for(doc->id);
    if (!graph) {
        ImGui::TextWrapped("%s is a text shader.", doc->id.c_str());
        ImGui::Spacing();
        if (ImGui::Button("Author this shader as a graph")) app.create_graph(doc->id);
        ImGui::TextDisabled(
            "The graph will generate the shader file. You can detach at any time to keep the "
            "generated source and edit it by hand.");
        return;
    }

    bool changed = false;
    const ThemeInk ink(theme);

    // --- toolbar -----------------------------------------------------------
    const float types_width = fitted_width(130.0f, 70.0f);

    // The explanatory half of this row is a whole sentence, so on a narrow panel
    // it is what folds first and the controls follow it onto the next line.
    FlowLayout row;
    if (graph->detached) {
        const char* note =
            "Detached: the text file is authoritative and this graph is a snapshot.";
        row.next(text_width(note) + design_px(18.0f));
        pill_label(note, ink.warn, with_alpha(ink.warn, 0.12f));
        row.next(button_width("Reattach"));
        if (ImGui::Button("Reattach")) {
            graph->detached = false;
            changed = true;
        }
    } else {
        row.next(button_width("Detach"));
        if (ImGui::Button("Detach")) {
            // The generated source stays on disk and stops being regenerated.
            app.detach_graph(doc->id);
        }
        const char* note = "Shader file is generated from this graph";
        row.next(text_width(note) + design_px(18.0f));
        pill_label(note, ink.accent, with_alpha(ink.accent, 0.12f));
    }

    // The settings of the graph itself, against the right edge as one group
    // when the row has room for them.
    static bool snap_to_grid = true;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float settings_width = labeled_width("Types", types_width) + style.ItemSpacing.x +
                                 design_px(9.0f) + style.ItemSpacing.x +
                                 checkbox_width("Formatted") + style.ItemSpacing.x +
                                 checkbox_width("Snap") + style.ItemSpacing.x +
                                 button_width("Recenter");
    row.next(settings_width);
    if (ImGui::GetCursorPosX() + settings_width < ImGui::GetContentRegionMax().x) {
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - settings_width);
    }

    int coercion = static_cast<int>(graph->coercion);
    const char* coercion_labels[] = {"strict", "widening", "loose"};
    ImGui::PushStyleColor(ImGuiCol_Text, ink.muted);
    const std::string types_id = left_label("Types", types_width);
    ImGui::PopStyleColor();
    if (ImGui::Combo(types_id.c_str(), &coercion, coercion_labels, IM_ARRAYSIZE(coercion_labels))) {
        graph->coercion = static_cast<Coercion>(coercion);
        changed = true;
    }
    ImGui::SameLine();
    toolbar_divider(ink);
    ImGui::SameLine();
    if (ImGui::Checkbox("Formatted", &graph->formatted_output)) changed = true;
    ImGui::SameLine();
    ImGui::Checkbox("Snap", &snap_to_grid);

    // The way back when a pan has gone far enough that the nodes are off-screen,
    // which is the one state panning can leave you in with no visible way out.
    const std::string pan_key = app.active_session_key() + '\x1f' + doc->id;
    ImVec2& pan = canvas_pan(pan_key);
    ImGui::SameLine();
    if (ImGui::Button("Recenter")) pan = ImVec2(0.0f, 0.0f);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Pan by dragging the empty canvas, holding the middle button,\n"
            "or scrolling - sideways, or with Shift held.");
    }

    ImGui::Separator();

    // --- layout: library | canvas | inspector ------------------------------
    // The selection names a node inside this project's graph, so it belongs to
    // the project. The in-flight drag does not survive a frame, let alone a tab
    // switch, so it stays a static.
    std::string& selected_node = app.graph_selected_node();
    static std::string drag_from_node;
    static std::string drag_from_pin;
    // The node being moved and where inside it the pointer took hold. Held
    // rather than recomputed because a drag has to follow the point that was
    // grabbed, not the node's corner.
    static std::string drag_node;
    static ImVec2 drag_grab(0.0f, 0.0f);
    // The node type waiting to be placed. In flight rather than part of the
    // document, so it lives here with the drag state rather than in the graph.
    static std::string armed_type;
    // Set when a press placed a node, so the same press does not then pan the
    // canvas out from under what was just put down.
    static bool press_consumed = false;
    // A move that has not been written to the graph file yet, and which shader's
    // file it belongs to. Flushed when the button comes up, so a drag costs one
    // write rather than one per frame.
    static std::string layout_dirty_id;

    // Library | canvas | inspector, with a draggable divider either side of the
    // canvas. The two widths are what the user set, and the canvas takes what is
    // left - which is the only arrangement that stays sensible when the panel
    // itself is resized, since the canvas is the part that wants the room.
    //
    // Kept for the session rather than written to settings, the same as the snap
    // toggle above: it is a working preference, not a project's.
    static float library_width = 0.0f;
    static float inspector_width = 0.0f;
    constexpr float kMinSide = 120.0f;
    constexpr float kMinCanvas = 120.0f;
    constexpr float kSplitter = 6.0f;

    const float total_width = ImGui::GetContentRegionAvail().x;
    const float body_height = ImGui::GetContentRegionAvail().y;

    // First frame only. Wide enough for the longest node label in the library
    // and for a labelled field in the inspector, but never more than a quarter
    // of the panel each - a fixed pixel default is either most of a small window
    // or a sliver of a large one, and this window opens at half the screen.
    if (library_width <= 0.0f) library_width = side_pane_width(210.0f, 0.25f);
    if (inspector_width <= 0.0f) inspector_width = side_pane_width(260.0f, 0.25f);

    // Clamped every frame rather than only when dragged: the panel can be made
    // narrower than the two panes were, and the canvas must not be squeezed out
    // of existence when it is. Each side gives way in turn, so a very narrow
    // panel ends up with two minimum-width panes rather than a negative canvas.
    // Room for the canvas is taken off the top; the two panes share what is left.
    const float available = std::max(0.0f, total_width - kSplitter * 2.0f);
    const float side_budget = std::max(0.0f, available - kMinCanvas);
    // The panes' own floor gives way before the arithmetic does. Clamping each
    // to a fixed minimum lets their sum exceed the room on a very narrow window,
    // and then something has to be squeezed out - which is exactly what used to
    // happen: the canvas was sized explicitly and floored upward, the inspector
    // filled whatever remained, and so it stopped tracking the width its own
    // divider was changing. Dragging it moved a number nothing on screen read.
    const float floor_side = std::min(kMinSide, side_budget * 0.5f);
    library_width = std::clamp(library_width, floor_side,
                               std::max(floor_side, side_budget - inspector_width));
    inspector_width = std::clamp(inspector_width, floor_side,
                                 std::max(floor_side, side_budget - library_width));

    // Exact, not floored: the three panes and the two dividers now add up to the
    // width available, so every one of them gets what it was given.
    const float canvas_width = std::max(1.0f, available - library_width - inspector_width);

    ImGui::BeginChild("##library", ImVec2(library_width, 0), true);
    ImGui::PushTextWrapPos(0.0f);
    draw_library(app, *graph, armed_type, changed);
    ImGui::PopTextWrapPos();
    ImGui::EndChild();

    ImGui::SameLine(0.0f, 0.0f);
    vertical_splitter("##split_library", body_height, library_width, floor_side,
                      std::max(floor_side, side_budget - inspector_width), 1.0f);

    ImGui::SameLine(0.0f, 0.0f);
    ImGui::BeginChild("##canvas", ImVec2(canvas_width, 0), true,
                      ImGuiWindowFlags_NoScrollWithMouse);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImVec2 canvas_top_left = ImGui::GetCursorScreenPos();
    const ImVec2 canvas_size = ImGui::GetContentRegionAvail();

    // Wheel and middle-drag need no item of their own, so they are read here
    // where the result can be used by this frame's drawing. The catch-all button
    // that handles a left-drag on empty space cannot be: see below.
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        // Middle-drag pans from anywhere, including from on top of a node: it is
        // the one gesture that cannot be confused with moving the node under it,
        // so it needs no item to tell it apart from one.
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            pan.x += io.MouseDelta.x;
            pan.y += io.MouseDelta.y;
        }
        // The child does not scroll, so the wheel pans instead. MouseWheelH is
        // what a trackpad's sideways swipe arrives as, and without reading it a
        // trackpad can only ever pan one way.
        constexpr float kWheelStep = 40.0f;
        // Shift turns the one wheel a mouse has into the other axis; a trackpad
        // sends the second axis itself and needs no modifier.
        if (io.KeyShift) {
            pan.x += io.MouseWheel * kWheelStep;
        } else {
            pan.y += io.MouseWheel * kWheelStep;
        }
        pan.x += io.MouseWheelH * kWheelStep;
    }

    // How far the pan may go: enough to put a margin outside the nodes on every
    // side, and no further. Without a limit the canvas pans forever into empty
    // space; with one, there is always somewhere to pan to in both directions
    // even when every node already fits on screen.
    {
        constexpr float kMarginX = 240.0f;
        constexpr float kMarginY = 160.0f;
        float min_x = 0.0f, min_y = 0.0f, max_x = kNodeWidth, max_y = kHeaderHeight;
        bool first_node = true;
        for (const auto& node : graph->nodes) {
            const NodeDef* def = resolve_node_def(*graph, node.type);
            const float height = def ? node_height(*def) : kHeaderHeight;
            if (first_node) {
                min_x = node.x;
                min_y = node.y;
                max_x = node.x + kNodeWidth;
                max_y = node.y + height;
                first_node = false;
            } else {
                min_x = std::min(min_x, node.x);
                min_y = std::min(min_y, node.y);
                max_x = std::max(max_x, node.x + kNodeWidth);
                max_y = std::max(max_y, node.y + height);
            }
        }
        // The two bounds cross when the content is smaller than the view, and
        // that is the interesting case rather than a degenerate one: the range
        // between them is the slack the nodes can slide around in.
        const auto clamp_axis = [](float value, float a, float b) {
            return std::clamp(value, std::min(a, b), std::max(a, b));
        };
        pan.x = clamp_axis(pan.x, kMarginX - min_x, canvas_size.x - kMarginX - max_x);
        pan.y = clamp_axis(pan.y, kMarginY - min_y, canvas_size.y - kMarginY - max_y);
    }

    // Everything below is drawn relative to this, so panning is one addition
    // rather than a term threaded through every position.
    const ImVec2 origin(canvas_top_left.x + pan.x, canvas_top_left.y + pan.y);

    // The canvas's own ground, a shade off the panel's, so the working area
    // reads as a surface to put things on.
    draw_list->AddRectFilled(canvas_top_left,
                             ImVec2(canvas_top_left.x + canvas_size.x,
                                    canvas_top_left.y + canvas_size.y),
                             theme_u32(theme.graph_color("canvas")));

    // Grid, aligned to the panned origin so it slides with the nodes rather than
    // staying nailed to the panel and making the pan look like nothing moved.
    // Dots at the crossings rather than lines: enough to align by, without
    // ruling the canvas into a table the wires have to fight.
    constexpr float kGrid = 24.0f;
    const float grid_x = std::fmod(pan.x, kGrid) - (pan.x < 0.0f ? kGrid : 0.0f);
    const float grid_y = std::fmod(pan.y, kGrid) - (pan.y < 0.0f ? kGrid : 0.0f);
    const ImU32 grid_color = theme_u32(theme.graph_color("grid"));
    const float dot = std::max(1.0f, design_px(1.0f));
    for (float y = grid_y; y < canvas_size.y; y += kGrid) {
        if (y < 0.0f) continue;
        for (float x = grid_x; x < canvas_size.x; x += kGrid) {
            if (x < 0.0f) continue;
            const ImVec2 at(canvas_top_left.x + x, canvas_top_left.y + y);
            draw_list->AddRectFilled(ImVec2(at.x - dot * 0.5f, at.y - dot * 0.5f),
                                     ImVec2(at.x + dot * 0.5f, at.y + dot * 0.5f), grid_color);
        }
    }

    std::map<std::string, PinLocation> output_pins;
    std::map<std::string, PinLocation> input_pins;

    // --- nodes -------------------------------------------------------------
    for (auto& node : graph->nodes) {
        const NodeDef* def = resolve_node_def(*graph, node.type);
        if (!def) continue;

        const ImVec2 position(origin.x + node.x, origin.y + node.y);
        const float height = node_height(*def);
        const ImVec2 bottom_right(position.x + kNodeWidth, position.y + height);

        // A card: a soft shadow under it, its category as a stripe along the
        // top and a faint wash of the same colour over the header, and the
        // selection as the accent outline with a wider, fainter ring around it.
        const bool is_selected = selected_node == node.id;
        const ImU32 category = header_color(def->category, theme);
        draw_list->AddRectFilled(ImVec2(position.x - 2.0f, position.y + 4.0f),
                                 ImVec2(bottom_right.x + 2.0f, bottom_right.y + 8.0f),
                                 IM_COL32(0, 0, 0, 40), kNodeRounding + 2.0f);
        if (is_selected) {
            draw_list->AddRect(ImVec2(position.x - 2.5f, position.y - 2.5f),
                               ImVec2(bottom_right.x + 2.5f, bottom_right.y + 2.5f),
                               ink.accent_muted, kNodeRounding + 2.5f, 0, 3.0f);
        }
        draw_list->AddRectFilled(position, bottom_right, theme_u32(theme.graph_color("node_bg")),
                                 kNodeRounding);
        draw_list->AddRectFilled(position, ImVec2(bottom_right.x, position.y + kHeaderHeight),
                                 with_alpha(category, 0.12f), kNodeRounding,
                                 ImDrawFlags_RoundCornersTop);
        draw_list->PushClipRect(position, ImVec2(bottom_right.x, position.y + kStripeHeight), true);
        draw_list->AddRectFilled(position, ImVec2(bottom_right.x, position.y + kNodeRounding),
                                 category, kNodeRounding, ImDrawFlags_RoundCornersTop);
        draw_list->PopClipRect();
        draw_list->AddRect(position, bottom_right,
                           is_selected ? theme_u32(theme.graph_color("node_border_selected"))
                                       : theme_u32(theme.graph_color("node_border")),
                           kNodeRounding, 0, 1.0f);
        const float title_y = position.y + kStripeHeight +
                              (kHeaderHeight - kStripeHeight - ImGui::GetTextLineHeight()) * 0.5f;
        draw_list->AddText(ImVec2(position.x + 10.0f, title_y),
                           theme_u32(theme.graph_color("node_title")),
                           (node.name.empty() ? node.id : node.name).c_str());
        {
            // The category's word, quietly, at the header's other end.
            FontScope small(nullptr, 10.5f);
            const std::string word(to_string(def->category));
            draw_list->AddText(ImVec2(bottom_right.x - 10.0f - text_width(word.c_str()),
                                      position.y + kStripeHeight +
                                          (kHeaderHeight - kStripeHeight -
                                           ImGui::GetTextLineHeight()) * 0.5f),
                               ink.muted, word.c_str());
        }

        // Header is the drag handle.
        ImGui::SetCursorScreenPos(position);
        ImGui::InvisibleButton((node.id + "##body").c_str(), ImVec2(kNodeWidth, kHeaderHeight));
        if (ImGui::IsItemActivated()) {
            drag_node = node.id;
            drag_grab = ImVec2(io.MousePos.x - position.x, io.MousePos.y - position.y);
        }
        if (ImGui::IsItemActive() && drag_node == node.id &&
            ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            // Derived from where the pointer is, not accumulated from how far it
            // moved each frame. Accumulating and then rounding threw away every
            // step smaller than the grid, so a slow drag moved in lurches and a
            // fast one skipped - the node was being snapped from wherever it had
            // been left rather than from where the pointer actually was.
            constexpr float kSnap = 12.0f;
            float x = io.MousePos.x - drag_grab.x - origin.x;
            float y = io.MousePos.y - drag_grab.y - origin.y;
            if (snap_to_grid) {
                x = std::round(x / kSnap) * kSnap;
                y = std::round(y / kSnap) * kSnap;
            }
            if (x != node.x || y != node.y) {
                node.x = x;
                node.y = y;
                // Not `changed`: moving a node cannot alter a line of generated
                // source, and treating it as an edit regenerated the shader and
                // rewrote the graph file on every frame of the drag - which is
                // most of what made dragging feel like it was fighting back.
                layout_dirty_id = doc->id;
            }
        }
        if (ImGui::IsItemClicked()) selected_node = node.id;

        for (std::size_t i = 0; i < def->inputs.size(); ++i) {
            const ImVec2 pin(position.x,
                             position.y + kHeaderHeight + kRowHeight * (static_cast<float>(i) + 0.5f));
            const bool connected = graph->incoming(node.id, def->inputs[i].name) != nullptr;
            // An input with nothing wired in is a ring rather than a dot: it
            // reads as an open socket, and the value it will use instead is
            // shown at the far end of its row.
            if (connected) {
                draw_list->AddCircleFilled(pin, kPinRadius, color_for(def->inputs[i].type, theme));
            } else {
                draw_list->AddCircleFilled(pin, kPinRadius, theme_u32(theme.graph_color("node_bg")));
                draw_list->AddCircle(pin, kPinRadius - 1.0f,
                                     theme_u32(theme.graph_color("pin_unconnected")), 0, 2.0f);
            }
            {
                MonoScope mono(12.0f);
                const float text_y = pin.y - ImGui::GetTextLineHeight() * 0.5f;
                draw_list->AddText(ImVec2(pin.x + 12.0f, text_y),
                                   theme_u32(theme.graph_color("node_label")),
                                   def->inputs[i].name.c_str());
                if (!connected && i >= def->outputs.size()) {
                    const auto stored = node.values.find(def->inputs[i].name);
                    const std::vector<double>& value =
                        stored != node.values.end() && !stored->second.empty()
                            ? stored->second
                            : def->inputs[i].default_value;
                    if (value.size() == 1) {
                        char text[32];
                        std::snprintf(text, sizeof(text), "%.1f", value.front());
                        draw_list->AddText(ImVec2(position.x + kNodeWidth - 12.0f - text_width(text),
                                                  text_y),
                                           ink.muted, text);
                    }
                }
            }
            input_pins[node.id + "." + def->inputs[i].name] = {pin, def->inputs[i].type};

            ImGui::SetCursorScreenPos(ImVec2(pin.x - kPinRadius * 2, pin.y - kPinRadius * 2));
            ImGui::InvisibleButton((node.id + "in" + def->inputs[i].name).c_str(),
                                   ImVec2(kPinRadius * 4, kPinRadius * 4));
            // The output pin's button owns the active id for the whole drag, so
            // the drop target only sees the hover if it accepts being blocked
            // by it.
            const bool dropped_here =
                ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                ImGui::IsMouseReleased(ImGuiMouseButton_Left);
            if (dropped_here && !drag_from_node.empty()) {
                // Replace whatever was connected: one link per input, always.
                graph->links.erase(
                    std::remove_if(graph->links.begin(), graph->links.end(),
                                   [&](const Link& l) {
                                       return l.to_node == node.id && l.to_pin == def->inputs[i].name;
                                   }),
                    graph->links.end());
                graph->links.push_back({drag_from_node, drag_from_pin, node.id, def->inputs[i].name});
                drag_from_node.clear();
                changed = true;
            }
        }

        for (std::size_t i = 0; i < def->outputs.size(); ++i) {
            const ImVec2 pin(position.x + kNodeWidth,
                             position.y + kHeaderHeight + kRowHeight * (static_cast<float>(i) + 0.5f));
            draw_list->AddCircleFilled(pin, kPinRadius, color_for(def->outputs[i].type, theme));
            const char* label = def->outputs[i].name.c_str();
            {
                MonoScope mono(12.0f);
                const float width = ImGui::CalcTextSize(label).x;
                draw_list->AddText(ImVec2(pin.x - width - 12.0f, pin.y - ImGui::GetTextLineHeight() * 0.5f),
                                   theme_u32(theme.graph_color("node_label")), label);
            }
            output_pins[node.id + "." + def->outputs[i].name] = {pin, def->outputs[i].type};

            ImGui::SetCursorScreenPos(ImVec2(pin.x - kPinRadius * 2, pin.y - kPinRadius * 2));
            ImGui::InvisibleButton((node.id + "out" + def->outputs[i].name).c_str(),
                                   ImVec2(kPinRadius * 4, kPinRadius * 4));
            if (ImGui::IsItemClicked()) {
                drag_from_node = node.id;
                drag_from_pin = def->outputs[i].name;
            }
        }
    }

    // --- panning by dragging empty space -----------------------------------
    // Submitted after every node and pin, and that ordering is the whole point.
    // ImGui refuses to hover an item while another item is already hovered
    // (ItemHoverable's HoveredId test), so a canvas-sized button submitted first
    // makes everything drawn over it unreachable - no node drag, no pin click,
    // no selection. Last means it only ever catches a press that missed
    // everything else.
    //
    // The cost is that a left-drag pan is applied to the next frame rather than
    // this one, because the nodes have already been placed by the time it is
    // read. One frame is not a thing anyone can see, and it is a great deal
    // cheaper than the alternative.
    ImGui::SetCursorScreenPos(canvas_top_left);
    // Never zero: InvisibleButton asserts on an empty rect, and a panel dragged
    // down to nothing does briefly report one.
    const ImVec2 pan_area(std::max(1.0f, canvas_size.x), std::max(1.0f, canvas_size.y));
    ImGui::InvisibleButton("##pan", pan_area, ImGuiButtonFlags_MouseButtonLeft);

    // A press with something armed places it, at the press rather than at the
    // release: the node should appear under the pointer the moment it is put
    // down, not after a gesture that might turn out to be a drag.
    if (ImGui::IsItemActivated() && !armed_type.empty()) {
        if (const NodeDef* def = resolve_node_def(*graph, armed_type)) {
            NodeRect at = placement_rect(*graph, *def, io.MousePos, origin, snap_to_grid);
            Node node;
            node.type = def->type;
            node.id = unique_id(*graph, def->type.substr(def->type.find('.') + 1));
            node.name = def->label;
            node.x = at.x;
            node.y = at.y;
            selected_node = node.id;
            graph->nodes.push_back(std::move(node));
            changed = true;
        }
        // The tool stays armed, so several of the same node can be stamped down
        // in a row. Escape, Cancel, right-click or picking the entry again is
        // what puts it back down - a placement that disarmed itself would mean
        // a trip to the library for every node.
        press_consumed = true;
    }
    // Right-clicking the canvas puts the tool down, the way it cancels a
    // placement in every editor that has one.
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
        !armed_type.empty()) {
        armed_type.clear();
        press_consumed = true;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        press_consumed = false;
    }

    if (!press_consumed && ImGui::IsItemActive() &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        pan.x += io.MouseDelta.x;
        pan.y += io.MouseDelta.y;
    }

    // What is about to be placed, where it is about to go. A ghost rather than a
    // cursor change: the node's size is the part that decides whether there is
    // room for it.
    if (!armed_type.empty() && ImGui::IsItemHovered()) {
        if (const NodeDef* def = resolve_node_def(*graph, armed_type)) {
            // The ghost is drawn where the node would actually land, nudge and
            // all, so the click holds no surprise.
            const NodeRect free = placement_rect(*graph, *def, io.MousePos, origin, snap_to_grid);
            const ImVec2 at(origin.x + free.x, origin.y + free.y);
            const ImVec2 to(at.x + kNodeWidth, at.y + node_height(*def));
            draw_list->AddRectFilled(at, to,
                                     theme_u32(color_alpha(theme.graph_color("node_bg"), 0.59f)),
                                     kNodeRounding);
            draw_list->AddRectFilled(at, ImVec2(to.x, at.y + kHeaderHeight),
                                     (header_color(def->category, theme) & 0x00FFFFFFu) | (60u << IM_COL32_A_SHIFT), kNodeRounding,
                                     ImDrawFlags_RoundCornersTop);
            draw_list->AddRect(at, to,
                               theme_u32(color_alpha(theme.graph_color("node_border_selected"),
                                                     0.78f)),
                               kNodeRounding, 0, 1.5f);
            draw_list->AddText(ImVec2(at.x + 10.0f,
                                      at.y + (kHeaderHeight - ImGui::GetTextLineHeight()) * 0.5f),
                               theme_u32(color_alpha(theme.graph_color("node_title"), 0.78f)),
                               def->label.c_str());
        }
    }

    // --- links -------------------------------------------------------------
    for (std::size_t i = 0; i < graph->links.size();) {
        const Link& link = graph->links[i];
        auto from = output_pins.find(link.from_node + "." + link.from_pin);
        auto to = input_pins.find(link.to_node + "." + link.to_pin);
        if (from == output_pins.end() || to == input_pins.end()) {
            ++i;
            continue;
        }
        const ImVec2 a = from->second.position;
        const ImVec2 b = to->second.position;
        const float bend = std::max(30.0f, std::fabs(b.x - a.x) * 0.5f);
        draw_list->AddBezierCubic(a, ImVec2(a.x + bend, a.y), ImVec2(b.x - bend, b.y), b,
                                  color_for(from->second.type, theme), 2.5f);

        // Clicking near the midpoint removes the link.
        const ImVec2 middle((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool hovered = std::fabs(mouse.x - middle.x) < 8.0f &&
                             std::fabs(mouse.y - middle.y) < 8.0f;
        if (hovered) {
            draw_list->AddCircleFilled(middle, 5.0f,
                                       theme_u32(theme.diagnostic(Severity::Error)));
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                graph->links.erase(graph->links.begin() + static_cast<long>(i));
                changed = true;
                continue;
            }
        }
        ++i;
    }

    if (!drag_from_node.empty()) {
        auto from = output_pins.find(drag_from_node + "." + drag_from_pin);
        if (from != output_pins.end()) {
            draw_list->AddLine(from->second.position, ImGui::GetIO().MousePos,
                               theme_u32(color_alpha(theme.graph_color("node_title"), 0.78f)), 2.0f);
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) drag_from_node.clear();
    }

    // How much is on the canvas, in the corner, over the grid.
    {
        const std::string summary =
            std::to_string(graph->nodes.size()) + (graph->nodes.size() == 1 ? " node" : " nodes") +
            " \xC2\xB7 " + std::to_string(graph->links.size()) +
            (graph->links.size() == 1 ? " wire" : " wires");
        FontScope small(nullptr, 12.0f);
        const float margin = design_px(12.0f);
        const float pad = design_px(10.0f);
        const ImVec2 max(canvas_top_left.x + margin + pad * 2.0f + text_width(summary.c_str()),
                         canvas_top_left.y + canvas_size.y - margin);
        const ImVec2 min(canvas_top_left.x + margin, max.y - ImGui::GetFrameHeight());
        overlay_frame(draw_list, min, max, ink);
        draw_list->AddText(ImVec2(min.x + pad, (min.y + max.y - ImGui::GetTextLineHeight()) * 0.5f),
                           ink.muted, summary.c_str());
    }

    ImGui::EndChild();

    ImGui::SameLine(0.0f, 0.0f);
    vertical_splitter("##split_inspector", body_height, inspector_width, floor_side,
                      std::max(floor_side, side_budget - library_width), -1.0f);

    ImGui::SameLine(0.0f, 0.0f);
    ImGui::BeginChild("##inspector", ImVec2(inspector_width, 0), true);
    ImGui::PushTextWrapPos(0.0f);
    draw_inspector(*graph, selected_node, changed, theme);

    ImGui::Spacing();
    ImGui::Separator();
    caps_label("Validation", ink);
    const Diagnostics diagnostics = validate_graph(*graph);
    if (diagnostics.empty()) {
        status_dot(ink.ok);
        ImGui::SameLine(0.0f, design_px(8.0f));
        colored_text("No problems", ink.ok);
    }
    for (const auto& d : diagnostics) {
        const ImU32 color = d.severity == Severity::Error ? ink.error : ink.warn;
        status_dot(color);
        ImGui::SameLine(0.0f, design_px(8.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextUnformatted(d.message.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();

    // Escape puts the armed node down without placing it. Checked before the
    // delete keys so one press does one thing.
    const bool panel_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
    if (!armed_type.empty() && panel_focused && !ImGui::GetIO().WantTextInput &&
        ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        armed_type.clear();
    }

    // Delete or Backspace removes what is selected. Guarded on the panel having
    // focus and on no text field being active - Backspace inside the node name
    // or the filter box has to stay Backspace.
    if (!selected_node.empty() && armed_type.empty() && panel_focused &&
        !ImGui::GetIO().WantTextInput &&
        (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace))) {
        remove_node(*graph, selected_node);
        selected_node.clear();
        changed = true;
    }

    // One write per drag, when the button comes up.
    if (!layout_dirty_id.empty() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        app.save_graph_layout(layout_dirty_id);
        layout_dirty_id.clear();
        drag_node.clear();
    }

    if (changed) app.on_graph_changed(doc->id);
}

}  // namespace ssstudio::gui
