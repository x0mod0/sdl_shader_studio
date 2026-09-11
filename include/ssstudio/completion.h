// Editor autocomplete.
//
// Kept in the core rather than the editor so the candidate list can be tested
// without a UI, and so a future headless language server could reuse it.
#ifndef SSSTUDIO_COMPLETION_H
#define SSSTUDIO_COMPLETION_H

#include <cstddef>
#include <string>
#include <vector>

#include "ssstudio/graph.h"
#include "ssstudio/reflection.h"
#include "ssstudio/types.h"

namespace ssstudio {

enum class CompletionKind {
    Intrinsic,
    Type,
    Keyword,
    Resource,
    UniformMember,
    Macro,
    Snippet,
    Semantic,   // the name after ':' on a member, parameter or return type
    Variable,   // declared by the shader being edited
    Function,   // declared by the shader being edited
    Field,      // a struct member or a vector component, offered after a '.'
};

struct Completion {
    std::string label;      // what the popup shows, and what gets inserted by default
    std::string insert;     // inserted instead of the label when not empty
    std::string detail;     // signature or type, shown to the right
    std::string doc;        // one-line explanation
    CompletionKind kind = CompletionKind::Intrinsic;
    int score = 0;          // higher sorts first
    /// Where the caret should end up, as a byte offset into the inserted text.
    /// -1 means the end, which is what every candidate but a snippet wants.
    int caret = -1;

    /// The text an editor actually types in, which is `insert` when a candidate
    /// has one. Exists so callers cannot forget the distinction.
    const std::string& insert_text() const { return insert.empty() ? label : insert; }
};

/// What the shader being edited declares itself. Kinds are separate fields
/// rather than a tag because the three are offered at different ranks and a
/// caller usually wants one of them.
enum class SymbolKind { Variable, Function, Struct, Field };

struct Symbol {
    std::string name;
    std::string type;       // declared type; empty when the scanner could not tell
    std::string signature;  // functions only, e.g. "float4 tonemap(float3 c)"
    /// Fields only: the struct that declares this one. A field is never offered
    /// on its own - only after a '.' whose left side has that struct's type -
    /// which is what this makes possible.
    std::string owner;
    SymbolKind kind = SymbolKind::Variable;
    /// Byte offset of the declaring name in the text it was scanned from.
    std::size_t offset = 0;
    /// The half-open range of caret positions the name can be referred to from.
    /// For a local that is its enclosing block, ending just past the '}' so the
    /// caret immediately before the brace is still inside; for a file-scope name
    /// it runs to one past the end of the text, which is where a file is usually
    /// being added to. Both ends matter: a name is not in scope before it is
    /// declared, and a local is gone after its block closes.
    std::size_t scope_begin = 0;
    std::size_t scope_end = 0;
};

struct DocumentSymbols {
    std::vector<Symbol> symbols;
};

/// Everything the shader itself declares: locals, parameters, file-scope
/// variables, functions and structs. A lexical scan over the same token stream
/// the highlighter uses - it does not resolve types or parse expressions, so a
/// declaration it cannot recognise is simply not offered rather than guessed at.
DocumentSymbols scan_symbols(const std::string& text, Language language);

/// True when `symbol` can be referred to at `cursor`: at or after its
/// declaration, and before the end of the block that owns it.
bool symbol_is_visible(const Symbol& symbol, std::size_t cursor);

/// The dotted expression the caret is completing a member of. `path` holds the
/// names left of the last '.' - one entry for "uv.", two for "input.uv." - and
/// is empty when the caret is not after a '.' at all.
struct MemberAccess {
    std::vector<std::string> path;
    std::string prefix;  // the partial member already typed
};

/// Reads the member access at `cursor`, or an empty path when there is none.
/// A '.' that follows a digit is part of a number rather than an access, which
/// is what keeps "1.0" from opening a list of components.
MemberAccess member_access_at(const std::string& text, std::size_t cursor);

struct CompletionContext {
    Language language = Language::HLSL;
    Stage stage = Stage::Fragment;
    const Reflection* reflection = nullptr;   // from the last successful compile
    std::vector<std::string> macros;          // preview macros the project defines
    /// What the shader currently in the editor declares, and where the caret is
    /// in it. Both or neither: the scope test needs the offset to mean anything.
    const DocumentSymbols* symbols = nullptr;
    std::size_t cursor = 0;
    /// True when the caret sits after a ':', where only a semantic can go. The
    /// list becomes semantics only, because in that position nothing else is
    /// legal and offering intrinsics would just bury them. Only ever set for
    /// HLSL: GLSL has no semantics, and its colons all belong to something else.
    bool semantic_position = false;
    /// The names left of the '.' being completed, from member_access_at. When
    /// this is set the list is that value's members and nothing else.
    std::vector<std::string> member_path;
    /// One level of indentation, used to lay out the statement snippets. The
    /// editor's own setting, so an inserted `if` matches the code around it.
    std::string indent = "    ";
};

// Returns candidates for `prefix`, best first. An empty prefix returns the
// context-specific items (resources, uniform members, macros) before the
// language's intrinsics, because those are what the user cannot look up.
std::vector<Completion> complete(const std::string& prefix, const CompletionContext& context,
                                 std::size_t limit = 50);

// The word being typed at `cursor` in `text`; the editor passes this as prefix.
std::string prefix_at(const std::string& text, std::size_t cursor);

// True when the word at `cursor` follows a ':' - an HLSL semantic's position.
// A "::" does not count, so a scope-qualified name is left alone.
bool semantic_position_at(const std::string& text, std::size_t cursor);

// Signature help for the intrinsic being called at `cursor`, empty when none.
// `symbols`, when given, lets a function the shader declares answer too.
std::string signature_at(const std::string& text, std::size_t cursor, Language language,
                         const DocumentSymbols* symbols = nullptr);

}  // namespace ssstudio

#endif  // SSSTUDIO_COMPLETION_H
