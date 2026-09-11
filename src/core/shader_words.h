// The vocabulary of the two shader dialects, in one place.
//
// Both the completion list and the syntax highlighter read these tables, so a
// name added for one cannot go missing from the other - which is the failure
// this header exists to prevent. Everything is constexpr and therefore internal
// to each translation unit that includes it; the tables are small enough that
// duplicating them costs less than an out-of-line definition would explain.
#ifndef SSSTUDIO_CORE_SHADER_WORDS_H
#define SSSTUDIO_CORE_SHADER_WORDS_H

#include <array>
#include <string_view>

namespace ssstudio::words {

/// A built-in function, with the two things the completion popup shows next to
/// it. The highlighter only reads the name; the other two are why these live in
/// a struct rather than a bare name list.
struct Intrinsic {
    const char* name;
    const char* signature;
    const char* doc;
};

// Shared by both languages. Where the spelling differs, the language-specific
// tables below override it, which is exactly the set of names people get wrong
// when moving between HLSL and GLSL.
constexpr auto kCommon = std::to_array<Intrinsic>({
    {"abs", "abs(x)", "absolute value"},
    {"acos", "acos(x)", "arc cosine, radians"},
    {"all", "all(x)", "true when every component is non-zero"},
    {"any", "any(x)", "true when any component is non-zero"},
    {"asin", "asin(x)", "arc sine, radians"},
    {"atan", "atan(y, x)", "arc tangent of y/x, quadrant aware"},
    {"ceil", "ceil(x)", "round up"},
    {"clamp", "clamp(x, lo, hi)", "constrain to a range"},
    {"cos", "cos(x)", "cosine, radians"},
    {"cosh", "cosh(x)", "hyperbolic cosine"},
    {"cross", "cross(a, b)", "cross product of two float3"},
    {"degrees", "degrees(radians)", "radians to degrees"},
    {"determinant", "determinant(m)", "matrix determinant"},
    {"distance", "distance(a, b)", "length(a - b)"},
    {"dot", "dot(a, b)", "dot product"},
    {"exp", "exp(x)", "e raised to x"},
    {"exp2", "exp2(x)", "2 raised to x"},
    {"faceforward", "faceforward(n, i, ng)", "n flipped to face away from i"},
    {"floor", "floor(x)", "round down"},
    {"fma", "fma(a, b, c)", "a * b + c with one rounding"},
    {"fwidth", "fwidth(x)", "sum of absolute screen-space derivatives"},
    {"isinf", "isinf(x)", "true for positive or negative infinity"},
    {"isnan", "isnan(x)", "true for not-a-number"},
    {"length", "length(v)", "vector magnitude"},
    {"log", "log(x)", "natural logarithm"},
    {"log2", "log2(x)", "base-2 logarithm"},
    {"max", "max(a, b)", "component-wise maximum"},
    {"min", "min(a, b)", "component-wise minimum"},
    {"modf", "modf(x, out ip)", "split into fractional and integer parts"},
    {"normalize", "normalize(v)", "vector scaled to unit length"},
    {"pow", "pow(base, exponent)", "base raised to exponent"},
    {"radians", "radians(degrees)", "degrees to radians"},
    {"reflect", "reflect(incident, normal)", "reflection vector"},
    {"refract", "refract(incident, normal, eta)", "refraction vector"},
    {"round", "round(x)", "round to nearest"},
    {"sign", "sign(x)", "-1, 0 or 1"},
    {"sin", "sin(x)", "sine, radians"},
    {"sinh", "sinh(x)", "hyperbolic sine"},
    {"smoothstep", "smoothstep(edge0, edge1, x)", "hermite interpolation between edges"},
    {"sqrt", "sqrt(x)", "square root"},
    {"step", "step(edge, x)", "0 below the edge, 1 at or above"},
    {"tan", "tan(x)", "tangent, radians"},
    {"tanh", "tanh(x)", "hyperbolic tangent"},
    {"transpose", "transpose(m)", "matrix transpose"},
    {"trunc", "trunc(x)", "truncate toward zero"},
});

constexpr auto kHlslOnly = std::to_array<Intrinsic>({
    {"saturate", "saturate(x)", "clamp to [0, 1]; in GLSL write clamp(x, 0.0, 1.0)"},
    {"lerp", "lerp(a, b, t)", "linear blend; GLSL calls this mix"},
    {"frac", "frac(x)", "fractional part; GLSL calls this fract"},
    {"mul", "mul(a, b)", "matrix multiply; GLSL uses the * operator"},
    {"rcp", "rcp(x)", "fast reciprocal"},
    {"rsqrt", "rsqrt(x)", "reciprocal square root; GLSL calls this inversesqrt"},
    {"ddx", "ddx(x)", "screen-space derivative in x; GLSL calls this dFdx"},
    {"ddy", "ddy(x)", "screen-space derivative in y; GLSL calls this dFdy"},
    {"ddx_fine", "ddx_fine(x)", "high-quality derivative in x"},
    {"ddy_fine", "ddy_fine(x)", "high-quality derivative in y"},
    {"asfloat", "asfloat(bits)", "reinterpret bits as float"},
    {"asuint", "asuint(value)", "reinterpret as uint"},
    {"asint", "asint(value)", "reinterpret as int"},
    {"atan2", "atan2(y, x)", "arc tangent of y/x; GLSL spells this atan(y, x)"},
    {"clip", "clip(x)", "discard the fragment when any component is negative"},
    {"fmod", "fmod(x, y)", "remainder toward zero; GLSL's mod rounds toward -inf"},
    {"mad", "mad(a, b, c)", "a * b + c"},
    {"sincos", "sincos(x, out s, out c)", "sine and cosine in one call"},
    {"countbits", "countbits(value)", "population count; GLSL calls this bitCount"},
    {"firstbithigh", "firstbithigh(value)", "index of the highest set bit"},
    {"f32tof16", "f32tof16(value)", "pack a float into half precision bits"},
    {"f16tof32", "f16tof32(bits)", "unpack half precision bits"},
    {"InterlockedAdd", "InterlockedAdd(dest, value, out original)", "atomic add"},
    {"GroupMemoryBarrierWithGroupSync", "GroupMemoryBarrierWithGroupSync()",
     "groupshared barrier; GLSL calls this barrier()"},
    {"DeviceMemoryBarrier", "DeviceMemoryBarrier()", "device memory barrier"},
});

constexpr auto kGlslOnly = std::to_array<Intrinsic>({
    {"mix", "mix(a, b, t)", "linear blend; HLSL calls this lerp"},
    {"fract", "fract(x)", "fractional part; HLSL calls this frac"},
    {"inversesqrt", "inversesqrt(x)", "reciprocal square root; HLSL calls this rsqrt"},
    {"dFdx", "dFdx(x)", "screen-space derivative in x; HLSL calls this ddx"},
    {"dFdy", "dFdy(x)", "screen-space derivative in y; HLSL calls this ddy"},
    {"mod", "mod(x, y)", "remainder toward -inf; HLSL's fmod rounds toward zero"},
    {"texture", "texture(sampler, uv)", "sample a combined sampler"},
    {"textureLod", "textureLod(sampler, uv, lod)", "sample at an explicit mip level"},
    {"textureGrad", "textureGrad(sampler, uv, ddx, ddy)", "sample with explicit derivatives"},
    {"textureSize", "textureSize(sampler, lod)", "dimensions of a mip level"},
    {"texelFetch", "texelFetch(sampler, coord, lod)", "unfiltered fetch at integer coordinates"},
    {"imageLoad", "imageLoad(image, coord)", "read from a storage image"},
    {"imageStore", "imageStore(image, coord, value)", "write to a storage image"},
    {"imageSize", "imageSize(image)", "dimensions of a storage image"},
    {"barrier", "barrier()", "workgroup execution barrier"},
    {"memoryBarrier", "memoryBarrier()", "full memory barrier"},
    {"groupMemoryBarrier", "groupMemoryBarrier()", "workgroup memory barrier"},
    {"bitCount", "bitCount(value)", "population count; HLSL calls this countbits"},
    {"findMSB", "findMSB(value)", "index of the highest set bit"},
    {"packHalf2x16", "packHalf2x16(v)", "two floats into one uint of halves"},
    {"unpackHalf2x16", "unpackHalf2x16(bits)", "one uint of halves into two floats"},
    {"floatBitsToInt", "floatBitsToInt(x)", "reinterpret as int; HLSL calls this asint"},
    {"intBitsToFloat", "intBitsToFloat(x)", "reinterpret as float; HLSL calls this asfloat"},
});

/// Types worth completing come first in each list. The completion popup offers
/// only that prefix - a popup listing forty spellings of a vector is a worse
/// popup - while the highlighter colors the whole list.
constexpr std::size_t kCompletableTypes = 12;

constexpr auto kHlslTypes = std::to_array<std::string_view>({
    "float", "float2", "float3", "float4", "float4x4", "int", "uint", "bool",
    "Texture2D", "SamplerState", "RWTexture2D", "StructuredBuffer",
    // Highlighting only past this point.
    "half", "double", "void", "min16float", "min16int", "min16uint",
    "float2x2", "float3x3", "float2x3", "float3x2", "float3x4", "float4x3",
    "int2", "int3", "int4", "uint2", "uint3", "uint4", "bool2", "bool3", "bool4",
    "half2", "half3", "half4", "double2", "double3", "double4",
    "matrix", "vector", "Buffer", "RWBuffer", "ByteAddressBuffer", "RWByteAddressBuffer",
    "RWStructuredBuffer", "ConstantBuffer", "SamplerComparisonState",
    "Texture1D", "Texture3D", "TextureCube", "Texture2DArray", "TextureCubeArray",
    "Texture2DMS", "RWTexture1D", "RWTexture3D",
});

constexpr auto kGlslTypes = std::to_array<std::string_view>({
    "float", "vec2", "vec3", "vec4", "mat4", "int", "uint", "bool",
    "sampler2D", "image2D", "uvec2", "ivec2",
    // Highlighting only past this point.
    "void", "double", "ivec3", "ivec4", "uvec3", "uvec4", "bvec2", "bvec3", "bvec4",
    "dvec2", "dvec3", "dvec4", "mat2", "mat3", "mat2x2", "mat2x3", "mat2x4",
    "mat3x2", "mat3x3", "mat3x4", "mat4x2", "mat4x3", "mat4x4",
    "sampler1D", "sampler3D", "samplerCube", "sampler2DArray", "sampler2DShadow",
    "samplerCubeShadow", "isampler2D", "usampler2D", "isampler3D", "usampler3D",
    "image1D", "image3D", "imageCube", "iimage2D", "uimage2D",
    "texture2D", "texture3D", "textureCube", "sampler", "samplerShadow",
    "atomic_uint", "subpassInput",
});

// Control flow and declaration words shared by the two dialects. These are the
// ones the completion popup offers; the per-language lists below only color.
constexpr auto kKeywords = std::to_array<std::string_view>({
    "if", "else", "for", "while", "return", "struct", "const", "in", "out", "discard",
});

constexpr auto kCommonKeywordsExtra = std::to_array<std::string_view>({
    "do", "switch", "case", "default", "break", "continue", "inout",
    "true", "false", "static", "inline", "uniform", "precise", "sample",
});

constexpr auto kHlslKeywords = std::to_array<std::string_view>({
    "cbuffer", "tbuffer", "register", "packoffset", "groupshared", "nointerpolation",
    "linear", "centroid", "noperspective", "row_major", "column_major", "unorm", "snorm",
    "namespace", "typedef", "class", "interface", "extern", "shared", "volatile",
    "numthreads", "unroll", "loop", "branch", "flatten", "earlydepthstencil",
    "globallycoherent",
    // System-value semantics. The bare ones (TEXCOORD, COLOR, NORMAL) are left
    // out on purpose: they are ordinary words that a shader may well use as a
    // variable name, and coloring those wrong is worse than leaving them plain.
    "SV_Position", "SV_Target", "SV_Target0", "SV_Target1", "SV_Depth",
    "SV_VertexID", "SV_InstanceID", "SV_IsFrontFace",
    "SV_DispatchThreadID", "SV_GroupThreadID", "SV_GroupID", "SV_GroupIndex",
});

constexpr auto kGlslKeywords = std::to_array<std::string_view>({
    "layout", "buffer", "shared", "attribute", "varying", "precision",
    "highp", "mediump", "lowp", "flat", "smooth", "noperspective", "invariant",
    "subroutine", "patch", "coherent", "volatile", "restrict", "readonly", "writeonly",
    "gl_Position", "gl_FragCoord", "gl_PointCoord", "gl_VertexIndex", "gl_InstanceIndex",
    "gl_GlobalInvocationID", "gl_LocalInvocationID", "gl_WorkGroupID", "gl_NumWorkGroups",
    "gl_LocalInvocationIndex", "gl_PointSize", "gl_FrontFacing",
});

/// An HLSL semantic: the name after ':' on a struct member, a parameter or a
/// return type. Completion offers these only in that position, which is why the
/// bare ones (TEXCOORD0, COLOR0, NORMAL) can appear here even though the
/// highlighter's keyword list leaves them out on purpose - after a ':' they
/// cannot be a variable, and everywhere else they can.
struct Semantic {
    const char* name;
    const char* doc;
    unsigned stages;  // which stages it is worth offering in
};

// Stage mask for Semantic::stages. Kept as a bitmask rather than a Stage enum
// because most semantics are legal in more than one stage.
enum : unsigned {
    kSemVertex = 1u << 0,
    kSemFragment = 1u << 1,
    kSemCompute = 1u << 2,
    kSemDraw = kSemVertex | kSemFragment,
    kSemAny = kSemVertex | kSemFragment | kSemCompute,
};

// System-value semantics first, then the user semantics, because SV_ names are
// the ones with a fixed meaning the driver enforces - getting those wrong is a
// compile error, whereas a TEXCOORD number is just a channel.
constexpr auto kHlslSemantics = std::to_array<Semantic>({
    {"SV_Position", "clip-space position out of the vertex stage, pixel coordinates into the fragment stage", kSemDraw},
    {"SV_Target", "the color written to render target 0", kSemFragment},
    {"SV_Target0", "the color written to render target 0", kSemFragment},
    {"SV_Target1", "the color written to render target 1", kSemFragment},
    {"SV_Target2", "the color written to render target 2", kSemFragment},
    {"SV_Target3", "the color written to render target 3", kSemFragment},
    {"SV_Depth", "depth written by the fragment stage, defeats early-z", kSemFragment},
    {"SV_DepthGreaterEqual", "depth that only ever increases, keeps early-z", kSemFragment},
    {"SV_DepthLessEqual", "depth that only ever decreases, keeps early-z", kSemFragment},
    {"SV_IsFrontFace", "true when the triangle faces the camera", kSemFragment},
    {"SV_SampleIndex", "sample being shaded; forces per-sample shading", kSemFragment},
    {"SV_Coverage", "sample coverage mask", kSemFragment},
    {"SV_PrimitiveID", "index of the primitive being rasterized", kSemFragment},
    {"SV_VertexID", "index of the vertex being shaded", kSemVertex},
    {"SV_InstanceID", "index of the instance being drawn", kSemVertex},
    {"SV_ClipDistance", "user clip distance", kSemVertex},
    {"SV_CullDistance", "user cull distance", kSemVertex},
    {"SV_DispatchThreadID", "global thread coordinates", kSemCompute},
    {"SV_GroupThreadID", "thread coordinates within the group", kSemCompute},
    {"SV_GroupID", "coordinates of the group within the dispatch", kSemCompute},
    {"SV_GroupIndex", "flattened index of the thread within the group", kSemCompute},
    {"TEXCOORD0", "interpolated channel 0, usually uv", kSemDraw},
    {"TEXCOORD1", "interpolated channel 1", kSemDraw},
    {"TEXCOORD2", "interpolated channel 2", kSemDraw},
    {"TEXCOORD3", "interpolated channel 3", kSemDraw},
    {"COLOR0", "interpolated vertex color 0", kSemDraw},
    {"COLOR1", "interpolated vertex color 1", kSemDraw},
    {"POSITION", "object-space vertex position from the vertex buffer", kSemVertex},
    {"NORMAL", "vertex normal from the vertex buffer", kSemDraw},
    {"TANGENT", "vertex tangent from the vertex buffer", kSemDraw},
    {"BINORMAL", "vertex bitangent from the vertex buffer", kSemDraw},
    {"BLENDWEIGHT", "skinning weights", kSemVertex},
    {"BLENDINDICES", "skinning bone indices", kSemVertex},
});

/// A built-in that only exists in some stages. Looked up by name across all
/// three intrinsic tables rather than being a field on Intrinsic, because the
/// restricted set is a couple of dozen names out of a hundred and threading a
/// stage mask through every entry would bury the ones that matter.
///
/// Only completion reads this. The highlighter still colors ddx in a vertex
/// shader, because a word written in the wrong stage is the compiler's to
/// complain about and coloring it plain would only look like a typo.
struct StageOnly {
    const char* name;
    unsigned stages;
};

constexpr auto kStageOnly = std::to_array<StageOnly>({
    // Screen-space derivatives, and the two spellings of "throw this pixel
    // away": all of them need neighbouring pixels, which only the fragment
    // stage has.
    {"ddx", kSemFragment},
    {"ddy", kSemFragment},
    {"ddx_fine", kSemFragment},
    {"ddy_fine", kSemFragment},
    {"dFdx", kSemFragment},
    {"dFdy", kSemFragment},
    {"fwidth", kSemFragment},
    {"clip", kSemFragment},
    {"discard", kSemFragment},
    // Barriers synchronise a workgroup, which only a compute dispatch has.
    {"InterlockedAdd", kSemCompute},
    {"GroupMemoryBarrierWithGroupSync", kSemCompute},
    {"DeviceMemoryBarrier", kSemCompute},
    {"barrier", kSemCompute},
    {"memoryBarrier", kSemCompute},
    {"groupMemoryBarrier", kSemCompute},
    // Storage images are legal elsewhere in principle, but writing one outside a
    // compute shader is nearly always a mistake in a project shaped like this.
    {"imageStore", kSemCompute},
    {"imageLoad", kSemCompute},
    {"imageSize", kSemCompute},
});

/// A control-flow statement offered as a whole statement rather than a bare
/// word. `body` is the text that gets inserted: '\t' stands for one level of
/// the editor's indent (which is a tab or some number of spaces depending on
/// settings, so it cannot be baked in here) and '\v' marks where the caret
/// should land afterwards.
struct Statement {
    const char* name;
    const char* body;
    const char* doc;
};

// Identical in both dialects - these are the parts of the two languages that C
// gave them both, which is why there is one table rather than two.
constexpr auto kStatements = std::to_array<Statement>({
    {"if", "if (\v) {\n\t\n}", "conditional"},
    {"else", "else {\n\t\v\n}", "alternative branch"},
    {"elseif", "else if (\v) {\n\t\n}", "chained condition"},
    {"for", "for (int i = 0; i < \v; ++i) {\n\t\n}", "counted loop"},
    {"while", "while (\v) {\n\t\n}", "conditional loop"},
    {"do", "do {\n\t\v\n} while ();", "loop with the test at the end"},
    {"switch", "switch (\v) {\n\tcase 0:\n\t\tbreak;\n\tdefault:\n\t\tbreak;\n}", "multi-way branch"},
    {"struct", "struct \v {\n\t\n};", "aggregate type"},
});

// Recognized only between "layout(" and its closing paren. "set", "binding" and
// "offset" are all words a shader may reasonably use for a variable, so they are
// keywords where they mean something and plain identifiers everywhere else - and
// getting them visibly right is worth the small amount of state it costs, since
// the set/binding numbers are the thing SDL GPU is fussiest about.
constexpr auto kGlslLayoutQualifiers = std::to_array<std::string_view>({
    "location", "binding", "set", "offset", "component", "index",
    "std140", "std430", "push_constant", "constant_id", "input_attachment_index",
    "local_size_x", "local_size_y", "local_size_z", "rgba8", "rgba16f", "rgba32f", "r32f",
    "triangles", "points", "lines", "early_fragment_tests",
});

}  // namespace ssstudio::words

#endif  // SSSTUDIO_CORE_SHADER_WORDS_H
