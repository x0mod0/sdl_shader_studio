// Scheduling a multipass pipeline.
//
// No GPU, no display, no SDL. Everything hard about multipass - how many targets
// are needed, which one each pass writes, which reads cross a frame boundary,
// and what alternates - is decided here, so it can all be pinned down in plain
// data before anything is drawn.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include "ssstudio/pass_graph.h"
#include "ssstudio/project.h"
#include "test.h"

using namespace ssstudio;

namespace {

PassDesc buffer(std::string shader, PassFormat format = PassFormat::Rgba16Float) {
    PassDesc pass;
    pass.shader_id = std::move(shader);
    pass.format = format;
    return pass;
}

PassInput reads(std::uint32_t slot, std::string source, bool previous = false) {
    PassInput input;
    input.slot = slot;
    input.source_pass = std::move(source);
    input.previous_frame = previous;
    return input;
}

/// The scheduled entry for one shader, or null.
const ScheduledPass* find(const PassSchedule& schedule, const std::string& shader) {
    const auto it = std::find_if(schedule.passes.begin(), schedule.passes.end(),
                                 [&shader](const ScheduledPass& p) {
                                     return p.shader_id == shader;
                                 });
    return it == schedule.passes.end() ? nullptr : &*it;
}

PassSchedule schedule_of(const PassChain& chain, std::uint32_t parity = 0) {
    PassSchedule out;
    Diagnostics diags;
    build_pass_schedule(chain, parity, out, diags);
    return out;
}

bool refuses(const PassChain& chain, Diagnostics& out_diags) {
    PassSchedule out;
    const bool ok = build_pass_schedule(chain, 0, out, out_diags);
    // A refusal must not leave half a schedule behind for a caller to trip over.
    if (!ok) {
        CHECK(out.passes.empty());
        CHECK(out.targets.empty());
    }
    return !ok;
}

}  // namespace

// ---------------------------------------------------------------------------
// The ordinary shapes
// ---------------------------------------------------------------------------

TEST(pass_schedule_of_a_lone_image_pass_is_one_pass_and_one_target) {
    PassChain chain;
    chain.image_shader = "image";

    const PassSchedule schedule = schedule_of(chain);
    CHECK_EQ(schedule.passes.size(), static_cast<std::size_t>(1));
    CHECK_EQ(schedule.targets.size(), static_cast<std::size_t>(1));
    CHECK(schedule.passes.front().is_image);
    CHECK_EQ(schedule.passes.front().write_target, schedule.present_target);
    // Nothing to alternate, so both parities are the same schedule.
    CHECK(!schedule.double_buffered);
}

TEST(pass_schedule_runs_buffers_in_order_then_the_image) {
    PassChain chain;
    chain.buffers = {buffer("a"), buffer("b")};
    chain.image_shader = "image";

    const PassSchedule schedule = schedule_of(chain);
    CHECK_EQ(schedule.passes.size(), static_cast<std::size_t>(3));
    CHECK_STREQ(schedule.passes[0].shader_id, "a");
    CHECK_STREQ(schedule.passes[1].shader_id, "b");
    CHECK_STREQ(schedule.passes[2].shader_id, "image");
    CHECK(!schedule.passes[0].is_image);
    CHECK(schedule.passes[2].is_image);

    // Two buffers and the image, and nothing read backwards, so no copies.
    CHECK_EQ(schedule.targets.size(), static_cast<std::size_t>(3));
    CHECK(!schedule.double_buffered);
}

TEST(pass_schedule_reads_an_earlier_buffer_from_this_frame) {
    // b runs after a, so what b samples is what a just wrote - the same target,
    // not a copy, and no alternation needed for it.
    PassChain chain;
    chain.buffers = {buffer("a"), buffer("b")};
    chain.image_shader = "image";
    chain.inputs["b"] = {reads(0, "a")};

    const PassSchedule schedule = schedule_of(chain);
    const ScheduledPass* a = find(schedule, "a");
    const ScheduledPass* b = find(schedule, "b");
    CHECK(a != nullptr && b != nullptr);
    CHECK_EQ(b->read_targets.size(), static_cast<std::size_t>(1));
    CHECK_EQ(b->read_targets.at(0), a->write_target);
    CHECK(!schedule.double_buffered);
}

TEST(pass_schedule_gives_the_image_pass_its_inputs_too) {
    PassChain chain;
    chain.buffers = {buffer("a")};
    chain.image_shader = "image";
    chain.inputs["image"] = {reads(0, "a")};

    const PassSchedule schedule = schedule_of(chain);
    const ScheduledPass* image = find(schedule, "image");
    CHECK(image != nullptr);
    CHECK_EQ(image->read_targets.at(0), find(schedule, "a")->write_target);
}

// ---------------------------------------------------------------------------
// Reads that cross a frame boundary
// ---------------------------------------------------------------------------

TEST(pass_schedule_makes_a_self_read_a_previous_frame_read) {
    // The whole point of a buffer: it accumulates by sampling what it wrote
    // last time. That cannot be the target it is drawing into, so it gets two.
    PassChain chain;
    chain.buffers = {buffer("a")};
    chain.image_shader = "image";
    chain.inputs["a"] = {reads(0, "a")};

    const PassSchedule schedule = schedule_of(chain);
    CHECK(schedule.double_buffered);
    // Two copies of a, plus the image target.
    CHECK_EQ(schedule.targets.size(), static_cast<std::size_t>(3));

    const ScheduledPass* a = find(schedule, "a");
    CHECK(a != nullptr);
    CHECK(a->read_targets.at(0) != a->write_target);
}

TEST(pass_schedule_treats_reading_a_later_pass_as_the_previous_frame) {
    // a runs before b, so when a reads b there is nothing from this frame to
    // read. That is allowed - it is how a feedback loop between two buffers is
    // written - but it can only mean last frame's b.
    PassChain chain;
    chain.buffers = {buffer("a"), buffer("b")};
    chain.image_shader = "image";
    chain.inputs["a"] = {reads(0, "b")};

    const PassSchedule schedule = schedule_of(chain);
    CHECK(schedule.double_buffered);
    const ScheduledPass* a = find(schedule, "a");
    const ScheduledPass* b = find(schedule, "b");
    CHECK(a->read_targets.at(0) != b->write_target);
}

TEST(pass_schedule_only_doubles_the_buffers_that_need_it) {
    // a is read backwards and needs two targets; b is read forwards and does
    // not. A chain should not pay for copies nobody asked for.
    PassChain chain;
    chain.buffers = {buffer("a"), buffer("b")};
    chain.image_shader = "image";
    chain.inputs["a"] = {reads(0, "a")};
    chain.inputs["image"] = {reads(0, "b")};

    const PassSchedule schedule = schedule_of(chain);
    // Two for a, one for b, one for the image.
    CHECK_EQ(schedule.targets.size(), static_cast<std::size_t>(4));

    std::size_t copies_of_a = 0;
    std::size_t copies_of_b = 0;
    for (const TargetDesc& target : schedule.targets) {
        if (target.owner_pass == "a") ++copies_of_a;
        if (target.owner_pass == "b") ++copies_of_b;
    }
    CHECK_EQ(copies_of_a, static_cast<std::size_t>(2));
    CHECK_EQ(copies_of_b, static_cast<std::size_t>(1));
}

TEST(pass_schedule_swaps_only_the_alternating_targets_between_parities) {
    PassChain chain;
    chain.buffers = {buffer("a"), buffer("b")};
    chain.image_shader = "image";
    chain.inputs["a"] = {reads(0, "a")};
    chain.inputs["image"] = {reads(0, "b")};

    const PassSchedule even = schedule_of(chain, 0);
    const PassSchedule odd = schedule_of(chain, 1);

    // The same targets exist either way; only which of a's two copies is
    // written and read moves.
    CHECK_EQ(even.targets.size(), odd.targets.size());
    CHECK_EQ(even.present_target, odd.present_target);
    CHECK(find(even, "a")->write_target != find(odd, "a")->write_target);
    CHECK(find(even, "a")->read_targets.at(0) != find(odd, "a")->read_targets.at(0));
    // What a writes on one frame is what it reads on the next: that is the
    // whole of what alternating means.
    CHECK_EQ(find(even, "a")->write_target, find(odd, "a")->read_targets.at(0));
    CHECK_EQ(find(odd, "a")->write_target, find(even, "a")->read_targets.at(0));

    // b alternates nothing, so it does not move.
    CHECK_EQ(find(even, "b")->write_target, find(odd, "b")->write_target);
}

TEST(pass_schedule_never_reads_the_target_it_writes) {
    // The hazard the driver refuses and cannot explain. It should be
    // unreachable by construction; this asserts that across every shape here.
    const std::vector<PassChain> chains = [] {
        std::vector<PassChain> all;
        PassChain self;
        self.buffers = {buffer("a")};
        self.image_shader = "image";
        self.inputs["a"] = {reads(0, "a")};
        all.push_back(self);

        PassChain forward;
        forward.buffers = {buffer("a"), buffer("b")};
        forward.image_shader = "image";
        forward.inputs["a"] = {reads(0, "b"), reads(1, "a")};
        forward.inputs["b"] = {reads(0, "a")};
        all.push_back(forward);
        return all;
    }();

    for (const PassChain& chain : chains) {
        for (std::uint32_t parity = 0; parity < 2; ++parity) {
            const PassSchedule schedule = schedule_of(chain, parity);
            CHECK(!schedule.passes.empty());
            for (const ScheduledPass& pass : schedule.passes) {
                for (const auto& [slot, target] : pass.read_targets) {
                    (void)slot;
                    CHECK(target != pass.write_target);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Refusals
// ---------------------------------------------------------------------------

TEST(pass_schedule_refuses_a_pipeline_with_nothing_to_draw) {
    PassChain chain;
    chain.buffers = {buffer("a")};
    Diagnostics diags;
    CHECK(refuses(chain, diags));
    CHECK(has_errors(diags));
}

TEST(pass_schedule_refuses_a_pass_with_no_shader) {
    PassChain chain;
    chain.buffers = {buffer("")};
    chain.image_shader = "image";
    Diagnostics diags;
    CHECK(refuses(chain, diags));
}

TEST(pass_schedule_refuses_the_same_pass_twice) {
    // Two targets under one name, and every reference to it ambiguous.
    PassChain chain;
    chain.buffers = {buffer("a"), buffer("a")};
    chain.image_shader = "image";
    Diagnostics diags;
    CHECK(refuses(chain, diags));
    CHECK(diags.front().message.find("twice") != std::string::npos);
}

TEST(pass_schedule_refuses_a_shader_that_is_both_a_buffer_and_the_image) {
    PassChain chain;
    chain.buffers = {buffer("image")};
    chain.image_shader = "image";
    Diagnostics diags;
    CHECK(refuses(chain, diags));
}

TEST(pass_schedule_refuses_a_read_of_something_that_is_not_a_pass) {
    PassChain chain;
    chain.buffers = {buffer("a")};
    chain.image_shader = "image";
    chain.inputs["a"] = {reads(0, "nowhere")};
    Diagnostics diags;
    CHECK(refuses(chain, diags));
    CHECK(diags.front().message.find("nowhere") != std::string::npos);
}

TEST(pass_schedule_refuses_inputs_for_a_pass_that_is_not_in_the_chain) {
    PassChain chain;
    chain.buffers = {buffer("a")};
    chain.image_shader = "image";
    chain.inputs["ghost"] = {reads(0, "a")};
    Diagnostics diags;
    CHECK(refuses(chain, diags));
}

TEST(pass_schedule_refuses_reading_the_image_pass) {
    // The presented target is redrawn from nothing every frame and has no
    // previous copy, so there is nothing honest to bind. The message says what
    // would fix it.
    PassChain chain;
    chain.buffers = {buffer("a")};
    chain.image_shader = "image";
    chain.inputs["a"] = {reads(0, "image")};
    Diagnostics diags;
    CHECK(refuses(chain, diags));
    CHECK(diags.front().message.find("buffer pass") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Targets
// ---------------------------------------------------------------------------

TEST(pass_schedule_keeps_each_buffers_own_format) {
    PassChain chain;
    chain.buffers = {buffer("a", PassFormat::Rgba32Float), buffer("b", PassFormat::Rgba8Unorm)};
    chain.image_shader = "image";

    const PassSchedule schedule = schedule_of(chain);
    for (const TargetDesc& target : schedule.targets) {
        if (target.owner_pass == "a") CHECK(target.format == PassFormat::Rgba32Float);
        if (target.owner_pass == "b") CHECK(target.format == PassFormat::Rgba8Unorm);
    }
    // The presented target is shown rather than sampled and is redrawn every
    // frame, so it has neither the range problem nor the accumulation.
    CHECK(schedule.targets[schedule.present_target].format == PassFormat::Rgba8Unorm);
    CHECK(schedule.targets[schedule.present_target].owner_pass.empty());
}

TEST(pass_schedule_target_indices_are_all_in_range) {
    PassChain chain;
    chain.buffers = {buffer("a"), buffer("b"), buffer("c")};
    chain.image_shader = "image";
    chain.inputs["a"] = {reads(0, "a")};
    chain.inputs["b"] = {reads(0, "a"), reads(1, "c")};
    chain.inputs["image"] = {reads(0, "b"), reads(1, "c")};

    for (std::uint32_t parity = 0; parity < 2; ++parity) {
        const PassSchedule schedule = schedule_of(chain, parity);
        const std::uint32_t count = static_cast<std::uint32_t>(schedule.targets.size());
        CHECK(schedule.present_target < count);
        for (const ScheduledPass& pass : schedule.passes) {
            CHECK(pass.write_target < count);
            for (const auto& [slot, target] : pass.read_targets) {
                (void)slot;
                CHECK(target < count);
            }
        }
    }
}

TEST(pass_schedule_carries_clear_on_restart_to_the_target) {
    PassDesc keeps = buffer("a");
    keeps.clear_on_restart = false;
    PassChain chain;
    chain.buffers = {keeps};
    chain.image_shader = "image";

    const PassSchedule schedule = schedule_of(chain);
    for (const TargetDesc& target : schedule.targets) {
        if (target.owner_pass == "a") CHECK(!target.clear_on_restart);
    }
}

// ---------------------------------------------------------------------------
// A chain survives the manifest
// ---------------------------------------------------------------------------

TEST(pipeline_passes_round_trip_through_the_manifest) {
    const auto dir = std::filesystem::temp_directory_path() / "ssstudio-tests" / "pipeline_passes";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    Project project;
    project.name = "Chain";
    project.root = dir;
    project.manifest = dir / "project.toml";
    project.profiles.push_back(BuildProfile{});

    PreviewPipeline pipeline;
    pipeline.name = "chain";
    pipeline.vertex = "fullscreen_vert";
    pipeline.fragment = "image";
    PassDesc accumulator = buffer("buf_a", PassFormat::Rgba32Float);
    accumulator.clear_on_restart = false;
    pipeline.passes = {accumulator, buffer("buf_b", PassFormat::Rgba16Float)};
    project.pipelines.push_back(pipeline);

    Diagnostics diags;
    CHECK(save_project(project, diags));

    Project loaded;
    Diagnostics load_diags;
    CHECK(load_project(project.manifest, loaded, load_diags));
    CHECK_EQ(loaded.pipelines.size(), static_cast<std::size_t>(1));

    const PreviewPipeline& back = loaded.pipelines.front();
    CHECK_STREQ(back.fragment, "image");
    CHECK_EQ(back.passes.size(), static_cast<std::size_t>(2));
    CHECK_STREQ(back.passes[0].shader_id, "buf_a");
    CHECK(back.passes[0].format == PassFormat::Rgba32Float);
    CHECK(!back.passes[0].clear_on_restart);
    CHECK_STREQ(back.passes[1].shader_id, "buf_b");
    CHECK(back.passes[1].format == PassFormat::Rgba16Float);
    CHECK(back.passes[1].clear_on_restart);
}

TEST(pipeline_with_no_passes_stays_exactly_as_it_was) {
    // The ordinary case has to be untouched by all of this: a pipeline that
    // names a vertex and a fragment shader and nothing else must round-trip
    // without growing a passes table.
    const auto dir = std::filesystem::temp_directory_path() / "ssstudio-tests" / "pipeline_plain";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    Project project;
    project.name = "Plain";
    project.root = dir;
    project.manifest = dir / "project.toml";
    project.profiles.push_back(BuildProfile{});
    project.pipelines.push_back(PreviewPipeline{"plain", "vert", "frag", {}});

    Diagnostics diags;
    CHECK(save_project(project, diags));

    Project loaded;
    Diagnostics load_diags;
    CHECK(load_project(project.manifest, loaded, load_diags));
    CHECK_EQ(loaded.pipelines.size(), static_cast<std::size_t>(1));
    CHECK(loaded.pipelines.front().passes.empty());
}

TEST(pipeline_drops_a_pass_that_names_no_shader) {
    // A pass with nothing to run would only surface later as a target nobody
    // writes into, which is a much worse place to find out about it.
    const auto dir = std::filesystem::temp_directory_path() / "ssstudio-tests" / "pipeline_empty";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    std::ofstream manifest(dir / "project.toml");
    manifest << "[project]\nname = \"Empty\"\nformat_version = 3\n\n";
    manifest << "[[pipelines]]\nname = \"chain\"\nvertex = \"v\"\nfragment = \"f\"\n\n";
    manifest << "[[pipelines.passes]]\nshader = \"\"\n\n";
    manifest << "[[pipelines.passes]]\nshader = \"real\"\n";
    manifest.close();

    Project loaded;
    Diagnostics diags;
    CHECK(load_project(dir / "project.toml", loaded, diags));
    CHECK_EQ(loaded.pipelines.size(), static_cast<std::size_t>(1));
    CHECK_EQ(loaded.pipelines.front().passes.size(), static_cast<std::size_t>(1));
    CHECK_STREQ(loaded.pipelines.front().passes.front().shader_id, "real");
}
