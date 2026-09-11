#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>

#include "app.h"
#include "panel_common.h"
#include "bindings.h"
#include "preview/renderer.h"

namespace ssstudio::gui {
namespace {

void draw_toolbar(App& app) {
    PreviewSettings& preview = app.project().preview;

    // Both widths are settled before anything is placed: once the row starts,
    // the available region only describes what is left of the current line.
    const float speed_width = fitted_width(110.0f, 60.0f);
    const float size_width = fitted_width(150.0f, 80.0f);

    // The whole toolbar folds onto as many rows as the panel's width needs. The
    // play button is what someone reaches for first, so it leads and the size
    // fields - the ones you set once - are what drop off the end.
    FlowLayout row;

    const char* play_label = preview.paused ? "Play" : "Pause";
    row.next(button_width(play_label));
    if (ImGui::Button(play_label)) preview.paused = !preview.paused;

    row.next(button_width("Restart"));
    if (ImGui::Button("Restart")) app.reset_preview_time();

    row.next(labeled_width("speed", speed_width));
    float speed = static_cast<float>(preview.speed);
    if (ImGui::DragFloat(left_label("Speed", speed_width).c_str(), &speed, 0.01f, 0.0f, 8.0f)) {
        preview.speed = speed;
    }

    char clock[32];
    std::snprintf(clock, sizeof(clock), "t = %.2fs", app.preview_time());
    row.next(text_width(clock));
    // Same reason as the pipeline label below: bare text between framed widgets
    // sits half a line high without this.
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", clock);

    // The label only: the manifest key stays `follow_panel`, so renaming what it
    // is called does not silently reset the setting in every project that
    // already has one.
    row.next(checkbox_width("Match panel size"));
    ImGui::Checkbox("Match panel size", &preview.follow_panel);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Render at the panel's own size, so the image is one pixel per pixel.\n"
            "Off, the preview keeps the size you set and is scaled to fit - which is\n"
            "what a shader that reads its own resolution needs to stay reproducible.");
    }

    if (!preview.follow_panel) {
        row.next(labeled_width("size", size_width));
        int size[2] = {preview.width, preview.height};
        if (ImGui::DragInt2(left_label("Size", size_width).c_str(), size, 1.0f, 1, 8192)) {
            preview.width = size[0];
            preview.height = size[1];
        }
    }
}

/// The shaders of a stage a pipeline may name. All of them, not only the ones
/// that have compiled: a saved pipeline can name a shader that is still being
/// worked on, and it has to stay selectable while it is.
std::vector<const ShaderDesc*> shaders_of_stage(const Project& project, Stage stage) {
    std::vector<const ShaderDesc*> out;
    for (const auto& shader : project.shaders) {
        if (shader.stage == stage) out.push_back(&shader);
    }
    return out;
}

/// Drops a shader id a pipeline names once it is no longer in the project.
/// Tested against the project rather than against what has compiled: a shader
/// that has not compiled yet is about to, and rewriting a saved pipeline for
/// that would throw away a choice the user made.
bool refresh_choice(const Project& project, std::string& choice,
                    const std::vector<const ShaderDesc*>& list) {
    if (!choice.empty() && project.find_shader(choice) != nullptr) return false;
    const std::string replacement = list.empty() ? std::string() : list.front()->id;
    if (choice == replacement) return false;
    choice = replacement;
    return true;
}

/// The pipeline bar: which named pipeline is being previewed, and the way in to
/// setting it up. The shaders themselves are chosen in the pipeline editor - a
/// pipeline is a saved thing, and editing it from the toolbar made every glance
/// at the preview an opportunity to change the project by accident.
void draw_pass_selector(App& app) {
    PreviewRenderer* renderer = app.preview();
    if (!renderer) return;
    Project& project = app.project();

    // A project with exactly one of each gets its pipeline made for it. Only
    // ever the first one; after that this does nothing, so the pipeline it made
    // can be renamed or deleted like any other.
    bool project_changed = ensure_default_pipeline(project);

    const std::vector<const ShaderDesc*> vertices = shaders_of_stage(project, Stage::Vertex);
    const std::vector<const ShaderDesc*> fragments = shaders_of_stage(project, Stage::Fragment);

    PreviewPipeline* pipeline = project.active_pipeline();
    if (pipeline) {
        project_changed |= refresh_choice(project, pipeline->vertex, vertices);
        project_changed |= refresh_choice(project, pipeline->fragment, fragments);
    }

    const float combo_width = fitted_width(160.0f, 70.0f);
    FlowLayout row;

    // Which pipeline is in use, named first: with the shader pickers moved into
    // the editor, the name is the only thing on this bar that says what the
    // image below it is.
    row.next(text_width("Pipeline:"));
    // Plain text sits on the line's top edge while a framed widget sits one
    // padding in, so a label beside a combo reads as half a line too high
    // without this.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Pipeline:");

    row.next(combo_width);
    ImGui::SetNextItemWidth(combo_width);
    // Disabled rather than absent when there are none: the row keeps its shape,
    // and an empty list that opens onto nothing would be the more confusing of
    // the two.
    ImGui::BeginDisabled(project.pipelines.empty());
    if (ImGui::BeginCombo("##pipeline", pipeline ? pipeline->name.c_str() : "(none)")) {
        for (const auto& candidate : project.pipelines) {
            const bool selected = pipeline && candidate.name == pipeline->name;
            if (ImGui::Selectable(candidate.name.c_str(), selected)) {
                project.preview.pipeline = candidate.name;
                project_changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    // What it is made of, on hover rather than in the row: it is the answer to a
    // second question, and spelling it out here would push the buttons off a
    // narrow panel for something that is read once.
    if (pipeline && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s  ->  %s", pipeline->vertex.empty() ? "(none)"
                                                                 : pipeline->vertex.c_str(),
                          pipeline->fragment.empty() ? "(none)" : pipeline->fragment.c_str());
    }

    // One button rather than three: New, Duplicate, Edit and Delete are all
    // about the pipeline named to the left, and four of them in a toolbar is a
    // row that folds on any panel narrower than the preview it sits above.
    //
    // Named rather than drawn as a gear: no font is loaded beyond ImGui's
    // built-in one, which stops at Latin-1, so a gear glyph would come out as a
    // missing-character box. The trailing dots are this app's mark for a control
    // that opens something, as on "New shader..." and "Rename...".
    row.next(button_width("Manage..."));
    if (ImGui::Button("Manage...")) ImGui::OpenPopup("##pipeline_menu");

    if (ImGui::BeginPopup("##pipeline_menu")) {
        if (ImGui::MenuItem("New pipeline")) {
            PreviewPipeline created;
            created.name = unique_pipeline_name(project, "Pipeline");
            // Seeded from the one on screen, so a new pipeline starts as a copy
            // of what is being looked at rather than as an empty form.
            created.vertex = pipeline ? pipeline->vertex
                                      : (vertices.empty() ? std::string() : vertices.front()->id);
            created.fragment = pipeline ? pipeline->fragment
                                        : (fragments.empty() ? std::string()
                                                             : fragments.front()->id);
            project.preview.pipeline = created.name;
            project.pipelines.push_back(std::move(created));
            project_changed = true;
            // Straight into the editor: a pipeline that has just been made is
            // one the user is about to set up.
            app.show_pipeline_editor = true;
            app.request_panel_focus("Pipeline");
        }
        if (ImGui::MenuItem("Duplicate pipeline", nullptr, false, pipeline != nullptr)) {
            PreviewPipeline copy = *pipeline;
            copy.name = unique_pipeline_name(project, pipeline->name);
            project.preview.pipeline = copy.name;
            project.pipelines.push_back(std::move(copy));
            project_changed = true;
        }

        ImGui::Separator();

        if (ImGui::MenuItem("Edit pipeline", nullptr, app.show_pipeline_editor,
                            pipeline != nullptr)) {
            app.show_pipeline_editor = true;
            app.request_panel_focus("Pipeline");
        }

        // Greyed rather than hidden: there is always one pipeline that cannot be
        // removed, and a menu whose items come and go is harder to learn than
        // one that says why an item is unavailable.
        const bool can_delete = project.pipelines.size() > 1 && pipeline != nullptr;
        if (ImGui::MenuItem("Delete pipeline", nullptr, false, can_delete)) {
            const std::string going = pipeline->name;
            project.pipelines.erase(
                std::remove_if(project.pipelines.begin(), project.pipelines.end(),
                               [&](const PreviewPipeline& p) { return p.name == going; }),
                project.pipelines.end());
            project.preview.pipeline =
                project.pipelines.empty() ? std::string() : project.pipelines.front().name;
            project_changed = true;
        }
        if (!can_delete && pipeline != nullptr &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("The last pipeline cannot be deleted.");
        }
        ImGui::EndPopup();
    }

    // Re-read: a delete above may have moved the active pipeline.
    pipeline = project.active_pipeline();
    const std::string vertex = pipeline ? pipeline->vertex : std::string();
    const std::string fragment = pipeline ? pipeline->fragment : std::string();

    // The session keeps the resolved pair, because a project tab coming back to
    // the front re-asserts the pass from it before this bar has run.
    app.preview_vertex_choice() = vertex;
    app.preview_fragment_choice() = fragment;

    // Set every frame rather than only on a change: the renderer is shared by
    // every open project and is emptied on a tab switch, so the active pass has
    // to be re-asserted rather than assumed to have survived. set_active() is a
    // no-op when nothing moved.
    renderer->set_active(vertex, fragment);

    if (project_changed) {
        Diagnostics diags;
        save_project(project, diags);
        for (const auto& d : diags) app.log(d.severity, d.format());
    }
}

/// A fifth of the display the main window is on.
///
/// The same rule and the same fraction as the main window's own minimum size in
/// main.cpp. The display rather than the application's work area on purpose:
/// this editor is a window of its own and can be dragged clear of the main one,
/// so what it has to stay usable against is the screen, not whatever size the
/// app happens to have been left at - and a fifth of a main window that has
/// itself been shrunk to a fifth of the screen is a quarter of the intended
/// minimum in each direction.
///
/// The monitor is found by hit-testing the main viewport's centre rather than
/// read off the viewport, because the field recording it is internal. Falls back
/// to the viewport's own work area when the backend reports no monitors at all,
/// which is what a build without multi-viewport support gives.
ImVec2 minimal_windows_size() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 centre(viewport->Pos.x + viewport->Size.x * 0.5f,
                        viewport->Pos.y + viewport->Size.y * 0.5f);

    const ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
    for (const ImGuiPlatformMonitor& monitor : platform.Monitors) {
        const bool inside = centre.x >= monitor.MainPos.x &&
                            centre.x < monitor.MainPos.x + monitor.MainSize.x &&
                            centre.y >= monitor.MainPos.y &&
                            centre.y < monitor.MainPos.y + monitor.MainSize.y;
        if (inside) return ImVec2(monitor.MainSize.x * 0.2f, monitor.MainSize.y * 0.2f);
    }
    if (platform.Monitors.Size > 0) {
        const ImVec2 size = platform.Monitors[0].MainSize;
        return ImVec2(size.x * 0.2f, size.y * 0.2f);
    }
    return ImVec2(viewport->WorkSize.x * 0.2f, viewport->WorkSize.y * 0.2f);
}

}  // namespace

/// The pipeline editor.
///
/// A window of its own rather than a row in the preview toolbar: a pipeline is
/// saved with the project, and editing one from the toolbar made every glance at
/// the preview an opportunity to change the project by accident. It is opened to
/// set something up and closed again, which is what a window is for and what a
/// dock tab is not.
///
/// Which pipeline it edits is not its own choice: it is whichever one the
/// preview bar has selected, and the title says which. One selection living in
/// one place, rather than two that have to be kept agreeing with each other.
void draw_pipeline_panel(App& app) {
    ImGuiWindowClass standalone;
    standalone.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
    ImGui::SetNextWindowClass(&standalone);

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.32f, viewport->WorkSize.y * 0.3f),
                             ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
               viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
        ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(minimal_windows_size(), ImVec2(FLT_MAX, FLT_MAX));

    // Everything before ### is the label and everything after is the identity, so
    // the window keeps one id while its title follows the selection. That is
    // what request_panel_focus("Pipeline"), the docking layout and the ini file
    // all key on, and a title that changed the id would lose the window's place
    // every time the preview bar switched pipeline.
    std::string title = "Pipeline###Pipeline";
    if (app.project_open()) {
        if (const PreviewPipeline* active = app.project().active_pipeline()) {
            title = "Pipeline: " + active->name + "###Pipeline";
        }
    }

    PanelScope panel(title.c_str(), &app.show_pipeline_editor, ImGuiWindowFlags_NoDocking);
    if (!panel) return;

    if (!app.project_open()) {
        ImGui::TextDisabled("No project open.");
        return;
    }

    Project& project = app.project();
    PreviewPipeline* pipeline = project.active_pipeline();
    if (!pipeline) {
        ImGui::TextWrapped(
            "This project has no preview pipeline yet. Make one from Pipelines > New pipeline "
            "in the Preview panel.");
        return;
    }

    bool changed = false;

    char name[96];
    std::snprintf(name, sizeof(name), "%s", pipeline->name.c_str());
    if (ImGui::InputText(left_label("Name", 220.0f).c_str(), name, sizeof(name))) {
        // Refused rather than silently adjusted: two pipelines of one name would
        // make every list that shows them a guess.
        const std::string wanted = name;
        const PreviewPipeline* clash = project.find_pipeline(wanted);
        if (!wanted.empty() && (clash == nullptr || clash == pipeline)) {
            project.preview.pipeline = wanted;
            pipeline->name = wanted;
            changed = true;
        }
    }

    ImGui::SeparatorText("Stages");
    ImGui::TextDisabled("One vertex and one fragment shader, which is what the preview draws.");
    ImGui::Spacing();

    // Every shader of the stage, not only the compiled ones: a pipeline may name
    // a shader that is still being worked on, and it has to stay selectable
    // while it is. Whether it is ready is said beside it instead.
    const auto stage_combo = [&](const char* label, Stage stage, std::string& choice) {
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::BeginCombo(left_label(label, 220.0f).c_str(),
                              choice.empty() ? "(none)" : choice.c_str())) {
            for (const auto* shader : shaders_of_stage(project, stage)) {
                if (ImGui::Selectable(shader->id.c_str(), shader->id == choice)) {
                    choice = shader->id;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        const Document* doc = choice.empty() ? nullptr : app.find_document(choice);
        ImGui::SameLine();
        if (choice.empty()) {
            ImGui::TextColored(ImVec4(0.92f, 0.42f, 0.42f, 1.0f), "not set");
        } else if (doc && doc->compiled_ok) {
            ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f), "compiled");
        } else {
            ImGui::TextColored(ImVec4(0.92f, 0.75f, 0.35f, 1.0f), "not compiled yet");
        }
    };
    stage_combo("Vertex", Stage::Vertex, pipeline->vertex);

    // A shader cannot be both a buffer pass and the pass that draws the image:
    // it would own two targets under one name, and every reference to it would
    // be ambiguous. That is why the pass dropdown below leaves the image shader
    // out - but picking one here that the chain is already running reaches the
    // same state from the other side, and something has to give.
    //
    // It is resolved in favour of the choice just made, because that is the one
    // the user is looking at. The pass is dropped rather than blanked, the same
    // way one naming a deleted shader is: a chain with a hole in it is not a
    // chain, and an empty entry would only be a target nobody writes.
    const std::string previous_fragment = pipeline->fragment;
    stage_combo("Fragment", Stage::Fragment, pipeline->fragment);
    if (pipeline->fragment != previous_fragment && !pipeline->fragment.empty()) {
        auto& passes = pipeline->passes;
        const auto dropped =
            std::remove_if(passes.begin(), passes.end(), [&pipeline](const PassDesc& pass) {
                return pass.shader_id == pipeline->fragment;
            });
        if (dropped != passes.end()) {
            passes.erase(dropped, passes.end());
            // Said out loud rather than done quietly: this edits the project,
            // and a pass disappearing from a list the user set up is not
            // something to work out afterwards from a preview that changed.
            app.log(Severity::Info, "'" + pipeline->fragment +
                                        "' draws the image now, so it was dropped from the "
                                        "passes of '" + pipeline->name + "'.");
            changed = true;
        }
    }

    // --- the chain ---------------------------------------------------------
    ImGui::SeparatorText("Passes");
    ImGui::TextDisabled(
        "Passes run in order before the fragment shader above, each into a target the later "
        "ones can sample. A pipeline with none draws the fragment shader straight to the "
        "screen, which is the ordinary case.");
    ImGui::Spacing();

    int remove_at = -1;
    int move_from = -1;
    int move_to = -1;

    for (std::size_t i = 0; i < pipeline->passes.size(); ++i) {
        PassDesc& pass = pipeline->passes[i];
        ImGui::PushID(static_cast<int>(i));

        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::BeginCombo("##shader",
                              pass.shader_id.empty() ? "(none)" : pass.shader_id.c_str())) {
            for (const auto* shader : shaders_of_stage(project, Stage::Fragment)) {
                if (shader->id == pipeline->fragment) continue;  // that one is the image pass
                if (ImGui::Selectable(shader->id.c_str(), shader->id == pass.shader_id)) {
                    pass.shader_id = shader->id;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        const char* formats[] = {"rgba8", "rgba16f", "rgba32f"};
        int format = static_cast<int>(pass.format);
        if (ImGui::Combo("##format", &format, formats, IM_ARRAYSIZE(formats))) {
            pass.format = static_cast<PassFormat>(format);
            changed = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Eight bits per channel clamps to 0..1 and quantises to 256 steps.\n"
                "A buffer holding anything but colour wants one of the float formats.");
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(i == 0);
        if (ImGui::ArrowButton("##up", ImGuiDir_Up)) {
            move_from = static_cast<int>(i);
            move_to = static_cast<int>(i) - 1;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(i + 1 >= pipeline->passes.size());
        if (ImGui::ArrowButton("##down", ImGuiDir_Down)) {
            move_from = static_cast<int>(i);
            move_to = static_cast<int>(i) + 1;
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) remove_at = static_cast<int>(i);

        ImGui::PopID();
    }

    // Applied after the loop: changing the list while iterating it is how a
    // reorder button ends up removing the wrong row.
    if (move_from >= 0 && move_to >= 0) {
        std::swap(pipeline->passes[static_cast<std::size_t>(move_from)],
                  pipeline->passes[static_cast<std::size_t>(move_to)]);
        changed = true;
    }
    if (remove_at >= 0) {
        pipeline->passes.erase(pipeline->passes.begin() + remove_at);
        changed = true;
    }

    const auto* first_free = [&]() -> const ShaderDesc* {
        for (const auto* shader : shaders_of_stage(project, Stage::Fragment)) {
            if (shader->id == pipeline->fragment) continue;
            const bool taken = std::any_of(pipeline->passes.begin(), pipeline->passes.end(),
                                           [&shader](const PassDesc& p) {
                                               return p.shader_id == shader->id;
                                           });
            if (!taken) return shader;
        }
        return nullptr;
    }();

    ImGui::BeginDisabled(first_free == nullptr);
    if (ImGui::Button("Add pass")) {
        PassDesc pass;
        pass.shader_id = first_free->id;
        pipeline->passes.push_back(std::move(pass));
        changed = true;
    }
    ImGui::EndDisabled();
    if (first_free == nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            "Every fragment shader in this project is already the image pass or a pass here.");
    }

    if (changed) {
        Diagnostics diags;
        save_project(project, diags);
        for (const auto& d : diags) app.log(d.severity, d.format());
    }
}

/// The failure, spelled out where the user is looking and in a form they can
/// hand on. A preview that fails is usually a driver's complaint about a
/// pipeline, which is exactly the kind of message that has to be pasted into a
/// search or a bug report to be worth anything - so it is selectable text with
/// a copy button rather than a greyed-out line.
void draw_preview_error(const std::string& message) {
    ImGui::TextColored(ImVec4(0.92f, 0.42f, 0.42f, 1.0f), "The preview could not draw.");
    ImGui::Spacing();

    // Read-only rather than static text: ImGui's plain text cannot be selected
    // with the mouse, and half a driver message is often the half that matters.
    //
    // The wrapping is done here, in the string, because a multi-line input box
    // does not wrap - and a driver message is one long line, so left to itself
    // it scrolls off the right edge and the panel shows the middle of a
    // sentence. Only the copy is broken up; the clipboard still gets the
    // original, which is what anybody pasting it into a search wants.
    const float width = ImGui::GetContentRegionAvail().x;
    const float glyph = std::max(1.0f, ImGui::CalcTextSize("M").x);
    const std::size_t columns =
        static_cast<std::size_t>(std::max(20.0f, (width - 16.0f) / glyph));

    std::string text;
    std::size_t column = 0;
    for (std::size_t i = 0; i < message.size(); ++i) {
        if (message[i] == '\n') {
            column = 0;
        } else if (column >= columns) {
            // Back up to the last space so a word is not split, unless the run
            // is longer than the line - a path or a hash usually is.
            const std::size_t space = text.find_last_of(' ');
            const std::size_t line_start = text.find_last_of('\n');
            if (space != std::string::npos &&
                (line_start == std::string::npos || space > line_start + 1)) {
                text[space] = '\n';
                column = text.size() - space - 1;
            } else {
                text += '\n';
                column = 0;
            }
        }
        text += message[i];
        ++column;
    }

    const int lines = static_cast<int>(std::count(text.begin(), text.end(), '\n')) + 1;
    const float height = ImGui::GetTextLineHeight() * static_cast<float>(std::min(lines, 8) + 1);
    ImGui::InputTextMultiline("##preview_error", text.data(), text.size() + 1,
                              ImVec2(-FLT_MIN, height), ImGuiInputTextFlags_ReadOnly);

    if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(message.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("Also logged in Diagnostics.");
}

void draw_preview_panel(App& app) {
    PreviewRenderer* renderer = app.preview();
    const bool preview_usable = renderer != nullptr && renderer->ready() &&
                                app.project_open() && app.preview_ready();

    // The docked tab carries the state as well as the panel body. Everything
    // after "###" is the window's identity, so the visible half can change
    // without moving the window in the dock layout or in the layout presets.
    PanelScope panel(preview_usable ? "Preview###Preview" : "Preview (unavailable)###Preview",
                     &app.show_preview);
    if (!panel) return;

    if (!renderer || !renderer->ready()) {
        ImGui::TextWrapped("The preview device is unavailable: %s",
                           renderer ? renderer->last_error().c_str() : "no renderer");
        ImGui::TextDisabled("Editing, compiling and building all still work.");
        return;
    }
    if (!app.project_open()) {
        ImGui::TextDisabled("No project open.");
        return;
    }

    // Nothing here can do anything useful until a vertex and a fragment shader
    // have each compiled once, so the whole panel stays disabled until then and
    // says which half is still missing.
    if (!app.preview_ready()) {
        bool has_vertex = false;
        bool has_fragment = false;
        bool compiling = false;
        for (const auto& doc : app.documents()) {
            if (doc.stage == Stage::Vertex) has_vertex = true;
            if (doc.stage == Stage::Fragment) has_fragment = true;
            compiling = compiling || doc.compiling;
        }

        ImGui::TextDisabled("Preview unavailable.");
        ImGui::Spacing();
        if (!has_vertex || !has_fragment) {
            ImGui::TextWrapped(
                "The preview draws a vertex and a fragment shader together; this project has "
                "%s. Add the missing stage to the project and it will start here.",
                !has_vertex && !has_fragment ? "neither"
                                             : (has_vertex ? "no fragment shader"
                                                           : "no vertex shader"));
        } else if (compiling) {
            ImGui::TextWrapped("Waiting for the first compile to finish.");
        } else {
            ImGui::TextWrapped(
                "Both stages are here, but they have not both compiled yet. The Diagnostics "
                "panel lists what the compiler objected to; fix that and the preview turns on "
                "by itself.");
        }

        ImGui::Spacing();
        ImGui::BeginDisabled(true);
        draw_toolbar(app);
        draw_pass_selector(app);
        ImGui::EndDisabled();
        return;
    }

    draw_toolbar(app);
    draw_pass_selector(app);
    ImGui::Separator();

    PreviewSettings& settings = app.project().preview;
    const ImVec2 available = ImGui::GetContentRegionAvail();
    if (settings.follow_panel) {
        settings.width = std::max(1, static_cast<int>(available.x));
        settings.height = std::max(1, static_cast<int>(available.y - 24.0f));
    }
    renderer->resize(settings.width, settings.height);

    // The chain is worked out first and drawn second. A pipeline with no passes
    // schedules to exactly one pass into one target, so the ordinary case goes
    // down the same road as a chain rather than round it.
    const PassChain chain = evaluate_pass_chain(app);
    PassSchedule schedule;
    Diagnostics schedule_diags;
    if (!build_pass_schedule(chain, static_cast<std::uint32_t>(app.preview_frame() & 1u), schedule,
                             schedule_diags)) {
        const std::string reason = schedule_diags.empty() ? std::string("this pipeline cannot be "
                                                                       "drawn")
                                                          : schedule_diags.front().message;
        app.report_preview_status(reason);
        draw_preview_error(reason);
        return;
    }

    const std::map<std::string, PassFeed> feeds = evaluate_pass_feeds(app, schedule);
    const bool rendered = renderer->render(schedule, feeds, settings);

    // Which target is on screen. Only worth offering once there is more than
    // one, and it is what turns "the chain is wrong somewhere" into "the chain
    // is wrong here".
    if (rendered && schedule.targets.size() > 1) {
        const auto labels = renderer->target_labels();
        const auto describe = [&](std::size_t index) {
            if (index >= labels.size()) return std::string("(none)");
            const auto& label = labels[index];
            if (label.owner_pass.empty()) return std::string("Image");
            std::string text = label.owner_pass;
            if (label.copy != 0) text += " (previous)";
            return text + "  " + std::string(to_string(label.format));
        };
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::BeginCombo("Showing", describe(renderer->inspected_target()).c_str())) {
            for (std::size_t i = 0; i < labels.size(); ++i) {
                if (ImGui::Selectable(describe(i).c_str(), renderer->inspected_target() == i)) {
                    renderer->set_inspected_target(static_cast<std::uint32_t>(i));
                }
            }
            ImGui::EndCombo();
        }
    }

    if (!rendered) {
        app.report_preview_status(renderer->status());
        draw_preview_error(renderer->status());
        return;
    }
    // A frame that drew clears the standing failure, so the same message
    // arriving again later is reported again.
    app.report_preview_status(std::string());

    SDL_GPUTexture* texture = renderer->texture_id();
    if (!texture) {
        ImGui::TextDisabled("no preview target");
        return;
    }

    // Fit the target into the panel while preserving its aspect ratio.
    const float target_aspect =
        static_cast<float>(renderer->width()) / static_cast<float>(std::max(1, renderer->height()));
    float draw_width = available.x;
    float draw_height = draw_width / target_aspect;
    if (draw_height > available.y - 24.0f) {
        draw_height = available.y - 24.0f;
        draw_width = draw_height * target_aspect;
    }

    const ImVec2 image_pos = ImGui::GetCursorScreenPos();
    const ImVec2 image_end(image_pos.x + draw_width, image_pos.y + draw_height);
    const ResolvedTheme& theme = app.theme();

    // Behind the image, so it shows through wherever the shader wrote alpha.
    // "color" needs nothing here: the renderer has already cleared to the
    // project's clear colour, which is the project's business rather than the
    // theme's - the theme owns the frame, not the picture.
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    if (app.settings().preview.background == "checkerboard") {
        const float square = std::max(4.0f, theme.checker_size);
        draw_list->AddRectFilled(image_pos, image_end, theme_u32(theme.preview_color("checker_a")));
        draw_list->PushClipRect(image_pos, image_end, true);
        const ImU32 second = theme_u32(theme.preview_color("checker_b"));
        int row = 0;
        for (float y = image_pos.y; y < image_end.y; y += square, ++row) {
            for (int column = row % 2; ; column += 2) {
                const float x = image_pos.x + static_cast<float>(column) * square;
                if (x >= image_end.x) break;
                draw_list->AddRectFilled(ImVec2(x, y), ImVec2(x + square, y + square), second);
            }
        }
        draw_list->PopClipRect();
    }

    ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture)),
                 ImVec2(draw_width, draw_height));
    // Drawn after the image so the edge of the output is visible against a
    // window of a similar colour.
    draw_list->AddRect(image_pos, image_end, theme_u32(theme.preview_color("border")));
    // The one place an output actually reaches the screen, and so the one place
    // that can say the shader clock has a frame to account for.
    app.note_preview_presented();

    // Pointer state for shaders that take a mouse uniform. Derived from the
    // rectangle the image was just drawn into and flipped to the bottom-left
    // origin a fullscreen fragment shader works in.
    //
    // The hover test can only be answered after the image has been placed, which
    // is after this frame's render() call - so a shader sees the pointer one
    // frame late. That is the right trade: render() has to run first because the
    // image needs the texture it produces.
    if (draw_width > 0.0f && draw_height > 0.0f) {
        PreviewMouse mouse = app.preview_mouse();
        const bool hovered = ImGui::IsItemHovered();
        mouse.pressed = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        mouse.down = hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left);
        if (mouse.down || mouse.pressed) {
            const ImVec2 position = ImGui::GetMousePos();
            const float target_w = static_cast<float>(renderer->width());
            const float target_h = static_cast<float>(renderer->height());
            mouse.x = (position.x - image_pos.x) / draw_width * target_w;
            mouse.y = target_h - (position.y - image_pos.y) / draw_height * target_h;
            if (mouse.pressed) {
                mouse.click_x = mouse.x;
                mouse.click_y = mouse.y;
            }
        }
        app.set_preview_mouse(mouse);
    }

    // Pixel inspector: hover to read the rendered value back off the GPU.
    if (app.settings().preview.pixel_inspector && ImGui::IsItemHovered()) {
        const ImVec2 mouse = ImGui::GetMousePos();
        const int x = static_cast<int>((mouse.x - image_pos.x) / draw_width * renderer->width());
        const int y = static_cast<int>((mouse.y - image_pos.y) / draw_height * renderer->height());
        float rgba[4] = {0, 0, 0, 0};
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && renderer->read_pixel(x, y, rgba)) {
            ImGui::BeginTooltip();
            ImGui::Text("(%d, %d)", x, y);
            ImGui::ColorButton("##pixel", ImVec4(rgba[0], rgba[1], rgba[2], rgba[3]));
            ImGui::SameLine();
            ImGui::Text("%.3f %.3f %.3f %.3f", rgba[0], rgba[1], rgba[2], rgba[3]);
            ImGui::EndTooltip();
        } else {
            ImGui::SetTooltip("(%d, %d) - hold left mouse to sample", x, y);
        }
    }

    if (app.settings().preview.show_stats) {
        ImGui::TextColored(theme_vec4(app.theme().preview_color("stats_ink")),
                           "%dx%d  |  %.1f fps  |  %s", renderer->width(), renderer->height(),
                           ImGui::GetIO().Framerate, renderer->status().c_str());
    }

}

}  // namespace ssstudio::gui
