#include "ssstudio/completion.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <set>
#include <string_view>

#include "ssstudio/syntax.h"
#include "shader_words.h"

namespace ssstudio {
namespace {

using namespace words;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Prefix matches beat substring matches beat subsequence ("smst" -> smoothstep),
// which is what makes typing three letters feel like it understood you.
int match_score(const std::string& candidate, const std::string& prefix) {
    if (prefix.empty()) return 1;
    const std::string c = lower(candidate);
    const std::string p = lower(prefix);

    if (c.rfind(p, 0) == 0) return 1000 - static_cast<int>(c.size());
    const std::size_t at = c.find(p);
    if (at != std::string::npos) return 600 - static_cast<int>(at) - static_cast<int>(c.size());

    std::size_t index = 0;
    for (char ch : c) {
        if (index < p.size() && ch == p[index]) ++index;
    }
    if (index == p.size()) return 300 - static_cast<int>(c.size());
    return -1;
}

void add(std::vector<Completion>& out, const std::string& prefix, std::string label,
         std::string detail, std::string doc, CompletionKind kind, int bonus) {
    const int score = match_score(label, prefix);
    if (score < 0) return;
    Completion c;
    c.label = std::move(label);
    c.detail = std::move(detail);
    c.doc = std::move(doc);
    c.kind = kind;
    c.score = score + bonus;
    out.push_back(std::move(c));
}

bool is_identifier_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

/// The stage mask a Stage corresponds to, for filtering the semantics table.
unsigned stage_mask(Stage stage) {
    switch (stage) {
        case Stage::Vertex: return kSemVertex;
        case Stage::Fragment: return kSemFragment;
        case Stage::Compute: return kSemCompute;
    }
    return kSemAny;
}

/// Expands a statement template into the text that actually gets inserted:
/// every '\t' becomes one level of the editor's indent, and the single '\v'
/// marks where the caret goes and is removed. Returns -1 for the caret when the
/// template has no marker, which the editor reads as "end of the insertion".
std::string expand_statement(std::string_view body, const std::string& indent, int& caret) {
    std::string out;
    caret = -1;
    out.reserve(body.size() + 16);
    for (char c : body) {
        if (c == '\t') {
            out += indent;
        } else if (c == '\v') {
            caret = static_cast<int>(out.size());
        } else {
            out += c;
        }
    }
    return out;
}

/// True when `name` is a built-in this stage actually has. Anything the table
/// does not mention is available everywhere.
bool available_in_stage(std::string_view name, Stage stage) {
    for (const auto& restricted : kStageOnly) {
        if (name == restricted.name) return (restricted.stages & stage_mask(stage)) != 0;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Member access.
//
// The two things that can sit to the right of a '.' in a shader: the components
// of a vector, and the fields of a struct. Both are answered the same way - work
// out the type of what is left of the dot, then ask that type what it has.
// ---------------------------------------------------------------------------

/// How many components a vector type has, or 0 when the name is not one.
/// Both dialects' spellings, because a project can mix them and the type of a
/// uniform member arrives in HLSL spelling whatever the shader is written in.
int vector_width(std::string_view type) {
    static constexpr std::array<std::string_view, 11> kPrefixes = {
        "float", "half", "double", "int", "uint", "bool",
        "vec", "ivec", "uvec", "bvec", "dvec"};
    for (std::string_view prefix : kPrefixes) {
        if (type.size() != prefix.size() + 1 || type.rfind(prefix, 0) != 0) continue;
        const char digit = type.back();
        // "float2x2" is a matrix and never reaches here: the size check above
        // only admits exactly one trailing character.
        if (digit >= '2' && digit <= '4') return digit - '0';
    }
    // A bare scalar has one component, and ".x" on it is legal in HLSL.
    static constexpr std::array<std::string_view, 6> kScalars = {"float", "half", "double",
                                                                 "int", "uint", "bool"};
    for (std::string_view scalar : kScalars) {
        if (type == scalar) return 1;
    }
    return 0;
}

/// True when every character of `swizzle` names a component of a vector this
/// wide, in either of the two naming sets. Mixing the sets ("xg") is not legal
/// and is not accepted here either.
bool is_swizzle(std::string_view swizzle, int width) {
    if (swizzle.empty() || swizzle.size() > 4 || width <= 0) return false;
    constexpr std::string_view kPosition = "xyzw";
    constexpr std::string_view kColor = "rgba";
    for (std::string_view set : {kPosition, kColor}) {
        bool all = true;
        for (char c : swizzle) {
            const std::size_t at = set.find(c);
            if (at == std::string_view::npos || static_cast<int>(at) >= width) {
                all = false;
                break;
            }
        }
        if (all) return true;
    }
    return false;
}

/// The type a swizzle of `type` produces: the same scalar, widened or narrowed
/// to the number of components the swizzle names. "float4" swizzled by "xy" is
/// "float2"; by "x" it is "float".
std::string swizzle_type(const std::string& type, std::size_t components) {
    const std::size_t last = type.find_last_not_of("234");
    if (last == std::string::npos) return type;
    const std::string base = type.substr(0, last + 1);
    // GLSL's vector names carry no scalar of their own, so a single component of
    // a "vec3" is a "float" rather than a "vec".
    if (components <= 1) {
        if (base == "vec") return "float";
        if (base == "ivec") return "int";
        if (base == "uvec") return "uint";
        if (base == "bvec") return "bool";
        if (base == "dvec") return "double";
        return base;
    }
    return base + static_cast<char>('0' + components);
}

// ---------------------------------------------------------------------------
// The symbol scan.
//
// A pass over the same tokens the highlighter produces, looking for two shapes:
// a type-ish token followed by a name, and "struct" followed by a name. That is
// all a shader declaration is, once the lexer has already decided which words
// are types. Nothing here resolves anything - a declaration the scan does not
// recognise is left out rather than guessed at, because a wrong entry in a
// completion popup costs more than a missing one.
// ---------------------------------------------------------------------------

struct ScanToken {
    Token token;
    std::string_view text;
};

/// Tokens with the ones that never take part in a declaration removed, so the
/// patterns below can be written as "the next token" rather than "the next
/// token that matters".
std::vector<ScanToken> significant_tokens(const std::string& text,
                                          const std::vector<Token>& tokens) {
    std::vector<ScanToken> out;
    out.reserve(tokens.size());
    for (const Token& t : tokens) {
        if (t.kind == TokenKind::Plain || t.kind == TokenKind::Comment) continue;
        out.push_back({t, std::string_view(text).substr(t.offset, t.length)});
    }
    return out;
}

bool is_operator(const ScanToken& t, std::string_view spelling) {
    return t.token.kind == TokenKind::Operator && t.text == spelling;
}

/// Index of the token after a bracketed group that starts at `open`, or the end
/// when it never closes. Used to step over template arguments, parameter lists
/// and array bounds without caring what is inside them.
std::size_t skip_group(const std::vector<ScanToken>& t, std::size_t open, char opener,
                       char closer) {
    int depth = 0;
    for (std::size_t i = open; i < t.size(); ++i) {
        if (t[i].token.kind != TokenKind::Operator || t[i].text.size() != 1) continue;
        if (t[i].text[0] == opener) ++depth;
        else if (t[i].text[0] == closer && --depth == 0) return i + 1;
    }
    return t.size();
}

/// Source text from the start of `from` to the end of `to`, with every run of
/// whitespace squeezed to one space. What a function signature is made of.
std::string flatten(const std::string& text, const Token& from, const Token& to) {
    const std::size_t begin = from.offset;
    const std::size_t end = std::min<std::size_t>(to.offset + to.length, text.size());
    std::string out;
    bool space = false;
    for (std::size_t i = begin; i < end; ++i) {
        const char c = text[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

}  // namespace

std::string prefix_at(const std::string& text, std::size_t cursor) {
    cursor = std::min(cursor, text.size());
    std::size_t start = cursor;
    while (start > 0 && is_identifier_char(text[start - 1])) --start;
    return text.substr(start, cursor - start);
}

MemberAccess member_access_at(const std::string& text, std::size_t cursor) {
    MemberAccess out;
    cursor = std::min(cursor, text.size());

    std::size_t at = cursor;
    while (at > 0 && is_identifier_char(text[at - 1])) --at;
    out.prefix = text.substr(at, cursor - at);
    if (at == 0 || text[at - 1] != '.') return {};

    // Walk the chain of ".name" leftwards. Anything that is not a name stops it,
    // which includes the ')' of a call and the ']' of an index - those have a
    // type this cannot work out, so it declines rather than guesses.
    std::size_t dot = at - 1;
    while (true) {
        std::size_t name_end = dot;
        std::size_t name_begin = name_end;
        while (name_begin > 0 && is_identifier_char(text[name_begin - 1])) --name_begin;
        if (name_begin == name_end) return {};
        // "1.0" is a number, not a member access on the digit 1.
        if (std::isdigit(static_cast<unsigned char>(text[name_begin]))) return {};

        out.path.insert(out.path.begin(), text.substr(name_begin, name_end - name_begin));
        if (name_begin == 0 || text[name_begin - 1] != '.') break;
        dot = name_begin - 1;
    }
    return out;
}

bool semantic_position_at(const std::string& text, std::size_t cursor) {
    cursor = std::min(cursor, text.size());
    std::size_t at = cursor;
    while (at > 0 && is_identifier_char(text[at - 1])) --at;
    while (at > 0 && (text[at - 1] == ' ' || text[at - 1] == '\t')) --at;
    if (at == 0 || text[at - 1] != ':') return false;
    const std::size_t colon = at - 1;

    // "::" is a scope qualifier rather than a semantic marker.
    if (colon > 0 && text[colon - 1] == ':') return false;

    // The other two colons in a shader - the one closing a conditional and the
    // one after a case label - are told apart by what precedes them on their own
    // line. A semantic is never further from its ':' than that, so the line is
    // the whole context this needs.
    std::size_t line_begin = colon;
    while (line_begin > 0 && text[line_begin - 1] != '\n') --line_begin;
    if (text.find('?', line_begin) < colon) return false;

    const std::string_view line(text.data() + line_begin, colon - line_begin);
    const std::size_t first = line.find_first_not_of(" \t");
    if (first != std::string_view::npos) {
        const std::string_view head = line.substr(first);
        if (head.rfind("case", 0) == 0 || head.rfind("default", 0) == 0) return false;
    }
    return true;
}

DocumentSymbols scan_symbols(const std::string& text, Language language) {
    DocumentSymbols out;
    const std::vector<Token> tokens = tokenize(text, language);
    const std::vector<ScanToken> t = significant_tokens(text, tokens);
    if (t.empty()) return out;

    // Pass one: the names of the structs this shader declares, so that pass two
    // can read "VSOutput output;" as a declaration. The lexer has no way to know
    // those are types - it colors them as identifiers - which is exactly why the
    // scan needs two passes rather than one.
    std::set<std::string_view> struct_names;
    for (std::size_t i = 0; i + 1 < t.size(); ++i) {
        if (t[i].token.kind == TokenKind::Keyword && t[i].text == "struct" &&
            t[i + 1].token.kind == TokenKind::Identifier) {
            struct_names.insert(t[i + 1].text);
        }
    }

    const auto is_type = [&](const ScanToken& s) {
        return s.token.kind == TokenKind::Type ||
               (s.token.kind == TokenKind::Identifier && struct_names.count(s.text) > 0);
    };

    // One past the last caret position rather than one past the last byte: the
    // caret can sit at text.size(), which is where a file is usually being
    // added to, and a range that stopped short of it would make every
    // file-scope name invisible exactly there. A block's end is the offset just
    // after its '}' for the same reason - the caret immediately before the brace
    // is still inside the block.
    const std::size_t file_end = text.size() + 1;

    // Symbols whose scope has not been closed yet, innermost block last. A block
    // that never closes - the file being edited half way through a function -
    // leaves its symbols reaching the end of the text, which is the reading that
    // keeps completion working while the code is still being written.
    std::vector<std::vector<std::size_t>> open_blocks;
    // The struct each open block declares the fields of, empty for an ordinary
    // one. Parallel to open_blocks so that a declaration knows, without looking
    // anything up, whether it is a local or a member.
    std::vector<std::string> block_owner;
    // Set by "struct Name" and consumed by the '{' that follows it.
    std::string pending_owner;
    // Parameters are declared before the '{' they belong to, so they wait here
    // until that brace opens the block that owns them.
    std::vector<std::size_t> pending_params;

    const auto record = [&](std::string name, std::string type, SymbolKind kind,
                            std::size_t offset, bool file_scope) -> std::size_t {
        Symbol s;
        s.name = std::move(name);
        s.type = std::move(type);
        s.kind = kind;
        s.offset = offset;
        s.scope_begin = offset;
        s.scope_end = file_end;
        out.symbols.push_back(std::move(s));
        const std::size_t index = out.symbols.size() - 1;
        if (!file_scope && !open_blocks.empty()) open_blocks.back().push_back(index);
        return index;
    };

    for (std::size_t i = 0; i < t.size(); ++i) {
        const ScanToken& tok = t[i];

        if (is_operator(tok, "{")) {
            open_blocks.emplace_back();
            block_owner.push_back(pending_owner);
            pending_owner.clear();
            for (std::size_t index : pending_params) open_blocks.back().push_back(index);
            pending_params.clear();
            continue;
        }
        if (is_operator(tok, "}")) {
            if (!open_blocks.empty()) {
                const std::size_t end = tok.token.offset + tok.token.length;
                for (std::size_t index : open_blocks.back()) out.symbols[index].scope_end = end;
                open_blocks.pop_back();
                block_owner.pop_back();
            }
            continue;
        }

        // "#define NAME ..." - a name the shader introduces just as surely as a
        // variable does, and one the compiler will accept anywhere below it.
        if (tok.token.kind == TokenKind::Preprocessor && i + 1 < t.size() &&
            tok.text.rfind("#define", 0) == 0 &&
            t[i + 1].token.kind == TokenKind::Identifier) {
            record(std::string(t[i + 1].text), "#define", SymbolKind::Variable,
                   t[i + 1].token.offset, true);
            ++i;
            continue;
        }

        if (tok.token.kind == TokenKind::Keyword && tok.text == "struct" && i + 1 < t.size() &&
            t[i + 1].token.kind == TokenKind::Identifier) {
            record(std::string(t[i + 1].text), "struct", SymbolKind::Struct,
                   t[i + 1].token.offset, true);
            pending_owner = std::string(t[i + 1].text);
            ++i;
            continue;
        }

        if (!is_type(tok)) continue;

        // "Texture2D<float4> albedo" and "matrix<float, 4, 4> m": the arguments
        // are part of the type, so step over them to reach the name.
        std::size_t name_index = i + 1;
        if (name_index < t.size() && is_operator(t[name_index], "<")) {
            name_index = skip_group(t, name_index, '<', '>');
        }
        if (name_index >= t.size() || t[name_index].token.kind != TokenKind::Identifier) continue;

        const ScanToken& name = t[name_index];
        const std::size_t after = name_index + 1;
        const bool is_call = after < t.size() && is_operator(t[after], "(");

        if (is_call) {
            // A function only when this is its declaration rather than a call
            // through a value: "float4 tonemap(" declares, "tonemap(" calls, and
            // the difference is the type token we already required.
            const std::size_t close = skip_group(t, after, '(', ')');
            const std::size_t index =
                record(std::string(name.text), std::string(tok.text), SymbolKind::Function,
                       name.token.offset, true);
            out.symbols[index].signature =
                flatten(text, tok.token, t[close - 1 < t.size() ? close - 1 : t.size() - 1].token);

            // Its parameters, read with the same rule one level in. They belong
            // to the body, which the '{' below will open.
            for (std::size_t p = after + 1; p + 1 < close && p < t.size(); ++p) {
                if (!is_type(t[p])) continue;
                std::size_t pn = p + 1;
                if (pn < t.size() && is_operator(t[pn], "<")) pn = skip_group(t, pn, '<', '>');
                if (pn >= close || pn >= t.size() ||
                    t[pn].token.kind != TokenKind::Identifier) {
                    continue;
                }
                Symbol s;
                s.name = std::string(t[pn].text);
                s.type = std::string(t[p].text);
                s.kind = SymbolKind::Variable;
                s.offset = t[pn].token.offset;
                s.scope_begin = t[pn].token.offset;
                s.scope_end = file_end;
                out.symbols.push_back(std::move(s));
                pending_params.push_back(out.symbols.size() - 1);
                p = pn;
            }
            i = close - 1;
            continue;
        }

        // A variable, if what follows a name can follow a declaration. Anything
        // else - an operator, another name - means this was an expression that
        // happened to start with a word the lexer colors as a type.
        static constexpr std::array<std::string_view, 6> kDeclarationFollowers = {
            ";", "=", ",", "[", ":", ")"};
        const bool declares =
            after < t.size() &&
            std::any_of(kDeclarationFollowers.begin(), kDeclarationFollowers.end(),
                        [&](std::string_view s) { return is_operator(t[after], s); });
        if (!declares) continue;

        // Inside a struct body this is a field rather than a local. Fields are
        // never offered on their own - only after a '.' - so they are tagged
        // here rather than being told apart later by where they sit.
        const std::string owner = block_owner.empty() ? std::string() : block_owner.back();
        const std::size_t index =
            record(std::string(name.text), std::string(tok.text),
                   owner.empty() ? SymbolKind::Variable : SymbolKind::Field, name.token.offset,
                   open_blocks.empty());
        out.symbols[index].owner = owner;
        i = name_index;
    }

    return out;
}

// Whether a name the shader declares can be referred to from where the caret is.
// Both ends of the scan's [scope_begin, scope_end) range are enforced, which is
// what the two languages themselves require: a name has to be declared before it
// is used, and a local is gone once its block closes. A file-scope name -
// a function, a struct, a #define - has the whole text after it as its range, so
// the same test covers it without a special case.
//
// The cost of enforcing scope_begin is that a helper written at the bottom of a
// file is not offered at the top. That is the honest answer rather than a
// limitation: the compiler would reject the call, so offering it would only be
// a faster way to reach an error.
bool symbol_is_visible(const Symbol& symbol, std::size_t cursor) {
    return cursor >= symbol.scope_begin && cursor < symbol.scope_end;
}

std::string signature_at(const std::string& text, std::size_t cursor, Language language,
                         const DocumentSymbols* symbols) {
    cursor = std::min(cursor, text.size());

    // Walk back to the innermost unclosed '(' and read the identifier before it.
    int depth = 0;
    std::size_t i = cursor;
    while (i > 0) {
        --i;
        const char c = text[i];
        if (c == ')') ++depth;
        else if (c == '(') {
            if (depth == 0) break;
            --depth;
        }
    }
    if (i == 0 && (text.empty() || text[0] != '(')) return {};

    std::size_t end = i;
    while (end > 0 && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    std::size_t start = end;
    while (start > 0 && is_identifier_char(text[start - 1])) --start;
    if (start == end) return {};

    const std::string name = text.substr(start, end - start);
    // The shader's own functions first: a name it declares shadows a built-in of
    // the same spelling, and it is the one the reader is less likely to know.
    if (symbols) {
        for (const auto& symbol : symbols->symbols) {
            if (symbol.kind == SymbolKind::Function && symbol.name == name) {
                return symbol.signature;
            }
        }
    }
    for (const auto& intrinsic : kCommon) {
        if (name == intrinsic.name) return intrinsic.signature;
    }
    if (language == Language::HLSL) {
        for (const auto& intrinsic : kHlslOnly) {
            if (name == intrinsic.name) return intrinsic.signature;
        }
    } else {
        for (const auto& intrinsic : kGlslOnly) {
            if (name == intrinsic.name) return intrinsic.signature;
        }
    }
    return {};
}

namespace {

/// The declared type of a name at the caret, or empty when the scan never saw
/// it. The nearest declaration wins: a local shadows a file-scope name of the
/// same spelling, and the nearest one is the one declared latest before the
/// caret among those still in scope.
std::string root_type(const std::string& name, const CompletionContext& context) {
    if (context.symbols == nullptr) return {};
    const Symbol* best = nullptr;
    for (const auto& symbol : context.symbols->symbols) {
        if (symbol.name != name) continue;
        if (symbol.kind != SymbolKind::Variable && symbol.kind != SymbolKind::Field) continue;
        if (!symbol_is_visible(symbol, context.cursor)) continue;
        if (best == nullptr || symbol.scope_begin > best->scope_begin) best = &symbol;
    }
    return best ? best->type : std::string();
}

/// The type of `field` within `type`, following either a struct's declaration or
/// a vector's components. Empty when the type has no such member, which is what
/// closes the popup rather than showing a list for a name that cannot have one.
std::string member_type(const std::string& type, const std::string& field,
                        const CompletionContext& context) {
    const int width = vector_width(type);
    if (width > 0) {
        return is_swizzle(field, width) ? swizzle_type(type, field.size()) : std::string();
    }
    if (context.symbols == nullptr) return {};
    for (const auto& symbol : context.symbols->symbols) {
        if (symbol.kind == SymbolKind::Field && symbol.owner == type && symbol.name == field) {
            return symbol.type;
        }
    }
    return {};
}

/// The uniform block of this name, when the caret is completing one. HLSL leaves
/// a cbuffer's members unqualified, but GLSL's named blocks are reached through
/// the block, so the reflection has to answer for both.
const UniformBlock* find_block(const std::string& name, const CompletionContext& context) {
    if (context.reflection == nullptr) return nullptr;
    for (const auto& block : context.reflection->uniform_blocks) {
        if (block.name == name) return &block;
    }
    return nullptr;
}

/// The components worth listing for a vector this wide: every single one, then
/// the swizzles people actually write - the leading runs and the broadcasts.
/// The full set is 4 + 16 + 64 + 256 spellings for a float4, which is a list
/// nobody reads; typing any of the rest by hand is quicker than finding it.
void add_swizzles(std::vector<Completion>& out, const std::string& prefix,
                  const std::string& type, int width) {
    constexpr std::string_view kPosition = "xyzw";
    constexpr std::string_view kColor = "rgba";

    const auto offer = [&](const std::string& swizzle, int rank) {
        add(out, prefix, swizzle, swizzle_type(type, swizzle.size()),
            swizzle.size() == 1 ? "component" : "swizzle", CompletionKind::Field, rank);
    };

    // Single components first, positions before colors: a shader that is doing
    // geometry reads xyzw, and one that is doing color reads rgba, but the first
    // is the more common of the two by some distance.
    for (int i = 0; i < width; ++i) offer(std::string(1, kPosition[i]), 3000 - i);
    for (int i = 0; i < width; ++i) offer(std::string(1, kColor[i]), 2900 - i);

    // Leading runs: ".xy" off a float3, ".rgb" off a float4 - taking a prefix of
    // a vector is most of what swizzling is used for.
    for (int n = 2; n <= width; ++n) {
        offer(std::string(kPosition.substr(0, static_cast<std::size_t>(n))), 2800 - n);
        offer(std::string(kColor.substr(0, static_cast<std::size_t>(n))), 2700 - n);
    }
    // Broadcasts, which widen rather than narrow: a float scaled up to a float3.
    for (int n = 2; n <= 4; ++n) offer(std::string(static_cast<std::size_t>(n), 'x'), 2600 - n);
}

std::vector<Completion> complete_member(const std::string& prefix,
                                        const CompletionContext& context, std::size_t limit) {
    std::vector<Completion> out;
    const std::vector<std::string>& path = context.member_path;

    // A uniform block reached by name, which is the one member access that does
    // not go through the shader's own declarations.
    if (path.size() == 1) {
        if (const UniformBlock* block = find_block(path.front(), context)) {
            for (const auto& member : block->members) {
                add(out, prefix, member.name, member.c_type(), "uniform member",
                    CompletionKind::UniformMember, 3000);
            }
        }
    }

    std::string type = root_type(path.front(), context);
    for (std::size_t i = 1; i < path.size() && !type.empty(); ++i) {
        type = member_type(type, path[i], context);
    }

    if (!type.empty()) {
        const int width = vector_width(type);
        if (width > 0) {
            add_swizzles(out, prefix, type, width);
        } else if (context.symbols != nullptr) {
            for (const auto& symbol : context.symbols->symbols) {
                if (symbol.kind != SymbolKind::Field || symbol.owner != type) continue;
                add(out, prefix, symbol.name, symbol.type, type + " field",
                    CompletionKind::Field, 3000);
            }
        }
    }

    std::stable_sort(out.begin(), out.end(),
                     [](const Completion& a, const Completion& b) { return a.score > b.score; });
    if (out.size() > limit) out.resize(limit);
    return out;
}

}  // namespace

std::vector<Completion> complete(const std::string& prefix, const CompletionContext& context,
                                 std::size_t limit) {
    std::vector<Completion> out;
    const bool hlsl = context.language == Language::HLSL;

    // After a '.' the list is that value's members and nothing else, for the same
    // reason a semantic position is: nothing else can legally go there.
    if (!context.member_path.empty()) return complete_member(prefix, context, limit);

    // After a ':' only a semantic is legal, so the list becomes semantics and
    // nothing else. GLSL has none - it spells the same thing with layout() - so
    // there the popup simply does not open.
    if (context.semantic_position) {
        if (!hlsl) return {};
        const unsigned mask = stage_mask(context.stage);
        for (const auto& semantic : kHlslSemantics) {
            // Off-stage semantics are still offered, just below the ones that
            // belong here: a vertex shader's output struct is read by the
            // fragment stage, so "wrong stage" is a hint and not a rule.
            const int bonus = (semantic.stages & mask) != 0 ? 2000 : 1000;
            add(out, prefix, semantic.name, "semantic", semantic.doc, CompletionKind::Semantic,
                bonus);
        }
        std::stable_sort(out.begin(), out.end(),
                         [](const Completion& a, const Completion& b) { return a.score > b.score; });
        if (out.size() > limit) out.resize(limit);
        return out;
    }

    // What the shader itself declares, ranked above everything else. These are
    // the names with the shortest half-life - a local declared two lines up is
    // far more likely to be what is being typed than any built-in - and the ones
    // no reference page can supply.
    if (context.symbols) {
        for (const auto& symbol : context.symbols->symbols) {
            if (!symbol_is_visible(symbol, context.cursor)) continue;
            switch (symbol.kind) {
                case SymbolKind::Function:
                    add(out, prefix, symbol.name,
                        symbol.signature.empty() ? "function" : symbol.signature,
                        "declared by this shader", CompletionKind::Function, 4200);
                    break;
                case SymbolKind::Struct:
                    add(out, prefix, symbol.name, "struct", "declared by this shader",
                        CompletionKind::Type, 4100);
                    break;
                case SymbolKind::Variable:
                    add(out, prefix, symbol.name, symbol.type.empty() ? "variable" : symbol.type,
                        "declared by this shader", CompletionKind::Variable, 4400);
                    break;
                case SymbolKind::Field:
                    // Only ever reachable through the '.' that qualifies it.
                    break;
            }
        }
    }

    // Project-specific names rank above the language, because a resource name is
    // the thing you cannot look up in a reference.
    if (context.reflection) {
        for (const auto& resource : context.reflection->resources) {
            add(out, prefix, resource.name,
                std::string(to_string(resource.kind)) + " (space" +
                    std::to_string(resource.set) + ", " + std::to_string(resource.binding) + ")",
                "declared by this shader", CompletionKind::Resource, 4000);
        }
        for (const auto& block : context.reflection->uniform_blocks) {
            add(out, prefix, block.name, "cbuffer (space" + std::to_string(block.set) + ")",
                "uniform block", CompletionKind::Resource, 3800);
            for (const auto& member : block.members) {
                add(out, prefix, member.name, member.c_type() + " in " + block.name,
                    "uniform member", CompletionKind::UniformMember, 3600);
            }
        }
        for (const auto& input : context.reflection->vertex_inputs) {
            add(out, prefix, input.name,
                input.semantic.empty() ? "vertex input" : input.semantic, "vertex input",
                CompletionKind::Resource, 3400);
        }
    }

    for (const auto& macro : context.macros) {
        add(out, prefix, macro, "preview macro", "bound automatically by name in the I/O panel",
            CompletionKind::Macro, 3200);
    }

    // Only the built-ins this stage actually has: fwidth in a vertex shader and
    // barrier() in a fragment one are compile errors, so offering them would be
    // offering a mistake.
    const auto offer_intrinsic = [&](const words::Intrinsic& intrinsic, int bonus) {
        if (!available_in_stage(intrinsic.name, context.stage)) return;
        add(out, prefix, intrinsic.name, intrinsic.signature, intrinsic.doc,
            CompletionKind::Intrinsic, bonus);
    };

    for (const auto& intrinsic : kCommon) offer_intrinsic(intrinsic, 2000);
    // The language-specific spellings rank above the shared ones. They are the
    // set people get wrong moving between the two dialects, so on an empty
    // prefix they are worth more of the popup than another way to reach abs().
    if (hlsl) {
        for (const auto& intrinsic : kHlslOnly) offer_intrinsic(intrinsic, 2100);
        for (std::size_t i = 0; i < kCompletableTypes; ++i) {
            add(out, prefix, std::string(kHlslTypes[i]), "type", "", CompletionKind::Type, 1500);
        }
    } else {
        for (const auto& intrinsic : kGlslOnly) offer_intrinsic(intrinsic, 2100);
        for (std::size_t i = 0; i < kCompletableTypes; ++i) {
            add(out, prefix, std::string(kGlslTypes[i]), "type", "", CompletionKind::Type, 1500);
        }
    }

    // Control flow, offered as the whole statement rather than the bare word:
    // the braces and the semicolons of a switch are the part worth not typing.
    // Ranked below the intrinsics so an empty prefix still opens on names, but
    // above them the moment a prefix is typed, since "fo" means "for".
    for (const auto& statement : kStatements) {
        Completion c;
        c.label = statement.name;
        if (c.label == "elseif") c.label = "else if";
        const int score = match_score(c.label, prefix);
        if (score < 0) continue;
        c.insert = expand_statement(statement.body, context.indent, c.caret);
        c.detail = "statement";
        c.doc = statement.doc;
        c.kind = CompletionKind::Snippet;
        c.score = score + 1800;
        out.push_back(std::move(c));
    }

    // kKeywords only: the per-language keyword lists exist to be colored, and
    // "row_major" in a completion popup would be noise. The ones the statement
    // snippets already cover are skipped, so "for" appears once rather than as a
    // word and a statement that differ only in what they insert.
    for (std::string_view keyword : kKeywords) {
        const bool covered = std::any_of(kStatements.begin(), kStatements.end(),
                                         [&](const Statement& s) { return keyword == s.name; });
        if (covered) continue;
        if (!available_in_stage(keyword, context.stage)) continue;
        add(out, prefix, std::string(keyword), "keyword", "", CompletionKind::Keyword, 1000);
    }

    // Snippets that write the register spaces correctly, since that is the
    // single most common mistake with the SDL GPU binding model.
    if (context.stage == Stage::Fragment) {
        add(out, prefix, hlsl ? "Texture2D" : "sampler2D",
            hlsl ? "Texture2D<float4> name : register(t0, space2);"
                 : "layout(set = 2, binding = 0) uniform sampler2D name;",
            "texture declaration in the space SDL expects", CompletionKind::Snippet, 2600);
        add(out, prefix, hlsl ? "cbuffer" : "uniform",
            hlsl ? "cbuffer Name : register(b0, space3) { ... };"
                 : "layout(set = 3, binding = 0) uniform Name { ... };",
            "uniform block in the space SDL expects", CompletionKind::Snippet, 2600);
    } else if (context.stage == Stage::Vertex) {
        add(out, prefix, hlsl ? "cbuffer" : "uniform",
            hlsl ? "cbuffer Name : register(b0, space1) { ... };"
                 : "layout(set = 1, binding = 0) uniform Name { ... };",
            "uniform block in the space SDL expects", CompletionKind::Snippet, 2600);
    }

    std::stable_sort(out.begin(), out.end(),
                     [](const Completion& a, const Completion& b) { return a.score > b.score; });

    // Duplicate labels can arrive from a snippet and a type; keep the best.
    std::vector<Completion> unique;
    for (auto& candidate : out) {
        const bool seen = std::any_of(unique.begin(), unique.end(), [&](const Completion& c) {
            return c.label == candidate.label && c.detail == candidate.detail;
        });
        if (!seen) unique.push_back(std::move(candidate));
        if (unique.size() >= limit) break;
    }
    return unique;
}

}  // namespace ssstudio
