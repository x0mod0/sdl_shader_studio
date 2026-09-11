#include "scene/batcher.h"

#include <algorithm>
#include <cmath>

namespace ssstudio::gui {
namespace {

SpriteInstance make_instance(const Transform& world, const float size[2], const float uv[4],
                             const float tint[4], const float pivot[2], bool flip_x, bool flip_y) {
    SpriteInstance instance{};
    instance.transform[0] = world.position[0] - (pivot[0] - 0.5f) * size[0] * world.scale[0];
    instance.transform[1] = world.position[1] - (pivot[1] - 0.5f) * size[1] * world.scale[1];
    instance.transform[2] = size[0] * world.scale[0] * (flip_x ? -1.0f : 1.0f);
    instance.transform[3] = size[1] * world.scale[1] * (flip_y ? -1.0f : 1.0f);
    instance.rotation_z[0] = world.rotation;
    instance.rotation_z[1] = world.z;
    for (int i = 0; i < 4; ++i) instance.uv_rect[i] = uv[i];
    for (int i = 0; i < 4; ++i) instance.tint[i] = tint[i];
    return instance;
}

SpriteBatch* find_batch(std::vector<SpriteBatch>& batches, const std::string& material,
                        const std::string& texture, int layer) {
    for (auto& batch : batches) {
        if (batch.material == material && batch.texture == texture && batch.layer == layer) {
            return &batch;
        }
    }
    return nullptr;
}

bool layer_is_additive(const Scene& scene, int layer) {
    for (const auto& l : scene.layers) {
        if (l.index == layer) return l.additive;
    }
    return false;
}

bool layer_enabled(const Scene& scene, int layer) {
    for (const auto& l : scene.layers) {
        if (l.index == layer) return l.enabled;
    }
    return true;  // an unlisted layer still draws; layers are a grouping, not a gate
}

void sort_batches(std::vector<SpriteBatch>& batches) {
    std::stable_sort(batches.begin(), batches.end(),
                     [](const SpriteBatch& a, const SpriteBatch& b) {
                         if (a.layer != b.layer) return a.layer < b.layer;
                         if (a.material != b.material) return a.material < b.material;
                         return a.texture < b.texture;
                     });
}

}  // namespace

std::vector<SpriteBatch> build_sprite_batches(const Scene& scene) {
    // Sprites are gathered per (layer, material, texture) and sorted within a
    // batch by sort key, so overlapping sprites draw in a predictable order.
    struct Pending {
        const Entity* entity;
        int sort_key;
    };
    std::vector<Pending> pending;
    for (const auto& entity : scene.entities) {
        if (!entity.enabled || !entity.sprite) continue;
        if (!layer_enabled(scene, entity.sprite->layer)) continue;
        pending.push_back({&entity, entity.sprite->sort_key});
    }
    std::stable_sort(pending.begin(), pending.end(),
                     [](const Pending& a, const Pending& b) { return a.sort_key < b.sort_key; });

    std::vector<SpriteBatch> batches;
    for (const auto& item : pending) {
        const Sprite& sprite = *item.entity->sprite;
        const std::string texture = sprite.texture.generic_string();
        SpriteBatch* batch = find_batch(batches, sprite.material, texture, sprite.layer);
        if (!batch) {
            SpriteBatch created;
            created.material = sprite.material;
            created.texture = texture;
            created.layer = sprite.layer;
            created.additive = layer_is_additive(scene, sprite.layer);
            batches.push_back(std::move(created));
            batch = &batches.back();
        }
        const Transform world = scene.world_transform(item.entity->id);
        batch->instances.push_back(make_instance(world, sprite.size, sprite.source_rect,
                                                 sprite.tint, sprite.pivot, sprite.flip_x,
                                                 sprite.flip_y));
    }
    sort_batches(batches);
    return batches;
}

std::vector<SpriteBatch> build_tilemap_batches(const Scene& scene) {
    std::vector<SpriteBatch> batches;
    for (const auto& entity : scene.entities) {
        if (!entity.enabled || !entity.tilemap) continue;
        const Tilemap& map = *entity.tilemap;
        if (map.tiles.size() != static_cast<std::size_t>(map.width * map.height)) continue;

        SpriteBatch batch;
        batch.material = map.material;
        batch.texture = map.tileset.generic_string();
        batch.layer = 0;
        batch.instances.reserve(map.tiles.size());

        const Transform world = scene.world_transform(entity.id);
        const float du = map.tileset_columns > 0 ? 1.0f / static_cast<float>(map.tileset_columns)
                                                 : 1.0f;

        for (int y = 0; y < map.height; ++y) {
            for (int x = 0; x < map.width; ++x) {
                const int tile = map.tiles[static_cast<std::size_t>(y * map.width + x)];
                if (tile < 0) continue;  // empty cell

                Transform cell = world;
                cell.position[0] += static_cast<float>(x) * map.tile_size[0] * world.scale[0];
                cell.position[1] += static_cast<float>(y) * map.tile_size[1] * world.scale[1];

                const int column = map.tileset_columns > 0 ? tile % map.tileset_columns : 0;
                const int row = map.tileset_columns > 0 ? tile / map.tileset_columns : 0;
                const float uv[4] = {static_cast<float>(column) * du,
                                     static_cast<float>(row) * du, du, du};
                const float tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                const float pivot[2] = {0.5f, 0.5f};
                batch.instances.push_back(
                    make_instance(cell, map.tile_size, uv, tint, pivot, false, false));
            }
        }
        if (!batch.instances.empty()) batches.push_back(std::move(batch));
    }
    sort_batches(batches);
    return batches;
}

std::vector<const PostEffect*> ordered_post_effects(const Scene& scene) {
    std::vector<const PostEffect*> effects;
    for (const auto& entity : scene.entities) {
        if (!entity.enabled || !entity.post || !entity.post->enabled) continue;
        effects.push_back(&*entity.post);
    }
    std::stable_sort(effects.begin(), effects.end(),
                     [](const PostEffect* a, const PostEffect* b) { return a->order < b->order; });
    return effects;
}

CameraView active_camera(const Scene& scene) {
    for (const auto& entity : scene.entities) {
        if (!entity.enabled) continue;
        if (entity.camera3d && entity.camera3d->active) {
            CameraView view;
            const Transform world = scene.world_transform(entity.id);
            view.position[0] = world.position[0];
            view.position[1] = world.position[1];
            view.is_3d = true;
            return view;
        }
    }
    for (const auto& entity : scene.entities) {
        if (!entity.enabled || !entity.camera2d || !entity.camera2d->active) continue;
        CameraView view;
        const Transform world = scene.world_transform(entity.id);
        view.position[0] = world.position[0];
        view.position[1] = world.position[1];
        view.zoom = entity.camera2d->zoom;
        view.rotation = entity.camera2d->rotation + world.rotation;
        view.pixel_perfect = entity.camera2d->pixel_perfect;
        return view;
    }
    return CameraView{};  // a scene without a camera still previews
}

}  // namespace ssstudio::gui
