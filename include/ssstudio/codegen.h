// Generation of the artifacts that ship next to the pack: the id enum header,
// the single-header loader (C API + C++ RAII wrapper), the embed header, docs
// with copy-pasteable snippets, the metadata file and the CMake snippet.
#ifndef SSSTUDIO_CODEGEN_H
#define SSSTUDIO_CODEGEN_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "ssstudio/packer.h"
#include "ssstudio/project.h"

namespace ssstudio {

struct GenInfo {
    std::string project_name;
    std::string profile_name;
    std::string tool_version;
    std::string pack_filename;      // e.g. "shaders.s3pack"
    std::string enum_prefix = "SHADER";
    std::string cpp_namespace;      // empty == no C++ namespace wrapper
    PackLayout layout;
    Compression compression = Compression::LZ4;
    PackStats stats;
    std::vector<std::string> defines;
    std::string build_time;
    /// Attribution for shaders that were imported rather than written here, by
    /// shader id. Carried into SHADERS.md so the terms an imported shader came
    /// with reach whoever ships the pack.
    std::map<std::string, Provenance> provenance;
};

// shaders.h: the id enum plus per-shader resource counts as constants.
std::string generate_id_header(const std::vector<PackShader>& shaders, const GenInfo& info);

// s3pack.h: the loader. `info.layout` is baked into a compile-time signature so a
// header generated for one layout refuses to open a pack built with another.
std::string generate_loader_header(const GenInfo& info);

// shaders_embed.h: the pack as a byte array for zero-file shipping.
std::string generate_embed_header(const std::vector<std::uint8_t>& pack, const GenInfo& info);

// SHADERS.md: resource tables, key values and copy-pasteable C / C++ snippets.
// Compute-only packs get a trimmed template (no pipeline/vertex sections).
std::string generate_docs(const std::vector<PackShader>& shaders, const GenInfo& info);

// shaders_meta.toml: build provenance, fully user-editable, optional at runtime.
std::string generate_meta_toml(const std::vector<PackShader>& shaders, const GenInfo& info);

// shaders_reflection.json: full reflection for external tooling.
std::string generate_reflection_json(const std::vector<PackShader>& shaders);

// CMake helper that reruns the CLI at build time.
std::string generate_cmake_snippet(const GenInfo& info);

// ---------------------------------------------------------------------------
// Snippets shown in the Build panel with copy buttons. Kept separate from the
// docs so the UI and SHADERS.md never drift.
// ---------------------------------------------------------------------------
enum class SnippetFlavor { C, CppRaii };

std::string snippet_create_shader(const PackShader& s, const GenInfo& info, SnippetFlavor f);
std::string snippet_vertex_input_state(const PackShader& s, const GenInfo& info);
std::string snippet_pipeline(const PackShader& vs, const PackShader& fs, const GenInfo& info,
                             SnippetFlavor f);
std::string snippet_uniform_struct(const PackShader& s);
std::string snippet_uniform_push(const PackShader& s);
std::string snippet_samplers(const PackShader& s);
std::string snippet_compute_dispatch(const PackShader& s, const GenInfo& info, SnippetFlavor f);

}  // namespace ssstudio

#endif  // SSSTUDIO_CODEGEN_H
