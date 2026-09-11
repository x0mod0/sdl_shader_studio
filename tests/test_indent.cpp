#include "ssstudio/indent.h"
#include "test.h"

using namespace ssstudio;

namespace {

/// Applies an edit and returns the buffer with a '|' where the caret ended up,
/// so a test can be read as the before and after of a keystroke.
std::string applied(const std::string& text, const TextEdit& edit) {
    if (!edit.valid) return text;
    std::string out = text;
    out.replace(edit.begin, edit.end - edit.begin, edit.replacement);
    out.insert(edit.select_end, "|");
    return out;
}

/// The offset of '|' in a written-out buffer, with the marker removed. Lets the
/// caret be written where it is rather than counted out by hand.
std::size_t caret_of(std::string& text) {
    const std::size_t at = text.find('|');
    if (at == std::string::npos) return text.size();
    text.erase(at, 1);
    return at;
}

const IndentStyle kSpaces4{4, true};
const IndentStyle kSpaces2{2, true};
const IndentStyle kTabs{4, false};

}  // namespace

// --- Enter ---------------------------------------------------------------

TEST(a_new_line_starts_at_the_indentation_of_the_one_it_came_from) {
    std::string text = "void main() {\n    float a = 1.0;|\n}\n";
    const std::size_t caret = caret_of(text);
    CHECK_STREQ(applied(text, newline_edit(text, caret, caret, kSpaces4)),
                "void main() {\n    float a = 1.0;\n    |\n}\n");
}

TEST(an_opening_brace_adds_a_level) {
    std::string text = "void main() {|";
    const std::size_t caret = caret_of(text);
    CHECK_STREQ(applied(text, newline_edit(text, caret, caret, kSpaces4)),
                "void main() {\n    |");

    // And it nests, because the level is added to the line's own indentation
    // rather than counted from the margin.
    std::string nested = "    if (x) {|";
    const std::size_t at = caret_of(nested);
    CHECK_STREQ(applied(nested, newline_edit(nested, at, at, kSpaces4)),
                "    if (x) {\n        |");
}

TEST(a_brace_the_caret_sits_against_is_carried_down_a_line) {
    std::string text = "void main() {|}";
    const std::size_t caret = caret_of(text);
    CHECK_STREQ(applied(text, newline_edit(text, caret, caret, kSpaces4)),
                "void main() {\n    |\n}");
}

TEST(a_closing_brace_further_away_is_left_where_it_is) {
    // The '}' belongs to something else; only the one directly against the caret
    // is the one that was just opened.
    std::string text = "void main() {| float a; }";
    const std::size_t caret = caret_of(text);
    CHECK_STREQ(applied(text, newline_edit(text, caret, caret, kSpaces4)),
                "void main() {\n    | float a; }");
}

TEST(a_new_line_copies_only_the_indentation_left_of_the_caret) {
    // Enter pressed from the middle of a line's leading whitespace.
    std::string text = "        |    float a;";
    const std::size_t caret = caret_of(text);
    CHECK_STREQ(applied(text, newline_edit(text, caret, caret, kSpaces4)),
                "        \n        |    float a;");
}

TEST(enter_over_a_selection_replaces_it) {
    // The whole statement selected, semicolon included.
    std::string text = "    float a = 1.0;\n";
    CHECK_STREQ(applied(text, newline_edit(text, 4, 18, kSpaces4)), "    \n    |\n");
}

TEST(the_indent_style_decides_what_a_level_is) {
    std::string text = "if (x) {|";
    const std::size_t caret = caret_of(text);
    CHECK_STREQ(applied(text, newline_edit(text, caret, caret, kSpaces2)), "if (x) {\n  |");
    CHECK_STREQ(applied(text, newline_edit(text, caret, caret, kTabs)), "if (x) {\n\t|");
}

// --- Tab -----------------------------------------------------------------

TEST(tab_goes_to_the_next_stop_rather_than_a_whole_level) {
    std::string text = "  |float a;";
    const std::size_t caret = caret_of(text);
    // Two columns in, with a width of four: two more spaces, not four.
    CHECK_STREQ(applied(text, indent_edit(text, caret, caret, kSpaces4)), "    |float a;");

    std::string aligned = "    |float a;";
    const std::size_t at = caret_of(aligned);
    CHECK_STREQ(applied(aligned, indent_edit(aligned, at, at, kSpaces4)), "        |float a;");
}

TEST(tab_inserts_a_tab_when_that_is_the_style) {
    std::string text = "|float a;";
    const std::size_t caret = caret_of(text);
    CHECK_STREQ(applied(text, indent_edit(text, caret, caret, kTabs)), "\t|float a;");
}

TEST(tab_over_several_lines_moves_all_of_them) {
    const std::string text = "float a;\nfloat b;\nfloat c;\n";
    // A selection touching the first two lines.
    const TextEdit edit = indent_edit(text, 2, 12, kSpaces4);
    CHECK(edit.valid);
    std::string out = text;
    out.replace(edit.begin, edit.end - edit.begin, edit.replacement);
    CHECK_STREQ(out, "    float a;\n    float b;\nfloat c;\n");
    // The whole affected span stays selected, so the key can be pressed again.
    CHECK(edit.select_begin == std::size_t{0});
    CHECK(edit.select_end == std::size_t{0} + edit.replacement.size());
}

TEST(a_blank_line_in_a_selection_is_left_empty) {
    const std::string text = "float a;\n\nfloat b;\n";
    const TextEdit edit = indent_edit(text, 0, 18, kSpaces4);
    std::string out = text;
    out.replace(edit.begin, edit.end - edit.begin, edit.replacement);
    CHECK_STREQ(out, "    float a;\n\n    float b;\n");
}

// --- Shift+Tab -----------------------------------------------------------

TEST(shift_tab_takes_one_level_off_the_line) {
    std::string text = "        float |a;";
    const std::size_t caret = caret_of(text);
    const TextEdit edit = unindent_edit(text, caret, caret, kSpaces4);
    CHECK(edit.valid);
    std::string out = text;
    out.replace(edit.begin, edit.end - edit.begin, edit.replacement);
    CHECK_STREQ(out, "    float a;");
}

TEST(shift_tab_removes_a_tab_or_spaces_whatever_the_setting_says) {
    // A file written with tabs unindents under a spaces setting, and the other
    // way round: the key works on the file in front of you.
    const std::string tabbed = "\t\tfloat a;";
    TextEdit edit = unindent_edit(tabbed, 3, 3, kSpaces4);
    std::string out = tabbed;
    out.replace(edit.begin, edit.end - edit.begin, edit.replacement);
    CHECK_STREQ(out, "\tfloat a;");

    const std::string spaced = "    float a;";
    edit = unindent_edit(spaced, 6, 6, kTabs);
    out = spaced;
    out.replace(edit.begin, edit.end - edit.begin, edit.replacement);
    CHECK_STREQ(out, "float a;");
}

TEST(shift_tab_removes_a_partial_level_rather_than_nothing) {
    const std::string text = "  float a;";
    const TextEdit edit = unindent_edit(text, 4, 4, kSpaces4);
    std::string out = text;
    out.replace(edit.begin, edit.end - edit.begin, edit.replacement);
    CHECK_STREQ(out, "float a;");
}

TEST(shift_tab_on_a_line_with_no_indentation_declines) {
    const std::string text = "float a;";
    CHECK(!unindent_edit(text, 3, 3, kSpaces4).valid);
}

TEST(shift_tab_over_several_lines_moves_all_of_them) {
    const std::string text = "    float a;\n    float b;\nfloat c;\n";
    const TextEdit edit = unindent_edit(text, 0, 20, kSpaces4);
    std::string out = text;
    out.replace(edit.begin, edit.end - edit.begin, edit.replacement);
    CHECK_STREQ(out, "float a;\nfloat b;\nfloat c;\n");
}

// --- Backspace -----------------------------------------------------------

TEST(backspace_in_the_indentation_goes_back_a_whole_stop) {
    std::string text = "        |float a;";
    const std::size_t caret = caret_of(text);
    const TextEdit edit = backspace_indent_edit(text, caret, caret, kSpaces4);
    CHECK(edit.valid);
    CHECK_STREQ(applied(text, edit), "    |float a;");
}

TEST(backspace_off_the_grid_returns_to_it) {
    std::string text = "      |float a;";  // six columns, width four
    const std::size_t caret = caret_of(text);
    CHECK_STREQ(applied(text, backspace_indent_edit(text, caret, caret, kSpaces4)),
                "    |float a;");
}

TEST(backspace_is_left_alone_everywhere_it_is_not_indentation) {
    // After real code on the line.
    std::string text = "    float a;|";
    std::size_t caret = caret_of(text);
    CHECK(!backspace_indent_edit(text, caret, caret, kSpaces4).valid);

    // At the very start of a line, where there is nothing of ours to remove.
    std::string start = "\n|float a;";
    caret = caret_of(start);
    CHECK(!backspace_indent_edit(start, caret, caret, kSpaces4).valid);

    // With a selection, which backspace deletes.
    CHECK(!backspace_indent_edit("    float a;", 2, 6, kSpaces4).valid);

    // On a tab-indented line, where one press already removes one level.
    std::string tabbed = "\t\t|float a;";
    caret = caret_of(tabbed);
    CHECK(!backspace_indent_edit(tabbed, caret, caret, kSpaces4).valid);

    // And never when the style is tabs.
    std::string spaced = "        |float a;";
    caret = caret_of(spaced);
    CHECK(!backspace_indent_edit(spaced, caret, caret, kTabs).valid);
}

TEST(backspace_never_eats_past_the_start_of_the_line) {
    std::string text = "  |float a;";  // two columns, width four
    const std::size_t caret = caret_of(text);
    CHECK_STREQ(applied(text, backspace_indent_edit(text, caret, caret, kSpaces4)), "|float a;");
}
