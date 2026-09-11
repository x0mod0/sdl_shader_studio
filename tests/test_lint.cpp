#include <string>

#include "ssstudio/lint.h"
#include "test.h"

using namespace ssstudio;

namespace {

bool has_code(const Diagnostics& d, const std::string& code) {
    for (const auto& x : d) {
        if (x.code == code) return true;
    }
    return false;
}

const Diagnostic* find(const Diagnostics& d, const std::string& code) {
    for (const auto& x : d) {
        if (x.code == code) return &x;
    }
    return nullptr;
}

}  // namespace

TEST(lint_names_the_right_spelling_for_a_cross_dialect_call) {
    const std::string hlsl = "float4 main() : SV_Target {\n    return mix(a, b, t);\n}\n";
    const Diagnostics d = lint(hlsl, Language::HLSL, Stage::Fragment);
    const Diagnostic* slip = find(d, "SSSTUDIO-DIALECT");
    CHECK(slip != nullptr);
    if (slip) {
        CHECK_EQ(slip->line, 2);
        CHECK(slip->message.find("lerp") != std::string::npos);
        CHECK(slip->severity == Severity::Warning);
    }

    const std::string glsl = "void main() {\n    c = lerp(a, b, t);\n}\n";
    // The Diagnostics must outlive the pointer into it; find() borrows.
    const Diagnostics glsl_diags = lint(glsl, Language::GLSL, Stage::Fragment);
    const Diagnostic* other = find(glsl_diags, "SSSTUDIO-DIALECT");
    CHECK(other != nullptr);
    if (other) CHECK(other->message.find("mix") != std::string::npos);
}

TEST(lint_leaves_correct_code_alone) {
    // The name that belongs to this dialect, and the same word used as a
    // variable rather than a call, are both fine.
    CHECK(!has_code(lint("c = lerp(a, b, t);", Language::HLSL, Stage::Fragment),
                    "SSSTUDIO-DIALECT"));
    CHECK(!has_code(lint("float mix = 0.5; c = a * mix;", Language::HLSL, Stage::Fragment),
                    "SSSTUDIO-DIALECT"));
    CHECK(!has_code(lint("c = saturate(x);", Language::HLSL, Stage::Fragment), "SSSTUDIO-DIALECT"));
}

TEST(lint_flags_a_glsl_resource_with_no_binding) {
    const std::string bad = "uniform sampler2D albedo;\n";
    const Diagnostics bad_diags = lint(bad, Language::GLSL, Stage::Fragment);
    const Diagnostic* d = find(bad_diags, "SSSTUDIO-BINDING");
    CHECK(d != nullptr);
    if (d) {
        CHECK_EQ(d->line, 1);
        CHECK(d->message.find("set 0") != std::string::npos);
    }

    const std::string good = "layout(set = 2, binding = 0) uniform sampler2D albedo;\n";
    CHECK(!has_code(lint(good, Language::GLSL, Stage::Fragment), "SSSTUDIO-BINDING"));

    // The qualifier search must stop at the previous statement rather than
    // treating one layout as covering everything below it.
    const std::string mixed =
        "layout(set = 2, binding = 0) uniform sampler2D albedo;\n"
        "uniform sampler2D normal;\n";
    const Diagnostics d2 = lint(mixed, Language::GLSL, Stage::Fragment);
    const Diagnostic* second = find(d2, "SSSTUDIO-BINDING");
    CHECK(second != nullptr);
    if (second) CHECK_EQ(second->line, 2);
}

TEST(lint_does_not_look_for_bindings_in_hlsl) {
    // HLSL says the same thing with register(t0, space2); that spelling is the
    // compiler's business, not this one's.
    CHECK(!has_code(lint("Texture2D albedo;", Language::HLSL, Stage::Fragment),
                    "SSSTUDIO-BINDING"));
}

TEST(lint_is_quiet_on_an_empty_or_plain_buffer) {
    CHECK(lint("", Language::HLSL, Stage::Fragment).empty());
    CHECK(lint("// nothing here\n", Language::GLSL, Stage::Fragment).empty());
}

TEST(every_lint_is_a_warning_never_an_error) {
    // The compiler owns errors. Nothing here is certain enough to fail a build.
    const std::string source = "uniform sampler2D s;\nvoid main() { c = lerp(a, b, t); }\n";
    const Diagnostics d = lint(source, Language::GLSL, Stage::Fragment);
    CHECK(!d.empty());
    CHECK(!has_errors(d));
}
