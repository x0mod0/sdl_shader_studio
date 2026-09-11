// Sprite batching for the test scenes.
//
// The instance layout below is deliberately public: a user shader previewed in a
// scene consumes exactly this struct, and the same layout is what the docs
// describe — from shader reflection, not from this file. Nothing here is
// exported, and the batcher never influences a build.
#ifndef SSSTUDIO_GUI_BATCHER_H
#define SSSTUDIO_GUI_BATCHER_H

#include <cstdint>
#include <string>
#include <vector>

#include "scene/scene.h"

namespace ssstudio::gui {

// One sprite instance, tightly packed and matching the vertex layout the preview
// binds. Keep in sync with the instance struct shown in the Scene panel.
struct SpriteInstance {
    float transform[4];  // position.xy, scale.xy
    float rotation_z[2]; // rotation, z
    float uv_rect[4];    // u0, v0, du, dv
    float tint[4];
};

struct SpriteBatch {
    std::string material;         // shader id, empty == the preview default
    std::string texture;          // path, empty == white
    int layer = 0;
    bool additive = false;
    std::vector<SpriteInstance> instances;
};

// Flattens a scene into as few batches as possible: sorted by layer, then
// material, then texture, so a scene with one atlas becomes one draw.
std::vector<SpriteBatch> build_sprite_batches(const Scene& scene);

// Tiles expand into the same instance stream, so a tilemap costs one draw too.
std::vector<SpriteBatch> build_tilemap_batches(const Scene& scene);

// Post effects in execution order, skipping disabled ones.
std::vector<const PostEffect*> ordered_post_effects(const Scene& scene);

// The active camera's view parameters; falls back to a sensible default when a
// scene has no camera, rather than refusing to draw.
struct CameraView {
    float position[2] = {0.0f, 0.0f};
    float zoom = 1.0f;
    float rotation = 0.0f;
    bool pixel_perfect = false;
    bool is_3d = false;
};
CameraView active_camera(const Scene& scene);

}  // namespace ssstudio::gui

#endif  // SSSTUDIO_GUI_BATCHER_H
