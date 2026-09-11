// Importing a fullscreen fragment shader written to the common web-shader
// convention.
//
// That convention is a single fragment shader covering the screen, with one
// entry point `void mainImage(out vec4 fragColor, in vec2 fragCoord)` and a
// fixed set of `i`-prefixed uniforms (time, resolution, mouse, four texture
// channels). It declares no version, no descriptor sets and no output variable,
// so it does not compile as Vulkan GLSL on its own.
//
// The import is therefore a wrapping problem rather than a translation one: the
// body is kept byte for byte and a generated prelude and epilogue are put around
// it. Nothing here knows about the preview, the packer or SDL - the result is an
// ordinary GLSL source file that the existing front end compiles.
#ifndef SSSTUDIO_SHADER_IMPORT_H
#define SSSTUDIO_SHADER_IMPORT_H

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ssstudio/project.h"
#include "ssstudio/types.h"

namespace ssstudio {

/// Which entry point a pasted source declares. Only Image can be imported; the
/// rest exist so the importer can refuse them by name instead of handing the
/// user a compile error about a missing function.
enum class ImportEntry : std::uint8_t {
    /// No recognised entry point. Usually means the paste is incomplete, or is
    /// a fragment of a larger shader rather than a whole one.
    None,
    /// void mainImage(out vec4 fragColor, in vec2 fragCoord)
    Image,
    /// vec2 mainSound(int samp, float time) - an audio shader.
    Sound,
    /// void mainVR(out vec4, in vec2, in vec3 rayOrigin, in vec3 rayDirection)
    VR,
    /// void mainCubemap(out vec4, in vec3 rayOrigin, in vec3 rayDirection)
    Cubemap,
};

std::string_view to_string(ImportEntry e);

/// True for the entry points the importer can actually wrap.
bool is_importable(ImportEntry e);

/// What the scanner found in a pasted source. Everything here is advisory
/// except `entry`, which decides whether an import is possible at all.
struct ImportScan {
    /// The entry point the source declares, or None when there is not one.
    ImportEntry entry = ImportEntry::None;
    /// Every entry point found, when the paste carries more than one. Kept so
    /// the report can say what was ignored.
    std::vector<ImportEntry> all_entries;
    /// Channels referenced as a bare `iChannelN` token, indexed 0..3.
    std::array<bool, 4> channels_used{};
    /// Legacy spellings present. Recorded for the import report only - the
    /// compatibility defines are emitted whether or not these are set, because
    /// three unused defines cost nothing and a missed one costs a compile.
    bool uses_texture2d = false;
    bool uses_texturecube = false;
    bool uses_legacy_time = false;
    /// A version directive the paste carried, which the wrapper replaces with
    /// its own. Reported so the user is not surprised that it vanished.
    std::optional<int> version_directive;
    /// The leading comment block when it reads like a licence or attribution
    /// notice. Preserved into the generated file: an imported shader can end up
    /// inside a shipped pack, so the notice has to travel with it.
    std::string leading_comment;
    /// Anything worth telling the user. Never fatal on its own.
    Diagnostics notes;
};

/// Scans a pasted source. Comment-aware, so an entry point mentioned inside a
/// comment is not mistaken for a real one. Never throws and never rejects -
/// judgement about what to do with the result belongs to the caller.
ImportScan scan_fullscreen_source(std::string_view source);

/// Everything the wrapper needs beyond the body itself.
struct ImportOptions {
    /// Shared code the source expects to be in scope, prepended ahead of the
    /// body. Empty when there is none.
    std::string common;
    /// Attribution, written into the generated file's header comment. All three
    /// may be empty.
    std::string source_url;
    std::string author;
    std::string licence;
    /// Write the result as fully opaque, discarding whatever alpha the shader
    /// produced. Correct for a shader whose output is presented directly;
    /// wrong for one whose alpha channel carries data.
    bool force_opaque = true;
    /// Emit the `texture2D` / `textureCube` / legacy-time compatibility defines.
    bool compat_defines = true;
    /// Descriptor sets for the fragment stage. The defaults are what SDL GPU
    /// requires and should not normally be changed; they are parameters so that
    /// a future multipass importer can reuse the emitter unchanged.
    std::uint32_t resource_set = 2;
    std::uint32_t uniform_set = 3;
};

/// Wraps a scanned body into compilable Vulkan GLSL.
///
/// Returns nothing when `scan.entry` cannot be wrapped, having appended an
/// explanation to `out_diags`. The body is copied verbatim apart from version
/// directives, which are removed because the prelude supplies its own.
std::optional<std::string> wrap_fullscreen_source(std::string_view source, const ImportScan& scan,
                                                  const ImportOptions& options,
                                                  Diagnostics& out_diags);

/// Byte offset of the first line of imported body inside a wrapped source, so
/// an editor can fold or grey out the generated prelude. Returns npos when the
/// marker is absent, which is the case for any source not produced by
/// wrap_fullscreen_source.
std::size_t imported_body_offset(std::string_view wrapped);

/// The marker line that separates the generated prelude from the imported body.
/// Exposed so the editor and the tests agree on it rather than each spelling it
/// out.
std::string_view imported_body_marker();

// ---------------------------------------------------------------------------
// Landing an import in a project
// ---------------------------------------------------------------------------

/// One import, as the caller describes it.
struct ImportRequest {
    /// The pasted source, exactly as it arrived.
    std::string source;
    /// The shader id to aim for. Sanitised, and given a numeric suffix when the
    /// project already holds one by that name. Empty means "imported".
    std::string id;
    ImportOptions options;
};

/// What an import wrote, so the caller can register it.
struct ImportResult {
    /// The id the shader actually got, which is not always the one asked for.
    std::string shader_id;
    /// Where the wrapped source was written, relative to the project root.
    std::filesystem::path shader_path;
    /// The vertex shader the preview should pair this one with. Always set: an
    /// imported shader covers the screen, so it needs a partner that does too.
    std::string vertex_id;
    /// Where that vertex shader was written, relative to the project root.
    /// Empty when an existing one was reused, which is also how the caller
    /// tells "register this" apart from "just select it".
    std::filesystem::path vertex_path;
    /// Attribution assembled from the request and the scan, dated today.
    Provenance provenance;
    /// The pairing this import makes sense as. Registering it is what tells the
    /// rest of the tool which vertex shader this one is meant to run with,
    /// rather than leaving it to be guessed from what else is in the project.
    PreviewPipeline pipeline;
    /// What the scan found, so the caller can show a report without rescanning.
    ImportScan scan;
};

// ---------------------------------------------------------------------------
// Importing a whole chain
// ---------------------------------------------------------------------------

/// One pass of a chain read out of a scene description.
struct ImportedPass {
    /// What the description called it, before it is made into a shader id.
    std::string name;
    /// The body, before wrapping.
    std::string source;
    /// True for the pass that draws the presented image.
    bool is_image = false;
    /// Which sampler channel reads what. The key is the channel number; the
    /// value is either another pass's name or an address for an image.
    std::map<std::uint32_t, std::string> channel_passes;
    std::map<std::uint32_t, std::string> channel_urls;
    /// Sampler settings per channel, ready for `Binding::extra`.
    std::map<std::uint32_t, std::map<std::string, std::string>> channel_sampler;
};

/// A whole chain, read but not yet landed.
struct ImportedChain {
    std::vector<ImportedPass> passes;  // buffers in order, image last
    /// Code every pass expects to be in scope, prepended to each of them.
    std::string common;
    std::string name;
    std::string author;
    std::string source_url;
    /// What could not be brought across, named. Never fatal on its own: a
    /// chain missing one channel is still worth importing with that channel
    /// unbound.
    Diagnostics notes;
};

/// Reads a scene description into a chain.
///
/// The format is the one these shaders are exported as: a list of render passes,
/// each with a name, a type, its code, and inputs that name either an image or
/// another pass. Inputs reference a pass by matching an id to that pass's
/// declared output rather than by name, so the id table is what is followed.
///
/// Returns false only when there is nothing usable at all. Anything that could
/// not be brought across - an audio pass, a channel fed by a webcam - is left
/// out and named in `out.notes`.
bool read_imported_chain(std::string_view json, ImportedChain& out, Diagnostics& out_diags);

/// Turns `desired` into an id no shader in `project` is already using. Strips
/// anything that would not survive being turned into an enumerator name.
std::string unique_shader_id(const Project& project, std::string_view desired);

/// Wraps the source and writes it into the project directory, along with a
/// fullscreen vertex shader when the project does not already have one.
///
/// Deliberately does not register anything on `project`: the app has to build an
/// editor buffer alongside each shader it adds and the command line tool does
/// not, so the two register in their own way from `out`. Existing files are
/// never overwritten - that is reported as an error instead.
bool write_import_files(const Project& project, const ImportRequest& request, ImportResult& out,
                        Diagnostics& out_diags);

/// What landing a whole chain produced.
struct ChainImportResult {
    /// The pipeline to register, with its passes already in order.
    PreviewPipeline pipeline;
    /// Every shader written, as {id, relative path}, buffers first.
    std::vector<std::pair<std::string, std::filesystem::path>> shaders;
    /// The fullscreen vertex shader, when one had to be written.
    std::string vertex_id;
    std::filesystem::path vertex_path;
    /// Texture bindings to install, by shader id then by binding key.
    std::map<std::string, std::map<std::string, Binding>> bindings;
    Provenance provenance;
};

/// Writes a whole chain into a project.
///
/// Every pass becomes an ordinary GLSL shader wrapped the same way a single one
/// is, so nothing downstream knows a chain from a shader somebody typed. Channels
/// that read another pass become `pass_output` or `previous_frame` bindings;
/// channels that name an image become `url` bindings, left unfetched - opening a
/// description someone sent must not make this machine start downloading from it.
bool write_chain_files(const Project& project, const ImportedChain& chain,
                       const ImportOptions& options, ChainImportResult& out,
                       Diagnostics& out_diags);

}  // namespace ssstudio

#endif  // SSSTUDIO_SHADER_IMPORT_H
