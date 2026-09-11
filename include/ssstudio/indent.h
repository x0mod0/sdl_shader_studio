// The editor's indentation rules.
//
// In the core rather than in the editor for the same reason completion and the
// lexer are: every rule here is a function of the text and the selection, so it
// can be tested by writing down a buffer and reading back what it became. The
// editor's part is only to notice the keystroke and to hand the result to
// ImGui's input box, which is the part a test could not check anyway.
#ifndef SSSTUDIO_INDENT_H
#define SSSTUDIO_INDENT_H

#include <cstddef>
#include <string>

namespace ssstudio {

/// What one level of indentation is. Both fields come from settings: a project
/// that indents with two spaces and one that indents with tabs are the same
/// editor with a different IndentStyle.
struct IndentStyle {
    int width = 4;        // columns per level, and the tab stop spacing
    bool spaces = true;   // false inserts a literal tab per level

    /// The text of one level.
    std::string unit() const {
        return spaces ? std::string(static_cast<std::size_t>(width < 1 ? 1 : width), ' ')
                      : std::string("\t");
    }
};

/// A replacement of the half-open byte range [begin, end) with `replacement`,
/// and what should be selected afterwards. A single caret is the two selection
/// offsets being equal, which is also how ImGui's input box says it.
///
/// `valid` false means the keystroke should be left to the input box: the rules
/// here decline rather than invent an edit, so an unhandled case behaves exactly
/// as it did before any of this existed.
struct TextEdit {
    bool valid = false;
    std::size_t begin = 0;
    std::size_t end = 0;
    std::string replacement;
    std::size_t select_begin = 0;
    std::size_t select_end = 0;
};

/// Enter: a line break followed by the indentation the new line should start
/// with. That is the current line's own leading whitespace, plus one level when
/// the line being left opens a scope, so the body of a block lands inside it.
///
/// When the caret sits directly before the '}' that closes the scope just
/// opened, the brace is carried down to a line of its own at the outer level and
/// the caret is left on the empty line between the two - which is what typing
/// '{', Enter is nearly always trying to produce.
TextEdit newline_edit(const std::string& text, std::size_t select_begin, std::size_t select_end,
                      const IndentStyle& style);

/// Tab: one level in. With a selection that covers more than one line every line
/// in it moves, and the selection is kept so the key can be pressed again.
/// Without one it is an insertion to the next tab stop rather than a fixed
/// number of spaces, so indentation stays on the grid from any column.
TextEdit indent_edit(const std::string& text, std::size_t select_begin, std::size_t select_end,
                     const IndentStyle& style);

/// Shift+Tab: one level out, for the caret's line or for every line of a
/// selection. Invalid when nothing in range has any indentation left to remove.
TextEdit unindent_edit(const std::string& text, std::size_t select_begin, std::size_t select_end,
                       const IndentStyle& style);

/// Backspace inside a line's leading indentation: back to the previous tab stop
/// in one press rather than one space at a time. Invalid everywhere else -
/// with a selection, on a line indented with tabs, or once there is anything but
/// whitespace between the line start and the caret - so ordinary backspace is
/// left completely alone.
TextEdit backspace_indent_edit(const std::string& text, std::size_t select_begin,
                               std::size_t select_end, const IndentStyle& style);

}  // namespace ssstudio

#endif  // SSSTUDIO_INDENT_H
