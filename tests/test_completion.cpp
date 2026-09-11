#include "ssstudio/completion.h"
#include "test.h"

using namespace ssstudio;

namespace {

Reflection fragment_reflection() {
    Reflection r;
    r.stage = Stage::Fragment;
    Resource tex;
    tex.name = "albedo_texture";
    tex.kind = ResourceKind::SampledTexture;
    tex.set = 2;
    r.resources.push_back(tex);

    UniformBlock block;
    block.name = "Frame";
    block.set = 3;
    UniformMember member;
    member.name = "tint_color";
    member.type = ScalarType::Float;
    member.cols = 4;
    block.members.push_back(member);
    r.uniform_blocks.push_back(block);
    return r;
}

bool contains(const std::vector<Completion>& list, const std::string& label) {
    for (const auto& c : list) {
        if (c.label == label) return true;
    }
    return false;
}

int index_of(const std::vector<Completion>& list, const std::string& label) {
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (list[i].label == label) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace

TEST(completion_prefers_project_names_over_language_names) {
    CompletionContext ctx;
    const Reflection reflection = fragment_reflection();
    ctx.reflection = &reflection;
    ctx.macros = {"time", "resolution"};

    const auto list = complete("", ctx);
    const int resource = index_of(list, "albedo_texture");
    const int intrinsic = index_of(list, "saturate");
    CHECK(resource >= 0);
    CHECK(intrinsic >= 0);
    CHECK(resource < intrinsic);
}

TEST(completion_matches_prefix_then_substring_then_subsequence) {
    CompletionContext ctx;
    const auto prefix = complete("norm", ctx);
    CHECK(contains(prefix, "normalize"));

    const auto subsequence = complete("smst", ctx);
    CHECK(contains(subsequence, "smoothstep"));

    const auto nothing = complete("zzzzq", ctx);
    CHECK(nothing.empty());
}

TEST(completion_is_language_aware) {
    CompletionContext hlsl;
    hlsl.language = Language::HLSL;
    const auto hlsl_list = complete("l", hlsl);
    CHECK(contains(hlsl_list, "lerp"));
    CHECK(!contains(hlsl_list, "mix"));

    CompletionContext glsl;
    glsl.language = Language::GLSL;
    const auto glsl_list = complete("m", glsl);
    CHECK(contains(glsl_list, "mix"));
    CHECK(!contains(glsl_list, "lerp"));
}

TEST(cross_language_docs_name_the_other_spelling) {
    CompletionContext ctx;
    ctx.language = Language::HLSL;
    for (const auto& c : complete("frac", ctx)) {
        if (c.label != "frac") continue;
        CHECK(c.doc.find("fract") != std::string::npos);
    }
}

TEST(uniform_members_and_macros_are_offered) {
    CompletionContext ctx;
    const Reflection reflection = fragment_reflection();
    ctx.reflection = &reflection;
    ctx.macros = {"time"};

    CHECK(contains(complete("tint", ctx), "tint_color"));
    CHECK(contains(complete("tim", ctx), "time"));
}

TEST(snippets_carry_the_right_register_space_per_stage) {
    CompletionContext fragment;
    fragment.stage = Stage::Fragment;
    bool checked = false;
    for (const auto& c : complete("cbuffer", fragment)) {
        if (c.kind != CompletionKind::Snippet) continue;
        CHECK(c.detail.find("space3") != std::string::npos);
        checked = true;
    }
    CHECK(checked);

    CompletionContext vertex;
    vertex.stage = Stage::Vertex;
    for (const auto& c : complete("cbuffer", vertex)) {
        if (c.kind != CompletionKind::Snippet) continue;
        CHECK(c.detail.find("space1") != std::string::npos);
    }
}

TEST(prefix_at_reads_the_word_under_the_cursor) {
    const std::string text = "float4 color = norm";
    CHECK_STREQ(prefix_at(text, text.size()), "norm");
    CHECK_STREQ(prefix_at(text, 6), "float4");
    CHECK_STREQ(prefix_at("a + ", 4), "");
}

TEST(signature_help_finds_the_enclosing_call) {
    const std::string text = "float x = smoothstep(0.0, 1.0, ";
    CHECK_STREQ(signature_at(text, text.size(), Language::HLSL),
                "smoothstep(edge0, edge1, x)");

    const std::string nested = "float x = max(dot(a, b), ";
    CHECK_STREQ(signature_at(nested, nested.size(), Language::HLSL), "max(a, b)");

    CHECK_STREQ(signature_at("float x = 1.0;", 14, Language::HLSL), "");
}

TEST(completion_respects_its_limit) {
    CompletionContext ctx;
    CHECK(complete("", ctx, 5).size() <= std::size_t{5});
}

// --- HLSL semantics ------------------------------------------------------

TEST(semantic_position_is_the_word_after_a_colon) {
    CHECK(semantic_position_at("float4 pos : SV_", 16));
    CHECK(semantic_position_at("float4 pos :", 12));
    CHECK(!semantic_position_at("float4 pos = a", 14));
    // A scope qualifier and the conditional operator both end in ':' without
    // introducing a semantic.
    CHECK(!semantic_position_at("Ns::name", 8));
    CHECK(!semantic_position_at("a ? b : c", 9));
}

TEST(a_semantic_position_offers_semantics_and_nothing_else) {
    CompletionContext ctx;
    ctx.language = Language::HLSL;
    ctx.stage = Stage::Fragment;
    ctx.semantic_position = true;

    const auto list = complete("SV_", ctx);
    CHECK(!list.empty());
    for (const auto& c : list) CHECK(c.kind == CompletionKind::Semantic);
    CHECK(contains(list, "SV_Target"));
    // Nothing from the ordinary list leaks in.
    CHECK(!contains(list, "saturate"));
    CHECK(!contains(list, "float4"));
}

TEST(semantics_for_this_stage_come_first) {
    CompletionContext fragment;
    fragment.stage = Stage::Fragment;
    fragment.semantic_position = true;
    const auto list = complete("SV_", fragment);
    CHECK(index_of(list, "SV_Target") < index_of(list, "SV_VertexID"));

    CompletionContext compute;
    compute.stage = Stage::Compute;
    compute.semantic_position = true;
    const auto dispatch = complete("SV_", compute);
    CHECK(index_of(dispatch, "SV_DispatchThreadID") < index_of(dispatch, "SV_Target"));
}

TEST(bare_semantics_are_offered_even_though_they_are_not_colored) {
    // TEXCOORD0 and NORMAL are deliberately absent from the highlighter's
    // keyword list, because a shader may use those words as variable names.
    // After a ':' they cannot be anything else, so completion still offers them.
    CompletionContext ctx;
    ctx.stage = Stage::Vertex;
    ctx.semantic_position = true;
    CHECK(contains(complete("TEX", ctx), "TEXCOORD0"));
    CHECK(contains(complete("NORM", ctx), "NORMAL"));
}

TEST(glsl_has_no_semantics_so_the_popup_stays_shut) {
    CompletionContext ctx;
    ctx.language = Language::GLSL;
    ctx.semantic_position = true;
    CHECK(complete("SV_", ctx).empty());
}

// --- control flow statements --------------------------------------------

TEST(control_flow_is_offered_as_a_whole_statement) {
    CompletionContext ctx;
    ctx.indent = "  ";
    const auto list = complete("fo", ctx);
    CHECK(!list.empty());
    CHECK_STREQ(list[0].label, "for");
    CHECK(list[0].kind == CompletionKind::Snippet);
    // The body carries the loop, indented the way the editor is configured.
    CHECK(list[0].insert_text().find("for (int i = 0;") != std::string::npos);
    CHECK(list[0].insert_text().find("\n  \n}") != std::string::npos);
    // And the caret lands inside the parentheses rather than at the end.
    CHECK(list[0].caret > 0);
    CHECK(list[0].caret < static_cast<int>(list[0].insert_text().size()));
}

TEST(statement_snippets_do_not_double_up_with_the_bare_keyword) {
    CompletionContext ctx;
    int seen = 0;
    for (const auto& c : complete("if", ctx)) {
        if (c.label == "if") ++seen;
    }
    CHECK(seen == 1);
    // A keyword no statement covers is still offered on its own.
    CHECK(contains(complete("disc", ctx), "discard"));
}

TEST(a_candidate_with_no_snippet_body_inserts_its_label) {
    CompletionContext ctx;
    for (const auto& c : complete("normalize", ctx)) {
        if (c.label != "normalize") continue;
        CHECK_STREQ(c.insert_text(), "normalize");
        CHECK(c.caret == -1);
    }
}

// --- the symbol scan -----------------------------------------------------

namespace {

const Symbol* find_symbol(const DocumentSymbols& symbols, const std::string& name) {
    for (const auto& s : symbols.symbols) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

constexpr const char* kShader = R"(
#define PI 3.14159

struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

Texture2D<float4> albedo : register(t0, space2);

float3 tonemap(float3 color, float exposure) {
    float3 scaled = color * exposure;
    return scaled / (scaled + 1.0);
}

float4 main(VSOutput input) : SV_Target {
    float2 uv = input.uv;
    float3 lit = tonemap(uv.xyy, PI);
    return float4(lit, 1.0);
}
)";

}  // namespace

TEST(the_scan_finds_functions_with_their_signatures) {
    const auto symbols = scan_symbols(kShader, Language::HLSL);
    const Symbol* tonemap = find_symbol(symbols, "tonemap");
    CHECK(tonemap != nullptr);
    CHECK(tonemap->kind == SymbolKind::Function);
    CHECK_STREQ(tonemap->signature, "float3 tonemap(float3 color, float exposure)");
    CHECK(find_symbol(symbols, "main") != nullptr);
}

TEST(the_scan_finds_variables_parameters_structs_and_defines) {
    const auto symbols = scan_symbols(kShader, Language::HLSL);

    const Symbol* scaled = find_symbol(symbols, "scaled");
    CHECK(scaled != nullptr);
    CHECK(scaled->kind == SymbolKind::Variable);
    CHECK_STREQ(scaled->type, "float3");

    const Symbol* exposure = find_symbol(symbols, "exposure");
    CHECK(exposure != nullptr);
    CHECK_STREQ(exposure->type, "float");

    const Symbol* output = find_symbol(symbols, "VSOutput");
    CHECK(output != nullptr);
    CHECK(output->kind == SymbolKind::Struct);

    const Symbol* pi = find_symbol(symbols, "PI");
    CHECK(pi != nullptr);
    CHECK_STREQ(pi->type, "#define");

    // A declaration whose type is one the shader itself introduced.
    const Symbol* input = find_symbol(symbols, "input");
    CHECK(input != nullptr);
    CHECK_STREQ(input->type, "VSOutput");

    // Angle brackets belong to the type, not to the name after them.
    const Symbol* texture = find_symbol(symbols, "albedo");
    CHECK(texture != nullptr);
    CHECK_STREQ(texture->type, "Texture2D");
}

TEST(the_scan_does_not_mistake_a_call_for_a_declaration) {
    const auto symbols = scan_symbols("float4 c = float4(1.0, 0.0, 0.0, 1.0);\n"
                                      "float d = dot(c.rgb, c.rgb);\n",
                                      Language::HLSL);
    CHECK(find_symbol(symbols, "c") != nullptr);
    CHECK(find_symbol(symbols, "d") != nullptr);
    // "float4(...)" is a constructor call, not a function this shader declares.
    for (const auto& s : symbols.symbols) CHECK(s.kind != SymbolKind::Function);
}

TEST(the_scan_reads_glsl_declarations_too) {
    const auto symbols = scan_symbols(
        "layout(set = 2, binding = 0) uniform sampler2D tex;\n"
        "vec4 shade(vec2 uv) {\n"
        "    vec3 base = texture(tex, uv).rgb;\n"
        "    return vec4(base, 1.0);\n"
        "}\n",
        Language::GLSL);
    CHECK(find_symbol(symbols, "tex") != nullptr);
    CHECK(find_symbol(symbols, "uv") != nullptr);
    const Symbol* base = find_symbol(symbols, "base");
    CHECK(base != nullptr);
    CHECK_STREQ(base->type, "vec3");
    const Symbol* shade = find_symbol(symbols, "shade");
    CHECK(shade != nullptr);
    CHECK(shade->kind == SymbolKind::Function);
}

TEST(declared_names_are_offered_above_the_language_builtins) {
    const std::string text = kShader;
    const auto symbols = scan_symbols(text, Language::HLSL);
    CompletionContext ctx;
    ctx.symbols = &symbols;
    // In main's body, after both of its locals have been written.
    ctx.cursor = text.find("return float4(lit");
    CHECK(ctx.cursor != std::string::npos);

    const auto list = complete("ton", ctx);
    CHECK(index_of(list, "tonemap") == 0);

    // A local outranks an intrinsic that matches the same prefix.
    const auto both = complete("u", ctx);
    CHECK(index_of(both, "uv") >= 0);
    CHECK(index_of(both, "uv") < index_of(both, "uint"));

    // tonemap's local is not one of main's, and a struct field is only ever
    // reachable through the '.' that qualifies it.
    CHECK(!contains(complete("scal", ctx), "scaled"));
    CHECK(!contains(complete("posi", ctx), "position"));
}

TEST(signature_help_answers_for_the_shaders_own_functions) {
    const std::string text = kShader;
    const auto symbols = scan_symbols(text, Language::HLSL);
    const std::string call = "float3 c = tonemap(";
    CHECK_STREQ(signature_at(call, call.size(), Language::HLSL, &symbols),
                "float3 tonemap(float3 color, float exposure)");
    // Without the scan it is still only the intrinsics that answer.
    CHECK_STREQ(signature_at(call, call.size(), Language::HLSL), "");
}

TEST(a_local_is_out_of_scope_outside_the_block_that_declares_it) {
    const std::string text = kShader;
    const auto symbols = scan_symbols(text, Language::HLSL);
    const Symbol* scaled = find_symbol(symbols, "scaled");
    CHECK(scaled != nullptr);

    // Inside tonemap's body, just after the declaration.
    const std::size_t inside = text.find("return scaled");
    CHECK(inside != std::string::npos);
    CHECK(symbol_is_visible(*scaled, inside));

    // In main, which is past tonemap's closing brace.
    const std::size_t elsewhere = text.find("float3 lit");
    CHECK(elsewhere != std::string::npos);
    CHECK(!symbol_is_visible(*scaled, elsewhere));

    // And before it was written at all.
    CHECK(!symbol_is_visible(*scaled, text.find("float3 tonemap")));
}

// --- stage-aware built-ins ----------------------------------------------

TEST(built_ins_the_stage_does_not_have_are_not_offered) {
    CompletionContext fragment;
    fragment.stage = Stage::Fragment;
    CHECK(contains(complete("ddx", fragment), "ddx"));
    CHECK(contains(complete("fwid", fragment), "fwidth"));
    CHECK(contains(complete("disc", fragment), "discard"));

    // Derivatives need neighbouring pixels, which a vertex shader has none of.
    CompletionContext vertex;
    vertex.stage = Stage::Vertex;
    CHECK(!contains(complete("ddx", vertex), "ddx"));
    CHECK(!contains(complete("fwid", vertex), "fwidth"));
    CHECK(!contains(complete("disc", vertex), "discard"));

    // Barriers synchronise a workgroup, which only a dispatch has.
    CompletionContext compute;
    compute.stage = Stage::Compute;
    compute.language = Language::GLSL;
    CHECK(contains(complete("barr", compute), "barrier"));
    CHECK(!contains(complete("disc", compute), "discard"));

    CompletionContext glsl_fragment;
    glsl_fragment.language = Language::GLSL;
    glsl_fragment.stage = Stage::Fragment;
    CHECK(!contains(complete("barr", glsl_fragment), "barrier"));
    CHECK(contains(complete("dFd", glsl_fragment), "dFdx"));
}

// --- member access -------------------------------------------------------

TEST(member_access_reads_the_dotted_chain_before_the_caret) {
    const std::string one = "myVec.x";
    const auto simple = member_access_at(one, one.size());
    CHECK(simple.path.size() == std::size_t{1});
    CHECK_STREQ(simple.path[0], "myVec");
    CHECK_STREQ(simple.prefix, "x");

    // Just after the dot, with nothing typed yet.
    const std::string bare = "myVec.";
    CHECK(member_access_at(bare, bare.size()).path.size() == std::size_t{1});
    CHECK_STREQ(member_access_at(bare, bare.size()).prefix, "");

    const std::string chain = "input.uv.x";
    const auto nested = member_access_at(chain, chain.size());
    CHECK(nested.path.size() == std::size_t{2});
    CHECK_STREQ(nested.path[0], "input");
    CHECK_STREQ(nested.path[1], "uv");

    // A decimal point is not a member access.
    CHECK(member_access_at("float x = 1.0", 13).path.empty());
    // Neither is a plain word.
    CHECK(member_access_at("myVec", 5).path.empty());
}

TEST(a_vector_offers_its_components) {
    const std::string text = "float2 myVec = float2(0.0, 1.0);\nfloat a = myVec.";
    const auto symbols = scan_symbols(text, Language::HLSL);
    CompletionContext ctx;
    ctx.symbols = &symbols;
    ctx.cursor = text.size();
    ctx.member_path = {"myVec"};

    const auto list = complete("", ctx);
    CHECK(contains(list, "x"));
    CHECK(contains(list, "y"));
    CHECK(contains(list, "r"));
    CHECK(contains(list, "g"));
    // A float2 has no third component.
    CHECK(!contains(list, "z"));
    CHECK(!contains(list, "b"));
    // Nothing but members: the language's own names have no business here.
    for (const auto& c : list) {
        CHECK(c.kind == CompletionKind::Field);
    }
    // "myVec.xy = float2(...)" - the multi-component form is offered too.
    CHECK(contains(complete("xy", ctx), "xy"));
}

TEST(a_component_is_typed_by_how_many_of_them_were_taken) {
    const std::string text = "float4 color = float4(0.0, 0.0, 0.0, 1.0);\ncolor.";
    const auto symbols = scan_symbols(text, Language::HLSL);
    CompletionContext ctx;
    ctx.symbols = &symbols;
    ctx.cursor = text.size();
    ctx.member_path = {"color"};

    for (const auto& c : complete("", ctx)) {
        if (c.label == "x") CHECK_STREQ(c.detail, "float");
        if (c.label == "xy") CHECK_STREQ(c.detail, "float2");
        if (c.label == "rgb") CHECK_STREQ(c.detail, "float3");
        if (c.label == "xyzw") CHECK_STREQ(c.detail, "float4");
    }
    CHECK(contains(complete("", ctx), "w"));
    CHECK(contains(complete("", ctx), "a"));
}

TEST(glsl_vectors_swizzle_with_glsl_type_names) {
    const std::string text = "vec3 base = vec3(1.0);\nbase.";
    const auto symbols = scan_symbols(text, Language::GLSL);
    CompletionContext ctx;
    ctx.language = Language::GLSL;
    ctx.symbols = &symbols;
    ctx.cursor = text.size();
    ctx.member_path = {"base"};

    const auto list = complete("", ctx);
    CHECK(contains(list, "z"));
    CHECK(!contains(list, "w"));
    for (const auto& c : list) {
        // A single component of a vec3 is a float, not a "vec".
        if (c.label == "x") CHECK_STREQ(c.detail, "float");
        if (c.label == "xy") CHECK_STREQ(c.detail, "vec2");
    }
}

TEST(a_struct_offers_its_fields_and_a_chain_follows_their_types) {
    const std::string text = kShader;
    const auto symbols = scan_symbols(text, Language::HLSL);
    CompletionContext ctx;
    ctx.symbols = &symbols;
    ctx.cursor = text.find("float3 lit");
    CHECK(ctx.cursor != std::string::npos);

    ctx.member_path = {"input"};
    const auto fields = complete("", ctx);
    CHECK(contains(fields, "uv"));
    CHECK(contains(fields, "position"));
    for (const auto& c : fields) CHECK(c.kind == CompletionKind::Field);

    // input.uv is a float2, so input.uv. offers two components and no more.
    ctx.member_path = {"input", "uv"};
    const auto components = complete("", ctx);
    CHECK(contains(components, "x"));
    CHECK(contains(components, "y"));
    CHECK(!contains(components, "z"));
}

TEST(a_name_the_scan_never_saw_offers_nothing_rather_than_guessing) {
    const std::string text = "float4 main() : SV_Target { return 1.0; }";
    const auto symbols = scan_symbols(text, Language::HLSL);
    CompletionContext ctx;
    ctx.symbols = &symbols;
    ctx.cursor = text.size();
    ctx.member_path = {"mystery"};
    CHECK(complete("", ctx).empty());
}

TEST(a_uniform_block_offers_its_members_by_name) {
    const Reflection reflection = fragment_reflection();
    CompletionContext ctx;
    ctx.reflection = &reflection;
    ctx.member_path = {"Frame"};
    CHECK(contains(complete("", ctx), "tint_color"));
}

TEST(the_nearest_declaration_wins_when_a_name_is_reused) {
    const std::string text =
        "float3 wide;\n"
        "float4 shade() {\n"
        "    float2 wide;\n"
        "    return wide.\n"
        "}\n";
    const auto symbols = scan_symbols(text, Language::HLSL);
    CompletionContext ctx;
    ctx.symbols = &symbols;
    ctx.cursor = text.find("wide.\n") + 5;
    ctx.member_path = {"wide"};
    // The local float2 shadows the file-scope float3, so there is no z.
    const auto list = complete("", ctx);
    CHECK(contains(list, "y"));
    CHECK(!contains(list, "z"));
}
