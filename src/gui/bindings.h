// Bridge between the I/O panel (which owns binding evaluation) and the preview
// panel (which needs the packed bytes each frame).
#ifndef SSSTUDIO_GUI_BINDINGS_H
#define SSSTUDIO_GUI_BINDINGS_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "ssstudio/pass_graph.h"
#include "ssstudio/reflection.h"

#include "preview/renderer.h"
#include "preview/texture_registry.h"

namespace ssstudio::gui {

class App;

// Packs every bound uniform into the layout its shader reflected. Members with
// no binding stay zero, so a half-finished shader still renders.
UniformFeed evaluate_uniforms(App& app);

/// What one texture binding resolved to, for the row that describes it and for
/// the channel-size macros.
struct TextureBindingState {
    TextureStatus status = TextureStatus::Unbound;
    std::uint32_t width = 1;
    std::uint32_t height = 1;
};

/// The textures for one draw.
///
/// `fragment` is indexed by the reflected binding slot rather than by position
/// in the resource list, because those two are not the same thing: an unused
/// sampler is dropped from the compiled module, so a shader declaring channels
/// 0 and 2 reflects bindings 0 and 2 and nothing at 1.
struct TextureFeed {
    std::vector<ResolvedTexture> fragment;
    /// The same resolutions keyed by resource name, which is how the panel and
    /// the macros ask about them.
    std::map<std::string, TextureBindingState> by_name;
};

/// Uniforms for one named vertex/fragment pair rather than the active one.
/// `channels` supplies the sizes the channel macros report.
UniformFeed evaluate_uniforms_for(App& app, const std::string& vertex_id,
                                  const std::string& fragment_id, const TextureFeed& channels);

/// The chain the active pipeline describes, ready to be scheduled.
///
/// A pipeline with no passes yields a chain of just its fragment shader, which
/// schedules to one pass drawing into one target - precisely what the preview
/// did before any of this existed. The ordinary case is not a special case.
PassChain evaluate_pass_chain(App& app);

/// Uniforms and textures for every pass of a schedule, by shader id.
std::map<std::string, PassFeed> evaluate_pass_feeds(App& app, const PassSchedule& schedule);

/// Resolves every texture the previewed fragment shader declares. Anything
/// unbound, still loading or broken resolves to the white stand-in, so the
/// preview always has something to draw.
TextureFeed evaluate_textures(App& app);

/// The same, for one named fragment shader rather than the previewed one.
TextureFeed evaluate_textures_for(App& app, const std::string& fragment_id);

}  // namespace ssstudio::gui

#endif  // SSSTUDIO_GUI_BINDINGS_H
