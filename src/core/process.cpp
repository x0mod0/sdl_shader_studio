#include "ssstudio/process.h"

#include <cstring>

#include <array>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

// _NSGetExecutablePath: the only way to ask macOS where this program is.
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace ssstudio {
namespace {

#if defined(_WIN32)

/// Closes a Windows handle once, and tolerates being asked twice. The read ends
/// of the pipes are closed early on the parent side, so a plain destructor is not
/// enough.
class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) : h_(h) {}
    ~Handle() { close(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    HANDLE get() const { return h_; }
    HANDLE* put() { return &h_; }
    void close() {
        if (h_ != nullptr && h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
        h_ = nullptr;
    }

private:
    HANDLE h_ = nullptr;
};

/// Quotes one argument the way the C runtime's command line parser expects to
/// read it back. Only backslashes that precede a quote are doubled, which is the
/// rule CommandLineToArgvW documents.
std::string quote_argument(const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) return arg;

    std::string out = "\"";
    for (auto it = arg.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != arg.end() && *it == '\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            out.append(backslashes * 2, '\\');
            break;
        }
        if (*it == '"') {
            out.append(backslashes * 2 + 1, '\\');
        } else {
            out.append(backslashes, '\\');
        }
        out.push_back(*it);
    }
    out.push_back('"');
    return out;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string last_error_text() {
    const DWORD code = GetLastError();
    char* buffer = nullptr;
    const DWORD n = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<char*>(&buffer), 0, nullptr);
    std::string message = n > 0 && buffer != nullptr ? std::string(buffer, n)
                                                     : "error " + std::to_string(code);
    if (buffer != nullptr) LocalFree(buffer);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
        message.pop_back();
    }
    return message;
}

void drain(HANDLE pipe, std::string& out) {
    std::array<char, 4096> buffer{};
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) ||
            read == 0) {
            return;
        }
        out.append(buffer.data(), read);
    }
}

#else

/// Owns one end of a pipe. The child's ends are closed in the parent as soon as
/// the process is spawned, otherwise the reads below would never see end of file.
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() { close(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    int get() const { return fd_; }
    void close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

private:
    int fd_ = -1;
};

void drain(int fd, std::string& out) {
    std::array<char, 4096> buffer{};
    for (;;) {
        const ssize_t n = ::read(fd, buffer.data(), buffer.size());
        if (n > 0) {
            out.append(buffer.data(), static_cast<std::size_t>(n));
        } else if (n == 0 || errno != EINTR) {
            return;
        }
    }
}

#endif

}  // namespace

#if defined(_WIN32)

ProcessResult run_process(const std::filesystem::path& exe,
                          const std::vector<std::string>& args) {
    ProcessResult result;

    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;

    Handle out_read, out_write, err_read, err_write;
    if (!CreatePipe(out_read.put(), out_write.put(), &inheritable, 0) ||
        !CreatePipe(err_read.put(), err_write.put(), &inheritable, 0)) {
        result.error = "could not create a pipe: " + last_error_text();
        return result;
    }
    // Only the child may inherit the writing ends; a handle it can read from
    // would keep the pipe open after it exits.
    SetHandleInformation(out_read.get(), HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_read.get(), HANDLE_FLAG_INHERIT, 0);

    Handle null_in(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &inheritable, OPEN_EXISTING, 0, nullptr));

    std::string command = quote_argument(exe.string());
    for (const auto& arg : args) command += " " + quote_argument(arg);
    std::wstring command_line = widen(command);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = null_in.get();
    startup.hStdOutput = out_write.get();
    startup.hStdError = err_write.get();

    PROCESS_INFORMATION process{};
    const std::wstring application = exe.wstring();
    if (!CreateProcessW(application.c_str(), command_line.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        result.error = "could not run " + exe.string() + ": " + last_error_text();
        return result;
    }

    // The parent's copies of the writing ends have to go before reading, or the
    // pipes never report end of file.
    out_write.close();
    err_write.close();

    std::thread err_reader([&] { drain(err_read.get(), result.err); });
    drain(out_read.get(), result.out);
    err_reader.join();

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD status = 0;
    GetExitCodeProcess(process.hProcess, &status);
    result.exit_code = static_cast<int>(status);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return result;
}

#else

ProcessResult run_process(const std::filesystem::path& exe,
                          const std::vector<std::string>& args) {
    ProcessResult result;

    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (pipe(out_pipe) != 0) {
        result.error = std::string("could not create a pipe: ") + std::strerror(errno);
        return result;
    }
    if (pipe(err_pipe) != 0) {
        result.error = std::string("could not create a pipe: ") + std::strerror(errno);
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        return result;
    }
    Fd out_read(out_pipe[0]), out_write(out_pipe[1]);
    Fd err_read(err_pipe[0]), err_write(err_pipe[1]);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, out_write.get(), STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, err_write.get(), STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, out_read.get());
    posix_spawn_file_actions_addclose(&actions, err_read.get());

    // posix_spawn wants a mutable argv, and the strings have to outlive the call.
    const std::string program = exe.string();
    std::vector<std::string> storage;
    storage.reserve(args.size() + 1);
    storage.push_back(program);
    for (const auto& arg : args) storage.push_back(arg);

    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (auto& s : storage) argv.push_back(s.data());
    argv.push_back(nullptr);

    pid_t pid = 0;
    const int spawned = posix_spawn(&pid, program.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0) {
        result.error = "could not run " + program + ": " + std::strerror(spawned);
        return result;
    }

    // The child owns the writing ends now; holding on to them here would keep
    // the reads below waiting forever after it exits.
    out_write.close();
    err_write.close();

    std::thread err_reader([&] { drain(err_read.get(), result.err); });
    drain(out_read.get(), result.out);
    err_reader.join();

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

#endif

std::filesystem::path executable_directory() {
    std::error_code ec;
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) return {};
        // The call truncates rather than failing, so a full buffer means "try a
        // bigger one" rather than "this is the answer".
        if (written < buffer.size()) {
            buffer.resize(written);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    return std::filesystem::path(buffer).parent_path();
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);  // asks for the length
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    buffer.resize(std::strlen(buffer.c_str()));
    // Resolved, because the path handed back may go through a symlink and the
    // libraries beside a tool are found relative to where it really is.
    const std::filesystem::path resolved = std::filesystem::weakly_canonical(buffer, ec);
    return (ec ? std::filesystem::path(buffer) : resolved).parent_path();
#else
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path{} : self.parent_path();
#endif
}

std::vector<std::filesystem::path> bundled_tool_roots(const std::string& tool_name) {
    const std::filesystem::path exe_dir = executable_directory();
    if (exe_dir.empty()) return {};

    std::vector<std::filesystem::path> roots;
    // A macOS app keeps everything that is not the executable under Resources,
    // and putting the tools there is also what keeps the SDL3 they ship out of
    // Contents/MacOS, where the app's own loader would find it first.
    roots.push_back(exe_dir / ".." / "Resources" / "tools" / tool_name);
    roots.push_back(exe_dir / "tools" / tool_name);
    // A plain build tree or an unpacked archive, where the tools sit beside the
    // program rather than inside a bundle.
    roots.push_back(exe_dir / ".." / "tools" / tool_name);

    std::vector<std::filesystem::path> existing;
    for (const auto& root : roots) {
        std::error_code ec;
        if (std::filesystem::is_directory(root, ec)) {
            std::filesystem::path normalized = std::filesystem::weakly_canonical(root, ec);
            existing.push_back(ec ? root : normalized);
        }
    }
    return existing;
}

}  // namespace ssstudio
