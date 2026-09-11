#include "ssstudio/reflection.h"

#include <sstream>

namespace ssstudio {
namespace {

const char* scalar_prefix(ScalarType t) {
    switch (t) {
        case ScalarType::Bool: return "bool";
        case ScalarType::Int: return "int";
        case ScalarType::UInt: return "uint";
        case ScalarType::Float: return "float";
        case ScalarType::Double: return "double";
        case ScalarType::Struct: return "struct";
        default: return "float";
    }
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c;
        }
    }
    return out;
}

// SDL GPU register-space contract, per stage.
struct SpaceRule {
    std::uint32_t resource_space;
    std::uint32_t uniform_space;
};

SpaceRule rules_for(Stage s) {
    switch (s) {
        case Stage::Vertex: return {0, 1};
        case Stage::Fragment: return {2, 3};
        case Stage::Compute: return {0, 2};  // space1 is the read-write set
    }
    return {0, 1};
}

}  // namespace

std::string_view to_string(ScalarType t) { return scalar_prefix(t); }

std::string_view to_string(ResourceKind k) {
    switch (k) {
        case ResourceKind::SampledTexture: return "sampled_texture";
        case ResourceKind::StorageTexture: return "storage_texture";
        case ResourceKind::StorageBuffer: return "storage_buffer";
        case ResourceKind::Sampler: return "sampler";
    }
    return "unknown";
}

std::string_view to_string(TextureDim d) {
    switch (d) {
        case TextureDim::Tex2D: return "2d";
        case TextureDim::Tex2DArray: return "2d_array";
        case TextureDim::Tex3D: return "3d";
        case TextureDim::TexCube: return "cube";
        case TextureDim::TexCubeArray: return "cube_array";
        case TextureDim::Buffer: return "buffer";
    }
    return "2d";
}

std::string UniformMember::c_type() const {
    if (type == ScalarType::Struct) return name + "_t";
    std::string base = scalar_prefix(type);
    if (rows > 1) return base + std::to_string(rows) + "x" + std::to_string(cols);
    if (cols > 1) return base + std::to_string(cols);
    return base;
}

std::string VertexInput::sdl_vertex_format() const {
    const char* base = nullptr;
    switch (type) {
        case ScalarType::Float: base = "FLOAT"; break;
        case ScalarType::Int: base = "INT"; break;
        case ScalarType::UInt: base = "UINT"; break;
        default: base = "FLOAT"; break;
    }
    std::string suffix = components > 1 ? std::to_string(components) : std::string("");
    return std::string("SDL_GPU_VERTEXELEMENTFORMAT_") + base + suffix;
}

// ---------------------------------------------------------------------------
std::uint32_t resource_slot_count(const Resource& resource) {
    return resource.array_size == 0 ? 1u : resource.array_size;
}

std::string resource_binding_key(const Resource& resource, std::uint32_t element) {
    if (resource.array_size == 0) return resource.name;
    return resource.name + "[" + std::to_string(element) + "]";
}

std::uint32_t Reflection::num_samplers() const {
    std::uint32_t n = 0;
    for (const auto& r : resources)
        if (r.kind == ResourceKind::SampledTexture) n += r.array_size ? r.array_size : 1;
    return n;
}

std::uint32_t Reflection::num_storage_textures() const {
    std::uint32_t n = 0;
    for (const auto& r : resources)
        if (r.kind == ResourceKind::StorageTexture) n += r.array_size ? r.array_size : 1;
    return n;
}

std::uint32_t Reflection::num_storage_buffers() const {
    std::uint32_t n = 0;
    for (const auto& r : resources)
        if (r.kind == ResourceKind::StorageBuffer) n += r.array_size ? r.array_size : 1;
    return n;
}

std::uint32_t Reflection::num_uniform_buffers() const {
    return static_cast<std::uint32_t>(uniform_blocks.size());
}

std::uint32_t Reflection::num_readonly_storage_textures() const {
    std::uint32_t n = 0;
    for (const auto& r : resources)
        if (r.kind == ResourceKind::StorageTexture && !r.writable) n += r.array_size ? r.array_size : 1;
    return n;
}

std::uint32_t Reflection::num_readonly_storage_buffers() const {
    std::uint32_t n = 0;
    for (const auto& r : resources)
        if (r.kind == ResourceKind::StorageBuffer && !r.writable) n += r.array_size ? r.array_size : 1;
    return n;
}

std::uint32_t Reflection::num_readwrite_storage_textures() const {
    return num_storage_textures() - num_readonly_storage_textures();
}

std::uint32_t Reflection::num_readwrite_storage_buffers() const {
    return num_storage_buffers() - num_readonly_storage_buffers();
}

Diagnostics Reflection::validate_binding_model(const std::string& file) const {
    Diagnostics out;
    const SpaceRule rule = rules_for(stage);

    auto err = [&](const std::string& msg) {
        Diagnostic d;
        d.severity = Severity::Error;
        d.file = file;
        d.message = msg;
        d.code = "SSSTUDIO-BIND";
        out.push_back(std::move(d));
    };

    for (const auto& r : resources) {
        if (stage == Stage::Compute) {
            const bool ok = r.writable ? (r.set == 1) : (r.set == 0);
            if (!ok) {
                err("compute resource '" + r.name + "' is in space/set " + std::to_string(r.set) +
                    "; SDL GPU expects space0 for read-only and space1 for read-write");
            }
        } else if (r.set != rule.resource_space) {
            err(std::string(to_string(stage)) + " resource '" + r.name + "' is in space/set " +
                std::to_string(r.set) + "; SDL GPU expects space" +
                std::to_string(rule.resource_space));
        }
    }

    for (const auto& b : uniform_blocks) {
        if (b.set != rule.uniform_space) {
            err(std::string(to_string(stage)) + " uniform buffer '" + b.name + "' is in space/set " +
                std::to_string(b.set) + "; SDL GPU expects space" +
                std::to_string(rule.uniform_space));
        }
    }

    if (stage == Stage::Compute) {
        for (int i = 0; i < 3; ++i) {
            if (compute_threads[i] == 0) {
                err("compute thread group size component is zero");
                break;
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
namespace {

void member_json(std::ostringstream& os, const UniformMember& m, const std::string& pad) {
    os << pad << "{\"name\":\"" << json_escape(m.name) << "\",\"type\":\"" << to_string(m.type)
       << "\",\"rows\":" << m.rows << ",\"cols\":" << m.cols
       << ",\"array\":" << m.array_size << ",\"offset\":" << m.offset << ",\"size\":" << m.size;
    if (!m.members.empty()) {
        os << ",\"members\":[";
        for (std::size_t i = 0; i < m.members.size(); ++i) {
            if (i) os << ",";
            member_json(os, m.members[i], "");
        }
        os << "]";
    }
    os << "}";
}

}  // namespace

std::string reflection_to_json(const Reflection& r, int indent) {
    const std::string nl = indent > 0 ? "\n" : "";
    const std::string pad = indent > 0 ? std::string(static_cast<std::size_t>(indent), ' ') : "";
    std::ostringstream os;
    os << "{" << nl;
    os << pad << "\"stage\":\"" << to_string(r.stage) << "\"," << nl;
    os << pad << "\"entry_point\":\"" << json_escape(r.entry_point) << "\"," << nl;
    os << pad << "\"counts\":{\"samplers\":" << r.num_samplers()
       << ",\"storage_textures\":" << r.num_storage_textures()
       << ",\"storage_buffers\":" << r.num_storage_buffers()
       << ",\"uniform_buffers\":" << r.num_uniform_buffers() << "}," << nl;

    os << pad << "\"uniform_blocks\":[";
    for (std::size_t i = 0; i < r.uniform_blocks.size(); ++i) {
        const auto& b = r.uniform_blocks[i];
        if (i) os << ",";
        os << "{\"name\":\"" << json_escape(b.name) << "\",\"set\":" << b.set
           << ",\"binding\":" << b.binding << ",\"size\":" << b.size << ",\"members\":[";
        for (std::size_t j = 0; j < b.members.size(); ++j) {
            if (j) os << ",";
            member_json(os, b.members[j], "");
        }
        os << "]}";
    }
    os << "]," << nl;

    os << pad << "\"resources\":[";
    for (std::size_t i = 0; i < r.resources.size(); ++i) {
        const auto& x = r.resources[i];
        if (i) os << ",";
        os << "{\"name\":\"" << json_escape(x.name) << "\",\"kind\":\"" << to_string(x.kind)
           << "\",\"dim\":\"" << to_string(x.dim) << "\",\"set\":" << x.set
           << ",\"binding\":" << x.binding << ",\"writable\":" << (x.writable ? "true" : "false")
           << ",\"array\":" << x.array_size << ",\"stride\":" << x.struct_stride << "}";
    }
    os << "]," << nl;

    os << pad << "\"vertex_inputs\":[";
    for (std::size_t i = 0; i < r.vertex_inputs.size(); ++i) {
        const auto& v = r.vertex_inputs[i];
        if (i) os << ",";
        os << "{\"name\":\"" << json_escape(v.name) << "\",\"semantic\":\""
           << json_escape(v.semantic) << "\",\"location\":" << v.location << ",\"type\":\""
           << to_string(v.type) << "\",\"components\":" << v.components << ",\"sdl_format\":\""
           << v.sdl_vertex_format() << "\"}";
    }
    os << "]," << nl;

    os << pad << "\"outputs\":[";
    for (std::size_t i = 0; i < r.outputs.size(); ++i) {
        const auto& o = r.outputs[i];
        if (i) os << ",";
        os << "{\"name\":\"" << json_escape(o.name) << "\",\"location\":" << o.location
           << ",\"components\":" << o.components << "}";
    }
    os << "]," << nl;

    os << pad << "\"compute_threads\":[" << r.compute_threads[0] << "," << r.compute_threads[1]
       << "," << r.compute_threads[2] << "]" << nl;
    os << "}";
    return os.str();
}

}  // namespace ssstudio
