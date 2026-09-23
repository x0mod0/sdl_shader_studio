// ssstudio - command line front end.
//
// Uses exactly the same core as the GUI, so a pack built in CI is byte-identical
// to one built from the Build panel.
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include "ssstudio/build.h"
#include "ssstudio/keys.h"
#include "ssstudio/packer.h"
#include "ssstudio/project.h"
#include "ssstudio/settings.h"
#include "ssstudio/shader_import.h"
#include "ssstudio/spirv_reflect.h"
#include "ssstudio/theme_pack.h"

namespace fs = std::filesystem;
using namespace ssstudio;

namespace {

// The shadercross tool's location is a user setting, so a build from the command
// line reaches for the same toolchain the app does rather than only whatever the
// build was configured against.
fs::path configured_tool_dir() {
    AppSettings settings;
    Diagnostics diags;
    load_settings(AppSettings::default_path(), settings, diags);
    return settings.tools.shadercross_dir;
}

int print_diagnostics(const Diagnostics& diags) {
    int errors = 0;
    for (const auto& d : diags) {
        std::ostream& os = d.severity == Severity::Error ? std::cerr : std::cout;
        os << d.format() << "\n";
        if (d.severity == Severity::Error) ++errors;
    }
    return errors;
}

void usage() {
    std::cout <<
        R"(ssstudio - SDL Shader Studio command line

usage:
  ssstudio new <dir> [--name NAME]           create a project skeleton
  ssstudio import <project> <file|->         import a fullscreen shader, or a
                                             whole chain from a .json description
  ssstudio check <project> [--profile P]     front-end pass, diagnostics only
  ssstudio build <project> [options]         compile, pack and emit artifacts
  ssstudio pack <project> [options]          build without writing helper files
  ssstudio reflect <shader.spv> [--stage S]  dump reflection for a SPIR-V blob
  ssstudio inspect <pack file>               describe an existing pack
  ssstudio keys <project> [--profile P]      show the key each shader will get
  ssstudio settings [--path]                 print settings location / contents
  ssstudio theme <pack> [--resolve --lint]   check a theme pack

options:
  --profile NAME       build profile to use (default: the first one)
  --out DIR            override the profile's output directory
  --dry-run            compile and pack in memory, write nothing
  --no-verify          skip the pack round-trip check
  --no-pin             do not write newly assigned keys back to the manifest
  --quiet              only print errors

import options:
  --id NAME            shader id to use (default: the file's stem)
  --common FILE        shared code to prepend ahead of the imported body
  --url URL            where the shader came from, kept for attribution
  --author NAME        who wrote it
  --licence TEXT       the terms it is offered under
  --keep-alpha         do not force the result opaque
)";
}

struct Args {
    std::string command;
    std::string target;
    std::string profile;
    std::string out;
    std::string name;
    std::string stage = "fragment";
    bool dry_run = false;
    bool verify = true;
    bool pin = true;
    bool quiet = false;
    bool path_only = false;
    /// Second positional argument, used by `import` for the source file.
    std::string source;
    std::string id;
    std::string common;
    std::string url;
    std::string author;
    std::string licence;
    bool keep_alpha = false;
    bool lint = false;
    bool resolve = false;
};

/// What parse_args() made of the command line. Asking for help and getting the
/// arguments wrong both end in the usage text, but only one of them is a
/// failure: a CI step with a misspelled option has to stop, not report success.
enum class ParseResult {
    /// The arguments are usable; run the command.
    Ok,
    /// `-h` or `--help` was asked for. Print the usage and exit 0.
    Help,
    /// No command, or an option nobody knows. Print the usage and exit 2, the
    /// same status an unknown command gets.
    Invalid,
};

/// Both spellings of the help flag, accepted in place of a command or after one.
bool is_help_flag(const std::string& a) { return a == "-h" || a == "--help"; }

ParseResult parse_args(int argc, char** argv, Args& args) {
    if (argc < 2) return ParseResult::Invalid;
    args.command = argv[1];
    // `ssstudio --help` names no command; without this it would be read as one
    // and rejected as unknown.
    if (is_help_flag(args.command)) return ParseResult::Help;
    int i = 2;
    if (i < argc && argv[i][0] != '-') args.target = argv[i++];
    // `import` takes a second positional: the file to read, or "-" for stdin.
    if (i < argc && (argv[i][0] != '-' || std::string(argv[i]) == "-")) args.source = argv[i++];
    for (; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            return (i + 1 < argc) ? std::string(argv[++i]) : std::string();
        };
        if (a == "--profile") args.profile = next();
        else if (a == "--out") args.out = next();
        else if (a == "--name") args.name = next();
        else if (a == "--stage") args.stage = next();
        else if (a == "--dry-run") args.dry_run = true;
        else if (a == "--no-verify") args.verify = false;
        else if (a == "--no-pin") args.pin = false;
        else if (a == "--quiet") args.quiet = true;
        else if (a == "--path") args.path_only = true;
        else if (a == "--lint") args.lint = true;
        else if (a == "--resolve") args.resolve = true;
        else if (a == "--id") args.id = next();
        else if (a == "--common") args.common = next();
        else if (a == "--url") args.url = next();
        else if (a == "--author") args.author = next();
        else if (a == "--licence" || a == "--license") args.licence = next();
        else if (a == "--keep-alpha") args.keep_alpha = true;
        else if (is_help_flag(a)) return ParseResult::Help;
        else {
            std::cerr << "unknown option: " << a << "\n";
            return ParseResult::Invalid;
        }
    }
    return ParseResult::Ok;
}

int cmd_new(const Args& args) {
    if (args.target.empty()) {
        std::cerr << "ssstudio new needs a directory\n";
        return 2;
    }
    const fs::path root = args.target;
    const std::string name = args.name.empty() ? root.filename().string() : args.name;

    Project project;
    Diagnostics diags;
    if (!scaffold_project(root, name, Language::HLSL, project, diags)) {
        print_diagnostics(diags);
        return 1;
    }
    print_diagnostics(diags);

    std::cout << "created " << project.manifest.string() << "\n";
    for (const auto& shader : project.shaders) {
        std::cout << "  " << shader.path.generic_string() << "\n";
    }
    std::cout << "next: ssstudio build " << root.string() << " --profile release\n";
    return 0;
}

/// Reads a whole file, or standard input when `path` is "-".
bool read_source(const std::string& path, std::string& out) {
    if (path == "-") {
        std::ostringstream buffer;
        buffer << std::cin.rdbuf();
        out = buffer.str();
        return true;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return true;
}

/// A filename with every extension removed, so "seascape.frag.glsl" suggests
/// the id "seascape" rather than "seascape.frag".
std::string stem_without_extensions(const fs::path& path) {
    const std::string name = path.filename().string();
    const std::size_t dot = name.find('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

int cmd_import(const Args& args) {
    if (args.target.empty() || args.source.empty()) {
        std::cerr << "ssstudio import needs a project and a source file (or - to read stdin)\n";
        return 2;
    }

    Project project;
    Diagnostics diags;
    if (!load_project(args.target, project, diags)) {
        print_diagnostics(diags);
        return 1;
    }
    print_diagnostics(diags);

    ImportRequest request;
    if (!read_source(args.source, request.source)) {
        std::cerr << "cannot read " << args.source << "\n";
        return 1;
    }
    if (request.source.find_first_not_of(" \t\r\n") == std::string::npos) {
        std::cerr << "nothing to import: the source is empty\n";
        return 1;
    }
    if (!args.common.empty() && !read_source(args.common, request.options.common)) {
        std::cerr << "cannot read " << args.common << "\n";
        return 1;
    }

    // A description of a whole chain rather than one shader's body. Recognised
    // by its shape rather than by a flag: a file that parses as one is one.
    if (request.source.find("\"renderpass\"") != std::string::npos) {
        ImportedChain chain;
        Diagnostics read_diags;
        if (!read_imported_chain(request.source, chain, read_diags)) {
            print_diagnostics(read_diags);
            return 1;
        }
        print_diagnostics(read_diags);
        print_diagnostics(chain.notes);

        ImportOptions options;
        options.source_url = args.url;
        options.author = args.author;
        options.licence = args.licence;

        ChainImportResult chain_result;
        Diagnostics chain_diags;
        const bool chain_ok = write_chain_files(project, chain, options, chain_result, chain_diags);
        print_diagnostics(chain_diags);
        if (!chain_ok) return 1;

        if (!chain_result.vertex_path.empty()) {
            ShaderDesc vertex;
            vertex.id = chain_result.vertex_id;
            vertex.path = chain_result.vertex_path;
            vertex.stage = Stage::Vertex;
            vertex.language = Language::GLSL;
            project.shaders.push_back(std::move(vertex));
        }
        for (const auto& [id, path] : chain_result.shaders) {
            ShaderDesc fragment;
            fragment.id = id;
            fragment.path = path;
            fragment.stage = Stage::Fragment;
            fragment.language = Language::GLSL;
            project.shaders.push_back(std::move(fragment));
        }
        for (const auto& [id, textures] : chain_result.bindings) {
            project.bindings[id].textures = textures;
        }
        if (!chain_result.provenance.empty()) {
            project.provenance[chain_result.pipeline.fragment] = chain_result.provenance;
        }
        project.pipelines.push_back(chain_result.pipeline);
        project.preview.pipeline = chain_result.pipeline.name;

        Diagnostics chain_save;
        const bool chain_saved = save_project(project, chain_save);
        print_diagnostics(chain_save);
        if (!chain_saved) return 1;

        std::cout << "imported " << chain_result.pipeline.passes.size() << " buffer pass(es) and "
                  << chain_result.pipeline.fragment << " as pipeline '"
                  << chain_result.pipeline.name << "'\n";
        for (const auto& [id, path] : chain_result.shaders) {
            std::cout << "  " << id << " -> " << path.generic_string() << "\n";
        }
        std::cout << "next: ssstudio check " << args.target << "\n";
        return 0;
    }

    request.id = !args.id.empty()            ? args.id
                 : args.source == "-"        ? std::string("imported")
                                             : stem_without_extensions(args.source);
    request.options.source_url = args.url;
    request.options.author = args.author;
    request.options.licence = args.licence;
    request.options.force_opaque = !args.keep_alpha;

    ImportResult result;
    Diagnostics import_diags;
    const bool ok = write_import_files(project, request, result, import_diags);
    print_diagnostics(import_diags);
    if (!ok) return 1;

    // The importer wrote the files; registering them is the caller's job so that
    // the app can build an editor buffer at the same time and this cannot.
    if (!result.vertex_path.empty()) {
        ShaderDesc vertex;
        vertex.id = result.vertex_id;
        vertex.path = result.vertex_path;
        vertex.stage = Stage::Vertex;
        vertex.language = Language::GLSL;
        project.shaders.push_back(std::move(vertex));
    }
    ShaderDesc fragment;
    fragment.id = result.shader_id;
    fragment.path = result.shader_path;
    fragment.stage = Stage::Fragment;
    // Set explicitly rather than left to the project default, which is HLSL for
    // every scaffolded project and would hand this file to the wrong front end.
    fragment.language = Language::GLSL;
    project.shaders.push_back(std::move(fragment));

    if (!result.provenance.empty()) project.provenance[result.shader_id] = result.provenance;
    if (!project.find_pipeline(result.pipeline.name)) {
        project.pipelines.push_back(result.pipeline);
    }

    Diagnostics save_diags;
    const bool saved = save_project(project, save_diags);
    print_diagnostics(save_diags);
    if (!saved) return 1;

    std::cout << "imported " << result.shader_id << " -> "
              << result.shader_path.generic_string() << "\n";
    if (!result.vertex_path.empty()) {
        std::cout << "added " << result.vertex_id << " -> "
                  << result.vertex_path.generic_string()
                  << " (the preview needs a vertex shader to pair with)\n";
    }
    std::cout << "next: ssstudio check " << args.target << "\n";
    return 0;
}

int cmd_check(const Args& args) {
    Project project;
    Diagnostics diags;
    if (!load_project(args.target, project, diags)) {
        print_diagnostics(diags);
        return 1;
    }
    const BuildProfile* profile =
        args.profile.empty() ? &project.profiles.front() : project.find_profile(args.profile);
    if (!profile) {
        std::cerr << "no such profile: " << args.profile << "\n";
        return 2;
    }
    auto backend = create_default_backend(configured_tool_dir());
    Diagnostics d = check_project(project, *backend, *profile);
    diags.insert(diags.end(), d.begin(), d.end());
    const int errors = print_diagnostics(diags);
    if (!args.quiet && errors == 0) {
        std::cout << "ok: " << project.shaders.size() << " shader(s), backend " << backend->name()
                  << "\n";
    }
    return errors ? 1 : 0;
}

int cmd_build(const Args& args, bool helper_files) {
    Project project;
    Diagnostics load_diags;
    if (!load_project(args.target, project, load_diags)) {
        print_diagnostics(load_diags);
        return 1;
    }
    print_diagnostics(load_diags);

    if (!helper_files) {
        for (auto& p : project.profiles) {
            p.emit.header = p.emit.loader = p.emit.docs = false;
            p.emit.embed_header = p.emit.reflection_json = p.emit.meta_toml = false;
            p.emit.cmake_snippet = false;
        }
    }

    BuildOptions options;
    options.profile_name = args.profile;
    if (!args.out.empty()) options.output_dir = args.out;
    options.write_files = !args.dry_run;
    options.verify_roundtrip = args.verify;
    options.pin_new_keys = args.pin;
    if (!args.quiet) {
        options.progress = [](const BuildProgress& p) {
            std::cout << "[" << to_string(p.stage) << "] " << p.message;
            if (p.total > 0) std::cout << " (" << p.done + 1 << "/" << p.total << ")";
            std::cout << "\n";
        };
    }

    auto backend = create_default_backend(configured_tool_dir());
    BuildReport report = build_project(project, *backend, options);
    const int errors = print_diagnostics(report.diagnostics);

    if (report.ok && !args.quiet) {
        std::cout << "\n" << report.stats.shader_count << " shader(s), "
                  << format_mask_to_string(report.stats.format_mask) << ", "
                  << report.stats.total_bytes << " bytes";
        if (report.stats.raw_bytes) {
            const double ratio =
                100.0 * static_cast<double>(report.stats.stored_bytes) /
                static_cast<double>(report.stats.raw_bytes);
            std::cout << " (blobs at " << static_cast<int>(ratio) << "% of raw)";
        }
        std::cout << " in " << report.seconds << "s\n";
        for (const auto& a : report.artifacts) {
            std::cout << "  " << a.kind << ": " << a.path.string() << " (" << a.bytes << " B)\n";
        }
    }
    return report.ok ? 0 : (errors ? 1 : 1);
}

int cmd_keys(const Args& args) {
    Project project;
    Diagnostics diags;
    if (!load_project(args.target, project, diags)) {
        print_diagnostics(diags);
        return 1;
    }
    const BuildProfile* profile =
        args.profile.empty() ? &project.profiles.front() : project.find_profile(args.profile);
    if (!profile) {
        std::cerr << "no such profile: " << args.profile << "\n";
        return 2;
    }
    const PackLayout layout = profile->layout_override.value_or(PackLayout{});

    std::vector<KeyRequest> requests;
    std::vector<const ShaderDesc*> shaders;
    for (const auto& s : project.shaders) {
        if (!s.include_in_pack) continue;
        if (profile->stage_filter && !(profile->stage_filter & stage_bit(s.stage))) continue;
        requests.push_back({s.id, s.key});
        shaders.push_back(&s);
    }
    Diagnostics kd;
    std::map<std::string, std::uint32_t> pins;
    const auto keys = assign_keys(requests, profile->key_strategy, layout.key16, kd, &pins);
    print_diagnostics(kd);

    for (std::size_t i = 0; i < shaders.size(); ++i) {
        std::cout << enum_name(profile->enum_prefix, shaders[i]->id) << " = " << keys[i] << "  (0x"
                  << std::hex << keys[i] << std::dec << ")"
                  << (shaders[i]->key ? "  [pinned]" : "  [assigned]") << "\n";
    }
    return 0;
}

/// Reads a theme pack, says what it resolved to, and reports what is wrong
/// with it. The counterpart of `inspect` for the other customizable format in
/// this project, and the command a theme generator should run before shipping
/// anything.
int cmd_theme(const Args& args) {
    if (args.target.empty()) {
        std::cerr << "usage: ssstudio theme <pack> [--resolve] [--lint]\n";
        return 2;
    }

    ThemePack pack;
    Diagnostics diags;
    const bool read = parse_theme_pack(args.target, pack, diags);
    print_diagnostics(diags);
    if (!read) return 1;

    // Sibling packs, so an `inherit` that names one resolves rather than
    // falling back to dark and quietly looking wrong.
    ThemeResolveContext context;
    const std::filesystem::path root = std::filesystem::path(args.target).parent_path();
    for (const auto& path : theme_pack_paths({root})) {
        ThemePack sibling;
        Diagnostics ignored;
        if (parse_theme_pack(path, sibling, ignored)) {
            context.available[sibling.id] = std::move(sibling);
        }
    }

    Diagnostics resolve_diags;
    ResolvedTheme theme;
    const bool resolved = resolve_theme(pack, context, theme, resolve_diags);
    print_diagnostics(resolve_diags);

    std::cout << "id          " << pack.id << "\n";
    std::cout << "name        " << pack.name << "\n";
    std::cout << "format      " << pack.format << "\n";
    std::cout << "inherit     " << pack.inherit << "\n";
    std::cout << "appearance  " << theme.appearance << (pack.appearance.empty() ? " (inferred)" : "")
              << "\n";
    if (!pack.author.empty()) std::cout << "author      " << pack.author << "\n";
    if (!pack.version.empty()) std::cout << "version     " << pack.version << "\n";
    std::cout << "set by pack " << pack.roles.size() << " role(s), " << pack.ui.size()
              << " color(s), " << pack.syntax.size() << " syntax, "
              << (pack.style.scalar.size() + pack.style.vec2.size() + pack.style.direction.size())
              << " metric(s)\n";
    std::cout << "resolved    " << theme.roles.size() << " role(s), " << theme.ui.size()
              << " widget color(s)\n";
    if (!resolved) {
        std::cout << "\nthe pack was refused; the values above are its base theme\n";
    }

    if (args.resolve) {
        // Which layer each value came from is the question a pack author
        // actually has, so the listing says "set" or "derived" per line rather
        // than printing two undifferentiated columns.
        std::cout << "\nroles\n";
        for (const auto& name : theme_role_names()) {
            std::cout << "  " << name << std::string(name.size() < 18 ? 18 - name.size() : 1, ' ')
                      << color_rgba_to_hex(theme.role(name))
                      << (pack.roles.count(name) != 0 ? "  set" : "  derived") << "\n";
        }
        std::cout << "\nwidget colors\n";
        for (const auto& name : theme_ui_color_names()) {
            std::cout << "  " << name << std::string(name.size() < 28 ? 28 - name.size() : 1, ' ')
                      << color_rgba_to_hex(theme.ui_color(name))
                      << (pack.ui.count(name) != 0 ? "  set" : "  derived") << "\n";
        }
        std::cout << "\nsyntax\n";
        for (std::size_t i = 0; i < kTokenKindCount; ++i) {
            const std::string name(to_string(static_cast<TokenKind>(i)));
            std::cout << "  " << name << std::string(name.size() < 14 ? 14 - name.size() : 1, ' ')
                      << color_rgba_to_hex(theme.syntax[i])
                      << (pack.syntax.count(name) != 0 ? "  set" : "  fallback palette") << "\n";
        }
        for (const auto* section : {&theme.diagnostics, &theme.graph, &theme.preview}) {
            std::cout << "\n"
                      << (section == &theme.diagnostics
                              ? "diagnostics"
                              : (section == &theme.graph ? "graph" : "preview"))
                      << "\n";
            for (const auto& [name, color] : *section) {
                std::cout << "  " << name
                          << std::string(name.size() < 24 ? 24 - name.size() : 1, ' ')
                          << color_rgba_to_hex(color) << "\n";
            }
        }
    }

    int findings = 0;
    if (args.lint) {
        const Diagnostics advice = lint_theme(theme);
        std::cout << "\nlint\n";
        if (advice.empty()) {
            std::cout << "  nothing to report\n";
        } else {
            for (const auto& d : advice) {
                std::cout << "  " << to_string(d.severity) << ": " << d.message << "\n";
                if (d.severity != Severity::Info) ++findings;
            }
        }
    }

    // A contrast finding is advice about a theme rather than a broken file, so
    // it does not fail the command; only a refused pack does. Reported all the
    // same, because a caller piping this somewhere wants the count.
    if (findings > 0) std::cout << "\n" << findings << " finding(s) worth fixing\n";
    return resolved ? 0 : 1;
}

int cmd_inspect(const Args& args) {
    std::ifstream f(args.target, std::ios::binary);
    if (!f) {
        std::cerr << "cannot open " << args.target << "\n";
        return 1;
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
    PackReader reader;
    Diagnostics diags;
    if (!reader.open(bytes, diags)) {
        print_diagnostics(diags);
        return 1;
    }
    print_diagnostics(diags);

    std::cout << "layout      " << reader.layout().signature() << "\n";
    std::cout << "shaders     " << reader.entries().size() << "\n";
    std::cout << "formats     " << format_mask_to_string(reader.format_mask()) << "\n";
    std::cout << "compression " << to_string(reader.compression()) << "\n";
    std::cout << "size        " << bytes.size() << " bytes\n";
    if (!reader.user_section().empty()) {
        std::cout << "user section " << reader.user_section().size() << " bytes\n";
    }
    std::cout << "\n";
    for (const auto& e : reader.entries()) {
        std::cout << (e.name.empty() ? "<unnamed>" : e.name) << "  key=" << e.key << " (0x"
                  << std::hex << e.key << std::dec << ")"
                  << "  " << to_string(e.stage) << "  entry='" << e.entry_point << "'\n";
        std::cout << "    samplers=" << e.num_samplers
                  << " storage_tex=" << e.num_storage_textures
                  << " storage_buf=" << e.num_storage_buffers
                  << " uniforms=" << e.num_uniform_buffers;
        if (e.stage == Stage::Compute) {
            std::cout << " rw_tex=" << e.num_readwrite_storage_textures
                      << " rw_buf=" << e.num_readwrite_storage_buffers << " threads="
                      << e.compute_threads[0] << "x" << e.compute_threads[1] << "x"
                      << e.compute_threads[2];
        }
        std::cout << "\n";
        for (const auto& b : e.blobs) {
            std::cout << "    " << to_string(b.format) << ": " << b.size << " B";
            if (b.size != b.uncompressed_size) std::cout << " (from " << b.uncompressed_size << ")";
            std::cout << " @" << b.offset << "\n";
        }
    }
    return 0;
}

int cmd_reflect(const Args& args) {
    std::ifstream f(args.target, std::ios::binary);
    if (!f) {
        std::cerr << "cannot open " << args.target << "\n";
        return 1;
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
    Diagnostics diags;
    const Stage stage = stage_from_string(args.stage).value_or(Stage::Fragment);
    const Reflection r = reflect_spirv(bytes, stage, "main", diags);
    print_diagnostics(diags);
    std::cout << reflection_to_json(r, 2) << "\n";
    return 0;
}

int cmd_settings(const Args& args) {
    const fs::path path = AppSettings::default_path();
    if (args.path_only) {
        std::cout << path.string() << "\n";
        return 0;
    }
    AppSettings settings;
    Diagnostics diags;
    load_settings(path, settings, diags);
    print_diagnostics(diags);
    if (!fs::exists(path)) {
        std::cout << "# no settings file yet; writing defaults to " << path.string() << "\n";
        save_settings(path, settings, diags);
        print_diagnostics(diags);
    }
    std::ifstream f(path);
    std::cout << f.rdbuf();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    switch (parse_args(argc, argv, args)) {
        case ParseResult::Ok:
            break;
        case ParseResult::Help:
            usage();
            return 0;
        case ParseResult::Invalid:
            usage();
            return 2;
    }

    if (args.command == "new") return cmd_new(args);
    if (args.command == "import") return cmd_import(args);
    if (args.command == "check") return cmd_check(args);
    if (args.command == "build") return cmd_build(args, true);
    if (args.command == "pack") return cmd_build(args, false);
    if (args.command == "keys") return cmd_keys(args);
    if (args.command == "inspect") return cmd_inspect(args);
    if (args.command == "reflect") return cmd_reflect(args);
    if (args.command == "settings") return cmd_settings(args);
    if (args.command == "theme") return cmd_theme(args);

    std::cerr << "unknown command: " << args.command << "\n\n";
    usage();
    return 2;
}
