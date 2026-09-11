// Running a command line tool and collecting what it printed.
//
// The shader toolchain is used out of process so that a prebuilt SDL_shadercross
// cannot drag a second copy of SDL3 into this one. That makes a small, blocking
// "run this and give me its output" helper part of the compiler's job.
#ifndef SSSTUDIO_PROCESS_H
#define SSSTUDIO_PROCESS_H

#include <filesystem>
#include <string>
#include <vector>

namespace ssstudio {

/// What a finished child process left behind.
struct ProcessResult {
    /// The child's exit status. Meaningless when the process never started, and
    /// set to -1 for a child killed by a signal.
    int exit_code = -1;
    std::string out;  ///< everything the child wrote to stdout
    std::string err;  ///< everything the child wrote to stderr
    /// Why the process could not be started at all, empty when it ran. A tool
    /// that is missing or not executable is reported here rather than through
    /// exit_code, so a caller can tell "no toolchain" from "the shader is wrong".
    std::string error;

    /// True when the child ran to completion, whatever it exited with.
    bool started() const { return error.empty(); }
    /// True when the child ran and reported success.
    bool ok() const { return started() && exit_code == 0; }
};

/// Runs `exe` with `args` and waits for it, capturing both output streams.
///
/// The arguments are passed to the child as they are given: no shell is
/// involved, so paths with spaces or quotes need no escaping. The child's stdin
/// is empty, so a tool that stops to ask a question sees end of input instead of
/// hanging the caller.
ProcessResult run_process(const std::filesystem::path& exe,
                          const std::vector<std::string>& args);

/// The directory this program's own executable is in, empty when the platform
/// will not say. Asked of the OS rather than derived from argv[0], which is
/// whatever the caller felt like passing.
///
/// Here rather than in the app because the core is what goes looking for tools,
/// and it has no SDL to ask - which is the whole reason it can be tested without
/// a window.
std::filesystem::path executable_directory();

/// Where a tool shipped alongside this program would be, best first. Empty when
/// the executable's own location is unknown.
///
/// Each entry is a root, so a tool laid out as `<root>/bin/tool` with its
/// libraries in `<root>/lib` is found without disturbing either - which matters,
/// because those libraries include an SDL3 that must never end up beside this
/// program's own.
std::vector<std::filesystem::path> bundled_tool_roots(const std::string& tool_name);

}  // namespace ssstudio

#endif  // SSSTUDIO_PROCESS_H
