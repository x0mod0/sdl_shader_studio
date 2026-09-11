// Graph -> shader source.
//
// The output is meant to be readable: node names become comments, values become
// named locals in evaluation order, and the register spaces / descriptor sets
// follow the same SDL rules the validator enforces. A user who detaches a graph
// should get source they are happy to keep by hand.
#include <algorithm>
#include <map>
#include <set>
#include <sstream>

#include "ssstudio/graph.h"

namespace ssstudio {
namespace {

struct Emitter {
    const Graph& graph;
    const GraphCodegenOptions& options;
    Diagnostics& diags;
    bool hlsl;

    std::map<std::string, std::string> expressions;  // "node.pin" -> local name
    std::vector<std::string> uniform_members;
    std::vector<std::pair<std::string, std::string>> textures;  // name, sampler name
    std::vector<std::string> vertex_attributes;
    std::vector<std::string> varyings;
    std::set<std::string> helpers;

    std::string type_name(PinType t) const { return pin_type_name(t, graph.language); }

    // Language-specific spellings, applied to the node expression templates.
    std::string apply_dialect(std::string expression) const {
        struct Replacement {
            const char* token;
            const char* hlsl_form;
            const char* glsl_form;
        };
        static const Replacement kReplacements[] = {
            {"FRAC(", "frac(", "fract("},
            {"LERP(", "lerp(", "mix("},
            {"SATURATE(", "saturate(", "clamp01("},
            {"VEC4(", "float4(", "vec4("},
            {"VEC3(", "float3(", "vec3("},
            {"VEC2(", "float2(", "vec2("},
        };
        for (const auto& r : kReplacements) {
            const std::string from = r.token;
            const std::string to = hlsl ? r.hlsl_form : r.glsl_form;
            std::size_t pos = 0;
            while ((pos = expression.find(from, pos)) != std::string::npos) {
                expression.replace(pos, from.size(), to);
                pos += to.size();
            }
        }
        return expression;
    }

    std::string literal(const std::vector<double>& values, PinType type) const {
        const std::uint32_t want = std::max<std::uint32_t>(1, component_count(type));
        std::ostringstream os;
        auto number = [](double v) {
            std::ostringstream n;
            n.precision(6);
            n << std::fixed << v;
            std::string s = n.str();
            while (s.size() > 3 && s.back() == '0' && s[s.size() - 2] != '.') s.pop_back();
            return s;
        };
        if (want == 1) {
            return number(values.empty() ? 0.0 : values.front());
        }
        os << (hlsl ? "float" : "vec") << want << "(";
        for (std::uint32_t i = 0; i < want; ++i) {
            if (i) os << ", ";
            os << number(i < values.size() ? values[i] : (values.empty() ? 0.0 : values.back()));
        }
        os << ")";
        return os.str();
    }

    // Adjusts an expression when the connected type differs from what the pin
    // wants. Widening splats scalars and pads with zeros; narrowing swizzles.
    std::string coerce(const std::string& expression, PinType from, PinType to) const {
        if (from == to || to == PinType::Any || from == PinType::Any) return expression;
        const std::uint32_t have = component_count(from);
        const std::uint32_t want = component_count(to);
        if (have == 0 || want == 0) return expression;

        if (have == want) return expression;
        if (have == 1) {
            // Scalar splat: float3(x) / vec3(x) is the readable form.
            std::ostringstream os;
            os << (hlsl ? "float" : "vec") << want << "(" << expression << ")";
            return os.str();
        }
        if (have > want) {
            static const char* kSwizzle[] = {"", "x", "xy", "xyz", "xyzw"};
            return "(" + expression + ")." + kSwizzle[want];
        }
        std::ostringstream os;
        os << (hlsl ? "float" : "vec") << want << "(" << expression;
        for (std::uint32_t i = have; i < want; ++i) os << ", " << (i == 3 ? "1.0" : "0.0");
        os << ")";
        return os.str();
    }

    std::string sample_call(const std::string& texture, const std::string& uv) const {
        if (hlsl) return texture + ".Sample(" + texture + "_sampler, " + uv + ")";
        return "texture(" + texture + ", " + uv + ")";
    }

    std::string mul_call(const std::string& a, const std::string& b) const {
        return hlsl ? "mul(" + a + ", " + b + ")" : "(" + a + " * " + b + ")";
    }
};

std::string sanitize(const std::string& raw) {
    std::string out;
    for (char c : raw) {
        out += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
    }
    if (out.empty()) out = "value";
    if (std::isdigit(static_cast<unsigned char>(out.front()))) out.insert(out.begin(), '_');
    return out;
}

std::string node_property(const Node& node, const std::string& key,
                          const std::string& fallback = {}) {
    auto it = node.properties.find(key);
    return it != node.properties.end() && !it->second.empty() ? it->second : fallback;
}

const char* value_noise_hlsl = R"(float ssstudio_hash21(float2 p) {
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float ssstudio_value_noise(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    float a = ssstudio_hash21(i);
    float b = ssstudio_hash21(i + float2(1.0, 0.0));
    float c = ssstudio_hash21(i + float2(0.0, 1.0));
    float d = ssstudio_hash21(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}
)";

const char* value_noise_glsl = R"(float ssstudio_hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float ssstudio_value_noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = ssstudio_hash21(i);
    float b = ssstudio_hash21(i + vec2(1.0, 0.0));
    float c = ssstudio_hash21(i + vec2(0.0, 1.0));
    float d = ssstudio_hash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}
)";

const char* hsv_hlsl = R"(float3 ssstudio_hsv_to_rgb(float3 hsv) {
    float3 k = frac(hsv.xxx + float3(1.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0;
    return hsv.z * lerp(float3(1.0, 1.0, 1.0), saturate(abs(k) - 1.0), hsv.y);
}
)";

const char* hsv_glsl = R"(vec3 ssstudio_hsv_to_rgb(vec3 hsv) {
    vec3 k = fract(hsv.xxx + vec3(1.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0;
    return hsv.z * mix(vec3(1.0), clamp01(abs(k) - 1.0), hsv.y);
}
)";

const char* clamp01_glsl = R"(float clamp01(float v) { return clamp(v, 0.0, 1.0); }
vec2 clamp01(vec2 v) { return clamp(v, vec2(0.0), vec2(1.0)); }
vec3 clamp01(vec3 v) { return clamp(v, vec3(0.0), vec3(1.0)); }
vec4 clamp01(vec4 v) { return clamp(v, vec4(0.0), vec4(1.0)); }
)";

}  // namespace

std::string generate_graph_source(const Graph& graph, const GraphCodegenOptions& options,
                                  Diagnostics& out_diags) {
    Diagnostics validation = validate_graph(graph);
    out_diags.insert(out_diags.end(), validation.begin(), validation.end());
    if (has_errors(validation)) return {};

    const std::vector<const Node*> order = topological_order(graph, out_diags);
    if (order.empty()) return {};

    Emitter emitter{graph, options, out_diags, graph.language == Language::HLSL, {}, {}, {}, {},
                    {}, {}};
    const bool hlsl = emitter.hlsl;

    // Space/set assignment mirrors the SDL contract the validator enforces.
    const std::uint32_t resource_space = graph.stage == Stage::Vertex ? 0 : 2;
    const std::uint32_t uniform_space = graph.stage == Stage::Vertex ? 1 : 3;

    std::ostringstream body;
    std::map<std::string, std::string> outputs;  // "node.pin" -> expression
    std::map<std::string, PinType> output_types;

    std::string color_expression;
    std::string position_expression;

    auto input_expression = [&](const Node& node, const PinDef& pin_def) {
        const Link* link = graph.incoming(node.id, pin_def.name);
        const PinType want = pin_def.type == PinType::Any
                                 ? PinType::Any
                                 : pin_def.type;
        if (!link) {
            auto it = node.values.find(pin_def.name);
            const std::vector<double>& values =
                it != node.values.end() ? it->second : pin_def.default_value;
            const PinType type = want == PinType::Any
                                     ? vector_of(static_cast<std::uint32_t>(values.size()))
                                     : want;
            return emitter.literal(values, type);
        }
        const std::string key = link->from_node + "." + link->from_pin;
        auto it = outputs.find(key);
        if (it == outputs.end()) return std::string("0.0");
        const PinType from = output_types.count(key) ? output_types.at(key) : PinType::Float;
        if (want == PinType::Any) return it->second;
        return emitter.coerce(it->second, from, want);
    };

    for (const Node* node_ptr : order) {
        const Node& node = *node_ptr;
        const NodeDef* def = resolve_node_def(graph, node.type);
        if (!def) continue;

        const std::string local = sanitize(node.name.empty() ? node.id : node.name) + "_" +
                                  sanitize(node.id);

        std::vector<std::string> args;
        args.reserve(def->inputs.size());
        for (const auto& pin_def : def->inputs) args.push_back(input_expression(node, pin_def));

        auto substitute = [&](std::string expression) {
            for (std::size_t i = 0; i < args.size(); ++i) {
                const std::string token = "$" + std::to_string(i);
                std::size_t pos = 0;
                while ((pos = expression.find(token, pos)) != std::string::npos) {
                    expression.replace(pos, token.size(), args[i]);
                    pos += args[i].size();
                }
            }
            return emitter.apply_dialect(expression);
        };

        // Nodes that declare something at file scope rather than compute a value.
        if (node.type == "input.uniform" || node.type == "input.macro") {
            const std::string name = sanitize(node_property(node, "name", node.id));
            const PinType type =
                pin_type_from_string(node_property(node, "type", "float")).value_or(PinType::Float);
            emitter.uniform_members.push_back(emitter.type_name(type) + " " + name + ";");
            outputs[node.id + ".value"] = name;
            output_types[node.id + ".value"] = type;
            continue;
        }
        if (node.type == "input.vertex_attribute") {
            const std::string name = sanitize(node_property(node, "name", node.id));
            const PinType type =
                pin_type_from_string(node_property(node, "type", "float4")).value_or(PinType::Float4);
            const std::string semantic = node_property(node, "semantic", "TEXCOORD0");
            emitter.vertex_attributes.push_back(emitter.type_name(type) + " " + name +
                                                (hlsl ? " : " + semantic : ""));
            outputs[node.id + ".value"] = hlsl ? "input." + name : name;
            output_types[node.id + ".value"] = type;
            continue;
        }
        if (node.type == "input.texture_sample") {
            const std::string name = sanitize(node_property(node, "name", node.id));
            if (std::none_of(emitter.textures.begin(), emitter.textures.end(),
                             [&](const auto& t) { return t.first == name; })) {
                emitter.textures.emplace_back(name, name + "_sampler");
            }
            const std::string uv = args.empty() ? (hlsl ? "input.uv" : "v_uv") : args[0];
            const std::string expression = emitter.sample_call(name, uv);
            if (options.include_node_comments && !node.name.empty()) {
                body << "    // " << node.name << "\n";
            }
            body << "    " << emitter.type_name(PinType::Float4) << " " << local << " = "
                 << expression << ";\n";
            outputs[node.id + ".color"] = local;
            output_types[node.id + ".color"] = PinType::Float4;
            continue;
        }
        if (node.type == "input.uv") {
            outputs[node.id + ".uv"] = hlsl ? "input.uv" : "v_uv";
            output_types[node.id + ".uv"] = PinType::Float2;
            continue;
        }
        if (node.type == "input.position") {
            outputs[node.id + ".position"] = hlsl ? "input.position" : "gl_FragCoord";
            output_types[node.id + ".position"] = PinType::Float4;
            continue;
        }
        if (node.type == "input.constant") {
            auto it = node.values.find("value");
            const std::vector<double> values = it != node.values.end() ? it->second
                                                                       : std::vector<double>{0.0};
            const PinType type =
                pin_type_from_string(node_property(node, "type", ""))
                    .value_or(vector_of(static_cast<std::uint32_t>(values.size())));
            outputs[node.id + ".value"] = emitter.literal(values, type);
            output_types[node.id + ".value"] = type;
            continue;
        }
        if (node.type == "output.color") {
            color_expression = args.empty() ? emitter.literal({0, 0, 0, 1}, PinType::Float4)
                                            : emitter.coerce(args[0], PinType::Float4, PinType::Float4);
            continue;
        }
        if (node.type == "output.position") {
            position_expression = args.empty() ? emitter.literal({0, 0, 0, 1}, PinType::Float4)
                                               : args[0];
            continue;
        }
        if (node.type == "output.varying") {
            const std::string name = sanitize(node_property(node, "name", node.id));
            const PinType type =
                pin_type_from_string(node_property(node, "type", "float4")).value_or(PinType::Float4);
            emitter.varyings.push_back(emitter.type_name(type) + " " + name);
            body << "    output." << name << " = " << (args.empty() ? "0.0" : args[0]) << ";\n";
            continue;
        }
        if (node.type == "color.split") {
            const std::string source = args.empty() ? "0.0" : args[0];
            if (options.include_node_comments && !node.name.empty()) {
                body << "    // " << node.name << "\n";
            }
            body << "    " << emitter.type_name(PinType::Float4) << " " << local << " = " << source
                 << ";\n";
            static const char* kComponents[] = {"r", "g", "b", "a"};
            static const char* kSwizzle[] = {"x", "y", "z", "w"};
            for (int i = 0; i < 4; ++i) {
                outputs[node.id + "." + kComponents[i]] = local + "." + kSwizzle[i];
                output_types[node.id + "." + kComponents[i]] = PinType::Float;
            }
            continue;
        }
        if (node.type == "custom.code") {
            const std::string code = node_property(node, "code", "return $0;");
            const PinType type =
                pin_type_from_string(node_property(node, "return_type", "float4"))
                    .value_or(PinType::Float4);
            if (options.include_node_comments) {
                body << "    // custom: " << (node.name.empty() ? node.id : node.name) << "\n";
            }
            // The body is inlined inside a block so locals cannot leak.
            body << "    " << emitter.type_name(type) << " " << local << ";\n";
            body << "    {\n";
            std::string inlined = substitute(code);
            std::size_t pos = 0;
            while ((pos = inlined.find("return ", pos)) != std::string::npos) {
                inlined.replace(pos, 7, local + " = ");
                pos += local.size() + 3;
            }
            std::istringstream lines(inlined);
            std::string line;
            while (std::getline(lines, line)) body << "        " << line << "\n";
            body << "    }\n";
            outputs[node.id + ".result"] = local;
            output_types[node.id + ".result"] = type;
            continue;
        }

        // Ordinary value node.
        std::string expression;
        if (node.type == "math.mul_matrix") {
            expression = emitter.mul_call(args[0], args[1]);
        } else if (node.type == "input.color") {
            // Built up component by component rather than handed to literal()
            // as it stands: literal() pads a short list by repeating its last
            // value, which would turn a colour saved without an alpha into a
            // transparent one instead of an opaque one.
            std::vector<double> rgba = {0.0, 0.0, 0.0, 1.0};
            if (const auto it = node.values.find("color"); it != node.values.end()) {
                for (std::size_t i = 0; i < rgba.size() && i < it->second.size(); ++i) {
                    rgba[i] = it->second[i];
                }
            } else if (!def->outputs.empty() && def->outputs.front().default_value.size() == 4) {
                rgba = def->outputs.front().default_value;
            }
            expression = emitter.literal(rgba, PinType::Float4);
        } else if (node.type == "utility.swizzle") {
            const std::string spelling = node_property(node, "swizzle", "xy");
            PinType source = PinType::Float;
            if (const Link* link = graph.incoming(node.id, "value")) {
                const std::string key = link->from_node + "." + link->from_pin;
                if (output_types.count(key)) source = output_types.at(key);
            } else if (const auto it = node.values.find("value"); it != node.values.end()) {
                source = vector_of(static_cast<std::uint32_t>(it->second.size()));
            }
            // One component off a scalar is the scalar: neither language spells
            // that as a swizzle, and validation has already refused any longer
            // swizzle of one.
            expression = (component_count(source) <= 1 && spelling.size() <= 1)
                             ? "(" + args[0] + ")"
                             : "(" + args[0] + ")." + swizzle_xyzw(spelling);
        } else if (node.type == "utility.component") {
            const std::uint32_t index =
                component_index(node_property(node, "component", "x")).value_or(0);
            // What is actually feeding the pin, not what the pin is declared as:
            // the input is Any, so the width is only knowable from the link.
            PinType source = PinType::Float;
            if (const Link* link = graph.incoming(node.id, "value")) {
                const std::string key = link->from_node + "." + link->from_pin;
                if (output_types.count(key)) source = output_types.at(key);
            } else if (const auto it = node.values.find("value"); it != node.values.end()) {
                source = vector_of(static_cast<std::uint32_t>(it->second.size()));
            }
            // A scalar has no components to take. HLSL would accept ".x" on one
            // anyway; GLSL would not, so the value passes through instead - which
            // is also the answer a reader expects from "the x of a float".
            expression = component_count(source) <= 1
                             ? "(" + args[0] + ")"
                             : "(" + args[0] + ")." + std::string(component_name(index));
        } else {
            expression = substitute(def->expression);
        }
        if (def->expression.find("ssstudio_value_noise") != std::string::npos) {
            emitter.helpers.insert("noise");
        }
        if (def->expression.find("ssstudio_hsv_to_rgb") != std::string::npos) {
            emitter.helpers.insert("hsv");
        }
        if (!hlsl && def->expression.find("SATURATE(") != std::string::npos) {
            emitter.helpers.insert("clamp01");
        }
        if (!hlsl && (def->type == "color.hsv_to_rgb")) emitter.helpers.insert("clamp01");

        PinType out_type =
            def->outputs.empty() ? PinType::Float
                                 : (def->outputs.front().type == PinType::Any
                                        ? [&] {
                                              PinType widest = PinType::Float;
                                              for (std::size_t i = 0; i < def->inputs.size(); ++i) {
                                                  const Link* link =
                                                      graph.incoming(node.id, def->inputs[i].name);
                                                  if (!link) continue;
                                                  const std::string key =
                                                      link->from_node + "." + link->from_pin;
                                                  const PinType t = output_types.count(key)
                                                                        ? output_types.at(key)
                                                                        : PinType::Float;
                                                  if (component_count(t) > component_count(widest)) {
                                                      widest = t;
                                                  }
                                              }
                                              return widest;
                                          }()
                                        : def->outputs.front().type);

        // The widest-input rule above is right for arithmetic and wrong for a
        // swizzle, whose width is the length of the swizzle rather than the size
        // of what feeds it. Asking the shared resolver rather than repeating its
        // reasoning is what keeps the declared type and the expression agreeing.
        if (node.type == "utility.swizzle" && !def->outputs.empty()) {
            out_type = resolved_pin_type(graph, node, *def, def->outputs.front().name, true);
        }

        if (options.include_node_comments && !node.name.empty()) {
            body << "    // " << node.name << "\n";
        }
        body << "    " << emitter.type_name(out_type) << " " << local << " = " << expression
             << ";\n";
        if (!def->outputs.empty()) {
            outputs[node.id + "." + def->outputs.front().name] = local;
            output_types[node.id + "." + def->outputs.front().name] = out_type;
        }
    }

    // --- assemble ----------------------------------------------------------
    std::ostringstream os;
    os << "// Generated from graphs/" << graph.shader_id << ".toml by " << options.generated_by
       << ".\n"
       << "// Editing this file does nothing until you detach the graph; detaching keeps this\n"
       << "// source and stops regenerating it.\n\n";

    if (!hlsl) os << "#version 450\n\n";

    if (emitter.helpers.count("clamp01")) os << clamp01_glsl << "\n";
    if (emitter.helpers.count("noise")) os << (hlsl ? value_noise_hlsl : value_noise_glsl) << "\n";
    if (emitter.helpers.count("hsv")) os << (hlsl ? hsv_hlsl : hsv_glsl) << "\n";

    // Textures and samplers.
    for (std::size_t i = 0; i < emitter.textures.size(); ++i) {
        const auto& [name, sampler] = emitter.textures[i];
        if (hlsl) {
            os << "Texture2D<float4> " << name << " : register(t" << i << ", space"
               << resource_space << ");\n";
            os << "SamplerState " << sampler << " : register(s" << i << ", space" << resource_space
               << ");\n";
        } else {
            os << "layout(set = " << resource_space << ", binding = " << i << ") uniform sampler2D "
               << name << ";\n";
        }
    }
    if (!emitter.textures.empty()) os << "\n";

    // Uniform block.
    if (!emitter.uniform_members.empty()) {
        if (hlsl) {
            os << "cbuffer GraphUniforms : register(b0, space" << uniform_space << ") {\n";
            for (const auto& member : emitter.uniform_members) os << "    " << member << "\n";
            os << "};\n\n";
        } else {
            os << "layout(set = " << uniform_space << ", binding = 0) uniform GraphUniforms {\n";
            for (const auto& member : emitter.uniform_members) os << "    " << member << "\n";
            os << "} u;\n\n";
        }
    }

    if (graph.stage == Stage::Fragment) {
        if (hlsl) {
            os << "struct Input {\n"
               << "    float4 position : SV_Position;\n"
               << "    float2 uv       : TEXCOORD0;\n"
               << "};\n\n";
            os << "float4 " << options.entry_point << "(Input input) : SV_Target0 {\n";
            os << body.str();
            os << "    return "
               << (color_expression.empty() ? "float4(0.0, 0.0, 0.0, 1.0)" : color_expression)
               << ";\n";
            os << "}\n";
        } else {
            os << "layout(location = 0) in vec2 v_uv;\n";
            os << "layout(location = 0) out vec4 out_color;\n\n";
            os << "void " << options.entry_point << "() {\n";
            os << body.str();
            os << "    out_color = "
               << (color_expression.empty() ? "vec4(0.0, 0.0, 0.0, 1.0)" : color_expression)
               << ";\n";
            os << "}\n";
        }
    } else if (graph.stage == Stage::Vertex) {
        if (hlsl) {
            os << "struct Input {\n";
            for (const auto& attribute : emitter.vertex_attributes) {
                os << "    " << attribute << ";\n";
            }
            os << "};\n\n";
            os << "struct Output {\n    float4 position : SV_Position;\n";
            for (std::size_t i = 0; i < emitter.varyings.size(); ++i) {
                os << "    " << emitter.varyings[i] << " : TEXCOORD" << i << ";\n";
            }
            os << "};\n\n";
            os << "Output " << options.entry_point << "(Input input) {\n";
            os << "    Output output;\n";
            os << body.str();
            os << "    output.position = "
               << (position_expression.empty() ? "float4(0.0, 0.0, 0.0, 1.0)" : position_expression)
               << ";\n";
            os << "    return output;\n}\n";
        } else {
            for (std::size_t i = 0; i < emitter.vertex_attributes.size(); ++i) {
                os << "layout(location = " << i << ") in " << emitter.vertex_attributes[i] << ";\n";
            }
            for (std::size_t i = 0; i < emitter.varyings.size(); ++i) {
                os << "layout(location = " << i << ") out " << emitter.varyings[i] << ";\n";
            }
            os << "\nvoid " << options.entry_point << "() {\n";
            os << body.str();
            os << "    gl_Position = "
               << (position_expression.empty() ? "vec4(0.0, 0.0, 0.0, 1.0)" : position_expression)
               << ";\n}\n";
        }
    } else {
        out_diags.push_back([] {
            Diagnostic d;
            d.severity = Severity::Error;
            d.code = "SSSTUDIO-GRAPH";
            d.message = "compute graphs are not supported yet; write compute shaders as text";
            return d;
        }());
        return {};
    }

    std::string source = os.str();
    if (!graph.formatted_output) {
        // Compact mode drops comments and blank lines for people who only ever
        // read the generated file through a diff.
        std::istringstream lines(source);
        std::string line;
        std::string compact;
        while (std::getline(lines, line)) {
            const std::size_t first = line.find_first_not_of(" \t");
            if (first != std::string::npos && line.compare(first, 2, "//") == 0) continue;
            if (line.find_first_not_of(" \t") == std::string::npos) continue;
            compact += line + "\n";
        }
        source = compact;
    }
    return source;
}

}  // namespace ssstudio
