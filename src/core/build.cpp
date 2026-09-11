#include "ssstudio/build.h"

#include <set>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <fstream>
#include <future>
#include <mutex>
#include <thread>
#include <cstring>
#include <sstream>

#include "ssstudio/hash.h"
#include "ssstudio/keys.h"
#include "ssstudio/templates.h"

namespace ssstudio {
namespace {

Diagnostic error(std::string msg, std::string file = {}) {
    Diagnostic d;
    d.severity = Severity::Error;
    d.code = "SSSTUDIO-BUILD";
    d.message = std::move(msg);
    d.file = std::move(file);
    return d;
}

Diagnostic warning(std::string msg, std::string file = {}) {
    Diagnostic d;
    d.severity = Severity::Warning;
    d.code = "SSSTUDIO-BUILD";
    d.message = std::move(msg);
    d.file = std::move(file);
    return d;
}

bool read_file(const std::filesystem::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream os;
    os << f.rdbuf();
    out = os.str();
    return true;
}

// True when the file already holds exactly these bytes, in which case rewriting
// it would only churn its timestamp and wake up whatever watches the directory.
bool content_matches(const std::filesystem::path& p, const char* data, std::size_t size) {
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) return false;
    if (static_cast<std::uintmax_t>(size) != std::filesystem::file_size(p, ec) || ec) return false;
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::string existing((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return existing.size() == size && std::memcmp(existing.data(), data, size) == 0;
}

bool write_file(const std::filesystem::path& p, const std::string& text, Diagnostics& diags) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) {
        diags.push_back(error("cannot write " + p.string()));
        return false;
    }
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return true;
}

bool write_file(const std::filesystem::path& p, const std::vector<std::uint8_t>& bytes,
                Diagnostics& diags) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) {
        diags.push_back(error("cannot write " + p.string()));
        return false;
    }
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return true;
}

std::string now_string() {
    const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char buf[64] = {0};
    std::tm tm_{};
#if defined(_WIN32)
    gmtime_s(&tm_, &t);
#else
    gmtime_r(&t, &tm_);
#endif
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%SZ", &tm_);
    return buf;
}

CompileRequest make_request(const Project& project, const ShaderDesc& shader,
                            const BuildProfile& profile, const std::string& source) {
    CompileRequest req;
    req.id = shader.id;
    req.source = source;
    req.path = project.absolute(shader.path);
    req.stage = shader.stage;
    req.language = project.language_of(shader);
    req.entry_point = shader.entry_point.empty() ? "main" : shader.entry_point;
    req.profile = shader.profile;
    req.formats = profile.formats;
    req.optimization = profile.optimization;
    req.debug_info = profile.debug_info;
    req.warnings_as_errors = profile.warnings_as_errors;
    req.defines = profile.defines;
    req.defines.insert(req.defines.end(), shader.defines.begin(), shader.defines.end());
    for (const auto& d : profile.include_dirs) req.include_dirs.push_back(project.absolute(d));
    req.include_dirs.push_back(req.path.parent_path());
    return req;
}

}  // namespace

std::string_view to_string(BuildStage s) {
    switch (s) {
        case BuildStage::Check: return "check";
        case BuildStage::Compile: return "compile";
        case BuildStage::Translate: return "translate";
        case BuildStage::Pack: return "pack";
        case BuildStage::Emit: return "emit";
        case BuildStage::Verify: return "verify";
    }
    return "build";
}

std::string shader_binary_stem(const std::filesystem::path& source) {
    const std::string filename = source.filename().string();
    // Only the last extension comes off: "test.frag.hlsl" keeps its stage and
    // becomes "test.frag", which is what the shader is called everywhere else.
    const std::size_t dot = filename.rfind('.');
    if (dot == std::string::npos || dot == 0) return filename;
    return filename.substr(0, dot);
}

std::string shader_binary_filename(const std::string& stem, ShaderFormat format,
                                   bool single_format, const std::string& extension) {
    std::string name = stem;
    if (!single_format) {
        name += '.';
        name += to_string(format);
    }
    if (!extension.empty()) {
        // Typed with or without the dot; both mean the same thing, and guessing
        // wrong would produce "test.fragbin".
        if (extension.front() != '.') name += '.';
        name += extension;
    }
    return name;
}

std::string tool_version() { return std::string("SDL Shader Studio ") + SSSTUDIO_VERSION_STRING; }

Diagnostics check_project(const Project& project, ICompilerBackend& backend,
                          const BuildProfile& profile) {
    Diagnostics out = project.validate();
    for (const auto& shader : project.shaders) {
        std::string source;
        const std::filesystem::path path = project.absolute(shader.path);
        if (!read_file(path, source)) {
            out.push_back(error("cannot read " + path.string(), path.string()));
            continue;
        }
        CompileRequest req = make_request(project, shader, profile, source);
        Diagnostics d = backend.check(req);
        out.insert(out.end(), d.begin(), d.end());
    }
    return out;
}

BuildReport build_project(Project& project, ICompilerBackend& backend,
                          const BuildOptions& options) {
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();

    BuildReport report;
    auto progress = [&](BuildStage stage, std::string message, int done, int total) {
        if (!options.progress) return;
        BuildProgress p;
        p.stage = stage;
        p.message = std::move(message);
        p.done = done;
        p.total = total;
        options.progress(p);
    };

    // --- profile -----------------------------------------------------------
    const BuildProfile* profile = options.profile_name.empty()
                                      ? (project.profiles.empty() ? nullptr : &project.profiles.front())
                                      : project.find_profile(options.profile_name);
    if (!profile) {
        report.diagnostics.push_back(
            error("no build profile named '" + options.profile_name + "'"));
        return report;
    }

    Diagnostics validation = project.validate();
    report.diagnostics.insert(report.diagnostics.end(), validation.begin(), validation.end());
    if (has_errors(validation)) return report;

    // --- shader selection --------------------------------------------------
    std::vector<const ShaderDesc*> selected;
    for (const auto& s : project.shaders) {
        if (!s.include_in_pack) continue;
        if (profile->stage_filter && !(profile->stage_filter & stage_bit(s.stage))) continue;
        selected.push_back(&s);
    }
    if (selected.empty()) {
        report.diagnostics.push_back(error(
            "profile '" + profile->name + "' selects no shaders (check include_in_pack and stages)"));
        return report;
    }

    // --- keys --------------------------------------------------------------
    const PackLayout layout = profile->layout_override.value_or(PackLayout{});
    std::vector<KeyRequest> key_requests;
    for (const auto* s : selected) key_requests.push_back({s->id, s->key});

    std::map<std::string, std::uint32_t> new_pins;
    Diagnostics key_diags;
    const std::vector<std::uint32_t> keys =
        assign_keys(key_requests, profile->key_strategy, layout.key16, key_diags, &new_pins);
    report.diagnostics.insert(report.diagnostics.end(), key_diags.begin(), key_diags.end());
    if (has_errors(key_diags)) return report;

    // --- compile -----------------------------------------------------------
    // Shaders are independent, so they compile in parallel. Results are indexed
    // by position and merged in order afterwards: the diagnostics a user reads
    // and the pack that gets written must not depend on thread scheduling.
    struct Compiled {
        bool ok = false;
        bool from_cache = false;
        Diagnostics diagnostics;
        PackShader shader;
    };
    std::vector<Compiled> compiled(selected.size());

    int worker_count = options.compile_threads > 0
                           ? options.compile_threads
                           : static_cast<int>(std::thread::hardware_concurrency());
    worker_count = std::clamp(worker_count, 1, 16);

    std::atomic<std::size_t> next_index{0};
    std::atomic<int> finished{0};
    std::mutex progress_mutex;

    auto compile_one = [&](std::size_t index) {
        const ShaderDesc* shader = selected[index];
        Compiled out;

        {
            std::lock_guard<std::mutex> lock(progress_mutex);
            progress(BuildStage::Compile, shader->id, finished.load(),
                     static_cast<int>(selected.size()));
        }

        std::string source;
        const std::filesystem::path path = project.absolute(shader->path);
        if (!read_file(path, source)) {
            out.diagnostics.push_back(error("cannot read " + path.string(), path.string()));
            compiled[index] = std::move(out);
            ++finished;
            return;
        }

        CompileRequest req = make_request(project, *shader, *profile, source);

        CompileResult result;
        bool have_result = false;
        const std::uint64_t cache_key = req.cache_key();
        if (options.cache) {
            if (auto hit = options.cache->lookup(cache_key)) {
                result = std::move(*hit);
                have_result = true;
                out.from_cache = true;
            }
        }
        if (!have_result) {
            result = backend.compile(req);
            if (result.ok && options.cache) options.cache->store(cache_key, result);
        }

        out.diagnostics = result.diagnostics;
        if (!result.ok) {
            compiled[index] = std::move(out);
            ++finished;
            return;
        }

        std::uint32_t produced = 0;
        for (const auto& [fmt, bytes] : result.blobs) {
            (void)bytes;
            produced |= fmt;
        }
        const std::uint32_t missing = profile->formats & ~produced;
        if (missing) {
            const std::string msg = "shader '" + shader->id + "' is missing format(s): " +
                                    format_mask_to_string(missing);
            out.diagnostics.push_back(profile->allow_missing_format ? warning(msg, path.string())
                                                                    : error(msg, path.string()));
        }

        PackShader ps;
        ps.id = shader->id;
        ps.name = shader->id;
        ps.entry_point = req.entry_point;
        ps.stage = shader->stage;
        ps.language = req.language;
        ps.key = keys[index];
        ps.reflection = std::move(result.reflection);
        for (auto& [fmt, bytes] : result.blobs) ps.blobs.emplace_back(fmt, std::move(bytes));

        out.shader = std::move(ps);
        out.ok = true;
        compiled[index] = std::move(out);
        ++finished;
    };

    if (worker_count == 1 || selected.size() == 1) {
        for (std::size_t i = 0; i < selected.size(); ++i) compile_one(i);
    } else {
        std::vector<std::thread> workers;
        for (int w = 0; w < worker_count; ++w) {
            workers.emplace_back([&] {
                for (;;) {
                    const std::size_t index = next_index.fetch_add(1);
                    if (index >= selected.size()) return;
                    compile_one(index);
                }
            });
        }
        for (auto& worker : workers) worker.join();
    }

    std::vector<PackShader> packed;
    packed.reserve(selected.size());
    for (auto& entry : compiled) {
        report.diagnostics.insert(report.diagnostics.end(), entry.diagnostics.begin(),
                                  entry.diagnostics.end());
        if (!entry.ok) continue;
        if (entry.from_cache) ++report.cache_hits;
        else ++report.compiled;
        packed.push_back(std::move(entry.shader));
    }

    // Cross-stage check: a vertex/fragment pair must agree on its varyings, and
    // that holds across languages too, since matching is by location.
    //
    // Which shaders form a pair is the whole question. Comparing every vertex
    // shader against every fragment shader is only right when there is one of
    // each: the moment a project holds two of either, most of the combinations
    // are pairings nobody intends, and failing a build over one of those is
    // failing over a question that was never asked. So declared pipelines are
    // believed when there are any, an unambiguous single pair is checked as
    // strictly as before, and anything else is reported without stopping the
    // build.
    {
        const auto find = [&packed](const std::string& id) -> const PackShader* {
            for (const auto& s : packed) {
                if (s.id == id) return &s;
            }
            return nullptr;
        };

        std::vector<std::pair<const PackShader*, const PackShader*>> pairs;
        bool pairs_are_intended = true;

        for (const PreviewPipeline& pipeline : project.pipelines) {
            const PackShader* vertex = find(pipeline.vertex);
            const PackShader* fragment = find(pipeline.fragment);
            if (vertex && fragment) pairs.emplace_back(vertex, fragment);
        }

        if (pairs.empty()) {
            std::vector<const PackShader*> vertices;
            std::vector<const PackShader*> fragments;
            for (const auto& shader : packed) {
                if (shader.stage == Stage::Vertex) vertices.push_back(&shader);
                if (shader.stage == Stage::Fragment) fragments.push_back(&shader);
            }
            // Nothing declared. One of each is still unambiguous; more than that
            // is guesswork, and guesswork must not fail a build.
            pairs_are_intended = vertices.size() <= 1 && fragments.size() <= 1;
            for (const PackShader* vertex : vertices) {
                for (const PackShader* fragment : fragments) {
                    pairs.emplace_back(vertex, fragment);
                }
            }
        }

        for (const auto& [vertex, fragment] : pairs) {
            Diagnostics varying =
                validate_varyings(vertex->reflection, fragment->reflection,
                                  vertex->id + " -> " + fragment->id);
            for (auto& d : varying) {
                // An unread varying is normal even in an intended pair, so only
                // errors were ever promoted. A pair nobody declared cannot be
                // asserted to be wrong at all, so its errors become warnings.
                if (d.severity != Severity::Error) continue;
                if (!pairs_are_intended) d.severity = Severity::Warning;
                report.diagnostics.push_back(std::move(d));
            }
        }
    }

    if (has_errors(report.diagnostics)) return report;
    if (packed.empty()) {
        report.diagnostics.push_back(error("no shader compiled successfully"));
        return report;
    }

    // --- pack --------------------------------------------------------------
    progress(BuildStage::Pack, "writing container", 0, 1);

    PackOptions pack_options;
    pack_options.layout = layout;
    pack_options.compression = profile->compression;
    pack_options.compression_threshold = profile->compression_threshold;
    pack_options.strip_names = profile->strip_names;
    pack_options.strip_reflection = profile->strip_reflection;

    PackBuilder builder;
    for (const auto& s : packed) builder.add(s);

    Diagnostics pack_diags;
    const std::vector<std::uint8_t> pack_bytes =
        builder.build(pack_options, pack_diags, &report.stats);
    report.diagnostics.insert(report.diagnostics.end(), pack_diags.begin(), pack_diags.end());
    if (pack_bytes.empty()) return report;

    // --- generation info ---------------------------------------------------
    GenInfo info;
    info.project_name = project.name;
    info.profile_name = profile->name;
    info.tool_version = tool_version();
    info.pack_filename = profile->pack_basename + layout.extension;
    info.enum_prefix = profile->enum_prefix;
    info.cpp_namespace = profile->cpp_namespace;
    info.layout = layout;
    info.compression = profile->compression;
    info.stats = report.stats;
    info.build_time = now_string();
    info.provenance = project.provenance;
    for (const auto& [k, v] : profile->defines) info.defines.push_back(v.empty() ? k : k + "=" + v);

    report.gen_info = info;
    report.shaders = packed;

    // --- verify ------------------------------------------------------------
    if (options.verify_roundtrip) {
        progress(BuildStage::Verify, "re-reading the pack", 0, 1);
        PackReader reader;
        Diagnostics rd;
        if (!reader.open(pack_bytes, rd)) {
            report.diagnostics.insert(report.diagnostics.end(), rd.begin(), rd.end());
            report.diagnostics.push_back(error("the pack just written could not be read back"));
            return report;
        }
        report.diagnostics.insert(report.diagnostics.end(), rd.begin(), rd.end());
        for (const auto& s : packed) {
            const auto* entry = reader.find(s.key);
            if (!entry) {
                report.diagnostics.push_back(
                    error("shader '" + s.id + "' is missing from the pack it was just written to"));
                continue;
            }
            for (const auto& [fmt, bytes] : s.blobs) {
                Diagnostics bd;
                const auto got = reader.blob(s.key, fmt, bd);
                report.diagnostics.insert(report.diagnostics.end(), bd.begin(), bd.end());
                if (got != bytes) {
                    report.diagnostics.push_back(
                        error("round-trip mismatch for '" + s.id + "' (" +
                              std::string(to_string(fmt)) + ")"));
                }
            }
        }
        if (has_errors(report.diagnostics)) return report;
    }

    if (!options.write_files) {
        report.ok = true;
        report.seconds = std::chrono::duration<double>(clock::now() - start).count();
        return report;
    }

    // --- emit --------------------------------------------------------------
    progress(BuildStage::Emit, "writing artifacts", 0, 1);
    const std::filesystem::path out_dir =
        options.output_dir.empty() ? project.absolute(profile->output_dir) : options.output_dir;

    auto emit = [&](const std::filesystem::path& path, const std::string& text,
                    const char* kind) {
        if (options.skip_identical_writes && content_matches(path, text.data(), text.size())) {
            ++report.artifacts_unchanged;
            BuildArtifact a;
            a.path = path;
            a.kind = kind;
            a.bytes = text.size();
            report.artifacts.push_back(std::move(a));
            return;
        }
        if (!write_file(path, text, report.diagnostics)) return;
        BuildArtifact a;
        a.path = path;
        a.kind = kind;
        a.bytes = text.size();
        report.artifacts.push_back(std::move(a));
    };

    if (profile->emit.pack) {
        const std::filesystem::path p = out_dir / info.pack_filename;
        const bool unchanged =
            options.skip_identical_writes &&
            content_matches(p, reinterpret_cast<const char*>(pack_bytes.data()), pack_bytes.size());
        if (unchanged) ++report.artifacts_unchanged;
        if (unchanged || write_file(p, pack_bytes, report.diagnostics)) {
            BuildArtifact a;
            a.path = p;
            a.kind = "pack";
            a.bytes = pack_bytes.size();
            report.artifacts.push_back(std::move(a));
        }
    }
    if (profile->emit.shader_binaries) {
        // How many distinct formats this build actually produced, rather than
        // how many the profile asked for: a profile may list a format that no
        // shader could be translated to, and naming files after a format that
        // never appeared would be a promise the output does not keep.
        std::set<ShaderFormat> produced;
        for (const auto& s : packed) {
            for (const auto& [fmt, bytes] : s.blobs) {
                (void)bytes;
                produced.insert(fmt);
            }
        }
        const bool single_format = produced.size() <= 1;

        // Two shaders of one name in different languages share a source stem and
        // would overwrite one another. The id cannot collide, so the colliding
        // ones fall back to it - and only those, so one awkward pair does not
        // rename everything else.
        std::map<std::string, int> stem_uses;
        for (const auto& s : packed) {
            const ShaderDesc* desc = project.find_shader(s.id);
            stem_uses[desc ? shader_binary_stem(desc->path) : s.id] += 1;
        }

        for (const auto& s : packed) {
            const ShaderDesc* desc = project.find_shader(s.id);
            std::string stem = desc ? shader_binary_stem(desc->path) : s.id;
            if (stem_uses[stem] > 1) {
                report.diagnostics.push_back(
                    warning("more than one shader is called '" + stem +
                            "'; '" + s.id + "' is written under its id instead"));
                stem = s.id;
            }
            for (const auto& [fmt, bytes] : s.blobs) {
                const std::filesystem::path p =
                    out_dir / shader_binary_filename(stem, fmt, single_format,
                                                     profile->shader_extension);
                const bool unchanged =
                    options.skip_identical_writes &&
                    content_matches(p, reinterpret_cast<const char*>(bytes.data()), bytes.size());
                if (unchanged) ++report.artifacts_unchanged;
                if (unchanged || write_file(p, bytes, report.diagnostics)) {
                    BuildArtifact a;
                    a.path = p;
                    a.kind = "shader";
                    a.bytes = bytes.size();
                    report.artifacts.push_back(std::move(a));
                }
            }
        }
    }
    if (profile->emit.header) {
        emit(out_dir / (profile->pack_basename + ".h"), generate_id_header(packed, info), "header");
    }
    if (profile->emit.loader) {
        emit(out_dir / "s3pack.h", generate_loader_header(info), "loader");
    }
    if (profile->emit.docs) {
        emit(out_dir / "SHADERS.md", generate_docs(packed, info), "docs");
    }
    if (profile->emit.embed_header) {
        emit(out_dir / (profile->pack_basename + "_embed.h"),
             generate_embed_header(pack_bytes, info), "embed");
    }
    if (profile->emit.reflection_json) {
        emit(out_dir / (profile->pack_basename + "_reflection.json"),
             generate_reflection_json(packed), "reflection");
    }
    if (profile->emit.meta_toml) {
        emit(out_dir / (profile->pack_basename + "_meta.toml"), generate_meta_toml(packed, info),
             "meta");
    }
    if (profile->emit.cmake_snippet) {
        emit(out_dir / "ssstudio_shaders.cmake", generate_cmake_snippet(info), "cmake");
    }

    // --- pin keys ----------------------------------------------------------
    if (options.pin_new_keys && profile->key_strategy == KeyStrategy::Explicit && !new_pins.empty()) {
        Diagnostics pd;
        if (!pin_keys(project, new_pins, pd)) {
            report.diagnostics.push_back(
                warning("keys were assigned but could not be written back to the manifest; "
                        "they may change on the next build"));
        }
        report.diagnostics.insert(report.diagnostics.end(), pd.begin(), pd.end());
    }

    report.ok = !has_errors(report.diagnostics);
    report.seconds = std::chrono::duration<double>(clock::now() - start).count();
    return report;
}

BuildReport build_from_path(const std::filesystem::path& project_path,
                            const BuildOptions& options) {
    BuildReport report;
    Project project;
    if (!load_project(project_path, project, report.diagnostics)) return report;

    auto backend = create_default_backend();
    return build_project(project, *backend, options);
}

}  // namespace ssstudio
