#include "file_dialog.h"

#include <utility>

namespace ssstudio::gui {
namespace {

/// Everything SDL must keep reading until it invokes the callback. SDL documents
/// that the filter array has to stay valid that long, so the request owns it and
/// deletes itself from inside the callback.
struct Request {
    /// Weak on purpose: a dialog left open at shutdown must not resurrect the
    /// app's state, it must simply find it gone and drop the result.
    std::weak_ptr<detail::DialogState> state;
    /// Backing storage for the strings the SDL_DialogFileFilter entries point at.
    std::vector<std::string> filter_strings;
    /// The filter array handed to SDL, pointing into filter_strings.
    std::vector<SDL_DialogFileFilter> filters;
};

void SDLCALL on_dialog_finished(void* userdata, const char* const* filelist, int /*filter*/) {
    std::unique_ptr<Request> request(static_cast<Request*>(userdata));
    const std::shared_ptr<detail::DialogState> state = request->state.lock();
    if (!state) return;

    std::lock_guard<std::mutex> lock(state->mutex);
    if (filelist == nullptr) {
        // SDL reports a failure to show the dialog this way, not by cancelling.
        const char* error = SDL_GetError();
        state->error = (error != nullptr && *error != '\0') ? error : "the file dialog failed";
    } else if (*filelist != nullptr) {
        state->path = std::filesystem::path(*filelist);
    }
    state->done = true;
}

}  // namespace

FileDialog::FileDialog() : state_(std::make_shared<detail::DialogState>()) {}

bool FileDialog::busy() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->open;
}

void FileDialog::open_file(SDL_Window* parent, const std::string& title,
                           const std::filesystem::path& start,
                           const std::vector<FileFilter>& filters, Callback on_pick) {
    show(SDL_FILEDIALOG_OPENFILE, parent, title, start, filters, std::move(on_pick));
}

void FileDialog::open_folder(SDL_Window* parent, const std::string& title,
                             const std::filesystem::path& start, Callback on_pick) {
    show(SDL_FILEDIALOG_OPENFOLDER, parent, title, start, {}, std::move(on_pick));
}

void FileDialog::show(SDL_FileDialogType type, SDL_Window* parent, const std::string& title,
                      const std::filesystem::path& start, const std::vector<FileFilter>& filters,
                      Callback on_pick) {
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->open) return;
        state_->open = true;
        state_->done = false;
        state_->path.clear();
        state_->error.clear();
    }
    pending_ = std::move(on_pick);

    auto request = std::make_unique<Request>();
    request->state = state_;
    // Two strings per filter, reserved up front so the pointers taken below stay
    // valid as the vector grows.
    request->filter_strings.reserve(filters.size() * 2);
    for (const auto& filter : filters) {
        request->filter_strings.push_back(filter.description);
        request->filter_strings.push_back(filter.pattern);
    }
    for (std::size_t i = 0; i < filters.size(); ++i) {
        SDL_DialogFileFilter entry;
        entry.name = request->filter_strings[i * 2].c_str();
        entry.pattern = request->filter_strings[i * 2 + 1].c_str();
        request->filters.push_back(entry);
    }

    const SDL_PropertiesID props = SDL_CreateProperties();
    if (props == 0) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->error = SDL_GetError();
        state_->done = true;
        return;
    }
    if (!request->filters.empty()) {
        SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER,
                               request->filters.data());
        SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER,
                              static_cast<Sint64>(request->filters.size()));
    }
    if (parent != nullptr) SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, parent);
    if (!title.empty()) SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING,
                                              title.c_str());
    const std::string location = start.string();
    if (!location.empty()) {
        SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, location.c_str());
    }
    SDL_SetBooleanProperty(props, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN, false);

    // SDL owns the request from here: on_dialog_finished deletes it.
    SDL_ShowFileDialogWithProperties(type, on_dialog_finished, request.release(), props);
    SDL_DestroyProperties(props);
}

void FileDialog::poll() {
    std::filesystem::path picked;
    std::string error;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->open || !state_->done) return;
        picked = std::move(state_->path);
        error = std::move(state_->error);
        state_->path.clear();
        state_->error.clear();
        state_->open = false;
        state_->done = false;
    }

    // Moved out before running, so a callback that opens another dialog works.
    const Callback callback = std::move(pending_);
    pending_ = nullptr;

    if (!error.empty()) {
        if (on_error_) on_error_(error);
        return;
    }
    if (!picked.empty() && callback) callback(picked);
}

}  // namespace ssstudio::gui
