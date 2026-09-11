// Build orchestration: the same code path is used by the GUI Build panel and by
// the CLI, so a pack produced in CI is byte-identical to one produced in the app.
#ifndef SSSTUDIO_BUILD_H
#define SSSTUDIO_BUILD_H

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "ssstudio/cache.h"
#include "ssstudio/codegen.h"
#include "ssstudio/compiler.h"
#include "ssstudio/packer.h"
#include "ssstudio/project.h"

namespace ssstudio {

enum class BuildStage { Check, Compile, Translate, Pack, Emit, Verify };
std::string_view to_string(BuildStage s);

struct BuildProgress {
    BuildStage stage = BuildStage::Check;
    std::string message;
    int done = 0;
    int total = 0;
};

using ProgressCallback = std::function<void(const BuildProgress&)>;

struct BuildArtifact {
    std::filesystem::path path;
    std::string kind;  // "pack" | "header" | "loader" | "docs" | ...
    std::uint64_t bytes = 0;
};

struct BuildReport {
    bool ok = false;
    int cache_hits = 0;
    int compiled = 0;
    int artifacts_unchanged = 0;
    Diagnostics diagnostics;
    std::vector<BuildArtifact> artifacts;
    PackStats stats;
    std::vector<PackShader> shaders;  // kept so the UI can show snippets
    GenInfo gen_info;
    double seconds = 0.0;
};

struct BuildOptions {
    std::string profile_name;                 // empty == first profile
    std::filesystem::path output_dir;         // overrides the profile
    bool write_files = true;                  // false == dry run
    bool pin_new_keys = true;                 // write assigned keys back
    bool verify_roundtrip = true;             // re-read the pack after writing
    ProgressCallback progress;

    // Shared compile cache. When set, unchanged shaders are not recompiled;
    // pass nullptr to force a cold build.
    CompileCache* cache = nullptr;
    // 0 == hardware concurrency. Shaders compile in parallel; the diagnostics
    // and the resulting pack are ordered as if they had run sequentially.
    int compile_threads = 0;
    // Skip rewriting an artifact whose bytes are unchanged, so a no-op build
    // leaves file timestamps alone and downstream build systems stay quiet.
    bool skip_identical_writes = true;
};

// Front-end only pass over every shader in the project.
Diagnostics check_project(const Project& project, ICompilerBackend& backend,
                          const BuildProfile& profile);

BuildReport build_project(Project& project, ICompilerBackend& backend,
                          const BuildOptions& options);

// Convenience for the CLI: loads, builds, saves pinned keys.
BuildReport build_from_path(const std::filesystem::path& project_path,
                            const BuildOptions& options);

std::string tool_version();

/// What a shader's own binary is named after: its source filename with the
/// language extension taken off, so "sprites/test.frag.hlsl" becomes
/// "test.frag". The name a person would use for the file, rather than the id,
/// which carries the language and reads like a symbol.
std::string shader_binary_stem(const std::filesystem::path& source);

/// The filename that binary is written under.
///
/// The format is left out when the build produced only one, because there it
/// says nothing and "test.frag.bin" is the whole point; it is put in when the
/// build produced several, because two formats cannot share a file. Adding a
/// format to a profile therefore renames these outputs - which is the honest
/// outcome, since one file per shader stops being possible at that moment.
///
/// `extension` may be given with or without its leading dot, and may be empty.
std::string shader_binary_filename(const std::string& stem, ShaderFormat format,
                                   bool single_format, const std::string& extension);

}  // namespace ssstudio

#endif  // SSSTUDIO_BUILD_H
