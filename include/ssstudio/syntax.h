// Lexical highlighting for the shader languages the editor edits.
//
// Kept in the core rather than the editor for the same reason completion is:
// the token stream is something a test can look at directly, and the editor is
// only one of its consumers. It is a lexer and nothing more - it does not know
// what a declaration is, so it colors words, not meanings.
#ifndef SSSTUDIO_SYNTAX_H
#define SSSTUDIO_SYNTAX_H

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "ssstudio/types.h"

namespace ssstudio {

/// What the lexer decided a run of characters is. The editor maps each of these
/// to one color, and the settings file names them, so the set is deliberately
/// small: it is a palette a person has to choose ten colors for, not a grammar.
enum class TokenKind : std::uint8_t {
    Plain,          // whitespace, and anything the lexer has no opinion about
    Comment,
    Preprocessor,   // the '#' and the directive name after it
    Keyword,
    Type,
    Intrinsic,      // built-in functions: what the driver provides
    Number,
    String,
    Operator,       // operators and punctuation
    Identifier,     // every name the shader itself introduced
};

/// Kept next to the enum because SyntaxColors is an array of exactly this size;
/// adding a kind without widening this would not compile, which is the point.
inline constexpr std::size_t kTokenKindCount = 10;

/// One run of source text and what it is. Offsets into the string that was
/// tokenized rather than copies, so a whole shader costs one allocation.
struct Token {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
    TokenKind kind = TokenKind::Plain;
};

/// Two invariants the editor's renderer depends on, so it can draw one line
/// without lexing from the top of the file:
///
///   - the tokens tile the text: the first starts at 0, each one begins where
///     the previous ended, and the last ends at text.size();
///   - no token contains a line break. A '\n' (or "\r\n") is its own Plain
///     token, and a block comment is emitted as one token per line.
std::vector<Token> tokenize(std::string_view text, Language language);

/// Stable spellings, used as TOML keys and as labels in Settings. Renaming one
/// silently retires whatever color a user had saved under the old name.
std::string_view to_string(TokenKind k);
bool token_kind_from_string(std::string_view name, TokenKind& out);

}  // namespace ssstudio

#endif  // SSSTUDIO_SYNTAX_H
