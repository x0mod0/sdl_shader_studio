// The rule behind the editor's close-tab commands.
//
// Closing a tab never touches the project, so these are cheap to get wrong and
// expensive to notice: "close to the right" that is off by one takes a tab the
// user was working in, and looks like it worked.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "ssstudio/settings.h"
#include "tab_scope.h"
#include "test.h"

using namespace ssstudio::gui;

namespace {

const std::vector<std::string> kTabs = {"a", "b", "c", "d"};

}  // namespace

TEST(close_others_keeps_only_the_tab_it_was_asked_for) {
    const std::vector<std::string> got = tabs_in_scope(kTabs, "b", CloseScope::Others);
    const std::vector<std::string> want = {"a", "c", "d"};
    CHECK(got == want);
}

TEST(close_all_includes_the_tab_it_was_asked_for) {
    CHECK(tabs_in_scope(kTabs, "b", CloseScope::All) == kTabs);
    // And does not depend on the anchor at all.
    CHECK(tabs_in_scope(kTabs, "nothing", CloseScope::All) == kTabs);
}

TEST(close_left_and_right_are_exclusive_of_the_anchor) {
    const std::vector<std::string> left = {"a", "b"};
    const std::vector<std::string> right = {"d"};
    CHECK(tabs_in_scope(kTabs, "c", CloseScope::Left) == left);
    CHECK(tabs_in_scope(kTabs, "c", CloseScope::Right) == right);

    // Between them they account for every other tab exactly once, which is the
    // same set "close others" names.
    std::vector<std::string> both = tabs_in_scope(kTabs, "c", CloseScope::Left);
    for (const std::string& id : tabs_in_scope(kTabs, "c", CloseScope::Right)) {
        both.push_back(id);
    }
    CHECK(both == tabs_in_scope(kTabs, "c", CloseScope::Others));
}

TEST(the_ends_of_the_strip_have_nothing_beyond_them) {
    CHECK(tabs_in_scope(kTabs, "a", CloseScope::Left).empty());
    CHECK(tabs_in_scope(kTabs, "d", CloseScope::Right).empty());
    CHECK(tabs_in_scope(kTabs, "a", CloseScope::Right).size() == 3);
    CHECK(tabs_in_scope(kTabs, "d", CloseScope::Left).size() == 3);
}

// The order on screen is not the order the documents sit in once a tab has been
// dragged, and these commands follow the screen.
TEST(scope_follows_the_order_it_is_given) {
    const std::vector<std::string> dragged = {"c", "a", "d", "b"};
    const std::vector<std::string> left = {"c", "a"};
    CHECK(tabs_in_scope(dragged, "d", CloseScope::Left) == left);
    const std::vector<std::string> right = {"b"};
    CHECK(tabs_in_scope(dragged, "d", CloseScope::Right) == right);
}

TEST(an_anchor_that_is_not_on_screen_has_no_side) {
    CHECK(tabs_in_scope(kTabs, "gone", CloseScope::Left).empty());
    CHECK(tabs_in_scope(kTabs, "gone", CloseScope::Right).empty());
    // Others does not need to find it, so every tab is fair game.
    CHECK(tabs_in_scope(kTabs, "gone", CloseScope::Others) == kTabs);
}

TEST(a_single_tab_has_no_others_and_no_sides) {
    const std::vector<std::string> one = {"only"};
    CHECK(tabs_in_scope(one, "only", CloseScope::Others).empty());
    CHECK(tabs_in_scope(one, "only", CloseScope::Left).empty());
    CHECK(tabs_in_scope(one, "only", CloseScope::Right).empty());
    CHECK(tabs_in_scope(one, "only", CloseScope::All) == one);
}

// ---------------------------------------------------------------------------
// Persistence
//
// Which tabs were left closed is remembered per project in the settings file,
// so it survives a restart. It goes there rather than into project.toml because
// it is one user's view of the project, not part of the project: closing a tab
// must not show up as a change on a colleague's checkout.
// ---------------------------------------------------------------------------

namespace {

std::filesystem::path settings_file(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() / "ssstudio-tests" / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir / "settings.toml";
}

/// Writes `in` and reads it straight back, which is the only thing that proves
/// the two halves of the serializer agree with each other.
ssstudio::AppSettings round_trip(const ssstudio::AppSettings& in, const char* name) {
    const std::filesystem::path path = settings_file(name);
    ssstudio::Diagnostics diags;
    CHECK(ssstudio::save_settings(path, in, diags));
    ssstudio::AppSettings out;
    CHECK(ssstudio::load_settings(path, out, diags));
    return out;
}

}  // namespace

TEST(the_tab_arrangement_survives_a_round_trip_through_the_settings) {
    ssstudio::AppSettings in;
    ssstudio::ProjectEditorState one;
    one.tab_order = {"a_frag_hlsl", "b_vert_hlsl"};
    one.closed_tabs = {"b_vert_hlsl"};
    in.project_editor_state["/projects/one/project.toml"] = one;

    ssstudio::ProjectEditorState two;
    two.tab_order = {"c_comp_glsl"};
    in.project_editor_state["/projects/two/project.toml"] = two;

    const ssstudio::AppSettings out = round_trip(in, "settings-editor-state");
    CHECK(out.project_editor_state.size() == 2);
    CHECK(out.project_editor_state.at("/projects/one/project.toml").tab_order == one.tab_order);
    CHECK(out.project_editor_state.at("/projects/one/project.toml").closed_tabs == one.closed_tabs);
    CHECK(out.project_editor_state.at("/projects/two/project.toml").tab_order == two.tab_order);
    CHECK(out.project_editor_state.at("/projects/two/project.toml").closed_tabs.empty());
}

// The rest of the [session] block is written as scalars before this one, and an
// array-of-tables swallows every key that follows it. If the ordering were wrong
// these would come back as part of the wrong table, or not at all.
TEST(the_tab_arrangement_does_not_disturb_the_rest_of_the_session_block) {
    ssstudio::AppSettings in;
    in.open_projects = {"/projects/one/project.toml", "/projects/two/project.toml"};
    in.active_project = 1;
    in.recent_projects = {"/projects/one/project.toml"};
    in.project_editor_state["/projects/one/project.toml"].closed_tabs = {"a_frag_hlsl"};

    const ssstudio::AppSettings out = round_trip(in, "settings-session-block");
    CHECK(out.open_projects == in.open_projects);
    CHECK(out.active_project == 1);
    CHECK(out.recent_projects == in.recent_projects);
    CHECK(out.project_editor_state.at("/projects/one/project.toml").closed_tabs.size() == 1);
}

// A project left in its default state is a project with no entry: the record is
// of exceptions, so the common case costs nothing and the file stays readable.
TEST(a_project_left_untouched_is_not_recorded) {
    ssstudio::AppSettings in;
    in.project_editor_state["/projects/empty/project.toml"] = {};
    in.project_editor_state["/projects/real/project.toml"].closed_tabs = {"a_frag_hlsl"};

    const ssstudio::AppSettings out = round_trip(in, "settings-empty-entry");
    CHECK(out.project_editor_state.count("/projects/empty/project.toml") == 0);
    CHECK(out.project_editor_state.count("/projects/real/project.toml") == 1);
}

TEST(settings_with_no_editor_state_still_load) {
    ssstudio::AppSettings in;
    in.open_projects = {"/projects/one/project.toml"};

    const ssstudio::AppSettings out = round_trip(in, "settings-no-editor-state");
    CHECK(out.project_editor_state.empty());
    CHECK(out.open_projects == in.open_projects);
}

// ---------------------------------------------------------------------------
// Reconciling the remembered order with the project as it is now
// ---------------------------------------------------------------------------

TEST(a_remembered_order_is_used_as_it_stands) {
    const std::vector<std::string> remembered = {"c", "a", "b"};
    const std::vector<std::string> present = {"a", "b", "c"};
    CHECK(merge_tab_order(remembered, present) == remembered);
}

// The whole point of the merge: a project that gained a shader between sessions
// must not lose it, and must not have it appear somewhere arbitrary either.
TEST(shaders_added_since_go_after_the_remembered_ones_in_project_order) {
    const std::vector<std::string> remembered = {"c", "a"};
    const std::vector<std::string> present = {"a", "b", "c", "d"};
    const std::vector<std::string> want = {"c", "a", "b", "d"};
    CHECK(merge_tab_order(remembered, present) == want);
}

TEST(shaders_removed_since_drop_out_of_the_order) {
    const std::vector<std::string> remembered = {"c", "gone", "a"};
    const std::vector<std::string> present = {"a", "c"};
    const std::vector<std::string> want = {"c", "a"};
    CHECK(merge_tab_order(remembered, present) == want);
}

TEST(nothing_remembered_leaves_the_project_order_alone) {
    const std::vector<std::string> present = {"a", "b", "c"};
    CHECK(merge_tab_order({}, present) == present);
}

// A hand-edited settings file can say anything; the result still has to be every
// shader exactly once, or a tab would be drawn twice or not at all.
TEST(a_repeated_id_in_the_record_is_not_shown_twice) {
    const std::vector<std::string> remembered = {"b", "b", "a", "b"};
    const std::vector<std::string> present = {"a", "b", "c"};
    const std::vector<std::string> want = {"b", "a", "c"};
    CHECK(merge_tab_order(remembered, present) == want);
}

TEST(the_merge_is_a_permutation_of_what_the_project_has) {
    const std::vector<std::string> present = {"a", "b", "c", "d"};
    for (const std::vector<std::string>& remembered :
         {std::vector<std::string>{}, std::vector<std::string>{"d"},
          std::vector<std::string>{"d", "c", "b", "a"},
          std::vector<std::string>{"x", "d", "x"}}) {
        std::vector<std::string> merged = merge_tab_order(remembered, present);
        CHECK(merged.size() == present.size());
        std::sort(merged.begin(), merged.end());
        CHECK(merged == present);  // already sorted
    }
}

TEST(recent_projects_survive_a_round_trip) {
    ssstudio::AppSettings in;
    in.recent_projects = {"/projects/one/project.toml", "/projects/two/project.toml"};

    const ssstudio::AppSettings out = round_trip(in, "settings-recent-projects");
    CHECK(out.recent_projects == in.recent_projects);
}

// And a file written before the key moved still gives its list up, so nobody
// loses the one they already have on disk.
TEST(recent_projects_are_recovered_from_where_they_used_to_be_written) {
    const std::filesystem::path path = settings_file("settings-recent-legacy");
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "[pack_layout]\n"
             << "alignment = 16\n"
             << "recent_projects = [\"/projects/one/project.toml\"]\n";
    }
    ssstudio::AppSettings out;
    ssstudio::Diagnostics diags;
    CHECK(ssstudio::load_settings(path, out, diags));
    CHECK(out.recent_projects.size() == 1);
    CHECK(out.recent_projects.front() == std::filesystem::path("/projects/one/project.toml"));
}
