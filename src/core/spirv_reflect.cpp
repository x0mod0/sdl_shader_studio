// Minimal SPIR-V reflector.
//
// Only what the tool needs: names, descriptor sets/bindings, resource kinds,
// uniform block member layout, vertex inputs/outputs and the compute local size.
// Keeping this in-tree means there is no SPIRV-Cross dependency and the
// reflection tests run anywhere.
#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#include "ssstudio/spirv_reflect.h"

namespace ssstudio {
namespace {

// --- SPIR-V constants we care about ---------------------------------------
enum Op : std::uint32_t {
    OpName = 5,
    OpMemberName = 6,
    OpEntryPoint = 15,
    OpExecutionMode = 16,
    OpTypeVoid = 19,
    OpTypeBool = 20,
    OpTypeInt = 21,
    OpTypeFloat = 22,
    OpTypeVector = 23,
    OpTypeMatrix = 24,
    OpTypeImage = 25,
    OpTypeSampler = 26,
    OpTypeSampledImage = 27,
    OpTypeArray = 28,
    OpTypeRuntimeArray = 29,
    OpTypeStruct = 30,
    OpTypePointer = 32,
    OpConstant = 43,
    OpVariable = 59,
    OpDecorate = 71,
    OpMemberDecorate = 72,
};

enum Decoration : std::uint32_t {
    DecBlock = 2,
    DecBufferBlock = 3,
    DecArrayStride = 6,
    DecMatrixStride = 7,
    DecBuiltIn = 11,
    DecNonWritable = 24,
    DecLocation = 30,
    DecBinding = 33,
    DecDescriptorSet = 34,
    DecOffset = 35,
};

enum StorageClass : std::uint32_t {
    ScUniformConstant = 0,
    ScInput = 1,
    ScUniform = 2,
    ScOutput = 3,
    ScStorageBuffer = 12,
    ScPushConstant = 9,
};

enum ExecutionMode : std::uint32_t { ExecLocalSize = 17 };
enum ImageDim : std::uint32_t { Dim1D = 0, Dim2D = 1, Dim3D = 2, DimCube = 3, DimBuffer = 5 };

struct TypeInfo {
    Op op = OpTypeVoid;
    std::uint32_t id = 0;
    ScalarType scalar = ScalarType::Unknown;
    std::uint32_t width = 32;
    bool sign = false;
    std::uint32_t component_type = 0;   // vector/matrix/array element
    std::uint32_t component_count = 1;  // vector components / matrix columns
    std::uint32_t array_length = 0;
    std::vector<std::uint32_t> members;
    // image
    std::uint32_t image_dim = Dim2D;
    bool image_arrayed = false;
    std::uint32_t image_sampled = 0;  // 1 = sampled, 2 = storage
    std::uint32_t sampled_image_type = 0;
};

struct VarInfo {
    std::uint32_t id = 0;
    std::uint32_t type_id = 0;   // pointer type
    std::uint32_t pointee = 0;   // pointee type id
    std::uint32_t storage = 0;
};

struct Decorations {
    std::map<std::uint32_t, std::uint32_t> values;  // decoration -> operand
    bool has(std::uint32_t d) const { return values.count(d) != 0; }
    std::uint32_t get(std::uint32_t d, std::uint32_t def = 0) const {
        auto it = values.find(d);
        return it == values.end() ? def : it->second;
    }
};

struct Module {
    std::map<std::uint32_t, TypeInfo> types;
    std::map<std::uint32_t, VarInfo> vars;
    std::map<std::uint32_t, std::string> names;
    std::map<std::uint32_t, std::map<std::uint32_t, std::string>> member_names;
    std::map<std::uint32_t, Decorations> decorations;
    std::map<std::uint32_t, std::map<std::uint32_t, Decorations>> member_decorations;
    std::map<std::uint32_t, std::uint32_t> constants;
    std::uint32_t local_size[3] = {1, 1, 1};
    std::string entry_point;
};

std::string read_string(const std::uint32_t* words, std::size_t count, std::size_t& index) {
    std::string out;
    for (; index < count; ++index) {
        const std::uint32_t w = words[index];
        for (int b = 0; b < 4; ++b) {
            const char c = static_cast<char>((w >> (b * 8)) & 0xFF);
            if (c == '\0') {
                ++index;
                return out;
            }
            out += c;
        }
    }
    return out;
}

ScalarType scalar_of(const Module& m, std::uint32_t type_id) {
    auto it = m.types.find(type_id);
    if (it == m.types.end()) return ScalarType::Unknown;
    const TypeInfo& t = it->second;
    switch (t.op) {
        case OpTypeBool: return ScalarType::Bool;
        case OpTypeInt: return t.sign ? ScalarType::Int : ScalarType::UInt;
        case OpTypeFloat: return t.width == 64 ? ScalarType::Double : ScalarType::Float;
        case OpTypeVector:
        case OpTypeMatrix:
        case OpTypeArray:
        case OpTypeRuntimeArray: return scalar_of(m, t.component_type);
        case OpTypeStruct: return ScalarType::Struct;
        default: return ScalarType::Unknown;
    }
}

void shape_of(const Module& m, std::uint32_t type_id, std::uint32_t& rows, std::uint32_t& cols,
              std::uint32_t& array_size) {
    rows = 1;
    cols = 1;
    array_size = 0;
    auto it = m.types.find(type_id);
    if (it == m.types.end()) return;
    const TypeInfo* t = &it->second;

    if (t->op == OpTypeArray || t->op == OpTypeRuntimeArray) {
        array_size = t->array_length;
        auto inner = m.types.find(t->component_type);
        if (inner == m.types.end()) return;
        t = &inner->second;
    }
    if (t->op == OpTypeMatrix) {
        rows = t->component_count;  // columns, but reported as rows for c_type()
        auto vec = m.types.find(t->component_type);
        cols = vec != m.types.end() ? vec->second.component_count : 1;
        return;
    }
    if (t->op == OpTypeVector) {
        cols = t->component_count;
        return;
    }
}

std::uint32_t byte_size(const Module& m, std::uint32_t type_id) {
    auto it = m.types.find(type_id);
    if (it == m.types.end()) return 0;
    const TypeInfo& t = it->second;
    switch (t.op) {
        case OpTypeBool: return 4;
        case OpTypeInt:
        case OpTypeFloat: return t.width / 8;
        case OpTypeVector: return byte_size(m, t.component_type) * t.component_count;
        case OpTypeMatrix: return byte_size(m, t.component_type) * t.component_count;
        case OpTypeArray: {
            auto dec = m.decorations.find(type_id);
            const std::uint32_t stride =
                dec != m.decorations.end() ? dec->second.get(DecArrayStride, 0) : 0;
            const std::uint32_t elem = stride ? stride : byte_size(m, t.component_type);
            return elem * (t.array_length ? t.array_length : 1);
        }
        case OpTypeStruct: {
            std::uint32_t total = 0;
            auto md = m.member_decorations.find(type_id);
            for (std::size_t i = 0; i < t.members.size(); ++i) {
                std::uint32_t off = 0;
                if (md != m.member_decorations.end()) {
                    auto mit = md->second.find(static_cast<std::uint32_t>(i));
                    if (mit != md->second.end()) off = mit->second.get(DecOffset, 0);
                }
                total = std::max(total, off + byte_size(m, t.members[i]));
            }
            return (total + 15) & ~15u;  // cbuffers round up to 16 bytes
        }
        default: return 0;
    }
}

TextureDim dim_of(const TypeInfo& t) {
    switch (t.image_dim) {
        case Dim3D: return TextureDim::Tex3D;
        case DimCube: return t.image_arrayed ? TextureDim::TexCubeArray : TextureDim::TexCube;
        case DimBuffer: return TextureDim::Buffer;
        default: return t.image_arrayed ? TextureDim::Tex2DArray : TextureDim::Tex2D;
    }
}

void collect_members(const Module& m, std::uint32_t struct_type,
                     std::vector<UniformMember>& out) {
    auto it = m.types.find(struct_type);
    if (it == m.types.end()) return;
    const TypeInfo& t = it->second;
    auto names_it = m.member_names.find(struct_type);
    auto decs_it = m.member_decorations.find(struct_type);

    for (std::size_t i = 0; i < t.members.size(); ++i) {
        UniformMember mem;
        if (names_it != m.member_names.end()) {
            auto n = names_it->second.find(static_cast<std::uint32_t>(i));
            if (n != names_it->second.end()) mem.name = n->second;
        }
        if (mem.name.empty()) mem.name = "member" + std::to_string(i);

        const std::uint32_t member_type = t.members[i];
        mem.type = scalar_of(m, member_type);
        shape_of(m, member_type, mem.rows, mem.cols, mem.array_size);
        mem.size = byte_size(m, member_type);
        if (decs_it != m.member_decorations.end()) {
            auto d = decs_it->second.find(static_cast<std::uint32_t>(i));
            if (d != decs_it->second.end()) mem.offset = d->second.get(DecOffset, 0);
        }
        if (mem.type == ScalarType::Struct) collect_members(m, member_type, mem.members);
        out.push_back(std::move(mem));
    }
}

}  // namespace

Reflection reflect_spirv(const std::vector<std::uint8_t>& spirv, Stage stage,
                         const std::string& entry_point, Diagnostics& out_diags) {
    Reflection refl;
    refl.stage = stage;
    refl.entry_point = entry_point;

    auto fail = [&](const char* msg) {
        Diagnostic d;
        d.severity = Severity::Warning;
        d.code = "SSSTUDIO-REFLECT";
        d.message = std::string("reflection unavailable: ") + msg;
        out_diags.push_back(std::move(d));
        return refl;
    };

    if (spirv.size() < 20 || spirv.size() % 4 != 0) return fail("blob is not word-aligned SPIR-V");

    std::vector<std::uint32_t> words(spirv.size() / 4);
    std::memcpy(words.data(), spirv.data(), spirv.size());
    if (words[0] != 0x07230203u) return fail("bad SPIR-V magic");

    Module m;
    std::size_t i = 5;  // skip header
    while (i < words.size()) {
        const std::uint32_t instruction = words[i];
        const std::uint32_t opcode = instruction & 0xFFFFu;
        const std::uint32_t length = instruction >> 16;
        if (length == 0 || i + length > words.size()) break;
        const std::uint32_t* ops = &words[i + 1];
        const std::uint32_t n = length - 1;

        switch (opcode) {
            case OpName: {
                std::size_t idx = 1;
                m.names[ops[0]] = read_string(ops, n, idx);
                break;
            }
            case OpMemberName: {
                std::size_t idx = 2;
                m.member_names[ops[0]][ops[1]] = read_string(ops, n, idx);
                break;
            }
            case OpEntryPoint: {
                std::size_t idx = 2;
                m.entry_point = read_string(ops, n, idx);
                break;
            }
            case OpExecutionMode: {
                if (n >= 5 && ops[1] == ExecLocalSize) {
                    m.local_size[0] = ops[2];
                    m.local_size[1] = ops[3];
                    m.local_size[2] = ops[4];
                }
                break;
            }
            case OpDecorate: {
                if (n >= 2) m.decorations[ops[0]].values[ops[1]] = n >= 3 ? ops[2] : 1;
                break;
            }
            case OpMemberDecorate: {
                if (n >= 3) {
                    m.member_decorations[ops[0]][ops[1]].values[ops[2]] = n >= 4 ? ops[3] : 1;
                }
                break;
            }
            case OpConstant: {
                if (n >= 3) m.constants[ops[1]] = ops[2];
                break;
            }
            case OpTypeBool:
            case OpTypeVoid: {
                TypeInfo t;
                t.op = static_cast<Op>(opcode);
                t.id = ops[0];
                m.types[t.id] = t;
                break;
            }
            case OpTypeInt: {
                TypeInfo t;
                t.op = OpTypeInt;
                t.id = ops[0];
                t.width = ops[1];
                t.sign = ops[2] != 0;
                m.types[t.id] = t;
                break;
            }
            case OpTypeFloat: {
                TypeInfo t;
                t.op = OpTypeFloat;
                t.id = ops[0];
                t.width = ops[1];
                m.types[t.id] = t;
                break;
            }
            case OpTypeVector:
            case OpTypeMatrix: {
                TypeInfo t;
                t.op = static_cast<Op>(opcode);
                t.id = ops[0];
                t.component_type = ops[1];
                t.component_count = ops[2];
                m.types[t.id] = t;
                break;
            }
            case OpTypeImage: {
                TypeInfo t;
                t.op = OpTypeImage;
                t.id = ops[0];
                t.component_type = ops[1];
                t.image_dim = ops[2];
                t.image_arrayed = n > 4 && ops[4] != 0;
                t.image_sampled = n > 6 ? ops[6] : 1;
                m.types[t.id] = t;
                break;
            }
            case OpTypeSampler: {
                TypeInfo t;
                t.op = OpTypeSampler;
                t.id = ops[0];
                m.types[t.id] = t;
                break;
            }
            case OpTypeSampledImage: {
                TypeInfo t;
                t.op = OpTypeSampledImage;
                t.id = ops[0];
                t.sampled_image_type = ops[1];
                m.types[t.id] = t;
                break;
            }
            case OpTypeArray: {
                TypeInfo t;
                t.op = OpTypeArray;
                t.id = ops[0];
                t.component_type = ops[1];
                auto c = m.constants.find(ops[2]);
                t.array_length = c == m.constants.end() ? 0 : c->second;
                m.types[t.id] = t;
                break;
            }
            case OpTypeRuntimeArray: {
                TypeInfo t;
                t.op = OpTypeRuntimeArray;
                t.id = ops[0];
                t.component_type = ops[1];
                m.types[t.id] = t;
                break;
            }
            case OpTypeStruct: {
                TypeInfo t;
                t.op = OpTypeStruct;
                t.id = ops[0];
                for (std::uint32_t k = 1; k < n; ++k) t.members.push_back(ops[k]);
                m.types[t.id] = t;
                break;
            }
            case OpTypePointer: {
                TypeInfo t;
                t.op = OpTypePointer;
                t.id = ops[0];
                t.component_type = ops[2];
                t.image_sampled = ops[1];  // reuse: storage class
                m.types[t.id] = t;
                break;
            }
            case OpVariable: {
                VarInfo v;
                v.type_id = ops[0];
                v.id = ops[1];
                v.storage = ops[2];
                auto p = m.types.find(v.type_id);
                v.pointee = p != m.types.end() ? p->second.component_type : 0;
                m.vars[v.id] = v;
                break;
            }
            default: break;
        }
        i += length;
    }

    if (stage == Stage::Compute) {
        for (int k = 0; k < 3; ++k) refl.compute_threads[k] = m.local_size[k] ? m.local_size[k] : 1;
    }

    auto name_of = [&](std::uint32_t id, std::uint32_t type_id) {
        auto it = m.names.find(id);
        if (it != m.names.end() && !it->second.empty()) return it->second;
        auto t = m.names.find(type_id);
        if (t != m.names.end() && !t->second.empty()) return t->second;
        return std::string("unnamed_") + std::to_string(id);
    };

    for (const auto& [id, var] : m.vars) {
        const Decorations& dec = m.decorations[id];
        if (dec.has(DecBuiltIn)) continue;  // gl_Position and friends

        auto ptype = m.types.find(var.pointee);
        if (ptype == m.types.end()) continue;
        const TypeInfo* t = &ptype->second;

        std::uint32_t array_size = 0;
        if (t->op == OpTypeArray) {
            array_size = t->array_length;
            auto inner = m.types.find(t->component_type);
            if (inner != m.types.end()) t = &inner->second;
        }

        const std::uint32_t set = dec.get(DecDescriptorSet, 0);
        const std::uint32_t binding = dec.get(DecBinding, 0);

        switch (var.storage) {
            case ScUniform:
            case ScStorageBuffer:
            case ScPushConstant: {
                const Decorations& type_dec = m.decorations[t->id];
                const bool is_storage =
                    var.storage == ScStorageBuffer || type_dec.has(DecBufferBlock);
                if (is_storage) {
                    Resource r;
                    r.name = name_of(id, t->id);
                    r.kind = ResourceKind::StorageBuffer;
                    r.dim = TextureDim::Buffer;
                    r.set = set;
                    r.binding = binding;
                    r.array_size = array_size;
                    r.writable = !m.decorations[t->id].has(DecNonWritable) &&
                                 !dec.has(DecNonWritable);
                    r.struct_name = name_of(t->id, t->id);
                    if (!t->members.empty()) {
                        r.struct_stride = byte_size(m, t->members.front());
                    }
                    refl.resources.push_back(std::move(r));
                } else {
                    UniformBlock b;
                    b.name = name_of(t->id, t->id);
                    if (b.name.rfind("type.", 0) == 0) b.name = b.name.substr(5);
                    if (b.name.rfind("_Globals", 0) == 0) b.name = "Globals";
                    b.set = set;
                    b.binding = binding;
                    b.size = byte_size(m, t->id);
                    collect_members(m, t->id, b.members);
                    refl.uniform_blocks.push_back(std::move(b));
                }
                break;
            }
            case ScUniformConstant: {
                Resource r;
                r.name = name_of(id, t->id);
                r.set = set;
                r.binding = binding;
                r.array_size = array_size;
                if (t->op == OpTypeSampler) {
                    r.kind = ResourceKind::Sampler;
                } else if (t->op == OpTypeSampledImage) {
                    r.kind = ResourceKind::SampledTexture;
                    auto img = m.types.find(t->sampled_image_type);
                    if (img != m.types.end()) r.dim = dim_of(img->second);
                } else if (t->op == OpTypeImage) {
                    if (t->image_sampled == 2) {
                        r.kind = ResourceKind::StorageTexture;
                        r.writable = !dec.has(DecNonWritable);
                    } else {
                        r.kind = ResourceKind::SampledTexture;
                    }
                    r.dim = dim_of(*t);
                } else {
                    continue;
                }
                refl.resources.push_back(std::move(r));
                break;
            }
            case ScInput: {
                if (stage == Stage::Fragment) {
                    // Fragment inputs are the varyings the vertex stage must
                    // have written; recorded for cross-stage validation.
                    VertexInput in;
                    in.name = name_of(id, t->id);
                    in.location = dec.get(DecLocation, 0);
                    in.type = scalar_of(m, t->id);
                    std::uint32_t r = 1, c = 1, a = 0;
                    shape_of(m, t->id, r, c, a);
                    in.components = c;
                    refl.inputs_as_varyings.push_back(std::move(in));
                    break;
                }
                if (stage != Stage::Vertex) break;
                VertexInput v;
                v.name = name_of(id, t->id);
                v.location = dec.get(DecLocation, 0);
                v.type = scalar_of(m, t->id);
                std::uint32_t rows = 1, cols = 1, arr = 0;
                shape_of(m, t->id, rows, cols, arr);
                v.components = cols;
                // DXC keeps HLSL semantics in the name as "in.var.TEXCOORD0".
                const std::size_t dot = v.name.rfind('.');
                if (dot != std::string::npos && v.name.rfind("in.var.", 0) == 0) {
                    v.semantic = v.name.substr(dot + 1);
                    v.name = v.semantic;
                }
                refl.vertex_inputs.push_back(std::move(v));
                break;
            }
            case ScOutput: {
                if (stage == Stage::Vertex) {
                    VertexInput out;
                    out.name = name_of(id, t->id);
                    out.location = dec.get(DecLocation, 0);
                    out.type = scalar_of(m, t->id);
                    std::uint32_t r = 1, c = 1, a = 0;
                    shape_of(m, t->id, r, c, a);
                    out.components = c;
                    refl.outputs_as_varyings.push_back(std::move(out));
                    break;
                }
                if (stage != Stage::Fragment) break;
                FragmentOutput o;
                o.name = name_of(id, t->id);
                o.location = dec.get(DecLocation, 0);
                o.type = scalar_of(m, t->id);
                std::uint32_t rows = 1, cols = 1, arr = 0;
                shape_of(m, t->id, rows, cols, arr);
                o.components = cols;
                refl.outputs.push_back(std::move(o));
                break;
            }
            default: break;
        }
    }

    // Stable ordering: set, then binding. The I/O panel and docs rely on it.
    auto by_slot = [](const auto& a, const auto& b) {
        if (a.set != b.set) return a.set < b.set;
        return a.binding < b.binding;
    };
    std::sort(refl.resources.begin(), refl.resources.end(), by_slot);
    std::sort(refl.uniform_blocks.begin(), refl.uniform_blocks.end(), by_slot);
    std::sort(refl.vertex_inputs.begin(), refl.vertex_inputs.end(),
              [](const VertexInput& a, const VertexInput& b) { return a.location < b.location; });
    std::sort(refl.outputs.begin(), refl.outputs.end(),
              [](const FragmentOutput& a, const FragmentOutput& b) { return a.location < b.location; });
    const auto by_location = [](const VertexInput& a, const VertexInput& b) {
        return a.location < b.location;
    };
    std::sort(refl.outputs_as_varyings.begin(), refl.outputs_as_varyings.end(), by_location);
    std::sort(refl.inputs_as_varyings.begin(), refl.inputs_as_varyings.end(), by_location);

    return refl;
}

}  // namespace ssstudio
