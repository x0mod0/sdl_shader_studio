#include "preview/renderer.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <tuple>

namespace ssstudio::gui {
namespace {

constexpr SDL_GPUTextureFormat kTargetFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

// ---------------------------------------------------------------------------
// The built-in quad, for vertex shaders that declare inputs.
// ---------------------------------------------------------------------------

/// One corner: clip-space position and the texture coordinate that goes with
/// it. v runs downwards, so the corner at the top of the image is v = 0 - the
/// convention the fragment stage samples an image under, and the one a uv
/// gradient has to follow to look the way its author expects.
struct Corner {
    float x, y, u, v;
};

/// Two triangles, counter-clockwise, covering the whole target.
constexpr Corner kQuad[] = {
    {-1.0f, -1.0f, 0.0f, 1.0f}, {1.0f, -1.0f, 1.0f, 1.0f}, {-1.0f, 1.0f, 0.0f, 0.0f},
    {-1.0f, 1.0f, 0.0f, 0.0f},  {1.0f, -1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 0.0f},
};

/// What the quad has to say about one vertex input.
enum class Channel { Position, Texcoord, Color, Normal, Zero };

/// Reads an input's role off its name.
///
/// HLSL says it in the semantic and GLSL in the identifier, and neither is
/// spelled one agreed way - POSITION, in_position, a_Pos and vPosition are all
/// the same thing. Matching on a substring of the lower-cased text covers the
/// spellings in the wild; anything unrecognised is fed zeros, which is honest:
/// the preview has no opinion about what a custom attribute should contain.
Channel channel_for(const VertexInput& input) {
    std::string text = input.semantic.empty() ? input.name : input.semantic;
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto has = [&text](const char* needle) {
        return text.find(needle) != std::string::npos;
    };
    if (has("pos")) return Channel::Position;
    if (has("texcoord") || has("uv") || has("tex")) return Channel::Texcoord;
    if (has("color") || has("colour")) return Channel::Color;
    if (has("normal")) return Channel::Normal;
    return Channel::Zero;
}

float channel_value(Channel channel, const Corner& corner, std::uint32_t component) {
    switch (channel) {
        case Channel::Position:
            // w is 1 so that a float4 position arrives already homogeneous.
            switch (component) {
                case 0: return corner.x;
                case 1: return corner.y;
                case 2: return 0.0f;
                default: return 1.0f;
            }
        case Channel::Texcoord:
            switch (component) {
                case 0: return corner.u;
                case 1: return corner.v;
                default: return 0.0f;
            }
        case Channel::Color: return 1.0f;  // opaque white, so a tint shows as itself
        case Channel::Normal: return component == 2 ? 1.0f : 0.0f;  // facing the viewer
        case Channel::Zero: break;
    }
    return 0.0f;
}

SDL_GPUVertexElementFormat element_format(ScalarType type, std::uint32_t components) {
    const std::uint32_t n = std::clamp<std::uint32_t>(components, 1u, 4u);
    switch (type) {
        case ScalarType::Int:
        case ScalarType::Bool:
            switch (n) {
                case 1: return SDL_GPU_VERTEXELEMENTFORMAT_INT;
                case 2: return SDL_GPU_VERTEXELEMENTFORMAT_INT2;
                case 3: return SDL_GPU_VERTEXELEMENTFORMAT_INT3;
                default: return SDL_GPU_VERTEXELEMENTFORMAT_INT4;
            }
        case ScalarType::UInt:
            switch (n) {
                case 1: return SDL_GPU_VERTEXELEMENTFORMAT_UINT;
                case 2: return SDL_GPU_VERTEXELEMENTFORMAT_UINT2;
                case 3: return SDL_GPU_VERTEXELEMENTFORMAT_UINT3;
                default: return SDL_GPU_VERTEXELEMENTFORMAT_UINT4;
            }
        default:
            switch (n) {
                case 1: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT;
                case 2: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
                case 3: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
                default: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
            }
    }
}

/// The GPU format for a pass target, falling back when the device will not take
/// the one asked for.
///
/// Float targets are what make a buffer able to hold data rather than colour, so
/// they are worth asking for - but not worth failing over. Thirty-two bits give
/// way to sixteen and sixteen to eight, and the caller says what was lost.
SDL_GPUTextureFormat gpu_format_for(SDL_GPUDevice* device, PassFormat format,
                                    PassFormat& out_granted) {
    const SDL_GPUTextureUsageFlags usage =
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    struct Candidate {
        PassFormat format;
        SDL_GPUTextureFormat gpu;
    };
    static constexpr Candidate kLadder[] = {
        {PassFormat::Rgba32Float, SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT},
        {PassFormat::Rgba16Float, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT},
        {PassFormat::Rgba8Unorm, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM},
    };

    bool reached = false;
    for (const Candidate& candidate : kLadder) {
        if (!reached && candidate.format != format) continue;
        reached = true;
        if (SDL_GPUTextureSupportsFormat(device, candidate.gpu, SDL_GPU_TEXTURETYPE_2D, usage)) {
            out_granted = candidate.format;
            return candidate.gpu;
        }
    }
    out_granted = PassFormat::Rgba8Unorm;
    return kTargetFormat;
}

}  // namespace

bool PreviewRenderer::PipelineKey::operator<(const PipelineKey& other) const {
    return std::tie(vertex, fragment, format, blend) <
           std::tie(other.vertex, other.fragment, other.format, other.blend);
}

bool PreviewRenderer::init(SDL_GPUDevice* device, TextureRegistry* registry) {
    registry_ = registry;
    if (!device) {
        last_error_ = "no GPU device";
        return false;
    }
    device_ = device;

    // The front end always produces SPIR-V, but only a Vulkan device will take
    // it. Whichever of the three the device does accept is what every compile is
    // asked to translate to, so the panel works the same on all three backends.
    const SDL_GPUShaderFormat supported = SDL_GetGPUShaderFormats(device_);
    if (supported & SDL_GPU_SHADERFORMAT_SPIRV) {
        preview_format_ = FORMAT_SPIRV;
    } else if (supported & SDL_GPU_SHADERFORMAT_MSL) {
        preview_format_ = FORMAT_MSL;
    } else if (supported & SDL_GPU_SHADERFORMAT_DXIL) {
        preview_format_ = FORMAT_DXIL;
    } else {
        last_error_ = "this GPU backend accepts none of the shader formats the compiler can "
                      "produce (SPIR-V, MSL, DXIL)";
        device_ = nullptr;
        return false;
    }

    SDL_GPUSamplerCreateInfo sampler_info;
    SDL_zero(sampler_info);
    sampler_info.min_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = SDL_CreateGPUSampler(device_, &sampler_info);
    if (!sampler_) {
        last_error_ = SDL_GetError();
        return false;
    }

    // 1x1 white texture stands in for any sampler the user has not bound yet,
    // so an unfinished shader still previews instead of failing to draw.
    SDL_GPUTextureCreateInfo tex_info;
    SDL_zero(tex_info);
    tex_info.type = SDL_GPU_TEXTURETYPE_2D;
    tex_info.format = kTargetFormat;
    tex_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tex_info.width = 1;
    tex_info.height = 1;
    tex_info.layer_count_or_depth = 1;
    tex_info.num_levels = 1;
    fallback_texture_ = SDL_CreateGPUTexture(device_, &tex_info);
    if (fallback_texture_) {
        SDL_GPUTransferBufferCreateInfo tb;
        SDL_zero(tb);
        tb.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tb.size = 4;
        SDL_GPUTransferBuffer* staging = SDL_CreateGPUTransferBuffer(device_, &tb);
        if (staging) {
            if (void* mapped = SDL_MapGPUTransferBuffer(device_, staging, false)) {
                const std::uint8_t white[4] = {255, 255, 255, 255};
                SDL_memcpy(mapped, white, sizeof(white));
                SDL_UnmapGPUTransferBuffer(device_, staging);

                SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
                SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(cmd);
                SDL_GPUTextureTransferInfo src;
                SDL_zero(src);
                src.transfer_buffer = staging;
                SDL_GPUTextureRegion dst;
                SDL_zero(dst);
                dst.texture = fallback_texture_;
                dst.w = 1;
                dst.h = 1;
                dst.d = 1;
                SDL_UploadToGPUTexture(pass, &src, &dst, false);
                SDL_EndGPUCopyPass(pass);
                SDL_SubmitGPUCommandBuffer(cmd);
            }
            SDL_ReleaseGPUTransferBuffer(device_, staging);
        }
    }

    status_ = "idle";
    return true;
}

void PreviewRenderer::shutdown() {
    if (!device_) return;
    clear();
    if (sampler_) SDL_ReleaseGPUSampler(device_, sampler_);
    if (fallback_texture_) SDL_ReleaseGPUTexture(device_, fallback_texture_);
    if (readback_) SDL_ReleaseGPUTransferBuffer(device_, readback_);
    sampler_ = nullptr;
    fallback_texture_ = nullptr;
    readback_ = nullptr;
    pool_.clear();
    pool_signature_.clear();
    registry_ = nullptr;
    device_ = nullptr;
}

void PreviewRenderer::clear() {
    if (!device_) return;
    release_pipelines();
    release_mesh();
    for (auto& [id, slot] : shaders_) {
        if (slot.shader) SDL_ReleaseGPUShader(device_, slot.shader);
    }
    shaders_.clear();
    active_vertex_.clear();
    active_fragment_.clear();
    active_compute_.clear();
    status_ = "idle";
}

bool PreviewRenderer::ensure_mesh(const Reflection& vertex) {
    // Attributes in location order, packed tightly. Every scalar a vertex
    // format can carry is four bytes wide, so the tight packing is also an
    // aligned one and no padding has to be inserted between attributes.
    std::vector<const VertexInput*> inputs;
    inputs.reserve(vertex.vertex_inputs.size());
    for (const auto& input : vertex.vertex_inputs) inputs.push_back(&input);
    std::sort(inputs.begin(), inputs.end(),
              [](const VertexInput* a, const VertexInput* b) { return a->location < b->location; });

    std::vector<SDL_GPUVertexAttribute> attributes;
    std::string signature;
    std::uint32_t stride = 0;
    for (const VertexInput* input : inputs) {
        const std::uint32_t components = std::clamp<std::uint32_t>(input->components, 1u, 4u);
        SDL_GPUVertexAttribute attribute;
        SDL_zero(attribute);
        attribute.location = input->location;
        attribute.buffer_slot = 0;
        attribute.format = element_format(input->type, components);
        attribute.offset = stride;
        attributes.push_back(attribute);
        signature += std::to_string(input->location) + ':' +
                     std::to_string(static_cast<int>(attribute.format)) + ':' +
                     std::to_string(static_cast<int>(channel_for(*input))) + ';';
        stride += components * 4;
    }
    if (attributes.empty() || stride == 0) return false;
    if (mesh_buffer_ && signature == mesh_signature_) return true;
    release_mesh();

    const std::uint32_t count = static_cast<std::uint32_t>(SDL_arraysize(kQuad));
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(stride) * count, 0);
    for (std::uint32_t v = 0; v < count; ++v) {
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const VertexInput& input = *inputs[i];
            const Channel channel = channel_for(input);
            const std::uint32_t components = std::clamp<std::uint32_t>(input.components, 1u, 4u);
            for (std::uint32_t c = 0; c < components; ++c) {
                const std::size_t at =
                    static_cast<std::size_t>(v) * stride + attributes[i].offset + c * 4;
                const float value = channel_value(channel, kQuad[v], c);
                // An integer attribute is written as an integer, not as the bit
                // pattern of a float: the two agree on 0 and on nothing else.
                if (input.type == ScalarType::Int || input.type == ScalarType::UInt ||
                    input.type == ScalarType::Bool) {
                    const std::int32_t integral = static_cast<std::int32_t>(value);
                    std::memcpy(bytes.data() + at, &integral, 4);
                } else {
                    std::memcpy(bytes.data() + at, &value, 4);
                }
            }
        }
    }

    SDL_GPUBufferCreateInfo buffer_info;
    SDL_zero(buffer_info);
    buffer_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    buffer_info.size = static_cast<Uint32>(bytes.size());
    mesh_buffer_ = SDL_CreateGPUBuffer(device_, &buffer_info);
    if (!mesh_buffer_) {
        last_error_ = SDL_GetError();
        return false;
    }

    SDL_GPUTransferBufferCreateInfo transfer_info;
    SDL_zero(transfer_info);
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size = static_cast<Uint32>(bytes.size());
    SDL_GPUTransferBuffer* staging = SDL_CreateGPUTransferBuffer(device_, &transfer_info);
    if (!staging) {
        last_error_ = SDL_GetError();
        release_mesh();
        return false;
    }

    bool uploaded = false;
    if (void* mapped = SDL_MapGPUTransferBuffer(device_, staging, false)) {
        SDL_memcpy(mapped, bytes.data(), bytes.size());
        SDL_UnmapGPUTransferBuffer(device_, staging);

        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
        SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTransferBufferLocation src;
        SDL_zero(src);
        src.transfer_buffer = staging;
        SDL_GPUBufferRegion dst;
        SDL_zero(dst);
        dst.buffer = mesh_buffer_;
        dst.size = static_cast<Uint32>(bytes.size());
        SDL_UploadToGPUBuffer(pass, &src, &dst, false);
        SDL_EndGPUCopyPass(pass);
        SDL_SubmitGPUCommandBuffer(cmd);
        uploaded = true;
    } else {
        last_error_ = SDL_GetError();
    }
    SDL_ReleaseGPUTransferBuffer(device_, staging);
    if (!uploaded) {
        release_mesh();
        return false;
    }

    mesh_attributes_ = std::move(attributes);
    mesh_signature_ = std::move(signature);
    mesh_stride_ = stride;
    mesh_vertex_count_ = count;
    return true;
}

void PreviewRenderer::release_mesh() {
    if (mesh_buffer_) SDL_ReleaseGPUBuffer(device_, mesh_buffer_);
    mesh_buffer_ = nullptr;
    mesh_attributes_.clear();
    mesh_signature_.clear();
    mesh_stride_ = 0;
    mesh_vertex_count_ = 0;
}

void PreviewRenderer::release_pipelines() {
    for (auto& [key, pipeline] : pipelines_) {
        if (pipeline) SDL_ReleaseGPUGraphicsPipeline(device_, pipeline);
    }
    pipelines_.clear();
    if (compute_pipeline_) {
        SDL_ReleaseGPUComputePipeline(device_, compute_pipeline_);
        compute_pipeline_ = nullptr;
    }
}

void PreviewRenderer::release_pipelines_using(const std::string& shader_id) {
    for (auto it = pipelines_.begin(); it != pipelines_.end();) {
        if (it->first.vertex != shader_id && it->first.fragment != shader_id) {
            ++it;
            continue;
        }
        if (it->second) SDL_ReleaseGPUGraphicsPipeline(device_, it->second);
        it = pipelines_.erase(it);
    }
}

SDL_GPUShader* PreviewRenderer::create_shader(Stage stage, const std::vector<std::uint8_t>& code,
                                              const Reflection& reflection) {
    if (code.empty()) {
        last_error_ = "no " + std::string(to_string(preview_format_)) +
                      " blob for this shader; the compile that produced it did not translate";
        return nullptr;
    }

    // Translating to MSL renames the entry point: `main` is reserved in Metal
    // Shading Language, so SPIRV-Cross emits `main0` whatever the source called
    // it. Every other format keeps the name the shader was compiled under.
    const char* entry = reflection.entry_point.empty() ? "main" : reflection.entry_point.c_str();
    if (preview_format_ == FORMAT_MSL) entry = "main0";

    SDL_GPUShaderCreateInfo info;
    SDL_zero(info);
    info.code = code.data();
    info.code_size = code.size();
    info.entrypoint = entry;
    info.format = static_cast<SDL_GPUShaderFormat>(preview_format_);
    info.stage = stage == Stage::Vertex ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
    info.num_samplers = reflection.num_samplers();
    info.num_storage_textures = reflection.num_storage_textures();
    info.num_storage_buffers = reflection.num_storage_buffers();
    info.num_uniform_buffers = reflection.num_uniform_buffers();

    SDL_GPUShader* shader = SDL_CreateGPUShader(device_, &info);
    if (!shader) last_error_ = SDL_GetError();
    return shader;
}

void PreviewRenderer::set_shader(const std::string& id, Stage stage,
                                 const std::vector<std::uint8_t>& code,
                                 const Reflection& reflection) {
    if (!device_) return;

    ShaderSlot& slot = shaders_[id];
    if (slot.shader) SDL_ReleaseGPUShader(device_, slot.shader);
    slot.stage = stage;
    slot.reflection = reflection;
    slot.shader = stage == Stage::Compute ? nullptr : create_shader(stage, code, reflection);

    // Auto-select the newest compiled shader for its stage when nothing is set.
    if (stage == Stage::Vertex && active_vertex_.empty()) active_vertex_ = id;
    if (stage == Stage::Fragment && active_fragment_.empty()) active_fragment_ = id;
    if (stage == Stage::Compute && active_compute_.empty()) active_compute_ = id;

    // Any pipeline built from the shader that was just released is now holding
    // a dangling handle, so it goes with it.
    release_pipelines_using(id);
}

void PreviewRenderer::remove_shader(const std::string& id) {
    auto it = shaders_.find(id);
    if (it == shaders_.end()) return;
    if (it->second.shader) SDL_ReleaseGPUShader(device_, it->second.shader);
    shaders_.erase(it);
    if (active_vertex_ == id) active_vertex_.clear();
    if (active_fragment_ == id) active_fragment_.clear();
    if (active_compute_ == id) active_compute_.clear();
    release_pipelines_using(id);
}

void PreviewRenderer::set_active(const std::string& vertex_id, const std::string& fragment_id) {
    if (active_vertex_ == vertex_id && active_fragment_ == fragment_id) return;
    active_vertex_ = vertex_id;
    active_fragment_ = fragment_id;
}

void PreviewRenderer::set_active_compute(const std::string& compute_id) {
    active_compute_ = compute_id;
}

bool PreviewRenderer::resize(int width, int height) {
    width = std::max(1, width);
    height = std::max(1, height);
    if (width == width_ && height == height_) return true;

    // Every target is the size of the preview, so a resize throws all of them
    // away - and with them whatever a buffer had accumulated. Unavoidable, and
    // worth saying out loud rather than letting it look like a shader bug.
    width_ = width;
    height_ = height;
    pool_.clear();
    pool_signature_.clear();
    return true;
}

void PreviewRenderer::restart() {
    for (PoolTarget& target : pool_) target.needs_clear = true;
}

void PreviewRenderer::set_inspected_target(std::uint32_t index) {
    inspect_target_ = index < pool_.size() ? index : present_target_;
}

bool PreviewRenderer::ensure_pool(const PassSchedule& schedule) {
    if (!device_ || width_ <= 0 || height_ <= 0) return false;

    // What the pool would have to be for this schedule at this size. Comparing a
    // string is cheap next to recreating textures, and it is done every frame.
    std::string signature = std::to_string(width_) + "x" + std::to_string(height_);
    for (const TargetDesc& target : schedule.targets) {
        signature += "|" + target.owner_pass + ":" + std::string(to_string(target.format)) + ":" +
                     std::to_string(target.copy);
    }
    if (signature == pool_signature_ && pool_.size() == schedule.targets.size()) {
        present_target_ = schedule.present_target;
        if (inspect_target_ >= pool_.size()) inspect_target_ = present_target_;
        return true;
    }

    // Names go before the textures do, so nothing is left pointing at a target
    // that is about to be released.
    if (registry_) {
        for (const PoolTarget& target : pool_) {
            if (!target.owner_pass.empty()) {
                registry_->set_external(pass_target_key(target.owner_pass, target.copy), nullptr,
                                        0, 0);
            }
        }
    }
    pool_.clear();

    for (const TargetDesc& desc : schedule.targets) {
        PoolTarget target;
        target.owner_pass = desc.owner_pass;
        target.copy = desc.copy;
        target.needs_clear = true;

        PassFormat granted = desc.format;
        const SDL_GPUTextureFormat format = gpu_format_for(device_, desc.format, granted);
        if (granted != desc.format) {
            status_ = "this GPU does not support " + std::string(to_string(desc.format)) +
                      " targets; " + (desc.owner_pass.empty() ? "the image" : desc.owner_pass) +
                      " is using " + std::string(to_string(granted)) + " instead";
        }
        target.format = granted;

        SDL_GPUTextureCreateInfo info;
        SDL_zero(info);
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = format;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = static_cast<Uint32>(width_);
        info.height = static_cast<Uint32>(height_);
        info.layer_count_or_depth = 1;
        info.num_levels = 1;

        target.texture =
            GpuTexturePtr(SDL_CreateGPUTexture(device_, &info), GpuTextureDeleter{device_});
        if (!target.texture) {
            last_error_ = SDL_GetError();
            pool_.clear();
            pool_signature_.clear();
            return false;
        }
        pool_.push_back(std::move(target));
    }

    // Published under the pass's name and copy, so resolving one goes through
    // the same path an image from disk does.
    if (registry_) {
        for (const PoolTarget& target : pool_) {
            if (target.owner_pass.empty()) continue;
            registry_->set_external(pass_target_key(target.owner_pass, target.copy),
                                    target.texture.get(), static_cast<std::uint32_t>(width_),
                                    static_cast<std::uint32_t>(height_));
        }
    }

    pool_signature_ = std::move(signature);
    present_target_ = schedule.present_target;
    inspect_target_ = present_target_;
    return true;
}

SDL_GPUGraphicsPipeline* PreviewRenderer::pipeline_for(const std::string& vertex,
                                                      const std::string& fragment,
                                                      PassFormat format) {
    const PipelineKey key{vertex, fragment, format, blend_};
    if (const auto it = pipelines_.find(key); it != pipelines_.end()) return it->second;

    const auto vit = shaders_.find(vertex);
    const auto fit = shaders_.find(fragment);
    if (vit == shaders_.end() || fit == shaders_.end() || !vit->second.shader ||
        !fit->second.shader) {
        status_ = "waiting for a vertex and fragment shader that compile";
        return nullptr;
    }

    SDL_GPUColorTargetDescription color;
    SDL_zero(color);
    PassFormat granted = format;
    color.format = gpu_format_for(device_, format, granted);
    // Off unless the project asks for it. A fragment shader's alpha is usually a
    // value the shader computed rather than a coverage term, so blending it
    // against the clear colour silently washes the image out - and it is what a
    // shader brought in from a convention that does not blend, or one written as
    // a post-process stage, will trip over first.
    if (blend_) {
        color.blend_state.enable_blend = true;
        color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    }

    SDL_GPUGraphicsPipelineCreateInfo info;
    SDL_zero(info);
    info.vertex_shader = vit->second.shader;
    info.fragment_shader = fit->second.shader;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.target_info.color_target_descriptions = &color;
    info.target_info.num_color_targets = 1;

    // Most preview meshes are generated in the vertex shader from SV_VertexID
    // and need no vertex buffer at all. A shader that declares inputs is fed the
    // built-in quad instead, laid out to match its own reflected attributes -
    // leaving the layout empty is what made Metal refuse the pipeline with
    // "vertex function has input attributes but no vertex descriptor was set".
    SDL_GPUVertexBufferDescription buffer_description;
    SDL_zero(buffer_description);
    if (!vit->second.reflection.vertex_inputs.empty()) {
        if (!ensure_mesh(vit->second.reflection)) {
            status_ = "cannot build a mesh for this vertex layout: " + last_error_;
            return nullptr;
        }
        buffer_description.slot = 0;
        buffer_description.pitch = mesh_stride_;
        buffer_description.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        info.vertex_input_state.vertex_buffer_descriptions = &buffer_description;
        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_attributes = mesh_attributes_.data();
        info.vertex_input_state.num_vertex_attributes =
            static_cast<Uint32>(mesh_attributes_.size());
    }

    SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(device_, &info);
    if (!pipeline) {
        last_error_ = SDL_GetError();
        status_ = "pipeline creation failed: " + last_error_;
        return nullptr;
    }
    pipelines_.emplace(key, pipeline);
    status_ = "ok";
    return pipeline;
}

bool PreviewRenderer::render(const PassSchedule& schedule,
                             const std::map<std::string, PassFeed>& feeds,
                             const PreviewSettings& settings) {
    if (!device_) return false;
    if (schedule.passes.empty()) {
        status_ = "nothing to draw";
        return false;
    }

    // Blending is baked into the pipeline, so a change of mind invalidates every
    // one of them rather than being applied at draw time.
    if (settings.blend != blend_) {
        blend_ = settings.blend;
        release_pipelines();
    }
    if (!ensure_pool(schedule)) return false;

    // Every pipeline is built before anything is recorded: half a chain drawn
    // because the last pass would not compile is worse than none of it, because
    // what is on screen then is a mixture of this frame and the last.
    std::vector<SDL_GPUGraphicsPipeline*> pipelines;
    pipelines.reserve(schedule.passes.size());
    for (const ScheduledPass& pass : schedule.passes) {
        SDL_GPUGraphicsPipeline* pipeline =
            pipeline_for(schedule.vertex_shader, pass.shader_id,
                         pool_[pass.write_target].format);
        if (!pipeline) return false;
        pipelines.push_back(pipeline);
    }

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) {
        last_error_ = SDL_GetError();
        return false;
    }

    // One command buffer for the whole chain. Writing a target in one render
    // pass and sampling it in the next is exactly what this is for, and
    // submitting per pass would only add synchronisation nobody asked for.
    for (std::size_t i = 0; i < schedule.passes.size(); ++i) {
        const ScheduledPass& pass = schedule.passes[i];
        PoolTarget& destination = pool_[pass.write_target];

        SDL_GPUColorTargetInfo target;
        SDL_zero(target);
        target.texture = destination.texture.get();
        target.clear_color.r = settings.clear_color[0];
        target.clear_color.g = settings.clear_color[1];
        target.clear_color.b = settings.clear_color[2];
        target.clear_color.a = settings.clear_color[3];
        // The image is redrawn from nothing every frame. A buffer is not - it
        // is cleared once, and after that it keeps whatever it accumulated,
        // which is the entire reason for having one. DONT_CARE rather than LOAD
        // because the pass overwrites every pixel either way, and LOAD would pay
        // for bandwidth to preserve what it is about to replace.
        target.load_op = (pass.is_image || destination.needs_clear) ? SDL_GPU_LOADOP_CLEAR
                                                                    : SDL_GPU_LOADOP_DONT_CARE;
        target.store_op = SDL_GPU_STOREOP_STORE;
        destination.needs_clear = false;

        SDL_GPURenderPass* render_pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(render_pass, pipelines[i]);

        const auto feed_it = feeds.find(pass.shader_id);
        const PassFeed* feed = feed_it == feeds.end() ? nullptr : &feed_it->second;

        // Uniforms are pushed per pass, not once per frame: they apply to the
        // command buffer, and reusing the image pass's values for a buffer is
        // the kind of mistake that looks like a shader nearly working.
        if (feed && feed->uniforms.vertex_present && !feed->uniforms.vertex_bytes.empty()) {
            SDL_PushGPUVertexUniformData(cmd, 0, feed->uniforms.vertex_bytes.data(),
                                         static_cast<Uint32>(feed->uniforms.vertex_bytes.size()));
        }
        if (feed && feed->uniforms.fragment_present && !feed->uniforms.fragment_bytes.empty()) {
            SDL_PushGPUFragmentUniformData(
                cmd, 0, feed->uniforms.fragment_bytes.data(),
                static_cast<Uint32>(feed->uniforms.fragment_bytes.size()));
        }

        // Bound by reflected slot, not by position. An unused sampler is
        // dropped from the compiled module, so a shader declaring channels 0
        // and 2 reflects bindings 0 and 2 with nothing at 1 - filling slots
        // 0 and 1 instead would put channel 2's image where channel 1 is read.
        const auto sit = shaders_.find(pass.shader_id);
        if (sit != shaders_.end() && fallback_texture_) {
            std::uint32_t count = 0;
            for (const Resource& resource : sit->second.reflection.resources) {
                if (resource.kind != ResourceKind::SampledTexture) continue;
                count = std::max(count, resource.binding + std::max(resource.array_size, 1u));
            }
            for (const auto& [slot, target_index] : pass.read_targets) {
                count = std::max(count, slot + 1);
            }
            if (count > 0) {
                std::vector<SDL_GPUTextureSamplerBinding> bindings(count);
                for (std::uint32_t slot = 0; slot < count; ++slot) {
                    bindings[slot].texture = fallback_texture_;
                    bindings[slot].sampler = sampler_;
                    if (feed && slot < feed->textures.size() && feed->textures[slot].texture) {
                        bindings[slot].texture = feed->textures[slot].texture;
                        if (feed->textures[slot].sampler) {
                            bindings[slot].sampler = feed->textures[slot].sampler;
                        }
                    }
                    // A slot the schedule fills wins over whatever the panel
                    // resolved: the schedule knows which copy of a buffer holds
                    // this frame's answer and the panel does not.
                    if (const auto it = pass.read_targets.find(slot);
                        it != pass.read_targets.end() && it->second < pool_.size()) {
                        bindings[slot].texture = pool_[it->second].texture.get();
                    }
                }
                SDL_BindGPUFragmentSamplers(render_pass, 0, bindings.data(), count);
            }
        }

        // Six vertices from the built-in quad when the shader reads attributes,
        // three generated from SV_VertexID when it does not.
        if (mesh_buffer_) {
            SDL_GPUBufferBinding mesh;
            SDL_zero(mesh);
            mesh.buffer = mesh_buffer_;
            SDL_BindGPUVertexBuffers(render_pass, 0, &mesh, 1);
            SDL_DrawGPUPrimitives(render_pass, mesh_vertex_count_, 1, 0, 0);
        } else {
            SDL_DrawGPUPrimitives(render_pass, 3, 1, 0, 0);
        }
        SDL_EndGPURenderPass(render_pass);
    }

    SDL_SubmitGPUCommandBuffer(cmd);
    return true;
}

std::vector<PreviewRenderer::TargetLabel> PreviewRenderer::target_labels() const {
    std::vector<TargetLabel> labels;
    labels.reserve(pool_.size());
    for (const PoolTarget& target : pool_) {
        labels.push_back(TargetLabel{target.owner_pass, target.copy, target.format});
    }
    return labels;
}

SDL_GPUTexture* PreviewRenderer::texture_id() {
    const std::uint32_t index = inspect_target_ < pool_.size() ? inspect_target_ : present_target_;
    return index < pool_.size() ? pool_[index].texture.get() : nullptr;
}

bool PreviewRenderer::read_pixel(int x, int y, float out_rgba[4]) {
    const std::uint32_t index = inspect_target_ < pool_.size() ? inspect_target_ : present_target_;
    if (!device_ || index >= pool_.size() || !pool_[index].texture) return false;
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return false;

    if (!readback_) {
        SDL_GPUTransferBufferCreateInfo info;
        SDL_zero(info);
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        info.size = 4;
        readback_ = SDL_CreateGPUTransferBuffer(device_, &info);
        if (!readback_) {
            last_error_ = SDL_GetError();
            return false;
        }
    }

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion region;
    SDL_zero(region);
    region.texture = pool_[index].texture.get();
    region.x = static_cast<Uint32>(x);
    region.y = static_cast<Uint32>(y);
    region.w = 1;
    region.h = 1;
    region.d = 1;
    SDL_GPUTextureTransferInfo dst;
    SDL_zero(dst);
    dst.transfer_buffer = readback_;
    SDL_DownloadFromGPUTexture(pass, &region, &dst);
    SDL_EndGPUCopyPass(pass);

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence) {
        SDL_WaitForGPUFences(device_, true, &fence, 1);
        SDL_ReleaseGPUFence(device_, fence);
    }

    void* mapped = SDL_MapGPUTransferBuffer(device_, readback_, false);
    if (!mapped) {
        last_error_ = SDL_GetError();
        return false;
    }
    const auto* pixel = static_cast<const std::uint8_t*>(mapped);
    for (int i = 0; i < 4; ++i) out_rgba[i] = static_cast<float>(pixel[i]) / 255.0f;
    SDL_UnmapGPUTransferBuffer(device_, readback_);
    return true;
}

}  // namespace ssstudio::gui
