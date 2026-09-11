#include "ssstudio/syntax.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>

#include "shader_words.h"

namespace ssstudio {
namespace {

bool is_identifier_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool is_identifier_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool is_hex_digit(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool contains(const auto& table, std::string_view word) {
    return std::find(table.begin(), table.end(), word) != table.end();
}

bool contains_named(const auto& table, std::string_view word) {
    return std::any_of(table.begin(), table.end(),
                       [&](const words::Intrinsic& i) { return word == i.name; });
}

// The lexer's whole state: which language, and whether the cursor is inside a
// GLSL layout(...) list, where a handful of otherwise ordinary words are
// qualifiers. Nothing else here needs to remember anything.
struct Lexer {
    std::string_view text;
    Language language = Language::HLSL;
    std::size_t at = 0;
    bool in_layout = false;
    int layout_depth = 0;
    std::vector<Token> out;

    void emit(std::size_t begin, std::size_t end, TokenKind kind) {
        if (end <= begin) return;
        out.push_back({static_cast<std::uint32_t>(begin),
                       static_cast<std::uint32_t>(end - begin), kind});
    }

    char peek(std::size_t ahead = 0) const {
        return at + ahead < text.size() ? text[at + ahead] : '\0';
    }

    TokenKind classify(std::string_view word) const {
        const bool hlsl = language == Language::HLSL;

        if (contains(words::kKeywords, word) || contains(words::kCommonKeywordsExtra, word)) {
            return TokenKind::Keyword;
        }
        if (contains_named(words::kCommon, word)) return TokenKind::Intrinsic;

        if (hlsl) {
            if (contains(words::kHlslKeywords, word)) return TokenKind::Keyword;
            if (contains(words::kHlslTypes, word)) return TokenKind::Type;
            if (contains_named(words::kHlslOnly, word)) return TokenKind::Intrinsic;
        } else {
            if (in_layout && contains(words::kGlslLayoutQualifiers, word)) {
                return TokenKind::Keyword;
            }
            if (contains(words::kGlslKeywords, word)) return TokenKind::Keyword;
            if (contains(words::kGlslTypes, word)) return TokenKind::Type;
            if (contains_named(words::kGlslOnly, word)) return TokenKind::Intrinsic;
        }
        return TokenKind::Identifier;
    }

    // A line break is always its own token, so a renderer can split by line
    // without inspecting token text. Returns false when the cursor is not on one.
    bool take_line_break() {
        if (peek() == '\r' && peek(1) == '\n') {
            emit(at, at + 2, TokenKind::Plain);
            at += 2;
            return true;
        }
        if (peek() == '\n' || peek() == '\r') {
            emit(at, at + 1, TokenKind::Plain);
            ++at;
            return true;
        }
        return false;
    }

    void take_line_comment() {
        const std::size_t begin = at;
        while (at < text.size() && text[at] != '\n' && text[at] != '\r') ++at;
        emit(begin, at, TokenKind::Comment);
    }

    // Split at every line break, so the "no token crosses a line" invariant
    // holds for the one construct in either language that can.
    void take_block_comment() {
        std::size_t begin = at;
        at += 2;  // the "/*" itself
        while (at < text.size()) {
            if (text[at] == '\n' || text[at] == '\r') {
                emit(begin, at, TokenKind::Comment);
                take_line_break();
                begin = at;
                continue;
            }
            if (text[at] == '*' && at + 1 < text.size() && text[at + 1] == '/') {
                at += 2;
                break;
            }
            ++at;
        }
        emit(begin, at, TokenKind::Comment);
    }

    // Unterminated literals stop at the end of the line rather than swallowing
    // the rest of the file: a stray quote is a typo, not a mode switch.
    void take_string(char quote) {
        const std::size_t begin = at;
        ++at;
        while (at < text.size() && text[at] != quote && text[at] != '\n' && text[at] != '\r') {
            if (text[at] == '\\' && at + 1 < text.size()) ++at;
            ++at;
        }
        if (at < text.size() && text[at] == quote) ++at;
        emit(begin, at, TokenKind::String);
    }

    void take_number() {
        const std::size_t begin = at;
        if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
            at += 2;
            while (at < text.size() && is_hex_digit(text[at])) ++at;
        } else {
            while (at < text.size() && is_digit(text[at])) ++at;
            if (at < text.size() && text[at] == '.') {
                ++at;
                while (at < text.size() && is_digit(text[at])) ++at;
            }
            if (at < text.size() && (text[at] == 'e' || text[at] == 'E')) {
                const std::size_t exponent = at;
                ++at;
                if (at < text.size() && (text[at] == '+' || text[at] == '-')) ++at;
                if (at < text.size() && is_digit(text[at])) {
                    while (at < text.size() && is_digit(text[at])) ++at;
                } else {
                    at = exponent;  // "1e" plus a letter is not an exponent
                }
            }
        }
        // Suffixes: f, u, l, h, lf and their uppercase spellings.
        while (at < text.size() && std::strchr("fFuUlLhH", text[at]) != nullptr) ++at;
        emit(begin, at, TokenKind::Number);
    }

    // The '#' and the directive word are the preprocessor token; the rest of the
    // line is lexed normally, so the name in "#define PI 3.14" still reads as a
    // name and the path in an #include still reads as a string.
    void take_preprocessor() {
        const std::size_t begin = at;
        ++at;
        while (at < text.size() && (text[at] == ' ' || text[at] == '\t')) ++at;
        while (at < text.size() && is_identifier_char(text[at])) ++at;
        emit(begin, at, TokenKind::Preprocessor);
    }

    // "#" only starts a directive when nothing but whitespace precedes it on the
    // line. Anywhere else it is the stringize/paste operator.
    bool at_line_start() const {
        for (std::size_t i = at; i-- > 0;) {
            const char c = text[i];
            if (c == '\n' || c == '\r') return true;
            if (c != ' ' && c != '\t') return false;
        }
        return true;
    }

    void run() {
        out.reserve(text.size() / 4 + 8);
        while (at < text.size()) {
            const char c = text[at];

            if (take_line_break()) continue;

            if (c == ' ' || c == '\t' || c == '\v' || c == '\f') {
                const std::size_t begin = at;
                while (at < text.size() &&
                       (text[at] == ' ' || text[at] == '\t' || text[at] == '\v' ||
                        text[at] == '\f')) {
                    ++at;
                }
                emit(begin, at, TokenKind::Plain);
                continue;
            }

            if (c == '/' && peek(1) == '/') {
                take_line_comment();
                continue;
            }
            if (c == '/' && peek(1) == '*') {
                take_block_comment();
                continue;
            }
            if (c == '"' || c == '\'') {
                take_string(c);
                continue;
            }
            if (c == '#' && at_line_start()) {
                take_preprocessor();
                continue;
            }
            if (is_digit(c) || (c == '.' && is_digit(peek(1)))) {
                take_number();
                continue;
            }
            if (is_identifier_start(c)) {
                const std::size_t begin = at;
                while (at < text.size() && is_identifier_char(text[at])) ++at;
                const std::string_view word = text.substr(begin, at - begin);
                emit(begin, at, classify(word));
                // "layout" opens the qualifier list at the '(' that follows.
                if (language == Language::GLSL && word == "layout") {
                    in_layout = true;
                    layout_depth = 0;
                }
                continue;
            }

            if (in_layout) {
                if (c == '(') {
                    ++layout_depth;
                } else if (c == ')') {
                    if (--layout_depth <= 0) in_layout = false;
                } else if (c == ';' || c == '{') {
                    in_layout = false;  // a malformed layout must not leak
                }
            }
            emit(at, at + 1, TokenKind::Operator);
            ++at;
        }
    }
};

constexpr std::array<std::string_view, kTokenKindCount> kKindNames = {
    "plain", "comment", "preprocessor", "keyword", "type",
    "intrinsic", "number", "string", "operator", "identifier",
};

}  // namespace

std::vector<Token> tokenize(std::string_view text, Language language) {
    Lexer lexer;
    lexer.text = text;
    lexer.language = language;
    lexer.run();
    return std::move(lexer.out);
}

std::string_view to_string(TokenKind k) {
    const auto index = static_cast<std::size_t>(k);
    return index < kKindNames.size() ? kKindNames[index] : kKindNames[0];
}

bool token_kind_from_string(std::string_view name, TokenKind& out) {
    for (std::size_t i = 0; i < kKindNames.size(); ++i) {
        if (kKindNames[i] == name) {
            out = static_cast<TokenKind>(i);
            return true;
        }
    }
    return false;
}

}  // namespace ssstudio
