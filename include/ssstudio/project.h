// Project model and its TOML manifest.
//
// Manifest rules that keep projects small and diff-friendly:
//   * shader sources live in plain files and are never duplicated into TOML;
//   * only bindings that differ from the reflected default are stored;
//   * assets are referenced by relative path + content hash (link by default);
//   * compiled blobs live in .cache/ keyed by source hash and are never shared.
#ifndef SSSTUDIO_PROJECT_H
#define SSSTUDIO_PROJECT_H

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ssstudio/pack_format.h"
#include "ssstudio/types.h"

namespace ssstudio {

/// Bumped to 3 when texture bindings learned to name a URL.
///
/// The bump is not decorative. An unrecognised source degrades to Manual on
/// load, and Manual serialises a value rather than text - so a version that
/// does not know "url" would open such a project, quietly drop the address and
/// write the loss back out on the next save. A project declaring a newer format
/// is refused with a diagnostic instead.
inline constexpr int kProjectFormatVersion = 3;

// ---------------------------------------------------------------------------
// Bindings. A binding says where the value for one reflected slot comes from.
// The set of sources is open-ended, so the value is a small variant tree rather
// than a fixed struct: "scene" and "graph" sources were added without a
// manifest format break.
// ---------------------------------------------------------------------------
enum class BindingSource : std::uint8_t {
    Default,     // not stored in the manifest
    Manual,
    Macro,
    Expression,
    Curve,
    File,
    /// An address, fetched once and cached on disk. `Binding::text` holds it.
    Url,
    Procedural,
    PreviousFrame,
    PassOutput,
    BuiltinMesh,
    Generated,
    Scene,
};

std::string_view to_string(BindingSource s);
std::optional<BindingSource> binding_source_from_string(std::string_view s);

struct Binding {
    BindingSource source = BindingSource::Default;
    std::vector<double> values;                     // Manual
    std::string text;                               // Macro / Expression / Procedural / mesh name
    std::filesystem::path path;                     // File
    std::string hash;                               // content hash of `path`
    std::map<std::string, std::string> extra;       // sampler state, curve id, ...
    bool locked = false;

    bool is_default() const { return source == BindingSource::Default && extra.empty(); }
};

// Bindings for one shader, grouped by reflected slot name.
struct ShaderBindings {
    std::map<std::string, Binding> uniforms;   // "block.member" or "member"
    std::map<std::string, Binding> textures;
    std::map<std::string, Binding> samplers;
    std::map<std::string, Binding> buffers;
    std::map<std::string, Binding> vertex_inputs;

    bool empty() const;
};

// ---------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------
struct ShaderDesc {
    std::string id;                     // stable, used for keys and enum names
    std::filesystem::path path;         // relative to the project root
    Stage stage = Stage::Fragment;
    std::optional<Language> language;   // falls back to Project::default_language
    std::string entry_point = "main";
    std::string profile;                // shader model override, may be empty
    std::optional<std::uint32_t> key;   // explicit key strategy
    bool include_in_pack = true;
    std::vector<std::pair<std::string, std::string>> defines;

    std::string display_name() const { return id; }
};

// ---------------------------------------------------------------------------
// Provenance
// ---------------------------------------------------------------------------

/// Where a shader that was imported rather than written came from.
///
/// This exists because a build produces a pack someone ships. A shader brought
/// in from elsewhere usually carries terms, and those terms have to survive the
/// journey from the paste box to the generated documentation - otherwise the
/// tool quietly becomes a way of losing them. Every field is optional; an entry
/// with nothing in it is not written.
struct Provenance {
    /// Where the shader was found, when the importer was told.
    std::string url;
    /// Who wrote it.
    std::string author;
    /// The terms it is offered under, as free text rather than an identifier:
    /// the notices in the wild do not agree on a vocabulary.
    std::string licence;
    /// The date of the import, ISO 8601, so a stale attribution can be dated.
    std::string imported;

    bool empty() const {
        return url.empty() && author.empty() && licence.empty() && imported.empty();
    }
};

// ---------------------------------------------------------------------------
// Build profiles
// ---------------------------------------------------------------------------
struct EmitOptions {
    bool pack = true;
    bool header = true;
    bool loader = true;
    bool docs = true;
    bool embed_header = false;
    bool reflection_json = false;
    bool meta_toml = false;
    bool cmake_snippet = false;
    /// Each shader's compiled bytes as a file of its own, beside the pack. For
    /// an engine that loads shaders one at a time rather than from a pack, and
    /// for looking at a single blob with an external tool.
    bool shader_binaries = false;
};

struct BuildProfile {
    std::string name = "default";
    std::uint32_t formats = FORMAT_SPIRV;
    std::string output_dir = "build";
    std::string pack_basename = "shaders";
    /// Extension for the per-shader binaries, when emit.shader_binaries is on.
    /// A leading dot is optional; "bin" and ".bin" mean the same thing, and an
    /// empty string means no extension at all.
    std::string shader_extension = ".bin";
    int optimization = 3;
    bool debug_info = false;
    bool strip_names = false;
    bool strip_reflection = true;
    bool warnings_as_errors = false;
    bool allow_missing_format = false;
    bool headless_load_test = false;
    std::uint32_t stage_filter = 0;  // 0 == all stages
    Compression compression = Compression::LZ4;
    std::uint32_t compression_threshold = 1024;
    KeyStrategy key_strategy = KeyStrategy::Explicit;
    std::string enum_prefix = "SHADER";
    std::string cpp_namespace;  // empty == plain C enum only
    EmitOptions emit;
    std::optional<PackLayout> layout_override;  // per-profile format override
    std::vector<std::pair<std::string, std::string>> defines;
    std::vector<std::string> include_dirs;
};

// ---------------------------------------------------------------------------
// Preview pipelines
// ---------------------------------------------------------------------------

/// The shaders the preview draws together, under a name a person chose.
///
/// One vertex and one fragment shader, which is what the preview can draw
/// today. It is a struct rather than a pair of strings on PreviewSettings
/// because a project keeps several and switches between them, and because the
/// day a pipeline grows a third stage or a blend state, this is where it goes.
/// Pixel format of a buffer pass's target.
///
/// Buffers default to floating point because they routinely hold data rather
/// than colour - positions, velocities, counters, accumulators - and eight bits
/// per channel would clamp and quantise all of it away without saying so.
enum class PassFormat : std::uint8_t { Rgba8Unorm, Rgba16Float, Rgba32Float };

std::string_view to_string(PassFormat f);
std::optional<PassFormat> pass_format_from_string(std::string_view s);

/// One intermediate pass of a pipeline.
///
/// There is deliberately no "is this the final pass" flag. The pipeline's
/// `fragment` is the pass that draws what you see, and these are the passes that
/// run before it - so the final one is named exactly once, and a chain cannot be
/// written down with no output or with two of them.
struct PassDesc {
    /// Fragment shader this pass runs. Also the name it is referred to by.
    std::string shader_id;
    PassFormat format = PassFormat::Rgba16Float;
    /// Cleared before the first frame and whenever the preview is restarted,
    /// not every frame - a buffer that was cleared every frame could not
    /// accumulate, which is most of the point of having one.
    bool clear_on_restart = true;
};

struct PreviewPipeline {
    std::string name;
    std::string vertex;    // shader id
    std::string fragment;  // shader id, and the pass that draws what is shown
    /// Passes that run ahead of `fragment`, in order, each rendering into a
    /// target the later ones can sample.
    ///
    /// Empty is the ordinary case and means precisely what it meant before this
    /// existed: `vertex` and `fragment`, once, to the screen.
    std::vector<PassDesc> passes;
};

// ---------------------------------------------------------------------------
// Preview / playback state that belongs to the project rather than the app
// ---------------------------------------------------------------------------
struct PreviewSettings {
    int width = 1280;
    int height = 720;
    bool follow_panel = true;
    double speed = 1.0;
    bool paused = false;
    float clear_color[4] = {0.05f, 0.05f, 0.07f, 1.0f};
    bool alpha_checkerboard = true;
    /// Alpha-blend the shader's output against the clear colour instead of
    /// writing it straight to the target. Off by default because a fragment
    /// shader's alpha is far more often data than coverage - a shader returning
    /// alpha < 1 would otherwise preview washed out for no visible reason, and
    /// a multipass buffer's alpha channel would be destroyed outright.
    bool blend = false;
    /// There is deliberately no mesh here. Built-in geometry belongs to the
    /// scene harness, which picks it per entity and from a vocabulary that
    /// means something (SceneRenderer's cube | sphere | plane | quad). What the
    /// bare preview draws is not a setting at all: a vertex shader that
    /// declares no inputs builds its own geometry from SV_VertexID, and one
    /// that declares inputs is fed a quad laid out to match them. A `mesh` key
    /// left in an older manifest is read and dropped; see load_project().
    /// Name of the pipeline being previewed. Empty, or naming one that is not
    /// there, means the first - so a project can never end up previewing
    /// nothing because a pipeline was renamed underneath it.
    std::string pipeline;
};

// ---------------------------------------------------------------------------
// Project
// ---------------------------------------------------------------------------
struct Project {
    std::string name = "Untitled";
    int format_version = kProjectFormatVersion;
    Language default_language = Language::HLSL;
    std::filesystem::path root;      // directory holding project.toml
    std::filesystem::path manifest;  // root/project.toml

    std::vector<ShaderDesc> shaders;
    std::map<std::string, ShaderBindings> bindings;  // by shader id
    std::map<std::string, std::string> macros;       // name -> expression
    /// Attribution for imported shaders, by shader id. Absent for anything
    /// written in the editor.
    std::map<std::string, Provenance> provenance;
    std::vector<BuildProfile> profiles;
    std::vector<PreviewPipeline> pipelines;
    PreviewSettings preview;
    bool auto_map_macros_by_name = true;

    ShaderDesc* find_shader(const std::string& id);
    const ShaderDesc* find_shader(const std::string& id) const;
    PreviewPipeline* find_pipeline(const std::string& name);
    const PreviewPipeline* find_pipeline(const std::string& name) const;
    /// The pipeline being previewed: the one PreviewSettings names, or the first
    /// when it names nothing that is there. Null only when there are none.
    PreviewPipeline* active_pipeline();
    const PreviewPipeline* active_pipeline() const;
    BuildProfile* find_profile(const std::string& name);
    const BuildProfile* find_profile(const std::string& name) const;

    std::filesystem::path cache_dir() const { return root / ".cache"; }
    std::filesystem::path absolute(const std::filesystem::path& rel) const;

    Language language_of(const ShaderDesc& s) const {
        return s.language.value_or(default_language);
    }

    static Project create_default(std::string name, std::filesystem::path root);
    Diagnostics validate() const;
};

// TOML persistence. Both return false and fill diagnostics on failure.
bool load_project(const std::filesystem::path& manifest_or_dir, Project& out,
                  Diagnostics& out_diags);
bool save_project(const Project& project, Diagnostics& out_diags);

/// Gives a project its first pipeline, when there is exactly one vertex and one
/// fragment shader to make it out of. Returns true when it added one.
///
/// Only ever the first: once a project has any pipeline, this does nothing. That
/// is what lets the one it creates be renamed or deleted like any other, instead
/// of reappearing the moment it is gone.
bool ensure_default_pipeline(Project& project);

/// `wanted`, or `wanted` with a number after it, whichever is not taken. Two
/// pipelines with one name would make the dropdown a guess.
std::string unique_pipeline_name(const Project& project, const std::string& wanted);

// Writes a ready-to-build starter project into `root`: the manifest, a vertex
// and a fragment shader from the stage templates, and a .gitignore. `ssstudio new`
// and the app's File > New project both go through here so the two cannot drift.
// Refuses to touch a directory that already holds a manifest.
bool scaffold_project(const std::filesystem::path& root, const std::string& name,
                      Language language, Project& out, Diagnostics& out_diags);

// Writes back keys assigned by the Explicit strategy, preserving the rest of the
// file (comments included) where the TOML library allows it.
bool pin_keys(Project& project, const std::map<std::string, std::uint32_t>& pins,
              Diagnostics& out_diags);

}  // namespace ssstudio

#endif  // SSSTUDIO_PROJECT_H
