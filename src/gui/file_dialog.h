/// Native file and folder pickers, delivered back on the main thread.
#ifndef SSSTUDIO_GUI_FILE_DIALOG_H
#define SSSTUDIO_GUI_FILE_DIALOG_H

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

namespace ssstudio::gui {

/// One entry of a file dialog's type filter, e.g. {"Project manifest", "toml"}.
/// The pattern is SDL's form: extensions without a dot, separated by ';', or "*"
/// for everything.
struct FileFilter {
    std::string description;
    std::string pattern;
};

namespace detail {

/// The slice of a dialog's state that outlives the request. SDL may run the
/// completion callback on a thread of its own choosing, so the result lands here
/// under a lock and is picked up by the next FileDialog::poll().
struct DialogState {
    /// Guards every member below.
    mutable std::mutex mutex;
    /// True between showing a dialog and poll() consuming its result.
    bool open = false;
    /// True once SDL has called back, whether or not anything was chosen.
    bool done = false;
    /// What the user chose, empty if they cancelled or the dialog failed.
    std::filesystem::path path;
    /// SDL's error text when the dialog could not be shown at all.
    std::string error;
};

}  // namespace detail

/// Wraps SDL3's asynchronous dialogs so panel code can ask for a path with a
/// plain callback.
///
/// Only one dialog is allowed at a time: the platform pickers are modal for the
/// parent window, and a second request would leave the first callback with
/// nowhere to deliver its result.
class FileDialog {
public:
    /// Called on the main thread from poll() with the path the user chose. Not
    /// called at all when the dialog is cancelled.
    using Callback = std::function<void(const std::filesystem::path&)>;

    /// Called on the main thread from poll() when SDL could not show the dialog,
    /// with SDL's error text. Lets the caller offer a typed-path fallback.
    using ErrorCallback = std::function<void(const std::string&)>;

    FileDialog();

    /// True while a dialog is up and its result has not been consumed yet.
    bool busy() const;

    /// Picks an existing file. `start` may be empty, in which case the platform
    /// decides where to open.
    void open_file(SDL_Window* parent, const std::string& title,
                   const std::filesystem::path& start, const std::vector<FileFilter>& filters,
                   Callback on_pick);

    /// Picks a directory, existing or (on platforms that allow it) new.
    void open_folder(SDL_Window* parent, const std::string& title,
                     const std::filesystem::path& start, Callback on_pick);

    /// Installs the handler used when a dialog cannot be shown. Kept separate
    /// from the per-request callback because the remedy is always the same.
    void set_error_handler(ErrorCallback on_error) { on_error_ = std::move(on_error); }

    /// Runs the pending callback if the user has finished with the dialog.
    /// Call once per frame on the main thread.
    void poll();

private:
    /// Shared entry point for both dialog kinds.
    void show(SDL_FileDialogType type, SDL_Window* parent, const std::string& title,
              const std::filesystem::path& start, const std::vector<FileFilter>& filters,
              Callback on_pick);

    /// Alive for the whole life of the app, referenced weakly by in-flight
    /// requests so a late callback cannot write into a destroyed dialog.
    std::shared_ptr<detail::DialogState> state_;
    /// Main-thread only: what to run once the current request completes.
    Callback pending_;
    ErrorCallback on_error_;
};

}  // namespace ssstudio::gui

#endif  // SSSTUDIO_GUI_FILE_DIALOG_H
