// A small editor widget over ImGui's multiline input: line numbers in a gutter,
// diagnostic markers, syntax coloring, and the tab/indent behaviour from
// settings.
//
// The coloring is an overlay rather than a different widget. ImGui's input box
// draws its text in one color and offers no per-token hook, so the box is asked
// to draw its text fully transparent and the tokens are drawn on top of it, in
// the same place and with the same measurements. Everything the input box does
// - selection, undo, IME, the caret - keeps working, because it is still the
// thing doing it.
#ifndef SSSTUDIO_GUI_TEXT_EDITOR_H
#define SSSTUDIO_GUI_TEXT_EDITOR_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <imgui.h>

#include "ssstudio/completion.h"
#include "ssstudio/indent.h"
#include "ssstudio/reflection.h"
#include "ssstudio/syntax.h"
#include "ssstudio/types.h"

// Both live in imgui_internal.h, which only text_editor.cpp needs to include:
// the overlay's two arguments are opaque handles as far as callers care.
struct ImGuiWindow;
struct ImGuiInputTextState;
struct ImDrawList;
struct ImGuiInputTextCallbackData;

namespace ssstudio::gui {

struct EditorStyle {
    bool line_numbers = true;
    bool word_wrap = false;
    bool error_lens = true;
    int tab_width = 4;
    bool insert_spaces = true;
    /// Carry the current line's indentation onto the next one when Enter is
    /// pressed, and one level further in when the line opens a scope.
    bool auto_indent = true;
    float font_size = 15.0f;

    /// Which dialect's vocabulary to color against, and whether to color at all.
    bool syntax_highlight = true;
    Language language = Language::HLSL;
    /// Packed ImGui colors per TokenKind, already resolved from settings so the
    /// editor never has to know what a palette is.
    std::array<ImU32, kTokenKindCount> syntax_colors{};

    /// The severity ink for the squiggle and the error lens, packed the same
    /// way. Resolved from the theme by the panel for the same reason the syntax
    /// colors are: the editor draws, it does not decide what a theme is.
    ImU32 error_color = IM_COL32(232, 90, 90, 255);
    ImU32 warning_color = IM_COL32(230, 180, 80, 255);
    ImU32 info_color = IM_COL32(120, 170, 230, 255);

    /// Offer completions while typing. Ctrl+Space still opens the list when this
    /// is off, so turning it off means "stop interrupting" rather than "stop
    /// working".
    bool autocomplete = true;
};

/// What the editor needs about the document in front of it that only the panel
/// knows: which stage this shader is, what the last compile reflected, which
/// preview macros the project defines, and whether the text may be changed at
/// all. The editor supplies the rest - the caret, the prefix under it, and what
/// the buffer itself declares - because those change as it is typed and nothing
/// outside the widget can see them in time.
struct DocumentInputs {
    Stage stage = Stage::Fragment;
    const Reflection* reflection = nullptr;
    std::vector<std::string> macros;
    /// A shader a graph generates: the file is rewritten from the graph on every
    /// change, so an edit made here would be silently thrown away. The editor
    /// still selects, scrolls, colors and reports diagnostics - it just refuses
    /// to be typed into, which is the honest version of what was already true.
    bool read_only = false;
};

class TextEditor {
public:
    // Draws the editor filling the available region. Returns true when the text
    // changed this frame. `diagnostics` are drawn in the gutter and, if
    // error_lens is on, under the line they refer to.
    bool draw(const char* id, std::string& text, const Diagnostics& diagnostics,
              const EditorStyle& style, const DocumentInputs& document = {});

    // Line the caret was on at the last edit, 1-based; 0 when unknown.
    int cursor_line() const { return cursor_line_; }

    // Column of the caret on that line, 1-based, counted in characters rather
    // than bytes so a line with an accented comment still reads right.
    int cursor_column() const { return cursor_column_; }

    // Ask the next draw to scroll to a line (used when clicking a diagnostic).
    void request_scroll_to(int line) { scroll_to_line_ = line; }

    /// True while the completion popup is showing, which the panel needs so that
    /// its own shortcuts do not fire under it.
    bool completion_open() const { return completion_.open; }

private:
    /// Where the input box put its text this frame, and which of its lines can
    /// be seen. Worked out once and shared by the overlays, so the two can never
    /// disagree about where a line is.
    struct OverlayGeometry {
        ImVec2 origin;      /// screen position of the first character
        /// The visible interior of the text box, as two corners rather than an
        /// ImRect so this header stays clear of imgui_internal.h.
        ImVec2 clip_min;
        ImVec2 clip_max;
        int first_line = 0; /// inclusive, 0-based
        int last_line = 0;  /// inclusive, 0-based
    };

    /// The completion popup, and everything about it that has to survive between
    /// frames. Kept per editor rather than globally because two open documents
    /// each have their own caret and so each have their own list.
    struct CompletionState {
        bool open = false;
        /// Opened by Ctrl+Space rather than by typing. A manual list stays up on
        /// an empty prefix; an automatic one does not, because an empty prefix
        /// means the user just deleted their way out of a word.
        bool manual = false;
        std::vector<Completion> items;
        int selected = 0;
        /// The top row of the visible window into `items`, so a long list can be
        /// walked with the arrow keys without the popup growing to fit it.
        int scroll = 0;
        /// The word the list was built for, and where it sits in the buffer.
        std::string prefix;
        std::size_t cursor = 0;
        bool semantic = false;
        /// The dotted path the list is completing a member of, empty when it is
        /// an ordinary word. Part of the identity of a list, because typing a
        /// '.' turns one list into a completely different one.
        std::vector<std::string> member;
    };

    /// The one action a keystroke can ask of the popup. Recorded before the
    /// input box is drawn - which is the only place the keys can be taken away
    /// from it - and carried out inside the box's callback, where the buffer can
    /// be edited with the box's own undo stack intact.
    enum class CompletionAction { None, Accept, Dismiss, Up, Down };

    /// An edit to the buffer that a key asked for and the input box would
    /// otherwise have done differently. Recorded before the box is drawn, for
    /// the same reason the completion actions are: that is the only moment the
    /// key can be taken away from it.
    enum class EditAction { None, NewLine, Indent, Unindent, Backspace };

    /// Handed to the input box's callback as its user data. Everything the
    /// callback touches, and nothing else.
    struct CallbackPayload {
        std::string* text = nullptr;
        TextEditor* editor = nullptr;
    };

    static int input_callback(ImGuiInputTextCallbackData* data);

    /// Rebuilds `completion_.items` for the word at `cursor`, and decides
    /// whether the popup should be showing at all.
    void update_completion(const std::string& text, std::size_t cursor,
                           const EditorStyle& style, const DocumentInputs& inputs,
                           bool text_changed);

    /// Replaces the word under the caret with the selected candidate. Runs
    /// inside the input box's callback, so `data` is the buffer as the box holds
    /// it rather than the std::string the caller passed in.
    void apply_completion(ImGuiInputTextCallbackData* data);

    /// Works out which indentation key was pressed, taking it from the input box
    /// when one was. Called before the box is drawn; `active` says whether the
    /// box is the widget the keyboard is going to.
    void capture_indent_keys(bool active, const EditorStyle& style);

    /// Carries out the recorded edit inside the box's callback, so it lands on
    /// the box's own undo stack and one Ctrl+Z takes it back.
    void apply_edit(ImGuiInputTextCallbackData* data);

    /// Draws the popup at the caret, into the foreground so it is never clipped
    /// by the text box it hangs over. Deliberately keyboard-only: the overlay
    /// takes no input of its own, and a click that both picked a candidate and
    /// moved the caret would insert it in the wrong place.
    void draw_completion_popup(const ImVec2& caret, float line_height,
                               const EditorStyle& style) const;

    /// Where the caret is on screen, worked out from the text rather than from
    /// ImGui, so it is available on the frames the box is not the active widget.
    ImVec2 caret_position(const OverlayGeometry& geometry, const std::string& text,
                          std::size_t cursor, float line_height) const;

    /// Re-lexes only when the text or the language actually changed. The editor
    /// is redrawn every frame and shaders are edited a keystroke at a time, so
    /// the common case has to cost a comparison rather than a lex.
    void refresh_tokens(const std::string& text, Language language);

    /// Fills `out` for this frame. `box` is the child window ImGui renders the
    /// multiline input into and `state` is that input's state while it is the
    /// active widget, or null. False when there is nothing to draw over.
    bool overlay_geometry(const ImGuiWindow* box, const ImGuiInputTextState* state,
                          float line_height, OverlayGeometry& out) const;

    /// One past the last character of a line, excluding its line break.
    std::uint32_t line_end(int line) const;

    /// Draws the cached tokens over the input box's own (transparent) text.
    void draw_syntax(const OverlayGeometry& geometry, const EditorStyle& style,
                     ImDrawList* draw_list, float line_height) const;

    /// Underlines the span each diagnostic points at, in its severity's color.
    void draw_squiggles(const OverlayGeometry& geometry, const Diagnostics& diagnostics,
                        ImDrawList* draw_list, float line_height,
                        const EditorStyle& style) const;

    /// Tints every visible line a diagnostic is about and marks its left edge
    /// in the severity's colour. Drawn under the text.
    void draw_line_marks(const OverlayGeometry& geometry, const Diagnostics& diagnostics,
                         ImDrawList* draw_list, float line_height,
                         const EditorStyle& style) const;

    /// The worst diagnostic on each visible line, as a chip after the line's
    /// last character: the message where the eye already is, without a trip to
    /// the list below. Drawn over the text, and only where there is room.
    void draw_line_lenses(const OverlayGeometry& geometry, const Diagnostics& diagnostics,
                          ImDrawList* draw_list, float line_height,
                          const EditorStyle& style) const;

    /// The half-open byte range a diagnostic marks on `line` (0-based).
    void diagnostic_span(int line, int column, std::uint32_t& out_begin,
                         std::uint32_t& out_end) const;

    int cursor_line_ = 1;
    int cursor_column_ = 1;
    int scroll_to_line_ = 0;
    float last_scroll_y_ = 0.0f;

    CompletionState completion_;
    /// What the keys pressed before the input box asked for, consumed by the
    /// box's callback in the same frame.
    CompletionAction pending_action_ = CompletionAction::None;
    EditAction pending_edit_ = EditAction::None;
    /// The indentation rules the pending edit was decided under, kept so the
    /// callback applies the same ones the keystroke was read with.
    IndentStyle indent_style_;
    /// Caret position as of the previous frame, used to notice that the caret
    /// moved away from the word the list was built for.
    std::size_t last_cursor_ = 0;
    /// Set by the callback when it has just inserted a candidate, so the same
    /// frame's update does not immediately reopen the list on the word it wrote.
    bool just_completed_ = false;
    /// The indent one level of the statement snippets expands to, taken from the
    /// style each frame so a settings change is picked up without a reset.
    std::string indent_ = "    ";
    /// The buffer the cached symbols describe, held for the same reason
    /// tokenized_text_ is.
    std::string scanned_text_;
    Language scanned_language_ = Language::HLSL;
    DocumentSymbols symbols_;

    /// The text the cached tokens describe. Held by value because it is the only
    /// way to be certain the cache still matches a buffer that ImGui may have
    /// reallocated underneath us.
    std::string tokenized_text_;
    Language tokenized_language_ = Language::HLSL;
    std::vector<Token> tokens_;
    /// Byte offset of the start of each line. Tokens never cross a line break,
    /// so a search for a line's first token is a binary search over these, and
    /// drawing one visible line never walks the ones above it.
    std::vector<std::uint32_t> line_starts_;
};

}  // namespace ssstudio::gui

#endif  // SSSTUDIO_GUI_TEXT_EDITOR_H
