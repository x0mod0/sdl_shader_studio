// Backend-neutral reflection description. Produced by the compiler backend from
// SPIR-V, consumed by the I/O panel, the packer and the doc/code generator.
#ifndef SSSTUDIO_REFLECTION_H
#define SSSTUDIO_REFLECTION_H

#include <cstdint>
#include <string>
#include <vector>

#include "ssstudio/types.h"

namespace ssstudio {

enum class ScalarType : std::uint8_t {
    Unknown,
    Bool,
    Int,
    UInt,
    Float,
    Double,
    Struct,
};

std::string_view to_string(ScalarType t);

// A member of a uniform / push-constant block. Rows/cols describe matrices
// (rows==1 && cols==1 is a scalar, rows==1 && cols>1 is a vector).
struct UniformMember {
    std::string name;
    ScalarType type = ScalarType::Float;
    std::uint32_t rows = 1;
    std::uint32_t cols = 1;
    std::uint32_t array_size = 0;  // 0 == not an array
    std::uint32_t offset = 0;      // byte offset within the block
    std::uint32_t size = 0;        // byte size including array stride
    std::vector<UniformMember> members;  // when type == Struct

    // Name of the equivalent C type in generated code ("float", "float4x4"...).
    std::string c_type() const;
    std::uint32_t component_count() const { return rows * cols; }
};

struct UniformBlock {
    std::string name;
    std::uint32_t set = 0;      // HLSL space / GLSL descriptor set
    std::uint32_t binding = 0;  // register index
    std::uint32_t size = 0;     // total byte size
    std::vector<UniformMember> members;
};

enum class ResourceKind : std::uint8_t {
    SampledTexture,     // texture + sampler pair (SDL: sampler count)
    StorageTexture,     // read-only or read-write image
    StorageBuffer,      // structured / byte-address buffer
    Sampler,            // standalone sampler
};

std::string_view to_string(ResourceKind k);

enum class TextureDim : std::uint8_t { Tex2D, Tex2DArray, Tex3D, TexCube, TexCubeArray, Buffer };
std::string_view to_string(TextureDim d);

struct Resource {
    std::string name;
    ResourceKind kind = ResourceKind::SampledTexture;
    TextureDim dim = TextureDim::Tex2D;
    std::uint32_t set = 0;
    std::uint32_t binding = 0;
    bool writable = false;
    std::uint32_t array_size = 0;  // 0 == not an array
    std::string struct_name;       // storage buffers
    std::uint32_t struct_stride = 0;
};

/// How many binding slots a resource occupies: one, or its array size.
///
/// Zero means "not an array" here, which is also what a runtime-sized array
/// reports - the two cannot be told apart, and a preview that cannot do bindless
/// has no use for the second anyway.
std::uint32_t resource_slot_count(const Resource& resource);

/// The name one element of a resource is bound under.
///
/// A resource declared as an array occupies several consecutive slots and needs
/// a name per element, spelled the way it would be written in the shader:
/// `channels[0]`, `channels[1]`. Anything that is not an array keeps its plain
/// name.
///
/// The two front ends disagree about arrays and this is where they are made to
/// agree. glslang reports `sampler2D channels[3]` as one resource with an array
/// size; DXC reports it already split into resources literally named
/// `channels[0]` and `channels[2]`, with unused elements dropped. Because the
/// second form arrives with the subscript already in the name and an array size
/// of zero, both paths end up producing the same keys - so a manifest written
/// against one language reads correctly against the other.
std::string resource_binding_key(const Resource& resource, std::uint32_t element);

struct VertexInput {
    std::string name;
    std::string semantic;  // HLSL semantic, empty for GLSL
    std::uint32_t location = 0;
    ScalarType type = ScalarType::Float;
    std::uint32_t components = 4;

    // Best-guess SDL_GPUVertexElementFormat enum name for the generated snippet.
    std::string sdl_vertex_format() const;
};

struct FragmentOutput {
    std::string name;
    std::uint32_t location = 0;
    ScalarType type = ScalarType::Float;
    std::uint32_t components = 4;
};

// Everything the tool knows about one compiled shader.
struct Reflection {
    Stage stage = Stage::Fragment;
    std::string entry_point = "main";

    std::vector<UniformBlock> uniform_blocks;
    std::vector<Resource> resources;
    std::vector<VertexInput> vertex_inputs;
    std::vector<FragmentOutput> outputs;

    // The stage interface, used to check that a vertex/fragment pair agrees.
    // Matched by location rather than name, since HLSL semantics and GLSL names
    // never line up across languages.
    std::vector<VertexInput> outputs_as_varyings;  // written by the vertex stage
    std::vector<VertexInput> inputs_as_varyings;   // read by the fragment stage

    std::uint32_t compute_threads[3] = {1, 1, 1};

    // SDL_GPUShaderCreateInfo / SDL_GPUComputePipelineCreateInfo counts.
    std::uint32_t num_samplers() const;
    std::uint32_t num_storage_textures() const;
    std::uint32_t num_storage_buffers() const;
    std::uint32_t num_uniform_buffers() const;

    // Compute pipelines distinguish read-only from read-write bindings.
    std::uint32_t num_readonly_storage_textures() const;
    std::uint32_t num_readonly_storage_buffers() const;
    std::uint32_t num_readwrite_storage_textures() const;
    std::uint32_t num_readwrite_storage_buffers() const;

    // Validates register spaces / descriptor sets against the SDL GPU contract.
    Diagnostics validate_binding_model(const std::string& file) const;
};

// Compact JSON used for the optional reflection section inside a pack and for
// the standalone shaders_reflection.json artifact. Hand-rolled so the core has
// no JSON dependency.
std::string reflection_to_json(const Reflection& r, int indent = 0);

}  // namespace ssstudio

#endif  // SSSTUDIO_REFLECTION_H
