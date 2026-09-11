#include "scene/scene.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace ssstudio::gui {
namespace {

Diagnostic error(std::string message) {
    Diagnostic d;
    d.severity = Severity::Error;
    d.code = "SSSTUDIO-SCENE";
    d.message = std::move(message);
    return d;
}

Diagnostic warning(std::string message) {
    Diagnostic d;
    d.severity = Severity::Warning;
    d.code = "SSSTUDIO-SCENE";
    d.message = std::move(message);
    return d;
}

Entity make_entity(std::string id, std::string name, float x = 0.0f, float y = 0.0f) {
    Entity e;
    e.id = std::move(id);
    e.name = name.empty() ? e.id : std::move(name);
    e.transform.position[0] = x;
    e.transform.position[1] = y;
    return e;
}

Sprite make_sprite(const std::string& material, float w = 1.0f, float h = 1.0f, int layer = 0) {
    Sprite s;
    s.material = material;
    s.size[0] = w;
    s.size[1] = h;
    s.layer = layer;
    return s;
}

Transform combine(const Transform& parent, const Transform& child) {
    Transform out;
    const float c = std::cos(parent.rotation);
    const float s = std::sin(parent.rotation);
    const float x = child.position[0] * parent.scale[0];
    const float y = child.position[1] * parent.scale[1];

    out.position[0] = parent.position[0] + x * c - y * s;
    out.position[1] = parent.position[1] + x * s + y * c;
    out.rotation = parent.rotation + child.rotation;
    out.scale[0] = parent.scale[0] * child.scale[0];
    out.scale[1] = parent.scale[1] * child.scale[1];
    out.z = parent.z + child.z;
    return out;
}

}  // namespace

std::string_view to_string(ComponentKind kind) {
    switch (kind) {
        case ComponentKind::Sprite: return "sprite";
        case ComponentKind::Camera2D: return "camera2d";
        case ComponentKind::Camera3D: return "camera3d";
        case ComponentKind::MeshRenderer: return "mesh";
        case ComponentKind::Light2D: return "light2d";
        case ComponentKind::PostEffect: return "post";
        case ComponentKind::Tilemap: return "tilemap";
        case ComponentKind::Text: return "text";
    }
    return "sprite";
}

std::optional<ComponentKind> component_kind_from_string(std::string_view s) {
    if (s == "sprite") return ComponentKind::Sprite;
    if (s == "camera2d") return ComponentKind::Camera2D;
    if (s == "camera3d") return ComponentKind::Camera3D;
    if (s == "mesh") return ComponentKind::MeshRenderer;
    if (s == "light2d") return ComponentKind::Light2D;
    if (s == "post") return ComponentKind::PostEffect;
    if (s == "tilemap") return ComponentKind::Tilemap;
    if (s == "text") return ComponentKind::Text;
    return std::nullopt;
}

bool Entity::has(ComponentKind kind) const {
    switch (kind) {
        case ComponentKind::Sprite: return sprite.has_value();
        case ComponentKind::Camera2D: return camera2d.has_value();
        case ComponentKind::Camera3D: return camera3d.has_value();
        case ComponentKind::MeshRenderer: return mesh.has_value();
        case ComponentKind::Light2D: return light.has_value();
        case ComponentKind::PostEffect: return post.has_value();
        case ComponentKind::Tilemap: return tilemap.has_value();
        case ComponentKind::Text: return text.has_value();
    }
    return false;
}

Entity* Scene::find(const std::string& id) {
    for (auto& e : entities) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

const Entity* Scene::find(const std::string& id) const {
    for (const auto& e : entities) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

std::vector<const Entity*> Scene::children_of(const std::string& id) const {
    std::vector<const Entity*> out;
    for (const auto& e : entities) {
        if (e.parent == id) out.push_back(&e);
    }
    return out;
}

std::vector<const Entity*> Scene::roots() const {
    std::vector<const Entity*> out;
    for (const auto& e : entities) {
        if (e.parent.empty() || !find(e.parent)) out.push_back(&e);
    }
    return out;
}

Transform Scene::world_transform(const std::string& id) const {
    const Entity* entity = find(id);
    if (!entity) return Transform{};

    // Walk to the root first, guarding against a parent cycle so a malformed
    // scene file cannot hang the editor.
    std::vector<const Entity*> chain;
    std::set<std::string> seen;
    const Entity* current = entity;
    while (current) {
        if (!seen.insert(current->id).second) break;
        chain.push_back(current);
        current = current->parent.empty() ? nullptr : find(current->parent);
    }

    Transform result;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        result = combine(result, (*it)->transform);
    }
    return result;
}

Diagnostics Scene::validate() const {
    Diagnostics out;
    std::set<std::string> ids;
    for (const auto& e : entities) {
        if (e.id.empty()) out.push_back(error("an entity has no id"));
        else if (!ids.insert(e.id).second) out.push_back(error("duplicate entity id '" + e.id + "'"));
        if (!e.parent.empty() && !find(e.parent)) {
            out.push_back(warning("entity '" + e.id + "' names a parent that does not exist ('" +
                                  e.parent + "'); it will be treated as a root"));
        }
    }

    for (const auto& e : entities) {
        std::set<std::string> seen;
        const Entity* current = &e;
        while (current && !current->parent.empty()) {
            if (!seen.insert(current->id).second) {
                out.push_back(error("parent cycle involving entity '" + e.id + "'"));
                break;
            }
            current = find(current->parent);
        }
    }

    const bool has_camera =
        std::any_of(entities.begin(), entities.end(), [](const Entity& e) {
            return (e.camera2d && e.camera2d->active) || (e.camera3d && e.camera3d->active);
        });
    if (!has_camera) {
        out.push_back(warning("this scene has no active camera; the preview will use a default one"));
    }

    for (const auto& e : entities) {
        if (e.tilemap && e.tilemap->tiles.size() !=
                             static_cast<std::size_t>(e.tilemap->width * e.tilemap->height)) {
            out.push_back(error("tilemap on '" + e.id + "' has " +
                                std::to_string(e.tilemap->tiles.size()) + " tiles but is " +
                                std::to_string(e.tilemap->width) + "x" +
                                std::to_string(e.tilemap->height)));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Templates
// ---------------------------------------------------------------------------
const std::vector<SceneTemplateInfo>& scene_templates() {
    static const std::vector<SceneTemplateInfo> list = {
        {SceneTemplate::SpriteSheet, "Sprite sheet",
         "A grid of sprites sharing one texture, for testing UV maths and atlas bleeding."},
        {SceneTemplate::Tilemap, "Tilemap",
         "A 16x16 tile grid drawn as one instanced batch."},
        {SceneTemplate::ParallaxLayers, "Parallax layers",
         "Three scrolling layers at different speeds, for scroll and wrap behaviour."},
        {SceneTemplate::Lit2D, "Lit 2D",
         "Two point lights over sprites, feeding a light buffer your shader can read."},
        {SceneTemplate::PostStack, "Post stack",
         "A simple scene behind an ordered chain of fullscreen effects."},
        {SceneTemplate::SpinningMesh, "Spinning mesh",
         "A rotating cube with an orbit camera, for 3D transform and normal checks."},
    };
    return list;
}

Scene make_scene_template(SceneTemplate which, const std::string& material) {
    Scene scene;

    Entity camera = make_entity("camera", "Camera");
    camera.camera2d = Camera2D{};
    scene.entities.push_back(camera);
    scene.layers.push_back({0, "main", "camera", false, true, {}});

    switch (which) {
        case SceneTemplate::SpriteSheet: {
            scene.name = "Sprite sheet";
            for (int y = 0; y < 3; ++y) {
                for (int x = 0; x < 4; ++x) {
                    Entity e = make_entity("sprite_" + std::to_string(y * 4 + x), "Sprite",
                                           static_cast<float>(x) * 1.2f - 1.8f,
                                           static_cast<float>(y) * 1.2f - 1.2f);
                    Sprite sprite = make_sprite(material);
                    sprite.source_rect[0] = static_cast<float>(x) * 0.25f;
                    sprite.source_rect[1] = static_cast<float>(y) / 3.0f;
                    sprite.source_rect[2] = 0.25f;
                    sprite.source_rect[3] = 1.0f / 3.0f;
                    sprite.sort_key = y * 4 + x;
                    e.sprite = sprite;
                    scene.entities.push_back(std::move(e));
                }
            }
            break;
        }
        case SceneTemplate::Tilemap: {
            scene.name = "Tilemap";
            Entity e = make_entity("tilemap", "Tilemap");
            Tilemap map;
            map.material = material;
            map.tiles.resize(static_cast<std::size_t>(map.width * map.height));
            for (int y = 0; y < map.height; ++y) {
                for (int x = 0; x < map.width; ++x) {
                    const bool border = x == 0 || y == 0 || x == map.width - 1 || y == map.height - 1;
                    map.tiles[static_cast<std::size_t>(y * map.width + x)] =
                        border ? 1 : ((x + y) % 5 == 0 ? 2 : 0);
                }
            }
            e.tilemap = std::move(map);
            scene.entities.push_back(std::move(e));
            break;
        }
        case SceneTemplate::ParallaxLayers: {
            scene.name = "Parallax layers";
            const float speeds[] = {0.25f, 0.5f, 1.0f};
            for (int i = 0; i < 3; ++i) {
                Entity e = make_entity("layer_" + std::to_string(i), "Layer " + std::to_string(i),
                                       0.0f, static_cast<float>(i) * 0.1f);
                Sprite sprite = make_sprite(material, 8.0f, 4.0f, i);
                sprite.tint[0] = sprite.tint[1] = sprite.tint[2] = 0.5f + 0.25f * static_cast<float>(i);
                e.sprite = sprite;
                e.tags.push_back("parallax");
                e.tags.push_back("speed=" + std::to_string(speeds[i]));
                scene.entities.push_back(std::move(e));
                scene.layers.push_back({i, "parallax " + std::to_string(i), "camera", false, true, {}});
            }
            break;
        }
        case SceneTemplate::Lit2D: {
            scene.name = "Lit 2D";
            Entity ground = make_entity("ground", "Ground", 0.0f, -1.0f);
            ground.sprite = make_sprite(material, 8.0f, 2.0f, 0);
            scene.entities.push_back(std::move(ground));

            for (int i = 0; i < 2; ++i) {
                Entity light = make_entity("light_" + std::to_string(i),
                                           "Light " + std::to_string(i),
                                           i == 0 ? -1.5f : 1.5f, 1.0f);
                Light2D l;
                l.color[0] = i == 0 ? 1.0f : 0.4f;
                l.color[1] = i == 0 ? 0.8f : 0.6f;
                l.color[2] = i == 0 ? 0.5f : 1.0f;
                l.radius = 4.0f;
                light.light = l;
                scene.entities.push_back(std::move(light));
            }
            break;
        }
        case SceneTemplate::PostStack: {
            scene.name = "Post stack";
            Entity subject = make_entity("subject", "Subject");
            subject.sprite = make_sprite(material, 3.0f, 3.0f, 0);
            scene.entities.push_back(std::move(subject));

            const char* names[] = {"blur", "bloom", "grade"};
            for (int i = 0; i < 3; ++i) {
                Entity effect = make_entity(std::string("post_") + names[i], names[i]);
                PostEffect post;
                post.order = i;
                post.enabled = i == 0;  // start with one on, so the chain is obvious
                effect.post = post;
                scene.entities.push_back(std::move(effect));
            }
            break;
        }
        case SceneTemplate::SpinningMesh: {
            scene.name = "Spinning mesh";
            scene.entities.front().camera2d.reset();
            scene.entities.front().camera3d = Camera3D{};
            scene.entities.front().camera3d->active = true;

            Entity cube = make_entity("cube", "Cube");
            MeshRenderer mesh;
            mesh.mesh = "cube";
            mesh.material = material;
            cube.mesh = mesh;
            cube.tags.push_back("spin");
            scene.entities.push_back(std::move(cube));
            break;
        }
    }
    return scene;
}

}  // namespace ssstudio::gui
