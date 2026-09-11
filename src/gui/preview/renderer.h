// Offscreen preview: builds pipelines from the freshly compiled SPIR-V and
// renders into a texture the Preview panel displays.
//
// Everything here is preview-only. Nothing it produces is packed, exported or
// referenced by generated code (see the plan, section 14).
#ifndef SSSTUDIO_GUI_RENDERER_H
#define SSSTUDIO_GUI_RENDERER_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "preview/texture_registry.h"

#include "ssstudio/pass_graph.h"
#include "ssstudio/project.h"
#include "ssstudio/reflection.h"
#include "ssstudio/types.h"

namespace ssstudio::gui {

// Uniform values assembled from the I/O panel's bindings for one draw.
struct UniformFeed {
    std::vector<std::uint8_t> vertex_bytes;
    std::vector<std::uint8_t> fragment_bytes;
    bool vertex_present = false;
    bool fragment_present = false;
};

/// Everything one pass of a schedule needs beyond the schedule itself.
struct PassFeed {
    UniformFeed uniforms;
    /// Textures for this pass, indexed by reflected sampler slot. A slot the
    /// schedule fills from another pass's target overrides whatever is here.
    std::vector<ResolvedTexture> textures;
};

class PreviewRenderer {
public:
    /// `registry` is where the pool publishes its targets. It may be null, in
    /// which case a pass simply cannot sample another pass's output.
    bool init(SDL_GPUDevice* device, TextureRegistry* registry = nullptr);
    void shutdown();
    void clear();

    bool ready() const { return device_ != nullptr; }
    const std::string& last_error() const { return last_error_; }

    // The one binary format this device can actually load. Only Vulkan takes the
    // SPIR-V the front end produces; Metal wants MSL and D3D12 wants DXIL, so a
    // compile is asked for this format alongside SPIR-V and the result is what
    // set_shader() is handed. Meaningless before a successful init().
    ShaderFormat preview_format() const { return preview_format_; }

    // Replaces a stage's shader. `code` is a blob in preview_format(). Pipelines
    // are rebuilt lazily on the next draw.
    void set_shader(const std::string& id, Stage stage, const std::vector<std::uint8_t>& code,
                    const Reflection& reflection);
    void remove_shader(const std::string& id);

    // Chooses which compiled shaders form the previewed pipeline.
    void set_active(const std::string& vertex_id, const std::string& fragment_id);
    void set_active_compute(const std::string& compute_id);

    bool resize(int width, int height);
    int width() const { return width_; }
    int height() const { return height_; }

    // Renders one frame into the offscreen target. Returns false and fills
    // last_error() when the pipeline could not be built.
    /// Renders one frame into the offscreen target. `textures` is indexed by
    /// reflected binding slot; any slot it does not cover is bound to the white
    /// stand-in. Returns false and fills last_error() when the pipeline could
    /// not be built.
    bool render(const PassSchedule& schedule, const std::map<std::string, PassFeed>& feeds,
                const PreviewSettings& settings);

    /// Marks every target that asked for it to be cleared before the next frame.
    ///
    /// What Restart does. A buffer is not cleared each frame - it could not
    /// accumulate if it were - so without this a chain that has converged on
    /// something stays converged on it however many times you press the button.
    void restart();

    /// Which target the panel displays and the pixel inspector reads.
    ///
    /// Defaults to whatever the schedule presents. Pointing it at a buffer is
    /// how you see what an intermediate pass is actually producing, which is
    /// most of what debugging a chain consists of.
    void set_inspected_target(std::uint32_t index);
    std::uint32_t inspected_target() const { return inspect_target_; }

    /// What each target in the pool belongs to, for the inspector's list. Index
    /// matches the schedule's target indices; an empty name is the image.
    struct TargetLabel {
        std::string owner_pass;
        std::uint32_t copy = 0;
        PassFormat format = PassFormat::Rgba8Unorm;
    };
    std::vector<TargetLabel> target_labels() const;

    // The offscreen target, as the ImGui SDL_GPU backend wants it: since 1.92.2
    // that backend reads an ImTextureID as an SDL_GPUTexture* and pairs it with
    // its own sampler. Null until there is a target to show.
    SDL_GPUTexture* texture_id();

    // Reads a single pixel back for the inspector; slow, called on demand only.
    bool read_pixel(int x, int y, float out_rgba[4]);

    const std::string& status() const { return status_; }

private:
    struct ShaderSlot {
        Stage stage = Stage::Fragment;
        SDL_GPUShader* shader = nullptr;
        Reflection reflection;
        bool dirty = true;
    };

    /// What distinguishes one pipeline from another.
    ///
    /// The target's format is in here because it is baked into the pipeline: the
    /// same shader pair drawing into a float buffer and into the presented
    /// eight-bit image needs two, and handing SDL a pipeline built for the wrong
    /// one is not something it forgives.
    struct PipelineKey {
        std::string vertex;
        std::string fragment;
        PassFormat format = PassFormat::Rgba8Unorm;
        bool blend = false;

        bool operator<(const PipelineKey& other) const;
    };

    /// One target the pool holds, matching a TargetDesc of the schedule.
    struct PoolTarget {
        GpuTexturePtr texture;
        PassFormat format = PassFormat::Rgba8Unorm;
        std::string owner_pass;
        std::uint32_t copy = 0;
        /// Set on creation and on restart. A target is cleared on the frame
        /// after this goes up and not otherwise, because a buffer cleared every
        /// frame is a buffer that cannot accumulate.
        bool needs_clear = true;
    };

    SDL_GPUGraphicsPipeline* pipeline_for(const std::string& vertex, const std::string& fragment,
                                          PassFormat format);
    void release_pipelines();
    /// Drops cached pipelines built from a shader that is about to be replaced.
    /// Without this they would hold a released SDL_GPUShader.
    void release_pipelines_using(const std::string& shader_id);

    /// Rebuilds the pool when the schedule or the size changes, and registers
    /// every target with the texture registry so passes can sample them.
    bool ensure_pool(const PassSchedule& schedule);

    /// Builds the built-in mesh for a vertex shader that declares inputs, and
    /// the vertex layout that feeds it.
    ///
    /// The other preview meshes are generated from SV_VertexID and need no
    /// buffer at all, but a shader with input attributes cannot be drawn that
    /// way: Metal refuses to create the pipeline outright ("vertex function has
    /// input attributes but no vertex descriptor was set"), and the other
    /// backends read whatever is bound at slot 0, which is nothing. So the
    /// layout is taken from the shader's own reflected inputs rather than
    /// assumed, and a quad is generated to match it - position and texture
    /// coordinates filled in, everything else zeroed.
    bool ensure_mesh(const Reflection& vertex);
    void release_mesh();
    SDL_GPUShader* create_shader(Stage stage, const std::vector<std::uint8_t>& code,
                                 const Reflection& reflection);

    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUTexture* fallback_texture_ = nullptr;  // 1x1 white, for unbound samplers
    SDL_GPUSampler* sampler_ = nullptr;
    SDL_GPUTransferBuffer* readback_ = nullptr;

    /// Every target the current schedule asked for, in its order, so a
    /// ScheduledPass's indices are indices into this.
    std::vector<PoolTarget> pool_;
    /// What the pool was built for. Rebuilt when this stops matching.
    std::string pool_signature_;
    std::uint32_t present_target_ = 0;
    std::uint32_t inspect_target_ = 0;

    std::map<PipelineKey, SDL_GPUGraphicsPipeline*> pipelines_;
    SDL_GPUComputePipeline* compute_pipeline_ = nullptr;

    /// The registry the pool registers its targets with, so a pass output
    /// resolves through the same path as an image from disk.
    TextureRegistry* registry_ = nullptr;

    std::map<std::string, ShaderSlot> shaders_;
    std::string active_vertex_;
    std::string active_fragment_;
    std::string active_compute_;

    /// The generated quad, and the layout it was generated for. The signature
    /// is what the layout is compared by: pipelines are rebuilt on every
    /// recompile - which, with compile-while-typing, is every keystroke - and
    /// the buffer only has to be rewritten when the shader's inputs change.
    SDL_GPUBuffer* mesh_buffer_ = nullptr;
    std::vector<SDL_GPUVertexAttribute> mesh_attributes_;
    std::string mesh_signature_;
    std::uint32_t mesh_stride_ = 0;
    std::uint32_t mesh_vertex_count_ = 0;

    /// Mirrors PreviewSettings::blend. Held here because the blend state is
    /// baked into the pipeline, so a change has to invalidate it.
    bool blend_ = false;

    ShaderFormat preview_format_ = FORMAT_SPIRV;
    int width_ = 0;
    int height_ = 0;
    std::string last_error_;
    std::string status_;
};

}  // namespace ssstudio::gui

#endif  // SSSTUDIO_GUI_RENDERER_H
