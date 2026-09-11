#include "ssstudio/pass_graph.h"

#include <algorithm>
#include <array>

namespace ssstudio {
namespace {

Diagnostic error(std::string message) {
    Diagnostic d;
    d.severity = Severity::Error;
    d.code = "SSSTUDIO-PASS";
    d.message = std::move(message);
    return d;
}

/// Position of a pass in the running order, counting the image pass last.
/// Absent for a name that is not in the chain at all.
struct Order {
    std::map<std::string, std::size_t> index;
    std::size_t image = 0;
};

}  // namespace

bool build_pass_schedule(const PassChain& chain, std::uint32_t parity, PassSchedule& out,
                         Diagnostics& out_diags) {
    out = PassSchedule{};
    out.vertex_shader = chain.vertex_shader;

    if (chain.image_shader.empty()) {
        out_diags.push_back(error("this pipeline has no shader to draw with"));
        out = PassSchedule{};
        return false;
    }

    // --- the running order ------------------------------------------------
    Order order;
    for (std::size_t i = 0; i < chain.buffers.size(); ++i) {
        const PassDesc& pass = chain.buffers[i];
        if (pass.shader_id.empty()) {
            out_diags.push_back(error("pass " + std::to_string(i + 1) + " names no shader"));
            return false;
        }
        // A shader used twice would own two targets under one name, and every
        // reference to it would be ambiguous. Reading a pass by name is the
        // whole vocabulary here, so the names have to be unique.
        if (!order.index.emplace(pass.shader_id, i).second) {
            out_diags.push_back(error("'" + pass.shader_id +
                                      "' appears twice in this pipeline; a pass can only run once"));
            return false;
        }
    }
    order.image = chain.buffers.size();
    if (order.index.count(chain.image_shader) != 0) {
        out_diags.push_back(error("'" + chain.image_shader +
                                  "' is both a buffer pass and the pass that draws the image"));
        return false;
    }
    order.index.emplace(chain.image_shader, order.image);

    // --- which reads cross a frame boundary -------------------------------
    //
    // A read is from the previous frame when the pass being read has not run
    // yet this frame: itself, or one later in the order. That is not a rule
    // being imposed, it is the only thing such a read could mean - and a pass
    // whose result is wanted a frame later is exactly the one that needs two
    // targets to alternate between.
    std::vector<bool> alternates(chain.buffers.size(), false);
    std::map<std::string, std::vector<PassInput>> resolved_inputs;

    for (const auto& [reader, inputs] : chain.inputs) {
        const auto reader_at = order.index.find(reader);
        if (reader_at == order.index.end()) {
            out_diags.push_back(
                error("'" + reader + "' has inputs but is not a pass in this pipeline"));
            return false;
        }

        for (const PassInput& input : inputs) {
            if (input.source_pass.empty()) continue;

            const auto source_at = order.index.find(input.source_pass);
            if (source_at == order.index.end()) {
                out_diags.push_back(error("'" + reader + "' reads '" + input.source_pass +
                                          "', which is not a pass in this pipeline"));
                return false;
            }
            if (input.source_pass == chain.image_shader) {
                // The presented target is not double buffered and is redrawn
                // every frame, so there is no previous copy of it to read. Say
                // what would fix it rather than binding something misleading.
                out_diags.push_back(error("'" + reader + "' reads '" + input.source_pass +
                                          "', which draws the image; make it a buffer pass first"));
                return false;
            }

            PassInput copy = input;
            copy.previous_frame =
                input.previous_frame || source_at->second >= reader_at->second;
            if (copy.previous_frame) alternates[source_at->second] = true;
            resolved_inputs[reader].push_back(copy);
        }
    }

    // --- targets -----------------------------------------------------------
    //
    // One per buffer, two for a buffer somebody reads a frame late, and one for
    // the image. The second copy exists only where it earns itself: a chain
    // nobody reads backwards allocates exactly as many targets as it has passes.
    std::vector<std::array<std::uint32_t, 2>> buffer_targets(chain.buffers.size());
    for (std::size_t i = 0; i < chain.buffers.size(); ++i) {
        const PassDesc& pass = chain.buffers[i];

        TargetDesc target;
        target.format = pass.format;
        target.owner_pass = pass.shader_id;
        target.clear_on_restart = pass.clear_on_restart;
        target.copy = 0;
        buffer_targets[i][0] = static_cast<std::uint32_t>(out.targets.size());
        out.targets.push_back(target);

        if (alternates[i]) {
            target.copy = 1;
            buffer_targets[i][1] = static_cast<std::uint32_t>(out.targets.size());
            out.targets.push_back(target);
            out.double_buffered = true;
        } else {
            buffer_targets[i][1] = buffer_targets[i][0];
        }
    }

    TargetDesc presented;
    // The image target is always eight bits: it is shown rather than sampled,
    // and it is redrawn from nothing every frame, so it has neither the range
    // problem nor the accumulation a buffer has.
    presented.format = PassFormat::Rgba8Unorm;
    presented.clear_on_restart = false;
    out.present_target = static_cast<std::uint32_t>(out.targets.size());
    out.targets.push_back(presented);

    /// Which copy of a buffer holds what this frame writes, and which holds the
    /// frame before. They are the same target when nothing reads it backwards.
    const auto written_this_frame = [&](std::size_t pass) {
        return buffer_targets[pass][alternates[pass] ? (parity & 1u) : 0u];
    };
    const auto written_last_frame = [&](std::size_t pass) {
        return buffer_targets[pass][alternates[pass] ? ((parity & 1u) ^ 1u) : 0u];
    };

    // --- the schedule ------------------------------------------------------
    const auto schedule_one = [&](const std::string& shader_id, std::uint32_t write_target,
                                  bool is_image) {
        ScheduledPass scheduled;
        scheduled.shader_id = shader_id;
        scheduled.write_target = write_target;
        scheduled.is_image = is_image;

        if (const auto it = resolved_inputs.find(shader_id); it != resolved_inputs.end()) {
            for (const PassInput& input : it->second) {
                const std::size_t source = order.index.at(input.source_pass);
                scheduled.read_targets[input.slot] = input.previous_frame
                                                         ? written_last_frame(source)
                                                         : written_this_frame(source);
            }
        }
        out.passes.push_back(std::move(scheduled));
    };

    for (std::size_t i = 0; i < chain.buffers.size(); ++i) {
        schedule_one(chain.buffers[i].shader_id, written_this_frame(i), false);
    }
    schedule_one(chain.image_shader, out.present_target, true);

    // A pass that both writes a target and samples it would be asking the
    // driver for something it refuses, and its complaint is not one anybody can
    // act on. The alternation above makes that unreachable; this says so out
    // loud, so that a change to the assignment fails by name rather than by
    // driver error.
    for (const ScheduledPass& pass : out.passes) {
        for (const auto& [slot, target] : pass.read_targets) {
            if (target != pass.write_target) continue;
            out_diags.push_back(error("'" + pass.shader_id + "' would sample the target it draws "
                                      "into on slot " + std::to_string(slot)));
            out = PassSchedule{};
            return false;
        }
    }
    return true;
}

}  // namespace ssstudio
