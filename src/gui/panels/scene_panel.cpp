// Scene panel — the test harness UI.
//
// It says plainly, in the panel itself, that scenes never reach a build. That is
// not decoration: someone will eventually wonder whether their scene ships, and
// the answer should be visible where they are working.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <cfloat>

#include <imgui.h>

#include "app.h"
#include "panel_common.h"
#include "scene/batcher.h"
#include "scene/scene.h"

namespace ssstudio::gui {
namespace {

std::string unique_entity_id(const Scene& scene, const std::string& base) {
    if (!scene.find(base)) return base;
    for (int i = 2; i < 1000; ++i) {
        const std::string candidate = base + "_" + std::to_string(i);
        if (!scene.find(candidate)) return candidate;
    }
    return base + "_x";
}

void draw_hierarchy_node(Scene& scene, const Entity& entity, std::string& selected) {
    const auto children = scene.children_of(entity.id);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen;
    if (children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
    if (selected == entity.id) flags |= ImGuiTreeNodeFlags_Selected;

    ImGui::PushID(entity.id.c_str());
    const bool open = ImGui::TreeNodeEx(entity.name.c_str(), flags);
    if (ImGui::IsItemClicked()) selected = entity.id;

    if (!entity.enabled) {
        ImGui::SameLine();
        ImGui::TextDisabled("(off)");
    }
    if (open) {
        for (const Entity* child : children) draw_hierarchy_node(scene, *child, selected);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void draw_entity_inspector(App& app, Scene& scene, const std::string& selected, bool& changed) {
    Entity* entity = scene.find(selected);
    if (!entity) {
        ImGui::TextDisabled("Select an entity.");
        return;
    }

    char name[96];
    std::snprintf(name, sizeof(name), "%s", entity->name.c_str());
    if (ImGui::InputText(left_label("Name").c_str(), name, sizeof(name))) {
        entity->name = name;
        changed = true;
    }
    if (ImGui::Checkbox("Enabled", &entity->enabled)) changed = true;

    ImGui::SeparatorText("Transform");
    changed |= ImGui::DragFloat2(left_label("Position").c_str(), entity->transform.position, 0.01f);
    changed |= ImGui::DragFloat2(left_label("Scale").c_str(), entity->transform.scale, 0.01f);
    changed |= ImGui::SliderAngle(left_label("Rotation").c_str(), &entity->transform.rotation);
    changed |= ImGui::DragFloat(left_label("Z").c_str(), &entity->transform.z, 0.01f);

    // Material pickers list the project's shaders, so a scene can only ever name
    // a shader that exists.
    auto material_combo = [&](const char* label, std::string& material, Stage stage) {
        // No width of its own: ImGui's default leaves room for the label to the
        // right of the field, which is where every other row in this inspector
        // puts one. Filling the width instead pushed the label off the edge, and
        // it stayed off however the window was resized, because the field always
        // took all of it.
        if (!ImGui::BeginCombo(left_label(label).c_str(),
                               material.empty() ? "(preview default)" : material.c_str())) {
            return;
        }
        if (ImGui::Selectable("(preview default)", material.empty())) {
            material.clear();
            changed = true;
        }
        for (const auto& shader : app.project().shaders) {
            if (shader.stage != stage) continue;
            if (ImGui::Selectable(shader.id.c_str(), shader.id == material)) {
                material = shader.id;
                changed = true;
            }
        }
        ImGui::EndCombo();
    };

    if (entity->sprite) {
        ImGui::SeparatorText("Sprite");
        Sprite& sprite = *entity->sprite;
        material_combo("Material", sprite.material, Stage::Fragment);
        changed |= ImGui::DragFloat2(left_label("Size").c_str(), sprite.size, 0.01f);
        changed |= ImGui::DragFloat4(left_label("UV rect").c_str(), sprite.source_rect, 0.005f);
        changed |= color_field("Tint", sprite.tint, 4);
        changed |= ImGui::DragFloat2(left_label("Pivot").c_str(), sprite.pivot, 0.01f, 0.0f, 1.0f);
        changed |= ImGui::DragInt(left_label("Layer").c_str(), &sprite.layer);
        changed |= ImGui::DragInt(left_label("Sort key").c_str(), &sprite.sort_key);
        FlowLayout flips;
        flips.next(checkbox_width("flip x"));
        changed |= ImGui::Checkbox("Flip x", &sprite.flip_x);
        flips.next(checkbox_width("flip y"));
        changed |= ImGui::Checkbox("Flip y", &sprite.flip_y);
    }
    if (entity->camera2d) {
        ImGui::SeparatorText("Camera 2D");
        changed |= ImGui::DragFloat(left_label("Zoom").c_str(), &entity->camera2d->zoom, 0.01f, 0.01f, 64.0f);
        changed |= ImGui::SliderAngle(left_label("Camera rotation").c_str(), &entity->camera2d->rotation);
        changed |= ImGui::Checkbox("Pixel perfect", &entity->camera2d->pixel_perfect);
        changed |= ImGui::Checkbox("Active", &entity->camera2d->active);
    }
    if (entity->camera3d) {
        ImGui::SeparatorText("Camera 3D");
        changed |= ImGui::DragFloat(left_label("FOV").c_str(), &entity->camera3d->fov_degrees, 0.5f, 10.0f, 170.0f);
        changed |= ImGui::DragFloat(left_label("Orbit distance").c_str(), &entity->camera3d->orbit_distance, 0.05f);
        changed |= ImGui::SliderAngle(left_label("Yaw").c_str(), &entity->camera3d->orbit_yaw);
        changed |= ImGui::SliderAngle(left_label("Pitch").c_str(), &entity->camera3d->orbit_pitch);
        changed |= ImGui::Checkbox("Orthographic", &entity->camera3d->orthographic);
        changed |= ImGui::Checkbox("Active##3d", &entity->camera3d->active);
    }
    if (entity->mesh) {
        ImGui::SeparatorText("Mesh");
        const char* meshes[] = {"cube", "sphere", "plane", "quad"};
        int current = 0;
        for (int i = 0; i < IM_ARRAYSIZE(meshes); ++i) {
            if (entity->mesh->mesh == meshes[i]) current = i;
        }
        if (ImGui::Combo(left_label("Mesh").c_str(), &current, meshes, IM_ARRAYSIZE(meshes))) {
            entity->mesh->mesh = meshes[current];
            changed = true;
        }
        material_combo("Mesh material", entity->mesh->material, Stage::Fragment);
        changed |= ImGui::Checkbox("Wireframe", &entity->mesh->wireframe);
    }
    if (entity->light) {
        ImGui::SeparatorText("Light 2D");
        changed |= color_field("Color", entity->light->color, 3);
        changed |= ImGui::DragFloat(left_label("Intensity").c_str(), &entity->light->intensity, 0.01f, 0.0f, 20.0f);
        changed |= ImGui::DragFloat(left_label("Radius").c_str(), &entity->light->radius, 0.05f, 0.0f, 100.0f);
        changed |= ImGui::SliderAngle(left_label("Cone").c_str(), &entity->light->cone_degrees, 0.0f, 180.0f);
    }
    if (entity->post) {
        ImGui::SeparatorText("Post effect");
        material_combo("Effect", entity->post->material, Stage::Fragment);
        changed |= ImGui::DragInt(left_label("Order").c_str(), &entity->post->order);
        changed |= ImGui::Checkbox("Enabled##post", &entity->post->enabled);
    }
    if (entity->tilemap) {
        ImGui::SeparatorText("Tilemap");
        material_combo("Tile material", entity->tilemap->material, Stage::Fragment);
        ImGui::Text("%d x %d tiles", entity->tilemap->width, entity->tilemap->height);
        changed |= ImGui::DragFloat2(left_label("Tile size").c_str(), entity->tilemap->tile_size, 0.01f);
        changed |= ImGui::DragInt(left_label("Tileset columns").c_str(), &entity->tilemap->tileset_columns, 1, 1, 64);
    }
    if (entity->text) {
        ImGui::SeparatorText("Text");
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer), "%s", entity->text->text.c_str());
        if (ImGui::InputText(left_label("Text").c_str(), buffer, sizeof(buffer))) {
            entity->text->text = buffer;
            changed = true;
        }
        changed |= ImGui::DragFloat(left_label("Text size").c_str(), &entity->text->size, 0.01f);
        changed |= color_field("Text color", entity->text->color, 4);
    }

    ImGui::SeparatorText("Components");
    // A row of "+sprite  +camera  +light ..." buttons that folds rather than
    // running past the inspector's right edge.
    FlowLayout adders;
    auto add_component = [&](const char* label, bool present, auto&& adder) {
        if (present) return;
        adders.next(button_width(label));
        if (ImGui::SmallButton(label)) {
            adder();
            changed = true;
        }
    };
    add_component("+sprite", entity->sprite.has_value(), [&] { entity->sprite = Sprite{}; });
    add_component("+camera", entity->camera2d.has_value(), [&] { entity->camera2d = Camera2D{}; });
    add_component("+light", entity->light.has_value(), [&] { entity->light = Light2D{}; });
    add_component("+post", entity->post.has_value(), [&] { entity->post = PostEffect{}; });
    add_component("+mesh", entity->mesh.has_value(), [&] { entity->mesh = MeshRenderer{}; });
    // The row used to end with a dangling SameLine that a NewLine had to close;
    // the flow layout leaves the cursor on a fresh line, so all that is wanted
    // here is a gap before the destructive button.
    ImGui::Spacing();

    if (ImGui::Button("Delete entity")) {
        scene.entities.erase(std::remove_if(scene.entities.begin(), scene.entities.end(),
                                            [&](const Entity& e) { return e.id == selected; }),
                             scene.entities.end());
        changed = true;
    }
}

}  // namespace

void draw_scene_panel(App& app) {
    // A window of its own, like the graph editor: the scene harness is something
    // you set up beside the preview it drives, not a tab that replaces it.
    // NoAutoMerge asks the platform for a real window, and has to go through a
    // window class because ImGui decides whether to create a viewport before it
    // reads the window's own flags.
    ImGuiWindowClass standalone;
    standalone.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
    ImGui::SetNextWindowClass(&standalone);

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.4f, viewport->WorkSize.y * 0.6f),
                             ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
               viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
        ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(viewport->WorkSize.x * 0.2f, viewport->WorkSize.y * 0.2f),
        ImVec2(FLT_MAX, FLT_MAX));

    PanelScope panel("Scene", &app.show_scene, ImGuiWindowFlags_NoDocking);
    if (!panel) return;
    if (!app.project_open()) {
        ImGui::TextDisabled("No project open.");
        return;
    }

    ImGui::TextDisabled(
        "Scenes are a test harness. They are never packed, exported, or referenced by generated "
        "code, and the build produces identical output with or without them.");
    ImGui::Separator();

    Scene* scene = app.active_scene();

    if (!scene) {
        ImGui::TextWrapped("Start from a scenario:");
        // Button then description, side by side while the panel is wide enough
        // for the description to be worth reading, stacked when it is not.
        const float button = fitted_width(160.0f, 90.0f);
        for (const auto& info : scene_templates()) {
            if (ImGui::Button(info.name, ImVec2(button, 0.0f))) {
                app.create_scene_from_template(info.id);
            }
            const float room = ImGui::GetContentRegionAvail().x;
            if (room > ImGui::GetFontSize() * 8.0f) ImGui::SameLine();
            ImGui::TextWrapped("%s", info.description);
        }

        const auto existing = app.available_scenes();
        if (!existing.empty()) {
            ImGui::SeparatorText("Saved scenes");
            for (const auto& path : existing) {
                if (ImGui::Selectable(path.filename().string().c_str())) app.open_scene(path);
            }
        }
        return;
    }

    // --- toolbar -----------------------------------------------------------
    FlowLayout row;
    row.next(text_width(scene->name.c_str()));
    ImGui::Text("%s", scene->name.c_str());
    row.next(button_width("Save"));
    if (ImGui::SmallButton("Save")) app.save_active_scene();
    row.next(button_width("Close"));
    if (ImGui::SmallButton("Close")) {
        app.close_scene();
        return;
    }
    row.next(button_width("Add entity"));
    if (ImGui::SmallButton("Add entity")) {
        Entity entity;
        entity.id = unique_entity_id(*scene, "entity");
        entity.name = entity.id;
        entity.sprite = Sprite{};
        scene->entities.push_back(std::move(entity));
        app.mark_scene_dirty();
    }

    bool changed = false;
    // Per project: the id names an entity in this project's scene.
    std::string& selected = app.scene_selected_entity();

    // The hierarchy takes the width the user gave it and the inspector takes what
    // is left, with a divider between them. Kept for the session rather than
    // saved: it is a working preference, not part of the scene.
    static float hierarchy_width = 0.0f;
    constexpr float kMinSide = 120.0f;
    constexpr float kMinInspector = 260.0f;
    constexpr float kSplitter = 6.0f;

    const float total_width = ImGui::GetContentRegionAvail().x;
    const float body_height = ImGui::GetContentRegionAvail().y;
    // First frame only: start where the old fixed rule put it, so the panel
    // opens looking the way it always has.
    if (hierarchy_width <= 0.0f) hierarchy_width = side_pane_width(200.0f, 0.42f);
    // Clamped every frame, not only while dragging: the window can be made
    // narrower than the hierarchy was, and the inspector must not be squeezed
    // out of existence when it is.
    // The floor gives way before the inspector does: on a window too narrow for
    // both at their minimum, they share what there is rather than one of them
    // being pushed off the edge.
    const float available = std::max(0.0f, total_width - kSplitter);
    const float floor_side = std::min(kMinSide, available * 0.5f);
    const float max_side = std::max(floor_side, available - kMinInspector);
    hierarchy_width = std::clamp(hierarchy_width, floor_side, max_side);

    ImGui::BeginChild("##hierarchy", ImVec2(hierarchy_width, 0), true);
    ImGui::PushTextWrapPos(0.0f);
    for (const Entity* root : scene->roots()) draw_hierarchy_node(*scene, *root, selected);
    ImGui::PopTextWrapPos();
    ImGui::EndChild();

    ImGui::SameLine(0.0f, 0.0f);
    vertical_splitter("##split_hierarchy", body_height, hierarchy_width, floor_side, max_side,
                      1.0f);

    ImGui::SameLine(0.0f, 0.0f);
    ImGui::BeginChild("##inspector", ImVec2(0, 0), true);
    ImGui::PushTextWrapPos(0.0f);
    draw_entity_inspector(app, *scene, selected, changed);

    ImGui::SeparatorText("Batching");
    const auto sprite_batches = build_sprite_batches(*scene);
    const auto tile_batches = build_tilemap_batches(*scene);
    std::size_t instances = 0;
    for (const auto& batch : sprite_batches) instances += batch.instances.size();
    for (const auto& batch : tile_batches) instances += batch.instances.size();
    ImGui::Text("%zu draw(s), %zu instance(s)", sprite_batches.size() + tile_batches.size(),
                instances);
    ImGui::TextDisabled(
        "Instances are packed as position.xy, scale.xy, rotation, z, uv rect, tint. A material "
        "shader in this scene consumes exactly that layout.");

    const auto effects = ordered_post_effects(*scene);
    if (!effects.empty()) {
        ImGui::SeparatorText("Post stack");
        for (const auto* effect : effects) {
            ImGui::BulletText("%d: %s", effect->order,
                              effect->material.empty() ? "(none)" : effect->material.c_str());
        }
    }

    const Diagnostics diagnostics = scene->validate();
    if (!diagnostics.empty()) {
        ImGui::SeparatorText("Problems");
        for (const auto& d : diagnostics) {
            const ImVec4 color = theme_vec4(app.theme().diagnostic(d.severity));
            ImGui::TextColored(color, "%s", d.message.c_str());
        }
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();

    if (changed) app.mark_scene_dirty();
}

}  // namespace ssstudio::gui
