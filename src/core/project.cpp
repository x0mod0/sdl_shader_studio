#include "ssstudio/project.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include <toml++/toml.hpp>

#include "ssstudio/hash.h"
#include "ssstudio/templates.h"

namespace ssstudio {
namespace {

Diagnostic error(std::string msg, std::string file = {}) {
    Diagnostic d;
    d.severity = Severity::Error;
    d.code = "SSSTUDIO-PROJ";
    d.message = std::move(msg);
    d.file = std::move(file);
    return d;
}

Diagnostic warning(std::string msg, std::string file = {}) {
    Diagnostic d;
    d.severity = Severity::Warning;
    d.code = "SSSTUDIO-PROJ";
    d.message = std::move(msg);
    d.file = std::move(file);
    return d;
}

std::string toml_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    return out;
}

std::string path_to_toml(const std::filesystem::path& p) {
    std::string s = p.generic_string();
    return toml_escape(s);
}

// Bindings are written as inline tables so a project stays readable and small.
void write_binding(std::ostream& os, const std::string& key, const Binding& b) {
    os << "\"" << toml_escape(key) << "\" = { source = \"" << to_string(b.source) << "\"";
    if (!b.values.empty()) {
        os << ", value = [";
        for (std::size_t i = 0; i < b.values.size(); ++i) {
            if (i) os << ", ";
            os << b.values[i];
        }
        os << "]";
    }
    if (!b.text.empty()) os << ", text = \"" << toml_escape(b.text) << "\"";
    if (!b.path.empty()) os << ", path = \"" << path_to_toml(b.path) << "\"";
    if (!b.hash.empty()) os << ", hash = \"" << b.hash << "\"";
    if (b.locked) os << ", locked = true";
    for (const auto& [k, v] : b.extra) {
        os << ", " << k << " = \"" << toml_escape(v) << "\"";
    }
    os << " }\n";
}

Binding read_binding(const toml::table& t) {
    Binding b;
    if (auto s = t["source"].value<std::string>()) {
        b.source = binding_source_from_string(*s).value_or(BindingSource::Manual);
    }
    if (auto arr = t["value"].as_array()) {
        for (const auto& v : *arr) {
            if (auto d = v.value<double>()) b.values.push_back(*d);
        }
    }
    if (auto s = t["text"].value<std::string>()) b.text = *s;
    if (auto s = t["path"].value<std::string>()) b.path = *s;
    if (auto s = t["hash"].value<std::string>()) b.hash = *s;
    b.locked = t["locked"].value_or(false);
    for (const auto& [k, v] : t) {
        const std::string key(k.str());
        if (key == "source" || key == "value" || key == "text" || key == "path" ||
            key == "hash" || key == "locked") {
            continue;
        }
        if (auto s = v.value<std::string>()) b.extra[key] = *s;
    }
    return b;
}

void read_binding_group(const toml::table* group, std::map<std::string, Binding>& out) {
    if (!group) return;
    for (const auto& [k, v] : *group) {
        if (const auto* t = v.as_table()) out[std::string(k.str())] = read_binding(*t);
    }
}

void write_binding_group(std::ostream& os, const std::string& shader_id, const char* group,
                         const std::map<std::string, Binding>& bindings) {
    if (bindings.empty()) return;
    os << "\n[bindings." << shader_id << "." << group << "]\n";
    for (const auto& [k, b] : bindings) write_binding(os, k, b);
}

}  // namespace

std::string_view to_string(PassFormat f) {
    switch (f) {
        case PassFormat::Rgba8Unorm: return "rgba8";
        case PassFormat::Rgba16Float: return "rgba16f";
        case PassFormat::Rgba32Float: return "rgba32f";
    }
    return "rgba16f";
}

std::optional<PassFormat> pass_format_from_string(std::string_view s) {
    if (s == "rgba8") return PassFormat::Rgba8Unorm;
    if (s == "rgba16f") return PassFormat::Rgba16Float;
    if (s == "rgba32f") return PassFormat::Rgba32Float;
    return std::nullopt;
}

std::string_view to_string(BindingSource s) {
    switch (s) {
        case BindingSource::Default: return "default";
        case BindingSource::Manual: return "manual";
        case BindingSource::Macro: return "macro";
        case BindingSource::Expression: return "expression";
        case BindingSource::Curve: return "curve";
        case BindingSource::File: return "file";
        case BindingSource::Url: return "url";
        case BindingSource::Procedural: return "procedural";
        case BindingSource::PreviousFrame: return "previous_frame";
        case BindingSource::PassOutput: return "pass_output";
        case BindingSource::BuiltinMesh: return "builtin_mesh";
        case BindingSource::Generated: return "generated";
        case BindingSource::Scene: return "scene";
    }
    return "default";
}

std::optional<BindingSource> binding_source_from_string(std::string_view s) {
    if (s == "default") return BindingSource::Default;
    if (s == "manual") return BindingSource::Manual;
    if (s == "macro") return BindingSource::Macro;
    if (s == "expression") return BindingSource::Expression;
    if (s == "curve") return BindingSource::Curve;
    if (s == "file") return BindingSource::File;
    if (s == "url") return BindingSource::Url;
    if (s == "procedural") return BindingSource::Procedural;
    if (s == "previous_frame") return BindingSource::PreviousFrame;
    if (s == "pass_output") return BindingSource::PassOutput;
    if (s == "builtin_mesh") return BindingSource::BuiltinMesh;
    if (s == "generated") return BindingSource::Generated;
    if (s == "scene") return BindingSource::Scene;
    return std::nullopt;
}

bool ShaderBindings::empty() const {
    return uniforms.empty() && textures.empty() && samplers.empty() && buffers.empty() &&
           vertex_inputs.empty();
}

ShaderDesc* Project::find_shader(const std::string& id) {
    for (auto& s : shaders)
        if (s.id == id) return &s;
    return nullptr;
}

const ShaderDesc* Project::find_shader(const std::string& id) const {
    for (const auto& s : shaders)
        if (s.id == id) return &s;
    return nullptr;
}

PreviewPipeline* Project::find_pipeline(const std::string& name) {
    for (auto& pipeline : pipelines) {
        if (pipeline.name == name) return &pipeline;
    }
    return nullptr;
}

const PreviewPipeline* Project::find_pipeline(const std::string& name) const {
    return const_cast<Project*>(this)->find_pipeline(name);
}

PreviewPipeline* Project::active_pipeline() {
    if (pipelines.empty()) return nullptr;
    // Falling back to the first rather than to nothing: the name is a
    // preference, and a preference that no longer resolves should not take the
    // preview down with it.
    PreviewPipeline* named = find_pipeline(preview.pipeline);
    return named ? named : &pipelines.front();
}

const PreviewPipeline* Project::active_pipeline() const {
    return const_cast<Project*>(this)->active_pipeline();
}

BuildProfile* Project::find_profile(const std::string& name) {
    for (auto& p : profiles)
        if (p.name == name) return &p;
    return nullptr;
}

const BuildProfile* Project::find_profile(const std::string& name) const {
    for (const auto& p : profiles)
        if (p.name == name) return &p;
    return nullptr;
}

std::filesystem::path Project::absolute(const std::filesystem::path& rel) const {
    return rel.is_absolute() ? rel : root / rel;
}

Project Project::create_default(std::string name, std::filesystem::path root) {
    Project p;
    p.name = std::move(name);
    p.root = std::move(root);
    p.manifest = p.root / "project.toml";

    BuildProfile debug;
    debug.name = "debug";
    debug.formats = FORMAT_SPIRV;
    debug.optimization = 0;
    debug.debug_info = true;
    debug.compression = Compression::None;
    debug.strip_names = false;

    BuildProfile release;
    release.name = "release";
    release.formats = FORMAT_SPIRV | FORMAT_DXIL | FORMAT_MSL;
    release.optimization = 3;
    release.compression = Compression::LZ4;

    p.profiles = {debug, release};
    p.macros["pulse"] = "sin(time*2)*0.5+0.5";
    return p;
}

Diagnostics Project::validate() const {
    Diagnostics out;
    std::map<std::string, int> seen;
    for (const auto& s : shaders) {
        if (s.id.empty()) out.push_back(error("a shader has an empty id"));
        if (++seen[s.id] == 2) out.push_back(error("duplicate shader id '" + s.id + "'"));
        if (s.path.empty()) out.push_back(error("shader '" + s.id + "' has no path"));
        else if (!std::filesystem::exists(absolute(s.path))) {
            out.push_back(error("shader '" + s.id + "' points at a missing file: " +
                                s.path.generic_string()));
        }
        if (s.entry_point.empty()) {
            out.push_back(warning("shader '" + s.id + "' has no entry point; assuming 'main'"));
        }
    }
    if (profiles.empty()) out.push_back(error("project has no build profiles"));

    std::map<std::string, int> profile_names;
    for (const auto& p : profiles) {
        if (++profile_names[p.name] == 2) {
            out.push_back(error("duplicate build profile '" + p.name + "'"));
        }
        if (p.formats == FORMAT_NONE) {
            out.push_back(error("profile '" + p.name + "' requests no shader formats"));
        }
        if (p.layout_override) {
            Diagnostics d = p.layout_override->validate();
            out.insert(out.end(), d.begin(), d.end());
        }
    }
    for (const auto& [id, b] : bindings) {
        (void)b;
        if (!find_shader(id)) {
            out.push_back(warning("bindings exist for unknown shader '" + id + "'"));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------
bool load_project(const std::filesystem::path& manifest_or_dir, Project& out,
                  Diagnostics& out_diags) {
    std::filesystem::path manifest = manifest_or_dir;
    std::error_code ec;
    if (std::filesystem::is_directory(manifest, ec)) manifest /= "project.toml";
    if (!std::filesystem::exists(manifest)) {
        out_diags.push_back(error("no manifest at " + manifest.string()));
        return false;
    }

    toml::table tbl;
    try {
        tbl = toml::parse_file(manifest.string());
    } catch (const toml::parse_error& e) {
        Diagnostic d = error(std::string(e.description()), manifest.string());
        d.line = static_cast<int>(e.source().begin.line);
        d.column = static_cast<int>(e.source().begin.column);
        out_diags.push_back(std::move(d));
        return false;
    }

    out = Project{};
    // Resolved to an absolute path here, once, rather than left as whatever the
    // caller passed. Project::absolute() only prepends the root, so a project
    // opened as `ssstudio examples/hello` would hand every shader a path that is
    // still relative - which reads fine in the editor and then fails the moment
    // something outside the process is given it, such as a file:// URL for the
    // desktop's file manager.
    std::error_code path_ec;
    const std::filesystem::path resolved = std::filesystem::weakly_canonical(manifest, path_ec);
    out.manifest = path_ec ? std::filesystem::absolute(manifest, path_ec) : resolved;
    if (path_ec) out.manifest = manifest;  // nothing else to fall back to
    out.root = out.manifest.parent_path();

    if (const auto* project = tbl["project"].as_table()) {
        out.name = (*project)["name"].value_or(std::string("Untitled"));
        out.format_version = (*project)["format_version"].value_or(kProjectFormatVersion);
        if (auto lang = (*project)["default_language"].value<std::string>()) {
            out.default_language = language_from_string(*lang).value_or(Language::HLSL);
        }
        out.auto_map_macros_by_name = (*project)["auto_map_macros"].value_or(true);
    }
    if (out.format_version > kProjectFormatVersion) {
        out_diags.push_back(warning("manifest format version " +
                                    std::to_string(out.format_version) +
                                    " is newer than this build understands; unknown keys are kept "
                                    "only if you do not re-save"));
    }

    if (const auto* arr = tbl["shaders"].as_array()) {
        for (const auto& node : *arr) {
            const auto* t = node.as_table();
            if (!t) continue;
            ShaderDesc s;
            s.id = (*t)["id"].value_or(std::string());
            s.path = (*t)["path"].value_or(std::string());
            if (auto st = (*t)["stage"].value<std::string>()) {
                s.stage = stage_from_string(*st).value_or(Stage::Fragment);
            }
            if (auto l = (*t)["language"].value<std::string>()) s.language = language_from_string(*l);
            s.entry_point = (*t)["entry"].value_or(std::string("main"));
            s.profile = (*t)["profile"].value_or(std::string());
            if (auto k = (*t)["key"].value<std::int64_t>()) {
                s.key = static_cast<std::uint32_t>(*k);
            }
            s.include_in_pack = (*t)["include_in_pack"].value_or(true);
            if (const auto* defs = (*t)["defines"].as_table()) {
                for (const auto& [k, v] : *defs) {
                    s.defines.emplace_back(std::string(k.str()), v.value_or(std::string()));
                }
            }
            out.shaders.push_back(std::move(s));
        }
    }

    if (const auto* binds = tbl["bindings"].as_table()) {
        for (const auto& [shader_id, node] : *binds) {
            const auto* groups = node.as_table();
            if (!groups) continue;
            ShaderBindings sb;
            read_binding_group((*groups)["uniforms"].as_table(), sb.uniforms);
            read_binding_group((*groups)["textures"].as_table(), sb.textures);
            read_binding_group((*groups)["samplers"].as_table(), sb.samplers);
            read_binding_group((*groups)["buffers"].as_table(), sb.buffers);
            read_binding_group((*groups)["vertex_inputs"].as_table(), sb.vertex_inputs);
            out.bindings[std::string(shader_id.str())] = std::move(sb);
        }
    }

    if (const auto* macros = tbl["macros"].as_table()) {
        for (const auto& [k, v] : *macros) {
            out.macros[std::string(k.str())] = v.value_or(std::string());
        }
    }

    if (const auto* provenance = tbl["provenance"].as_table()) {
        for (const auto& [shader_id, node] : *provenance) {
            const auto* t = node.as_table();
            if (!t) continue;
            Provenance p;
            p.url = (*t)["url"].value_or(std::string());
            p.author = (*t)["author"].value_or(std::string());
            p.licence = (*t)["licence"].value_or(std::string());
            p.imported = (*t)["imported"].value_or(std::string());
            if (!p.empty()) out.provenance[std::string(shader_id.str())] = std::move(p);
        }
    }

    if (const auto* arr = tbl["profiles"].as_array()) {
        for (const auto& node : *arr) {
            const auto* t = node.as_table();
            if (!t) continue;
            BuildProfile p;
            p.name = (*t)["name"].value_or(std::string("default"));
            p.formats = FORMAT_NONE;
            if (const auto* fmts = (*t)["formats"].as_array()) {
                for (const auto& f : *fmts) {
                    if (auto s = f.value<std::string>()) {
                        if (auto fmt = format_from_string(*s)) p.formats |= *fmt;
                        else out_diags.push_back(warning("unknown shader format '" + *s +
                                                         "' in profile '" + p.name + "'"));
                    }
                }
            }
            if (p.formats == FORMAT_NONE) p.formats = FORMAT_SPIRV;
            p.output_dir = (*t)["output_dir"].value_or(std::string("build"));
            p.pack_basename = (*t)["pack_basename"].value_or(std::string("shaders"));
            p.shader_extension = (*t)["shader_extension"].value_or(std::string(".bin"));
            p.optimization = static_cast<int>((*t)["optimization"].value_or(3));
            p.debug_info = (*t)["debug_info"].value_or(false);
            p.strip_names = (*t)["strip_names"].value_or(false);
            p.strip_reflection = (*t)["strip_reflection"].value_or(true);
            p.warnings_as_errors = (*t)["warnings_as_errors"].value_or(false);
            p.allow_missing_format = (*t)["allow_missing_format"].value_or(false);
            p.headless_load_test = (*t)["headless_load_test"].value_or(false);
            p.enum_prefix = (*t)["enum_prefix"].value_or(std::string("SHADER"));
            p.cpp_namespace = (*t)["cpp_namespace"].value_or(std::string());
            p.compression_threshold =
                static_cast<std::uint32_t>((*t)["compression_threshold"].value_or(1024));
            if (auto c = (*t)["compression"].value<std::string>()) {
                p.compression = compression_from_string(*c).value_or(Compression::LZ4);
            }
            if (auto k = (*t)["key_strategy"].value<std::string>()) {
                p.key_strategy = key_strategy_from_string(*k).value_or(KeyStrategy::Explicit);
            }
            if (const auto* stages = (*t)["stages"].as_array()) {
                for (const auto& s : *stages) {
                    if (auto str = s.value<std::string>()) {
                        if (auto st = stage_from_string(*str)) p.stage_filter |= stage_bit(*st);
                    }
                }
            }
            if (const auto* emit = (*t)["emit"].as_array()) {
                EmitOptions e{};
                e.pack = e.header = e.loader = e.docs = false;
                for (const auto& n : *emit) {
                    const std::string s = n.value_or(std::string());
                    if (s == "pack") e.pack = true;
                    else if (s == "header") e.header = true;
                    else if (s == "loader") e.loader = true;
                    else if (s == "docs") e.docs = true;
                    else if (s == "embed") e.embed_header = true;
                    else if (s == "reflection") e.reflection_json = true;
                    else if (s == "meta") e.meta_toml = true;
                    else if (s == "cmake") e.cmake_snippet = true;
                    else if (s == "shaders") e.shader_binaries = true;
                    else out_diags.push_back(warning("unknown emit target '" + s + "'"));
                }
                p.emit = e;
            }
            if (const auto* defs = (*t)["defines"].as_table()) {
                for (const auto& [k, v] : *defs) {
                    p.defines.emplace_back(std::string(k.str()), v.value_or(std::string()));
                }
            }
            if (const auto* incs = (*t)["include_dirs"].as_array()) {
                for (const auto& n : *incs) p.include_dirs.push_back(n.value_or(std::string()));
            }
            if (const auto* layout = (*t)["layout"].as_table()) {
                PackLayout l;
                if (auto s = (*layout)["magic"].value<std::string>()) l.set_magic(*s);
                l.extension = (*layout)["extension"].value_or(std::string(".s3pack"));
                if (auto s = (*layout)["header_preset"].value<std::string>()) {
                    l.header_preset = header_preset_from_string(*s).value_or(HeaderPreset::Compact);
                }
                if (auto s = (*layout)["entry_sort"].value<std::string>()) {
                    l.entry_sort = entry_sort_from_string(*s).value_or(EntrySort::ByKey);
                }
                l.blobs_before_table = (*layout)["blobs_before_table"].value_or(false);
                l.key16 = (*layout)["key16"].value_or(false);
                l.alignment = static_cast<std::uint32_t>((*layout)["alignment"].value_or(16));
                l.include_names = (*layout)["include_names"].value_or(true);
                l.include_reflection = (*layout)["include_reflection"].value_or(false);
                l.include_user_section = (*layout)["include_user_section"].value_or(false);
                p.layout_override = l;
            }
            out.profiles.push_back(std::move(p));
        }
    }
    if (out.profiles.empty()) {
        out.profiles.push_back(BuildProfile{});
        out_diags.push_back(warning("no profiles defined; using built-in defaults"));
    }

    if (const auto* pv = tbl["preview"].as_table()) {
        out.preview.width = static_cast<int>((*pv)["width"].value_or(1280));
        out.preview.height = static_cast<int>((*pv)["height"].value_or(720));
        out.preview.follow_panel = (*pv)["follow_panel"].value_or(true);
        out.preview.speed = (*pv)["speed"].value_or(1.0);
        out.preview.paused = (*pv)["paused"].value_or(false);
        out.preview.alpha_checkerboard = (*pv)["alpha_checkerboard"].value_or(true);
        out.preview.blend = (*pv)["blend"].value_or(false);
        // A `mesh` key from an older manifest is ignored rather than carried.
        // It was never honoured by anything - the preview drew the same
        // geometry whatever it said - so there is no setting to preserve and
        // nothing a reader could act on. It disappears on the next save.
        out.preview.pipeline = (*pv)["pipeline"].value_or(std::string());
        if (const auto* c = (*pv)["clear_color"].as_array()) {
            int i = 0;
            for (const auto& n : *c) {
                if (i < 4) out.preview.clear_color[i++] = static_cast<float>(n.value_or(0.0));
            }
        }
    }

    if (const auto* pipelines = tbl["pipelines"].as_array()) {
        for (const auto& node : *pipelines) {
            const auto* t = node.as_table();
            if (!t) continue;
            PreviewPipeline pipeline;
            pipeline.name = (*t)["name"].value_or(std::string());
            pipeline.vertex = (*t)["vertex"].value_or(std::string());
            pipeline.fragment = (*t)["fragment"].value_or(std::string());
            if (const auto* passes = (*t)["passes"].as_array()) {
                for (const auto& entry : *passes) {
                    const auto* pt = entry.as_table();
                    if (!pt) continue;
                    PassDesc pass;
                    pass.shader_id = (*pt)["shader"].value_or(std::string());
                    // A pass with no shader has nothing to run and would only
                    // show up later as a target nobody writes.
                    if (pass.shader_id.empty()) continue;
                    pass.format = pass_format_from_string((*pt)["format"].value_or(std::string()))
                                      .value_or(PassFormat::Rgba16Float);
                    pass.clear_on_restart = (*pt)["clear_on_restart"].value_or(true);
                    pipeline.passes.push_back(std::move(pass));
                }
            }
            // A pipeline with no name cannot be picked from a list, and two of
            // one name cannot be told apart: both are dropped rather than
            // silently shadowing something.
            if (pipeline.name.empty()) {
                out_diags.push_back(warning("a preview pipeline has no name and was skipped"));
                continue;
            }
            if (out.find_pipeline(pipeline.name)) {
                out_diags.push_back(warning("more than one preview pipeline is called '" +
                                            pipeline.name + "'; only the first was kept"));
                continue;
            }
            out.pipelines.push_back(std::move(pipeline));
        }
    }


    Diagnostics v = out.validate();
    out_diags.insert(out_diags.end(), v.begin(), v.end());
    return !has_errors(v);
}

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------
std::string unique_pipeline_name(const Project& project, const std::string& wanted) {
    const std::string base = wanted.empty() ? std::string("Pipeline") : wanted;
    if (project.find_pipeline(base) == nullptr) return base;
    for (int suffix = 2; suffix < 1000; ++suffix) {
        const std::string candidate = base + " " + std::to_string(suffix);
        if (project.find_pipeline(candidate) == nullptr) return candidate;
    }
    return base;
}

bool ensure_default_pipeline(Project& project) {
    if (!project.pipelines.empty()) return false;

    // Exactly one of each. With two fragment shaders there is no obvious pairing
    // and guessing one would be worse than asking; with none there is nothing to
    // pair at all.
    const ShaderDesc* vertex = nullptr;
    const ShaderDesc* fragment = nullptr;
    int vertices = 0;
    int fragments = 0;
    for (const auto& shader : project.shaders) {
        if (shader.stage == Stage::Vertex) {
            ++vertices;
            vertex = &shader;
        } else if (shader.stage == Stage::Fragment) {
            ++fragments;
            fragment = &shader;
        }
    }
    if (vertices != 1 || fragments != 1) return false;

    PreviewPipeline pipeline;
    pipeline.name = "Default";
    pipeline.vertex = vertex->id;
    pipeline.fragment = fragment->id;
    project.pipelines.push_back(std::move(pipeline));
    project.preview.pipeline = "Default";
    return true;
}

bool save_project(const Project& project, Diagnostics& out_diags) {
    std::error_code ec;
    std::filesystem::create_directories(project.root, ec);

    std::ostringstream os;
    os << "# " << project.name << " - SDL Shader Studio project\n";
    os << "# Sources live next to this file; only non-default settings are stored here.\n\n";
    os << "[project]\n";
    os << "name = \"" << toml_escape(project.name) << "\"\n";
    os << "format_version = " << project.format_version << "\n";
    os << "default_language = \"" << to_string(project.default_language) << "\"\n";
    if (!project.auto_map_macros_by_name) os << "auto_map_macros = false\n";

    for (const auto& s : project.shaders) {
        os << "\n[[shaders]]\n";
        os << "id = \"" << toml_escape(s.id) << "\"\n";
        os << "path = \"" << path_to_toml(s.path) << "\"\n";
        os << "stage = \"" << to_string(s.stage) << "\"\n";
        if (s.language) os << "language = \"" << to_string(*s.language) << "\"\n";
        if (s.entry_point != "main") os << "entry = \"" << toml_escape(s.entry_point) << "\"\n";
        if (!s.profile.empty()) os << "profile = \"" << toml_escape(s.profile) << "\"\n";
        if (s.key) os << "key = " << *s.key << "\n";
        if (!s.include_in_pack) os << "include_in_pack = false\n";
        if (!s.defines.empty()) {
            os << "defines = { ";
            for (std::size_t i = 0; i < s.defines.size(); ++i) {
                if (i) os << ", ";
                os << s.defines[i].first << " = \"" << toml_escape(s.defines[i].second) << "\"";
            }
            os << " }\n";
        }
    }

    for (const auto& [id, b] : project.bindings) {
        if (b.empty()) continue;
        write_binding_group(os, id, "uniforms", b.uniforms);
        write_binding_group(os, id, "textures", b.textures);
        write_binding_group(os, id, "samplers", b.samplers);
        write_binding_group(os, id, "buffers", b.buffers);
        write_binding_group(os, id, "vertex_inputs", b.vertex_inputs);
    }

    if (!project.macros.empty()) {
        os << "\n[macros]\n";
        for (const auto& [k, v] : project.macros) {
            os << k << " = \"" << toml_escape(v) << "\"\n";
        }
    }

    for (const auto& [shader_id, p] : project.provenance) {
        if (p.empty()) continue;
        os << "\n[provenance." << shader_id << "]\n";
        if (!p.url.empty()) os << "url = \"" << toml_escape(p.url) << "\"\n";
        if (!p.author.empty()) os << "author = \"" << toml_escape(p.author) << "\"\n";
        if (!p.licence.empty()) os << "licence = \"" << toml_escape(p.licence) << "\"\n";
        if (!p.imported.empty()) os << "imported = \"" << toml_escape(p.imported) << "\"\n";
    }

    for (const auto& p : project.profiles) {
        os << "\n[[profiles]]\n";
        os << "name = \"" << toml_escape(p.name) << "\"\n";
        os << "formats = [";
        bool first = true;
        for (ShaderFormat f : formats_in_mask(p.formats)) {
            if (!first) os << ", ";
            os << "\"" << to_string(f) << "\"";
            first = false;
        }
        os << "]\n";
        os << "output_dir = \"" << toml_escape(p.output_dir) << "\"\n";
        os << "pack_basename = \"" << toml_escape(p.pack_basename) << "\"\n";
        os << "shader_extension = \"" << toml_escape(p.shader_extension) << "\"\n";
        os << "compression = \"" << to_string(p.compression) << "\"\n";
        os << "key_strategy = \"" << to_string(p.key_strategy) << "\"\n";
        if (p.optimization != 3) os << "optimization = " << p.optimization << "\n";
        if (p.debug_info) os << "debug_info = true\n";
        if (p.strip_names) os << "strip_names = true\n";
        if (!p.strip_reflection) os << "strip_reflection = false\n";
        if (p.warnings_as_errors) os << "warnings_as_errors = true\n";
        if (p.allow_missing_format) os << "allow_missing_format = true\n";
        if (p.headless_load_test) os << "headless_load_test = true\n";
        if (p.enum_prefix != "SHADER") os << "enum_prefix = \"" << toml_escape(p.enum_prefix) << "\"\n";
        if (!p.cpp_namespace.empty()) os << "cpp_namespace = \"" << toml_escape(p.cpp_namespace) << "\"\n";
        if (p.compression_threshold != 1024) {
            os << "compression_threshold = " << p.compression_threshold << "\n";
        }
        if (p.stage_filter) {
            os << "stages = [";
            bool f2 = true;
            for (Stage s : {Stage::Vertex, Stage::Fragment, Stage::Compute}) {
                if (!(p.stage_filter & stage_bit(s))) continue;
                if (!f2) os << ", ";
                os << "\"" << to_string(s) << "\"";
                f2 = false;
            }
            os << "]\n";
        }
        os << "emit = [";
        {
            std::vector<const char*> e;
            if (p.emit.pack) e.push_back("pack");
            if (p.emit.header) e.push_back("header");
            if (p.emit.loader) e.push_back("loader");
            if (p.emit.docs) e.push_back("docs");
            if (p.emit.embed_header) e.push_back("embed");
            if (p.emit.reflection_json) e.push_back("reflection");
            if (p.emit.meta_toml) e.push_back("meta");
            if (p.emit.cmake_snippet) e.push_back("cmake");
            if (p.emit.shader_binaries) e.push_back("shaders");
            for (std::size_t i = 0; i < e.size(); ++i) {
                if (i) os << ", ";
                os << "\"" << e[i] << "\"";
            }
        }
        os << "]\n";
        if (!p.defines.empty()) {
            os << "defines = { ";
            for (std::size_t i = 0; i < p.defines.size(); ++i) {
                if (i) os << ", ";
                os << p.defines[i].first << " = \"" << toml_escape(p.defines[i].second) << "\"";
            }
            os << " }\n";
        }
        if (!p.include_dirs.empty()) {
            os << "include_dirs = [";
            for (std::size_t i = 0; i < p.include_dirs.size(); ++i) {
                if (i) os << ", ";
                os << "\"" << toml_escape(p.include_dirs[i]) << "\"";
            }
            os << "]\n";
        }
        if (p.layout_override) {
            const PackLayout& l = *p.layout_override;
            os << "\n[profiles.layout]  # container customization for this profile\n";
            os << "magic = \"" << l.magic_string() << "\"\n";
            os << "extension = \"" << l.extension << "\"\n";
            os << "header_preset = \"" << to_string(l.header_preset) << "\"\n";
            os << "entry_sort = \"" << to_string(l.entry_sort) << "\"\n";
            os << "alignment = " << l.alignment << "\n";
            if (l.blobs_before_table) os << "blobs_before_table = true\n";
            if (l.key16) os << "key16 = true\n";
            if (!l.include_names) os << "include_names = false\n";
            if (l.include_reflection) os << "include_reflection = true\n";
            if (l.include_user_section) os << "include_user_section = true\n";
        }
    }

    // Before [preview], which names one of them: a reader meeting the name first
    // would have to hold it until the list arrived.
    for (const auto& pipeline : project.pipelines) {
        os << "\n[[pipelines]]\n";
        os << "name = \"" << toml_escape(pipeline.name) << "\"\n";
        os << "vertex = \"" << toml_escape(pipeline.vertex) << "\"\n";
        os << "fragment = \"" << toml_escape(pipeline.fragment) << "\"\n";
        for (const PassDesc& pass : pipeline.passes) {
            os << "\n[[pipelines.passes]]\n";
            os << "shader = \"" << toml_escape(pass.shader_id) << "\"\n";
            os << "format = \"" << to_string(pass.format) << "\"\n";
            if (!pass.clear_on_restart) os << "clear_on_restart = false\n";
        }
    }

    os << "\n[preview]\n";
    os << "width = " << project.preview.width << "\n";
    os << "height = " << project.preview.height << "\n";
    os << "follow_panel = " << (project.preview.follow_panel ? "true" : "false") << "\n";
    os << "blend = " << (project.preview.blend ? "true" : "false") << "\n";
    os << "speed = " << project.preview.speed << "\n";
    if (!project.preview.pipeline.empty()) {
        os << "pipeline = \"" << toml_escape(project.preview.pipeline) << "\"\n";
    }
    os << "clear_color = [" << project.preview.clear_color[0] << ", "
       << project.preview.clear_color[1] << ", " << project.preview.clear_color[2] << ", "
       << project.preview.clear_color[3] << "]\n";


    // Write to a temporary file and rename, so a crash mid-write cannot destroy
    // an existing manifest.
    const std::filesystem::path tmp = project.manifest.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            out_diags.push_back(error("cannot write " + tmp.string()));
            return false;
        }
        const std::string text = os.str();
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    std::filesystem::rename(tmp, project.manifest, ec);
    if (ec) {
        out_diags.push_back(error("cannot replace " + project.manifest.string() + ": " +
                                  ec.message()));
        return false;
    }
    return true;
}

bool scaffold_project(const std::filesystem::path& root, const std::string& name,
                      Language language, Project& out, Diagnostics& out_diags) {
    std::error_code ec;
    if (std::filesystem::exists(root / "project.toml", ec)) {
        out_diags.push_back(error("a project already exists at " + root.string()));
        return false;
    }
    if (!std::filesystem::create_directories(root, ec) && ec) {
        out_diags.push_back(error("cannot create " + root.string() + ": " + ec.message()));
        return false;
    }

    Project p = Project::create_default(name.empty() ? root.filename().string() : name, root);
    p.default_language = language;

    // The starter pair is a vertex/fragment couple that already compiles, so the
    // first thing a new project does is succeed rather than report an error.
    struct Starter {
        const char* id;
        const char* basename;
        Stage stage;
    };
    const Starter starters[] = {
        {"sprite_vert", "sprite", Stage::Vertex},
        {"sprite_frag", "sprite", Stage::Fragment},
    };

    for (const auto& starter : starters) {
        ShaderDesc desc;
        desc.id = starter.id;
        desc.stage = starter.stage;
        desc.path = std::filesystem::path("shaders") /
                    (std::string(starter.basename) + default_extension(starter.stage, language));

        const std::filesystem::path absolute = root / desc.path;
        std::filesystem::create_directories(absolute.parent_path(), ec);
        std::ofstream f(absolute, std::ios::binary | std::ios::trunc);
        if (!f) {
            out_diags.push_back(error("cannot write " + absolute.string()));
            return false;
        }
        const std::string source = default_source(starter.stage, language);
        f.write(source.data(), static_cast<std::streamsize>(source.size()));
        if (!f) {
            out_diags.push_back(error("cannot write " + absolute.string()));
            return false;
        }
        p.shaders.push_back(std::move(desc));
    }

    // The two uniforms the preview can drive on its own are bound up front; the
    // tint is left as a literal so there is something to scrub in the UI.
    Binding time_binding;
    time_binding.source = BindingSource::Macro;
    time_binding.text = "time";
    Binding resolution_binding;
    resolution_binding.source = BindingSource::Macro;
    resolution_binding.text = "resolution";
    Binding tint_binding;
    tint_binding.source = BindingSource::Manual;
    tint_binding.values = {1.0, 1.0, 1.0, 1.0};

    ShaderBindings frag_bindings;
    frag_bindings.uniforms["Frame.time"] = time_binding;
    frag_bindings.uniforms["Frame.resolution"] = resolution_binding;
    frag_bindings.uniforms["Frame.tint"] = tint_binding;
    p.bindings["sprite_frag"] = std::move(frag_bindings);

    if (!save_project(p, out_diags)) return false;

    std::ofstream ignore(root / ".gitignore", std::ios::binary | std::ios::trunc);
    if (ignore) ignore << ".cache/\nbuild/\n*.autosave\n";

    out = std::move(p);
    return true;
}

bool pin_keys(Project& project, const std::map<std::string, std::uint32_t>& pins,
              Diagnostics& out_diags) {
    if (pins.empty()) return true;
    bool changed = false;
    for (const auto& [id, key] : pins) {
        if (ShaderDesc* s = project.find_shader(id)) {
            if (!s->key) {
                s->key = key;
                changed = true;
            }
        }
    }
    if (!changed) return true;
    return save_project(project, out_diags);
}

}  // namespace ssstudio
