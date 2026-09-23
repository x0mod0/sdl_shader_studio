#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <iterator>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <utility>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>  // GetCurrentTabBar, for the order the tabs are shown in

#include "app.h"
#include "fonts.h"
#include "panel_common.h"
#include "tab_scope.h"
#include "text_editor.h"
#include "widgets.h"

namespace ssstudio::gui {
namespace {

// One editor state per document, so caret and scroll survive tab switches. The
// key carries the project's session key as well as the shader id, because two
// open projects may each hold a shader called "plasma" and they must not share
// a caret. The prefix is also what lets a closing project drop its states in one
// pass; see forget_editor_states.
std::map<std::string, TextEditor>& editors() {
    static std::map<std::string, TextEditor> map;
    return map;
}

// '\x1f' (unit separator) cannot occur in a session key or a shader id, so the
// two halves can never run together into another pair's key.
std::string editor_key(const std::string& session_key, const std::string& document_id) {
    return session_key + '\x1f' + document_id;
}

/// Ctrl/Cmd with =, - and 0 while the editor has focus: the same zoom keys every
/// browser and editor on the platform uses. ImGuiMod_Ctrl is Cmd on macOS and
/// Ctrl elsewhere, so one chord is native in both places without asking which we
/// are on.
///
/// Zooming in accepts Shift too, because '+' is Shift and '=' on most layouts
/// and someone reaching for "Cmd and plus" presses that. ImGui matches a chord's
/// modifiers exactly, so the two have to be spelled out separately rather than
/// one covering both. The keypad's own +, - and 0 are accepted alongside.
///
/// Routed globally because the text box is the active item while the caret is in
/// it and would otherwise own the keys; the focus test above the call is what
/// keeps the shortcut to the editor.
///
/// No repeat on purpose: each press is one step, and each step is written to the
/// settings file. Holding the key would be a write per frame for a control that
/// is used a few presses at a time.
float zoom_delta_from_keyboard(bool& out_reset) {
    out_reset = false;
    constexpr ImGuiInputFlags route = ImGuiInputFlags_RouteGlobal;

    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_0, route) ||
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Keypad0, route)) {
        out_reset = true;
        return 0.0f;
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Equal, route) ||
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Equal, route) ||
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_KeypadAdd, route)) {
        return 1.0f;
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Minus, route) ||
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_KeypadSubtract, route)) {
        return -1.0f;
    }
    return 0.0f;
}

/// Applies a zoom step to the editor font size, saving only when the size really
/// moved - at the ends of the range the shortcut is a no-op, and a no-op has no
/// business rewriting the settings file.
void apply_editor_zoom(App& app, float delta, bool reset) {
    float& size = app.settings().editor.font_size;
    const float previous = size;
    size = reset ? kDefaultEditorFontSize
                 : std::clamp(size + delta, kMinEditorFontSize, kMaxEditorFontSize);
    if (size != previous) app.save_settings_now();
}

/// Both of these now live in panel_common.h, because the graph window's title
/// names the same shader the tabs do and the two must agree.
std::string display_name(const Document& doc) {
    return shader_display_name(doc.path, doc.id);
}

const char* stage_label(Stage s) { return stage_suffix(s); }

/// The language a shader is written in, the way the header names it.
const char* language_name(Language language) {
    return language == Language::GLSL ? "GLSL" : "HLSL";
}

void draw_document_header(App& app, Document& doc, const ThemeInk& ink) {
    // Where the file is and how its last compile went, on one line: the path on
    // the left, the verdict and the file's language on the right. On a panel
    // too narrow for both, the verdict folds under the path rather than
    // pushing it off the edge.
    //
    // The path is spelled from the directory holding the project, so it starts
    // with the project's own name - which is the part that says which of two
    // similar shaders this is.
    std::error_code ec;
    std::filesystem::path shown =
        std::filesystem::relative(doc.path, app.project().root.parent_path(), ec);
    if (ec || shown.empty() || shown.native().rfind("..", 0) == 0) shown = doc.path;
    const std::string folder =
        shown.has_parent_path() ? shown.parent_path().generic_string() + "/" : std::string();
    const std::string file = shown.filename().string();

    const float row_width = ImGui::GetContentRegionAvail().x;
    ImGui::BeginGroup();
    {
        MonoScope mono(11.0f);
        colored_text(folder, ink.muted);
        ImGui::SameLine(0.0f, 0.0f);
        colored_text(file, ink.soft);
    }
    ImGui::EndGroup();
    // Hover for the whole of it and click to copy, the way every other path in
    // the interface behaves.
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::IsItemClicked()) {
            ImGui::SetClipboardText(external_path(doc.path).string().c_str());
        }
        ImGui::SetItemTooltip("%s\nClick to copy", external_path(doc.path).string().c_str());
    }
    const float path_width = ImGui::GetItemRectSize().x;

    // The verdict.
    char status[96];
    ImU32 status_color = ink.muted;
    if (doc.compiling) {
        std::snprintf(status, sizeof(status), "compiling...");
    } else if (doc.compiled_ok) {
        std::snprintf(status, sizeof(status), "ok \xC2\xB7 %.0f ms", doc.last_compile_ms);
        status_color = ink.ok;
    } else if (!doc.diagnostics.empty()) {
        int errors = 0, warnings = 0;
        for (const auto& d : doc.diagnostics) {
            if (d.severity == Severity::Error) ++errors;
            if (d.severity == Severity::Warning) ++warnings;
        }
        std::snprintf(status, sizeof(status), "%d error(s), %d warning(s)", errors, warnings);
        status_color = ink.error;
    } else {
        std::snprintf(status, sizeof(status), "not compiled yet");
    }

    // What the file is written in, and how Tab indents it.
    char details[64] = "";
    if (const ShaderDesc* desc = app.project().find_shader(doc.id)) {
        const auto& editor = app.settings().editor;
        std::snprintf(details, sizeof(details), "%s \xC2\xB7 %s",
                      language_name(app.project().language_of(*desc)),
                      editor.insert_spaces
                          ? ("spaces " + std::to_string(editor.tab_width)).c_str()
                          : "tabs");
    }

    FontScope small(nullptr, 12.0f);
    const float gap = design_px(6.0f);
    const float group_gap = design_px(12.0f);
    float right_width = design_px(6.0f) + gap + text_width(status);
    if (details[0] != '\0') right_width += group_gap + text_width(details);
    if (doc.dirty) right_width += text_width("modified") + gap + button_width("Save") + group_gap;

    if (path_width + group_gap + right_width <= row_width) same_line_right_aligned(right_width);

    // Unsaved edits come first, where the eye lands, with the way to keep them
    // beside the word.
    if (doc.dirty) {
        ImGui::AlignTextToFramePadding();
        colored_text("modified", ink.warn);
        ImGui::SameLine(0.0f, gap);
        if (ImGui::SmallButton("Save")) app.save_document(doc);
        ImGui::SameLine(0.0f, group_gap);
    }
    status_dot(status_color);
    ImGui::SameLine(0.0f, gap);
    colored_text(status, status_color);
    if (details[0] != '\0') {
        ImGui::SameLine(0.0f, group_gap);
        colored_text(details, ink.muted);
    }

    // A shader a graph writes. Said here rather than left for the user to
    // discover by typing into a box that ignores them, and with the way out
    // beside it, because "read-only" without "and here is how to stop that"
    // reads as a fault rather than a state.
    if (doc.generated_by_graph) {
        ImGui::Separator();
        const char* note = "Generated by a graph. Edit it there, or detach to take over the file.";
        FlowLayout generated;
        generated.next(text_width(note));
        ImGui::TextDisabled("%s", note);
        generated.next(button_width("Open graph"));
        if (ImGui::SmallButton("Open graph")) app.show_graph = true;
        generated.next(button_width("Detach"));
        if (ImGui::SmallButton("Detach")) app.detach_graph(doc.id);
    }

    // The file changed underneath an unsaved buffer: never silently pick a side.
    if (doc.external_change_pending) {
        ImGui::Separator();
        const char* warning = "This file changed on disk while you had unsaved edits.";
        FlowLayout conflict;
        conflict.next(text_width(warning));
        ImGui::TextColored(theme_vec4(app.theme().diagnostic(Severity::Warning)), "%s", warning);
        conflict.next(button_width("Keep mine"));
        if (ImGui::SmallButton("Keep mine")) doc.external_change_pending = false;
        conflict.next(button_width("Reload from disk"));
        if (ImGui::SmallButton("Reload from disk")) {
            std::ifstream f(doc.path, std::ios::binary);
            if (f) {
                std::ostringstream os;
                os << f.rdbuf();
                doc.text = os.str();
                doc.dirty = false;
                app.schedule_compile(doc, true);
            }
            doc.external_change_pending = false;
        }
    }
}

/// The recent-project list on the landing screen, so a returning session is one
/// click from where it left off instead of a trip through the File menu.
///
/// Entries are stored as manifest paths, which all end in the same file name, so
/// the directory name is what gets the clickable row and the path below it
/// disambiguates two projects that happen to share one.
void draw_recent_projects(App& app) {
    auto& recent = app.settings().recent_projects;
    if (recent.empty()) return;

    ImGui::Spacing();
    ImGui::SeparatorText("Recent projects");

    // Opening a project moves it to the front of this same vector, and every
    // form of forgetting erases from it, so the loop only records the intent and
    // all of it happens once the loop has finished walking.
    std::filesystem::path chosen;
    std::filesystem::path forget;

    for (std::size_t i = 0; i < recent.size(); ++i) {
        const std::filesystem::path& manifest = recent[i];
        std::error_code ec;
        const bool exists = std::filesystem::exists(manifest, ec);

        // Directory names repeat often enough ("shaders", "demo") that the label
        // is a poor id; the position in a list this short is a stable one.
        ImGui::PushID(static_cast<int>(i));

        const std::filesystem::path dir = manifest.parent_path();
        // A manifest at the filesystem root has no parent to name it. Falling
        // back to the whole path keeps the row clickable rather than blank.
        const std::string name =
            dir.filename().empty() ? manifest.generic_string() : dir.filename().string();

        // A project that moved or was deleted still lists, greyed out, so the
        // entry explains itself rather than failing on click - same as the menu.
        const ImGuiSelectableFlags flags =
            (exists ? ImGuiSelectableFlags_None : ImGuiSelectableFlags_Disabled) |
            // The remove button below sits on top of this row, and an item that
            // has already claimed the pointer will not let it be pressed.
            ImGuiSelectableFlags_AllowOverlap;
        if (ImGui::Selectable(name.c_str(), false, flags)) chosen = manifest;
        if (!exists && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("missing on disk");
        }

        // One entry at a time, on every row rather than only on the ones whose
        // project has gone missing. A list this short fills up with projects
        // opened once to look at something, and those are precisely the entries
        // in the way of the two or three that are being worked on - a list you
        // can only prune by deleting the folder is not a list you can prune.
        //
        // The button is an item of its own, drawn after the row, so the row's
        // Disabled flag does not reach it and a missing entry is still one
        // click from gone.
        same_line_right_aligned(button_width("Forget"));
        if (ImGui::SmallButton("Forget")) forget = manifest;
        if (ImGui::IsItemHovered()) {
            // Worth spelling out: this is a list of places the app has been,
            // and forgetting one is not the same as deleting a project.
            ImGui::SetTooltip("Remove from this list. The project on disk is left alone.");
        }

        ImGui::Indent();
        // Copy on click rather than a button per entry: it keeps the row to two
        // lines, and the same gesture works on every path in the interface.
        copyable_path(dir.generic_string());
        ImGui::Unindent();

        ImGui::PopID();
    }

    // No whole-list actions here on purpose. "Forget missing projects" and
    // "Clear recent projects" live in File > Recent projects, which is the other
    // half of the division that menu's own comment describes: the whole-list
    // gesture belongs in the menu, and pruning one entry at a time belongs on
    // this screen, where a click does not close what you are working in.

    // After the loop: both of these change the vector it walks.
    if (!forget.empty()) app.forget_project(forget);
    if (!chosen.empty()) app.open_project(chosen);
}

/// The "open something that is closed" pair of items: a submenu listing what is
/// closed, and an "open all" beside it rather than buried inside it.
///
/// Shared by the tab context menu and the add button's menu, which offer the
/// same thing from two places someone might reach for it.
void draw_open_closed_items(App& app, TabMenuRequest& out) {
    const std::vector<std::string> closed = app.closed_documents();

    if (ImGui::BeginMenu("Open shader", !closed.empty())) {
        for (const std::string& id : closed) {
            const Document* doc = app.find_document(id);
            if (!doc) continue;
            // By name, as everywhere else in the editor, with the stage to tell
            // a pair apart and an unsaved marker because a closed tab can still
            // be holding edits.
            const std::string label = display_name(*doc) + " (" + stage_label(doc->stage) + ")" +
                                      (doc->dirty ? " *" : "");
            if (ImGui::MenuItem(label.c_str())) out.open.push_back(id);
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Open all shaders", nullptr, false, !closed.empty())) out.open = closed;
}

/// Opens the shader's containing directory in the desktop's file manager.
///
/// SDL has no "reveal this file" call, so this opens the folder rather than
/// selecting the file inside it - which is the portable half of the gesture and
/// the useful one when the next step is dragging something in beside it.
void open_containing_directory(App& app, const std::filesystem::path& file) {
    const std::filesystem::path directory = external_path(file).parent_path();
    // A file:// URL has to be absolute; a relative one is not a location at all,
    // and every desktop refuses it without saying why.
    if (directory.empty() || !directory.is_absolute()) {
        app.log(Severity::Warning,
                "could not work out where " + file.filename().string() + " lives on disk");
        return;
    }
    if (!SDL_OpenURL(file_url(directory).c_str())) {
        app.log(Severity::Warning,
                std::string("could not open ") + directory.string() + ": " + SDL_GetError());
    }
}

/// The shader tab bar's look: pills in the code font rather than tabs hanging
/// from an overline. Still ImGui's own tab bar underneath - reordering by drag,
/// scrolling when the row overflows, the list button and the close buttons all
/// keep working - only its colours and metrics change.
///
/// Suspendable, because a tab's context menu and tooltip are drawn from inside
/// the bar, and neither should come out in the code font with pill padding.
class PillTabStyle {
public:
    explicit PillTabStyle(const ThemeInk& ink) : ink_(ink) { push(); }
    ~PillTabStyle() {
        if (pushed_) pop();
    }
    PillTabStyle(const PillTabStyle&) = delete;
    PillTabStyle& operator=(const PillTabStyle&) = delete;

    void suspend() {
        if (pushed_) pop();
    }
    void resume() {
        if (!pushed_) push();
    }

private:
    void push() {
        const ImU32 clear = with_alpha(ink_.raised, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, design_px(5.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_TabBarOverlineSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_TabBarBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_TabBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(design_px(10.0f), design_px(7.0f)));
        ImGui::PushStyleColor(ImGuiCol_Tab, clear);
        ImGui::PushStyleColor(ImGuiCol_TabHovered, ImGui::GetColorU32(ImGuiCol_HeaderHovered));
        ImGui::PushStyleColor(ImGuiCol_TabSelected, ink_.sunken);
        ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, clear);
        ImGui::PushStyleColor(ImGuiCol_TabDimmed, clear);
        ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected, ink_.sunken);
        ImGui::PushStyleColor(ImGuiCol_TabDimmedSelectedOverline, clear);
        ImGui::PushFont(mono_font(), type_size(12.0f));
        // The close button only under the pointer, on the selected tab as on
        // the others: its slot is where the status dot sits the rest of the
        // time, the way ImGui's own unsaved-document marker shares it. There is
        // no style variable for this, so the field is set and put back.
        ImGuiStyle& style = ImGui::GetStyle();
        saved_close_width_ = style.TabCloseButtonMinWidthSelected;
        style.TabCloseButtonMinWidthSelected = 0.0f;
        pushed_ = true;
    }
    void pop() {
        ImGui::GetStyle().TabCloseButtonMinWidthSelected = saved_close_width_;
        ImGui::PopFont();
        ImGui::PopStyleColor(7);
        ImGui::PopStyleVar(5);
        pushed_ = false;
    }

    const ThemeInk& ink_;
    bool pushed_ = false;
    float saved_close_width_ = 0.0f;
};

/// What a shader tab's dot says: a failed compile first, because that is what
/// the dot is for, then warnings, then unsaved edits. Zero for nothing to say.
ImU32 tab_status_color(const Document& doc, const ThemeInk& ink) {
    if (!doc.compiled_ok && !doc.compiling && !doc.diagnostics.empty()) return ink.error;
    for (const auto& d : doc.diagnostics) {
        if (d.severity == Severity::Warning) return ink.warn;
    }
    if (doc.dirty) return ink.accent;
    return 0;
}

/// Room after a tab's name for the stage suffix, as spaces in the tab's own
/// font - which is what the label is measured in. The suffix is then drawn into
/// that room by draw_tab_decorations().
std::string tab_label_padding(const char* suffix) {
    float needed = 0.0f;
    {
        FontScope small(mono_font(), 10.5f);
        needed = ImGui::CalcTextSize(suffix).x;
    }
    needed += design_px(8.0f);
    const float space = std::max(1.0f, ImGui::CalcTextSize(" ").x);
    return std::string(static_cast<std::size_t>(std::ceil(needed / space)), ' ');
}

/// The stage suffix over the room tab_label_padding() left, the status dot in
/// the close button's slot while the close button is not showing, and the
/// outline the selected pill carries. Called right after the tab item is
/// submitted, while its rectangle is the last item's.
void draw_tab_decorations(const std::string& name, const char* suffix, bool selected, ImU32 dot,
                          const ThemeInk& ink) {
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 padding = ImGui::GetStyle().FramePadding;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->PushClipRect(min, max, true);
    if (selected) {
        // ImGui trims a pixel off the top of every tab so it can sit flush
        // against the bar's edge; the outline follows the same shape.
        draw_list->AddRect(ImVec2(min.x, min.y + 1.0f), max, ink.subtle, design_px(5.0f));
    }
    const float center_y = (min.y + 1.0f + max.y) * 0.5f;
    {
        const float x = min.x + padding.x + ImGui::CalcTextSize(name.c_str()).x + design_px(8.0f);
        FontScope small(mono_font(), 10.5f);
        draw_list->AddText(ImVec2(x, center_y - ImGui::GetTextLineHeight() * 0.5f),
                           selected ? ink.muted : with_alpha(ink.muted, 0.7f), suffix);
    }
    // Where TabItemEx puts the close button: its own frame padding in from the
    // right edge, a font size wide.
    if (dot != 0 && !hovered) {
        const float size = design_px(6.0f);
        draw_list->AddCircleFilled(
            ImVec2(max.x - padding.x - ImGui::GetFontSize() * 0.5f, center_y), size * 0.5f, dot);
    }
    draw_list->PopClipRect();
}

}  // namespace

void draw_shader_tab_menu(App& app, const Document& doc, const std::vector<std::string>& order,
                          TabMenuRequest& out) {
    // Attached with a null id so the popup is keyed on the tab's own ImGui id.
    // A string id would be resolved against the id stack instead, and the stack
    // gains an entry the moment a tab becomes the selected one - so the popup
    // would lose its identity, and close, exactly when it was clicked.
    if (!ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonRight)) return;

    // Right-clicking does not select a tab, so the menu can be about a shader
    // that is not the one on screen. Naming it removes the doubt - by name, as
    // everywhere else in the editor.
    ImGui::TextDisabled("%s", display_name(doc).c_str());
    ImGui::Separator();

    if (ImGui::MenuItem("Save", nullptr, false, doc.dirty)) {
        if (Document* target = app.find_document(doc.id)) app.save_document(*target);
    }
    if (ImGui::MenuItem("Rename...")) app.prompt_rename_shader(doc.id);
    if (ImGui::MenuItem("Duplicate")) out.duplicate = doc.id;

    ImGui::Separator();

    // Closing a tab is not closing the shader: the buffer, its unsaved edits and
    // its compiled blob all stay, the preview keeps drawing it, and a build
    // still includes it. That is why none of these ask for confirmation and why
    // "Delete..." is kept well away from them, at the bottom.
    const std::vector<std::string> left = tabs_in_scope(order, doc.id, CloseScope::Left);
    const std::vector<std::string> right = tabs_in_scope(order, doc.id, CloseScope::Right);
    const std::vector<std::string> others = tabs_in_scope(order, doc.id, CloseScope::Others);

    if (ImGui::MenuItem("Close")) out.close = {doc.id};
    if (ImGui::MenuItem("Close others", nullptr, false, !others.empty())) out.close = others;
    if (ImGui::MenuItem("Close to the left", nullptr, false, !left.empty())) out.close = left;
    if (ImGui::MenuItem("Close to the right", nullptr, false, !right.empty())) out.close = right;
    if (ImGui::MenuItem("Close all")) out.close = tabs_in_scope(order, doc.id, CloseScope::All);

    // Here as well as on the add button, because this is where the hand already
    // is after closing one too many.
    draw_open_closed_items(app, out);

    ImGui::Separator();

    // Only the stage the shader actually is: a vertex shader cannot be the
    // fragment half of the preview, and a compute shader is neither.
    if (doc.stage == Stage::Vertex || doc.stage == Stage::Fragment) {
        const bool fragment = doc.stage == Stage::Fragment;
        std::string& choice =
            fragment ? app.preview_fragment_choice() : app.preview_vertex_choice();
        const char* label = fragment ? "Preview as the fragment stage"
                                     : "Preview as the vertex stage";
        // Disabled rather than hidden when the shader has never compiled: the
        // preview only accepts a shader it has a blob for, and greying the item
        // out is what says why.
        if (ImGui::MenuItem(label, nullptr, choice == doc.id, doc.ever_compiled_ok)) {
            choice = doc.id;
            // The active pipeline is what the preview bar reads, so setting only
            // the session choice would be undone on the next frame. Saved here
            // rather than left dirty, because a pipeline lives in the manifest.
            if (PreviewPipeline* pipeline = app.project().active_pipeline()) {
                (fragment ? pipeline->fragment : pipeline->vertex) = doc.id;
                Diagnostics diags;
                save_project(app.project(), diags);
                for (const auto& d : diags) app.log(d.severity, d.format());
            }
        }
        if (!doc.ever_compiled_ok && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("This shader has not compiled yet.");
        }
    }

    if (ImGui::MenuItem("Copy file path")) {
        // The absolute path: what gets pasted into a terminal or another
        // application, neither of which knows where the project directory is.
        ImGui::SetClipboardText(external_path(doc.path).string().c_str());
    }
    if (ImGui::MenuItem("Show in file manager")) open_containing_directory(app, doc.path);

    ImGui::Separator();
    if (ImGui::MenuItem("New shader...", "Ctrl+Shift+N")) out.new_shader = true;

    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, theme_vec4(app.theme().diagnostic(Severity::Error)));
    const bool remove = ImGui::MenuItem("Delete...");
    ImGui::PopStyleColor();
    if (remove) app.prompt_delete_shader(doc.id);

    ImGui::EndPopup();
}

void forget_editor_states(const std::string& session_key) {
    const std::string prefix = session_key + '\x1f';
    auto& map = editors();
    for (auto it = map.begin(); it != map.end();) {
        it = it->first.compare(0, prefix.size(), prefix) == 0 ? map.erase(it) : std::next(it);
    }
}

void forget_editor_state(const std::string& session_key, const std::string& document_id) {
    editors().erase(editor_key(session_key, document_id));
}

void rename_editor_state(const std::string& session_key, const std::string& old_id,
                         const std::string& new_id) {
    if (old_id == new_id) return;
    auto& map = editors();
    const auto it = map.find(editor_key(session_key, old_id));
    if (it == map.end()) return;
    TextEditor state = std::move(it->second);
    map.erase(it);
    // Assigned rather than inserted: a state may already exist under the new key
    // if that id was open earlier in the session, and the one being carried over
    // is the current one.
    map[editor_key(session_key, new_id)] = std::move(state);
}

void draw_editor_panel(App& app) {
    // Plainly "Editor": which project is in front is the project tab bar's to
    // say (App::draw_project_tabs), and repeating the name here only said the
    // same thing twice. The label still carries the "###Editor" id so the six
    // DockBuilderDockWindow("Editor", ...) calls in the layout presets, and a
    // saved layout, keep matching this window.
    const char* title = "Editor###Editor";

    // No p_open, so the tab gets no close button. A close button here would read
    // as "close the project" but would only hide the panel; closing a project is
    // the project tab bar's close button, or File > Close. View > Editor still
    // toggles the panel.
    PanelScope panel(title, nullptr);
    if (!panel) return;

    if (!app.project_open()) {
        ImGui::TextDisabled("No project open.");
        ImGui::Spacing();
        FlowLayout row;
        row.next(button_width("New project..."));
        if (ImGui::Button("New project...")) app.prompt_new_project();
        row.next(button_width("Open project..."));
        if (ImGui::Button("Open project...")) app.prompt_open_project();
        draw_recent_projects(app);
        ImGui::Spacing();
        ImGui::TextWrapped(
            "You can also pass a project path on the command line, or scaffold one with "
            "`ssstudio new <dir>`.");
        return;
    }

    if (app.documents().empty()) {
        ImGui::TextDisabled("This project has no shaders yet.");
        ImGui::Spacing();
        FlowLayout row;
        row.next(button_width("New shader..."));
        if (ImGui::Button("New shader...")) app.prompt_new_shader();
        row.next(button_width("Import fullscreen shader..."));
        if (ImGui::Button("Import fullscreen shader...")) app.prompt_import_shader();
        return;
    }

    EditorStyle style;
    const auto& editor_settings = app.settings().editor;
    style.line_numbers = editor_settings.show_line_numbers;
    style.word_wrap = editor_settings.word_wrap;
    style.error_lens = editor_settings.error_lens;
    style.tab_width = editor_settings.tab_width;
    style.insert_spaces = editor_settings.insert_spaces;
    style.auto_indent = editor_settings.auto_indent;
    // Clamped rather than trusted: ImGui reads 0 as "keep the current size", so
    // a hand-edited settings file could otherwise leave the editor at whatever
    // size the surrounding UI happens to use.
    style.font_size =
        std::clamp(editor_settings.font_size, kMinEditorFontSize, kMaxEditorFontSize);
    style.syntax_highlight = editor_settings.syntax_highlight;
    style.autocomplete = editor_settings.autocomplete;
    // Settings keep colors as 0xRRGGBB so the TOML stays readable; the editor
    // wants them packed the way ImGui draws with, opaque.
    for (std::size_t i = 0; i < kTokenKindCount; ++i) {
        const std::uint32_t rgb = editor_settings.syntax_colors.rgb[i];
        style.syntax_colors[i] = IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 0xFF);
    }
    style.error_color = theme_u32(app.theme().diagnostic(Severity::Error));
    style.warning_color = theme_u32(app.theme().diagnostic(Severity::Warning));
    style.info_color = theme_u32(app.theme().diagnostic(Severity::Info));

    // Collected inside the loop and applied after it; see TabMenuRequest.
    TabMenuRequest request;

    // What "to the left" and "to the right" are measured against: the order the
    // tabs were shown in on the last frame that drew them, which the session
    // holds. ImGui only reports it from inside BeginTabBar/EndTabBar, after the
    // context menu has already been drawn, so it is recorded there and read back
    // here. A frame old is close enough - a tab cannot be dragged and a menu
    // item picked in the same one.
    const std::string& session_key = app.active_session_key();
    std::vector<std::string> order = app.tab_order();
    if (order.empty()) {
        for (const auto& doc : app.documents()) {
            if (doc.open_in_editor) order.push_back(doc.id);
        }
    }

    // Which tab the bar has selected, and whether it got there from a reveal
    // request. The editor for it is drawn after the bar rather than inside the
    // tab item, because the bar now sits in a narrow table cell and its contents
    // must not be confined to it.
    Document* selected = nullptr;
    bool selected_revealing = false;

    // The "add" button lives beside the tab bar rather than in it. A trailing
    // tab item shares the tab bar's scrolling space, so as soon as the shader
    // tabs overflow the panel the button scrolls out of reach - which is exactly
    // the moment someone is most likely to be adding another shader. The table
    // reserves its width, so the bar can never grow underneath it. The way to
    // the node graph shares the column, for the same reason.
    const ThemeInk ink(app.theme());
    const float add_width = ImGui::GetFrameHeight();
    const float tools_width = add_width + ImGui::GetStyle().ItemSpacing.x + button_width("Graph");
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0.0f, 0.0f));
    const bool row = ImGui::BeginTable("##shader_tab_row", 3, ImGuiTableFlags_SizingFixedFit);
    ImGui::PopStyleVar();
    if (row) {
        ImGui::TableSetupColumn("##list", ImGuiTableColumnFlags_WidthFixed, add_width);
        ImGui::TableSetupColumn("##tabs", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##add", ImGuiTableColumnFlags_WidthFixed, tools_width);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();

        // Every open tab by its full name, stage and all, for when the row has
        // scrolled some of them out of sight. ImGui's own list button would
        // read the labels, and those no longer carry the stage - it is drawn
        // beside the name instead - so two shaders called "sprite" would read
        // the same there. Picking one selects its tab, which scrolls the bar
        // to it.
        ImGui::PushStyleColor(ImGuiCol_Button, with_alpha(ink.raised, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, with_alpha(ink.raised, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ink.muted);
        const bool list = ImGui::ArrowButton("##tab_list", ImGuiDir_Down);
        ImGui::PopStyleColor(3);
        ImGui::SetItemTooltip("Open shaders");
        if (list) ImGui::OpenPopup("##tab_list_menu");
        if (ImGui::BeginPopup("##tab_list_menu")) {
            const Document* front = app.active_document();
            for (const std::string& id : order) {
                const Document* doc = app.find_document(id);
                if (doc == nullptr || !doc->open_in_editor) continue;
                const std::string label = display_name(*doc) + " (" + stage_label(doc->stage) +
                                          ")" + (doc->dirty ? " *" : "") + "##" + doc->id;
                if (ImGui::Selectable(label.c_str(), front != nullptr && front->id == doc->id)) {
                    app.reveal(doc->id, 0);
                }
            }
            ImGui::EndPopup();
        }
        ImGui::TableNextColumn();
    }

    // Each open tab's shader id paired with the ImGui id of its tab, so the
    // order ImGui lays them out in can be read back below. The tab id has to be
    // taken here, inside the bar: it is hashed against the id stack, and the
    // popup the context menu draws in has a stack of its own.
    std::vector<std::pair<std::string, ImGuiID>> submitted;

    // Which tab was in front last frame, so the others can be drawn in the
    // quieter ink before BeginTabItem has said which one is selected now.
    const Document* current = app.active_document();
    const std::string current_id = current ? current->id : std::string();

    std::optional<PillTabStyle> pill_style(std::in_place, ink);
    if (ImGui::BeginTabBar("##shaders", ImGuiTabBarFlags_Reorderable |
                                            ImGuiTabBarFlags_FittingPolicyScroll)) {
        for (auto& doc : app.documents()) {
            if (!doc.open_in_editor) continue;
            // Everything after "###" is the tab's ImGui id and nothing before it
            // is. Keying on the shader id rather than on the text keeps two tabs
            // apart when they show the same name in different languages, and
            // stops the tab from being torn down and rebuilt - losing its place
            // in the row - the moment the unsaved marker changes the label.
            //
            // The session key goes in too, because this is one tab bar shared by
            // every open project. Without it, two projects that both contain a
            // "plasma_frag" would share that tab: switching between them would
            // hand the second project the first one's position in the row, and
            // that wrong order is now something that gets written to disk.
            //
            // The stage and the state are not in the label's text: the stage is
            // drawn smaller and dimmer after the name, and unsaved edits and
            // compile problems are the dot after that (see tab_status_color).
            // The label only reserves the room they are drawn into.
            //
            // The dot can only say one thing, and a compile problem outranks
            // unsaved edits. When it is saying the problem, the name carries
            // the unsaved marker it always had, so a tab in the background
            // never hides that it holds edits.
            const ImU32 dot = tab_status_color(doc, ink);
            const std::string name =
                display_name(doc) + (doc.dirty && dot != ink.accent ? " *" : "");
            const std::string label = name + tab_label_padding(stage_label(doc.stage)) + "###" +
                                      session_key + '\x1f' + doc.id;
            ImGuiTabItemFlags flags = ImGuiTabItemFlags_None;
            // Someone clicked a diagnostic for this shader: pull its tab to the
            // front so the scroll below lands somewhere the user can see.
            const bool revealing = app.reveal_request_id() == doc.id;
            if (revealing) flags |= ImGuiTabItemFlags_SetSelected;

            // p_open puts the close cross on the tab. It closes the tab and
            // nothing else - the shader stays in the project, which is why it
            // needs no confirmation even with unsaved edits in the buffer.
            bool keep_open = true;
            const bool in_front = doc.id == current_id;
            if (!in_front) ImGui::PushStyleColor(ImGuiCol_Text, ink.muted);
            const bool tab_open = ImGui::BeginTabItem(label.c_str(), &keep_open, flags);
            if (!in_front) ImGui::PopStyleColor();
            submitted.emplace_back(doc.id, ImGui::GetItemID());
            if (!keep_open) request.close.push_back(doc.id);
            draw_tab_decorations(name, stage_label(doc.stage), tab_open, dot, ink);

            // Before anything inside the tab is submitted, so the menu is
            // attached to the tab button itself - and outside the `tab_open`
            // test, because a tab the user has not switched to is exactly the
            // one they are most likely to right-click.
            // Two shaders of one name and stage in different languages read the
            // same on a tab, so the file - which is never ambiguous - is one
            // hover away.
            //
            // Both drawn with the pill style set aside: they are windows of
            // their own, and should read like every other menu and tooltip.
            pill_style->suspend();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", doc.path.generic_string().c_str());
            }

            draw_shader_tab_menu(app, doc, order, request);
            pill_style->resume();

            if (tab_open) {
                selected = &doc;
                selected_revealing = revealing;
                ImGui::EndTabItem();
            }
        }

        // Before EndTabBar, which is the last moment ImGui will say what order it
        // put the tabs in. Handed to the session, which persists it and hands it
        // back on the next frame - and on the next run of the application.
        //
        // Only tabs submitted this frame are recorded: on the frame a project
        // tab switch happens, the bar still holds the previous project's tabs,
        // which ImGui collects on the frame after.
        if (const ImGuiTabBar* bar = ImGui::GetCurrentTabBar()) {
            std::vector<std::string> shown;
            shown.reserve(submitted.size());
            for (int i = 0; i < bar->Tabs.Size; ++i) {
                const ImGuiID id = bar->Tabs[i].ID;
                const auto it = std::find_if(
                    submitted.begin(), submitted.end(),
                    [id](const std::pair<std::string, ImGuiID>& p) { return p.second == id; });
                if (it != submitted.end()) shown.push_back(it->first);
            }
            app.set_tab_order(std::move(shown));
        }
        ImGui::EndTabBar();
    }
    pill_style.reset();

    if (row) {
        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Button, with_alpha(ink.raised, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, with_alpha(ink.raised, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ink.muted);
        const bool add = ImGui::Button("+", ImVec2(add_width, 0.0f));
        ImGui::PopStyleColor(3);
        if (add) ImGui::OpenPopup("##add_shader_menu");
        // Taken before the tooltip, which begins and ends a window of its own
        // between here and the menu below.
        const ImVec2 menu_anchor = ImGui::GetItemRectMax();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add or open a shader");

        // Anchored to the button's bottom-right corner and grown leftwards: the
        // button sits at the right edge of the panel, so a menu opening rightward
        // from the pointer would immediately be pushed back by the viewport.
        // Safe to set unconditionally - BeginPopup consumes it either way.
        ImGui::SetNextWindowPos(menu_anchor, ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        if (ImGui::BeginPopup("##add_shader_menu")) {
            if (ImGui::MenuItem("Create new shader", "Ctrl+Shift+N")) request.new_shader = true;
            ImGui::Separator();
            draw_open_closed_items(app, request);
            ImGui::EndPopup();
        }

        // The node graph for the shader in front. Raised rather than toggled:
        // picking it when it is already open means "show me", as View > Graph
        // does.
        ImGui::SameLine();
        if (ghost_button("Graph", ink)) {
            app.show_graph = true;
            app.request_panel_focus("Graph");
        }
        ImGui::SetItemTooltip("Open the node graph for the shader in front");
        ImGui::EndTable();
    }

    if (!selected) {
        // Every tab closed. The shaders are all still there - this is the one
        // state where the way back has to be spelled out rather than implied.
        ImGui::Spacing();
        ImGui::TextDisabled("No shader open. The project's shaders are all still here.");
        ImGui::Spacing();
        for (const std::string& id : app.closed_documents()) {
            if (const Document* doc = app.find_document(id)) {
                const std::string label =
                    display_name(*doc) + " (" + stage_label(doc->stage) + ")" +
                    (doc->dirty ? " *" : "");
                if (ImGui::Selectable(label.c_str())) request.open.push_back(id);
            }
        }
    }

    // Set by the editor below. The zoom shortcuts are routed globally, so while
    // the completion list is up they have to stand down - Ctrl+= and Ctrl+- are
    // otherwise indistinguishable from someone typing into an open popup.
    bool completion_showing = false;

    if (selected) {
        Document& doc = *selected;
        app.focus_document(doc.id);
        draw_document_header(app, doc, ink);
        ImGui::Dummy(ImVec2(0.0f, design_px(2.0f)));

        // Per document: a project can mix an HLSL vertex shader with a GLSL
        // fragment one, and the two color against different words.
        if (const ShaderDesc* desc = app.project().find_shader(doc.id)) {
            style.language = app.project().language_of(*desc);
        }

        // What the editor cannot work out from the buffer alone: the stage
        // decides which semantics and which register spaces are the right ones,
        // and the last successful compile is where the resource names come from.
        // Reflection from a compile that failed is deliberately still used - the
        // names it found are the ones the user is most likely reaching for while
        // fixing whatever broke.
        DocumentInputs document;
        document.stage = doc.stage;
        document.reflection = &doc.reflection;
        document.read_only = doc.generated_by_graph;
        for (const auto& [name, expression] : app.project().macros) {
            (void)expression;
            document.macros.push_back(name);
        }

        TextEditor& editor = editors()[editor_key(app.active_session_key(), doc.id)];
        if (selected_revealing) {
            if (app.reveal_request_line() > 0) editor.request_scroll_to(app.reveal_request_line());
            app.clear_reveal_request();
        }
        const std::string widget_id = "##text_" + doc.id;
        if (editor.draw(widget_id.c_str(), doc.text, doc.diagnostics, style, document)) {
            app.mark_dirty(doc);
        }
        doc.cursor_line = editor.cursor_line();
        doc.cursor_column = editor.cursor_column();
        completion_showing = editor.completion_open();
    }

    // Last, because every one of these changes the document vector that
    // everything above walks and holds a pointer into.
    for (const std::string& id : request.close) app.close_document(id);
    for (const std::string& id : request.open) app.open_document(id);
    if (!request.duplicate.empty()) {
        std::string error;
        if (!app.duplicate_shader(request.duplicate, error)) app.log(Severity::Error, error);
    }
    if (request.new_shader) app.prompt_new_shader();

    // After the tabs, so the focus test covers the whole panel including the
    // text box inside it, and a click on a tab does not lose the zoom keys.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !completion_showing) {
        bool reset = false;
        const float delta = zoom_delta_from_keyboard(reset);
        if (delta != 0.0f || reset) apply_editor_zoom(app, delta, reset);
    }
}

}  // namespace ssstudio::gui
