#include "fonts.h"

#include <system_error>

#include <imgui.h>

namespace ssstudio::gui {
namespace {

/// What mono_font() answers. One per process, like the ImGui context the
/// fonts belong to.
ImFont* g_mono_font = nullptr;

}  // namespace

ImFont* mono_font() { return g_mono_font; }

void FontLibrary::request(const FontChoice& choice) {
    wanted_ = choice;
    pending_ = true;
}

ImFont* FontLibrary::builtin(float size) {
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    if (builtin_ == nullptr) builtin_ = atlas->AddFontDefaultBitmap();
    // The size the pixel font was drawn for. Anything else would scale its
    // pixels, which is the blur the scalable version exists to avoid.
    constexpr float kBitmapSize = 13.0f;
    if (size <= 0.0f || size == kBitmapSize) return builtin_;
    if (builtin_scalable_ == nullptr) builtin_scalable_ = atlas->AddFontDefaultVector();
    return builtin_scalable_;
}

ImFont* FontLibrary::load(const std::filesystem::path& path, std::vector<std::string>& problems) {
    if (path.empty()) return nullptr;
    if (const auto it = loaded_.find(path); it != loaded_.end()) return it->second;

    // Checked before ImGui is handed the path: the atlas treats a file it cannot
    // open as a programming error and asserts, where here it is an ordinary
    // state - a pack whose fonts have not been dropped in yet.
    std::error_code ec;
    ImFont* font = nullptr;
    if (std::filesystem::is_regular_file(path, ec)) {
        font = ImGui::GetIO().Fonts->AddFontFromFileTTF(path.string().c_str());
    }
    if (font == nullptr) {
        problems.push_back("font " + path.filename().string() + " is not installed (looked in " +
                           path.parent_path().generic_string() + "); using the built-in font");
    }
    loaded_[path] = font;
    return font;
}

std::vector<std::string> FontLibrary::apply() {
    std::vector<std::string> problems;
    pending_ = false;

    // First, so it is the atlas's first font whatever else is loaded: ImGui
    // falls back to Fonts[0], and that should be a font that always exists.
    ImFont* fallback = builtin(wanted_.size);

    ImFont* ui = load(wanted_.ui, problems);
    ImFont* mono = load(wanted_.mono, problems);

    ui_ = ui != nullptr ? ui : fallback;
    mono_ = mono != nullptr ? mono : fallback;

    ImGui::GetIO().FontDefault = ui_;
    g_mono_font = mono_;
    return problems;
}

}  // namespace ssstudio::gui
