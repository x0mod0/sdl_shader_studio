#include "ssstudio/compiler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <sstream>
#include <string_view>
#include <utility>

#include "ssstudio/hash.h"
#include "ssstudio/process.h"
#include "ssstudio/spirv_reflect.h"

#if defined(SSSTUDIO_HAVE_SHADERCROSS)
#include <SDL3_shadercross/SDL_shadercross.h>
#endif

namespace ssstudio {
namespace {

Diagnostic make_diag(Severity sev, std::string msg, std::string file = {}, int line = 0,
                     int col = 0, std::string code = {}) {
    Diagnostic d;
    d.severity = sev;
    d.message = std::move(msg);
    d.file = std::move(file);
    d.line = line;
    d.column = col;
    d.code = std::move(code);
    return d;
}

// DXC emits "file.hlsl:12:5: error: message [code]". Anything unparseable is
// kept verbatim so nothing from the compiler is ever swallowed.
Diagnostics parse_dxc_output(const std::string& text, const std::string& fallback_file) {
    Diagnostics out;
    std::istringstream is(text);
    std::string line;
    while (std::getline(is, line)) {
        if (line.empty()) continue;

        std::size_t pos = 0;
        std::string file = fallback_file;
        int ln = 0, col = 0;

        // Find "<file>:<num>:<num>: ", which may sit anywhere in the line rather
        // than at its start: the command line tool prefixes what it prints with a
        // log stamp, and it wraps the compiler's message in one of its own. Every
        // colon is tried so that a Windows drive letter, a timestamp or a process
        // id earlier in the line cannot be mistaken for the position.
        for (std::size_t first = line.find(':'); first != std::string::npos;
             first = line.find(':', first + 1)) {
            const std::size_t second = line.find(':', first + 1);
            if (second == std::string::npos) break;
            const std::size_t third = line.find(':', second + 1);
            if (third == std::string::npos) break;

            const std::string l = line.substr(first + 1, second - first - 1);
            const std::string c = line.substr(second + 1, third - second - 1);
            if (l.empty() || l.find_first_not_of("0123456789") != std::string::npos) continue;
            if (c.empty() || c.find_first_not_of("0123456789") != std::string::npos) continue;

            // The file name is whatever runs up to the numbers without a space in
            // it, so any prefix the tool added is left behind.
            const std::size_t space = line.find_last_of(" \t", first);
            const std::size_t start = space == std::string::npos ? 0 : space + 1;
            file = line.substr(start, first - start);
            ln = std::stoi(l);
            col = std::stoi(c);
            pos = third + 1;
            break;
        }

        // An indented line with no position of its own is the source excerpt and
        // caret DXC prints underneath a message. It belongs to the diagnostic
        // above it; on its own it would arrive as a second, positionless error
        // and fail a build over a line of punctuation.
        if (pos == 0 && (line.front() == ' ' || line.front() == '\t') && !out.empty()) {
            out.back().message += "\n" + line;
            continue;
        }

        std::string rest = line.substr(pos);
        while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());

        Severity sev = Severity::Error;
        if (rest.rfind("warning:", 0) == 0) {
            sev = Severity::Warning;
            rest = rest.substr(8);
        } else if (rest.rfind("error:", 0) == 0) {
            rest = rest.substr(6);
        } else if (rest.rfind("note:", 0) == 0) {
            sev = Severity::Info;
            rest = rest.substr(5);
        }
        while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());

        std::string code;
        if (!rest.empty() && rest.back() == ']') {
            const std::size_t open = rest.rfind('[');
            if (open != std::string::npos) {
                code = rest.substr(open + 1, rest.size() - open - 2);
                rest = rest.substr(0, open);
                while (!rest.empty() && rest.back() == ' ') rest.pop_back();
            }
        }
        out.push_back(make_diag(sev, rest, file, ln, col, code));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Fallback backend: no toolchain present. It still parses the source enough to
// keep the editor useful (entry point presence, obviously wrong register spaces)
// and always reports why nothing can be built.
// ---------------------------------------------------------------------------
class NullBackend final : public ICompilerBackend {
public:
    explicit NullBackend(std::string reason) : reason_(std::move(reason)) {}

    std::string name() const override { return "none (" + reason_ + ")"; }
    bool supports(Language) const override { return false; }
    std::uint32_t supported_formats() const override { return FORMAT_NONE; }

    Diagnostics check(const CompileRequest& req) override {
        Diagnostics out;
        out.push_back(make_diag(Severity::Error,
                                "no shader compiler available: " + reason_ +
                                    ". Set the toolchain path in Settings > Tools.",
                                req.path.string(), 0, 0, "SSSTUDIO-TOOL"));
        if (req.source.find(req.entry_point) == std::string::npos) {
            out.push_back(make_diag(Severity::Warning,
                                    "entry point '" + req.entry_point + "' not found in source",
                                    req.path.string(), 0, 0, "SSSTUDIO-ENTRY"));
        }
        return out;
    }

    CompileResult compile(const CompileRequest& req) override {
        CompileResult r;
        r.ok = false;
        r.diagnostics = check(req);
        return r;
    }

private:
    std::string reason_;
};

/// The Metal Shading Language version asked for when transpiling.
///
/// SDL_shadercross defaults to 1.2, which cannot express an array of textures -
/// a shader declaring one transpiles to nothing with "MSL 2.0 or greater is
/// required for arrays of textures". Two is the lowest version that can, so it
/// is what is asked for: raising the floor further would rule out machines for
/// no gain. MSL 2.0 is macOS 10.13 and iOS 11.
constexpr const char* kMslVersion = "2.0.0";

/// Whether a failed compile looks like one that debug info caused.
///
/// DXC's SPIR-V legalizer gives up on some constructs only when debug info is
/// on - an array of textures among them, where it cannot rewrite the
/// DebugGlobalVariable that describes it. The message is recognisable, and the
/// alternative to recognising it is retrying every failure twice, which would
/// double the cost of the error path that compile-on-type spends most of its
/// time in.
bool looks_like_a_legalization_failure(const std::string& text) {
    return text.find("legalize") != std::string::npos;
}

#if defined(SSSTUDIO_HAVE_SHADERCROSS)
// ---------------------------------------------------------------------------
// SDL_shadercross backend: HLSL -> SPIR-V (DXC) -> DXIL / DXBC / MSL.
// ---------------------------------------------------------------------------
/// Debug info and the shader's debug name are passed to SDL_shadercross through
/// a properties object rather than fields on the info struct. The handle is
/// owned for the duration of the compile call that uses it; a value of zero is
/// the documented "no extensions needed" case, so nothing is allocated when
/// neither option is requested.
class ShaderProperties {
public:
    ShaderProperties(bool debug_info, const std::string& debug_name) {
        id_ = SDL_CreateProperties();
        if (id_ == 0) return;
        // Always set, because the default cannot express an array of textures.
        SDL_SetStringProperty(id_, SDL_SHADERCROSS_PROP_SPIRV_MSL_VERSION_STRING, kMslVersion);
        if (debug_info) {
            SDL_SetBooleanProperty(id_, SDL_SHADERCROSS_PROP_SHADER_DEBUG_ENABLE_BOOLEAN, true);
        }
        if (!debug_name.empty()) {
            SDL_SetStringProperty(id_, SDL_SHADERCROSS_PROP_SHADER_DEBUG_NAME_STRING,
                                  debug_name.c_str());
        }
    }
    ~ShaderProperties() {
        if (id_ != 0) SDL_DestroyProperties(id_);
    }
    ShaderProperties(const ShaderProperties&) = delete;
    ShaderProperties& operator=(const ShaderProperties&) = delete;

    /// Zero only when the properties object could not be created, which the
    /// API reads as "no extensions".
    SDL_PropertiesID id() const { return id_; }

private:
    SDL_PropertiesID id_ = 0;
};

class ShadercrossBackend final : public ICompilerBackend, public ISpirvTranslator {
public:
    ShadercrossBackend() { initialized_ = SDL_ShaderCross_Init(); }
    ~ShadercrossBackend() override {
        if (initialized_) SDL_ShaderCross_Quit();
    }

    std::string name() const override { return "SDL_shadercross"; }
    bool supports(Language l) const override { return l == Language::HLSL; }

    std::uint32_t supported_formats() const override {
        std::uint32_t mask = FORMAT_SPIRV | FORMAT_MSL;
        const SDL_GPUShaderFormat f = SDL_ShaderCross_GetSPIRVShaderFormats();
        if (f & SDL_GPU_SHADERFORMAT_DXBC) mask |= FORMAT_DXBC;
        if (f & SDL_GPU_SHADERFORMAT_DXIL) mask |= FORMAT_DXIL;
        return mask;
    }

    Diagnostics check(const CompileRequest& req) override {
        CompileRequest r = req;
        r.formats = FORMAT_SPIRV;
        r.optimization = r.skip_optimizer_on_check ? 0 : r.optimization;
        return compile(r).diagnostics;
    }

    CompileResult compile(const CompileRequest& req) override;

    // SPIR-V -> DXIL / DXBC / MSL only. Shared with the GLSL front end, whose
    // compiler produces SPIR-V and nothing else.
    CompileResult translate(const CompileRequest& req,
                            const std::vector<std::uint8_t>& spirv) override;

private:
    bool initialized_ = false;
};

CompileResult ShadercrossBackend::compile(const CompileRequest& req) {
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();

    CompileResult result;
    if (!initialized_) {
        result.diagnostics.push_back(make_diag(Severity::Error,
                                               "SDL_shadercross failed to initialize",
                                               req.path.string(), 0, 0, "SSSTUDIO-TOOL"));
        return result;
    }

    std::vector<const char*> define_names, define_values;
    for (const auto& [k, v] : req.defines) {
        define_names.push_back(k.c_str());
        define_values.push_back(v.c_str());
    }
    std::vector<const char*> include_dirs;
    std::vector<std::string> include_storage;
    include_storage.reserve(req.include_dirs.size());
    for (const auto& d : req.include_dirs) include_storage.push_back(d.string());
    for (const auto& d : include_storage) include_dirs.push_back(d.c_str());

    SDL_ShaderCross_HLSL_Info hlsl;
    SDL_zero(hlsl);
    hlsl.source = req.source.c_str();
    hlsl.entrypoint = req.entry_point.c_str();
    hlsl.include_dir = include_dirs.empty() ? nullptr : include_dirs.front();
    hlsl.defines = nullptr;  // filled below when defines exist
    hlsl.shader_stage = req.stage == Stage::Vertex     ? SDL_SHADERCROSS_SHADERSTAGE_VERTEX
                        : req.stage == Stage::Fragment ? SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT
                                                       : SDL_SHADERCROSS_SHADERSTAGE_COMPUTE;
    const ShaderProperties props(req.debug_info, req.id);
    hlsl.props = props.id();

    std::vector<SDL_ShaderCross_HLSL_Define> defines;
    for (const auto& [k, v] : req.defines) {
        SDL_ShaderCross_HLSL_Define d;
        d.name = const_cast<char*>(k.c_str());
        d.value = const_cast<char*>(v.c_str());
        defines.push_back(d);
    }
    if (!defines.empty()) {
        defines.push_back({nullptr, nullptr});
        hlsl.defines = defines.data();
    }

    size_t spirv_size = 0;
    void* spirv = SDL_ShaderCross_CompileSPIRVFromHLSL(&hlsl, &spirv_size);
    if (!spirv) {
        const char* err = SDL_GetError();
        Diagnostics parsed = parse_dxc_output(err ? err : "unknown compile error",
                                              req.path.string());
        if (parsed.empty()) {
            // SDL_shadercross reports the compiler's output through SDL_GetError().
            // Getting nothing back means the detail was lost on the way here - most
            // often because the app and the shadercross build resolve to different
            // copies of SDL3, so the error was set in one and read from the other.
            parsed.push_back(make_diag(
                Severity::Error,
                "the HLSL compiler rejected this shader but returned no detail. Check the "
                "entry point and this stage's register spaces; running the shadercross "
                "tool on this file from a terminal prints the compiler's full output.",
                req.path.string()));
        }
        result.diagnostics = std::move(parsed);
        return result;
    }

    std::vector<std::uint8_t> spirv_bytes(static_cast<const std::uint8_t*>(spirv),
                                          static_cast<const std::uint8_t*>(spirv) + spirv_size);

    // Reflection first: everything downstream (UI, packer, docs) uses it.
    result.reflection = reflect_spirv(spirv_bytes, req.stage, req.entry_point,
                                      result.diagnostics);
    result.reflection.entry_point = req.entry_point;

    if (req.formats & FORMAT_SPIRV) result.blobs[FORMAT_SPIRV] = spirv_bytes;

    CompileResult translated = translate(req, spirv_bytes);
    for (auto& [format, blob] : translated.blobs) result.blobs[format] = std::move(blob);
    result.diagnostics.insert(result.diagnostics.end(), translated.diagnostics.begin(),
                              translated.diagnostics.end());

    SDL_free(spirv);

    Diagnostics binding = result.reflection.validate_binding_model(req.path.string());
    result.diagnostics.insert(result.diagnostics.end(), binding.begin(), binding.end());

    if (req.warnings_as_errors) {
        for (auto& d : result.diagnostics)
            if (d.severity == Severity::Warning) d.severity = Severity::Error;
    }

    result.ok = !has_errors(result.diagnostics) && !result.blobs.empty();
    result.milliseconds =
        std::chrono::duration<double, std::milli>(clock::now() - start).count();
    return result;
}

CompileResult ShadercrossBackend::translate(const CompileRequest& req,
                                            const std::vector<std::uint8_t>& spirv) {
    CompileResult result;
    SDL_ShaderCross_SPIRV_Info spv;
    SDL_zero(spv);
    spv.bytecode = spirv.data();
    spv.bytecode_size = spirv.size();
    spv.entrypoint = req.entry_point.c_str();
    spv.shader_stage = req.stage == Stage::Vertex     ? SDL_SHADERCROSS_SHADERSTAGE_VERTEX
                       : req.stage == Stage::Fragment ? SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT
                                                      : SDL_SHADERCROSS_SHADERSTAGE_COMPUTE;
    const ShaderProperties props(req.debug_info, req.id);
    spv.props = props.id();

    auto emit = [&](ShaderFormat fmt) {
        if (!(req.formats & fmt)) return;
        size_t size = 0;
        void* out = nullptr;
        switch (fmt) {
            case FORMAT_DXIL: out = SDL_ShaderCross_CompileDXILFromSPIRV(&spv, &size); break;
            case FORMAT_DXBC: out = SDL_ShaderCross_CompileDXBCFromSPIRV(&spv, &size); break;
            case FORMAT_MSL:
                out = SDL_ShaderCross_TranspileMSLFromSPIRV(&spv);
                size = out ? std::strlen(static_cast<const char*>(out)) + 1 : 0;
                break;
            default: return;
        }
        if (!out || size == 0) {
            result.diagnostics.push_back(make_diag(
                Severity::Error,
                std::string("could not produce ") + std::string(to_string(fmt)) + ": " +
                    (SDL_GetError() ? SDL_GetError() : "unknown error"),
                req.path.string(), 0, 0, "SSSTUDIO-TRANSLATE"));
            if (out) SDL_free(out);
            return;
        }
        result.blobs[fmt].assign(static_cast<const std::uint8_t*>(out),
                                 static_cast<const std::uint8_t*>(out) + size);
        SDL_free(out);
    };

    emit(FORMAT_DXIL);
    emit(FORMAT_DXBC);
    emit(FORMAT_MSL);
    result.ok = !has_errors(result.diagnostics);
    return result;
}
#endif  // SSSTUDIO_HAVE_SHADERCROSS

// ---------------------------------------------------------------------------
// SDL_shadercross command line backend: the same toolchain, one process away.
//
// A prebuilt SDL_shadercross ships the SDL3 it was built against and loads it
// through its own rpath, so linking it puts two copies of SDL3 in this process.
// They do not share the error buffer, which is where the compiler's message
// lives, so every failed HLSL compile arrives with nothing to show the user.
// Running the tool instead keeps this process to a single SDL3 and hands us the
// compiler's real output, file and line included, on the child's stderr.
// ---------------------------------------------------------------------------

/// A scratch directory that removes itself. shadercross reads its input from a
/// file and writes its output to another, so every request needs somewhere of its
/// own to put them; compiles run on several worker threads at once, hence a name
/// no two of them can agree on.
class TempDir {
public:
    TempDir() {
        static const std::uint64_t salt = [] {
            std::random_device rd;
            return (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
        }();
        static std::atomic<std::uint64_t> counter{0};

        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::string material = std::to_string(salt) + "-" + std::to_string(now) + "-" +
                                     std::to_string(counter.fetch_add(1));

        std::error_code ec;
        std::filesystem::path dir =
            std::filesystem::temp_directory_path(ec) / ("ssstudio-" + to_hex(fnv1a64(material)));
        if (!ec && std::filesystem::create_directories(dir, ec) && !ec) path_ = std::move(dir);
    }

    ~TempDir() {
        if (path_.empty()) return;
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    /// Empty when the directory could not be created, which the caller reports
    /// rather than compiling into a path that does not exist.
    const std::filesystem::path& path() const { return path_; }
    bool ok() const { return !path_.empty(); }

private:
    std::filesystem::path path_;
};

const char* stage_argument(Stage stage) {
    switch (stage) {
        case Stage::Vertex: return "vertex";
        case Stage::Compute: return "compute";
        default: return "fragment";
    }
}

/// The tool's name for a format, and the extension it expects to write it to. It
/// infers both from the output file name, but they are passed explicitly so a
/// change to either cannot quietly select a different target.
bool format_argument(ShaderFormat fmt, const char*& name, const char*& extension) {
    switch (fmt) {
        case FORMAT_SPIRV: name = "SPIRV"; extension = ".spv"; return true;
        case FORMAT_MSL: name = "MSL"; extension = ".msl"; return true;
        case FORMAT_DXIL: name = "DXIL"; extension = ".dxil"; return true;
        case FORMAT_DXBC: name = "DXBC"; extension = ".dxbc"; return true;
        default: return false;
    }
}

/// SDL's logger stamps "<date> <time> <program>[<pid>:<tid>] " onto every line
/// the tool prints on some platforms. Diagnostics are parsed around it, but the
/// raw text is also shown when nothing could be parsed, and it reads better
/// without. Only that exact shape is removed.
std::string strip_log_prefix(const std::string& line) {
    if (line.empty() || !std::isdigit(static_cast<unsigned char>(line.front()))) return line;
    const std::size_t end = line.find("] ");
    if (end == std::string::npos) return line;
    if (line.find('[') > end) return line;
    return line.substr(end + 2);
}

std::string strip_log_prefixes(const std::string& text) {
    std::string out;
    std::istringstream is(text);
    std::string line;
    while (std::getline(is, line)) {
        out += strip_log_prefix(line);
        out += '\n';
    }
    return out;
}

bool read_binary_file(const std::filesystem::path& p, std::vector<std::uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

/// Looks for one of the tool's runtime libraries beside it. shadercross cannot be
/// asked which targets its build supports, and it ships the libraries that decide
/// that question next to the executable or in the lib directory above it.
bool has_runtime_library(const std::filesystem::path& exe, std::string_view stem) {
    const std::filesystem::path bin = exe.parent_path();
    const std::array<std::filesystem::path, 3> dirs = {bin, bin / "lib",
                                                       bin.parent_path() / "lib"};
    for (const auto& dir : dirs) {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.path().filename().string().find(stem) != std::string::npos) return true;
        }
    }
    return false;
}

/// Where the tool that is about to run came from. The copy that ships with the
/// application is not a choice anyone made, so its path - which on macOS is a
/// long walk through the bundle - says nothing a user needs to read.
enum class ToolOrigin {
    Bundled,     // shipped alongside this program
    Configured,  // Settings > Tools, or the directory the build was pointed at
    Path,        // found on PATH
};

/// How the tool is named wherever a backend names itself: the menu bar status,
/// the compiler line in the log, and Settings > Tools. It has to stay short
/// enough to sit at the end of a menu bar and still say which tool is running.
std::string describe_shadercross(const std::filesystem::path& exe, ToolOrigin origin) {
    switch (origin) {
        case ToolOrigin::Bundled:
            return "SDL_shadercross (bundled)";
        case ToolOrigin::Configured:
            // Someone pointed at this one, so the path is the confirmation that
            // the override took effect - the one case where it is worth its
            // width.
            return "SDL_shadercross (" + exe.string() + ")";
        case ToolOrigin::Path:
            break;
    }
    return "SDL_shadercross (PATH)";
}

class ShadercrossCliBackend final : public ICompilerBackend, public ISpirvTranslator {
public:
    ShadercrossCliBackend(std::filesystem::path exe, ToolOrigin origin)
        : exe_(std::move(exe)), origin_(origin) {
        formats_ = FORMAT_SPIRV | FORMAT_MSL;
        if (has_runtime_library(exe_, "dxil")) formats_ |= FORMAT_DXIL;
#if defined(_WIN32)
        // d3dcompiler_47 is part of Windows rather than something shadercross
        // ships, so there is nothing beside the tool to look for.
        formats_ |= FORMAT_DXBC;
#else
        if (has_runtime_library(exe_, "vkd3d-utils")) formats_ |= FORMAT_DXBC;
#endif
    }

    std::string name() const override { return describe_shadercross(exe_, origin_); }
    bool supports(Language l) const override { return l == Language::HLSL; }
    std::uint32_t supported_formats() const override { return formats_; }

    Diagnostics check(const CompileRequest& req) override {
        CompileRequest r = req;
        r.formats = FORMAT_SPIRV;
        r.optimization = r.skip_optimizer_on_check ? 0 : r.optimization;
        return compile(r).diagnostics;
    }

    CompileResult compile(const CompileRequest& req) override;
    CompileResult translate(const CompileRequest& req,
                            const std::vector<std::uint8_t>& spirv) override;

private:
    /// Turns what the tool printed into diagnostics. On a failed run everything is
    /// kept, including the lines that carry no position, because one of them is
    /// usually the only explanation there is. On a successful run only warnings
    /// and notes are kept: the source excerpt DXC prints under a message would
    /// otherwise arrive as errors and fail a shader that in fact compiled.
    Diagnostics diagnose(const ProcessResult& run, const CompileRequest& req,
                         const std::filesystem::path& temp_source, bool failed) const;

    std::filesystem::path exe_;
    ToolOrigin origin_ = ToolOrigin::Path;
    std::uint32_t formats_ = FORMAT_NONE;
};

Diagnostics ShadercrossCliBackend::diagnose(const ProcessResult& run, const CompileRequest& req,
                                            const std::filesystem::path& temp_source,
                                            bool failed) const {
    // Stripped first so that the log stamp cannot hide the indentation that marks
    // a source excerpt, and so the raw fallback below reads as the compiler wrote
    // it rather than as the tool logged it.
    const std::string text = strip_log_prefixes(run.err + run.out);
    Diagnostics parsed = parse_dxc_output(text, req.path.string());

    // The tool compiles a copy of the buffer under a name of its own choosing, so
    // a position it reports against that name belongs to the file being edited.
    // A name it does not recognise is an #include and is left alone.
    const std::string temp_name = temp_source.filename().string();
    for (auto& d : parsed) {
        if (d.file == temp_name || d.file == temp_source.string() || d.file == "hlsl.hlsl") {
            d.file = req.path.string();
        }
    }

    if (!failed) {
        Diagnostics warnings;
        for (auto& d : parsed) {
            if (d.severity != Severity::Error) warnings.push_back(std::move(d));
        }
        return warnings;
    }

    if (parsed.empty()) {
        std::string detail = text;
        while (!detail.empty() && (detail.back() == '\n' || detail.back() == ' ')) detail.pop_back();
        if (detail.empty()) detail = "the shader compiler failed without saying why";
        parsed.push_back(make_diag(Severity::Error, detail, req.path.string(), 0, 0, "SSSTUDIO-TOOL"));
    }
    return parsed;
}

CompileResult ShadercrossCliBackend::compile(const CompileRequest& req) {
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();

    CompileResult result;
    TempDir temp;
    if (!temp.ok()) {
        result.diagnostics.push_back(make_diag(Severity::Error,
                                               "could not create a temporary directory for the "
                                               "shader compiler",
                                               req.path.string(), 0, 0, "SSSTUDIO-TOOL"));
        return result;
    }

    // The editor's buffer is compiled, not the file on disk, so it has to be
    // written out for the tool to read.
    const std::filesystem::path source = temp.path() / "shader.hlsl";
    {
        std::ofstream f(source, std::ios::binary | std::ios::trunc);
        if (!f) {
            result.diagnostics.push_back(
                make_diag(Severity::Error, "could not write " + source.string(),
                          req.path.string(), 0, 0, "SSSTUDIO-TOOL"));
            return result;
        }
        f.write(req.source.data(), static_cast<std::streamsize>(req.source.size()));
    }

    const std::filesystem::path spirv_path = temp.path() / "shader.spv";
    std::vector<std::string> args = {
        source.string(),   "-s", "HLSL", "-d", "SPIRV", "-t", stage_argument(req.stage),
        "-e", req.entry_point, "-o", spirv_path.string()};
    // The tool takes a single include directory, the same as the library entry
    // point does, so the request's first one wins exactly as it does there.
    if (!req.include_dirs.empty()) {
        args.push_back("-I");
        args.push_back(req.include_dirs.front().string());
    }
    for (const auto& [key, value] : req.defines) {
        args.push_back(value.empty() ? "-D" + key : "-D" + key + "=" + value);
    }
    std::vector<std::string> args_with_debug = args;
    if (req.debug_info) args_with_debug.push_back("-g");

    ProcessResult run = run_process(exe_, req.debug_info ? args_with_debug : args);

    // Debug info is a convenience, and it must never be the reason a shader will
    // not compile at all. When the legalizer is what gave up, the same shader is
    // tried once more without it; succeeding that way is far better than telling
    // someone their shader is broken when it is not.
    bool debug_info_dropped = false;
    if (req.debug_info && run.started() && !run.ok() &&
        looks_like_a_legalization_failure(run.err + run.out)) {
        const ProcessResult retry = run_process(exe_, args);
        if (retry.ok()) {
            run = retry;
            debug_info_dropped = true;
        }
    }

    if (!run.started()) {
        result.diagnostics.push_back(
            make_diag(Severity::Error,
                      run.error + ". Set Settings > Tools > shadercross dir to the directory "
                                  "holding the shadercross tool.",
                      req.path.string(), 0, 0, "SSSTUDIO-TOOL"));
        return result;
    }

    std::vector<std::uint8_t> spirv;
    const bool produced = run.ok() && read_binary_file(spirv_path, spirv) && !spirv.empty();
    result.diagnostics = diagnose(run, req, source, !produced);
    if (!produced) return result;

    if (debug_info_dropped) {
        result.diagnostics.push_back(
            make_diag(Severity::Info,
                      "compiled without debug info: the shader compiler cannot emit it for this "
                      "shader, and an array of textures is the usual reason",
                      req.path.string(), 0, 0, "SSSTUDIO-TOOL"));
    }

    // Reflection first: everything downstream (UI, packer, docs) uses it.
    result.reflection = reflect_spirv(spirv, req.stage, req.entry_point, result.diagnostics);
    result.reflection.entry_point = req.entry_point;

    if (req.formats & FORMAT_SPIRV) result.blobs[FORMAT_SPIRV] = spirv;

    CompileResult translated = translate(req, spirv);
    for (auto& [format, blob] : translated.blobs) result.blobs[format] = std::move(blob);
    result.diagnostics.insert(result.diagnostics.end(), translated.diagnostics.begin(),
                              translated.diagnostics.end());

    Diagnostics binding = result.reflection.validate_binding_model(req.path.string());
    result.diagnostics.insert(result.diagnostics.end(), binding.begin(), binding.end());

    if (req.warnings_as_errors) {
        for (auto& d : result.diagnostics)
            if (d.severity == Severity::Warning) d.severity = Severity::Error;
    }

    result.ok = !has_errors(result.diagnostics) && !result.blobs.empty();
    result.milliseconds = std::chrono::duration<double, std::milli>(clock::now() - start).count();
    return result;
}

CompileResult ShadercrossCliBackend::translate(const CompileRequest& req,
                                               const std::vector<std::uint8_t>& spirv) {
    CompileResult result;
    result.ok = true;

    const std::uint32_t wanted = req.formats & (FORMAT_DXIL | FORMAT_DXBC | FORMAT_MSL);
    if (wanted == 0) return result;

    TempDir temp;
    if (!temp.ok()) {
        result.diagnostics.push_back(make_diag(Severity::Error,
                                               "could not create a temporary directory for the "
                                               "shader compiler",
                                               req.path.string(), 0, 0, "SSSTUDIO-TOOL"));
        result.ok = false;
        return result;
    }

    const std::filesystem::path input = temp.path() / "shader.spv";
    {
        std::ofstream f(input, std::ios::binary | std::ios::trunc);
        if (!f) {
            result.diagnostics.push_back(
                make_diag(Severity::Error, "could not write " + input.string(),
                          req.path.string(), 0, 0, "SSSTUDIO-TRANSLATE"));
            result.ok = false;
            return result;
        }
        f.write(reinterpret_cast<const char*>(spirv.data()),
                static_cast<std::streamsize>(spirv.size()));
    }

    auto emit = [&](ShaderFormat fmt) {
        if (!(req.formats & fmt)) return;
        const char* format_name = nullptr;
        const char* extension = nullptr;
        if (!format_argument(fmt, format_name, extension)) return;

        const std::filesystem::path output = temp.path() / ("shader" + std::string(extension));
        std::vector<std::string> args = {input.string(),   "-s", "SPIRV",
                                         "-d",             format_name,
                                         "-t",             stage_argument(req.stage),
                                         "-e",             req.entry_point,
                                         "-o",             output.string()};
        // The tool's default cannot express an array of textures; see kMslVersion.
        if (fmt == FORMAT_MSL) {
            args.push_back("--msl-version");
            args.push_back(kMslVersion);
        }
        const ProcessResult run = run_process(exe_, args);

        std::vector<std::uint8_t> blob;
        if (run.ok() && read_binary_file(output, blob) && !blob.empty()) {
            // MSL is source, and every consumer of it expects a C string; the
            // library entry point returns one, so the file gets the same ending.
            if (fmt == FORMAT_MSL && blob.back() != '\0') blob.push_back('\0');
            result.blobs[fmt] = std::move(blob);
            return;
        }

        std::string why = strip_log_prefixes(run.started() ? run.err + run.out : run.error);
        while (!why.empty() && (why.back() == '\n' || why.back() == ' ')) why.pop_back();
        if (why.empty()) why = "the shader compiler produced nothing";
        result.diagnostics.push_back(
            make_diag(Severity::Error,
                      "could not produce " + std::string(to_string(fmt)) + ": " + why,
                      req.path.string(), 0, 0, "SSSTUDIO-TRANSLATE"));
    };

    emit(FORMAT_DXIL);
    emit(FORMAT_DXBC);
    emit(FORMAT_MSL);
    result.ok = !has_errors(result.diagnostics);
    return result;
}

/// The tool, and where it was found - which decides how it is named, not which
/// one is used.
struct FoundTool {
    std::filesystem::path exe;
    ToolOrigin origin = ToolOrigin::Path;
};

/// Finds the shadercross tool. The Settings > Tools override wins, then the
/// directory the build was configured against, then whatever is on PATH; each
/// candidate is tried both directly and under a bin subdirectory, which is how
/// the official builds are laid out.
FoundTool find_shadercross_cli(const std::filesystem::path& tool_dir) {
#if defined(_WIN32)
    const char* executable = "shadercross.exe";
#else
    const char* executable = "shadercross";
#endif

    std::vector<std::pair<std::filesystem::path, ToolOrigin>> roots;
    if (!tool_dir.empty()) roots.emplace_back(tool_dir, ToolOrigin::Configured);

    // The copy that ships with the application, before the one the build was
    // configured against: SSSTUDIO_SHADERCROSS_CLI_DIR is an absolute path on the
    // machine that did the building, which on anyone else's machine is either
    // missing or, worse, a different version that happens to be there.
    for (auto& root : bundled_tool_roots("shadercross")) {
        roots.emplace_back(std::move(root), ToolOrigin::Bundled);
    }

#if defined(SSSTUDIO_SHADERCROSS_CLI_DIR)
    roots.emplace_back(std::filesystem::path(SSSTUDIO_SHADERCROSS_CLI_DIR), ToolOrigin::Configured);
#endif

    // PATH last, so a packaged tool is found without configuration but never
    // shadows a directory the user or the build pointed at deliberately.
#if defined(_WIN32)
    constexpr char kPathSeparator = ';';
#else
    constexpr char kPathSeparator = ':';
#endif
    if (const char* path = std::getenv("PATH")) {
        std::string entry;
        std::istringstream is(path);
        while (std::getline(is, entry, kPathSeparator)) {
            if (!entry.empty()) roots.emplace_back(std::filesystem::path(entry), ToolOrigin::Path);
        }
    }

    for (const auto& [root, origin] : roots) {
        for (const auto& candidate : {root / executable, root / "bin" / executable}) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(candidate, ec)) return {candidate, origin};
        }
    }
    return {};
}

}  // namespace

std::uint64_t CompileRequest::cache_key() const {
    std::string material;
    material.reserve(source.size() + 128);
    material += source;
    material += "|";
    material += entry_point;
    material += "|";
    material += profile;
    material += "|";
    material += std::to_string(static_cast<int>(stage));
    material += std::to_string(static_cast<int>(language));
    material += std::to_string(formats);
    material += std::to_string(optimization);
    material += debug_info ? "d" : "-";
    // A check carries no artifacts, so its entry must never be handed to a
    // caller that asked for a real compile of the same source.
    material += check_only ? "c" : "-";
    for (const auto& [k, v] : defines) {
        material += "|" + k + "=" + v;
    }
    for (const auto& d : include_dirs) material += "|I" + d.string();
    return fnv1a64(material);
}

std::unique_ptr<ICompilerBackend> create_hlsl_backend(const std::filesystem::path& tool_dir) {
#if defined(SSSTUDIO_HAVE_SHADERCROSS)
    // The library is only linked by a build that shares its SDL3 with it, so when
    // it is there it is the cheaper of the two and there is nothing to gain from
    // spawning a process per compile.
    auto linked = std::make_unique<ShadercrossBackend>();
    if (linked->supported_formats() != FORMAT_NONE) return linked;
#endif
    // Either this build does not link SDL_shadercross, or the copy it linked has
    // no target it can produce - one built without DXC, say. The tool is the same
    // toolchain either way, so it is worth a look before giving up.
    if (FoundTool tool = find_shadercross_cli(tool_dir); !tool.exe.empty()) {
        return std::make_unique<ShadercrossCliBackend>(std::move(tool.exe), tool.origin);
    }
    return nullptr;
}

std::filesystem::path find_shadercross_tool(const std::filesystem::path& tool_dir) {
    return find_shadercross_cli(tool_dir).exe;
}

CompileResult translate_spirv(ICompilerBackend& translator, const CompileRequest& req,
                              const std::vector<std::uint8_t>& spirv) {
    if (auto* shadercross = dynamic_cast<ISpirvTranslator*>(&translator)) {
        return shadercross->translate(req, spirv);
    }
    CompileResult result;
    result.ok = true;
    return result;
}

std::unique_ptr<ICompilerBackend> create_default_backend(const std::filesystem::path& tool_dir) {
    auto hlsl = create_hlsl_backend(tool_dir);
    auto glsl = create_glslang_backend();

    if (!hlsl && !glsl) {
#if defined(SSSTUDIO_HAVE_SHADERCROSS)
        return std::make_unique<NullBackend>(
            "this build has no shader compiler; reconfigure with -DSSSTUDIO_WITH_SHADERCROSS=ON "
            "and/or -DSSSTUDIO_WITH_GLSLANG=ON");
#else
        return std::make_unique<NullBackend>(
            "the shadercross tool was not found on PATH or in Settings > Tools > shadercross "
            "dir, and this build has no GLSL front end");
#endif
    }
    return std::make_unique<CompositeBackend>(std::move(hlsl), std::move(glsl));
}

// ---------------------------------------------------------------------------
// CompilerService
// ---------------------------------------------------------------------------
struct CompilerService::Job {
    CompileRequest request;
    Callback callback;
    CompileResult result;
    std::chrono::steady_clock::time_point due;
    std::uint64_t serial = 0;
    bool started = false;
    bool finished = false;
    bool cancelled = false;
};

CompilerService::CompilerService(std::unique_ptr<ICompilerBackend> backend, int worker_count)
    : backend_(std::move(backend)) {
    if (worker_count < 1) worker_count = 1;
    for (int i = 0; i < worker_count; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
}

CompilerService::~CompilerService() {
    running_ = false;
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
}

void CompilerService::set_cache_dir(std::filesystem::path dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_dir_ = std::move(dir);
    if (!cache_dir_.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(cache_dir_, ec);
    }
}

void CompilerService::submit(CompileRequest req, int debounce_ms, Callback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Replace a queued-but-unstarted job for the same shader: typing quickly
    // must not pile up compiles.
    for (auto it = queue_.begin(); it != queue_.end();) {
        if ((*it)->request.id == req.id && (*it)->request.owner == req.owner &&
            !(*it)->started) {
            it = queue_.erase(it);
        } else {
            ++it;
        }
    }
    auto job = std::make_shared<Job>();
    job->request = std::move(req);
    job->callback = std::move(cb);
    job->serial = next_serial_++;
    job->due = std::chrono::steady_clock::now() + std::chrono::milliseconds(debounce_ms);
    queue_.push_back(std::move(job));
    cv_.notify_one();
}

void CompilerService::cancel(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& j : queue_) {
        if (j->request.id == id) j->cancelled = true;
    }
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                [&](const std::shared_ptr<Job>& j) {
                                    return j->cancelled && !j->started;
                                }),
                 queue_.end());
}

void CompilerService::cancel_owner(const std::string& owner) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& j : queue_) {
        if (j->request.owner == owner) j->cancelled = true;
    }
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                [](const std::shared_ptr<Job>& j) {
                                    return j->cancelled && !j->started;
                                }),
                 queue_.end());
}

void CompilerService::cancel_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& j : queue_) j->cancelled = true;
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                [](const std::shared_ptr<Job>& j) { return !j->started; }),
                 queue_.end());
}

std::size_t CompilerService::pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

void CompilerService::worker_loop() {
    while (running_) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(20), [this] {
                if (!running_) return true;
                const auto now = std::chrono::steady_clock::now();
                for (const auto& j : queue_) {
                    if (!j->started && !j->cancelled && j->due <= now) return true;
                }
                return false;
            });
            if (!running_) return;

            const auto now = std::chrono::steady_clock::now();
            for (auto& j : queue_) {
                if (!j->started && !j->cancelled && j->due <= now) {
                    j->started = true;
                    job = j;
                    break;
                }
            }
        }
        if (!job) continue;

        CompileResult result;
        if (job->request.check_only) {
            const auto started = std::chrono::steady_clock::now();
            result.diagnostics = backend_->check(job->request);
            result.ok = !has_errors(result.diagnostics);
            result.milliseconds =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                          started)
                    .count();
        } else {
            result = backend_->compile(job->request);
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            job->result = std::move(result);
            job->finished = true;
            done_.push_back(job);
            queue_.erase(std::remove(queue_.begin(), queue_.end(), job), queue_.end());
        }
    }
}

void CompilerService::poll() {
    std::vector<std::shared_ptr<Job>> ready;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ready.swap(done_);
    }
    for (auto& job : ready) {
        if (job->cancelled || !job->callback) continue;
        job->callback(job->request.id, std::move(job->result));
    }
}

}  // namespace ssstudio
