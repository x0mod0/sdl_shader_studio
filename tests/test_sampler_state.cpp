// Sampler state as it survives the manifest.
//
// The map these read and write is free-form text in project.toml, editable by
// hand and possibly written by a version that knew more spellings than this one,
// so most of what matters here is what happens to values nobody planned for.
#include <map>
#include <string>

#include "ssstudio/project.h"
#include "ssstudio/sampler_state.h"
#include "test.h"

using namespace ssstudio;

namespace {

std::map<std::string, std::string> extra_of(const SamplerState& state) {
    std::map<std::string, std::string> extra;
    sampler_state_to_extra(state, extra);
    return extra;
}

}  // namespace

TEST(sampler_state_defaults_are_not_written_down) {
    // The manifest records what someone chose, not everything they left alone.
    const auto extra = extra_of(SamplerState{});
    CHECK(extra.empty());
}

TEST(sampler_state_round_trips_every_field) {
    SamplerState state;
    state.filter = SamplerState::Filter::Mipmap;
    state.wrap = SamplerState::Wrap::Repeat;
    state.vflip = true;
    state.srgb = true;

    const auto extra = extra_of(state);
    CHECK_EQ(extra.size(), static_cast<std::size_t>(4));
    CHECK(sampler_state_from_extra(extra) == state);

    // Every individual field on its own, too, since only differing ones are
    // written and a field that is alone in the map is the interesting case.
    for (int which = 0; which < 4; ++which) {
        SamplerState one;
        if (which == 0) one.filter = SamplerState::Filter::Nearest;
        if (which == 1) one.wrap = SamplerState::Wrap::Repeat;
        if (which == 2) one.vflip = true;
        if (which == 3) one.srgb = true;
        CHECK(sampler_state_from_extra(extra_of(one)) == one);
    }
}

TEST(sampler_state_writing_a_default_removes_the_key) {
    SamplerState state;
    state.wrap = SamplerState::Wrap::Repeat;
    auto extra = extra_of(state);
    CHECK_EQ(extra.count("wrap"), static_cast<std::size_t>(1));

    // Going back to the default takes the line out rather than writing
    // "wrap = clamp", so a file does not grow every time a row is touched.
    state.wrap = SamplerState::Wrap::Clamp;
    sampler_state_to_extra(state, extra);
    CHECK_EQ(extra.count("wrap"), static_cast<std::size_t>(0));
}

TEST(sampler_state_leaves_unrelated_keys_alone) {
    // `extra` is shared with whatever else a binding wants to remember, so
    // writing sampler state must not tidy away someone else's key.
    std::map<std::string, std::string> extra;
    extra["curve"] = "ease_in";
    sampler_state_to_extra(SamplerState{}, extra);
    CHECK_EQ(extra.count("curve"), static_cast<std::size_t>(1));
    CHECK_STREQ(extra["curve"], "ease_in");
}

TEST(sampler_state_falls_back_silently_on_anything_it_cannot_read) {
    const SamplerState defaults;
    std::map<std::string, std::string> extra = {
        {"filter", "trilinear"},  // not a spelling this version knows
        {"wrap", "MIRROR"},       // nor this
        {"vflip", "yes"},         // booleans are "true"/"false", not "yes"
        {"srgb", ""},
    };
    CHECK(sampler_state_from_extra(extra) == defaults);

    // An empty map is the same as a map full of nonsense: both mean "nobody
    // said", which is what makes a hand-edited project keep opening.
    CHECK(sampler_state_from_extra({}) == defaults);
}

TEST(sampler_state_reads_each_spelling) {
    const auto read = [](const char* key, const char* value) {
        return sampler_state_from_extra({{key, value}});
    };
    CHECK(read("filter", "nearest").filter == SamplerState::Filter::Nearest);
    CHECK(read("filter", "linear").filter == SamplerState::Filter::Linear);
    CHECK(read("filter", "mipmap").filter == SamplerState::Filter::Mipmap);
    CHECK(read("wrap", "clamp").wrap == SamplerState::Wrap::Clamp);
    CHECK(read("wrap", "repeat").wrap == SamplerState::Wrap::Repeat);
    CHECK(read("vflip", "true").vflip);
    CHECK(!read("vflip", "false").vflip);
    CHECK(read("srgb", "true").srgb);
    CHECK(!read("srgb", "false").srgb);
}

TEST(sampler_state_orders_so_a_cache_can_key_on_it) {
    // Six filter/wrap combinations share one sampler each, which only works if
    // states that differ compare as different and equal ones do not.
    SamplerState a;
    SamplerState b;
    CHECK(!(a < b) && !(b < a));
    b.wrap = SamplerState::Wrap::Repeat;
    CHECK((a < b) != (b < a));
}

TEST(sampler_state_survives_the_manifest_itself) {
    // The end to end version of the round trip: through a real project file,
    // where the values are text and the reader is the TOML one.
    const auto dir = std::filesystem::temp_directory_path() / "ssstudio-tests" / "sampler_manifest";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    Project project;
    project.name = "SamplerManifest";
    project.root = dir;
    project.manifest = dir / "project.toml";
    project.profiles.push_back(BuildProfile{});

    SamplerState state;
    state.filter = SamplerState::Filter::Mipmap;
    state.wrap = SamplerState::Wrap::Repeat;
    state.vflip = true;

    Binding binding;
    binding.source = BindingSource::File;
    binding.path = "assets/noise.png";
    sampler_state_to_extra(state, binding.extra);
    project.bindings["toy"].textures["iChannel0"] = binding;

    Diagnostics diags;
    CHECK(save_project(project, diags));

    Project loaded;
    Diagnostics load_diags;
    CHECK(load_project(project.manifest, loaded, load_diags));

    const Binding& back = loaded.bindings["toy"].textures["iChannel0"];
    CHECK(back.source == BindingSource::File);
    CHECK_STREQ(back.path.generic_string(), "assets/noise.png");
    CHECK(sampler_state_from_extra(back.extra) == state);
}
