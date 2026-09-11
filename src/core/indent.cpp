#include "ssstudio/indent.h"

#include <algorithm>

namespace ssstudio {
namespace {

bool is_blank(char c) { return c == ' ' || c == '\t'; }

/// Offset of the start of the line `at` falls on.
std::size_t line_begin(const std::string& text, std::size_t at) {
    at = std::min(at, text.size());
    while (at > 0 && text[at - 1] != '\n') --at;
    return at;
}

/// Offset of the line break ending the line `at` falls on, or the end of the
/// text when it is the last line.
std::size_t line_end(const std::string& text, std::size_t at) {
    at = std::min(at, text.size());
    while (at < text.size() && text[at] != '\n') ++at;
    return at;
}

/// The run of spaces and tabs a line starts with, never reaching past `limit`.
/// The limit is what makes pressing Enter from inside a line's indentation copy
/// only the part that is actually to the left of the caret.
std::string leading_blanks(const std::string& text, std::size_t begin, std::size_t limit) {
    std::size_t at = begin;
    while (at < limit && at < text.size() && is_blank(text[at])) ++at;
    return text.substr(begin, at - begin);
}

/// The last character before `at` that is not whitespace, or '\0' when the line
/// has none. Stops at the line break: what the previous line ended with is not
/// this line's business.
char last_code_char(const std::string& text, std::size_t begin, std::size_t at) {
    for (std::size_t i = std::min(at, text.size()); i-- > begin;) {
        if (!is_blank(text[i])) return text[i];
    }
    return '\0';
}

/// The first character at or after `at` that is not whitespace, looking no
/// further than the end of the line.
char next_code_char(const std::string& text, std::size_t at) {
    for (std::size_t i = at; i < text.size() && text[i] != '\n'; ++i) {
        if (!is_blank(text[i])) return text[i];
    }
    return '\0';
}

/// How wide a run of leading whitespace is in columns, counting a tab as a jump
/// to the next stop. Only used on the leading run, so nothing else can be in it.
int column_of(const std::string& text, std::size_t begin, std::size_t at, int width) {
    const int stop = std::max(width, 1);
    int column = 0;
    for (std::size_t i = begin; i < at && i < text.size(); ++i) {
        column = text[i] == '\t' ? ((column / stop) + 1) * stop : column + 1;
    }
    return column;
}

/// Ordered [begin, end) with both ends inside the text.
void normalize(const std::string& text, std::size_t& begin, std::size_t& end) {
    if (begin > end) std::swap(begin, end);
    begin = std::min(begin, text.size());
    end = std::min(end, text.size());
}

/// True when the range covers more than one line, which is what turns Tab from
/// "insert here" into "move these lines".
bool spans_lines(const std::string& text, std::size_t begin, std::size_t end) {
    return text.find('\n', begin) < end;
}

/// Rewrites every line of [begin, end) through `transform`, as one edit that
/// replaces whole lines. Keeping it to whole lines is what lets the selection be
/// restored afterwards without tracking how much each line moved.
TextEdit rewrite_lines(const std::string& text, std::size_t begin, std::size_t end,
                       const auto& transform) {
    const std::size_t first = line_begin(text, begin);
    const std::size_t last = line_end(text, end);

    std::string out;
    bool any = false;
    std::size_t at = first;
    while (true) {
        const std::size_t stop = line_end(text, at);
        const std::string line = text.substr(at, stop - at);
        std::string changed = line;
        any |= transform(changed);
        out += changed;
        if (stop >= last) break;
        out += '\n';
        at = stop + 1;
    }
    if (!any) return {};

    TextEdit edit;
    edit.valid = true;
    edit.begin = first;
    edit.end = last;
    edit.replacement = std::move(out);
    edit.select_begin = first;
    edit.select_end = first + edit.replacement.size();
    return edit;
}

}  // namespace

TextEdit newline_edit(const std::string& text, std::size_t select_begin, std::size_t select_end,
                      const IndentStyle& style) {
    normalize(text, select_begin, select_end);

    // Enter with a selection replaces it, so the indentation is read from where
    // the selection starts - that is where the caret will be once it is gone.
    const std::size_t begin = line_begin(text, select_begin);
    const std::string base = leading_blanks(text, begin, select_begin);
    const std::string unit = style.unit();

    const bool opens_scope = last_code_char(text, begin, select_begin) == '{';
    const std::string inner = opens_scope ? base + unit : base;

    TextEdit edit;
    edit.valid = true;
    edit.begin = select_begin;
    edit.end = select_end;
    edit.replacement = "\n" + inner;
    edit.select_begin = edit.select_end = select_begin + edit.replacement.size();

    // "{|}" becomes three lines with the caret on the middle one. Only when the
    // brace really is the next thing: a '}' further down the file belongs to
    // whatever is between here and there.
    if (opens_scope && next_code_char(text, select_end) == '}') {
        edit.replacement += "\n" + base;
    }
    return edit;
}

TextEdit indent_edit(const std::string& text, std::size_t select_begin, std::size_t select_end,
                     const IndentStyle& style) {
    normalize(text, select_begin, select_end);
    const std::string unit = style.unit();

    if (spans_lines(text, select_begin, select_end)) {
        return rewrite_lines(text, select_begin, select_end, [&](std::string& line) {
            // A blank line gets nothing: trailing whitespace on an empty line is
            // noise, and the line will be indented properly when it is typed on.
            if (line.find_first_not_of(" \t") == std::string::npos) return false;
            line.insert(0, unit);
            return true;
        });
    }

    // To the next tab stop rather than a whole level, so that indentation lands
    // on the grid from wherever the caret happens to be.
    const std::size_t begin = line_begin(text, select_begin);
    std::string insertion = unit;
    if (style.spaces) {
        const int stop = std::max(style.width, 1);
        const int column = column_of(text, begin, select_begin, stop);
        insertion.assign(static_cast<std::size_t>(stop - (column % stop)), ' ');
    }

    TextEdit edit;
    edit.valid = true;
    edit.begin = select_begin;
    edit.end = select_end;
    edit.replacement = std::move(insertion);
    edit.select_begin = edit.select_end = select_begin + edit.replacement.size();
    return edit;
}

TextEdit unindent_edit(const std::string& text, std::size_t select_begin, std::size_t select_end,
                       const IndentStyle& style) {
    normalize(text, select_begin, select_end);
    const int stop = std::max(style.width, 1);

    // One level off the front of a line: a tab, or up to `width` spaces. Both
    // spellings are accepted whatever the setting says, because a file may have
    // been written with the other one and Shift+Tab should still work in it.
    const auto strip = [stop](std::string& line) {
        if (line.empty()) return false;
        if (line[0] == '\t') {
            line.erase(0, 1);
            return true;
        }
        std::size_t remove = 0;
        while (remove < static_cast<std::size_t>(stop) && remove < line.size() &&
               line[remove] == ' ') {
            ++remove;
        }
        if (remove == 0) return false;
        line.erase(0, remove);
        return true;
    };
    return rewrite_lines(text, select_begin, select_end, strip);
}

TextEdit backspace_indent_edit(const std::string& text, std::size_t select_begin,
                               std::size_t select_end, const IndentStyle& style) {
    normalize(text, select_begin, select_end);
    // A selection makes backspace a deletion of the selection, and a tab is one
    // character already: neither is this rule's business.
    if (select_begin != select_end || !style.spaces) return {};

    const std::size_t begin = line_begin(text, select_begin);
    if (select_begin == begin) return {};
    for (std::size_t i = begin; i < select_begin; ++i) {
        if (text[i] != ' ') return {};
    }

    const int stop = std::max(style.width, 1);
    const int column = static_cast<int>(select_begin - begin);
    // Back to the previous stop, and a whole level when already on one.
    int remove = column % stop;
    if (remove == 0) remove = stop;
    remove = std::min(remove, column);

    TextEdit edit;
    edit.valid = true;
    edit.begin = select_begin - static_cast<std::size_t>(remove);
    edit.end = select_begin;
    edit.select_begin = edit.select_end = edit.begin;
    return edit;
}

}  // namespace ssstudio
