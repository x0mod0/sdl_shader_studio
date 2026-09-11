#include "ssstudio/lint.h"

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <vector>

#include "ssstudio/syntax.h"
#include "shader_words.h"

namespace ssstudio {
namespace {

/// Line and column of a byte offset, both 1-based.
void position_of(std::string_view source, std::uint32_t offset, int& out_line, int& out_column) {
    out_line = 1;
    out_column = 1;
    for (std::uint32_t i = 0; i < offset && i < source.size(); ++i) {
        if (source[i] == '\n') {
            ++out_line;
            out_column = 1;
        } else {
            ++out_column;
        }
    }
}

Diagnostic warning(std::string message, std::string code, int line, int column) {
    Diagnostic d;
    d.severity = Severity::Warning;
    d.message = std::move(message);
    d.code = std::move(code);
    d.line = line;
    d.column = column;
    return d;
}

/// The spelling of `name` in the other dialect, or empty. Read out of the same
/// tables completion uses: those docs already say "GLSL calls this mix", which
/// is exactly the sentence this lint wants to put on the line.
std::string_view other_spelling(std::string_view name, Language language) {
    // A name only counts when it is unknown here and known there, which is what
    // makes this specific rather than a list of vocabulary trivia.
    const auto& theirs = language == Language::HLSL ? std::span<const words::Intrinsic>(
                                                          words::kGlslOnly)
                                                    : std::span<const words::Intrinsic>(
                                                          words::kHlslOnly);
    for (const auto& intrinsic : theirs) {
        if (name == intrinsic.name) return intrinsic.doc;
    }
    return {};
}

bool known_here(std::string_view name, Language language) {
    const auto named = [&](const auto& table) {
        return std::any_of(table.begin(), table.end(),
                           [&](const words::Intrinsic& i) { return name == i.name; });
    };
    if (named(words::kCommon)) return true;
    return language == Language::HLSL ? named(words::kHlslOnly) : named(words::kGlslOnly);
}

/// True when the token at `index` is being called, i.e. the next thing that is
/// not whitespace is '('. A variable that happens to be called "mix" is not a
/// cross-dialect slip.
bool is_call(std::string_view source, const std::vector<Token>& tokens, std::size_t index) {
    for (std::size_t i = index + 1; i < tokens.size(); ++i) {
        if (tokens[i].kind == TokenKind::Plain) continue;
        return source[tokens[i].offset] == '(';
    }
    return false;
}

// -- GLSL resource bindings -------------------------------------------------

/// The declaration keywords that need a layout(...) in front of them to land in
/// the set SDL GPU expects.
bool declares_a_resource(std::string_view word) {
    return word == "uniform" || word == "buffer";
}

}  // namespace

Diagnostics lint(std::string_view source, Language language, Stage stage) {
    (void)stage;
    Diagnostics out;
    const std::vector<Token> tokens = tokenize(source, language);

    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const Token& token = tokens[i];
        const std::string_view word = source.substr(token.offset, token.length);

        // 1. A built-in spelled the way the other dialect spells it. The
        //    compiler will say "undeclared identifier", which is true and
        //    unhelpful; the fix is the name, and we know it.
        if (token.kind == TokenKind::Identifier && is_call(source, tokens, i) &&
            !known_here(word, language)) {
            const std::string_view doc = other_spelling(word, language);
            if (!doc.empty()) {
                int line = 0;
                int column = 0;
                position_of(source, token.offset, line, column);
                out.push_back(warning(
                    std::string(word) + " is a " +
                        std::string(language == Language::HLSL ? "GLSL" : "HLSL") +
                        " name; " + std::string(doc),
                    "SSSTUDIO-DIALECT", line, column));
            }
        }

        // 2. A GLSL resource with no layout(...) in front of it. This compiles,
        //    and then binds to set 0 - which is not where SDL GPU looks - so the
        //    failure lands at draw time with nothing pointing back here.
        if (language == Language::GLSL && token.kind == TokenKind::Keyword &&
            declares_a_resource(word)) {
            // A uniform inside a layout(...) list, or one preceded by a layout
            // on the same statement, is already qualified.
            bool qualified = false;
            for (std::size_t j = i; j-- > 0;) {
                const std::string_view previous = source.substr(tokens[j].offset, tokens[j].length);
                if (previous == ";" || previous == "}") break;  // a previous statement
                if (previous == "layout") {
                    qualified = true;
                    break;
                }
            }
            // "uniform sampler2D" and friends declare a resource; a bare
            // "uniform float x" is a default-block member, which is a different
            // mistake and one the compiler does report.
            if (!qualified) {
                int line = 0;
                int column = 0;
                position_of(source, token.offset, line, column);
                out.push_back(warning(
                    "this " + std::string(word) +
                        " has no layout(set = , binding = ); it will bind to set 0, which is "
                        "not where SDL GPU looks, and the draw will fail rather than the compile",
                    "SSSTUDIO-BINDING", line, column));
            }
        }
    }
    return out;
}

}  // namespace ssstudio
