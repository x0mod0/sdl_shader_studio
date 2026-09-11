#include <filesystem>
#include <fstream>

#include "ssstudio/cache.h"
#include "ssstudio/diff.h"
#include "ssstudio/process.h"
#include "ssstudio/templates.h"
#include "test.h"

using namespace ssstudio;

namespace {

std::filesystem::path temp_dir(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() / "ssstudio-tests" / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

CompileResult sample_result(std::size_t blob_size = 512) {
    CompileResult r;
    r.ok = true;
    r.reflection.stage = Stage::Fragment;
    r.reflection.entry_point = "main";
    r.blobs[FORMAT_SPIRV] = std::vector<std::uint8_t>(blob_size, 0xAB);

    Diagnostic d;
    d.severity = Severity::Warning;
    d.message = "implicit truncation";
    d.line = 12;
    r.diagnostics.push_back(std::move(d));
    return r;
}

VertexInput varying(std::uint32_t location, std::uint32_t components,
                    ScalarType type = ScalarType::Float) {
    VertexInput v;
    v.name = "v" + std::to_string(location);
    v.location = location;
    v.components = components;
    v.type = type;
    return v;
}

}  // namespace

// ---------------------------------------------------------------------------
// Cache
// ---------------------------------------------------------------------------
TEST(cache_round_trips_a_result) {
    CompileCache cache(temp_dir("cache_roundtrip"));
    CHECK(cache.enabled());

    const CompileResult stored = sample_result();
    CHECK(cache.store(42, stored));

    auto hit = cache.lookup(42);
    CHECK(hit.has_value());
    if (!hit) return;
    CHECK(hit->ok);
    CHECK(hit->from_cache);
    CHECK(hit->blobs.at(FORMAT_SPIRV) == stored.blobs.at(FORMAT_SPIRV));
    CHECK_EQ(hit->diagnostics.size(), std::size_t{1});
    // Warnings must survive a cache hit, or they would only appear on a cold build.
    if (!hit->diagnostics.empty()) CHECK_STREQ(hit->diagnostics[0].message, "implicit truncation");
    CHECK_STREQ(hit->reflection.entry_point, "main");
}

TEST(cache_misses_on_an_unknown_key) {
    CompileCache cache(temp_dir("cache_miss"));
    CHECK(!cache.lookup(999).has_value());
    CHECK_EQ(cache.stats().misses, std::uint64_t{1});
}

TEST(cache_never_stores_failures) {
    CompileCache cache(temp_dir("cache_failures"));
    CompileResult failed;
    failed.ok = false;
    Diagnostic d;
    d.severity = Severity::Error;
    d.message = "syntax error";
    failed.diagnostics.push_back(d);

    CHECK(!cache.store(7, failed));
    CHECK(!cache.lookup(7).has_value());
}

TEST(cache_drops_corrupt_entries_instead_of_returning_them) {
    const auto dir = temp_dir("cache_corrupt");
    CompileCache cache(dir);
    CHECK(cache.store(1234, sample_result()));

    // Corrupt every entry file on disk.
    for (auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
        if (entry.path().extension() != ".sdsc") continue;
        std::ofstream f(entry.path(), std::ios::binary | std::ios::trunc);
        f << "garbage";
    }
    CHECK(!cache.lookup(1234).has_value());
    CHECK(!cache.lookup(1234).has_value());  // the bad entry was removed, not retried forever
}

TEST(cache_evicts_to_fit_its_budget) {
    const auto dir = temp_dir("cache_budget");
    CompileCache cache(dir, 4096);
    for (std::uint64_t key = 0; key < 20; ++key) {
        CHECK(cache.store(key, sample_result(1024)));
    }
    cache.trim();

    const CacheStats stats = cache.stats();
    CHECK(stats.bytes <= 4096);
    CHECK(stats.evictions > 0);
}

TEST(cache_is_a_no_op_when_disabled) {
    CompileCache cache;
    CHECK(!cache.enabled());
    CHECK(!cache.store(1, sample_result()));
    CHECK(!cache.lookup(1).has_value());
}

TEST(cache_clear_empties_it) {
    const auto dir = temp_dir("cache_clear");
    CompileCache cache(dir);
    cache.store(5, sample_result());
    cache.clear();
    CHECK(!cache.lookup(5).has_value());
}

// ---------------------------------------------------------------------------
// Diff
// ---------------------------------------------------------------------------
TEST(diff_reports_no_change_for_identical_text) {
    const auto lines = diff_lines("a\nb\nc\n", "a\nb\nc\n");
    CHECK(diff_stats(lines).identical());
    CHECK(format_unified(lines, "old", "new").empty());
}

TEST(diff_finds_a_single_edited_line) {
    const auto lines = diff_lines("float a = 1.0;\nreturn a;\n", "float a = 2.0;\nreturn a;\n");
    const DiffStats stats = diff_stats(lines);
    CHECK_EQ(stats.added, std::size_t{1});
    CHECK_EQ(stats.removed, std::size_t{1});
}

TEST(diff_handles_pure_insertion_and_removal) {
    const auto added = diff_lines("a\n", "a\nb\n");
    CHECK_EQ(diff_stats(added).added, std::size_t{1});
    CHECK_EQ(diff_stats(added).removed, std::size_t{0});

    const auto removed = diff_lines("a\nb\n", "a\n");
    CHECK_EQ(diff_stats(removed).removed, std::size_t{1});
    CHECK_EQ(diff_stats(removed).added, std::size_t{0});
}

TEST(diff_keeps_line_numbers_on_both_sides) {
    const auto lines = diff_lines("a\nb\n", "a\nx\nb\n");
    for (const auto& line : lines) {
        if (line.op == DiffOp::Keep) {
            CHECK(line.old_line > 0);
            CHECK(line.new_line > 0);
        }
        if (line.op == DiffOp::Insert) CHECK_EQ(line.old_line, std::size_t{0});
        if (line.op == DiffOp::Remove) CHECK_EQ(line.new_line, std::size_t{0});
    }
}

TEST(unified_output_marks_changes_and_collapses_context) {
    std::string before;
    for (int i = 0; i < 40; ++i) before += "line " + std::to_string(i) + "\n";
    std::string after = before;
    after.replace(after.find("line 20"), 7, "CHANGED");

    const std::string text = format_unified(diff_lines(before, after), "a.hlsl", "b.hlsl", 2);
    CHECK(text.find("--- a.hlsl") != std::string::npos);
    CHECK(text.find("+CHANGED") != std::string::npos);
    CHECK(text.find("-line 20") != std::string::npos);
    CHECK(text.find("@@") != std::string::npos);       // context collapsed
    CHECK(text.find("line 5") == std::string::npos);   // far context dropped
}

// ---------------------------------------------------------------------------
// Templates and cross-stage validation
// ---------------------------------------------------------------------------
TEST(templates_use_the_right_spaces_per_stage_and_language) {
    const std::string hlsl_vertex = default_source(Stage::Vertex, Language::HLSL);
    CHECK(hlsl_vertex.find("register(b0, space1)") != std::string::npos);

    const std::string hlsl_fragment = default_source(Stage::Fragment, Language::HLSL);
    CHECK(hlsl_fragment.find("register(t0, space2)") != std::string::npos);
    CHECK(hlsl_fragment.find("register(b0, space3)") != std::string::npos);

    const std::string glsl_fragment = default_source(Stage::Fragment, Language::GLSL);
    CHECK(glsl_fragment.find("#version 450") != std::string::npos);
    CHECK(glsl_fragment.find("set = 2") != std::string::npos);
    CHECK(glsl_fragment.find("set = 3") != std::string::npos);

    const std::string glsl_compute = default_source(Stage::Compute, Language::GLSL);
    CHECK(glsl_compute.find("set = 0") != std::string::npos);  // read-only
    CHECK(glsl_compute.find("set = 1") != std::string::npos);  // read-write
    CHECK(glsl_compute.find("local_size_x") != std::string::npos);
}

// One name has to be able to serve every stage and language. Shader ids are
// unique across a project because they become enumerators in the generated
// header, but files are not: test.vert.hlsl, test.frag.hlsl and test.frag.glsl
// are three different files, and refusing the second because "test" was taken is
// the bug this pair of functions exists to prevent.
TEST(one_name_yields_a_distinct_id_per_stage_and_language) {
    CHECK_STREQ(shader_id_for("test", Stage::Vertex, Language::HLSL), "test_vert_hlsl");
    CHECK_STREQ(shader_id_for("test", Stage::Fragment, Language::HLSL), "test_frag_hlsl");
    CHECK_STREQ(shader_id_for("test", Stage::Compute, Language::HLSL), "test_comp_hlsl");
    CHECK_STREQ(shader_id_for("test", Stage::Fragment, Language::GLSL), "test_frag_glsl");

    // Every one of the four differs, so all four can live in one project.
    const std::string ids[] = {
        shader_id_for("test", Stage::Vertex, Language::HLSL),
        shader_id_for("test", Stage::Fragment, Language::HLSL),
        shader_id_for("test", Stage::Compute, Language::HLSL),
        shader_id_for("test", Stage::Fragment, Language::GLSL),
    };
    for (std::size_t a = 0; a < 4; ++a) {
        for (std::size_t b = a + 1; b < 4; ++b) CHECK(ids[a] != ids[b]);
    }
}

// The id mirrors the extension exactly: test_frag_hlsl goes with test.frag.hlsl,
// so either can be read off the other.
TEST(the_id_suffix_mirrors_the_file_extension) {
    for (const Stage stage : {Stage::Vertex, Stage::Fragment, Stage::Compute}) {
        for (const Language language : {Language::HLSL, Language::GLSL}) {
            std::string extension = default_extension(stage, language);
            for (char& c : extension) {
                if (c == '.') c = '_';
            }
            CHECK_STREQ(id_suffix(stage, language), extension);
        }
    }
}

TEST(a_name_that_already_names_its_stage_is_not_suffixed_twice) {
    CHECK_STREQ(shader_id_for("test_frag_hlsl", Stage::Fragment, Language::HLSL),
                "test_frag_hlsl");
    // Another stage's or another language's suffix is not this one's, so it
    // still gets its own.
    CHECK_STREQ(shader_id_for("test_frag_hlsl", Stage::Vertex, Language::HLSL),
                "test_frag_hlsl_vert_hlsl");
    CHECK_STREQ(shader_id_for("test_frag_glsl", Stage::Fragment, Language::HLSL),
                "test_frag_glsl_frag_hlsl");
    // An empty name stays empty rather than becoming a bare suffix; the caller
    // is the one that reports it.
    CHECK_STREQ(shader_id_for("", Stage::Fragment, Language::HLSL), "");
}

TEST(basename_is_the_inverse_of_the_id_suffix) {
    CHECK_STREQ(shader_basename("test_frag_hlsl", Stage::Fragment, Language::HLSL), "test");
    CHECK_STREQ(shader_basename("test_frag_glsl", Stage::Fragment, Language::GLSL), "test");
    // Not this stage's suffix, so nothing comes off.
    CHECK_STREQ(shader_basename("test_frag_hlsl", Stage::Vertex, Language::HLSL),
                "test_frag_hlsl");
    // An id that is nothing but a suffix keeps it: stripping would leave no
    // filename at all.
    CHECK_STREQ(shader_basename("_frag_hlsl", Stage::Fragment, Language::HLSL), "_frag_hlsl");
}

// scaffold_project() writes ids that carry the stage alone, and so did every
// project made before the language joined the convention. Those still have to
// yield their name, or renaming one would not find its own file.
TEST(basename_still_reads_ids_that_carry_the_stage_alone) {
    CHECK_STREQ(shader_basename("sprite_vert", Stage::Vertex, Language::HLSL), "sprite");
    CHECK_STREQ(shader_basename("plasma_frag", Stage::Fragment, Language::HLSL), "plasma");
    CHECK_STREQ(shader_basename("plasma_frag", Stage::Fragment, Language::GLSL), "plasma");
    CHECK_STREQ(shader_basename("_vert", Stage::Vertex, Language::HLSL), "_vert");
}

// The round trip is what makes the pair usable as a naming convention: a name
// becomes an id, and the id names a file that leads back to the same name.
TEST(name_to_id_to_filename_round_trips) {
    for (const Stage stage : {Stage::Vertex, Stage::Fragment, Stage::Compute}) {
        for (const Language language : {Language::HLSL, Language::GLSL}) {
            const std::string id = shader_id_for("plasma", stage, language);
            CHECK_STREQ(shader_basename(id, stage, language), "plasma");
            const std::string file =
                shader_basename(id, stage, language) + default_extension(stage, language);
            CHECK(file.rfind("plasma.", 0) == 0);
        }
    }
    // And the filenames differ wherever the ids do.
    const std::string vertex = shader_basename(shader_id_for("plasma", Stage::Vertex,
                                                             Language::HLSL),
                                               Stage::Vertex, Language::HLSL) +
                               default_extension(Stage::Vertex, Language::HLSL);
    const std::string fragment = shader_basename(shader_id_for("plasma", Stage::Fragment,
                                                               Language::HLSL),
                                                 Stage::Fragment, Language::HLSL) +
                                 default_extension(Stage::Fragment, Language::HLSL);
    CHECK(vertex != fragment);
}

TEST(template_extensions_match_the_language) {
    CHECK_STREQ(default_extension(Stage::Vertex, Language::HLSL), ".vert.hlsl");
    CHECK_STREQ(default_extension(Stage::Compute, Language::GLSL), ".comp.glsl");
}

TEST(matching_varyings_validate_cleanly) {
    Reflection vertex;
    vertex.stage = Stage::Vertex;
    vertex.outputs_as_varyings = {varying(0, 2), varying(1, 4)};

    Reflection fragment;
    fragment.stage = Stage::Fragment;
    fragment.inputs_as_varyings = {varying(0, 2), varying(1, 4)};

    CHECK(!has_errors(validate_varyings(vertex, fragment, "pair")));
}

TEST(a_varying_the_vertex_stage_never_writes_is_an_error) {
    Reflection vertex;
    vertex.stage = Stage::Vertex;
    vertex.outputs_as_varyings = {varying(0, 2)};

    Reflection fragment;
    fragment.stage = Stage::Fragment;
    fragment.inputs_as_varyings = {varying(0, 2), varying(3, 4)};

    CHECK(has_errors(validate_varyings(vertex, fragment, "pair")));
}

TEST(varying_component_and_type_mismatches_are_errors) {
    Reflection vertex;
    vertex.stage = Stage::Vertex;
    vertex.outputs_as_varyings = {varying(0, 2)};

    Reflection wide;
    wide.stage = Stage::Fragment;
    wide.inputs_as_varyings = {varying(0, 4)};
    CHECK(has_errors(validate_varyings(vertex, wide, "pair")));

    Reflection retyped;
    retyped.stage = Stage::Fragment;
    retyped.inputs_as_varyings = {varying(0, 2, ScalarType::Int)};
    CHECK(has_errors(validate_varyings(vertex, retyped, "pair")));
}

TEST(an_unread_varying_is_only_a_warning) {
    Reflection vertex;
    vertex.stage = Stage::Vertex;
    vertex.outputs_as_varyings = {varying(0, 2), varying(1, 4)};

    Reflection fragment;
    fragment.stage = Stage::Fragment;
    fragment.inputs_as_varyings = {varying(0, 2)};

    const Diagnostics d = validate_varyings(vertex, fragment, "pair");
    CHECK(!has_errors(d));
    CHECK(!d.empty());
}

// --- finding the tools that ship with the application --------------------

TEST(the_executable_directory_is_the_one_this_test_runs_from) {
    const std::filesystem::path dir = executable_directory();
    CHECK(!dir.empty());
    std::error_code ec;
    CHECK(std::filesystem::is_directory(dir, ec));

    // The test binary is in it, which is the only way to check the answer is
    // this program's own directory rather than merely a plausible one.
    bool found = false;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().filename().string().rfind("ssstudio_tests", 0) == 0) found = true;
    }
    CHECK(found);
}

TEST(bundled_tool_roots_only_names_directories_that_are_there) {
    // Whatever it returns has to exist: the caller uses each entry as a place to
    // look, and a path that is not there would only push the real search later.
    for (const auto& root : bundled_tool_roots("shadercross")) {
        std::error_code ec;
        CHECK(std::filesystem::is_directory(root, ec));
    }
    // A tool nothing ships has nowhere to be found.
    CHECK(bundled_tool_roots("no-such-tool-ships-with-this").empty());
}
