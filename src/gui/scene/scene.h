// Scene layer — a TEST HARNESS, not a runtime.
//
// This header lives under src/gui on purpose. The core library, the packer, the
// code generator and the CLI cannot see it, which is the mechanical guarantee
// behind the rule that build output is identical whether or not a project
// contains scenes. Nothing here is ever packed, exported, or referenced by
// generated code; scenes exist so a shader can be tried in a realistic 2D setup
// without leaving the tool, and thrown away afterwards.
#ifndef SSSTUDIO_GUI_SCENE_H
#define SSSTUDIO_GUI_SCENE_H

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ssstudio/types.h"

namespace ssstudio::gui {

struct Transform {
    float position[2] = {0.0f, 0.0f};
    float rotation = 0.0f;  // radians
    float scale[2] = {1.0f, 1.0f};
    float z = 0.0f;         // depth within a layer, used for sorting
};

enum class ComponentKind : std::uint8_t {
    Sprite,
    Camera2D,
    Camera3D,
    MeshRenderer,
    Light2D,
    PostEffect,
    Tilemap,
    Text,
};

std::string_view to_string(ComponentKind kind);
std::optional<ComponentKind> component_kind_from_string(std::string_view s);

struct Sprite {
    std::filesystem::path texture;
    float source_rect[4] = {0.0f, 0.0f, 1.0f, 1.0f};  // uv rect
    float tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float pivot[2] = {0.5f, 0.5f};
    float size[2] = {1.0f, 1.0f};
    bool flip_x = false;
    bool flip_y = false;
    int layer = 0;
    int sort_key = 0;
    std::string material;  // shader id from the project, empty == default
};

struct Camera2D {
    float zoom = 1.0f;
    float rotation = 0.0f;
    float viewport[4] = {0.0f, 0.0f, 1.0f, 1.0f};  // fractions of the target
    bool pixel_perfect = false;
    bool active = true;
};

struct Camera3D {
    float fov_degrees = 60.0f;
    float near_plane = 0.1f;
    float far_plane = 100.0f;
    bool orthographic = false;
    float orbit_distance = 4.0f;
    float orbit_yaw = 0.0f;
    float orbit_pitch = 0.3f;
    bool active = false;
};

struct MeshRenderer {
    std::string mesh = "cube";  // cube | sphere | plane | quad
    std::string material;
    bool wireframe = false;
};

struct Light2D {
    float color[3] = {1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float radius = 3.0f;
    float angle = 0.0f;      // 0 == point light
    float cone_degrees = 45.0f;
};

struct PostEffect {
    std::string material;    // fragment shader id from the project
    int order = 0;
    bool enabled = true;
};

struct Tilemap {
    int width = 16;
    int height = 16;
    float tile_size[2] = {1.0f, 1.0f};
    int tileset_columns = 8;
    std::filesystem::path tileset;
    std::string material;
    std::vector<int> tiles;  // width*height indices, -1 == empty
};

struct TextComponent {
    std::string text = "Hello";
    float size = 1.0f;
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    std::filesystem::path font;  // bitmap font atlas
};

struct Entity {
    std::string id;
    std::string name;
    std::string parent;  // empty == root
    std::vector<std::string> tags;
    Transform transform;
    bool enabled = true;

    std::optional<Sprite> sprite;
    std::optional<Camera2D> camera2d;
    std::optional<Camera3D> camera3d;
    std::optional<MeshRenderer> mesh;
    std::optional<Light2D> light;
    std::optional<PostEffect> post;
    std::optional<Tilemap> tilemap;
    std::optional<TextComponent> text;

    bool has(ComponentKind kind) const;
};

struct RenderLayer {
    int index = 0;
    std::string name;
    std::string camera;      // entity id, empty == the first active camera
    bool additive = false;
    bool enabled = true;
    std::string target;      // empty == the main preview target
};

struct Scene {
    std::string name = "Untitled scene";
    std::filesystem::path path;      // scenes/<name>.toml inside the project
    float clear_color[4] = {0.05f, 0.05f, 0.07f, 1.0f};
    float gravity[2] = {0.0f, 0.0f};  // purely informational; no physics here
    std::vector<Entity> entities;
    std::vector<RenderLayer> layers;

    Entity* find(const std::string& id);
    const Entity* find(const std::string& id) const;
    std::vector<const Entity*> children_of(const std::string& id) const;
    std::vector<const Entity*> roots() const;

    // World transform with parents applied. Cycles in the parent chain are
    // broken rather than hung on.
    Transform world_transform(const std::string& id) const;

    Diagnostics validate() const;
};

// ---------------------------------------------------------------------------
// Built-in scenarios. One click to set up, easy to discard.
// ---------------------------------------------------------------------------
enum class SceneTemplate {
    SpriteSheet,
    Tilemap,
    ParallaxLayers,
    Lit2D,
    PostStack,
    SpinningMesh,
};

struct SceneTemplateInfo {
    SceneTemplate id;
    const char* name;
    const char* description;
};

const std::vector<SceneTemplateInfo>& scene_templates();
Scene make_scene_template(SceneTemplate which, const std::string& material_shader_id);

// TOML persistence under <project>/scenes/. Loading and saving a scene never
// touches project.toml, so a scene cannot leak into a build profile.
bool load_scene(const std::filesystem::path& path, Scene& out, Diagnostics& out_diags);
bool save_scene(const Scene& scene, Diagnostics& out_diags);
std::vector<std::filesystem::path> list_scenes(const std::filesystem::path& project_root);

}  // namespace ssstudio::gui

#endif  // SSSTUDIO_GUI_SCENE_H
