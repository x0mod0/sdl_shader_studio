// The scene layer is a test harness. These tests cover its behaviour and, just
// as importantly, the boundary: nothing here may reach a build artifact.
#include <cmath>

#include "scene/batcher.h"
#include "scene/scene.h"
#include "test.h"

using namespace ssstudio;
using namespace ssstudio::gui;

namespace {

Entity sprite_entity(std::string id, float x, float y, const std::string& material = "sprite_frag",
                     int layer = 0, const std::string& texture = "atlas.png") {
    Entity e;
    e.id = std::move(id);
    e.name = e.id;
    e.transform.position[0] = x;
    e.transform.position[1] = y;

    Sprite s;
    s.material = material;
    s.texture = texture;
    s.layer = layer;
    e.sprite = std::move(s);
    return e;
}

bool near(float a, float b, float tolerance = 0.0001f) { return std::fabs(a - b) < tolerance; }

}  // namespace

TEST(scene_world_transform_applies_parents) {
    Scene scene;
    Entity parent;
    parent.id = "parent";
    parent.transform.position[0] = 10.0f;
    parent.transform.scale[0] = 2.0f;
    parent.transform.scale[1] = 2.0f;

    Entity child;
    child.id = "child";
    child.parent = "parent";
    child.transform.position[0] = 1.0f;
    child.transform.position[1] = 3.0f;

    scene.entities = {parent, child};
    const Transform world = scene.world_transform("child");
    CHECK(near(world.position[0], 12.0f));
    CHECK(near(world.position[1], 6.0f));
    CHECK(near(world.scale[0], 2.0f));
}

TEST(scene_world_transform_survives_a_parent_cycle) {
    Scene scene;
    Entity a;
    a.id = "a";
    a.parent = "b";
    Entity b;
    b.id = "b";
    b.parent = "a";
    scene.entities = {a, b};

    // The point is that this returns rather than hanging.
    const Transform world = scene.world_transform("a");
    CHECK(near(world.scale[0], 1.0f));
    CHECK(has_errors(scene.validate()));
}

TEST(scene_validation_catches_duplicates_and_bad_tilemaps) {
    Scene scene;
    Entity a = sprite_entity("dup", 0, 0);
    Entity b = sprite_entity("dup", 1, 1);
    scene.entities = {a, b};
    CHECK(has_errors(scene.validate()));

    Scene tilemap_scene;
    Entity tiles;
    tiles.id = "tiles";
    Tilemap map;
    map.width = 4;
    map.height = 4;
    map.tiles.resize(9);  // wrong count
    tiles.tilemap = std::move(map);
    tilemap_scene.entities = {tiles};
    CHECK(has_errors(tilemap_scene.validate()));
}

TEST(missing_camera_is_a_warning_not_an_error) {
    Scene scene;
    scene.entities = {sprite_entity("s", 0, 0)};
    const Diagnostics d = scene.validate();
    CHECK(!has_errors(d));
    CHECK(!d.empty());
}

TEST(roots_and_children_reflect_the_hierarchy) {
    Scene scene;
    Entity root = sprite_entity("root", 0, 0);
    Entity child = sprite_entity("child", 1, 0);
    child.parent = "root";
    Entity orphan = sprite_entity("orphan", 2, 0);
    orphan.parent = "ghost";  // parent does not exist: treated as a root
    scene.entities = {root, child, orphan};

    CHECK_EQ(scene.roots().size(), std::size_t{2});
    CHECK_EQ(scene.children_of("root").size(), std::size_t{1});
}

TEST(sprites_sharing_material_and_texture_become_one_batch) {
    Scene scene;
    for (int i = 0; i < 8; ++i) {
        scene.entities.push_back(sprite_entity("s" + std::to_string(i), static_cast<float>(i), 0));
    }
    const auto batches = build_sprite_batches(scene);
    CHECK_EQ(batches.size(), std::size_t{1});
    if (!batches.empty()) CHECK_EQ(batches[0].instances.size(), std::size_t{8});
}

TEST(different_materials_or_textures_split_batches) {
    Scene scene;
    scene.entities = {sprite_entity("a", 0, 0, "one"),
                      sprite_entity("b", 1, 0, "two"),
                      sprite_entity("c", 2, 0, "two", 0, "other.png")};
    CHECK_EQ(build_sprite_batches(scene).size(), std::size_t{3});
}

TEST(batches_are_ordered_by_layer) {
    Scene scene;
    scene.entities = {sprite_entity("top", 0, 0, "m", 5), sprite_entity("bottom", 1, 0, "m", 1)};
    const auto batches = build_sprite_batches(scene);
    CHECK_EQ(batches.size(), std::size_t{2});
    if (batches.size() == 2) CHECK(batches[0].layer < batches[1].layer);
}

TEST(disabled_entities_and_layers_do_not_draw) {
    Scene scene;
    Entity hidden = sprite_entity("hidden", 0, 0);
    hidden.enabled = false;
    scene.entities = {hidden, sprite_entity("shown", 1, 0)};
    CHECK_EQ(build_sprite_batches(scene)[0].instances.size(), std::size_t{1});

    Scene layered;
    layered.entities = {sprite_entity("a", 0, 0, "m", 2)};
    layered.layers.push_back({2, "off", {}, false, false, {}});
    CHECK(build_sprite_batches(layered).empty());
}

TEST(a_tilemap_becomes_a_single_batch_skipping_empty_cells) {
    Scene scene;
    Entity entity;
    entity.id = "map";
    Tilemap map;
    map.width = 4;
    map.height = 4;
    map.material = "tile_frag";
    map.tiles.assign(16, 0);
    map.tiles[0] = -1;  // empty
    map.tiles[5] = -1;
    entity.tilemap = std::move(map);
    scene.entities = {entity};

    const auto batches = build_tilemap_batches(scene);
    CHECK_EQ(batches.size(), std::size_t{1});
    if (!batches.empty()) CHECK_EQ(batches[0].instances.size(), std::size_t{14});
}

TEST(post_effects_run_in_order_and_skip_disabled_ones) {
    Scene scene;
    for (int i = 0; i < 3; ++i) {
        Entity e;
        e.id = "post" + std::to_string(i);
        PostEffect post;
        post.order = 2 - i;  // declared out of order on purpose
        post.enabled = i != 1;
        post.material = "effect" + std::to_string(i);
        e.post = std::move(post);
        scene.entities.push_back(std::move(e));
    }
    const auto effects = ordered_post_effects(scene);
    CHECK_EQ(effects.size(), std::size_t{2});
    if (effects.size() == 2) CHECK(effects[0]->order < effects[1]->order);
}

TEST(active_camera_falls_back_to_a_default) {
    Scene empty;
    const CameraView fallback = active_camera(empty);
    CHECK(near(fallback.zoom, 1.0f));
    CHECK(!fallback.is_3d);

    Scene scene;
    Entity camera;
    camera.id = "cam";
    camera.transform.position[0] = 4.0f;
    Camera2D c;
    c.zoom = 2.0f;
    camera.camera2d = c;
    scene.entities = {camera};

    const CameraView view = active_camera(scene);
    CHECK(near(view.position[0], 4.0f));
    CHECK(near(view.zoom, 2.0f));
}

TEST(a_3d_camera_wins_when_active) {
    Scene scene;
    Entity camera;
    camera.id = "cam";
    Camera3D c;
    c.active = true;
    camera.camera3d = c;
    scene.entities = {camera};
    CHECK(active_camera(scene).is_3d);
}

TEST(every_scene_template_is_valid_and_uses_the_given_material) {
    CHECK_EQ(scene_templates().size(), std::size_t{6});
    for (const auto& info : scene_templates()) {
        const Scene scene = make_scene_template(info.id, "my_shader");
        const Diagnostics d = scene.validate();
        CHECK(!has_errors(d));
        CHECK(!scene.entities.empty());

        bool references_material = false;
        for (const auto& e : scene.entities) {
            if (e.sprite && e.sprite->material == "my_shader") references_material = true;
            if (e.mesh && e.mesh->material == "my_shader") references_material = true;
            if (e.tilemap && e.tilemap->material == "my_shader") references_material = true;
        }
        // The post-stack template wires effects by name rather than a material.
        if (info.id != SceneTemplate::PostStack) CHECK(references_material);
    }
}

TEST(templates_batch_efficiently) {
    const Scene sheet = make_scene_template(SceneTemplate::SpriteSheet, "m");
    // Twelve sprites, one atlas, one material: one draw.
    CHECK_EQ(build_sprite_batches(sheet).size(), std::size_t{1});

    const Scene tilemap = make_scene_template(SceneTemplate::Tilemap, "m");
    CHECK_EQ(build_tilemap_batches(tilemap).size(), std::size_t{1});
}

// --- the boundary ----------------------------------------------------------
TEST(scenes_reference_shaders_only_by_id) {
    // A scene may name a shader, but it can never carry source, blobs or
    // anything the packer could pick up: the only link is a string id that the
    // project resolves. This is what keeps build output scene-independent.
    const Scene scene = make_scene_template(SceneTemplate::Lit2D, "lit_frag");
    for (const auto& entity : scene.entities) {
        if (!entity.sprite) continue;
        CHECK(entity.sprite->material == "lit_frag" || entity.sprite->material.empty());
    }
}
