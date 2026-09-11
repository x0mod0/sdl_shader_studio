#include "ssstudio/graph.h"

#include <algorithm>
#include <set>

namespace ssstudio {
namespace {

Diagnostic error(std::string msg, std::string code = "SSSTUDIO-GRAPH") {
    Diagnostic d;
    d.severity = Severity::Error;
    d.code = std::move(code);
    d.message = std::move(msg);
    return d;
}

Diagnostic warning(std::string msg) {
    Diagnostic d;
    d.severity = Severity::Warning;
    d.code = "SSSTUDIO-GRAPH";
    d.message = std::move(msg);
    return d;
}

PinDef pin(std::string name, PinType type, std::vector<double> value = {}) {
    PinDef p;
    p.name = std::move(name);
    p.type = type;
    p.default_value = std::move(value);
    return p;
}

NodeDef def(std::string type, std::string label, NodeCategory category,
            std::vector<PinDef> inputs, std::vector<PinDef> outputs, std::string expression,
            std::string documentation, bool generic = false) {
    NodeDef d;
    d.type = std::move(type);
    d.label = std::move(label);
    d.category = category;
    d.inputs = std::move(inputs);
    d.outputs = std::move(outputs);
    d.expression = std::move(expression);
    d.documentation = std::move(documentation);
    d.generic = generic;
    return d;
}

std::vector<NodeDef> build_library() {
    std::vector<NodeDef> lib;

    // --- inputs ------------------------------------------------------------
    lib.push_back(def("input.uniform", "Uniform", NodeCategory::Input, {},
                      {pin("value", PinType::Any)}, "$name",
                      "A member of the generated uniform block. Set 'name' and 'type'."));
    lib.push_back(def("input.macro", "Macro", NodeCategory::Input, {},
                      {pin("value", PinType::Any)}, "$name",
                      "A preview-provided value such as time or resolution. Becomes a uniform "
                      "member so the shader stays self-contained."));
    lib.push_back(def("input.uv", "UV", NodeCategory::Input, {}, {pin("uv", PinType::Float2)},
                      "input.uv", "Interpolated texture coordinates from the vertex stage."));
    lib.push_back(def("input.position", "Screen position", NodeCategory::Input, {},
                      {pin("position", PinType::Float4)}, "input.position",
                      "SV_Position / gl_FragCoord."));
    lib.push_back(def("input.vertex_attribute", "Vertex attribute", NodeCategory::Input, {},
                      {pin("value", PinType::Any)}, "$name",
                      "A vertex input; 'name' and 'semantic' become part of the declared layout."));
    lib.push_back(def("input.texture_sample", "Sample texture", NodeCategory::Input,
                      {pin("uv", PinType::Float2)}, {pin("color", PinType::Float4)},
                      "SAMPLE($name, $0)",
                      "Samples a texture declared by this graph. Set 'name'."));
    lib.push_back(def("input.constant", "Constant", NodeCategory::Input, {},
                      {pin("value", PinType::Any)}, "$literal",
                      "A literal baked into the generated source."));
    // The default lives on the output pin. PinDef::default_value is otherwise an
    // unconnected input's value, but this node's value is the only thing it has,
    // and putting it here means the fallback is stated once where the pin is.
    lib.push_back(def("input.color", "Colour", NodeCategory::Input, {},
                      {pin("color", PinType::Float4, {1.0, 1.0, 1.0, 1.0})}, "$literal",
                      "A colour picked here and baked into the source, as one float4 rather "
                      "than four channels. Use Combine to build one from separate values."));

    // --- math --------------------------------------------------------------
    const auto binary = [&](const char* type, const char* label, const char* op) {
        lib.push_back(def(type, label, NodeCategory::Math,
                          {pin("a", PinType::Any), pin("b", PinType::Any)},
                          {pin("result", PinType::Any)},
                          std::string("($0 ") + op + " $1)", "", true));
    };
    binary("math.add", "Add", "+");
    binary("math.subtract", "Subtract", "-");
    binary("math.multiply", "Multiply", "*");
    binary("math.divide", "Divide", "/");

    lib.push_back(def("math.dot", "Dot", NodeCategory::Math,
                      {pin("a", PinType::Any), pin("b", PinType::Any)},
                      {pin("result", PinType::Float)}, "dot($0, $1)", "", true));
    lib.push_back(def("math.cross", "Cross", NodeCategory::Math,
                      {pin("a", PinType::Float3), pin("b", PinType::Float3)},
                      {pin("result", PinType::Float3)}, "cross($0, $1)", ""));
    lib.push_back(def("math.length", "Length", NodeCategory::Math, {pin("v", PinType::Any)},
                      {pin("result", PinType::Float)}, "length($0)", "", true));
    lib.push_back(def("math.normalize", "Normalize", NodeCategory::Math, {pin("v", PinType::Any)},
                      {pin("result", PinType::Any)}, "normalize($0)", "", true));
    lib.push_back(def("math.abs", "Abs", NodeCategory::Math, {pin("v", PinType::Any)},
                      {pin("result", PinType::Any)}, "abs($0)", "", true));
    lib.push_back(def("math.sin", "Sin", NodeCategory::Math, {pin("v", PinType::Any)},
                      {pin("result", PinType::Any)}, "sin($0)", "", true));
    lib.push_back(def("math.cos", "Cos", NodeCategory::Math, {pin("v", PinType::Any)},
                      {pin("result", PinType::Any)}, "cos($0)", "", true));
    lib.push_back(def("math.pow", "Power", NodeCategory::Math,
                      {pin("base", PinType::Any), pin("exponent", PinType::Any, {2.0})},
                      {pin("result", PinType::Any)}, "pow($0, $1)", "", true));
    lib.push_back(def("math.min", "Min", NodeCategory::Math,
                      {pin("a", PinType::Any), pin("b", PinType::Any)},
                      {pin("result", PinType::Any)}, "min($0, $1)", "", true));
    lib.push_back(def("math.max", "Max", NodeCategory::Math,
                      {pin("a", PinType::Any), pin("b", PinType::Any)},
                      {pin("result", PinType::Any)}, "max($0, $1)", "", true));
    lib.push_back(def("math.fract", "Fraction", NodeCategory::Math, {pin("v", PinType::Any)},
                      {pin("result", PinType::Any)}, "FRAC($0)", "", true));
    lib.push_back(def("math.floor", "Floor", NodeCategory::Math, {pin("v", PinType::Any)},
                      {pin("result", PinType::Any)}, "floor($0)", "", true));
    lib.push_back(def("math.mul_matrix", "Transform", NodeCategory::Math,
                      {pin("matrix", PinType::Matrix4), pin("vector", PinType::Float4)},
                      {pin("result", PinType::Float4)}, "MUL($0, $1)",
                      "Matrix times vector, in the multiplication order each language expects."));

    // --- utility -----------------------------------------------------------
    lib.push_back(def("utility.lerp", "Lerp", NodeCategory::Utility,
                      {pin("a", PinType::Any), pin("b", PinType::Any),
                       pin("t", PinType::Float, {0.5})},
                      {pin("result", PinType::Any)}, "LERP($0, $1, $2)", "", true));
    lib.push_back(def("utility.saturate", "Saturate", NodeCategory::Utility,
                      {pin("v", PinType::Any)}, {pin("result", PinType::Any)}, "SATURATE($0)", "",
                      true));
    lib.push_back(def("utility.step", "Step", NodeCategory::Utility,
                      {pin("edge", PinType::Any), pin("v", PinType::Any)},
                      {pin("result", PinType::Any)}, "step($0, $1)", "", true));
    lib.push_back(def("utility.smoothstep", "Smoothstep", NodeCategory::Utility,
                      {pin("edge0", PinType::Any), pin("edge1", PinType::Any),
                       pin("v", PinType::Any)},
                      {pin("result", PinType::Any)}, "smoothstep($0, $1, $2)", "", true));
    lib.push_back(def("utility.component", "Component", NodeCategory::Utility,
                      {pin("value", PinType::Any)}, {pin("result", PinType::Float)}, "$0",
                      "Takes one component out of a vector as a single float. Set 'component' "
                      "to x, y, z or w. A value that is already scalar passes through."));
    lib.push_back(def("utility.swizzle", "Swizzle", NodeCategory::Utility,
                      {pin("value", PinType::Any)}, {pin("result", PinType::Any)}, "$0",
                      "Reorders and reselects components: set 'swizzle' to xy, xyz, xxxx, bgr - "
                      "anything from one to four of them. The output is as wide as the swizzle "
                      "is long. Use Component when one float is all you want."));
    lib.push_back(def("utility.remap", "Remap", NodeCategory::Utility,
                      {pin("v", PinType::Float), pin("in_min", PinType::Float, {0.0}),
                       pin("in_max", PinType::Float, {1.0}), pin("out_min", PinType::Float, {0.0}),
                       pin("out_max", PinType::Float, {1.0})},
                      {pin("result", PinType::Float)},
                      "($3 + ($0 - $1) * ($4 - $3) / max($2 - $1, 1e-6))",
                      "Linear remap with a guard against a zero-width input range."));
    lib.push_back(def("utility.uv_transform", "UV transform", NodeCategory::Utility,
                      {pin("uv", PinType::Float2), pin("scale", PinType::Float2, {1.0, 1.0}),
                       pin("offset", PinType::Float2, {0.0, 0.0})},
                      {pin("result", PinType::Float2)}, "($0 * $1 + $2)", ""));
    lib.push_back(def("utility.noise", "Value noise", NodeCategory::Utility,
                      {pin("uv", PinType::Float2), pin("scale", PinType::Float, {8.0})},
                      {pin("result", PinType::Float)}, "ssstudio_value_noise($0 * $1)",
                      "Cheap hash-based value noise; the helper is emitted into the source."));
    lib.push_back(def("utility.time_wave", "Wave", NodeCategory::Utility,
                      {pin("time", PinType::Float), pin("frequency", PinType::Float, {1.0})},
                      {pin("result", PinType::Float)}, "(sin($0 * $1) * 0.5 + 0.5)", ""));

    // --- color -------------------------------------------------------------
    lib.push_back(def("color.gamma", "Gamma", NodeCategory::Color,
                      {pin("color", PinType::Float3), pin("gamma", PinType::Float, {2.2})},
                      {pin("result", PinType::Float3)}, "pow($0, VEC3($1))", ""));
    lib.push_back(def("color.hsv_to_rgb", "HSV to RGB", NodeCategory::Color,
                      {pin("hsv", PinType::Float3)}, {pin("result", PinType::Float3)},
                      "ssstudio_hsv_to_rgb($0)", "Helper emitted into the source."));
    lib.push_back(def("color.blend_multiply", "Blend: multiply", NodeCategory::Color,
                      {pin("base", PinType::Float4), pin("blend", PinType::Float4)},
                      {pin("result", PinType::Float4)}, "($0 * $1)", ""));
    lib.push_back(def("color.blend_screen", "Blend: screen", NodeCategory::Color,
                      {pin("base", PinType::Float4), pin("blend", PinType::Float4)},
                      {pin("result", PinType::Float4)}, "(VEC4(1.0) - (VEC4(1.0) - $0) * (VEC4(1.0) - $1))",
                      ""));
    lib.push_back(def("color.combine", "Combine", NodeCategory::Color,
                      {pin("r", PinType::Float), pin("g", PinType::Float),
                       pin("b", PinType::Float), pin("a", PinType::Float, {1.0})},
                      {pin("result", PinType::Float4)}, "VEC4($0, $1, $2, $3)", ""));
    lib.push_back(def("color.split", "Split", NodeCategory::Color, {pin("value", PinType::Float4)},
                      {pin("r", PinType::Float), pin("g", PinType::Float),
                       pin("b", PinType::Float), pin("a", PinType::Float)},
                      "$0", "Access components without writing swizzles by hand."));

    // --- control -----------------------------------------------------------
    lib.push_back(def("control.select", "Select", NodeCategory::Control,
                      {pin("condition", PinType::Bool), pin("if_true", PinType::Any),
                       pin("if_false", PinType::Any)},
                      {pin("result", PinType::Any)}, "($0 ? $1 : $2)",
                      "Branchless select; both sides are always evaluated.", true));
    lib.push_back(def("control.compare", "Compare", NodeCategory::Control,
                      {pin("a", PinType::Float), pin("b", PinType::Float)},
                      {pin("result", PinType::Bool)}, "($0 < $1)", "Less-than comparison."));

    // --- outputs -----------------------------------------------------------
    lib.push_back(def("output.color", "Target colour", NodeCategory::Output,
                      {pin("color", PinType::Float4, {0.0, 0.0, 0.0, 1.0})}, {}, "$0",
                      "SV_Target0 / the fragment output."));
    lib.push_back(def("output.position", "Clip position", NodeCategory::Output,
                      {pin("position", PinType::Float4)}, {}, "$0",
                      "SV_Position / gl_Position."));
    lib.push_back(def("output.varying", "Varying", NodeCategory::Output,
                      {pin("value", PinType::Any)}, {}, "$0",
                      "Passes a value to the next stage; 'name' names the varying.", true));

    // --- custom ------------------------------------------------------------
    lib.push_back(def("custom.code", "Custom code", NodeCategory::Custom,
                      {pin("a", PinType::Any), pin("b", PinType::Any)},
                      {pin("result", PinType::Any)}, "",
                      "Writes a function body verbatim; the 'code' property is the body and "
                      "'return_type' its result type.", true));
    return lib;
}

}  // namespace

// ---------------------------------------------------------------------------
std::string_view to_string(PinType t) {
    switch (t) {
        case PinType::Float: return "float";
        case PinType::Float2: return "float2";
        case PinType::Float3: return "float3";
        case PinType::Float4: return "float4";
        case PinType::Int: return "int";
        case PinType::Bool: return "bool";
        case PinType::Matrix4: return "float4x4";
        case PinType::Texture2D: return "texture2d";
        case PinType::Sampler: return "sampler";
        case PinType::Any: return "any";
    }
    return "float";
}

std::optional<PinType> pin_type_from_string(std::string_view s) {
    if (s == "float") return PinType::Float;
    if (s == "float2" || s == "vec2") return PinType::Float2;
    if (s == "float3" || s == "vec3") return PinType::Float3;
    if (s == "float4" || s == "vec4") return PinType::Float4;
    if (s == "int") return PinType::Int;
    if (s == "bool") return PinType::Bool;
    if (s == "float4x4" || s == "mat4") return PinType::Matrix4;
    if (s == "texture2d") return PinType::Texture2D;
    if (s == "sampler") return PinType::Sampler;
    if (s == "any") return PinType::Any;
    return std::nullopt;
}

std::optional<std::uint32_t> component_index(std::string_view name) {
    // Both naming sets, because a graph written against a colour reads better as
    // "g" than as "y" and the two mean the same component.
    constexpr std::string_view kPosition = "xyzw";
    constexpr std::string_view kColor = "rgba";
    if (name.size() != 1) return std::nullopt;
    if (const std::size_t at = kPosition.find(name[0]); at != std::string_view::npos) {
        return static_cast<std::uint32_t>(at);
    }
    if (const std::size_t at = kColor.find(name[0]); at != std::string_view::npos) {
        return static_cast<std::uint32_t>(at);
    }
    return std::nullopt;
}

std::string swizzle_error(std::string_view spelling, std::uint32_t available) {
    if (spelling.empty()) return "a swizzle has to name at least one component";
    if (spelling.size() > 4) {
        return "a swizzle can name at most four components, and '" + std::string(spelling) +
               "' names " + std::to_string(spelling.size());
    }
    for (char c : spelling) {
        if (!component_index(std::string_view(&c, 1))) {
            return "'" + std::string(1, c) + "' is not a component; use x, y, z, w or r, g, b, a";
        }
    }
    // Neither language lets one swizzle draw from both naming sets, and the
    // error a shader compiler gives for it is not obviously about that.
    const bool position = spelling.find_first_not_of("xyzw") == std::string_view::npos;
    const bool color = spelling.find_first_not_of("rgba") == std::string_view::npos;
    if (!position && !color) {
        return "'" + std::string(spelling) +
               "' mixes the xyzw and rgba names; a swizzle has to stay in one set";
    }
    for (char c : spelling) {
        const std::uint32_t index = *component_index(std::string_view(&c, 1));
        if (index >= available) {
            return "component '" + std::string(1, c) + "' does not exist on a value with " +
                   std::to_string(available) + (available == 1 ? " component" : " components");
        }
    }
    return {};
}

std::string swizzle_xyzw(std::string_view spelling) {
    std::string out;
    out.reserve(spelling.size());
    for (char c : spelling) {
        const auto index = component_index(std::string_view(&c, 1));
        out += index ? component_name(*index) : std::string_view("x");
    }
    return out;
}

namespace {

/// True when the two rectangles come within `gap` of one another. Touching
/// exactly is not a collision; `gap` is the room asked for on top of that.
bool rects_overlap(const NodeRect& a, const NodeRect& b, float gap) {
    return a.x < b.x + b.width + gap && b.x < a.x + a.width + gap &&
           a.y < b.y + b.height + gap && b.y < a.y + a.height + gap;
}

}  // namespace

void place_without_overlap(const std::vector<NodeRect>& occupied, NodeRect& candidate,
                           float step, float gap) {
    const auto free_at = [&](float x, float y) {
        NodeRect probe = candidate;
        probe.x = x;
        probe.y = y;
        return std::none_of(occupied.begin(), occupied.end(), [&](const NodeRect& other) {
            return rects_overlap(probe, other, gap);
        });
    };
    if (free_at(candidate.x, candidate.y)) return;
    if (step <= 0.0f) step = 1.0f;

    // Rings of increasing radius around the asked-for position, taking the
    // closest free cell in the first ring that has one. Searching ring by ring
    // is what makes the result the nearest free position rather than merely a
    // free one, and it stops as soon as a ring answers.
    constexpr int kMaxRings = 64;
    for (int radius = 1; radius <= kMaxRings; ++radius) {
        float best_x = 0.0f;
        float best_y = 0.0f;
        float best_distance = -1.0f;
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                // The ring itself, not the square it encloses: everything inside
                // was tested by a smaller radius already.
                if (std::max(std::abs(dx), std::abs(dy)) != radius) continue;
                const float offset_x = static_cast<float>(dx) * step;
                const float offset_y = static_cast<float>(dy) * step;
                if (!free_at(candidate.x + offset_x, candidate.y + offset_y)) continue;
                const float distance = offset_x * offset_x + offset_y * offset_y;
                if (best_distance < 0.0f || distance < best_distance) {
                    best_distance = distance;
                    best_x = candidate.x + offset_x;
                    best_y = candidate.y + offset_y;
                }
            }
        }
        if (best_distance >= 0.0f) {
            candidate.x = best_x;
            candidate.y = best_y;
            return;
        }
    }
}

std::string_view component_name(std::uint32_t index) {
    constexpr std::string_view kPosition = "xyzw";
    return index < kPosition.size() ? kPosition.substr(index, 1) : kPosition.substr(0, 1);
}

std::uint32_t component_count(PinType t) {
    switch (t) {
        case PinType::Float:
        case PinType::Int:
        case PinType::Bool: return 1;
        case PinType::Float2: return 2;
        case PinType::Float3: return 3;
        case PinType::Float4: return 4;
        case PinType::Matrix4: return 16;
        default: return 0;
    }
}

PinType vector_of(std::uint32_t components) {
    switch (components) {
        case 2: return PinType::Float2;
        case 3: return PinType::Float3;
        case 4: return PinType::Float4;
        default: return PinType::Float;
    }
}

std::string pin_type_name(PinType t, Language language) {
    if (language == Language::HLSL) return std::string(to_string(t));
    switch (t) {
        case PinType::Float: return "float";
        case PinType::Float2: return "vec2";
        case PinType::Float3: return "vec3";
        case PinType::Float4: return "vec4";
        case PinType::Int: return "int";
        case PinType::Bool: return "bool";
        case PinType::Matrix4: return "mat4";
        case PinType::Texture2D: return "sampler2D";
        case PinType::Sampler: return "sampler";
        case PinType::Any: return "float";
    }
    return "float";
}

std::string_view to_string(Coercion c) {
    switch (c) {
        case Coercion::Strict: return "strict";
        case Coercion::Widening: return "widening";
        case Coercion::Loose: return "loose";
    }
    return "widening";
}

std::optional<Coercion> coercion_from_string(std::string_view s) {
    if (s == "strict") return Coercion::Strict;
    if (s == "widening") return Coercion::Widening;
    if (s == "loose") return Coercion::Loose;
    return std::nullopt;
}

std::string_view to_string(NodeCategory c) {
    switch (c) {
        case NodeCategory::Input: return "input";
        case NodeCategory::Math: return "math";
        case NodeCategory::Utility: return "utility";
        case NodeCategory::Color: return "color";
        case NodeCategory::Control: return "control";
        case NodeCategory::Custom: return "custom";
        case NodeCategory::Output: return "output";
    }
    return "math";
}

const std::vector<NodeDef>& builtin_nodes() {
    static const std::vector<NodeDef> library = build_library();
    return library;
}

const NodeDef* find_node_def(const std::string& type) {
    for (const auto& d : builtin_nodes()) {
        if (d.type == type) return &d;
    }
    return nullptr;
}

const NodeDef* resolve_node_def(const Graph& graph, const std::string& type) {
    for (const auto& d : graph.custom_nodes) {
        if (d.type == type) return &d;
    }
    return find_node_def(type);
}

// ---------------------------------------------------------------------------
const Node* Graph::find_node(const std::string& id) const {
    for (const auto& n : nodes) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

Node* Graph::find_node(const std::string& id) {
    for (auto& n : nodes) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

const Link* Graph::incoming(const std::string& node_id, const std::string& pin_name) const {
    for (const auto& l : links) {
        if (l.to_node == node_id && l.to_pin == pin_name) return &l;
    }
    return nullptr;
}

Graph Graph::create_default(std::string shader_id, Stage stage, Language language) {
    Graph g;
    g.shader_id = std::move(shader_id);
    g.stage = stage;
    g.language = language;

    if (stage == Stage::Fragment) {
        Node uv;
        uv.id = "uv";
        uv.type = "input.uv";
        uv.name = "uv";
        uv.x = 40.0f;
        uv.y = 120.0f;

        // A starting graph that produces something to look at immediately, and
        // whose links are type-correct under the default (widening) coercion.
        Node noise;
        noise.id = "noise";
        noise.type = "utility.noise";
        noise.name = "noise";
        noise.x = 240.0f;
        noise.y = 120.0f;
        noise.values["scale"] = {8.0};

        Node combine;
        combine.id = "combine";
        combine.type = "color.combine";
        combine.name = "colour";
        combine.x = 460.0f;
        combine.y = 100.0f;
        combine.values["b"] = {0.5};
        combine.values["a"] = {1.0};

        Node out;
        out.id = "out";
        out.type = "output.color";
        out.name = "target";
        out.x = 700.0f;
        out.y = 120.0f;

        g.nodes = {uv, noise, combine, out};
        g.links = {{"uv", "uv", "noise", "uv"},
                   {"noise", "result", "combine", "r"},
                   {"noise", "result", "combine", "g"},
                   {"combine", "result", "out", "color"}};
    } else if (stage == Stage::Vertex) {
        Node position;
        position.id = "position";
        position.type = "input.vertex_attribute";
        position.name = "position";
        position.properties["name"] = "position";
        position.properties["semantic"] = "POSITION";
        position.properties["type"] = "float4";

        Node out;
        out.id = "out";
        out.type = "output.position";
        out.name = "clip";
        out.x = 360.0f;

        g.nodes = {position, out};
        g.links = {{"position", "value", "out", "position"}};
    }
    return g;
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------
namespace {

// The type a pin actually carries once generics are resolved.
PinType literal_type(const Node& node, const std::string& pin_name, PinType declared) {
    auto it = node.values.find(pin_name);
    if (it != node.values.end() && !it->second.empty() && declared == PinType::Any) {
        return vector_of(static_cast<std::uint32_t>(it->second.size()));
    }
    return declared;
}

PinType declared_type(const Node& node, PinType fallback) {
    auto it = node.properties.find("type");
    if (it == node.properties.end()) return fallback;
    return pin_type_from_string(it->second).value_or(fallback);
}

PinType input_pin_type(const Graph& graph, const Node& node, const NodeDef& def,
                       const std::string& pin_name) {
    for (const auto& p : def.inputs) {
        if (p.name != pin_name) continue;
        if (p.type != PinType::Any) return p.type;
        if (const Link* link = graph.incoming(node.id, pin_name)) {
            if (const Node* source = graph.find_node(link->from_node)) {
                if (const NodeDef* source_def = resolve_node_def(graph, source->type)) {
                    return resolved_pin_type(graph, *source, *source_def, link->from_pin, true);
                }
            }
        }
        return literal_type(node, pin_name, PinType::Float);
    }
    return PinType::Float;
}

bool coercible(PinType from, PinType to, Coercion rule) {
    if (from == to || to == PinType::Any || from == PinType::Any) return true;
    if (from == PinType::Texture2D || to == PinType::Texture2D) return false;
    if (from == PinType::Matrix4 || to == PinType::Matrix4) return false;

    switch (rule) {
        case Coercion::Strict:
            return false;
        case Coercion::Widening:
            // A scalar splats into any vector; a vector widens with zeros.
            if (component_count(from) == 1) return true;
            return component_count(to) >= component_count(from);
        case Coercion::Loose:
            return component_count(from) > 0 && component_count(to) > 0;
    }
    return false;
}

}  // namespace

PinType resolved_pin_type(const Graph& graph, const Node& node, const NodeDef& def,
                          const std::string& pin_name, bool is_output) {
    const std::vector<PinDef>& pins = is_output ? def.outputs : def.inputs;
    for (const auto& p : pins) {
        if (p.name != pin_name) continue;
        if (p.type != PinType::Any) return p.type;

        // Generic output: nodes that declare a type property win, otherwise the
        // widest connected input decides, which is what a person expects when
        // they wire a float3 into an Add.
        if (node.type == "input.uniform" || node.type == "input.macro" ||
            node.type == "input.constant" || node.type == "input.vertex_attribute" ||
            node.type == "output.varying") {
            // A declared type wins; failing that the stored literal's own width
            // decides, and only then does it fall back to a float. Passing Float
            // straight into literal_type skipped the middle step - it only
            // infers from a value when what it is given is Any - so a Constant
            // holding three numbers and no type property resolved here as a
            // float while code generation emitted a float3 from the same node.
            const PinType implied = literal_type(node, "value", PinType::Any);
            return declared_type(node, implied == PinType::Any ? PinType::Float : implied);
        }
        // The swizzle's width is the length of the swizzle, not the width of what
        // is feeding it: ".xy" off a float4 is a float2 and ".xxx" off a float
        // would be a float3. Nothing else in the graph can work that out.
        //
        // The output only. This node's input is generic in the ordinary way -
        // it takes whatever is wired to it - and giving it the swizzle's width
        // too would make feeding a float4 into a ".xy" a coercion error, which
        // is precisely the thing the node exists to do.
        if (is_output && node.type == "utility.swizzle") {
            auto it = node.properties.find("swizzle");
            const std::string spelling = it == node.properties.end() ? "xy" : it->second;
            return vector_of(static_cast<std::uint32_t>(
                std::clamp<std::size_t>(spelling.size(), 1, 4)));
        }
        if (node.type == "custom.code") {
            auto it = node.properties.find("return_type");
            if (it != node.properties.end()) {
                return pin_type_from_string(it->second).value_or(PinType::Float);
            }
        }
        PinType widest = PinType::Float;
        for (const auto& in : def.inputs) {
            if (in.type != PinType::Any) continue;
            const PinType t = input_pin_type(graph, node, def, in.name);
            if (component_count(t) > component_count(widest)) widest = t;
        }
        return widest;
    }
    return PinType::Float;
}

std::vector<const Node*> topological_order(const Graph& graph, Diagnostics& out_diags) {
    std::map<std::string, int> pending;
    std::map<std::string, std::vector<std::string>> dependents;

    for (const auto& node : graph.nodes) pending[node.id] = 0;
    for (const auto& link : graph.links) {
        if (!graph.find_node(link.from_node) || !graph.find_node(link.to_node)) continue;
        pending[link.to_node] += 1;
        dependents[link.from_node].push_back(link.to_node);
    }

    // Deterministic order: ready nodes are taken in declaration order, so the
    // generated source is byte-identical for the same graph every time.
    std::vector<const Node*> ready;
    for (const auto& node : graph.nodes) {
        if (pending[node.id] == 0) ready.push_back(&node);
    }

    std::vector<const Node*> order;
    while (!ready.empty()) {
        const Node* node = ready.front();
        ready.erase(ready.begin());
        order.push_back(node);
        for (const auto& id : dependents[node->id]) {
            if (--pending[id] == 0) {
                if (const Node* next = graph.find_node(id)) ready.push_back(next);
            }
        }
    }

    if (order.size() != graph.nodes.size()) {
        std::string cycle;
        for (const auto& [id, count] : pending) {
            if (count <= 0) continue;
            if (!cycle.empty()) cycle += ", ";
            cycle += id;
        }
        out_diags.push_back(error("the graph contains a cycle involving: " + cycle));
        return {};
    }
    return order;
}

Diagnostics validate_graph(const Graph& graph) {
    Diagnostics out;

    std::set<std::string> ids;
    for (const auto& node : graph.nodes) {
        if (!ids.insert(node.id).second) {
            out.push_back(error("duplicate node id '" + node.id + "'"));
        }
        if (!resolve_node_def(graph, node.type)) {
            out.push_back(error("unknown node type '" + node.type + "' (node " + node.id + ")"));
        }
    }

    for (const auto& link : graph.links) {
        const Node* from = graph.find_node(link.from_node);
        const Node* to = graph.find_node(link.to_node);
        if (!from || !to) {
            out.push_back(error("link refers to a node that does not exist (" + link.from_node +
                                " -> " + link.to_node + ")"));
            continue;
        }
        const NodeDef* from_def = resolve_node_def(graph, from->type);
        const NodeDef* to_def = resolve_node_def(graph, to->type);
        if (!from_def || !to_def) continue;

        const bool has_output = std::any_of(from_def->outputs.begin(), from_def->outputs.end(),
                                            [&](const PinDef& p) { return p.name == link.from_pin; });
        const bool has_input = std::any_of(to_def->inputs.begin(), to_def->inputs.end(),
                                           [&](const PinDef& p) { return p.name == link.to_pin; });
        if (!has_output) {
            out.push_back(error("node '" + from->id + "' has no output pin '" + link.from_pin + "'"));
            continue;
        }
        if (!has_input) {
            out.push_back(error("node '" + to->id + "' has no input pin '" + link.to_pin + "'"));
            continue;
        }

        const PinType source = resolved_pin_type(graph, *from, *from_def, link.from_pin, true);
        const PinType target = resolved_pin_type(graph, *to, *to_def, link.to_pin, false);
        if (!coercible(source, target, graph.coercion)) {
            out.push_back(error("cannot connect " + std::string(to_string(source)) + " to " +
                                std::string(to_string(target)) + " (" + from->id + "." +
                                link.from_pin + " -> " + to->id + "." + link.to_pin +
                                ") under " + std::string(to_string(graph.coercion)) +
                                " coercion"));
        }
    }

    // A component that the thing feeding it does not have. Worth catching here
    // rather than in the shader compiler: by then the message is about a
    // generated line the user did not write.
    for (const auto& node : graph.nodes) {
        if (node.type != "utility.component") continue;
        const NodeDef* def = resolve_node_def(graph, node.type);
        if (!def) continue;

        const auto it = node.properties.find("component");
        const std::string spelling = it == node.properties.end() ? "x" : it->second;
        const std::optional<std::uint32_t> index = component_index(spelling);
        if (!index) {
            out.push_back(error("node '" + node.id + "' selects component '" + spelling +
                                "', which is not one of x, y, z or w"));
            continue;
        }
        const PinType source = input_pin_type(graph, node, *def, "value");
        const std::uint32_t available = component_count(source);
        if (*index >= available) {
            out.push_back(error("node '" + node.id + "' selects component '" +
                                std::string(component_name(*index)) + "' of a " +
                                std::string(to_string(source)) + ", which has " +
                                std::to_string(available) +
                                (available == 1 ? " component" : " components")));
        }
    }

    for (const auto& node : graph.nodes) {
        if (node.type != "utility.swizzle") continue;
        const NodeDef* def = resolve_node_def(graph, node.type);
        if (!def) continue;
        const auto it = node.properties.find("swizzle");
        const std::string spelling = it == node.properties.end() ? "xy" : it->second;
        const PinType source = input_pin_type(graph, node, *def, "value");
        const std::string problem = swizzle_error(spelling, component_count(source));
        if (!problem.empty()) {
            out.push_back(error("node '" + node.id + "': " + problem));
        }
    }

    // Two links into one input pin is ambiguous; the last one silently winning
    // is exactly the kind of thing that wastes an afternoon.
    std::set<std::string> occupied;
    for (const auto& link : graph.links) {
        const std::string key = link.to_node + "." + link.to_pin;
        if (!occupied.insert(key).second) {
            out.push_back(error("input '" + key + "' has more than one incoming link"));
        }
    }

    const bool wants_color = graph.stage == Stage::Fragment;
    const std::string required = wants_color ? "output.color" : "output.position";
    const bool has_output =
        std::any_of(graph.nodes.begin(), graph.nodes.end(),
                    [&](const Node& n) { return n.type == required; });
    if (!has_output && graph.stage != Stage::Compute) {
        out.push_back(error("this " + std::string(to_string(graph.stage)) +
                            " graph has no '" + required + "' node, so it generates nothing"));
    }

    Diagnostics cycle_diags;
    topological_order(graph, cycle_diags);
    out.insert(out.end(), cycle_diags.begin(), cycle_diags.end());

    for (const auto& node : graph.nodes) {
        if (node.type != "input.uniform" && node.type != "input.texture_sample") continue;
        if (node.properties.count("name") == 0 || node.properties.at("name").empty()) {
            out.push_back(warning("node '" + node.id +
                                  "' has no name; the generated declaration will use its id"));
        }
    }
    return out;
}

std::vector<GraphInput> graph_inputs(const Graph& graph) {
    std::vector<GraphInput> inputs;
    for (const auto& node : graph.nodes) {
        const NodeDef* def = resolve_node_def(graph, node.type);
        if (!def) continue;

        auto name_of = [&]() {
            auto it = node.properties.find("name");
            return it != node.properties.end() && !it->second.empty() ? it->second : node.id;
        };

        if (node.type == "input.uniform" || node.type == "input.macro") {
            GraphInput in;
            in.name = name_of();
            in.type = resolved_pin_type(graph, node, *def, "value", true);
            auto value = node.values.find("value");
            if (value != node.values.end()) in.default_value = value->second;
            inputs.push_back(std::move(in));
        } else if (node.type == "input.texture_sample") {
            GraphInput in;
            in.name = name_of();
            in.type = PinType::Texture2D;
            in.is_texture = true;
            inputs.push_back(std::move(in));
        }
    }
    return inputs;
}

}  // namespace ssstudio
