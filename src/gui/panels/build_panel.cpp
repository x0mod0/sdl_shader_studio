#include <algorithm>
#include <cfloat>
#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <imgui.h>

#include "ssstudio/build.h"

#include "app.h"
#include "fonts.h"
#include "panel_common.h"
#include "ssstudio/codegen.h"
#include "ssstudio/keys.h"
#include "widgets.h"

namespace ssstudio::gui {
namespace {

void copy_button(const char* label, const std::string& text) {
    if (ImGui::SmallButton(label)) ImGui::SetClipboardText(text.c_str());
}

ImVec4 severity_color(Severity severity, const ResolvedTheme& theme) {
    return theme_vec4(theme.diagnostic(severity));
}

// One diagnostic as a full-width row. The text is drawn over a Selectable sized
// to its wrapped height rather than used as the Selectable's own label, because
// a label is clipped to one line - which is how everything past the file name in
// a compiler message used to disappear. Returns true when the row is clicked.
//
// `copy_all` is offered in the context menu when it is not empty.
bool diagnostic_row(int id, const std::string& text, Severity severity, bool selected,
                    const std::string& copy_all, bool& out_clear,
                    const ResolvedTheme& theme) {
    ImGui::PushID(id);
    // A compiler message with a source excerpt under it, in the code font, so
    // its caret lines up with the column it points at.
    const bool excerpt = text.find('\n') != std::string::npos;
    if (excerpt) ImGui::PushFont(mono_font(), type_size(12.0f));
    const float wrap_width = ImGui::GetContentRegionAvail().x;
    const ImVec2 text_size = ImGui::CalcTextSize(text.c_str(), nullptr, false, wrap_width);
    const ImVec2 row_start = ImGui::GetCursorScreenPos();

    const bool clicked = ImGui::Selectable("##row", selected, ImGuiSelectableFlags_AllowOverlap,
                                           ImVec2(0.0f, text_size.y));
    if (ImGui::BeginPopupContextItem("##row_menu")) {
        if (ImGui::MenuItem("Copy message")) ImGui::SetClipboardText(text.c_str());
        if (!copy_all.empty() && ImGui::MenuItem("Copy all messages")) {
            ImGui::SetClipboardText(copy_all.c_str());
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Clear all messages")) out_clear = true;
        ImGui::EndPopup();
    }

    ImGui::SetCursorScreenPos(row_start);
    ImGui::PushStyleColor(ImGuiCol_Text, severity_color(severity, theme));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_width);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    if (excerpt) ImGui::PopFont();
    ImGui::PopID();
    return clicked;
}

// A build diagnostic names a source file; the editor is addressed by shader id.
// Compared leniently: the manifest stores relative paths, the compiler reports
// whatever it was handed.
const Document* document_for_file(App& app, const std::string& file) {
    if (file.empty()) return nullptr;
    const std::filesystem::path wanted(file);
    for (const auto& doc : app.documents()) {
        if (doc.path == wanted) return &doc;
    }
    for (const auto& doc : app.documents()) {
        if (doc.path.filename() == wanted.filename()) return &doc;
    }
    return nullptr;
}

// What went wrong and where to go to fix it. Shown in place of a bare
// "Build failed." so the panel that reports the failure also points at it.
void draw_failure_report(App& app, const BuildReport& report) {
    ImGui::TextColored(severity_color(Severity::Error, app.theme()), "Build failed.");

    std::vector<const Diagnostic*> errors;
    for (const auto& d : report.diagnostics) {
        if (d.severity == Severity::Error) errors.push_back(&d);
    }
    if (errors.empty()) {
        ImGui::TextWrapped(
            "The build stopped without reporting an error. The Diagnostics panel has the full "
            "log for this run.");
        return;
    }

    int with_location = 0;
    for (const Diagnostic* d : errors) {
        if (!d->file.empty()) ++with_location;
    }
    if (with_location > 0) {
        ImGui::TextWrapped("%d error(s). Click one to open that line in the editor, fix it, "
                           "then build again.",
                           static_cast<int>(errors.size()));
    } else {
        ImGui::TextWrapped("%d error(s). These are project-level problems: check the shader "
                           "list and the build profile in Settings.",
                           static_cast<int>(errors.size()));
    }

    std::string all;
    for (const Diagnostic* d : errors) {
        all += d->format();
        all += '\n';
    }

    // The same right-click gesture the Diagnostics panel offers, because this is
    // the same set of messages seen from the other end.
    bool clear = false;
    for (int i = 0; i < static_cast<int>(errors.size()); ++i) {
        const Diagnostic& d = *errors[static_cast<std::size_t>(i)];
        const Document* doc = document_for_file(app, d.file);
        // Say which shader it is, not just which file: the shader id is what the
        // manifest, the editor tabs and the generated enum all use.
        const std::string text =
            (doc != nullptr ? doc->id + " - " : std::string()) + d.format();
        if (diagnostic_row(i, text, d.severity, false, all, clear, app.theme()) && doc != nullptr) {
            app.reveal(doc->id, d.line);
        }
    }
    // After the loop: clearing empties the vectors `errors` points into.
    if (clear) app.clear_diagnostics();
}

void draw_snippets(App& app) {
    const BuildReport& report = app.last_report();
    if (report.shaders.empty()) return;

    static int selected = 0;
    static int flavor = 1;  // default to the C++ RAII flavor
    selected = std::clamp(selected, 0, static_cast<int>(report.shaders.size()) - 1);

    const float shader_width = fitted_width(220.0f, 90.0f);

    FlowLayout row;
    row.next(labeled_width("shader", shader_width));
    if (ImGui::BeginCombo(left_label("Shader", shader_width).c_str(), report.shaders[selected].id.c_str())) {
        for (int i = 0; i < static_cast<int>(report.shaders.size()); ++i) {
            if (ImGui::Selectable(report.shaders[i].id.c_str(), i == selected)) selected = i;
        }
        ImGui::EndCombo();
    }
    row.next(checkbox_width("C"));
    ImGui::RadioButton("C", &flavor, 0);
    row.next(checkbox_width("C++ (RAII)"));
    ImGui::RadioButton("C++ (RAII)", &flavor, 1);

    const PackShader& shader = report.shaders[selected];
    const SnippetFlavor snippet_flavor = flavor == 0 ? SnippetFlavor::C : SnippetFlavor::CppRaii;

    auto block = [&](const char* title, const std::string& code) {
        if (code.empty()) return;
        ImGui::SeparatorText(title);
        copy_button((std::string("Copy##") + title).c_str(), code);
        MonoScope mono(12.0f);
        ImGui::TextUnformatted(code.c_str());
    };

    block("Create", snippet_create_shader(shader, report.gen_info, snippet_flavor));
    if (shader.stage == Stage::Compute) {
        block("Dispatch", snippet_compute_dispatch(shader, report.gen_info, snippet_flavor));
    }
    if (shader.stage == Stage::Vertex) {
        block("Vertex input state", snippet_vertex_input_state(shader, report.gen_info));
    }
    if (!shader.reflection.uniform_blocks.empty()) {
        block("Uniform struct", snippet_uniform_struct(shader));
        block("Push uniforms", snippet_uniform_push(shader));
    }
    if (shader.reflection.num_samplers() > 0) {
        block("Samplers", snippet_samplers(shader));
    }
}

}  // namespace

void draw_build_panel(App& app) {
    PanelScope panel("Build", &app.show_build);
    if (!panel) return;
    if (!app.project_open()) {
        ImGui::TextDisabled("No project open.");
        return;
    }
    const ThemeInk ink(app.theme());

    // Per project: a profile name from one project rarely exists in another.
    std::string& profile_choice = app.build_profile_choice();
    auto& profiles = app.project().profiles;
    const bool known = std::any_of(profiles.begin(), profiles.end(),
                                   [&](const BuildProfile& p) { return p.name == profile_choice; });
    if (!known) profile_choice = profiles.empty() ? std::string() : profiles.front().name;

    const float profile_width = fitted_width(150.0f, 80.0f);

    FlowLayout row;
    row.next(labeled_width("profile", profile_width));
    ImGui::PushStyleColor(ImGuiCol_Text, ink.muted);
    const std::string profile_id = left_label("Profile", profile_width);
    ImGui::PopStyleColor();
    if (ImGui::BeginCombo(profile_id.c_str(), profile_choice.c_str())) {
        for (const auto& profile : profiles) {
            if (ImGui::Selectable(profile.name.c_str(), profile.name == profile_choice)) {
                profile_choice = profile.name;
            }
        }
        ImGui::EndCombo();
    }

    // One build runs at a time across every open project, so a build started
    // from another tab disables these too - and says so, rather than looking
    // broken. Dry run first and Build last, so the one filled button is the
    // one at the end of the row, where the eye finishes.
    ImGui::BeginDisabled(app.build_busy());
    row.next(button_width("Dry run"));
    if (ImGui::Button("Dry run")) app.start_build(profile_choice, true);
    row.next(button_width("Build"));
    if (primary_button("Build", ink)) app.start_build(profile_choice, false);
    ImGui::EndDisabled();
    if (app.build_busy() && !app.build_running()) {
        const std::string note = "building " + app.building_project_name();
        row.next(text_width(note.c_str()));
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", note.c_str());
    }

    if (BuildProfile* profile = app.project().find_profile(profile_choice)) {
        // What the profile does, in the words project.toml uses for it.
        {
            MonoScope mono(11.0f);
            char summary[256];
            std::snprintf(summary, sizeof(summary), "%s %s %s \xC2\xB7 compression %s \xC2\xB7 keys %s",
                          format_mask_to_string(profile->formats).c_str(), arrow_glyph(),
                          profile->output_dir.c_str(),
                          std::string(to_string(profile->compression)).c_str(),
                          std::string(to_string(profile->key_strategy)).c_str());
            row.next(text_width(summary));
            ImGui::AlignTextToFramePadding();
            colored_text(summary, ink.muted);
        }

        // The only part of a build profile the app edits. The rest is hand-
        // written in project.toml, and this is here because it is the one option
        // whose result is a filename you have to be able to see to choose.
        bool profile_changed = false;
        if (ImGui::TreeNode("Per-shader binaries")) {
            FlowLayout options;
            options.next(checkbox_width("Write each shader as its own file"));
            profile_changed |= ImGui::Checkbox("Write each shader as its own file",
                                               &profile->emit.shader_binaries);
            ImGui::BeginDisabled(!profile->emit.shader_binaries);

            char extension[32];
            std::snprintf(extension, sizeof(extension), "%s", profile->shader_extension.c_str());
            const float extension_width = fitted_width(90.0f, 60.0f);
            options.next(labeled_width("Extension", extension_width));
            ImGui::PushStyleColor(ImGuiCol_Text, ink.muted);
            const std::string extension_id = left_label("Extension", extension_width);
            ImGui::PopStyleColor();
            bool extension_edited = false;
            {
                MonoScope mono(12.0f);
                extension_edited = ImGui::InputTextWithHint(extension_id.c_str(), ".bin", extension,
                                                            sizeof(extension));
            }
            if (extension_edited) {
                profile->shader_extension = extension;
                profile_changed = true;
            }

            // What the setting produces, spelled out with a real shader from
            // this project: an extension field with no example beside it is a
            // guess about how the name will be put together.
            const auto& shaders = app.project().shaders;
            if (!shaders.empty()) {
                const bool single_format = (profile->formats & (profile->formats - 1)) == 0;
                const std::string example = shader_binary_filename(
                    shader_binary_stem(shaders.front().path),
                    static_cast<ShaderFormat>(profile->formats & ~(profile->formats - 1)),
                    single_format, profile->shader_extension);
                MonoScope mono(11.0f);
                const std::string line = shaders.front().path.filename().string() + " " +
                                         arrow_glyph() + " " + example;
                options.next(text_width(line.c_str()));
                ImGui::AlignTextToFramePadding();
                colored_text(line, ink.muted);
                if (!single_format) {
                    ImGui::TextDisabled(
                        "one file per format, since this profile builds more than one");
                }
            }
            ImGui::EndDisabled();
            ImGui::TreePop();
        }
        if (profile_changed) {
            Diagnostics diags;
            save_project(app.project(), diags);
            for (const auto& d : diags) app.log(d.severity, d.format());
        }
    }

    if (app.build_running()) {
        const auto progress = app.build_progress_snapshot();
        ImGui::Separator();
        if (!progress.empty()) {
            const BuildProgress& last = progress.back();
            const float fraction =
                last.total > 0 ? static_cast<float>(last.done) / static_cast<float>(last.total) : 0.0f;
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ink.accent);
            ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 0),
                               (std::string(to_string(last.stage)) + ": " + last.message).c_str());
            ImGui::PopStyleColor();
        } else {
            ImGui::TextDisabled("starting...");
        }
        return;
    }

    const BuildReport& report = app.last_report();
    if (report.artifacts.empty() && report.diagnostics.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("No build yet.");
        return;
    }

    ImGui::Separator();

    // The verdict, and beside it which of the three views of the result is
    // showing.
    static int view = 0;
    const char* views[] = {"Artifacts", "Keys", "Snippets"};
    FlowLayout verdict;
    if (report.ok) {
        const std::string built = "Built " + std::to_string(report.stats.shader_count) +
                                  (report.stats.shader_count == 1 ? " shader" : " shaders");
        char numbers[128];
        std::snprintf(numbers, sizeof(numbers), "%s \xC2\xB7 %s bytes \xC2\xB7 %.2f s",
                      format_mask_to_string(report.stats.format_mask).c_str(),
                      grouped(report.stats.total_bytes).c_str(), report.seconds);
        verdict.next(design_px(12.0f) + text_width(built.c_str()));
        ImGui::AlignTextToFramePadding();
        status_dot(ink.ok);
        ImGui::SameLine(0.0f, design_px(6.0f));
        colored_text(built, ink.ok);
        MonoScope mono(11.5f);
        verdict.next(text_width(numbers));
        ImGui::AlignTextToFramePadding();
        colored_text(numbers, ink.soft);
    } else {
        draw_failure_report(app, report);
    }
    const float segments = segmented_control_width(views, 3);
    verdict.next(segments);
    if (report.ok && ImGui::GetCursorPosX() + segments < ImGui::GetContentRegionMax().x) {
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - segments);
    }
    segmented_control("##build_view", views, 3, view, ink);

    const ImGuiTableFlags table_flags =
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH;
    ImGui::PushStyleColor(ImGuiCol_TableBorderLight, ink.subtle);
    if (view == 0) {
        if (ImGui::BeginTable("##artifacts", 3, table_flags)) {
            ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthStretch, 0.15f);
            ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch, 0.65f);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch, 0.20f);
            caps_headers_row(ink, 1u << 2);
            MonoScope mono(12.0f);
            for (const auto& artifact : report.artifacts) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                colored_text(artifact.kind, ink.muted);
                ImGui::TableNextColumn();
                // Shown from the project's own directory, which is all of it
                // that says anything; copied whole, because a path on the
                // clipboard is on its way somewhere that does not know where the
                // project is.
                const std::string path = artifact.path.generic_string();
                std::error_code ec;
                const std::filesystem::path relative =
                    std::filesystem::relative(artifact.path, app.project().root, ec);
                const std::string shown = !ec && !relative.empty() &&
                                                  relative.native().rfind("..", 0) != 0
                                              ? relative.generic_string()
                                              : path;
                ImGui::PushID(path.c_str());
                if (ImGui::Selectable(shown.c_str())) ImGui::SetClipboardText(path.c_str());
                ImGui::PopID();
                if (shown != path) ImGui::SetItemTooltip("%s", path.c_str());
                ImGui::TableNextColumn();
                // Readable at a glance, exact on hover: the byte count is what a
                // loader's buffer has to hold.
                const std::string size = human_bytes(artifact.bytes);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                     std::max(0.0f, ImGui::GetContentRegionAvail().x -
                                                        text_width(size.c_str())));
                colored_text(size, ink.soft);
                ImGui::SetItemTooltip("%llu bytes", static_cast<unsigned long long>(artifact.bytes));
            }
            ImGui::EndTable();
        }
        FontScope small(nullptr, 12.0f);
        ImGui::TextDisabled("Click a path to copy it.");
    } else if (view == 1) {
        if (ImGui::BeginTable("##keys", 3, table_flags)) {
            ImGui::TableSetupColumn("Shader");
            ImGui::TableSetupColumn("Enum");
            ImGui::TableSetupColumn("Key");
            caps_headers_row(ink);
            MonoScope mono(12.0f);
            for (const auto& shader : report.shaders) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(shader.id.c_str());
                ImGui::TableNextColumn();
                colored_text(enum_name(report.gen_info.enum_prefix, shader.id), ink.soft);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", shader.key);
            }
            ImGui::EndTable();
        }
    } else {
        draw_snippets(app);
    }
    ImGui::PopStyleColor();

}

namespace {

/// One diagnostic as the Diagnostics panel lists it: the pieces it is shown
/// in, and the one-line form every copy puts on the clipboard.
struct DiagnosticRow {
    Severity severity = Severity::Info;
    std::string origin;  // shader id, "build", or empty for app log entries
    int line = 0;
    /// Exactly what the text view shows and every copy takes.
    std::string text;
    /// Which file, by name; the path is in `text`.
    std::string place;
    /// ":33:52", or empty when the diagnostic has no position.
    std::string position;
    std::string code;
    std::string message;
};

/// How wide the fixed columns are, measured across every row so they line up.
struct DiagnosticColumns {
    float severity = 0.0f;
    float location = 0.0f;
    float code = 0.0f;
};

const char* severity_word(Severity severity) {
    switch (severity) {
        case Severity::Error: return "error";
        case Severity::Warning: return "warning";
        case Severity::Info: return "info";
    }
    return "info";
}

/// One diagnostic as a row of columns - dot, severity, where, code, message -
/// with the message wrapping in whatever width is left. A compiler message
/// that carries a source excerpt (a line of code and a caret under it) is set
/// in the code font, where the caret lands under the character it means.
/// Returns true when the row is clicked.
bool diagnostic_grid_row(int id, const DiagnosticRow& row, const DiagnosticColumns& columns,
                         bool selected, const std::string& copy_all, bool& out_clear,
                         const ThemeInk& ink) {
    ImGui::PushID(id);
    const float pad = design_px(10.0f);
    const float gap = design_px(10.0f);
    const float dot = design_px(8.0f);
    const float width = ImGui::GetContentRegionAvail().x;
    const float message_x =
        pad + dot + gap + columns.severity + gap + columns.location + gap +
        (columns.code > 0.0f ? columns.code + gap : 0.0f);
    const float message_width = std::max(design_px(60.0f), width - message_x - pad);

    const bool excerpt = row.message.find('\n') != std::string::npos;
    ImFont* message_font = excerpt && mono_font() != nullptr ? mono_font() : ImGui::GetFont();
    const float message_size = excerpt ? type_pixels(12.0f) : ImGui::GetFontSize();
    const ImVec2 message_extent = message_font->CalcTextSizeA(
        message_size, FLT_MAX, message_width, row.message.c_str());
    const float line_height = ImGui::GetTextLineHeight();
    const float height = std::max(design_px(32.0f), message_extent.y + design_px(14.0f));

    // A Selectable for the gestures - click, keyboard navigation, the context
    // menu - with its own fill turned off, so the row can be drawn rounded and
    // tinted by severity beneath the columns.
    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Header, with_alpha(ink.raised, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, with_alpha(ink.raised, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, with_alpha(ink.raised, 0.0f));
    const bool clicked = ImGui::Selectable("##row", selected, ImGuiSelectableFlags_AllowOverlap,
                                           ImVec2(0.0f, height));
    ImGui::PopStyleColor(3);
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::BeginPopupContextItem("##row_menu")) {
        if (ImGui::MenuItem("Copy message")) ImGui::SetClipboardText(row.text.c_str());
        if (!copy_all.empty() && ImGui::MenuItem("Copy all messages")) {
            ImGui::SetClipboardText(copy_all.c_str());
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Clear all messages")) out_clear = true;
        ImGui::EndPopup();
    }

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const float rounding = design_px(5.0f);
    if (row.severity == Severity::Error) {
        draw_list->AddRectFilled(min, max, with_alpha(ink.error, 0.07f), rounding);
    }
    if (selected) {
        draw_list->AddRectFilled(min, max, ink.accent_muted, rounding);
    } else if (hovered) {
        draw_list->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_HeaderHovered), rounding);
    }

    // The columns sit on the row's first line; a wrapped message runs on
    // below them.
    const ImU32 ink_for = ink.severity(row.severity);
    const float text_y = std::max(start.y + (height - message_extent.y) * 0.5f,
                                  start.y + design_px(7.0f));
    float x = start.x + pad;
    draw_list->AddCircleFilled(ImVec2(x + dot * 0.5f, text_y + line_height * 0.5f), dot * 0.5f,
                               ink_for);
    x += dot + gap;
    {
        FontScope small(nullptr, 12.0f);
        draw_list->AddText(ImVec2(x, text_y + (line_height - ImGui::GetTextLineHeight()) * 0.5f),
                           ink_for, severity_word(row.severity));
    }
    x += columns.severity + gap;
    {
        MonoScope mono(12.0f);
        const float y = text_y + (line_height - ImGui::GetTextLineHeight()) * 0.5f;
        const std::string place = fit_text(row.place, columns.location -
                                                           ImGui::CalcTextSize(row.position.c_str()).x);
        draw_list->AddText(ImVec2(x, y), ink.text, place.c_str());
        draw_list->AddText(ImVec2(x + ImGui::CalcTextSize(place.c_str()).x, y), ink.muted,
                           row.position.c_str());
    }
    x += columns.location + gap;
    if (columns.code > 0.0f) {
        MonoScope mono(11.0f);
        draw_list->AddText(ImVec2(x, text_y + (line_height - ImGui::GetTextLineHeight()) * 0.5f),
                           ink.muted, row.code.c_str());
        x += columns.code + gap;
    }
    draw_list->AddText(message_font, message_size, ImVec2(start.x + message_x, text_y), ink.text,
                       row.message.c_str(), nullptr, message_width);

    ImGui::PopID();
    return clicked;
}

}  // namespace

void draw_diagnostics_panel(App& app) {
    PanelScope panel("Diagnostics", &app.show_diagnostics);
    if (!panel) return;
    const ThemeInk ink(app.theme());

    static bool show_errors = true;
    static bool show_info = true;
    static bool show_warnings = true;
    static bool as_text = false;
    static int selected = -1;

    // One flattened list feeds both views and every copy path, so what lands on
    // the clipboard is exactly what is on screen, filters included. Counted
    // before filtering, so a chip that is off still says what it is hiding.
    std::vector<DiagnosticRow> rows;
    int errors = 0, warnings = 0, infos = 0;
    auto collect = [&](const Diagnostic& d, const std::string& origin) {
        if (d.severity == Severity::Error) ++errors;
        if (d.severity == Severity::Warning) ++warnings;
        if (d.severity == Severity::Info) ++infos;
        if (d.severity == Severity::Error && !show_errors) return;
        if (d.severity == Severity::Info && !show_info) return;
        if (d.severity == Severity::Warning && !show_warnings) return;
        DiagnosticRow row;
        row.severity = d.severity;
        row.origin = origin;
        row.line = d.line;
        row.text = (origin.empty() ? std::string() : origin + ": ") + d.format();
        if (!d.file.empty()) {
            row.place = std::filesystem::path(d.file).filename().string();
        } else {
            row.place = origin.empty() ? std::string("app") : origin;
        }
        if (d.line > 0) {
            row.position = ":" + std::to_string(d.line);
            if (d.column > 0) row.position += ":" + std::to_string(d.column);
        }
        row.code = d.code;
        row.message = d.message;
        rows.push_back(std::move(row));
    };
    for (const auto& doc : app.documents()) {
        for (const auto& d : doc.diagnostics) collect(d, doc.id);
    }
    for (const auto& d : app.last_report().diagnostics) collect(d, "build");
    for (const auto& d : app.log_entries()) collect(d, std::string());

    auto joined = [&rows] {
        std::string out;
        for (const auto& row : rows) {
            out += row.text;
            out += '\n';
        }
        return out;
    };

    // --- the filter row ----------------------------------------------------
    FlowLayout filters;
    filters.next(toggle_chip_width("Errors", errors));
    toggle_chip("Errors", errors, ink.error, show_errors, ink);
    filters.next(toggle_chip_width("Warnings", warnings));
    toggle_chip("Warnings", warnings, ink.warn, show_warnings, ink);
    filters.next(toggle_chip_width("Info", infos));
    toggle_chip("Info", infos, ink.info, show_info, ink);
    // The text view trades the click-to-jump behaviour for a plain read-only
    // buffer, which is the only way to select part of a message with the mouse.
    filters.next(checkbox_width("As text"));
    ImGui::Checkbox("As text", &as_text);

    // The actions, against the right edge when the row has room for them.
    {
        float hint_width = 0.0f;
        {
            FontScope small(nullptr, 11.0f);
            hint_width = ImGui::CalcTextSize("F7").x;
        }
        const float actions = button_width("Copy all") + ImGui::GetStyle().ItemSpacing.x +
                              button_width("Recompile all") + design_px(8.0f) + hint_width;
        filters.next(actions);
        if (ImGui::GetCursorPosX() + actions < ImGui::GetContentRegionMax().x) {
            ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - actions);
        }
        ImGui::BeginDisabled(rows.empty());
        if (ghost_button("Copy all", ink)) ImGui::SetClipboardText(joined().c_str());
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (button_with_hint("Recompile all", "F7", ink)) app.compile_all();
    }
    ImGui::Separator();

    if (rows.empty()) {
        // Not a blank panel: say that the silence is good news, and what it is
        // good news about.
        int clean = 0;
        double milliseconds = 0.0;
        for (const auto& doc : app.documents()) {
            if (!doc.compiled_ok) continue;
            ++clean;
            milliseconds += doc.last_compile_ms;
        }
        std::string detail;
        if (clean > 0) {
            char text[96];
            std::snprintf(text, sizeof(text), "\xC2\xB7 %d shader%s compiled cleanly in %.0f ms",
                          clean, clean == 1 ? "" : "s", milliseconds);
            detail = text;
        }
        const char* headline = "Nothing to report";
        const float gap = design_px(10.0f);
        const float dot = design_px(8.0f);
        const float total = dot + gap + text_width(headline) +
                            (detail.empty() ? 0.0f : gap + text_width(detail.c_str()));
        const ImVec2 room = ImGui::GetContentRegionAvail();
        ImGui::SetCursorPos(ImVec2(
            ImGui::GetCursorPosX() + std::max(0.0f, (room.x - total) * 0.5f),
            ImGui::GetCursorPosY() + std::max(0.0f, (room.y - ImGui::GetTextLineHeight()) * 0.5f)));
        status_dot(clean > 0 ? ink.ok : ink.muted, 8.0f);
        ImGui::SameLine(0.0f, gap);
        colored_text(headline, ink.text);
        if (!detail.empty()) {
            ImGui::SameLine(0.0f, gap);
            colored_text(detail, ink.muted);
        }
        return;
    }

    if (selected >= static_cast<int>(rows.size())) selected = -1;

    // Ctrl/Cmd+C copies the selected message, which is what the keyboard reflex
    // expects once a row is highlighted.
    const ImGuiIO& io = ImGui::GetIO();
    if (!as_text && selected >= 0 && (io.KeyCtrl || io.KeySuper) &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::IsKeyPressed(ImGuiKey_C, false)) {
        ImGui::SetClipboardText(rows[static_cast<std::size_t>(selected)].text.c_str());
    }

    if (as_text) {
        std::string all = joined();
        MonoScope mono(12.0f);
        ImGui::InputTextMultiline("##diagnostics_text", all.data(), all.size() + 1,
                                  ImVec2(-FLT_MIN, -FLT_MIN), ImGuiInputTextFlags_ReadOnly);
        return;
    }

    // The fixed columns, as wide as their widest entry, so every row lines up.
    DiagnosticColumns columns;
    {
        FontScope small(nullptr, 12.0f);
        columns.severity = ImGui::CalcTextSize("warning").x;
    }
    {
        MonoScope mono(12.0f);
        for (const auto& row : rows) {
            columns.location = std::max(
                columns.location, ImGui::CalcTextSize((row.place + row.position).c_str()).x);
        }
    }
    {
        MonoScope mono(11.0f);
        for (const auto& row : rows) {
            if (!row.code.empty()) {
                columns.code = std::max(columns.code, ImGui::CalcTextSize(row.code.c_str()).x);
            }
        }
    }
    // A long path must not take the message's room: past a third of the panel
    // the file name is cut instead.
    columns.location = std::min(columns.location, ImGui::GetContentRegionAvail().x * 0.34f);

    // Collected rather than acted on inside the loop: clearing empties the very
    // vectors the rows were built from.
    bool clear = false;

    const std::string all = joined();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(ImGui::GetStyle().ItemSpacing.x, design_px(2.0f)));
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const DiagnosticRow& row = rows[i];
        if (diagnostic_grid_row(static_cast<int>(i), row, columns, selected == static_cast<int>(i),
                                all, clear, ink)) {
            selected = static_cast<int>(i);
            if (!row.origin.empty() && row.origin != "build") app.reveal(row.origin, row.line);
        }
    }
    ImGui::PopStyleVar();

    // The same item from the empty space below the list, so the gesture works
    // wherever in the panel the pointer happens to be.
    if (ImGui::BeginPopupContextWindow("##diagnostics_menu",
                                       ImGuiPopupFlags_MouseButtonRight |
                                           ImGuiPopupFlags_NoOpenOverItems)) {
        if (ImGui::MenuItem("Copy all messages", nullptr, false, !all.empty())) {
            ImGui::SetClipboardText(all.c_str());
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Clear all messages")) clear = true;
        ImGui::EndPopup();
    }

    if (clear) {
        app.clear_diagnostics();
        selected = -1;
    }

    ImGui::Separator();
    FontScope small(nullptr, 12.0f);
    ImGui::TextDisabled("Click to jump to the line, right-click to copy or clear, Ctrl+C copies "
                        "the selected message.");

}

}  // namespace ssstudio::gui
