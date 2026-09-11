#include "text_editor.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstddef>
#include <iterator>
#include <map>
#include <string>

#include <imgui.h>
#include <imgui_internal.h>  // GetInputTextState, FindWindowByName

namespace ssstudio::gui {
namespace {

int count_lines(const std::string& text) {
    int lines = 1;
    for (char c : text) {
        if (c == '\n') ++lines;
    }
    return lines;
}

/// The child window ImGui renders a multiline input into.
///
/// BeginChildEx names it "<parent window name>/<label>_<item id in hex>", so it
/// can be found by name - but only by composing that name exactly, which is why
/// this is a function rather than a literal. Must be called with the same ID
/// stack the input itself is drawn under, since that is what the item id is
/// derived from.
///
/// Returns null on the frame an input is drawn for the very first time: the
/// window does not exist until Begin() has run once. Callers use that to fall
/// back to ImGui's own rendering rather than drawing nothing.
ImGuiWindow* input_child_window(const char* label) {
    ImGuiWindow* parent = ImGui::GetCurrentWindow();
    if (parent == nullptr) return nullptr;
    char name[256];
    ImFormatString(name, IM_ARRAYSIZE(name), "%s/%s_%08X", parent->Name, label,
                   parent->GetID(label));
    return ImGui::FindWindowByName(name);
}

/// Where ImGui's input box put the first character of the text, this frame, in
/// screen space. The child window it renders into starts its layout cursor at
/// DC.CursorStartPos and InputTextEx offsets that by one FramePadding before it
/// draws anything, so those two are the whole answer.
///
/// The one wrinkle is the frame in which the box scrolls itself to follow the
/// caret: it moves the text without waiting for the next Begin(), and
/// CursorStartPos still holds the position from before the move. The box records
/// the scroll it started the frame with in its own state, so the difference
/// between that and where it ended up is exactly the correction, and it is zero
/// on every other frame - including ordinary wheel scrolling, which happens
/// before Begin() rather than during the widget.
ImVec2 text_origin(const ImGuiWindow* box, const ImGuiInputTextState* state) {
    const ImVec2 padding = ImGui::GetStyle().FramePadding;
    ImVec2 origin(box->DC.CursorStartPos.x + padding.x, box->DC.CursorStartPos.y + padding.y);
    if (state) origin.y += state->Scroll.y - box->Scroll.y;
    return origin;
}

/// The color a candidate's label is drawn in. Deliberately reuses the syntax
/// palette rather than inventing a second one: a name is the same color in the
/// popup as it will be once it is in the file, which is what makes the list
/// scannable without a legend.
ImU32 completion_color(CompletionKind kind, const EditorStyle& style) {
    const auto color_for = [&](TokenKind token) {
        return style.syntax_colors[static_cast<std::size_t>(token)];
    };
    switch (kind) {
        case CompletionKind::Intrinsic: return color_for(TokenKind::Intrinsic);
        case CompletionKind::Type: return color_for(TokenKind::Type);
        case CompletionKind::Keyword:
        case CompletionKind::Snippet: return color_for(TokenKind::Keyword);
        case CompletionKind::Semantic: return color_for(TokenKind::Preprocessor);
        case CompletionKind::Resource:
        case CompletionKind::UniformMember:
        case CompletionKind::Macro:
        case CompletionKind::Variable:
        case CompletionKind::Function: return color_for(TokenKind::Identifier);
        case CompletionKind::Field: return color_for(TokenKind::Identifier);
    }
    return color_for(TokenKind::Identifier);
}

/// A one-letter tag in the popup's left margin. Cheaper to read than a word and
/// cheaper to draw than an icon, and it is the only thing distinguishing a
/// uniform member from a local of the same name.
const char* completion_tag(CompletionKind kind) {
    switch (kind) {
        case CompletionKind::Intrinsic: return "f";
        case CompletionKind::Type: return "T";
        case CompletionKind::Keyword: return "k";
        case CompletionKind::Resource: return "R";
        case CompletionKind::UniformMember: return "u";
        case CompletionKind::Macro: return "m";
        case CompletionKind::Snippet: return "s";
        case CompletionKind::Semantic: return ":";
        case CompletionKind::Variable: return "v";
        case CompletionKind::Function: return "F";
        case CompletionKind::Field: return ".";
    }
    return " ";
}

bool is_word_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

ImU32 severity_color(Severity s, const EditorStyle& style) {
    switch (s) {
        case Severity::Error: return style.error_color;
        case Severity::Warning: return style.warning_color;
        case Severity::Info: return style.info_color;
    }
    return style.info_color;
}

}  // namespace

// The id ImGui will give the multiline input, worked out the same way it does:
// needed before the widget is drawn, to ask whether it is the active one and to
// take keys away from it.
namespace {
ImGuiID input_item_id(const char* label) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    return window ? window->GetID(label) : 0;
}
}  // namespace

void TextEditor::capture_indent_keys(bool active, const EditorStyle& style) {
    pending_edit_ = EditAction::None;
    indent_style_.width = std::clamp(style.tab_width, 1, 16);
    indent_style_.spaces = style.insert_spaces;
    // Only while the caret is actually in this box. Tab outside it still moves
    // focus, which is what Tab means everywhere else in the window.
    if (!active) return;

    // Every one of these keys means something else with a modifier held - word
    // delete, delete to line start, cycling windows - and all of that is the
    // input box's or ImGui's to do. Shift is the exception: Shift+Tab is the
    // unindent, and Shift+Enter is still a newline.
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl || io.KeyAlt || io.KeySuper) return;

    const ImGuiID owner = ImGui::GetID("##indent_keys");
    const auto take = [owner](ImGuiKey key) {
        ImGui::SetKeyOwner(key, owner, ImGuiInputFlags_LockThisFrame);
        return ImGui::IsKeyPressed(key, ImGuiInputFlags_Repeat, owner);
    };

    // Tab first: ImGui's own handling would otherwise move focus out of the
    // editor entirely, which is the one outcome nobody means by pressing it.
    if (take(ImGuiKey_Tab)) {
        pending_edit_ = io.KeyShift ? EditAction::Unindent : EditAction::Indent;
        return;
    }
    if (style.auto_indent) {
        if (take(ImGuiKey_Enter) || take(ImGuiKey_KeypadEnter)) {
            pending_edit_ = EditAction::NewLine;
            return;
        }
        // Backspace is only ours when the caret really is in a line's leading
        // spaces. The rule is checked again in the callback against the box's
        // own buffer, and declining there falls back to deleting one character -
        // which is what the box would have done with the key anyway.
        if (take(ImGuiKey_Backspace)) pending_edit_ = EditAction::Backspace;
    }
}

void TextEditor::apply_edit(ImGuiInputTextCallbackData* data) {
    const std::string buffer(data->Buf, static_cast<std::size_t>(data->BufTextLen));
    const auto begin = static_cast<std::size_t>(std::min(data->SelectionStart, data->SelectionEnd));
    const auto end = static_cast<std::size_t>(std::max(data->SelectionStart, data->SelectionEnd));
    // ImGui reports the two selection offsets as equal to the caret when nothing
    // is selected, but only while a selection has ever existed; before that they
    // are both zero, and the caret is the only thing that says where we are.
    const auto caret = static_cast<std::size_t>(data->CursorPos);
    const bool has_selection = data->SelectionStart != data->SelectionEnd;
    const std::size_t from = has_selection ? begin : caret;
    const std::size_t to = has_selection ? end : caret;

    TextEdit edit;
    switch (pending_edit_) {
        case EditAction::NewLine: edit = newline_edit(buffer, from, to, indent_style_); break;
        case EditAction::Indent: edit = indent_edit(buffer, from, to, indent_style_); break;
        case EditAction::Unindent: edit = unindent_edit(buffer, from, to, indent_style_); break;
        case EditAction::Backspace:
            edit = backspace_indent_edit(buffer, from, to, indent_style_);
            break;
        case EditAction::None: return;
    }
    // Nothing to do - Shift+Tab on an unindented line, backspace anywhere that
    // is not indentation. The key was taken from the box, so the fallback has to
    // be done here rather than left to it.
    if (!edit.valid) {
        if (pending_edit_ == EditAction::Backspace) {
            if (has_selection) {
                data->DeleteChars(static_cast<int>(begin), static_cast<int>(end - begin));
                data->CursorPos = static_cast<int>(begin);
            } else if (caret > 0) {
                data->DeleteChars(static_cast<int>(caret) - 1, 1);
                data->CursorPos = static_cast<int>(caret) - 1;
            }
            data->SelectionStart = data->SelectionEnd = data->CursorPos;
        }
        return;
    }

    if (edit.end > edit.begin) {
        data->DeleteChars(static_cast<int>(edit.begin),
                          static_cast<int>(edit.end - edit.begin));
    }
    if (!edit.replacement.empty()) {
        data->InsertChars(static_cast<int>(edit.begin), edit.replacement.c_str());
    }
    data->SelectionStart = static_cast<int>(edit.select_begin);
    data->SelectionEnd = static_cast<int>(edit.select_end);
    data->CursorPos = static_cast<int>(edit.select_end);
}

// The input box's callback, handling all four of the events the editor asks for.
// It is one function because ImGui takes one: the events are told apart by
// EventFlag, and only the resize event can arrive without a completion popup
// being involved.
int TextEditor::input_callback(ImGuiInputTextCallbackData* data) {
    auto* payload = static_cast<CallbackPayload*>(data->UserData);
    if (payload == nullptr) return 0;

    // ImGui hands us a fixed buffer; this event lets the std::string grow.
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        payload->text->resize(static_cast<std::size_t>(data->BufTextLen));
        data->Buf = payload->text->data();
        return 0;
    }

    TextEditor* editor = payload->editor;
    if (editor == nullptr) return 0;

    if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
        // The edits both go through the box's buffer rather than the caller's
        // string, so they land on the box's undo stack and one Ctrl+Z takes
        // either of them back.
        if (editor->pending_action_ == CompletionAction::Accept) {
            editor->apply_completion(data);
        } else if (editor->pending_edit_ != EditAction::None) {
            editor->apply_edit(data);
        }
        editor->pending_action_ = CompletionAction::None;
        editor->pending_edit_ = EditAction::None;
        editor->completion_.cursor = static_cast<std::size_t>(data->CursorPos);
    }
    return 0;
}

void TextEditor::apply_completion(ImGuiInputTextCallbackData* data) {
    if (!completion_.open || completion_.items.empty()) return;
    const int index =
        std::clamp(completion_.selected, 0, static_cast<int>(completion_.items.size()) - 1);
    const Completion& candidate = completion_.items[static_cast<std::size_t>(index)];
    const std::string& insert = candidate.insert_text();

    // The word to replace is worked out from the buffer the box is holding right
    // now rather than from the copy the caller passed in, so the edit lands where
    // the caret actually is even if the two have drifted apart this frame.
    int begin = data->CursorPos;
    while (begin > 0 && is_word_char(data->Buf[begin - 1])) --begin;
    if (data->CursorPos > begin) data->DeleteChars(begin, data->CursorPos - begin);
    data->InsertChars(begin, insert.c_str());

    const int caret = candidate.caret < 0 ? static_cast<int>(insert.size()) : candidate.caret;
    data->CursorPos = begin + caret;
    data->SelectionStart = data->SelectionEnd = data->CursorPos;

    completion_.open = false;
    completion_.items.clear();
    just_completed_ = true;
}

void TextEditor::update_completion(const std::string& text, std::size_t cursor,
                                   const EditorStyle& style, const DocumentInputs& inputs,
                                   bool text_changed) {
    // Ctrl+Space opens the list wherever the caret is, and is the way in when
    // completing-as-you-type is switched off.
    const bool requested = ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Space);
    if (requested) completion_.manual = true;

    if (inputs.read_only) {
        completion_.open = false;
        return;
    }

    const std::string prefix = prefix_at(text, cursor);
    // Only HLSL has semantics; a ':' in GLSL always belongs to something else, so
    // there the position is never a semantic one and the ordinary list stands.
    const bool semantic =
        style.language == Language::HLSL && semantic_position_at(text, cursor);
    const MemberAccess member = member_access_at(text, cursor);
    // Completing in the middle of a word would insert the candidate and leave the
    // rest of the old name trailing after it, so the list only opens at the end
    // of one.
    const bool at_word_end = cursor >= text.size() || !is_word_char(text[cursor]);

    if (!requested) {
        if (!completion_.open) {
            // Two characters before offering anything: one is not enough to mean
            // a name, and a popup after every single letter is what makes
            // autocomplete something people turn off.
            // A member access opens on the '.' itself: the components of a
            // vector are the one list nobody wants to type two letters into
            // first, since most of them are one letter long.
            const std::size_t needed = (semantic || !member.path.empty()) ? 0 : 2;
            const bool worth_opening = style.autocomplete && text_changed && at_word_end &&
                                       prefix.size() >= needed;
            if (!worth_opening) return;
        } else if (!at_word_end ||
                   (prefix.empty() && !completion_.manual && member.path.empty()) ||
                   (!text_changed && cursor != last_cursor_)) {
            // Open already: it closes when the caret leaves the word, when the
            // word is deleted, or when the caret is moved without typing - all
            // three mean the list is about something the user is no longer on.
            completion_.open = false;
            completion_.manual = false;
            return;
        }
    }

    if (scanned_text_ != text || scanned_language_ != style.language) {
        scanned_text_ = text;
        scanned_language_ = style.language;
        symbols_ = scan_symbols(scanned_text_, style.language);
    }

    CompletionContext context;
    context.language = style.language;
    context.stage = inputs.stage;
    context.reflection = inputs.reflection;
    context.macros = inputs.macros;
    context.symbols = &symbols_;
    context.cursor = cursor;
    context.semantic_position = semantic;
    context.member_path = member.path;
    context.indent = indent_;

    std::vector<Completion> items = complete(prefix, context, 40);
    if (items.empty()) {
        completion_.open = false;
        completion_.manual = false;
        return;
    }

    // A list of exactly the word already typed is noise: there is nothing left to
    // complete. Only when it was not asked for by hand, where showing the
    // documentation for that one name is the point.
    if (!completion_.manual && items.size() == 1 && items[0].insert_text() == prefix) {
        completion_.open = false;
        return;
    }

    // Rebuilding resets the selection unless the same word is still being typed,
    // in which case keeping it would point at a different candidate than the one
    // that was highlighted.
    if (!completion_.open || completion_.prefix != prefix || completion_.semantic != semantic ||
        completion_.member != member.path) {
        completion_.selected = 0;
        completion_.scroll = 0;
    }
    completion_.items = std::move(items);
    completion_.selected =
        std::clamp(completion_.selected, 0, static_cast<int>(completion_.items.size()) - 1);
    completion_.prefix = prefix;
    completion_.semantic = semantic;
    completion_.member = member.path;
    completion_.cursor = cursor;
    completion_.open = true;
}

ImVec2 TextEditor::caret_position(const OverlayGeometry& geometry, const std::string& text,
                                  std::size_t cursor, float line_height) const {
    cursor = std::min(cursor, text.size());
    std::size_t line_begin = cursor;
    while (line_begin > 0 && text[line_begin - 1] != '\n') --line_begin;
    int line = 0;
    for (std::size_t i = 0; i < line_begin; ++i) {
        if (text[i] == '\n') ++line;
    }
    ImFont* font = ImGui::GetFont();
    const float width =
        font->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.0f, text.data() + line_begin,
                            text.data() + cursor)
            .x;
    return ImVec2(geometry.origin.x + width,
                  geometry.origin.y + static_cast<float>(line) * line_height);
}

void TextEditor::draw_completion_popup(const ImVec2& caret, float line_height,
                                       const EditorStyle& style) const {
    if (!completion_.open || completion_.items.empty()) return;

    // Enough rows to choose from without the popup becoming the window. Past
    // this the list scrolls under the selection rather than growing.
    constexpr int kMaxRows = 9;
    constexpr float kPad = 6.0f;
    constexpr float kTagWidth = 16.0f;
    constexpr float kGap = 24.0f;

    const int count = static_cast<int>(completion_.items.size());
    const int rows = std::min(count, kMaxRows);
    // The window into the list follows the selection: it is the smallest one
    // that contains it, which keeps the highlighted row on screen without the
    // popup having to remember a scroll offset between frames.
    int first = std::clamp(0, completion_.selected - rows + 1, completion_.selected);
    first = std::clamp(first, 0, std::max(0, count - rows));

    ImFont* font = ImGui::GetFont();
    const float font_size = ImGui::GetFontSize();
    const auto measure = [&](const std::string& s) {
        return font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, s.c_str(), s.c_str() + s.size()).x;
    };

    float label_width = 0.0f;
    float detail_width = 0.0f;
    for (int i = first; i < first + rows; ++i) {
        const Completion& c = completion_.items[static_cast<std::size_t>(i)];
        label_width = std::max(label_width, measure(c.label));
        detail_width = std::max(detail_width, measure(c.detail));
    }
    // The documentation of the selected candidate, on its own line under the
    // list: it is the half of a completion popup that teaches something, and
    // squeezing it into the row would truncate it away.
    const std::string& doc = completion_.items[static_cast<std::size_t>(completion_.selected)].doc;
    const float doc_width = doc.empty() ? 0.0f : measure(doc);

    const float row_height = line_height + 2.0f;
    const float body_width = kTagWidth + label_width + kGap + detail_width;
    const float width = std::max(body_width, doc_width) + kPad * 2.0f;
    const float height =
        static_cast<float>(rows) * row_height + kPad * 2.0f + (doc.empty() ? 0.0f : row_height);

    // Below the caret's line by default, above it when there is no room - the
    // popup must never cover the line being typed.
    const ImVec2 viewport_min = ImGui::GetMainViewport()->WorkPos;
    const ImVec2 viewport_max(viewport_min.x + ImGui::GetMainViewport()->WorkSize.x,
                              viewport_min.y + ImGui::GetMainViewport()->WorkSize.y);
    ImVec2 position(caret.x, caret.y + line_height + 2.0f);
    if (position.y + height > viewport_max.y) position.y = caret.y - height - 2.0f;
    position.x = std::clamp(position.x, viewport_min.x, std::max(viewport_min.x, viewport_max.x - width));
    position.y = std::clamp(position.y, viewport_min.y, std::max(viewport_min.y, viewport_max.y - height));

    // The foreground list, so the popup is never clipped by the text box it
    // hangs over and never fights the input box for focus.
    ImDrawList* draw_list = ImGui::GetForegroundDrawList();
    const ImVec2 max(position.x + width, position.y + height);
    draw_list->AddRectFilled(position, max, ImGui::GetColorU32(ImGuiCol_PopupBg), 4.0f);
    draw_list->AddRect(position, max, ImGui::GetColorU32(ImGuiCol_Border), 4.0f);

    const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    for (int i = first; i < first + rows; ++i) {
        const Completion& c = completion_.items[static_cast<std::size_t>(i)];
        const float y = position.y + kPad + static_cast<float>(i - first) * row_height;
        if (i == completion_.selected) {
            draw_list->AddRectFilled(ImVec2(position.x + 2.0f, y - 1.0f),
                                     ImVec2(max.x - 2.0f, y + row_height - 1.0f),
                                     ImGui::GetColorU32(ImGuiCol_Header), 2.0f);
        }
        const float x = position.x + kPad;
        draw_list->AddText(font, font_size, ImVec2(x, y), dim, completion_tag(c.kind));
        draw_list->AddText(font, font_size, ImVec2(x + kTagWidth, y),
                           completion_color(c.kind, style), c.label.c_str());
        if (!c.detail.empty()) {
            draw_list->AddText(font, font_size,
                               ImVec2(max.x - kPad - measure(c.detail), y), dim,
                               c.detail.c_str());
        }
    }
    if (!doc.empty()) {
        const float y = position.y + kPad + static_cast<float>(rows) * row_height;
        draw_list->AddLine(ImVec2(position.x + kPad, y - 1.0f), ImVec2(max.x - kPad, y - 1.0f),
                           ImGui::GetColorU32(ImGuiCol_Border));
        draw_list->AddText(font, font_size, ImVec2(position.x + kPad, y + 1.0f), dim,
                           doc.c_str());
    }
}

void TextEditor::refresh_tokens(const std::string& text, Language language) {
    if (language == tokenized_language_ && text == tokenized_text_) return;

    tokenized_text_ = text;
    tokenized_language_ = language;
    tokens_ = tokenize(tokenized_text_, language);

    line_starts_.clear();
    line_starts_.push_back(0);
    for (std::size_t i = 0; i < tokenized_text_.size(); ++i) {
        if (tokenized_text_[i] == '\n') {
            line_starts_.push_back(static_cast<std::uint32_t>(i + 1));
        }
    }
}

bool TextEditor::overlay_geometry(const ImGuiWindow* box, const ImGuiInputTextState* state,
                                  float line_height, OverlayGeometry& out) const {
    if (line_starts_.empty() || line_height <= 0.0f) return false;

    out.origin = text_origin(box, state);
    // The box's own text is clipped to its whole interior, not to the padded
    // work area, so the overlays use the same rectangle - otherwise the last
    // partly visible line would end a few pixels earlier than the caret does.
    ImRect clip = box->InnerRect;
    clip.ClipWith(box->OuterRectClipped);
    out.clip_min = clip.Min;
    out.clip_max = clip.Max;

    // Only the lines that can be seen. Both ends get one line of slack so a
    // partially visible row at the top or bottom is still drawn.
    const int line_count = static_cast<int>(line_starts_.size());
    out.first_line = std::clamp(
        static_cast<int>((out.clip_min.y - out.origin.y) / line_height) - 1, 0, line_count - 1);
    out.last_line = std::clamp(
        static_cast<int>((out.clip_max.y - out.origin.y) / line_height) + 1, 0, line_count - 1);
    return true;
}

std::uint32_t TextEditor::line_end(int line) const {
    const int line_count = static_cast<int>(line_starts_.size());
    if (line + 1 < line_count) {
        // One past the line's last character, not counting the break itself.
        std::uint32_t end = line_starts_[static_cast<std::size_t>(line + 1)];
        if (end > 0 && tokenized_text_[end - 1] == '\n') --end;
        if (end > 0 && tokenized_text_[end - 1] == '\r') --end;
        return end;
    }
    return static_cast<std::uint32_t>(tokenized_text_.size());
}

void TextEditor::draw_syntax(const OverlayGeometry& geometry, const EditorStyle& style,
                             ImDrawList* draw_list, float line_height) const {
    if (tokens_.empty()) return;

    // ImGui clips the input's text per glyph rather than per character cell, so
    // the overlay fine-clips against the same rectangle and the two agree on the
    // half glyph at either edge.
    const ImVec4 fine_clip(geometry.clip_min.x, geometry.clip_min.y, geometry.clip_max.x,
                           geometry.clip_max.y);
    ImFont* font = ImGui::GetFont();
    const float font_size = ImGui::GetFontSize();
    const char* base = tokenized_text_.data();

    for (int line = geometry.first_line; line <= geometry.last_line; ++line) {
        const std::uint32_t begin_offset = line_starts_[static_cast<std::size_t>(line)];
        const std::uint32_t end_offset = line_end(line);

        // Tokens never cross a line break, so the first one on this line is the
        // first whose offset reaches the line start.
        auto it = std::lower_bound(tokens_.begin(), tokens_.end(), begin_offset,
                                   [](const Token& t, std::uint32_t offset) {
                                       return t.offset < offset;
                                   });

        const float y = geometry.origin.y + static_cast<float>(line) * line_height;
        float x = geometry.origin.x;
        for (; it != tokens_.end() && it->offset < end_offset; ++it) {
            const char* begin = base + it->offset;
            const char* end = begin + it->length;
            const float width = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, begin, end).x;

            if (it->kind != TokenKind::Plain && x + width >= geometry.clip_min.x &&
                x <= geometry.clip_max.x) {
                const ImU32 color = style.syntax_colors[static_cast<std::size_t>(it->kind)];
                draw_list->AddText(font, font_size, ImVec2(x, y), color, begin, end, 0.0f,
                                   &fine_clip);
            }
            x += width;
            if (x > geometry.clip_max.x) break;  // the rest of the line is off to the right
        }
    }
}

/// The span a diagnostic points at: the token under its column, or the line's
/// whole content when the compiler gave no usable column. A token is the right
/// unit because that is what the message is almost always about - an undeclared
/// name, a wrong type - and it needs no parsing to find.
void TextEditor::diagnostic_span(int line, int column, std::uint32_t& out_begin,
                                 std::uint32_t& out_end) const {
    const std::uint32_t begin_offset = line_starts_[static_cast<std::size_t>(line)];
    const std::uint32_t end_offset = line_end(line);

    out_begin = begin_offset;
    out_end = end_offset;
    if (column <= 0) {
        // Leading indentation carries no information; skip it so the squiggle
        // starts under the code rather than out in the margin.
        while (out_begin < out_end &&
               (tokenized_text_[out_begin] == ' ' || tokenized_text_[out_begin] == '\t')) {
            ++out_begin;
        }
        return;
    }

    const std::uint32_t at =
        std::min(begin_offset + static_cast<std::uint32_t>(column - 1), end_offset);
    for (const auto& token : tokens_) {
        if (token.offset > at) break;
        if (token.offset + token.length > at && token.kind != TokenKind::Plain) {
            out_begin = token.offset;
            out_end = token.offset + token.length;
            return;
        }
    }
    // A column pointing at whitespace or past the end: underline from there on,
    // which still puts the mark where the compiler was looking.
    out_begin = at;
}

void TextEditor::draw_squiggles(const OverlayGeometry& geometry, const Diagnostics& diagnostics,
                                ImDrawList* draw_list, float line_height,
                                const EditorStyle& style) const {
    if (diagnostics.empty() || line_starts_.empty()) return;

    ImFont* font = ImGui::GetFont();
    const float font_size = ImGui::GetFontSize();
    const char* base = tokenized_text_.data();
    const int line_count = static_cast<int>(line_starts_.size());

    for (const Diagnostic& d : diagnostics) {
        const int line = d.line - 1;  // diagnostics are 1-based
        if (line < geometry.first_line || line > geometry.last_line) continue;
        if (line < 0 || line >= line_count) continue;

        std::uint32_t begin_offset = 0;
        std::uint32_t end_offset = 0;
        diagnostic_span(line, d.column, begin_offset, end_offset);
        if (end_offset <= begin_offset) continue;

        const std::uint32_t line_offset = line_starts_[static_cast<std::size_t>(line)];
        const float x0 =
            geometry.origin.x +
            font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, base + line_offset, base + begin_offset).x;
        const float x1 =
            x0 + font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, base + begin_offset,
                                     base + end_offset).x;
        // Just below the glyphs, inside the line box, so consecutive bad lines
        // do not run their marks together.
        const float y = geometry.origin.y + static_cast<float>(line + 1) * line_height - 1.5f;

        // A zigzag rather than a straight rule: it reads as "wrong here" at a
        // glance and cannot be mistaken for an underline in the text itself.
        constexpr float kPeriod = 4.0f;
        constexpr float kAmplitude = 1.5f;
        const float left = std::max(x0, geometry.clip_min.x);
        const float right = std::min(x1, geometry.clip_max.x);
        if (right <= left) continue;

        draw_list->PathClear();
        bool up = true;
        for (float x = left; x < right; x += kPeriod * 0.5f) {
            draw_list->PathLineTo(ImVec2(x, up ? y - kAmplitude : y));
            up = !up;
        }
        draw_list->PathLineTo(ImVec2(right, up ? y - kAmplitude : y));
        draw_list->PathStroke(severity_color(d.severity, style), ImDrawFlags_None, 1.0f);
    }
}

bool TextEditor::draw(const char* id, std::string& text, const Diagnostics& diagnostics,
                      const EditorStyle& style, const DocumentInputs& document) {
    // Worst diagnostic per line, so the gutter marker reflects severity.
    std::map<int, Severity> line_marks;
    for (const auto& d : diagnostics) {
        if (d.line <= 0) continue;
        auto it = line_marks.find(d.line);
        if (it == line_marks.end() || static_cast<int>(d.severity) > static_cast<int>(it->second)) {
            line_marks[d.line] = d.severity;
        }
    }

    // The code area draws at the editor's own text size, which is what the zoom
    // shortcuts change. Pushed before anything is measured, so the gutter, the
    // input box and the token overlay all agree on how tall a line is. The
    // diagnostics below are popped back to the UI size: they are prose, and
    // zooming the code has no reason to reflow them.
    ImGui::PushFont(nullptr, style.font_size);

    const float line_height = ImGui::GetTextLineHeight();
    const int total_lines = count_lines(text);
    const float gutter_width =
        style.line_numbers
            ? ImGui::CalcTextSize(std::to_string(std::max(total_lines, 100)).c_str()).x + 18.0f
            : 0.0f;

    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float body_height = style.error_lens && !diagnostics.empty()
                                  ? available.y - std::min(available.y * 0.35f, 140.0f)
                                  : available.y;

    ImGui::BeginGroup();

    if (style.line_numbers) {
        ImGui::BeginChild("##gutter", ImVec2(gutter_width, body_height), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        // Kept in sync with the text box by mirroring its scroll offset.
        ImGui::SetScrollY(last_scroll_y_);
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        for (int line = 1; line <= total_lines; ++line) {
            const float y = origin.y + static_cast<float>(line - 1) * line_height;
            auto mark = line_marks.find(line);
            if (mark != line_marks.end()) {
                draw_list->AddCircleFilled(ImVec2(origin.x + 5.0f, y + line_height * 0.5f), 4.0f,
                                           severity_color(mark->second, style));
            }
            const std::string number = std::to_string(line);
            const float text_width = ImGui::CalcTextSize(number.c_str()).x;
            draw_list->AddText(ImVec2(origin.x + gutter_width - text_width - 6.0f, y),
                               ImGui::GetColorU32(ImGuiCol_TextDisabled), number.c_str());
        }
        ImGui::Dummy(ImVec2(gutter_width, static_cast<float>(total_lines) * line_height));
        ImGui::EndChild();
        ImGui::SameLine(0.0f, 0.0f);
    }

    indent_ = style.insert_spaces ? std::string(static_cast<std::size_t>(std::clamp(style.tab_width, 1, 16)), ' ')
                                 : std::string("\t");

    // Every key the editor handles itself has to be taken from the input box
    // before the box is drawn, because that is the only point at which ImGui
    // will believe someone else owns it. A key locked to another owner reads as
    // not pressed to anyone who does not name that owner - including the box's
    // own polling, whichever way it does it - so the newline, the tab and the
    // caret move simply never happen.
    //
    // The list gets first refusal on the keys it shares with indentation: while
    // it is up, Tab and Enter mean "insert this candidate".
    const bool box_active = ImGui::GetActiveID() == input_item_id(id);
    pending_action_ = CompletionAction::None;
    if (completion_.open) {
        const ImGuiID owner = ImGui::GetID("##completion_keys");
        const auto take = [owner](ImGuiKey key, ImGuiInputFlags repeat = ImGuiInputFlags_None) {
            ImGui::SetKeyOwner(key, owner, ImGuiInputFlags_LockThisFrame);
            return ImGui::IsKeyPressed(key, repeat, owner);
        };

        const bool up = take(ImGuiKey_UpArrow, ImGuiInputFlags_Repeat);
        const bool down = take(ImGuiKey_DownArrow, ImGuiInputFlags_Repeat);
        const bool accept =
            take(ImGuiKey_Enter) | take(ImGuiKey_KeypadEnter) | take(ImGuiKey_Tab);
        const bool cancel = take(ImGuiKey_Escape);

        if (up || down) {
            const int count = static_cast<int>(completion_.items.size());
            if (count > 0) {
                completion_.selected = (completion_.selected + (down ? 1 : -1) + count) % count;
            }
        } else if (accept) {
            pending_action_ = CompletionAction::Accept;
        } else if (cancel || ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                   ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            // A click is always a dismissal rather than a choice: the popup is
            // drawn into the foreground and takes no input of its own, so a click
            // on a row would also move the caret out from under the word.
            pending_action_ = CompletionAction::Dismiss;
        }
    }
    if (pending_action_ == CompletionAction::Dismiss) {
        completion_.open = false;
        completion_.manual = false;
        completion_.items.clear();
        pending_action_ = CompletionAction::None;
    }

    if (pending_action_ == CompletionAction::None) {
        capture_indent_keys(box_active && !document.read_only, style);
    } else {
        pending_edit_ = EditAction::None;
    }

    ImGuiInputTextFlags flags = ImGuiInputTextFlags_AllowTabInput |
                                ImGuiInputTextFlags_CallbackResize |
                                ImGuiInputTextFlags_CallbackAlways |
                                ImGuiInputTextFlags_NoHorizontalScroll * (style.word_wrap ? 1 : 0);
    // ReadOnly rather than not drawing the box: the caret, the selection, copy,
    // the scrollbar and every overlay keep working, and only the typing stops.
    if (document.read_only) flags |= ImGuiInputTextFlags_ReadOnly;

    if (text.capacity() == text.size()) text.reserve(text.size() + 1024);

    // Found before the input is drawn, not after, so that the decision to hide
    // ImGui's own text is only taken when there is somewhere to draw the
    // replacement. On the first frame of a document there is not, and the text
    // renders in one color for that frame rather than not at all.
    ImGuiWindow* box = input_child_window(id);
    const bool overlay = style.syntax_highlight && box != nullptr;

    // A fully transparent text color does not draw invisible glyphs: ImGui skips
    // the whole RenderText call when the alpha is zero, so the tokens drawn
    // below are the only text rendering that happens. The caret has its own
    // color and the selection is a rectangle behind the text, so both survive.
    if (overlay) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32_BLACK_TRANS);
    just_completed_ = false;
    CallbackPayload payload{&text, this};
    const bool changed =
        ImGui::InputTextMultiline(id, text.data(), text.capacity() + 1,
                                  ImVec2(-FLT_MIN, body_height), flags, input_callback, &payload);
    if (overlay) ImGui::PopStyleColor();

    // The caret, read back after the box has had its say. Kept even on the
    // frames the box is not active, where it is simply the last one seen: the
    // popup is only ever open while it is.
    bool active = false;
    std::size_t cursor_offset = completion_.cursor;
    if (ImGuiInputTextState* state = ImGui::GetInputTextState(ImGui::GetItemID())) {
        active = true;
        // Cursor line, used by the status bar and by "go to error".
        int line = 1;
        const int cursor = state->GetCursorPos();
        for (int i = 0; i < cursor && i < static_cast<int>(text.size()); ++i) {
            if (text[static_cast<std::size_t>(i)] == '\n') ++line;
        }
        cursor_line_ = line;
        cursor_offset = std::min(static_cast<std::size_t>(std::max(cursor, 0)), text.size());
    }

    // Mirror the text box's scroll into the gutter on the next frame, and honour
    // a pending jump to a line. Both read the child window found above, whose
    // fields the input has just refreshed.
    if (box != nullptr) {
        last_scroll_y_ = box->Scroll.y;
        if (scroll_to_line_ > 0) {
            ImGui::SetScrollY(box, static_cast<float>(scroll_to_line_ - 1) * line_height);
            scroll_to_line_ = 0;
        }
        // After the widget, not before: `text` now holds this frame's edit, so
        // what gets colored is what the box just drew rather than the buffer as
        // it stood a keystroke ago. The line table is needed for the caret's
        // position too, so it is refreshed whenever either overlay wants it.
        if (overlay || active) refresh_tokens(text, style.language);
        // `active` and not `completion_.open`: the geometry below is what lets
        // the list open in the first place, so gating it on the list already
        // being open would mean it never could.
        if (overlay || active) {
            OverlayGeometry geometry;
            const ImGuiInputTextState* state = ImGui::GetInputTextState(ImGui::GetItemID());
            if (overlay_geometry(box, state, line_height, geometry)) {
                if (overlay) {
                    ImDrawList* draw_list = box->DrawList;
                    draw_list->PushClipRect(geometry.clip_min, geometry.clip_max, true);
                    draw_syntax(geometry, style, draw_list, line_height);
                    if (style.error_lens) {
                        draw_squiggles(geometry, diagnostics, draw_list, line_height, style);
                    }
                    draw_list->PopClipRect();
                }
                // The list is rebuilt after the edit and drawn in the same frame,
                // so the popup never lags the letter that was just typed. Only
                // while the box is the active widget: a list hanging over a
                // document nobody is typing into is just an obstruction.
                if (active) {
                    if (just_completed_) {
                        completion_.open = false;
                        completion_.manual = false;
                    } else {
                        update_completion(text, cursor_offset, style, document, changed);
                    }
                    if (completion_.open) {
                        const ImVec2 caret =
                            caret_position(geometry, text, cursor_offset, line_height);
                        // A caret scrolled out of the box takes the popup with
                        // it, rather than leaving it pinned to an edge.
                        if (caret.y >= geometry.clip_min.y - line_height &&
                            caret.y <= geometry.clip_max.y) {
                            draw_completion_popup(caret, line_height, style);
                        }
                    }
                }
            }
        }
        if (!active && completion_.open) {
            completion_.open = false;
            completion_.manual = false;
        }
        last_cursor_ = cursor_offset;
    }

    ImGui::PopFont();

    if (style.error_lens && !diagnostics.empty()) {
        ImGui::BeginChild("##lens", ImVec2(0, 0), true);
        for (int i = 0; i < static_cast<int>(diagnostics.size()); ++i) {
            const Diagnostic& d = diagnostics[static_cast<std::size_t>(i)];
            const std::string label =
                (d.line > 0 ? "line " + std::to_string(d.line) + ": " : std::string()) + d.message;

            // A full-width row sized to the wrapped text, with the text drawn on
            // top of it: a Selectable's own label is clipped to one line, which
            // is how the interesting half of a compiler message used to vanish.
            ImGui::PushID(i);
            const float wrap_width = ImGui::GetContentRegionAvail().x;
            const ImVec2 text_size = ImGui::CalcTextSize(label.c_str(), nullptr, false, wrap_width);
            const ImVec2 row_start = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##lens_row", false, ImGuiSelectableFlags_AllowOverlap,
                                  ImVec2(0.0f, text_size.y)) &&
                d.line > 0) {
                request_scroll_to(d.line);
            }
            if (ImGui::BeginPopupContextItem("##lens_menu")) {
                if (ImGui::MenuItem("Copy message")) ImGui::SetClipboardText(d.format().c_str());
                ImGui::EndPopup();
            }
            ImGui::SetCursorScreenPos(row_start);
            ImGui::PushStyleColor(ImGuiCol_Text, severity_color(d.severity, style));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_width);
            ImGui::TextUnformatted(label.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    ImGui::EndGroup();
    return changed;
}

}  // namespace ssstudio::gui
