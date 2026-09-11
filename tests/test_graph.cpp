#include <cmath>

#include "ssstudio/graph.h"
#include "test.h"

using namespace ssstudio;

namespace {

Node make_node(std::string id, std::string type, std::string name = {}) {
    Node n;
    n.id = std::move(id);
    n.type = std::move(type);
    n.name = name.empty() ? n.id : std::move(name);
    return n;
}

// uv -> noise -> combine -> output, plus a uniform driving the noise scale.
Graph plasma_graph(Language language) {
    Graph g;
    g.shader_id = "plasma";
    g.stage = Stage::Fragment;
    g.language = language;

    Node uv = make_node("uv", "input.uv", "screen uv");
    Node scale = make_node("scale", "input.uniform", "noise scale");
    scale.properties["name"] = "noise_scale";
    scale.properties["type"] = "float";
    Node noise = make_node("noise", "utility.noise", "plasma");
    Node combine = make_node("combine", "color.combine", "colour");
    combine.values["a"] = {1.0};
    Node out = make_node("out", "output.color", "target");

    g.nodes = {uv, scale, noise, combine, out};
    g.links = {{"uv", "uv", "noise", "uv"},
               {"scale", "value", "noise", "scale"},
               {"noise", "result", "combine", "r"},
               {"noise", "result", "combine", "g"},
               {"combine", "result", "out", "color"}};
    return g;
}

std::string generate(const Graph& g, Diagnostics& diags) {
    GraphCodegenOptions options;
    options.generated_by = "test";
    return generate_graph_source(g, options, diags);
}

}  // namespace

TEST(graph_library_is_registered_and_findable) {
    CHECK(!builtin_nodes().empty());
    CHECK(find_node_def("math.add") != nullptr);
    CHECK(find_node_def("output.color") != nullptr);
    CHECK(find_node_def("does.not.exist") == nullptr);
}

TEST(default_graph_validates) {
    const Graph g = Graph::create_default("sprite_frag", Stage::Fragment, Language::HLSL);
    const Diagnostics d = validate_graph(g);
    CHECK(!has_errors(d));
}

TEST(graph_detects_cycles) {
    Graph g = plasma_graph(Language::HLSL);
    g.links.push_back({"combine", "result", "noise", "uv"});
    Diagnostics d;
    const auto order = topological_order(g, d);
    CHECK(order.empty());
    CHECK(has_errors(d));
}

TEST(graph_rejects_unknown_nodes_and_pins) {
    Graph g = plasma_graph(Language::HLSL);
    g.nodes.push_back(make_node("mystery", "not.a.node"));
    Diagnostics d = validate_graph(g);
    CHECK(has_errors(d));

    Graph h = plasma_graph(Language::HLSL);
    h.links.push_back({"uv", "nope", "combine", "b"});
    CHECK(has_errors(validate_graph(h)));
}

TEST(graph_rejects_two_links_into_one_input) {
    Graph g = plasma_graph(Language::HLSL);
    g.links.push_back({"uv", "uv", "combine", "r"});  // "r" already has a link
    CHECK(has_errors(validate_graph(g)));
}

TEST(graph_requires_an_output_node) {
    Graph g = plasma_graph(Language::HLSL);
    g.nodes.erase(g.nodes.end() - 1);
    g.links.pop_back();
    CHECK(has_errors(validate_graph(g)));
}

TEST(coercion_strictness_is_honoured) {
    Graph g;
    g.shader_id = "x";
    g.stage = Stage::Fragment;

    Node uv = make_node("uv", "input.uv");           // float2
    Node out = make_node("out", "output.color");     // wants float4
    g.nodes = {uv, out};
    g.links = {{"uv", "uv", "out", "color"}};

    g.coercion = Coercion::Strict;
    CHECK(has_errors(validate_graph(g)));

    g.coercion = Coercion::Widening;  // float2 -> float4 pads
    CHECK(!has_errors(validate_graph(g)));
}

TEST(strict_coercion_still_allows_exact_matches) {
    Graph g = plasma_graph(Language::HLSL);
    g.coercion = Coercion::Strict;
    // noise (float) into combine.r (float) and combine.result (float4) into
    // output.color (float4): every link is an exact match.
    CHECK(!has_errors(validate_graph(g)));
}

TEST(hlsl_generation_produces_expected_declarations) {
    Graph g = plasma_graph(Language::HLSL);
    Diagnostics d;
    const std::string source = generate(g, d);
    CHECK(!has_errors(d));

    CHECK(source.find("cbuffer GraphUniforms : register(b0, space3)") != std::string::npos);
    CHECK(source.find("float noise_scale;") != std::string::npos);
    CHECK(source.find("float4 main(Input input) : SV_Target0") != std::string::npos);
    CHECK(source.find("ssstudio_value_noise") != std::string::npos);
    CHECK(source.find("// plasma") != std::string::npos);   // node name as comment
    CHECK(source.find("return") != std::string::npos);
    CHECK(source.find("vec4(") == std::string::npos);       // no GLSL leaking in
}

TEST(glsl_generation_uses_glsl_spellings) {
    Graph g = plasma_graph(Language::GLSL);
    Diagnostics d;
    const std::string source = generate(g, d);
    CHECK(!has_errors(d));

    CHECK(source.find("#version 450") != std::string::npos);
    CHECK(source.find("layout(set = 3, binding = 0) uniform GraphUniforms") != std::string::npos);
    CHECK(source.find("out vec4 out_color") != std::string::npos);
    CHECK(source.find("float4") == std::string::npos);      // no HLSL leaking in
    CHECK(source.find("cbuffer") == std::string::npos);
}

TEST(texture_nodes_declare_texture_and_sampler_in_the_right_space) {
    Graph g;
    g.shader_id = "sprite";
    g.stage = Stage::Fragment;

    Node uv = make_node("uv", "input.uv");
    Node sample = make_node("sample", "input.texture_sample", "albedo");
    sample.properties["name"] = "albedo";
    Node out = make_node("out", "output.color");
    g.nodes = {uv, sample, out};
    g.links = {{"uv", "uv", "sample", "uv"}, {"sample", "color", "out", "color"}};

    Diagnostics d;
    const std::string hlsl = generate(g, d);
    CHECK(!has_errors(d));
    CHECK(hlsl.find("Texture2D<float4> albedo : register(t0, space2)") != std::string::npos);
    CHECK(hlsl.find("SamplerState albedo_sampler : register(s0, space2)") != std::string::npos);
    CHECK(hlsl.find("albedo.Sample(albedo_sampler, input.uv)") != std::string::npos);

    g.language = Language::GLSL;
    Diagnostics d2;
    const std::string glsl = generate(g, d2);
    CHECK(!has_errors(d2));
    CHECK(glsl.find("layout(set = 2, binding = 0) uniform sampler2D albedo") != std::string::npos);
    CHECK(glsl.find("texture(albedo, v_uv)") != std::string::npos);
}

TEST(vertex_graph_uses_space0_and_space1) {
    Graph g;
    g.shader_id = "sprite_vert";
    g.stage = Stage::Vertex;

    Node position = make_node("pos", "input.vertex_attribute", "position");
    position.properties["name"] = "position";
    position.properties["semantic"] = "POSITION";
    position.properties["type"] = "float4";

    Node mvp = make_node("mvp", "input.uniform", "mvp");
    mvp.properties["name"] = "mvp";
    mvp.properties["type"] = "float4x4";

    Node transform = make_node("transform", "math.mul_matrix", "to clip space");
    Node out = make_node("out", "output.position");

    g.nodes = {position, mvp, transform, out};
    g.links = {{"mvp", "value", "transform", "matrix"},
               {"pos", "value", "transform", "vector"},
               {"transform", "result", "out", "position"}};

    Diagnostics d;
    const std::string source = generate(g, d);
    CHECK(!has_errors(d));
    CHECK(source.find("register(b0, space1)") != std::string::npos);
    CHECK(source.find("float4x4 mvp;") != std::string::npos);
    CHECK(source.find("mul(mvp, input.position)") != std::string::npos);
    CHECK(source.find("output.position =") != std::string::npos);
}

TEST(custom_code_nodes_are_inlined) {
    Graph g;
    g.shader_id = "custom";
    g.stage = Stage::Fragment;

    Node uv = make_node("uv", "input.uv");
    Node code = make_node("code", "custom.code", "swirl");
    code.properties["return_type"] = "float4";
    code.properties["code"] = "float2 p = $0 - 0.5;\nreturn float4(p, 0.0, 1.0);";
    Node out = make_node("out", "output.color");

    g.nodes = {uv, code, out};
    g.links = {{"uv", "uv", "code", "a"}, {"code", "result", "out", "color"}};

    Diagnostics d;
    const std::string source = generate(g, d);
    CHECK(!has_errors(d));
    CHECK(source.find("float2 p = input.uv - 0.5;") != std::string::npos);
    CHECK(source.find("= float4(p, 0.0, 1.0);") != std::string::npos);
    // The custom body must not smuggle a bare `return` into the middle.
    const std::size_t body = source.find("float2 p =");
    CHECK(source.find("return ", body) > source.find("SV_Target0"));
}

TEST(generation_is_deterministic) {
    Graph g = plasma_graph(Language::HLSL);
    Diagnostics d1, d2;
    CHECK_STREQ(generate(g, d1), generate(g, d2));
}

TEST(compact_output_drops_comments_and_blank_lines) {
    Graph g = plasma_graph(Language::HLSL);
    g.formatted_output = false;
    Diagnostics d;
    const std::string source = generate(g, d);
    CHECK(!has_errors(d));
    CHECK(source.find("// plasma") == std::string::npos);
    CHECK(source.find("\n\n") == std::string::npos);
}

TEST(graph_inputs_are_listed_for_the_io_panel) {
    Graph g = plasma_graph(Language::HLSL);
    const auto inputs = graph_inputs(g);
    CHECK_EQ(inputs.size(), std::size_t{1});
    if (!inputs.empty()) {
        CHECK_STREQ(inputs[0].name, "noise_scale");
        CHECK(!inputs[0].is_texture);
    }
}

TEST(generation_refuses_an_invalid_graph) {
    Graph g = plasma_graph(Language::HLSL);
    g.links.push_back({"combine", "result", "noise", "uv"});  // cycle
    Diagnostics d;
    CHECK(generate(g, d).empty());
    CHECK(has_errors(d));
}

TEST(custom_node_definitions_extend_the_library) {
    Graph g = plasma_graph(Language::HLSL);
    NodeDef def;
    def.type = "project.checker";
    def.label = "Checker";
    def.category = NodeCategory::Utility;
    def.inputs = {{"uv", PinType::Float2, {}}};
    def.outputs = {{"result", PinType::Float, {}}};
    def.expression = "frac(($0.x + $0.y) * 4.0)";
    g.custom_nodes.push_back(def);

    Node checker = make_node("checker", "project.checker", "checker");
    g.nodes.push_back(checker);
    g.links.push_back({"uv", "uv", "checker", "uv"});
    g.links.push_back({"checker", "result", "combine", "b"});

    CHECK(resolve_node_def(g, "project.checker") != nullptr);
    Diagnostics d;
    const std::string source = generate(g, d);
    CHECK(!has_errors(d));
    CHECK(source.find("frac((input.uv.x + input.uv.y) * 4.0)") != std::string::npos);
}

// --- the Component node --------------------------------------------------

namespace {

/// uv (float2) -> component -> combine.r -> output. The component node is the
/// one under test; everything else is scaffolding to give it something typed to
/// read from and somewhere legal to send a float.
Graph component_graph(const char* component, Language language = Language::HLSL) {
    Graph g;
    g.shader_id = "pick";
    g.stage = Stage::Fragment;
    g.language = language;

    Node uv = make_node("uv", "input.uv", "screen uv");
    Node pick = make_node("pick", "utility.component", "pick");
    pick.properties["component"] = component;
    Node combine = make_node("combine", "color.combine", "colour");
    combine.values["a"] = {1.0};
    Node out = make_node("out", "output.color", "target");

    g.nodes = {uv, pick, combine, out};
    g.links = {{"uv", "uv", "pick", "value"},
               {"pick", "result", "combine", "r"},
               {"combine", "result", "out", "color"}};
    return g;
}

}  // namespace

TEST(the_component_node_is_in_the_library) {
    const NodeDef* def = find_node_def("utility.component");
    CHECK(def != nullptr);
    CHECK(def->inputs.size() == std::size_t{1});
    // The input takes whatever it is given; the output is always one float.
    CHECK(def->inputs[0].type == PinType::Any);
    CHECK(def->outputs.size() == std::size_t{1});
    CHECK(def->outputs[0].type == PinType::Float);
}

TEST(component_names_map_to_indices_in_both_spellings) {
    CHECK(component_index("x").value() == std::uint32_t{0});
    CHECK(component_index("y").value() == std::uint32_t{1});
    CHECK(component_index("z").value() == std::uint32_t{2});
    CHECK(component_index("w").value() == std::uint32_t{3});
    // The colour spelling names the same components.
    CHECK(component_index("r").value() == std::uint32_t{0});
    CHECK(component_index("a").value() == std::uint32_t{3});
    // And anything else is refused rather than read as x.
    CHECK(!component_index("q").has_value());
    CHECK(!component_index("xy").has_value());
    CHECK(!component_index("").has_value());

    // Generated source always uses the xyzw spelling, whichever was typed.
    CHECK_STREQ(std::string(component_name(1)), "y");
}

TEST(a_component_node_takes_the_component_it_names) {
    Diagnostics diags;
    const Graph g = component_graph("y");
    CHECK(!has_errors(validate_graph(g)));

    const std::string source = generate(g, diags);
    CHECK(!has_errors(diags));
    CHECK(source.find(".y") != std::string::npos);
    CHECK(source.find(".z") == std::string::npos);
}

TEST(the_colour_spelling_generates_the_position_spelling) {
    Diagnostics diags;
    const Graph g = component_graph("g");
    CHECK(!has_errors(validate_graph(g)));
    const std::string source = generate(g, diags);
    // "g" is the second component, and the second component is written ".y".
    CHECK(source.find(".y") != std::string::npos);
}

TEST(a_component_the_input_does_not_have_is_an_error) {
    // uv is a float2; there is no z to take.
    const Graph g = component_graph("z");
    const Diagnostics d = validate_graph(g);
    CHECK(has_errors(d));
    bool explained = false;
    for (const auto& e : d) {
        if (e.message.find("component 'z'") != std::string::npos &&
            e.message.find("float2") != std::string::npos) {
            explained = true;
        }
    }
    CHECK(explained);

    // A spelling that is not a component at all is caught too.
    CHECK(has_errors(validate_graph(component_graph("q"))));
}

TEST(a_component_node_works_the_same_in_glsl) {
    Diagnostics diags;
    const Graph g = component_graph("x", Language::GLSL);
    CHECK(!has_errors(validate_graph(g)));
    const std::string source = generate(g, diags);
    CHECK(!has_errors(diags));
    CHECK(source.find(".x") != std::string::npos);
}

TEST(a_scalar_input_passes_through_rather_than_being_swizzled) {
    Graph g;
    g.shader_id = "pick";
    g.stage = Stage::Fragment;
    g.language = Language::GLSL;

    Node scale = make_node("scale", "input.uniform", "scale");
    scale.properties["name"] = "scale";
    scale.properties["type"] = "float";
    Node pick = make_node("pick", "utility.component", "pick");
    pick.properties["component"] = "x";
    Node combine = make_node("combine", "color.combine", "colour");
    combine.values["a"] = {1.0};
    Node out = make_node("out", "output.color", "target");

    g.nodes = {scale, pick, combine, out};
    g.links = {{"scale", "value", "pick", "value"},
               {"pick", "result", "combine", "r"},
               {"combine", "result", "out", "color"}};

    CHECK(!has_errors(validate_graph(g)));
    Diagnostics diags;
    const std::string source = generate(g, diags);
    CHECK(!has_errors(diags));
    // GLSL has no ".x" on a float, so the value is used as it stands.
    CHECK(source.find("scale).x") == std::string::npos);
    CHECK(source.find("scale") != std::string::npos);
}

// --- the Swizzle node ----------------------------------------------------

namespace {

/// position (float4) -> swizzle -> output.color. A float4 source so every
/// swizzle length has enough to draw from.
Graph swizzle_graph(const char* swizzle, Language language = Language::HLSL) {
    Graph g;
    g.shader_id = "swizzled";
    g.stage = Stage::Fragment;
    g.language = language;

    Node position = make_node("position", "input.position", "screen position");
    Node pick = make_node("pick", "utility.swizzle", "pick");
    pick.properties["swizzle"] = swizzle;
    Node out = make_node("out", "output.color", "target");

    g.nodes = {position, pick, out};
    g.links = {{"position", "position", "pick", "value"}, {"pick", "result", "out", "color"}};
    return g;
}

}  // namespace

TEST(swizzle_errors_name_what_is_wrong) {
    // Fine: within the four components a float4 has.
    CHECK(swizzle_error("xy", 4).empty());
    CHECK(swizzle_error("bgra", 4).empty());
    CHECK(swizzle_error("xxxx", 1).empty());

    CHECK(!swizzle_error("", 4).empty());
    CHECK(swizzle_error("xyzwx", 4).find("four") != std::string::npos);
    CHECK(swizzle_error("xq", 4).find("not a component") != std::string::npos);
    // One swizzle cannot draw from both naming sets.
    CHECK(swizzle_error("xg", 4).find("mixes") != std::string::npos);
    // And a component the source does not have.
    CHECK(swizzle_error("xyz", 2).find("does not exist") != std::string::npos);
}

TEST(a_swizzle_is_rewritten_into_the_xyzw_set) {
    CHECK_STREQ(swizzle_xyzw("rgb"), "xyz");
    CHECK_STREQ(swizzle_xyzw("ba"), "zw");
    CHECK_STREQ(swizzle_xyzw("xy"), "xy");
}

TEST(a_swizzle_node_is_as_wide_as_its_swizzle) {
    const NodeDef* def = find_node_def("utility.swizzle");
    CHECK(def != nullptr);

    // The output pin resolves from the property, not from the input's width.
    const Graph two = swizzle_graph("xy");
    const Node* node = two.find_node("pick");
    CHECK(node != nullptr);
    CHECK(resolved_pin_type(two, *node, *def, "result", true) == PinType::Float2);

    const Graph three = swizzle_graph("xyz");
    CHECK(resolved_pin_type(three, *three.find_node("pick"), *def, "result", true) ==
          PinType::Float3);

    const Graph one = swizzle_graph("w");
    CHECK(resolved_pin_type(one, *one.find_node("pick"), *def, "result", true) == PinType::Float);
}

TEST(a_swizzle_node_generates_the_swizzle) {
    Diagnostics diags;
    const Graph g = swizzle_graph("bgr");
    CHECK(!has_errors(validate_graph(g)));
    const std::string source = generate(g, diags);
    CHECK(!has_errors(diags));
    // Written in the xyzw set whichever way it was typed.
    CHECK(source.find(".zyx") != std::string::npos);
}

TEST(a_swizzle_the_input_cannot_supply_is_an_error) {
    Graph g = swizzle_graph("xy");
    // uv is a float2, so a three-component swizzle has nothing to take from.
    g.nodes[0] = make_node("position", "input.uv", "screen uv");
    g.links[0] = {"position", "uv", "pick", "value"};
    g.find_node("pick")->properties["swizzle"] = "xyz";
    CHECK(has_errors(validate_graph(g)));

    // Two components off a float2 is fine.
    g.find_node("pick")->properties["swizzle"] = "yx";
    CHECK(!has_errors(validate_graph(g)));
}

TEST(a_swizzle_node_works_the_same_in_glsl) {
    Diagnostics diags;
    const Graph g = swizzle_graph("xyzw", Language::GLSL);
    CHECK(!has_errors(validate_graph(g)));
    const std::string source = generate(g, diags);
    CHECK(!has_errors(diags));
    CHECK(source.find(".xyzw") != std::string::npos);
}

// --- the Colour input node -----------------------------------------------

namespace {

Graph color_input_graph(std::vector<double> rgba, Language language = Language::HLSL) {
    Graph g;
    g.shader_id = "flat";
    g.stage = Stage::Fragment;
    g.language = language;

    Node color = make_node("tint", "input.color", "tint");
    if (!rgba.empty()) color.values["color"] = std::move(rgba);
    Node out = make_node("out", "output.color", "target");

    g.nodes = {color, out};
    g.links = {{"tint", "color", "out", "color"}};
    return g;
}

}  // namespace

TEST(the_colour_input_is_one_float4_rather_than_four_channels) {
    const NodeDef* def = find_node_def("input.color");
    CHECK(def != nullptr);
    CHECK(def->category == NodeCategory::Input);
    CHECK(def->inputs.empty());
    CHECK(def->outputs.size() == std::size_t{1});
    CHECK(def->outputs[0].type == PinType::Float4);
    CHECK_STREQ(def->outputs[0].name, "color");
}

TEST(a_colour_input_bakes_the_colour_it_was_given) {
    Diagnostics diags;
    const Graph g = color_input_graph({0.25, 0.5, 0.75, 1.0});
    CHECK(!has_errors(validate_graph(g)));
    const std::string source = generate(g, diags);
    CHECK(!has_errors(diags));
    CHECK(source.find("float4(0.25, 0.5, 0.75, 1.0)") != std::string::npos);

    Diagnostics glsl_diags;
    const std::string glsl = generate(color_input_graph({1.0, 0.0, 0.0, 0.5}, Language::GLSL),
                                      glsl_diags);
    CHECK(glsl.find("vec4(1.0, 0.0, 0.0, 0.5)") != std::string::npos);
}

TEST(a_colour_input_with_nothing_stored_is_opaque_white) {
    Diagnostics diags;
    const std::string source = generate(color_input_graph({}), diags);
    CHECK(!has_errors(diags));
    CHECK(source.find("float4(1.0, 1.0, 1.0, 1.0)") != std::string::npos);
}

TEST(a_colour_saved_without_an_alpha_stays_opaque) {
    // literal() pads a short list by repeating its last value, which would make
    // a colour written as three components transparent rather than opaque.
    Diagnostics diags;
    const std::string source = generate(color_input_graph({1.0, 0.0, 0.0}), diags);
    CHECK(source.find("float4(1.0, 0.0, 0.0, 1.0)") != std::string::npos);
}

// --- the Constant node ---------------------------------------------------

namespace {

Graph constant_graph(std::vector<double> value, const char* type,
                     Language language = Language::HLSL) {
    Graph g;
    g.shader_id = "flat";
    g.stage = Stage::Fragment;
    g.language = language;

    Node constant = make_node("k", "input.constant", "k");
    if (!value.empty()) constant.values["value"] = std::move(value);
    if (type != nullptr) constant.properties["type"] = type;
    Node out = make_node("out", "output.color", "target");

    g.nodes = {constant, out};
    g.links = {{"k", "value", "out", "color"}};
    return g;
}

}  // namespace

TEST(a_constant_generates_the_value_it_holds) {
    Diagnostics diags;
    const Graph g = constant_graph({0.5, 0.25}, "float2");
    CHECK(!has_errors(validate_graph(g)));
    const std::string source = generate(g, diags);
    CHECK(!has_errors(diags));
    CHECK(source.find("float2(0.5, 0.25)") != std::string::npos);
}

TEST(a_constants_type_property_decides_its_width) {
    const NodeDef* def = find_node_def("input.constant");
    CHECK(def != nullptr);

    const Graph four = constant_graph({1.0, 1.0, 1.0, 1.0}, "float4");
    CHECK(resolved_pin_type(four, *four.find_node("k"), *def, "value", true) == PinType::Float4);

    const Graph one = constant_graph({2.0}, "float");
    CHECK(resolved_pin_type(one, *one.find_node("k"), *def, "value", true) == PinType::Float);

    // With no type recorded the stored value's own width decides, which is what
    // a graph written before the type picker existed relies on.
    const Graph implied = constant_graph({1.0, 0.0, 0.0}, nullptr);
    CHECK(resolved_pin_type(implied, *implied.find_node("k"), *def, "value", true) ==
          PinType::Float3);
}

TEST(a_scalar_constant_generates_a_bare_number) {
    Diagnostics diags;
    const std::string source = generate(constant_graph({0.75}, "float"), diags);
    CHECK(source.find("0.75") != std::string::npos);
    CHECK(source.find("float(0.75)") == std::string::npos);
}

TEST(the_colour_preset_node_is_gone) {
    // Removed in favour of the Colour input node. A graph file still naming it
    // reports an unknown node type rather than silently generating nothing.
    CHECK(find_node_def("color.preset") == nullptr);

    Graph g;
    g.shader_id = "stale";
    g.stage = Stage::Fragment;
    g.nodes = {make_node("old", "color.preset", "old")};
    CHECK(has_errors(validate_graph(g)));
}

// --- placing nodes without overlapping -----------------------------------

namespace {

bool rects_touch(const NodeRect& a, const NodeRect& b, float gap) {
    return a.x < b.x + b.width + gap && b.x < a.x + a.width + gap &&
           a.y < b.y + b.height + gap && b.y < a.y + a.height + gap;
}

NodeRect node_at(float x, float y) { return NodeRect{x, y, 160.0f, 80.0f}; }

}  // namespace

TEST(a_free_position_is_left_exactly_where_it_was_asked_for) {
    const std::vector<NodeRect> occupied = {node_at(0.0f, 0.0f)};
    NodeRect candidate = node_at(400.0f, 400.0f);
    place_without_overlap(occupied, candidate, 12.0f, 12.0f);
    CHECK(candidate.x == 400.0f);
    CHECK(candidate.y == 400.0f);
}

TEST(an_empty_canvas_never_moves_anything) {
    NodeRect candidate = node_at(-30.0f, 17.0f);
    place_without_overlap({}, candidate, 12.0f, 12.0f);
    CHECK(candidate.x == -30.0f);
    CHECK(candidate.y == 17.0f);
}

TEST(a_node_dropped_on_another_one_moves_off_it) {
    const std::vector<NodeRect> occupied = {node_at(0.0f, 0.0f)};
    NodeRect candidate = node_at(0.0f, 0.0f);
    place_without_overlap(occupied, candidate, 12.0f, 12.0f);
    CHECK(!rects_touch(candidate, occupied.front(), 12.0f));
}

TEST(a_node_dropped_into_a_crowd_finds_a_gap) {
    // A wall of nodes with nowhere free anywhere near the drop point.
    std::vector<NodeRect> occupied;
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            occupied.push_back(node_at(static_cast<float>(column) * 170.0f,
                                       static_cast<float>(row) * 90.0f));
        }
    }
    NodeRect candidate = node_at(180.0f, 90.0f);
    place_without_overlap(occupied, candidate, 12.0f, 12.0f);
    for (const auto& other : occupied) {
        CHECK(!rects_touch(candidate, other, 12.0f));
    }
}

TEST(a_displaced_node_moves_no_further_than_it_has_to) {
    const std::vector<NodeRect> occupied = {node_at(0.0f, 0.0f)};
    NodeRect candidate = node_at(0.0f, 0.0f);
    place_without_overlap(occupied, candidate, 12.0f, 12.0f);

    // Clearing a 160-wide neighbour plus a 12 gap needs 172; on a 12 grid the
    // first multiple past that is 180. Nothing should have moved further.
    const float moved = std::max(std::fabs(candidate.x), std::fabs(candidate.y));
    CHECK(moved <= 180.0f);
    // And it stays on the grid it was searched over.
    CHECK(std::fmod(std::fabs(candidate.x), 12.0f) == 0.0f);
    CHECK(std::fmod(std::fabs(candidate.y), 12.0f) == 0.0f);
}

TEST(the_gap_is_respected_rather_than_merely_not_overlapping) {
    const std::vector<NodeRect> occupied = {node_at(0.0f, 0.0f)};
    NodeRect candidate = node_at(0.0f, 0.0f);
    place_without_overlap(occupied, candidate, 4.0f, 40.0f);
    // Touching exactly is allowed; coming within the gap is not.
    CHECK(!rects_touch(candidate, occupied.front(), 40.0f));
}
