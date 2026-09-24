#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>  // RenderArrow, for the target chip

#include "app.h"
#include "fonts.h"
#include "panel_common.h"
#include "bindings.h"
#include "preview/renderer.h"
#include "widgets.h"

namespace ssstudio::gui {
namespace {

/// Play or Pause, with the glyph drawn in front of the word rather than typed:
/// the built-in font has neither symbol, and a box where the pause bars should
/// be is worse than no symbol at all.
bool play_button(bool paused, const ThemeInk& ink) {
    const char* label = paused ? "Play" : "Pause";
    const ImGuiStyle& style = ImGui::GetStyle();
    const float icon = std::round(ImGui::GetFontSize() * 0.62f);
    const float gap = design_px(6.0f);
    const ImVec2 size(style.FramePadding.x * 2.0f + icon + gap + text_width(label),
                      ImGui::GetFrameHeight());
    const bool pressed = ImGui::Button("##play_pause", size);
    const ImVec2 min = ImGui::GetItemRectMin();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const float top = min.y + (size.y - icon) * 0.5f;
    const float left = min.x + style.FramePadding.x;
    if (paused) {
        draw_list->AddTriangleFilled(ImVec2(left + icon * 0.1f, top),
                                     ImVec2(left + icon * 0.1f, top + icon),
                                     ImVec2(left + icon * 0.95f, top + icon * 0.5f), ink.muted);
    } else {
        const float bar = std::max(1.0f, icon * 0.3f);
        draw_list->AddRectFilled(ImVec2(left + icon * 0.1f, top),
                                 ImVec2(left + icon * 0.1f + bar, top + icon), ink.muted, 1.0f);
        draw_list->AddRectFilled(ImVec2(left + icon * 0.9f - bar, top),
                                 ImVec2(left + icon * 0.9f, top + icon), ink.muted, 1.0f);
    }
    draw_list->AddText(ImVec2(left + icon + gap, min.y + style.FramePadding.y), ink.text, label);
    return pressed;
}

/// The left half of the preview's one toolbar row: the clock. Play, restart,
/// speed and the time it has reached.
void draw_toolbar(App& app, FlowLayout& row, const ThemeInk& ink) {
    PreviewSettings& preview = app.project().preview;

    // Settled before anything is placed: once the row starts, the available
    // region only describes what is left of the current line.
    const float speed_width = std::min(fitted_width(64.0f, 50.0f), design_px(72.0f));

    // The whole toolbar folds onto as many rows as the panel's width needs. The
    // play button is what someone reaches for first, so it leads.
    {
        const char* label = preview.paused ? "Play" : "Pause";
        row.next(button_width(label) + design_px(14.0f));
        if (play_button(preview.paused, ink)) preview.paused = !preview.paused;
    }

    row.next(button_width("Restart"));
    if (ImGui::Button("Restart")) app.reset_preview_time();

    row.next(design_px(9.0f));
    toolbar_divider(ink);

    row.next(labeled_width("Speed", speed_width));
    float speed = static_cast<float>(preview.speed);
    ImGui::PushStyleColor(ImGuiCol_Text, ink.muted);
    const std::string speed_id = left_label("Speed", speed_width);
    ImGui::PopStyleColor();
    bool speed_changed = false;
    {
        MonoScope mono(12.0f);
        speed_changed = ImGui::DragFloat(speed_id.c_str(), &speed, 0.01f, 0.0f, 8.0f,
                                         "%.2f\xC3\x97");
    }
    if (speed_changed) preview.speed = speed;

    char clock[32];
    std::snprintf(clock, sizeof(clock), "t %.2f s", app.preview_time());
    MonoScope mono(12.0f);
    row.next(text_width(clock));
    // Bare text between framed widgets sits half a line high without this.
    ImGui::AlignTextToFramePadding();
    colored_text(clock, ink.muted);
}

/// The end of the toolbar row: whether the image follows the panel's size, and
/// the size it keeps when it does not.
void draw_size_controls(App& app, const ThemeInk& ink) {
    PreviewSettings& preview = app.project().preview;

    // The label only: the manifest key stays `follow_panel`, so renaming what it
    // is called does not silently reset the setting in every project that
    // already has one.
    ImGui::Checkbox("Fit panel", &preview.follow_panel);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Render at the panel's own size, so the image is one pixel per pixel.\n"
            "Off, the preview keeps the size you set and is scaled to fit - which is\n"
            "what a shader that reads its own resolution needs to stay reproducible.");
    }

    if (!preview.follow_panel) {
        ImGui::SameLine();
        const float size_width = design_px(130.0f);
        int size[2] = {preview.width, preview.height};
        ImGui::PushStyleColor(ImGuiCol_Text, ink.muted);
        const std::string id = left_label("Size", size_width);
        ImGui::PopStyleColor();
        MonoScope mono(12.0f);
        if (ImGui::DragInt2(id.c_str(), size, 1.0f, 1, 8192)) {
            preview.width = size[0];
            preview.height = size[1];
        }
    }
}

/// How wide draw_size_controls() will be.
float size_controls_width(const PreviewSettings& preview) {
    float width = checkbox_width("Fit panel");
    if (!preview.follow_panel) {
        width += ImGui::GetStyle().ItemSpacing.x + labeled_width("Size", design_px(130.0f));
    }
    return width;
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
void draw_pass_selector(App& app, FlowLayout& row, const ThemeInk& ink) {
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

    const float combo_width = std::min(fitted_width(140.0f, 70.0f), design_px(140.0f));

    // The pipeline half of the row goes against the right edge, as one group:
    // it is about what is being drawn, where the left half is about the clock.
    // When the row is too narrow to hold both, the group folds onto a row of
    // its own rather than splitting.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float group_width = text_width("Pipeline") + style.ItemSpacing.x + combo_width +
                              style.ItemSpacing.x + button_width("Manage...") +
                              style.ItemSpacing.x + design_px(9.0f) + style.ItemSpacing.x +
                              size_controls_width(project.preview);
    row.next(group_width);
    if (ImGui::GetCursorPosX() + group_width < ImGui::GetContentRegionMax().x) {
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - group_width);
    }

    // Which pipeline is in use, named first: with the shader pickers moved into
    // the editor, the name is the only thing on this bar that says what the
    // image below it is.
    // Plain text sits on the line's top edge while a framed widget sits one
    // padding in, so a label beside a combo reads as half a line too high
    // without this.
    ImGui::AlignTextToFramePadding();
    colored_text("Pipeline", ink.muted);

    ImGui::SameLine();
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
    ImGui::SameLine();
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

    ImGui::SameLine();
    toolbar_divider(ink);
    ImGui::SameLine();
    draw_size_controls(app, ink);

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
    //
    // In the code font, which is what makes the column count below mean
    // something: every glyph is as wide as "M" there, and in the interface
    // font almost none are.
    {
        MonoScope mono(12.0f);
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
    }

    if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(message.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("Also logged in Diagnostics.");
}

void draw_preview_panel(App& app) {
    const ThemeInk ink(app.theme());
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
        FlowLayout row;
        draw_toolbar(app, row, ink);
        draw_pass_selector(app, row, ink);
        ImGui::EndDisabled();
        return;
    }

    {
        FlowLayout row;
        draw_toolbar(app, row, ink);
        draw_pass_selector(app, row, ink);
    }
    ImGui::Separator();

    // The image sits in from the panel's edges, the way the design frames it,
    // so its border reads as the picture's edge rather than the panel's.
    const float inset = design_px(6.0f);
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + inset, ImGui::GetCursorPosY() + inset));

    // A pass in the chain that no longer compiles. The renderer goes on drawing
    // the last blob that did, so the picture is still there - it is just not
    // the code on screen any more, and has to say so.
    const PreviewPipeline* active_pipeline = app.project().active_pipeline();
    std::vector<const Document*> chain_documents;
    if (active_pipeline != nullptr) {
        if (const Document* doc = app.find_document(active_pipeline->vertex)) {
            chain_documents.push_back(doc);
        }
        for (const PassDesc& pass : active_pipeline->passes) {
            if (const Document* doc = app.find_document(pass.shader_id)) chain_documents.push_back(doc);
        }
        if (const Document* doc = app.find_document(active_pipeline->fragment)) {
            chain_documents.push_back(doc);
        }
    }
    const auto failed = [](const Document* doc) {
        return !doc->compiled_ok && !doc->compiling && has_errors(doc->diagnostics);
    };
    const Document* failing = nullptr;
    for (const Document* doc : chain_documents) {
        if (failed(doc)) {
            failing = doc;
            break;
        }
    }
    // Room for the per-pass list drawn under the image while a pass is broken:
    // its caption and one frame-high row per pass, each followed by the item
    // spacing ImGui leaves under it, plus the box's own margins.
    const float spacing_y = ImGui::GetStyle().ItemSpacing.y;
    const float list_height =
        failing != nullptr
            ? design_px(14.0f) + ImGui::GetTextLineHeight() + spacing_y +
                  (ImGui::GetFrameHeight() + spacing_y) * static_cast<float>(chain_documents.size())
            : 0.0f;

    PreviewSettings& settings = app.project().preview;
    const ImVec2 room = ImGui::GetContentRegionAvail();
    const ImVec2 available(std::max(1.0f, room.x - inset),
                           std::max(1.0f, room.y - inset - list_height));
    if (settings.follow_panel) {
        settings.width = std::max(1, static_cast<int>(available.x));
        settings.height = std::max(1, static_cast<int>(available.y));
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
    if (draw_height > available.y) {
        draw_height = available.y;
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

    // The last good frame, when the code on screen no longer compiles: drawn at
    // a little over half strength so it cannot be mistaken for the current one.
    const ImTextureRef image(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture)));
    if (failing != nullptr) {
        ImGui::ImageWithBg(image, ImVec2(draw_width, draw_height), ImVec2(0, 0), ImVec2(1, 1),
                           ImVec4(0, 0, 0, 0), ImVec4(1, 1, 1, 0.55f));
    } else {
        ImGui::Image(image, ImVec2(draw_width, draw_height));
    }
    const bool image_hovered = ImGui::IsItemHovered();
    // Drawn after the image so the edge of the output is visible against a
    // window of a similar colour - and in the warning ink while it is stale.
    const float rounding = ImGui::GetStyle().ImageRounding;
    draw_list->AddRect(image_pos, image_end,
                       failing != nullptr ? with_alpha(ink.warn, 0.5f)
                                          : theme_u32(theme.preview_color("border")),
                       rounding);
    // The one place an output actually reaches the screen, and so the one place
    // that can say the shader clock has a frame to account for.
    app.note_preview_presented();

    // What sits on the picture: the stale-frame warning, which target is
    // showing, and the numbers. Laid out first, so the pointer can be kept off
    // the shader while it is over one of them.
    const float margin = design_px(10.0f);
    const float chip_height = ImGui::GetFrameHeight();
    const float chip_pad = design_px(10.0f);
    const float chip_gap = design_px(8.0f);
    float next_chip_y = image_pos.y + margin;
    std::vector<std::pair<ImVec2, ImVec2>> chips;

    std::string stale;
    ImVec2 stale_min, stale_max;
    if (failing != nullptr) {
        stale = std::string("Last good frame \xC2\xB7 ") + shader_display_name(failing->path, failing->id) +
                "." + stage_suffix(failing->stage) + " failed";
        FontScope small(nullptr, 12.0f);
        stale_min = ImVec2(image_pos.x + margin, next_chip_y);
        stale_max = ImVec2(stale_min.x + chip_pad * 2.0f + design_px(6.0f) + chip_gap +
                               text_width(stale.c_str()),
                           stale_min.y + chip_height);
        chips.emplace_back(stale_min, stale_max);
        next_chip_y = stale_max.y + design_px(6.0f);
    }

    // Which target is on screen. Only worth offering once there is more than
    // one, and it is what turns "the chain is wrong somewhere" into "the chain
    // is wrong here".
    const auto labels = renderer->target_labels();
    const auto describe = [&](std::size_t index) {
        if (index >= labels.size()) return std::string("(none)");
        const auto& label = labels[index];
        if (label.owner_pass.empty()) return std::string("Image");
        std::string text = label.owner_pass;
        if (label.copy != 0) text += " (previous)";
        return text + "  " + std::string(to_string(label.format));
    };
    const bool offer_targets = schedule.targets.size() > 1;
    const std::string target_text = describe(renderer->inspected_target());
    ImVec2 target_min, target_max;
    if (offer_targets) {
        const float arrow = ImGui::GetFontSize() * 0.6f;
        target_min = ImVec2(image_pos.x + margin, next_chip_y);
        target_max = ImVec2(target_min.x + chip_pad * 2.0f + text_width("Target") + chip_gap +
                                text_width(target_text.c_str()) + chip_gap + arrow,
                            target_min.y + chip_height);
        chips.emplace_back(target_min, target_max);
    }

    std::string stats;
    ImVec2 stats_min, stats_max;
    if (app.settings().preview.show_stats) {
        char text[256];
        std::snprintf(text, sizeof(text), "%d\xC3\x97%d   %.0f fps   %s", renderer->width(),
                      renderer->height(), ImGui::GetIO().Framerate, renderer->status().c_str());
        stats = text;
        MonoScope mono(11.0f);
        const float width = text_width(stats.c_str()) + chip_pad * 2.0f;
        stats_max = ImVec2(image_end.x - margin, image_pos.y + margin + chip_height);
        stats_min = ImVec2(stats_max.x - width, image_pos.y + margin);
        chips.emplace_back(stats_min, stats_max);
    }

    const ImVec2 pointer = ImGui::GetMousePos();
    const bool over_chip = std::any_of(chips.begin(), chips.end(), [&](const auto& chip) {
        return pointer.x >= chip.first.x && pointer.x < chip.second.x &&
               pointer.y >= chip.first.y && pointer.y < chip.second.y;
    });
    const bool picture_hovered = image_hovered && !over_chip;

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
        const bool hovered = picture_hovered;
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
    if (app.settings().preview.pixel_inspector && picture_hovered) {
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

    // --- the chips themselves -------------------------------------------
    if (failing != nullptr) {
        FontScope small(nullptr, 12.0f);
        overlay_frame(draw_list, stale_min, stale_max, ink, with_alpha(ink.warn, 0.5f));
        const float center_y = (stale_min.y + stale_max.y) * 0.5f;
        const float dot = design_px(6.0f);
        draw_list->AddCircleFilled(ImVec2(stale_min.x + chip_pad + dot * 0.5f, center_y), dot * 0.5f,
                                   ink.warn);
        draw_list->AddText(ImVec2(stale_min.x + chip_pad + dot + chip_gap,
                                  center_y - ImGui::GetTextLineHeight() * 0.5f),
                           ink.warn, stale.c_str());
    }

    if (offer_targets) {
        const ImVec2 after_image = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(target_min);
        if (ImGui::InvisibleButton("##target_chip",
                                   ImVec2(target_max.x - target_min.x, target_max.y - target_min.y))) {
            ImGui::OpenPopup("##target_menu");
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::SetItemTooltip("Which target of the chain is on screen");
        overlay_frame(draw_list, target_min, target_max, ink, hovered ? ink.strong : 0);
        const float text_y = (target_min.y + target_max.y - ImGui::GetTextLineHeight()) * 0.5f;
        float x = target_min.x + chip_pad;
        draw_list->AddText(ImVec2(x, text_y), ink.muted, "Target");
        x += text_width("Target") + chip_gap;
        draw_list->AddText(ImVec2(x, text_y), ink.text, target_text.c_str());
        x += text_width(target_text.c_str()) + chip_gap;
        const float arrow = ImGui::GetFontSize() * 0.6f;
        ImGui::RenderArrow(draw_list,
                           ImVec2(x, (target_min.y + target_max.y - arrow) * 0.5f), ink.muted,
                           ImGuiDir_Down, 0.6f);
        ImGui::SetNextWindowPos(ImVec2(target_min.x, target_max.y + design_px(4.0f)));
        if (ImGui::BeginPopup("##target_menu")) {
            for (std::size_t i = 0; i < labels.size(); ++i) {
                if (ImGui::Selectable(describe(i).c_str(), renderer->inspected_target() == i)) {
                    renderer->set_inspected_target(static_cast<std::uint32_t>(i));
                }
            }
            ImGui::EndPopup();
        }
        // Back to where the image left the layout, and an empty item there so
        // the window's extent is set by something that was submitted rather
        // than by a bare cursor move.
        ImGui::SetCursorScreenPos(after_image);
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
    }

    if (!stats.empty()) {
        MonoScope mono(11.0f);
        overlay_frame(draw_list, stats_min, stats_max, ink);
        draw_list->AddText(ImVec2(stats_min.x + chip_pad,
                                  (stats_min.y + stats_max.y - ImGui::GetTextLineHeight()) * 0.5f),
                           theme_u32(theme.preview_color("stats_ink")), stats.c_str());
    }

    // --- the chain, pass by pass, while one of its passes is broken -------
    if (failing != nullptr) {
        ImGui::Dummy(ImVec2(0.0f, design_px(4.0f)));
        // The box's size is only known once its rows are laid out, so the rows
        // go on the upper channel and the box is filled in beneath them after.
        draw_list->ChannelsSplit(2);
        draw_list->ChannelsSetCurrent(1);
        const ImVec2 box_min = ImGui::GetCursorScreenPos();
        const float box_width = std::max(1.0f, available.x);
        ImGui::Indent(design_px(12.0f));
        ImGui::Dummy(ImVec2(0.0f, design_px(2.0f)));
        caps_label("Pipeline", ink);
        bool blocked = false;
        int index = 0;
        for (const Document* doc : chain_documents) {
            ++index;
            const bool broken = failed(doc);
            const ImU32 color = broken ? ink.error : (blocked ? ink.strong : ink.ok);
            const char* state = broken ? "error" : (blocked ? "waiting" : "ok");
            ImGui::AlignTextToFramePadding();
            status_dot(color);
            ImGui::SameLine(0.0f, design_px(8.0f));
            MonoScope mono(12.0f);
            const std::string name = std::to_string(index) + " " +
                                     shader_display_name(doc->path, doc->id) + "." +
                                     stage_suffix(doc->stage);
            colored_text(name, blocked && !broken ? ink.muted : ink.text);
            same_line_right_aligned(text_width(state) + design_px(12.0f));
            colored_text(state, broken ? ink.error : ink.muted);
            // Everything after a broken pass reads what it would have written,
            // so it is waiting on it rather than working.
            blocked = blocked || broken;
        }
        ImGui::Unindent(design_px(12.0f));
        const ImVec2 box_max(box_min.x + box_width, ImGui::GetCursorScreenPos().y + design_px(2.0f));
        draw_list->ChannelsSetCurrent(0);
        draw_list->AddRectFilled(box_min, box_max, ImGui::GetColorU32(ImGuiCol_Header),
                                 design_px(6.0f));
        draw_list->AddRect(box_min, box_max, ink.subtle, design_px(6.0f));
        draw_list->ChannelsMerge();
    }

}

}  // namespace ssstudio::gui
