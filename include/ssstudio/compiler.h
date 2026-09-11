// Compilation front end.
//
// A backend turns source into SPIR-V plus diagnostics and reflection, and
// translates SPIR-V into the other SDL GPU formats. The SDL_shadercross/DXC
// backend is the default; the interface is language-agnostic so the glslang
// glslang front end plugs in without touching the packer, docs or UI.
#ifndef SSSTUDIO_COMPILER_H
#define SSSTUDIO_COMPILER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ssstudio/reflection.h"
#include "ssstudio/types.h"

namespace ssstudio {

struct CompileRequest {
    std::string id;             // shader id, used for cache keys and messages
    // Who asked. Two open projects can hold a shader with the same id, and the
    // queue's "one job per id in flight" rule would then let one project's edit
    // cancel the other's. The owner scopes that rule without reaching the cache
    // key, so the two still share a cached compile when the source matches.
    std::string owner;
    std::string source;         // full text (the editor buffer, not the file)
    std::filesystem::path path; // for diagnostics and #include resolution
    Stage stage = Stage::Fragment;
    Language language = Language::HLSL;
    std::string entry_point = "main";
    std::string profile;                     // e.g. "ps_6_0"; empty = default
    std::vector<std::pair<std::string, std::string>> defines;
    std::vector<std::filesystem::path> include_dirs;
    std::uint32_t formats = FORMAT_SPIRV;    // formats to emit
    int optimization = 3;                    // 0..3
    bool debug_info = false;
    bool warnings_as_errors = false;
    bool skip_optimizer_on_check = true;
    /// Diagnostics only: run the front end and skip code generation entirely.
    /// Much cheaper on a large shader, but it produces no SPIR-V, so nothing
    /// downstream of a compile - reflection, and the live preview - updates from
    /// a request that sets this. See EditorSettings::check_only_while_typing.
    bool check_only = false;

    std::uint64_t cache_key() const;
};

struct CompileResult {
    bool ok = false;
    Diagnostics diagnostics;
    Reflection reflection;
    std::map<ShaderFormat, std::vector<std::uint8_t>> blobs;
    double milliseconds = 0.0;
    bool from_cache = false;
};

class ICompilerBackend {
public:
    virtual ~ICompilerBackend() = default;
    virtual std::string name() const = 0;
    virtual bool supports(Language l) const = 0;
    virtual std::uint32_t supported_formats() const = 0;

    // Front end only: diagnostics, no artifacts. Used by compile-on-type.
    virtual Diagnostics check(const CompileRequest& req) = 0;

    // Full pipeline: source -> SPIR-V -> reflection -> requested formats.
    virtual CompileResult compile(const CompileRequest& req) = 0;
};

// Implemented by backends whose toolchain can also translate an existing SPIR-V
// blob into the other SDL GPU formats. It is a separate interface because a front
// end is not required to have one: glslang produces SPIR-V and nothing else, and
// borrows the HLSL backend's translator to ship DXIL and MSL alongside it.
class ISpirvTranslator {
public:
    virtual ~ISpirvTranslator() = default;
    virtual CompileResult translate(const CompileRequest& req,
                                    const std::vector<std::uint8_t>& spirv) = 0;
};

// Creates the best available backend. Returns a backend that reports a clear
// "no compiler available" diagnostic when the toolchain is missing, so the app
// still starts and the UI can point at Settings -> Tools.
std::unique_ptr<ICompilerBackend> create_default_backend(
    const std::filesystem::path& tool_dir = {});

// Individual front ends. Either may return null when the build excluded it.
//
// `tool_dir` is the Settings > Tools override for where the shadercross command
// line tool lives; it is ignored by a build that links SDL_shadercross as a
// library. An empty path means "look in the usual places".
std::unique_ptr<ICompilerBackend> create_hlsl_backend(const std::filesystem::path& tool_dir = {});
std::unique_ptr<ICompilerBackend> create_glslang_backend();

// Which shadercross executable the same search would settle on, empty when there
// is none. For reporting only - a backend from create_hlsl_backend() has already
// resolved its own, and names itself by where it came from rather than by path.
std::filesystem::path find_shadercross_tool(const std::filesystem::path& tool_dir = {});

// Translates an existing SPIR-V blob into the other formats a request asks for,
// using whichever backend owns the translator. Lets a GLSL shader ship DXIL and
// MSL exactly like an HLSL one.
CompileResult translate_spirv(ICompilerBackend& translator, const CompileRequest& req,
                              const std::vector<std::uint8_t>& spirv);

// Routes each request to the front end for its language, so one project can mix
// HLSL and GLSL shaders.
class CompositeBackend final : public ICompilerBackend {
public:
    CompositeBackend(std::unique_ptr<ICompilerBackend> hlsl,
                     std::unique_ptr<ICompilerBackend> glsl);

    std::string name() const override;
    bool supports(Language l) const override;
    std::uint32_t supported_formats() const override;
    Diagnostics check(const CompileRequest& req) override;
    CompileResult compile(const CompileRequest& req) override;

private:
    const ICompilerBackend* pick(Language l) const;
    ICompilerBackend* pick(Language l);
    Diagnostics missing(const CompileRequest& req) const;

    std::unique_ptr<ICompilerBackend> hlsl_;
    std::unique_ptr<ICompilerBackend> glsl_;
};

// ---------------------------------------------------------------------------
// Background service: debounced, cancellable, one job per shader id in flight.
// ---------------------------------------------------------------------------
class CompilerService {
public:
    using Callback = std::function<void(const std::string& id, CompileResult)>;

    CompilerService(std::unique_ptr<ICompilerBackend> backend, int worker_count = 2);
    ~CompilerService();

    CompilerService(const CompilerService&) = delete;
    CompilerService& operator=(const CompilerService&) = delete;

    // Replaces any queued-but-unstarted job with the same id. `debounce_ms` of 0
    // submits immediately (used by explicit builds); the editor passes its
    // configured debounce so typing does not spawn a compile per keystroke.
    void submit(CompileRequest req, int debounce_ms, Callback cb);

    // Drains completed jobs on the calling thread. The GUI calls this once per
    // frame so callbacks never touch UI state from a worker.
    void poll();

    void cancel(const std::string& id);
    // Drops every queued job submitted with this owner. Jobs already running
    // still finish and still call back; the caller is expected to recognise a
    // result for something it has closed and drop it.
    void cancel_owner(const std::string& owner);
    void cancel_all();
    std::size_t pending() const;

    ICompilerBackend& backend() { return *backend_; }

    // On-disk blob cache keyed by CompileRequest::cache_key().
    void set_cache_dir(std::filesystem::path dir);

private:
    struct Job;
    void worker_loop();

    std::unique_ptr<ICompilerBackend> backend_;
    std::filesystem::path cache_dir_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::shared_ptr<Job>> queue_;
    std::vector<std::shared_ptr<Job>> done_;
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{true};
    std::uint64_t next_serial_ = 1;
};

}  // namespace ssstudio

#endif  // SSSTUDIO_COMPILER_H
