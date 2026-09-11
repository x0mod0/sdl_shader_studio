// Fullscreen shader import: the scanner's judgement and the wrapper's output.
//
// The wrapper's contract is that the imported body survives byte for byte, so
// most of these tests are about what the generated text does *not* do to it.
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "ssstudio/compiler.h"
#include "ssstudio/shader_import.h"
#include "test.h"

using namespace ssstudio;

namespace {

/// A body in the shape the importer expects, with nothing unusual in it.
const char* kSimpleBody = R"(float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = (2.0 * fragCoord - iResolution.xy) / iResolution.y;
    float d = length(uv) - 0.5 + 0.05 * sin(iTime * 2.0);
    vec3 col = mix(vec3(0.1), vec3(1.0, 0.7, 0.3), smoothstep(0.01, -0.01, d));
    col *= 1.0 + 0.1 * hash(fragCoord + float(iFrame));
    fragColor = vec4(col, 1.0);
}
)";

/// The text between the body marker and the epilogue marker.
std::string extract_body(const std::string& wrapped) {
    const std::size_t start = imported_body_offset(wrapped);
    if (start == std::string::npos) return {};
    const std::size_t end = wrapped.find("// --- generated epilogue ---", start);
    if (end == std::string::npos) return {};
    return wrapped.substr(start, end - start);
}

std::string wrap_or_empty(const std::string& source, const ImportOptions& options = {}) {
    const ImportScan scan = scan_fullscreen_source(source);
    Diagnostics diags;
    const auto wrapped = wrap_fullscreen_source(source, scan, options, diags);
    return wrapped.value_or(std::string());
}

std::size_t count_occurrences(const std::string& haystack, const std::string& needle) {
    std::size_t count = 0;
    for (std::size_t at = haystack.find(needle); at != std::string::npos;
         at = haystack.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}

}  // namespace

// ---------------------------------------------------------------------------
// Scanner
// ---------------------------------------------------------------------------

TEST(import_scan_finds_each_entry_point) {
    CHECK(scan_fullscreen_source("void mainImage(out vec4 c, in vec2 p) {}").entry ==
          ImportEntry::Image);
    CHECK(scan_fullscreen_source("vec2 mainSound(int s, float t) { return vec2(0); }").entry ==
          ImportEntry::Sound);
    CHECK(scan_fullscreen_source("void mainVR(out vec4 c, in vec2 p, in vec3 o, in vec3 d) {}")
              .entry == ImportEntry::VR);
    CHECK(scan_fullscreen_source("void mainCubemap(out vec4 c, in vec3 o, in vec3 d) {}").entry ==
          ImportEntry::Cubemap);
    CHECK(scan_fullscreen_source("float f(vec2 p) { return 0.0; }").entry == ImportEntry::None);
}

TEST(import_scan_prefers_the_image_entry_point) {
    const std::string both =
        "vec2 mainSound(int s, float t) { return vec2(0); }\n"
        "void mainImage(out vec4 c, in vec2 p) {}\n";
    const ImportScan scan = scan_fullscreen_source(both);
    CHECK(scan.entry == ImportEntry::Image);
    CHECK_EQ(scan.all_entries.size(), static_cast<std::size_t>(2));
}

TEST(import_scan_ignores_entry_points_inside_comments) {
    CHECK(scan_fullscreen_source("// void mainImage(out vec4 c, in vec2 p) {}\n").entry ==
          ImportEntry::None);
    CHECK(scan_fullscreen_source("/* void mainImage(out vec4 c, in vec2 p) {} */\n").entry ==
          ImportEntry::None);
    CHECK(scan_fullscreen_source("/* a\n   void mainImage(out vec4 c, in vec2 p)\n*/\n").entry ==
          ImportEntry::None);
}

TEST(import_scan_survives_an_unterminated_block_comment) {
    // Runs to the end of the source instead of reading past it or looping.
    const ImportScan scan =
        scan_fullscreen_source("/* void mainImage(out vec4 c, in vec2 p) {}\n");
    CHECK(scan.entry == ImportEntry::None);
}

TEST(import_scan_requires_a_call_not_a_mention) {
    // The name on its own is not an entry point; it has to be used as one.
    CHECK(scan_fullscreen_source("float mainImage = 1.0;").entry == ImportEntry::None);
    // Whitespace between the name and the parenthesis is still a declaration.
    CHECK(scan_fullscreen_source("void mainImage\n(out vec4 c, in vec2 p) {}").entry ==
          ImportEntry::Image);
}

TEST(import_scan_matches_channels_as_whole_tokens) {
    ImportScan scan = scan_fullscreen_source(
        "void mainImage(out vec4 c, in vec2 p) { c = texture(iChannel0, p) + "
        "texture(iChannel2, p); }");
    CHECK(scan.channels_used[0]);
    CHECK(!scan.channels_used[1]);
    CHECK(scan.channels_used[2]);
    CHECK(!scan.channels_used[3]);

    // Neither a longer identifier that starts the same way nor a two-digit one
    // is channel zero.
    scan = scan_fullscreen_source(
        "void mainImage(out vec4 c, in vec2 p) { c = vec4(iChannel0Blurred + iChannel01); }");
    CHECK(!scan.channels_used[0]);
}

TEST(import_scan_detects_legacy_spellings_as_whole_tokens) {
    ImportScan scan = scan_fullscreen_source(
        "void mainImage(out vec4 c, in vec2 p) { c = texture2D(iChannel0, p) * iGlobalTime; }");
    CHECK(scan.uses_texture2d);
    CHECK(scan.uses_legacy_time);
    CHECK(!scan.uses_texturecube);

    scan = scan_fullscreen_source(
        "void mainImage(out vec4 c, in vec2 p) { float my_texture2Dx = 1.0; c = vec4(0); }");
    CHECK(!scan.uses_texture2d);
}

TEST(import_scan_captures_a_version_directive) {
    const ImportScan scan =
        scan_fullscreen_source("#version 300 es\nvoid mainImage(out vec4 c, in vec2 p) {}");
    CHECK(scan.version_directive.has_value());
    CHECK_EQ(*scan.version_directive, 300);
}

TEST(import_scan_keeps_a_leading_notice_and_ignores_ordinary_comments) {
    const ImportScan licensed = scan_fullscreen_source(
        "// Copyright 2026 Someone. Licensed under CC BY 4.0.\n"
        "// Second line of the notice.\n"
        "void mainImage(out vec4 c, in vec2 p) {}\n");
    CHECK(!licensed.leading_comment.empty());
    CHECK(licensed.leading_comment.find("Second line") != std::string::npos);

    const ImportScan plain = scan_fullscreen_source(
        "// a spinning torus, roughly\n"
        "void mainImage(out vec4 c, in vec2 p) {}\n");
    CHECK(plain.leading_comment.empty());
}

TEST(import_scan_normalises_windows_line_endings) {
    const ImportScan scan =
        scan_fullscreen_source("#version 300 es\r\nvoid mainImage(out vec4 c, in vec2 p) {}\r\n");
    CHECK(scan.entry == ImportEntry::Image);
    CHECK(scan.version_directive.has_value());
}

TEST(import_scan_reports_unsupported_entry_points_as_errors) {
    const ImportScan scan =
        scan_fullscreen_source("vec2 mainSound(int s, float t) { return vec2(0); }");
    CHECK(has_errors(scan.notes));
}

// ---------------------------------------------------------------------------
// Wrapper
// ---------------------------------------------------------------------------

TEST(import_wrap_refuses_what_it_cannot_run) {
    for (const char* source : {"vec2 mainSound(int s, float t) { return vec2(0); }",
                               "void mainVR(out vec4 c, in vec2 p, in vec3 o, in vec3 d) {}",
                               "void mainCubemap(out vec4 c, in vec3 o, in vec3 d) {}",
                               "float f() { return 0.0; }"}) {
        const ImportScan scan = scan_fullscreen_source(source);
        Diagnostics diags;
        const auto wrapped = wrap_fullscreen_source(source, scan, {}, diags);
        CHECK(!wrapped.has_value());
        CHECK(has_errors(diags));
    }
}

TEST(import_wrap_emits_exactly_one_version_directive) {
    const std::string wrapped = wrap_or_empty(kSimpleBody);
    CHECK(wrapped.rfind("#version 450", 0) == 0);
    CHECK_EQ(count_occurrences(wrapped, "#version"), static_cast<std::size_t>(1));
}

TEST(import_wrap_keeps_the_body_verbatim) {
    const std::string wrapped = wrap_or_empty(kSimpleBody);
    CHECK(extract_body(wrapped).find(kSimpleBody) != std::string::npos);
}

TEST(import_wrap_removes_a_version_from_the_body_but_keeps_precision) {
    const std::string source = std::string("#version 300 es\nprecision highp float;\n") + kSimpleBody;
    const std::string wrapped = wrap_or_empty(source);
    CHECK_EQ(count_occurrences(wrapped, "#version"), static_cast<std::size_t>(1));
    CHECK(wrapped.find("precision highp float;") != std::string::npos);
}

TEST(import_wrap_declares_the_whole_uniform_block_and_every_channel) {
    // A body that mentions none of them still gets all of them: pruning would
    // make the scanner decide whether the shader compiles.
    const std::string wrapped = wrap_or_empty("void mainImage(out vec4 c, in vec2 p) { c = vec4(1); }");
    for (const char* name : {"iResolution", "iTime", "iTimeDelta", "iFrameRate", "iFrame",
                             "iSampleRate", "iMouse", "iDate", "iChannelTime",
                             "iChannelResolution"}) {
        CHECK(wrapped.find(name) != std::string::npos);
    }
    for (int i = 0; i < 4; ++i) {
        CHECK(wrapped.find("uniform sampler2D iChannel" + std::to_string(i)) != std::string::npos);
    }
}

TEST(import_wrap_puts_resources_and_uniforms_in_the_sets_sdl_requires) {
    const std::string wrapped = wrap_or_empty(kSimpleBody);
    CHECK(wrapped.find("layout(set = 3, binding = 0) uniform Frame") != std::string::npos);
    CHECK(wrapped.find("layout(set = 2, binding = 0) uniform sampler2D iChannel0") !=
          std::string::npos);
}

TEST(import_wrap_honours_force_opaque) {
    ImportOptions options;
    options.force_opaque = true;
    CHECK(wrap_or_empty(kSimpleBody, options).find("vec4(color.rgb, 1.0)") != std::string::npos);

    options.force_opaque = false;
    const std::string transparent = wrap_or_empty(kSimpleBody, options);
    CHECK(transparent.find("ssstudio_frag_color = color;") != std::string::npos);
    CHECK(transparent.find("vec4(color.rgb, 1.0)") == std::string::npos);
}

TEST(import_wrap_can_omit_the_compatibility_defines) {
    ImportOptions options;
    options.compat_defines = false;
    CHECK(wrap_or_empty(kSimpleBody, options).find("#define texture2D") == std::string::npos);
    CHECK(wrap_or_empty(kSimpleBody).find("#define texture2D") != std::string::npos);
}

TEST(import_wrap_places_shared_code_ahead_of_the_body) {
    ImportOptions options;
    options.common = "#define TAU 6.28318530718\n";
    const std::string wrapped = wrap_or_empty(kSimpleBody, options);
    const std::size_t common_at = wrapped.find("#define TAU");
    const std::size_t body_at = imported_body_offset(wrapped);
    CHECK(common_at != std::string::npos);
    CHECK(body_at != std::string::npos);
    CHECK(common_at < body_at);
}

TEST(import_wrap_carries_attribution_into_the_file) {
    ImportOptions options;
    options.source_url = "https://example.invalid/s/abc";
    options.author = "Someone";
    options.licence = "CC BY 4.0";
    const std::string wrapped = wrap_or_empty(kSimpleBody, options);
    CHECK(wrapped.find("https://example.invalid/s/abc") != std::string::npos);
    CHECK(wrapped.find("Someone") != std::string::npos);
    CHECK(wrapped.find("CC BY 4.0") != std::string::npos);
}

TEST(import_wrap_preserves_a_notice_found_in_the_body) {
    const std::string source =
        std::string("// Copyright 2026 Someone, CC BY-SA.\n") + kSimpleBody;
    const std::string wrapped = wrap_or_empty(source);
    // Once in the generated header, and once where it still sits in the body.
    CHECK(count_occurrences(wrapped, "Copyright 2026 Someone") >= static_cast<std::size_t>(2));
}

TEST(import_body_offset_points_at_the_first_body_byte) {
    const std::string wrapped = wrap_or_empty(kSimpleBody);
    const std::size_t at = imported_body_offset(wrapped);
    CHECK(at != std::string::npos);
    CHECK(wrapped.compare(at, 5, "float") == 0);

    // Anything not produced by the wrapper has no marker at all.
    CHECK(imported_body_offset("#version 450\nvoid main() {}\n") == std::string::npos);
}

// ---------------------------------------------------------------------------
// The guarantee that matters: the result actually compiles.
// ---------------------------------------------------------------------------

TEST(import_wrap_produces_source_the_glsl_front_end_accepts) {
    auto backend = create_glslang_backend();
    if (!backend) return;  // built without the GLSL front end; nothing to prove

    CompileRequest req;
    req.id = "imported";
    req.source = wrap_or_empty(kSimpleBody);
    req.path = "imported.frag.glsl";
    req.stage = Stage::Fragment;
    req.language = Language::GLSL;
    req.formats = FORMAT_SPIRV;

    const CompileResult result = backend->compile(req);
    if (!result.ok) {
        std::string message = "wrapped source did not compile:";
        for (const auto& d : result.diagnostics) message += "\n  " + d.format();
        ::test::fail(__FILE__, __LINE__, message);
        return;
    }

    // Reflection has to see the block and the samplers, because that is what
    // drives the I/O panel, the docs and the pack.
    CHECK_EQ(result.reflection.uniform_blocks.size(), static_cast<std::size_t>(1));
    const UniformBlock& block = result.reflection.uniform_blocks.front();
    CHECK_STREQ(block.name, "Frame");
    CHECK_EQ(block.set, 3u);
    CHECK_EQ(block.binding, 0u);
    // vec3 + 5 scalars + 2 vec4 + float[4] + vec3[4], padded: 192 bytes.
    CHECK_EQ(block.size, 192u);
}

TEST(import_wrap_survives_a_body_using_every_convention_name) {
    auto backend = create_glslang_backend();
    if (!backend) return;

    const char* body = R"(void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = fragCoord / iResolution.xy;
    float t = iTime + iTimeDelta * iFrameRate + float(iFrame) + iSampleRate;
    t += iChannelTime[0] + iChannelResolution[1].x + iDate.w;
    vec4 c = texture2D(iChannel0, uv) + texture(iChannel3, uv) * iGlobalTime;
    if (iMouse.z > 0.0) c.r += 0.2;
    fragColor = c + t * 0.0;
}
)";

    CompileRequest req;
    req.id = "imported_full";
    req.source = wrap_or_empty(body);
    req.path = "imported_full.frag.glsl";
    req.stage = Stage::Fragment;
    req.language = Language::GLSL;
    req.formats = FORMAT_SPIRV;

    const CompileResult result = backend->compile(req);
    if (!result.ok) {
        std::string message = "wrapped source did not compile:";
        for (const auto& d : result.diagnostics) message += "\n  " + d.format();
        ::test::fail(__FILE__, __LINE__, message);
    }
}


// ---------------------------------------------------------------------------
// Landing an import in a project
// ---------------------------------------------------------------------------

namespace {

std::filesystem::path import_temp_dir(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() / "ssstudio-tests" / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

/// A project rooted at a scratch directory, with no shaders in it.
Project scratch_project(const char* name) {
    Project project;
    project.name = name;
    project.root = import_temp_dir(name);
    project.manifest = project.root / "project.toml";
    return project;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

}  // namespace

TEST(import_unique_id_sanitises_and_deduplicates) {
    Project project;
    CHECK_STREQ(unique_shader_id(project, "Sea Scape!"), "sea_scape");
    CHECK_STREQ(unique_shader_id(project, ""), "imported");
    CHECK_STREQ(unique_shader_id(project, "___"), "imported");
    // An id becomes an enumerator name, so it cannot start with a digit.
    CHECK_STREQ(unique_shader_id(project, "2d_noise"), "s2d_noise");

    ShaderDesc taken;
    taken.id = "seascape";
    project.shaders.push_back(taken);
    CHECK_STREQ(unique_shader_id(project, "seascape"), "seascape_2");
}

TEST(import_writes_the_shader_and_a_vertex_partner) {
    Project project = scratch_project("import_writes");

    ImportRequest request;
    request.source = kSimpleBody;
    request.id = "seascape";
    request.options.author = "A. Person";

    ImportResult result;
    Diagnostics diags;
    CHECK(write_import_files(project, request, result, diags));
    CHECK(!has_errors(diags));

    CHECK_STREQ(result.shader_id, "seascape");
    CHECK(std::filesystem::exists(project.absolute(result.shader_path)));
    CHECK(read_file(project.absolute(result.shader_path)).find(kSimpleBody) != std::string::npos);

    // The preview draws a pair, so a fullscreen vertex shader is written too.
    CHECK(!result.vertex_path.empty());
    CHECK(std::filesystem::exists(project.absolute(result.vertex_path)));

    CHECK_STREQ(result.provenance.author, "A. Person");
    CHECK(!result.provenance.imported.empty());
}

TEST(import_does_not_pair_with_an_unrelated_vertex_shader) {
    // An ordinary vertex shader expects vertex data the preview does not bind,
    // so it is no use to a shader that covers the screen. One gets written even
    // though the project is not short of vertex shaders.
    Project project = scratch_project("import_existing_vertex");
    ShaderDesc vertex;
    vertex.id = "sprite_vert";
    vertex.stage = Stage::Vertex;
    project.shaders.push_back(vertex);

    ImportRequest request;
    request.source = kSimpleBody;

    ImportResult result;
    Diagnostics diags;
    CHECK(write_import_files(project, request, result, diags));
    CHECK(!result.vertex_path.empty());
    CHECK_STREQ(result.vertex_id, "fullscreen_vert");
}

TEST(import_reuses_the_fullscreen_vertex_shader_a_previous_import_left) {
    Project project = scratch_project("import_reuse_vertex");
    ShaderDesc vertex;
    vertex.id = "fullscreen_vert";
    vertex.stage = Stage::Vertex;
    vertex.language = Language::GLSL;
    project.shaders.push_back(vertex);

    ImportRequest request;
    request.source = kSimpleBody;
    request.id = "second";

    ImportResult result;
    Diagnostics diags;
    CHECK(write_import_files(project, request, result, diags));
    // Selected, but nothing written: an empty path is how the caller knows.
    CHECK_STREQ(result.vertex_id, "fullscreen_vert");
    CHECK(result.vertex_path.empty());
}

TEST(import_refuses_to_overwrite_and_leaves_nothing_behind) {
    Project project = scratch_project("import_no_overwrite");

    ImportRequest request;
    request.source = kSimpleBody;
    request.id = "taken";

    ImportResult first;
    Diagnostics diags;
    CHECK(write_import_files(project, request, first, diags));

    // The id is free again as far as the project model knows - nothing was
    // registered - but the file is on disk, and that has to be enough to stop
    // a second import from replacing someone's edits.
    ImportResult second;
    Diagnostics second_diags;
    CHECK(!write_import_files(project, request, second, second_diags));
    CHECK(has_errors(second_diags));
}

TEST(import_does_not_half_write_when_the_vertex_shader_cannot_be_created) {
    Project project = scratch_project("import_rollback");
    // A directory where the vertex shader's file needs to go makes creating it
    // fail, which must take the fragment shader back out with it.
    std::error_code ec;
    std::filesystem::create_directories(
        project.root / "shaders" / "fullscreen_vert.vert.glsl", ec);

    ImportRequest request;
    request.source = kSimpleBody;
    request.id = "rolled_back";

    ImportResult result;
    Diagnostics diags;
    CHECK(!write_import_files(project, request, result, diags));
    CHECK(!std::filesystem::exists(project.root / "shaders" / "rolled_back.frag.glsl"));
}

TEST(import_provenance_round_trips_through_the_manifest) {
    Project project = scratch_project("import_provenance");
    project.profiles.push_back(BuildProfile{});

    Provenance origin;
    origin.url = "https://example.invalid/s/abc";
    origin.author = "A. Person";
    origin.licence = "CC BY 4.0";
    origin.imported = "2026-09-07";
    project.provenance["seascape"] = origin;

    Diagnostics diags;
    CHECK(save_project(project, diags));

    Project loaded;
    Diagnostics load_diags;
    CHECK(load_project(project.manifest, loaded, load_diags));
    CHECK_EQ(loaded.provenance.count("seascape"), static_cast<std::size_t>(1));
    CHECK_STREQ(loaded.provenance["seascape"].url, origin.url);
    CHECK_STREQ(loaded.provenance["seascape"].author, origin.author);
    CHECK_STREQ(loaded.provenance["seascape"].licence, origin.licence);
    CHECK_STREQ(loaded.provenance["seascape"].imported, origin.imported);
}

TEST(import_result_compiles_as_part_of_a_real_project) {
    auto backend = create_glslang_backend();
    if (!backend) return;

    Project project = scratch_project("import_compiles");
    ImportRequest request;
    request.source = kSimpleBody;
    request.id = "seascape";

    ImportResult result;
    Diagnostics diags;
    CHECK(write_import_files(project, request, result, diags));

    // Both halves of what the import produced have to compile, not just the
    // fragment shader: the generated vertex shader is what the preview pairs it
    // with, and nothing else in the test suite covers it.
    for (const auto& [path, stage] :
         {std::pair{result.shader_path, Stage::Fragment},
          std::pair{result.vertex_path, Stage::Vertex}}) {
        CompileRequest req;
        req.id = path.string();
        req.source = read_file(project.absolute(path));
        req.path = path;
        req.stage = stage;
        req.language = Language::GLSL;
        req.formats = FORMAT_SPIRV;

        const CompileResult compiled = backend->compile(req);
        if (!compiled.ok) {
            std::string message = path.string() + " did not compile:";
            for (const auto& d : compiled.diagnostics) message += "\n  " + d.format();
            ::test::fail(__FILE__, __LINE__, message);
        }
    }
}

TEST(a_project_opened_by_a_relative_path_still_knows_where_it_is) {
    Project project = scratch_project("relative_root");
    project.profiles.push_back(BuildProfile{});
    ShaderDesc shader;
    shader.id = "sprite_frag";
    shader.path = "shaders/sprite.frag.hlsl";
    project.shaders.push_back(shader);

    // The loader checks that a declared shader is really there, so it has to be.
    std::filesystem::create_directories(project.root / "shaders");
    std::ofstream(project.root / "shaders" / "sprite.frag.hlsl")
        << "float4 main() : SV_Target { return 1.0; }\n";

    Diagnostics diags;
    CHECK(save_project(project, diags));

    // Opened the way a command line opens one: a path relative to wherever the
    // process happens to be. Project::absolute() only prepends the root, so a
    // relative root would hand every shader a relative path - which reads fine
    // in the editor and is useless the moment it leaves the process, as a
    // file:// URL for the desktop or as something pasted into a terminal.
    const std::filesystem::path here = std::filesystem::current_path();
    std::filesystem::current_path(project.root.parent_path());
    Project loaded;
    Diagnostics load_diags;
    const std::filesystem::path relative = project.root.filename() / "project.toml";
    const bool ok = load_project(relative, loaded, load_diags);
    std::filesystem::current_path(here);

    CHECK(!relative.is_absolute());
    CHECK(ok);
    CHECK(loaded.root.is_absolute());
    CHECK(loaded.manifest.is_absolute());
    CHECK(!loaded.shaders.empty());
    CHECK(loaded.absolute(loaded.shaders.front().path).is_absolute());
}

// --- preview pipelines ---------------------------------------------------

namespace {

ShaderDesc shader_desc(const char* id, Stage stage) {
    ShaderDesc s;
    s.id = id;
    s.path = std::filesystem::path("shaders") / (std::string(id) + ".hlsl");
    s.stage = stage;
    return s;
}

}  // namespace

TEST(a_default_pipeline_appears_when_there_is_one_of_each_shader) {
    Project project;
    project.shaders = {shader_desc("sprite_vert", Stage::Vertex),
                       shader_desc("sprite_frag", Stage::Fragment)};

    CHECK(ensure_default_pipeline(project));
    CHECK(project.pipelines.size() == std::size_t{1});
    CHECK_STREQ(project.pipelines.front().name, "Default");
    CHECK_STREQ(project.pipelines.front().vertex, "sprite_vert");
    CHECK_STREQ(project.pipelines.front().fragment, "sprite_frag");
    CHECK_STREQ(project.preview.pipeline, "Default");
}

TEST(no_default_pipeline_is_guessed_when_the_pairing_is_not_obvious) {
    Project two_fragments;
    two_fragments.shaders = {shader_desc("v", Stage::Vertex), shader_desc("a", Stage::Fragment),
                             shader_desc("b", Stage::Fragment)};
    CHECK(!ensure_default_pipeline(two_fragments));
    CHECK(two_fragments.pipelines.empty());

    Project no_vertex;
    no_vertex.shaders = {shader_desc("a", Stage::Fragment)};
    CHECK(!ensure_default_pipeline(no_vertex));

    Project nothing;
    CHECK(!ensure_default_pipeline(nothing));
}

TEST(the_default_pipeline_never_comes_back_once_it_has_been_changed) {
    Project project;
    project.shaders = {shader_desc("sprite_vert", Stage::Vertex),
                       shader_desc("sprite_frag", Stage::Fragment)};
    CHECK(ensure_default_pipeline(project));

    // Renamed, which is exactly what the user is allowed to do to it.
    project.pipelines.front().name = "Main";
    project.preview.pipeline = "Main";
    CHECK(!ensure_default_pipeline(project));
    CHECK(project.pipelines.size() == std::size_t{1});
    CHECK_STREQ(project.pipelines.front().name, "Main");
}

TEST(the_active_pipeline_falls_back_to_the_first_rather_than_to_nothing) {
    Project project;
    project.pipelines = {{"One", "v", "a"}, {"Two", "v", "b"}};

    project.preview.pipeline = "Two";
    CHECK(project.active_pipeline() != nullptr);
    CHECK_STREQ(project.active_pipeline()->name, "Two");

    // A name that no longer resolves must not take the preview down with it.
    project.preview.pipeline = "Renamed away";
    CHECK_STREQ(project.active_pipeline()->name, "One");

    project.preview.pipeline.clear();
    CHECK_STREQ(project.active_pipeline()->name, "One");

    Project empty;
    CHECK(empty.active_pipeline() == nullptr);
}

TEST(a_new_pipeline_name_never_collides) {
    Project project;
    project.pipelines = {{"Default", "v", "f"}};
    CHECK_STREQ(unique_pipeline_name(project, "Default"), "Default 2");
    CHECK_STREQ(unique_pipeline_name(project, "Other"), "Other");

    project.pipelines.push_back({"Default 2", "v", "f"});
    CHECK_STREQ(unique_pipeline_name(project, "Default"), "Default 3");
    // An empty request still produces something choosable.
    CHECK(!unique_pipeline_name(project, "").empty());
}

TEST(pipelines_round_trip_through_the_manifest) {
    Project project = scratch_project("pipelines");
    project.profiles.push_back(BuildProfile{});
    project.shaders = {shader_desc("sprite_vert", Stage::Vertex),
                       shader_desc("sprite_frag", Stage::Fragment),
                       shader_desc("blur_frag", Stage::Fragment)};
    std::filesystem::create_directories(project.root / "shaders");
    for (const auto& shader : project.shaders) {
        std::ofstream(project.root / shader.path) << "// shader\n";
    }
    project.pipelines = {{"Default", "sprite_vert", "sprite_frag"},
                         {"Blur", "sprite_vert", "blur_frag"}};
    project.preview.pipeline = "Blur";

    Diagnostics diags;
    CHECK(save_project(project, diags));

    Project loaded;
    Diagnostics load_diags;
    CHECK(load_project(project.manifest, loaded, load_diags));
    CHECK(loaded.pipelines.size() == std::size_t{2});
    CHECK_STREQ(loaded.pipelines[0].name, "Default");
    CHECK_STREQ(loaded.pipelines[1].fragment, "blur_frag");
    CHECK_STREQ(loaded.preview.pipeline, "Blur");
    CHECK(loaded.active_pipeline() != nullptr);
    CHECK_STREQ(loaded.active_pipeline()->name, "Blur");
}

TEST(a_mesh_key_from_an_older_manifest_is_read_and_dropped) {
    // [preview].mesh was parsed and written back for as long as it existed and
    // obeyed by nothing: the preview drew the same geometry whatever it said.
    // That is worse than not having the field, because a project that sets it
    // watches the value survive a save and concludes the setting works. It is
    // gone now, and this pins both halves of its removal - a manifest that
    // still carries the key opens without complaint, and a save does not put
    // it back.
    const Project scratch = scratch_project("preview_mesh");
    std::filesystem::create_directories(scratch.root);
    std::ofstream(scratch.manifest) << R"([project]
name = "preview_mesh"
format_version = 3

[preview]
width = 640
height = 360
blend = true
mesh = "spinning_cube"
)";

    Project loaded;
    Diagnostics diags;
    CHECK(load_project(scratch.manifest, loaded, diags));
    for (const auto& d : diags) CHECK(d.severity != Severity::Error);
    // The keys either side of the one that went are still read.
    CHECK(loaded.preview.width == 640);
    CHECK(loaded.preview.height == 360);
    CHECK(loaded.preview.blend);

    Diagnostics save_diags;
    CHECK(save_project(loaded, save_diags));
    CHECK(read_file(loaded.manifest).find("mesh = ") == std::string::npos);
}

TEST(the_default_pipeline_survives_a_save_and_is_not_made_twice) {
    // The loop the preview bar performs on a project that has never had one:
    // create it, write it, and find it still there next time.
    Project project = scratch_project("pipeline_lifecycle");
    project.profiles.push_back(BuildProfile{});
    project.shaders = {shader_desc("sprite_vert", Stage::Vertex),
                       shader_desc("sprite_frag", Stage::Fragment)};
    std::filesystem::create_directories(project.root / "shaders");
    for (const auto& shader : project.shaders) {
        std::ofstream(project.root / shader.path) << "// shader\n";
    }

    Diagnostics diags;
    CHECK(save_project(project, diags));

    Project opened;
    Diagnostics open_diags;
    CHECK(load_project(project.manifest, opened, open_diags));
    CHECK(opened.pipelines.empty());

    CHECK(ensure_default_pipeline(opened));
    Diagnostics write_diags;
    CHECK(save_project(opened, write_diags));

    Project reopened;
    Diagnostics reopen_diags;
    CHECK(load_project(project.manifest, reopened, reopen_diags));
    CHECK(reopened.pipelines.size() == std::size_t{1});
    CHECK_STREQ(reopened.pipelines.front().name, "Default");
    CHECK_STREQ(reopened.active_pipeline()->vertex, "sprite_vert");
    // And opening it again does not add a second one.
    CHECK(!ensure_default_pipeline(reopened));
}
