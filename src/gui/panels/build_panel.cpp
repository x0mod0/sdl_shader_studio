#include <algorithm>
#include <cfloat>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include <imgui.h>

#include "ssstudio/build.h"

#include "app.h"
#include "panel_common.h"
#include "ssstudio/codegen.h"
#include "ssstudio/keys.h"

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

    // Per project: a profile name from one project rarely exists in another.
    std::string& profile_choice = app.build_profile_choice();
    auto& profiles = app.project().profiles;
    const bool known = std::any_of(profiles.begin(), profiles.end(),
                                   [&](const BuildProfile& p) { return p.name == profile_choice; });
    if (!known) profile_choice = profiles.empty() ? std::string() : profiles.front().name;

    const float profile_width = fitted_width(180.0f, 80.0f);

    FlowLayout row;
    row.next(labeled_width("profile", profile_width));
    if (ImGui::BeginCombo(left_label("Profile", profile_width).c_str(), profile_choice.c_str())) {
        for (const auto& profile : profiles) {
            if (ImGui::Selectable(profile.name.c_str(), profile.name == profile_choice)) {
                profile_choice = profile.name;
            }
        }
        ImGui::EndCombo();
    }

    // One build runs at a time across every open project, so a build started
    // from another tab disables these too - and says so, rather than looking
    // broken.
    ImGui::BeginDisabled(app.build_busy());
    row.next(button_width("Build"));
    if (ImGui::Button("Build")) app.start_build(profile_choice, false);
    row.next(button_width("Dry run"));
    if (ImGui::Button("Dry run")) app.start_build(profile_choice, true);
    ImGui::EndDisabled();
    if (app.build_busy() && !app.build_running()) {
        const std::string note = "building " + app.building_project_name();
        row.next(text_width(note.c_str()));
        ImGui::TextDisabled("%s", note.c_str());
    }

    if (BuildProfile* profile = app.project().find_profile(profile_choice)) {
        ImGui::TextDisabled("%s -> %s, compression %s, keys %s",
                            format_mask_to_string(profile->formats).c_str(),
                            profile->output_dir.c_str(),
                            std::string(to_string(profile->compression)).c_str(),
                            std::string(to_string(profile->key_strategy)).c_str());

        // The only part of a build profile the app edits. The rest is hand-
        // written in project.toml, and this is here because it is the one option
        // whose result is a filename you have to be able to see to choose.
        bool profile_changed = false;
        if (ImGui::TreeNode("Per-shader binaries")) {
            profile_changed |= ImGui::Checkbox("Write each shader as its own file",
                                               &profile->emit.shader_binaries);
            ImGui::BeginDisabled(!profile->emit.shader_binaries);

            char extension[32];
            std::snprintf(extension, sizeof(extension), "%s", profile->shader_extension.c_str());
            if (ImGui::InputTextWithHint(left_label("Extension", fitted_width(140.0f, 60.0f)).c_str(), ".bin", extension, sizeof(extension))) {
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
                ImGui::TextDisabled("%s becomes %s", shaders.front().path.filename().string().c_str(),
                                    example.c_str());
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
            ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 0),
                               (std::string(to_string(last.stage)) + ": " + last.message).c_str());
        } else {
            ImGui::TextDisabled("starting...");
        }
        return;
    }

    const BuildReport& report = app.last_report();
    if (report.artifacts.empty() && report.diagnostics.empty()) {
        ImGui::TextDisabled("No build yet.");
        return;
    }

    ImGui::Separator();
    if (report.ok) {
        ImGui::TextColored(theme_vec4(app.theme().success()),
                           "%u shader(s), %s, %llu bytes in %.2fs", report.stats.shader_count,
                           format_mask_to_string(report.stats.format_mask).c_str(),
                           static_cast<unsigned long long>(report.stats.total_bytes),
                           report.seconds);
    } else {
        draw_failure_report(app, report);
    }

    if (ImGui::BeginTabBar("##buildtabs")) {
        if (ImGui::BeginTabItem("Artifacts")) {
            if (ImGui::BeginTable("##artifacts", 3, ImGuiTableFlags_RowBg |
                                                        ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthStretch, 0.15f);
                ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch, 0.65f);
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch, 0.20f);
                ImGui::TableHeadersRow();
                for (const auto& artifact : report.artifacts) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(artifact.kind.c_str());
                    ImGui::TableNextColumn();
                    const std::string path = artifact.path.generic_string();
                    if (ImGui::Selectable(path.c_str())) ImGui::SetClipboardText(path.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%llu B", static_cast<unsigned long long>(artifact.bytes));
                }
                ImGui::EndTable();
            }
            ImGui::TextDisabled("Click a path to copy it.");
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Keys")) {
            if (ImGui::BeginTable("##keys", 3, ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Shader");
                ImGui::TableSetupColumn("Enum");
                ImGui::TableSetupColumn("Key");
                ImGui::TableHeadersRow();
                for (const auto& shader : report.shaders) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(shader.id.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(
                        enum_name(report.gen_info.enum_prefix, shader.id).c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("0x%08X", shader.key);
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Snippets")) {
            draw_snippets(app);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

}

void draw_diagnostics_panel(App& app) {
    PanelScope panel("Diagnostics", &app.show_diagnostics);
    if (!panel) return;

    static bool show_info = true;
    static bool show_warnings = true;
    static bool as_text = false;
    static int selected = -1;

    // One flattened list feeds both views and every copy path, so what lands on
    // the clipboard is exactly what is on screen, filters included.
    struct Row {
        Severity severity = Severity::Info;
        std::string origin;  // shader id, "build", or empty for app log entries
        int line = 0;
        std::string text;
    };
    std::vector<Row> rows;
    auto collect = [&](const Diagnostic& d, const std::string& origin) {
        if (d.severity == Severity::Info && !show_info) return;
        if (d.severity == Severity::Warning && !show_warnings) return;
        rows.push_back({d.severity, origin, d.line,
                        (origin.empty() ? std::string() : origin + ": ") + d.format()});
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

    FlowLayout filters;
    filters.next(checkbox_width("info"));
    ImGui::Checkbox("Info", &show_info);
    filters.next(checkbox_width("warnings"));
    ImGui::Checkbox("Warnings", &show_warnings);
    // The text view trades the click-to-jump behaviour for a plain read-only
    // buffer, which is the only way to select part of a message with the mouse.
    filters.next(checkbox_width("as text"));
    ImGui::Checkbox("As text", &as_text);
    ImGui::BeginDisabled(rows.empty());
    filters.next(button_width("Copy all"));
    if (ImGui::SmallButton("Copy all")) ImGui::SetClipboardText(joined().c_str());
    ImGui::EndDisabled();
    filters.next(button_width("Recompile all"));
    if (ImGui::SmallButton("Recompile all")) app.compile_all();
    ImGui::Separator();

    if (rows.empty()) {
        ImGui::TextDisabled("Nothing to report.");
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
        ImGui::InputTextMultiline("##diagnostics_text", all.data(), all.size() + 1,
                                  ImVec2(-FLT_MIN, -FLT_MIN), ImGuiInputTextFlags_ReadOnly);
        return;
    }

    // Collected rather than acted on inside the loop: clearing empties the very
    // vectors the rows were built from.
    bool clear = false;

    const std::string all = joined();
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Row& row = rows[i];
        if (diagnostic_row(static_cast<int>(i), row.text, row.severity,
                           selected == static_cast<int>(i), all, clear, app.theme())) {
            selected = static_cast<int>(i);
            if (!row.origin.empty() && row.origin != "build") app.reveal(row.origin, row.line);
        }
    }

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
    ImGui::TextDisabled("Click to jump to the line, right-click to copy or clear, Ctrl+C copies "
                        "the selected message.");

}

}  // namespace ssstudio::gui
