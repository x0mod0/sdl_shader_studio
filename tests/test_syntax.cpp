#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

#include "ssstudio/settings.h"
#include "ssstudio/syntax.h"
#include "test.h"

using namespace ssstudio;

namespace {

/// The kind the lexer gave the first token that spells `word` exactly. Tests ask
/// about words rather than offsets, which keeps them readable when the sample
/// shader around the word changes.
TokenKind kind_of(std::string_view source, std::string_view word,
                  Language language = Language::HLSL) {
    for (const auto& t : tokenize(source, language)) {
        if (source.substr(t.offset, t.length) == word) return t.kind;
    }
    return TokenKind::Plain;
}

int count_of(std::string_view source, TokenKind kind, Language language = Language::HLSL) {
    int n = 0;
    for (const auto& t : tokenize(source, language)) {
        if (t.kind == kind) ++n;
    }
    return n;
}

}  // namespace

TEST(tokens_tile_the_text_without_gaps_or_overlaps) {
    const std::string source =
        "// a comment\n"
        "float4 main(float2 uv : TEXCOORD0) : SV_Target {\n"
        "    return float4(saturate(uv.x), 0.5f, 0x1Fu, 1.0);\n"
        "}\n";

    const auto tokens = tokenize(source, Language::HLSL);
    CHECK(!tokens.empty());
    CHECK_EQ(tokens.front().offset, 0u);

    std::uint32_t expected = 0;
    for (const auto& t : tokens) {
        CHECK_EQ(t.offset, expected);
        CHECK(t.length > 0);
        expected = t.offset + t.length;
    }
    CHECK_EQ(static_cast<std::size_t>(expected), source.size());
}

TEST(no_token_crosses_a_line_break) {
    // The renderer draws one line at a time, so a block comment - the one
    // construct in either language that spans lines - has to arrive in pieces.
    const std::string source =
        "/* a comment\n"
        "   spanning three\n"
        "   lines */ float x;\n";

    for (const auto& t : tokenize(source, Language::HLSL)) {
        const std::string_view text(source.data() + t.offset, t.length);
        if (text == "\n" || text == "\r\n") continue;
        CHECK(text.find('\n') == std::string_view::npos);
        CHECK(text.find('\r') == std::string_view::npos);
    }
    CHECK_EQ(kind_of(source, "/* a comment") == TokenKind::Comment, true);
    CHECK_EQ(kind_of(source, "   lines */") == TokenKind::Comment, true);
}

TEST(keywords_types_and_intrinsics_are_told_apart) {
    const std::string source = "float4 color = saturate(uv);";
    CHECK(kind_of(source, "float4") == TokenKind::Type);
    CHECK(kind_of(source, "saturate") == TokenKind::Intrinsic);
    CHECK(kind_of(source, "color") == TokenKind::Identifier);
    CHECK(kind_of(source, "uv") == TokenKind::Identifier);
    CHECK(kind_of("if (x) return;", "if") == TokenKind::Keyword);
    CHECK(kind_of("if (x) return;", "return") == TokenKind::Keyword);
}

TEST(vocabulary_is_language_aware) {
    const std::string source = "a = lerp(b, c, t); d = mix(e, f, t);";
    CHECK(kind_of(source, "lerp", Language::HLSL) == TokenKind::Intrinsic);
    CHECK(kind_of(source, "mix", Language::HLSL) == TokenKind::Identifier);

    CHECK(kind_of(source, "mix", Language::GLSL) == TokenKind::Intrinsic);
    CHECK(kind_of(source, "lerp", Language::GLSL) == TokenKind::Identifier);

    CHECK(kind_of("vec4 c;", "vec4", Language::GLSL) == TokenKind::Type);
    CHECK(kind_of("vec4 c;", "vec4", Language::HLSL) == TokenKind::Identifier);
}

TEST(layout_qualifiers_are_keywords_only_inside_layout) {
    // "set" and "binding" are words a shader may well use for a variable, so
    // they only count as qualifiers where they actually are ones.
    const std::string inside = "layout(set = 2, binding = 0) uniform sampler2D tex;";
    CHECK(kind_of(inside, "set", Language::GLSL) == TokenKind::Keyword);
    CHECK(kind_of(inside, "binding", Language::GLSL) == TokenKind::Keyword);

    const std::string outside = "float set = 1.0; int binding = 2;";
    CHECK(kind_of(outside, "set", Language::GLSL) == TokenKind::Identifier);
    CHECK(kind_of(outside, "binding", Language::GLSL) == TokenKind::Identifier);

    // The list must close at its own paren rather than leaking down the file.
    const std::string after = "layout(set = 2) uniform Block { float offset; };\nfloat set;";
    const auto tokens = tokenize(after, Language::GLSL);
    int keyword_sets = 0;
    for (const auto& t : tokens) {
        if (std::string_view(after).substr(t.offset, t.length) == "set" &&
            t.kind == TokenKind::Keyword) {
            ++keyword_sets;
        }
    }
    CHECK_EQ(keyword_sets, 1);
}

TEST(numbers_strings_and_directives_are_recognized) {
    CHECK(kind_of("float x = 1.5f;", "1.5f") == TokenKind::Number);
    CHECK(kind_of("float x = .5;", ".5") == TokenKind::Number);
    CHECK(kind_of("uint m = 0xFFu;", "0xFFu") == TokenKind::Number);
    CHECK(kind_of("float x = 1e-3;", "1e-3") == TokenKind::Number);
    CHECK(kind_of("#include \"common.hlsli\"", "\"common.hlsli\"") == TokenKind::String);
    CHECK(kind_of("#include \"common.hlsli\"", "#include") == TokenKind::Preprocessor);
    // Indentation is its own token, so the directive starts at the '#'.
    CHECK(kind_of("  # define PI 3.14", "# define") == TokenKind::Preprocessor);

    // The name and the value after a #define are still an identifier and a
    // number, which is the point of not swallowing the whole line.
    CHECK(kind_of("#define PI 3.14", "PI") == TokenKind::Identifier);
    CHECK(kind_of("#define PI 3.14", "3.14") == TokenKind::Number);
}

TEST(an_unterminated_literal_stops_at_the_end_of_its_line) {
    // A stray quote is a typo. Letting it run to the end of the file would
    // recolor every line below it, which is the worst way to report one.
    const std::string source = "float a = 1.0; // \"oops\nfloat4 b;\n";
    CHECK(kind_of(source, "float4") == TokenKind::Type);

    const std::string quote = "\"unterminated\nfloat4 b;\n";
    CHECK(kind_of(quote, "float4") == TokenKind::Type);
}

TEST(an_empty_buffer_yields_no_tokens) {
    CHECK(tokenize("", Language::HLSL).empty());
    CHECK_EQ(count_of(" ", TokenKind::Plain), 1);
}

TEST(token_kind_names_round_trip) {
    for (std::size_t i = 0; i < kTokenKindCount; ++i) {
        const auto kind = static_cast<TokenKind>(i);
        TokenKind parsed = TokenKind::Plain;
        CHECK(token_kind_from_string(to_string(kind), parsed));
        CHECK(parsed == kind);
    }
    TokenKind unused = TokenKind::Plain;
    CHECK(!token_kind_from_string("not-a-kind", unused));
}

// ---------------------------------------------------------------------------
// The settings side: what the editor is actually handed to draw with.
// ---------------------------------------------------------------------------

TEST(a_palette_name_resolves_and_an_unknown_one_falls_back) {
    const SyntaxColors dark = syntax_palette("default");
    const SyntaxColors light = syntax_palette("light");
    CHECK(dark[TokenKind::Keyword] != light[TokenKind::Keyword]);

    // A typo in a hand-edited file must still open, and must not open black.
    const SyntaxColors typo = syntax_palette("drak");
    CHECK(typo.rgb == dark.rgb);

    CHECK(!syntax_palette_names().empty());
    for (const auto& name : syntax_palette_names()) {
        for (std::size_t i = 0; i < kTokenKindCount; ++i) {
            CHECK(syntax_palette(name).rgb[i] != 0u);
        }
    }
}

TEST(syntax_colors_survive_a_settings_round_trip) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "ssstudio_syntax_settings_test.toml";
    std::error_code ec;
    std::filesystem::remove(path, ec);

    AppSettings written;
    written.editor.syntax_highlight = false;
    written.editor.reload_theme_on_focus = true;
    written.editor.syntax_theme = "light";
    written.editor.syntax_colors = syntax_palette("light");
    // Edited by hand, so pinned: what makes a colour survive is the pin, not
    // the fact that a value is sitting in the struct.
    written.editor.syntax_colors[TokenKind::Comment] = 0x123456;
    written.editor.syntax_pinned[static_cast<std::size_t>(TokenKind::Comment)] = true;

    Diagnostics diags;
    CHECK(save_settings(path, written, diags));
    CHECK(!has_errors(diags));

    AppSettings read;
    Diagnostics read_diags;
    CHECK(load_settings(path, read, read_diags));
    CHECK(!has_errors(read_diags));

    CHECK_EQ(read.editor.syntax_highlight, false);
    CHECK_EQ(read.editor.reload_theme_on_focus, true);
    CHECK_STREQ(read.editor.syntax_theme, "light");
    CHECK_EQ(read.editor.syntax_colors[TokenKind::Comment], 0x123456u);
    CHECK(read.editor.syntax_pinned[static_cast<std::size_t>(TokenKind::Comment)]);
    CHECK(read.editor.syntax_colors.rgb == written.editor.syntax_colors.rgb);

    std::filesystem::remove(path, ec);
}

TEST(only_the_colors_a_user_pinned_are_written) {
    // Presence in the file is the pin, so the writer has to spell out the
    // deviations and nothing else. All ten would say all ten were chosen by
    // hand, and the next theme would then apply to none of them.
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "ssstudio_syntax_pins_test.toml";
    std::error_code ec;
    std::filesystem::remove(path, ec);

    AppSettings written;
    written.editor.syntax_colors[TokenKind::Keyword] = 0xfedcba;
    written.editor.syntax_pinned[static_cast<std::size_t>(TokenKind::Keyword)] = true;

    Diagnostics diags;
    CHECK(save_settings(path, written, diags));

    std::ifstream in(path);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text.find("keyword = \"#fedcba\"") != std::string::npos);
    CHECK(text.find("comment =") == std::string::npos);

    // And with nothing pinned there is no reason for the table to exist.
    AppSettings none;
    CHECK(save_settings(path, none, diags));
    std::ifstream again(path);
    std::string plain((std::istreambuf_iterator<char>(again)), std::istreambuf_iterator<char>());
    CHECK(plain.find("[editor.syntax]") == std::string::npos);

    std::filesystem::remove(path, ec);
}

TEST(a_file_from_before_theme_packs_keeps_only_its_deviations) {
    // An earlier build wrote all ten kinds every time. Reading presence as a
    // pin would pin all ten and freeze the reader out of every theme, so a file
    // with all ten is compared against the palette it names.
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "ssstudio_syntax_migrate_test.toml";
    const SyntaxColors light = syntax_palette("light");
    {
        std::ofstream out(path);
        out << "[editor]\nsyntax_theme = \"light\"\n\n[editor.syntax]\n";
        for (std::size_t i = 0; i < kTokenKindCount; ++i) {
            const auto kind = static_cast<TokenKind>(i);
            const std::uint32_t rgb =
                kind == TokenKind::Comment ? 0x123456u : light.rgb[i];
            char buffer[16];
            std::snprintf(buffer, sizeof(buffer), "#%06x", rgb);
            out << to_string(kind) << " = \"" << buffer << "\"\n";
        }
    }

    AppSettings read;
    Diagnostics diags;
    CHECK(load_settings(path, read, diags));
    for (std::size_t i = 0; i < kTokenKindCount; ++i) {
        const bool pinned = read.editor.syntax_pinned[i];
        CHECK_EQ(pinned, static_cast<TokenKind>(i) == TokenKind::Comment);
    }
    CHECK_EQ(read.editor.syntax_colors[TokenKind::Comment], 0x123456u);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(custom_migrates_to_the_palette_it_started_from) {
    // "custom" recorded that the name no longer described the colours, but not
    // what they had started from. The palette matching the most kinds is what
    // it meant in practice.
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "ssstudio_syntax_custom_test.toml";
    const SyntaxColors mono = syntax_palette("mono");
    {
        std::ofstream out(path);
        out << "[editor]\nsyntax_theme = \"custom\"\n\n[editor.syntax]\n";
        for (std::size_t i = 0; i < kTokenKindCount; ++i) {
            const auto kind = static_cast<TokenKind>(i);
            const std::uint32_t rgb = kind == TokenKind::String ? 0x00ff00u : mono.rgb[i];
            char buffer[16];
            std::snprintf(buffer, sizeof(buffer), "#%06x", rgb);
            out << to_string(kind) << " = \"" << buffer << "\"\n";
        }
    }

    AppSettings read;
    Diagnostics diags;
    CHECK(load_settings(path, read, diags));
    CHECK_STREQ(read.editor.syntax_theme, "mono");
    for (std::size_t i = 0; i < kTokenKindCount; ++i) {
        CHECK_EQ(read.editor.syntax_pinned[i], static_cast<TokenKind>(i) == TokenKind::String);
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(a_named_palette_supplies_the_colors_a_file_leaves_out) {
    // The point of keeping the theme name next to the table: a settings file
    // that mentions one color still gets the other nine from its palette.
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "ssstudio_syntax_partial_test.toml";
    {
        std::ofstream out(path);
        out << "[editor]\nsyntax_theme = \"light\"\n\n"
            << "[editor.syntax]\ncomment = \"#abcdef\"\n";
    }

    AppSettings read;
    Diagnostics diags;
    CHECK(load_settings(path, read, diags));
    CHECK_EQ(read.editor.syntax_colors[TokenKind::Comment], 0xabcdefu);
    CHECK_EQ(read.editor.syntax_colors[TokenKind::Keyword],
             syntax_palette("light")[TokenKind::Keyword]);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(the_examples_directory_is_found_by_walking_up) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "ssstudio_examples_walk_test";
    std::error_code ec;
    fs::remove_all(root, ec);

    // A layout shaped like a macOS bundle inside a build tree:
    //   root/examples                     <- the one that should win
    //   root/build/bin/App.app/Contents/MacOS
    const fs::path deep = root / "build" / "bin" / "App.app" / "Contents" / "MacOS";
    fs::create_directories(deep, ec);
    fs::create_directories(root / "examples", ec);

    CHECK(find_examples_dir(deep) == root / "examples");

    // The nearer of two wins, so a checkout inside another checkout cannot make
    // the app offer the outer one's examples.
    fs::create_directories(root / "build" / "examples", ec);
    CHECK(find_examples_dir(deep) == root / "build" / "examples");

    // Out of levels, and nothing there at all, both give an empty path rather
    // than a wrong one - the caller falls back to Documents.
    CHECK(find_examples_dir(deep, 2).empty());
    const fs::path bare = root / "bare" / "a" / "b";
    fs::create_directories(bare, ec);
    CHECK(find_examples_dir(bare, 2).empty());

    fs::remove_all(root, ec);
}
