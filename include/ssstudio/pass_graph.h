// Working out what a multipass pipeline does each frame, before any of it runs.
//
// A chain is a handful of buffer passes followed by the pass that draws what you
// see. Each renders into a target the later ones can sample, and a buffer may
// sample itself - which is how anything that accumulates over time works.
//
// The awkward parts of that are all decisions rather than drawing: how many
// targets are needed, which one each pass writes, which one each sampler slot
// reads, and which pairs alternate between frames. So they are made here, in a
// pure function over plain data, and the renderer is left with a list to execute
// and nothing to decide. That is the same split the scene layer already makes,
// and it is what lets the hard half be tested with no GPU, no display and no SDL.
#ifndef SSSTUDIO_PASS_GRAPH_H
#define SSSTUDIO_PASS_GRAPH_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "ssstudio/project.h"
#include "ssstudio/types.h"

namespace ssstudio {

/// One sampler slot of one pass that reads another pass rather than an image.
struct PassInput {
    /// The reflected binding slot this fills.
    std::uint32_t slot = 0;
    /// Shader id of the pass being read.
    std::string source_pass;
    /// Read the frame before this one rather than the one being drawn.
    ///
    /// Set explicitly by a `previous_frame` binding. It is also forced on for a
    /// read that could not mean anything else - a pass reading itself, or
    /// reading one that has not run yet this frame - so the scheduler's answer
    /// says what will actually happen rather than what was asked for.
    bool previous_frame = false;
};

/// A chain as the project describes it, gathered for scheduling.
struct PassChain {
    /// Buffer passes, in the order they run.
    std::vector<PassDesc> buffers;
    /// The shader drawing the presented image, which runs after every buffer.
    std::string image_shader;
    /// The vertex shader every pass is drawn with. One for the whole chain:
    /// passes differ in what they compute, not in the geometry they cover.
    std::string vertex_shader;
    /// Which sampler slots of which pass read another pass, by shader id. A
    /// slot that is absent is filled from the texture registry instead - a file,
    /// an address, or the stand-in.
    std::map<std::string, std::vector<PassInput>> inputs;
};

/// One target the pool has to provide.
struct TargetDesc {
    PassFormat format = PassFormat::Rgba16Float;
    /// The pass this belongs to, for the buffer inspector and for messages.
    /// Empty for the presented target.
    std::string owner_pass;
    /// 0 or 1 for a pass that alternates between frames, 0 for everything else.
    std::uint32_t copy = 0;
    /// Cleared before the first frame and on restart rather than every frame.
    bool clear_on_restart = true;
};

/// One entry of a resolved schedule: run this shader into this target, with
/// these slots fed from these targets.
struct ScheduledPass {
    std::string shader_id;
    /// Index into PassSchedule::targets that this pass renders into.
    std::uint32_t write_target = 0;
    /// Sampler slot to target index, for slots that read a pass. Anything
    /// absent is left to the texture registry.
    std::map<std::uint32_t, std::uint32_t> read_targets;
    /// True for the pass whose output is shown.
    bool is_image = false;
};

/// A whole frame, resolved. Executing this top to bottom is the entire contract
/// between the scheduler and the renderer.
struct PassSchedule {
    std::vector<TargetDesc> targets;
    std::vector<ScheduledPass> passes;
    /// Index into `targets` holding what the panel displays.
    std::uint32_t present_target = 0;
    /// Carried through from the chain, so the renderer needs nothing else to
    /// build a pipeline for each pass.
    std::string vertex_shader;
    /// True when some pass alternates targets between frames, which is what
    /// makes the schedule differ by parity.
    bool double_buffered = false;
};

/// Builds the schedule for one frame parity, or fails without producing one.
///
/// `parity` is the frame number's low bit. Where nothing alternates the two
/// parities give identical schedules; where something does, they differ only in
/// which copy of those targets is written and read. Build both once and index
/// them by parity rather than recomputing per frame.
///
/// Fails rather than half-succeeding: on false, `out` is left empty and
/// `out_diags` says what was wrong in terms of the passes involved.
bool build_pass_schedule(const PassChain& chain, std::uint32_t parity, PassSchedule& out,
                         Diagnostics& out_diags);

}  // namespace ssstudio

#endif  // SSSTUDIO_PASS_GRAPH_H
