#include "app.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <cfloat>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

#include <imgui.h>
#include <imgui_internal.h>  // DockBuilder

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include "panels/panel_common.h"
#include "tab_scope.h"
#include "widgets.h"
#include "preview/renderer.h"
#include "preview/texture_registry.h"
#include "scene/scene.h"
#include "ssstudio/diff.h"
#include "ssstudio/graph.h"
#include "ssstudio/templates.h"
#include "ssstudio/keys.h"
#include "ssstudio/pack_format.h"
#include "ssstudio/process.h"

namespace ssstudio::gui {


namespace {

// Two copies of SDL3 in one process is a silent failure, not a loud one: the app
// and the shader compiler each get their own SDL_GetError() slot, so a failed
// compile arrives with its message missing. Worth naming at startup, because the
// symptom ("compilation failed", no line number) points nowhere near the cause.
std::vector<std::string> duplicate_sdl_images() {
    std::vector<std::string> found;
#if defined(__APPLE__)
    const std::uint32_t count = _dyld_image_count();
    for (std::uint32_t i = 0; i < count; ++i) {
        const char* name = _dyld_get_image_name(i);
        if (name == nullptr) continue;
        const std::string path(name);
        if (path.find("libSDL3.") != std::string::npos ||
            path.find("libSDL3.0.dylib") != std::string::npos) {
            found.push_back(path);
        }
    }
#endif
    return found;
}

bool read_file(const std::filesystem::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream os;
    os << f.rdbuf();
    out = os.str();
    return true;
}

bool write_file(const std::filesystem::path& p, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return true;
}

std::filesystem::file_time_type mtime(const std::filesystem::path& p) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(p, ec);
    return ec ? std::filesystem::file_time_type{} : t;
}

/// What a shader id is allowed to look like, and why, in terms a form can show.
///
/// Ids become enumerator names in the generated header, so an id that is not a C
/// identifier produces a header that does not compile - a build failure a long
/// way from the field that caused it. Saying so here keeps it a typo rather than
/// a bug report.
bool valid_shader_id(const std::string& id, std::string& out_error) {
    if (id.empty()) {
        out_error = "Give the shader a name.";
        return false;
    }
    if (std::isdigit(static_cast<unsigned char>(id.front())) != 0) {
        out_error = "The name cannot start with a digit.";
        return false;
    }
    for (const char c : id) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_') continue;
        out_error = "The name can only hold letters, digits and underscores.";
        return false;
    }
    return true;
}

/// Whether a project-relative path is one a form may write to: inside the
/// project, and naming a file rather than a directory. A shader that lives
/// outside its project root cannot be packed by a build run elsewhere, and '..'
/// is the only way to spell that by accident.
bool valid_relative_path(const std::filesystem::path& relative, std::string& out_error) {
    if (relative.empty()) {
        out_error = "Give the file a name.";
        return false;
    }
    if (relative.is_absolute()) {
        out_error = "The file has to sit inside the project, so the path has to be a relative one.";
        return false;
    }
    if (relative.filename().empty()) {
        out_error = "That path names a directory rather than a file.";
        return false;
    }
    for (const auto& part : relative) {
        if (part != "..") continue;
        out_error = "The file has to sit inside the project, so the path cannot contain '..'.";
        return false;
    }
    return true;
}

/// One entry of the New shader form's Template combo.
struct TemplateOption {
    const char* label;
    ShaderTemplate value;
};

/// The starting points a stage offers, in the order the form lists them.
///
/// Every stage can start from its standard template or from nothing. Vertex has
/// a third: the screen-covering form, which has no counterpart in the other
/// stages because there is nothing for them to cover the screen with.
std::vector<TemplateOption> template_options(Stage stage) {
    std::vector<TemplateOption> options;
    options.push_back({"Standard - the stage template", ShaderTemplate::Standard});
    if (stage == Stage::Vertex) {
        options.push_back({"Triangle - covers the screen, no vertex buffer",
                           ShaderTemplate::FullscreenTriangle});
    }
    options.push_back({"None - empty file", ShaderTemplate::Empty});
    return options;
}

/// The display spelling of a language. to_string() is the manifest spelling and
/// is deliberately lower case; a form that used it would show
/// "Project default (hlsl)" beside an "HLSL" of its own.
const char* language_label(Language language) {
    return language == Language::GLSL ? "GLSL" : "HLSL";
}

/// The stage a New shader form's combo index means. The combo lists the stages
/// in pipeline order, which is not the order the enum declares them in, so the
/// two are mapped rather than cast.
Stage stage_from_index(int index) {
    switch (index) {
        case 0: return Stage::Vertex;
        case 2: return Stage::Compute;
        default: return Stage::Fragment;
    }
}

/// The language a form's combo index means. Index 0 is "whatever the project
/// says", which is stored as an absent override rather than a copy of the
/// project's own setting - so changing the project default later still moves
/// the shader with it.
std::optional<Language> language_from_index(int index) {
    switch (index) {
        case 1: return Language::HLSL;
        case 2: return Language::GLSL;
        default: return std::nullopt;
    }
}

/// Where a shader called `id` would go if nobody said otherwise. Matches what
/// scaffold_project() writes, so a project stays consistent whether its shaders
/// arrived with it or were added later.
std::filesystem::path default_shader_path(const std::string& name, Stage stage,
                                          Language language) {
    return std::filesystem::path("shaders") /
           (shader_basename(name, stage, language) + default_extension(stage, language));
}

/// Moves one entry of a keyed container from `from` to `to`, leaving the
/// container alone when there is nothing under `from`. The rename has half a
/// dozen of these - bindings, provenance, graphs - and they all get it wrong the
/// same way if written out by hand.
template <typename Map>
void move_keyed_entry(Map& map, const std::string& from, const std::string& to) {
    if (from == to) return;
    const auto it = map.find(from);
    if (it == map.end()) return;
    auto value = std::move(it->second);
    map.erase(it);
    map[to] = std::move(value);
}

}  // namespace

App::App() = default;

App::~App() {
    if (build_thread_.joinable()) build_thread_.join();
}

bool App::init(SDL_Window* window, SDL_GPUDevice* device) {
    window_ = window;
    device_ = device;

    settings_path_ = AppSettings::default_path();
    Diagnostics diags;
    load_settings(settings_path_, settings_, diags);
    for (auto& d : diags) log(d.severity, d.format());

    reload_theme_packs();
    apply_current_theme();
    // Before the first NewFrame(), so the very first frame is drawn in the
    // theme's fonts rather than flashing the built-in one.
    before_frame();

    auto backend = create_default_backend(settings_.tools.shadercross_dir);
    backend_name_ = backend->name();
    compiler_ = std::make_unique<CompilerService>(std::move(backend),
                                                  settings_.tools.compile_threads);

    // A shared cache means two projects containing the same shader compile it
    // once between them; a project cache would miss that entirely.
    if (settings_.tools.shared_compile_cache) {
        const std::filesystem::path dir = settings_.tools.shared_cache_dir.empty()
                                              ? CompileCache::default_directory()
                                              : settings_.tools.shared_cache_dir;
        cache_.open(dir, settings_.tools.asset_cache_bytes);
        cache_.trim();
    }

    // Always opened, whatever the compile cache is set to: the two share a
    // parent directory and nothing else.
    assets_.open(AssetCache::default_directory(), settings_.tools.download_cache_bytes);
    assets_.set_tool_path(settings_.tools.curl_path);
    assets_.trim();

    textures_ = std::make_unique<TextureRegistry>();
    if (!textures_->init(device_)) {
        log(Severity::Warning,
            "texture support unavailable: " + textures_->last_error() +
                ". Texture bindings will read as solid white.");
    }

    preview_ = std::make_unique<PreviewRenderer>();
    if (!preview_->init(device_, textures_.get())) {
        log(Severity::Warning,
            "preview device init failed: " + preview_->last_error() +
                ". The editor and build still work; the preview panel will stay empty.");
    }

    // A picker that cannot be shown must not look like a click that did nothing.
    file_dialog_.set_error_handler([this](const std::string& message) {
        log(Severity::Error,
            "the system file dialog is unavailable (" + message +
                "). Type a path into File > New project instead, or use `ssstudio new <dir>`.");
    });

    register_actions();

    // Kept beside settings.toml so the two travel together; ImGui writes nothing
    // here on its own (io.IniFilename is null), only save_layout_now() does.
    layout_path_ = settings_path_.parent_path() / "layout.ini";

    // Where the user left the panels last time. Loaded before the first frame,
    // because a DockBuilder pass would otherwise run over the top of it; leaving
    // layout_pending_ set is what asks for the default arrangement instead, so a
    // first run - or a deleted layout file - opens on kDefaultLayout.
    std::error_code layout_ec;
    if (std::filesystem::exists(layout_path_, layout_ec)) {
        ImGui::LoadIniSettingsFromDisk(layout_path_.string().c_str());
        layout_pending_ = false;
        layout_initialized_ = true;

    }

    log(Severity::Info, "compiler backend: " + backend_name_);

    // Named rather than assumed. Which decoders a build ends up with depends on
    // what was found when it was configured, and a format quietly missing is
    // otherwise discovered only by whoever's image will not open.
    if (image_loading_available()) {
        log(Severity::Info, "image formats: " + supported_image_formats());
    } else {
        log(Severity::Warning,
            "this build has no image support; texture bindings will read as solid white");
    }

    if (const auto sdl_images = duplicate_sdl_images(); sdl_images.size() > 1) {
        std::string list;
        for (const auto& path : sdl_images) list += "\n  " + path;
        log(Severity::Warning,
            "two copies of SDL3 are loaded in this process, so the shader compiler's error "
            "messages arrive empty and diagnostics lose their file and line:" + list +
                "\nThis build links SDL_shadercross rather than running it. Reconfigure with "
                "-DSSSTUDIO_SHADERCROSS_MODE=cli to drive the shadercross tool out of process, "
                "which keeps this process to one SDL3.");
    }

    return true;
}

void App::shutdown() {
    if (build_thread_.joinable()) build_thread_.join();
    if (compiler_) compiler_->cancel_all();
    if (preview_) preview_->shutdown();
    if (textures_) textures_->shutdown();

    // A rearrangement made in the last seconds before quitting has not had time
    // to raise WantSaveIniSettings; the dirty timer is how ImGui says one is
    // still on its way, and this is the last chance to honour it.
    if (ImGui::GetCurrentContext() != nullptr &&
        (ImGui::GetIO().WantSaveIniSettings || GImGui->SettingsDirtyTimer > 0.0f)) {
        save_layout_now();
    }
    remember_open_projects();
    save_settings_now();
}

void App::save_settings_now() {
    Diagnostics diags;
    save_settings(settings_path_, settings_, diags);
    for (auto& d : diags) log(d.severity, d.format());
}

void App::remember_open_projects() {
    // Once quitting has begun, a close is part of shutting down rather than the
    // user putting a project away. The record has to keep what was open when
    // they asked to quit: drive_quit() closes exactly the projects with unsaved
    // work, so without this the one project most worth reopening would be the
    // one that never came back.
    if (quit_pending_ || quit_requested_) return;

    settings_.open_projects.clear();
    for (const auto& session : sessions_) {
        settings_.open_projects.push_back(session->project.manifest);
    }
    settings_.active_project = std::max(active_session_, 0);

    // The set of projects worth remembering anything about has just changed.
    forget_stale_editor_state();
}

void App::restore_session() {
    if (!settings_.ui.restore_session) return;

    // Copied first: open_project() rewrites settings_.open_projects through
    // remember_open_projects(), which would otherwise be the list being walked.
    const std::vector<std::filesystem::path> wanted = settings_.open_projects;
    const int wanted_active = settings_.active_project;
    if (wanted.empty()) return;

    std::vector<std::string> missing;
    for (const auto& manifest : wanted) {
        std::error_code ec;
        if (!std::filesystem::exists(manifest, ec)) {
            missing.push_back(manifest.generic_string());
            continue;
        }
        open_project(manifest);
    }

    // Reported once rather than per project, and as a warning: a project that
    // moved is not an error, but silently opening fewer tabs than last time
    // would leave the user wondering what happened to them.
    if (!missing.empty()) {
        std::string list;
        for (const auto& m : missing) list += "\n  " + m;
        log(Severity::Warning,
            "could not reopen " + std::to_string(missing.size()) +
                " project(s) from the last session; they have moved or been deleted:" + list);
    }

    // By position in the list that was actually opened. The index is only a
    // hint - projects that went missing shift everything after them - so it is
    // clamped rather than trusted.
    if (!sessions_.empty()) {
        activate_session(std::clamp(wanted_active, 0, static_cast<int>(sessions_.size()) - 1));
    }
}

void App::log(Severity severity, std::string message) {
    Diagnostic d;
    d.severity = severity;
    d.message = std::move(message);
    log_.push_back(std::move(d));
    while (log_.size() > 500) log_.pop_front();
}

void App::report_preview_status(const std::string& message) {
    if (message == reported_preview_error_) return;
    reported_preview_error_ = message;
    if (message.empty()) return;
    // Prefixed, because the Diagnostics panel mixes the app log with build
    // output and compiler errors, and "pipeline creation failed" on its own
    // does not say which of the three it came from.
    log(Severity::Error, "preview: " + message);
}

// ---------------------------------------------------------------------------
// Sessions
//
// One session per open project. Everything below that reads or writes project
// state does it through active(); the panels do the same through App's
// accessors, which is what keeps the whole UI on one project at a time.
// ---------------------------------------------------------------------------
bool ProjectSession::has_unsaved_changes() const {
    for (const auto& doc : documents) {
        if (doc.dirty) return true;
    }
    return scene_open && scene_dirty;
}

ProjectSession* App::active() {
    if (active_session_ < 0 || active_session_ >= static_cast<int>(sessions_.size())) return nullptr;
    return sessions_[static_cast<std::size_t>(active_session_)].get();
}

const ProjectSession* App::active() const {
    if (active_session_ < 0 || active_session_ >= static_cast<int>(sessions_.size())) return nullptr;
    return sessions_[static_cast<std::size_t>(active_session_)].get();
}

ProjectSession* App::find_session(const std::string& key) {
    for (auto& session : sessions_) {
        if (session->key == key) return session.get();
    }
    return nullptr;
}

int App::index_of_session(const std::string& key) const {
    for (std::size_t i = 0; i < sessions_.size(); ++i) {
        if (sessions_[i]->key == key) return static_cast<int>(i);
    }
    return -1;
}

const std::string& App::active_session_key() const {
    const ProjectSession* session = active();
    return session ? session->key : null_string_;
}

Project& App::project() {
    ProjectSession* session = active();
    return session ? session->project : null_project_;
}

std::vector<Document>& App::documents() {
    ProjectSession* session = active();
    return session ? session->documents : null_documents_;
}

void App::bind_session_to_shared_state() {
    ProjectSession* session = active();

    // One preview renderer serves every project, and its shaders are keyed by
    // shader id, so two projects would otherwise overwrite each other's slots.
    // Emptying it and refilling it from the active session's blobs costs one
    // shader upload per tab switch and no recompiles.
    if (preview_) {
        preview_->clear();
        if (session) {
            for (const auto& doc : session->documents) {
                if (doc.preview_blob.empty()) continue;
                preview_->set_shader(doc.id, doc.stage, doc.preview_blob, doc.reflection);
            }
            // Only when the session has a choice to restore. set_shader
            // auto-selects the first shader of each stage as it goes, and
            // asserting an empty choice over that would leave the renderer with
            // no pass at all until the Preview panel next draws.
            if (!session->preview_vertex.empty() || !session->preview_fragment.empty()) {
                preview_->set_active(session->preview_vertex, session->preview_fragment);
            }
        }
    }
    if (compiler_) {
        compiler_->set_cache_dir(session ? session->project.cache_dir()
                                         : std::filesystem::path());
    }
}

void App::activate_session(int index) {
    if (index < 0 || index >= static_cast<int>(sessions_.size())) return;
    if (index == active_session_) return;
    active_session_ = index;
    bind_session_to_shared_state();
    // Only the index changed, so this does not need a write of its own: the
    // record is refreshed here and lands with the next save, and shutdown always
    // performs one.
    remember_open_projects();
}

void App::cycle_session(int delta) {
    const int count = static_cast<int>(sessions_.size());
    if (count < 2 || active_session_ < 0) return;
    const int next = ((active_session_ + delta) % count + count) % count;
    activate_session(next);
    // The tab bar owns which tab is drawn selected, so it has to be told; simply
    // changing the index would leave the old tab highlighted.
    pending_activate_key_ = sessions_[static_cast<std::size_t>(next)]->key;
}

// ---------------------------------------------------------------------------
// Project lifecycle
// ---------------------------------------------------------------------------
bool App::open_project(const std::filesystem::path& path) {
    Project loaded;
    Diagnostics diags;
    if (!load_project(path, loaded, diags)) {
        for (auto& d : diags) log(d.severity, d.format());
        return false;
    }
    for (auto& d : diags) log(d.severity, d.format());

    // Already open: bring its tab forward rather than opening the same files
    // twice. Two buffers over one file on disk is a lost edit waiting to happen.
    for (std::size_t i = 0; i < sessions_.size(); ++i) {
        std::error_code ec;
        if (!std::filesystem::equivalent(sessions_[i]->project.manifest, loaded.manifest, ec)) {
            continue;
        }
        activate_session(static_cast<int>(i));
        pending_activate_key_ = sessions_[i]->key;
        log(Severity::Info, loaded.name + " is already open");
        return true;
    }

    auto owned = std::make_unique<ProjectSession>();
    ProjectSession& session = *owned;
    session.key = "s" + std::to_string(next_session_serial_++);
    session.project = std::move(loaded);

    // How the editor was left in this project last time. Absent for a project
    // opened for the first time, in which case every tab opens in project order.
    ProjectEditorState remembered;
    if (const auto it =
            settings_.project_editor_state.find(session.project.manifest.generic_string());
        it != settings_.project_editor_state.end()) {
        remembered = it->second;
    }
    const std::set<std::string> closed(remembered.closed_tabs.begin(),
                                       remembered.closed_tabs.end());

    for (const auto& shader : session.project.shaders) {
        Document doc;
        doc.id = shader.id;
        doc.path = session.project.absolute(shader.path);
        doc.stage = shader.stage;
        doc.open_in_editor = closed.count(shader.id) == 0;
        if (!read_file(doc.path, doc.text)) {
            log(Severity::Error, "cannot read " + doc.path.string());
            continue;
        }
        doc.disk_time = mtime(doc.path);
        doc.undo_snapshots.push_back(doc.text);
        session.documents.push_back(std::move(doc));
    }
    // Put the documents in the order the tabs were left in. The tab bar submits
    // them in this order, and ImGui appends tabs it has not seen before in the
    // order they are submitted, so this is what ends up on screen.
    {
        std::vector<std::string> present;
        present.reserve(session.documents.size());
        for (const Document& doc : session.documents) present.push_back(doc.id);
        session.tab_order = merge_tab_order(remembered.tab_order, present);

        std::map<std::string, std::size_t> rank;
        for (std::size_t i = 0; i < session.tab_order.size(); ++i) {
            rank.emplace(session.tab_order[i], i);
        }
        std::sort(session.documents.begin(), session.documents.end(),
                  [&rank](const Document& a, const Document& b) {
                      return rank.at(a.id) < rank.at(b.id);
                  });
        // tab_order is the *open* tabs, and closed ones have no place in it.
        session.tab_order.erase(
            std::remove_if(session.tab_order.begin(), session.tab_order.end(),
                           [&closed](const std::string& id) { return closed.count(id) != 0; }),
            session.tab_order.end());
    }

    // The shader the other panels look at has to be one with a tab.
    for (const Document& doc : session.documents) {
        if (!doc.open_in_editor) continue;
        session.active_id = doc.id;
        break;
    }

    sessions_.push_back(std::move(owned));
    // Active from here on, because loading the graphs regenerates sources and
    // compiles them, and both of those go through the active session.
    active_session_ = static_cast<int>(sessions_.size()) - 1;
    pending_activate_key_ = session.key;
    bind_session_to_shared_state();

    // Graphs generate their shader's source; load them before the first compile
    // so a generated file on disk is never stale relative to its graph.
    for (const auto& shader : session.project.shaders) {
        const std::filesystem::path graph_path =
            session.project.root / "graphs" / (shader.id + ".toml");
        std::error_code ec;
        if (!std::filesystem::exists(graph_path, ec)) continue;

        Graph graph;
        Diagnostics graph_diags;
        if (!load_graph(graph_path, graph, graph_diags)) {
            for (auto& d : graph_diags) log(d.severity, d.format());
            continue;
        }
        for (auto& d : graph_diags) log(d.severity, d.format());
        if (Document* doc = find_document(shader.id)) doc->generated_by_graph = !graph.detached;
        session.graphs.emplace(shader.id, std::move(graph));
        if (!session.graphs[shader.id].detached) on_graph_changed(shader.id);
    }

    compile_all();

    // Keep the recent list short and free of duplicates.
    auto& recent = settings_.recent_projects;
    recent.erase(std::remove(recent.begin(), recent.end(), session.project.manifest),
                 recent.end());
    recent.insert(recent.begin(), session.project.manifest);
    if (recent.size() > 10) recent.resize(10);
    remember_open_projects();
    save_settings_now();

    log(Severity::Info, "opened " + session.project.name + " (" +
                            std::to_string(session.documents.size()) + " shaders)");
    return true;
}

bool App::new_project(const std::filesystem::path& dir, const std::string& name,
                      Language language) {
    Project p;
    Diagnostics diags;
    if (!scaffold_project(dir, name, language, p, diags)) {
        for (auto& d : diags) log(d.severity, d.format());
        return false;
    }
    for (auto& d : diags) log(d.severity, d.format());
    return open_project(p.manifest);
}

std::filesystem::path App::shipped_themes_dir() const {
    const char* base = SDL_GetBasePath();
    if (base == nullptr) return {};
    // SDL hands this back with a trailing separator, which makes parent_path()
    // strip the empty component after it rather than the directory name - so
    // the sibling lookup below has to start from a path without one, or it asks
    // for Resources/Resources and finds nothing.
    std::filesystem::path executable(base);
    if (!executable.has_filename()) executable = executable.parent_path();

    std::error_code ec;
    const std::filesystem::path beside = executable / "themes";
    if (std::filesystem::is_directory(beside, ec)) return beside;
    // A macOS bundle puts data in Contents/Resources. SDL already reports that
    // directory for a bundled app, so this is the fallback for the layouts
    // where it reports Contents/MacOS instead.
    const std::filesystem::path resources = executable.parent_path() / "Resources" / "themes";
    if (std::filesystem::is_directory(resources, ec)) return resources;
    return {};
}

std::vector<std::filesystem::path> App::theme_roots() const {
    std::vector<std::filesystem::path> roots;
    if (const std::filesystem::path shipped = shipped_themes_dir(); !shipped.empty()) {
        roots.push_back(shipped);
    }
    if (!settings_path_.empty()) roots.push_back(settings_path_.parent_path() / "themes");
    // The open project last, so a project can carry a theme for the people
    // working on it and have it win over the one they installed themselves.
    if (active_session_ >= 0 && active_session_ < static_cast<int>(sessions_.size())) {
        const std::filesystem::path& manifest = sessions_[static_cast<std::size_t>(active_session_)]
                                                    ->project.manifest;
        if (!manifest.empty()) roots.push_back(manifest.parent_path() / "themes");
    }
    return roots;
}

std::filesystem::path App::user_themes_dir() const {
    if (settings_path_.empty()) return {};
    return settings_path_.parent_path() / "themes";
}

void App::reload_theme_packs() {
    theme_packs_.clear();
    theme_previews_.clear();
    // theme_pack_paths() hands back bundled, then user, then project, so
    // assigning in order is what makes a later root win an id collision.
    for (const auto& path : theme_pack_paths(theme_roots())) {
        ThemePack pack;
        Diagnostics diags;
        const bool ok = parse_theme_pack(path, pack, diags);
        for (const auto& d : diags) log(d.severity, d.format());
        if (ok) theme_packs_[pack.id] = std::move(pack);
    }
}

// A theme file that changed while the app was in the background, re-read when
// the user comes back to it.
//
// One stat() of one file, and only when the user has asked for it in Settings -
// the alternative shapes were a real filesystem watcher, which is three
// platform back ends for a cosmetic feature, and doing nothing at all, which
// means alt-tabbing and clicking "Reload themes" after every edit while writing
// a theme. The rule that keeps a half-saved file from being complained about
// repeatedly lives in ThemeWatch::changed().
//
// Only the active theme is watched. A pack being *added* is not noticed, which
// is the honest cost of stat-ing one file rather than scanning directories; the
// picker's Reload button is what finds those.
void App::window_focus_gained() {
    if (!settings_.editor.reload_theme_on_focus) return;
    if (!theme_watch_.changed()) return;

    const auto it = theme_packs_.find(settings_.editor.color_theme);
    if (it == theme_packs_.end()) return;

    ThemePack reread;
    Diagnostics diags;
    if (parse_theme_pack(it->second.file.empty() ? it->second.dir : it->second.file, reread,
                         diags)) {
        theme_packs_[reread.id] = std::move(reread);
        apply_current_theme();
        log(Severity::Info, "reloaded theme '" + settings_.editor.color_theme + "'");
    }
    // Reported either way: an edit that broke the pack is the one time a theme
    // author most wants to be told, and the theme in effect stays as it was.
    for (const auto& d : diags) log(d.severity, d.format());
}

void App::apply_current_theme() {
    const std::string& id = settings_.editor.color_theme;
    Diagnostics diags;

    if (theme_is_builtin(id)) {
        theme_ = builtin_resolved_theme(id, settings_.editor.syntax_theme);
    } else if (const auto it = theme_packs_.find(id); it != theme_packs_.end()) {
        ThemeResolveContext context;
        context.available = theme_packs_;
        context.fallback_syntax_palette = settings_.editor.syntax_theme;
        // resolve_theme() fills `theme_` with the base theme when it refuses a
        // pack, so there is nothing to check here: a cosmetic mistake costs the
        // theme, never the session.
        resolve_theme(it->second, context, theme_, diags);
    } else {
        diags.push_back({Severity::Warning, settings_path_.string(), 0, 0,
                         "theme '" + id + "' is not installed; using dark", {}});
        theme_ = builtin_resolved_theme("dark", settings_.editor.syntax_theme);
    }
    for (const auto& d : diags) log(d.severity, d.format());

    // The kinds nobody pinned follow the theme; the pinned ones are the user's
    // and stay exactly as they were. Syntax colours are stored as 0xRRGGBB and
    // resolved as 0xRRGGBBAA, and alpha is meaningless for drawn text.
    for (std::size_t i = 0; i < kTokenKindCount; ++i) {
        if (settings_.editor.syntax_pinned[i]) continue;
        settings_.editor.syntax_colors.rgb[i] = theme_.syntax[i] >> 8;
    }

    // Whatever the theme resolved from, that is the file to watch. A built-in
    // has none, which stops the watch rather than leaving it on the last pack.
    const auto watched = theme_packs_.find(theme_.id);
    theme_watch_.reset(watched == theme_packs_.end() ? std::filesystem::path{}
                                                     : watched->second.file);

    apply_theme(theme_, settings_.ui.ui_scale);
    request_fonts();
}

void App::request_fonts() {
    FontChoice choice;
    choice.ui = theme_.font_ui;
    // The user's own font outranks the theme's: a font is often an
    // accessibility choice, and a theme change must not undo one.
    choice.mono = settings_.editor.font_path.empty()
                      ? theme_.font_editor
                      : std::filesystem::path(settings_.editor.font_path);
    fonts_.request(choice);
}

void App::before_frame() {
    if (!fonts_.pending()) return;
    for (const std::string& problem : fonts_.apply()) log(Severity::Info, problem);
}

const std::vector<ResolvedTheme>& App::theme_previews() {
    if (!theme_previews_.empty()) return theme_previews_;

    ThemeResolveContext context;
    context.available = theme_packs_;
    context.fallback_syntax_palette = settings_.editor.syntax_theme;
    // Packs first, the way the design lists them: the themes someone installed
    // are the ones they are most likely to be choosing between.
    for (const auto& [id, pack] : theme_packs_) {
        ResolvedTheme resolved;
        Diagnostics ignored;  // reported when the pack is applied, not here
        if (!resolve_theme(pack, context, resolved, ignored)) {
            // Refused packs still get a card, showing the base they would fall
            // back to, under their own name so they can still be found.
            resolved.id = id;
            resolved.name = pack.name.empty() ? id : pack.name;
        }
        theme_previews_.push_back(std::move(resolved));
    }
    for (const char* id : {"dark", "light", "classic"}) {
        theme_previews_.push_back(builtin_resolved_theme(id, settings_.editor.syntax_theme));
    }
    return theme_previews_;
}

std::filesystem::path App::projects_dir() const {
    if (!settings_.files.projects_dir.empty()) return settings_.files.projects_dir;

    // The examples/ directory that ships with the application. SDL_GetBasePath
    // is where the executable lives - build/bin in a development build, and
    // Contents/MacOS or Contents/Resources inside a macOS bundle - so the search
    // has to climb a few levels rather than look in one place.
    if (const char* base = SDL_GetBasePath()) {
        if (std::filesystem::path examples = find_examples_dir(base); !examples.empty()) {
            return examples;
        }
    }
    // SDL calls this "a good place to save a user's projects", and it is the one
    // location that exists on every desktop this runs on.
    if (const char* documents = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS)) {
        return std::filesystem::path(documents);
    }
    if (const char* home = SDL_GetUserFolder(SDL_FOLDER_HOME)) return std::filesystem::path(home);
    std::error_code ec;
    return std::filesystem::current_path(ec);
}

std::filesystem::path App::default_browse_dir() const {
    // Opening is about finding something that already exists, so where the last
    // project was beats the configured root; that root is the fallback rather
    // than the first answer. Creating a project is the other way round, and
    // prompt_new_project() starts from projects_dir() directly.
    for (const auto& recent : settings_.recent_projects) {
        std::error_code ec;
        const std::filesystem::path parent = recent.parent_path().parent_path();
        if (!parent.empty() && std::filesystem::is_directory(parent, ec)) return parent;
    }
    return projects_dir();
}

void App::prompt_for_file(const std::string& title, const std::filesystem::path& start,
                          const std::vector<FileFilter>& filters,
                          std::function<void(const std::filesystem::path&)> on_pick) {
    file_dialog_.open_file(window_, title, start, filters, std::move(on_pick));
}

bool App::file_dialog_busy() const { return file_dialog_.busy(); }

void App::prompt_new_project() {
    if (new_project_location_[0] == '\0') {
        const std::string start = projects_dir().string();
        std::snprintf(new_project_location_, sizeof(new_project_location_), "%s", start.c_str());
    }
    new_project_error_.clear();
    new_project_modal_pending_ = true;
    new_project_modal_focus_ = true;
}

void App::prompt_open_project() {
    if (file_dialog_.busy()) return;
    const std::vector<FileFilter> filters = {
        {"SDL Shader Studio project (project.toml)", "toml"},
        {"All files", "*"},
    };
    file_dialog_.open_file(window_, "Open project", default_browse_dir(), filters,
                           [this](const std::filesystem::path& picked) { open_project(picked); });
}

void App::close_project() { request_close_session(active_session_); }

void App::request_quit() {
    // Quitting closes every project, so it has to ask the same question closing
    // one does - once per project that has something to lose. drive_quit() walks
    // them; with nothing unsaved anywhere it quits on the spot.
    quit_pending_ = true;
    drive_quit();
}

void App::drive_quit() {
    if (!quit_pending_) return;
    // A confirmation is on screen; wait for its answer before asking the next.
    if (!close_pending_key_.empty()) return;

    for (std::size_t i = 0; i < sessions_.size(); ++i) {
        if (!sessions_[i]->has_unsaved_changes()) continue;
        request_close_session(static_cast<int>(i));
        return;
    }
    quit_pending_ = false;
    quit_requested_ = true;
}

void App::request_close_session(int index) {
    if (index < 0 || index >= static_cast<int>(sessions_.size())) return;
    ProjectSession& session = *sessions_[static_cast<std::size_t>(index)];

    // Nothing to lose: close it now. Otherwise the modal asks, and the close
    // happens when it is answered - always for this one project, never for the
    // others that stay open around it.
    if (!session.has_unsaved_changes()) {
        close_session(index);
        return;
    }
    close_pending_key_ = session.key;
    close_modal_pending_ = true;
    // Show what is about to be lost while the question is on screen.
    activate_session(index);
    pending_activate_key_ = session.key;
}

void App::close_session(int index) {
    if (index < 0 || index >= static_cast<int>(sessions_.size())) return;
    ProjectSession& session = *sessions_[static_cast<std::size_t>(index)];

    // The build thread holds a pointer into this session, so it has to be done
    // with it before the session goes away.
    if (session.build_running && build_thread_.joinable()) {
        log(Severity::Info, "waiting for the build of " + session.project.name + " to finish");
        build_thread_.join();
        build_finished_ = false;
    }
    // Queued compiles for this project are pointless now. One already running
    // still calls back, and on_compile_finished drops a result whose session has
    // gone.
    if (compiler_) compiler_->cancel_owner(session.key);

    // The scene autosaves on close, and that has to happen while the session is
    // still the active one, because save_active_scene() reads through active().
    const int previous_active = active_session_;
    active_session_ = index;
    close_scene();
    active_session_ = previous_active;

    forget_editor_states(session.key);
    const std::string name = session.project.name;
    // Which project was on screen, by key: the indices are about to shift.
    const std::string was_active = active_session_ >= 0
                                       ? sessions_[static_cast<std::size_t>(active_session_)]->key
                                       : std::string();
    sessions_.erase(sessions_.begin() + index);

    // Keep the neighbour: closing a tab should land on the one next to it, the
    // way every other tabbed editor behaves.
    if (sessions_.empty()) {
        active_session_ = -1;
    } else if (active_session_ > index) {
        active_session_ -= 1;
    } else if (active_session_ == index) {
        active_session_ = std::min(index, static_cast<int>(sessions_.size()) - 1);
    }
    if (active_session_ >= 0) {
        pending_activate_key_ = sessions_[static_cast<std::size_t>(active_session_)]->key;
    }
    // Closing a tab that was not in front leaves the preview alone; only a
    // change of active project has to hand the renderer a different set of
    // shaders.
    if (active_session_key() != was_active) bind_session_to_shared_state();
    remember_open_projects();
    save_settings_now();
    log(Severity::Info, "closed " + name);
}

void App::clear_diagnostics() {
    log_.clear();
    // Forgotten as well, or a preview that is still failing would never say so
    // again: the next frame would compare against the message just cleared and
    // decide it had already been reported.
    reported_preview_error_.clear();
    if (ProjectSession* session = active()) {
        session->last_report.diagnostics.clear();
        // The editor's gutter marks and squiggles read the same vectors, so they
        // go quiet too until the shader is compiled again. That is the honest
        // result: what is being cleared is a report, and there is no report
        // until something makes a new one.
        for (auto& doc : session->documents) doc.diagnostics.clear();
    }
}

void App::forget_project(const std::filesystem::path& manifest) {
    auto& recent = settings_.recent_projects;
    const std::size_t before = recent.size();
    recent.erase(std::remove(recent.begin(), recent.end(), manifest), recent.end());
    if (recent.size() == before) return;

    // The same tidy-up the bulk version does: the editor state kept for a
    // project nothing lists any more has nothing left to be about.
    forget_stale_editor_state();
    save_settings_now();
}

void App::clear_recent_projects() {
    if (settings_.recent_projects.empty()) return;
    settings_.recent_projects.clear();
    // The same tidy-up the two erases around it do. Projects still open keep
    // their editor state: forget_stale_editor_state() reads open_projects too.
    forget_stale_editor_state();
    save_settings_now();
}

void App::forget_missing_projects() {
    auto& recent = settings_.recent_projects;
    recent.erase(std::remove_if(recent.begin(), recent.end(),
                                [](const std::filesystem::path& p) {
                                    std::error_code ec;
                                    return !std::filesystem::exists(p, ec);
                                }),
                 recent.end());
    forget_stale_editor_state();
    save_settings_now();
}

bool App::save_document(Document& doc) {
    if (!write_file(doc.path, doc.text)) {
        log(Severity::Error, "cannot write " + doc.path.string());
        return false;
    }
    doc.dirty = false;
    doc.disk_time = mtime(doc.path);
    if (settings_.editor.compile_on_save) schedule_compile(doc, true);
    return true;
}

bool App::save_all() {
    ProjectSession* session = active();
    if (!session) return true;

    bool ok = true;
    for (auto& doc : session->documents) {
        if (doc.dirty) ok = save_document(doc) && ok;
    }
    if (session->scene_open && session->scene_dirty) save_active_scene();
    Diagnostics diags;
    if (!save_project(session->project, diags)) ok = false;
    for (auto& d : diags) log(d.severity, d.format());
    return ok;
}

// ---------------------------------------------------------------------------
// Documents
// ---------------------------------------------------------------------------
Document* App::active_document() {
    ProjectSession* session = active();
    return session ? find_document(session->active_id) : nullptr;
}

Document* App::find_document(const std::string& id) {
    ProjectSession* session = active();
    if (!session) return nullptr;
    for (auto& d : session->documents) {
        if (d.id == id) return &d;
    }
    return nullptr;
}

void App::focus_document(const std::string& id) {
    if (ProjectSession* session = active()) session->active_id = id;
}

void App::reveal(const std::string& shader_id, int line) {
    ProjectSession* session = active();
    if (!session || !find_document(shader_id)) return;
    focus_document(shader_id);
    session->reveal_id = shader_id;
    session->reveal_line = line;
    show_editor = true;
}

const std::string& App::reveal_request_id() const {
    const ProjectSession* session = active();
    return session ? session->reveal_id : null_string_;
}

int App::reveal_request_line() const {
    const ProjectSession* session = active();
    return session ? session->reveal_line : 0;
}

bool App::preview_ready() const {
    const ProjectSession* session = active();
    if (!session) return false;
    bool vertex = false;
    bool fragment = false;
    for (const auto& doc : session->documents) {
        if (!doc.ever_compiled_ok) continue;
        if (doc.stage == Stage::Vertex) vertex = true;
        if (doc.stage == Stage::Fragment) fragment = true;
    }
    return vertex && fragment;
}

void App::clear_reveal_request() {
    if (ProjectSession* session = active()) {
        session->reveal_id.clear();
        session->reveal_line = 0;
    }
}

double App::preview_time() const {
    const ProjectSession* session = active();
    return session ? session->preview_time : 0.0;
}

double App::preview_delta() const {
    const ProjectSession* session = active();
    return session ? session->preview_delta : 0.0;
}

std::uint64_t App::preview_frame() const {
    const ProjectSession* session = active();
    return session ? session->preview_frame : 0;
}

const PreviewMouse& App::preview_mouse() const {
    static const PreviewMouse none;
    const ProjectSession* session = active();
    return session ? session->preview_mouse : none;
}

void App::set_preview_mouse(const PreviewMouse& mouse) {
    if (ProjectSession* session = active()) session->preview_mouse = mouse;
}

void App::reset_preview_time() {
    if (ProjectSession* session = active()) {
        session->preview_time = 0.0;
        session->preview_delta = 0.0;
        session->preview_frame = 0;
    }
    // A buffer is not cleared each frame, so without this a chain that has
    // settled on something stays settled however many times Restart is pressed.
    if (preview_) preview_->restart();
}

std::string& App::preview_vertex_choice() {
    ProjectSession* session = active();
    return session ? session->preview_vertex : null_string_;
}

std::string& App::preview_fragment_choice() {
    ProjectSession* session = active();
    return session ? session->preview_fragment : null_string_;
}

std::string& App::build_profile_choice() {
    ProjectSession* session = active();
    return session ? session->build_profile : null_string_;
}

std::string& App::graph_selected_node() {
    ProjectSession* session = active();
    return session ? session->graph_selected_node : null_string_;
}

std::string& App::scene_selected_entity() {
    ProjectSession* session = active();
    return session ? session->scene_selected_entity : null_string_;
}

void App::close_document(const std::string& id) {
    ProjectSession* session = active();
    if (!session) return;
    Document* doc = find_document(id);
    if (!doc || !doc->open_in_editor) return;
    doc->open_in_editor = false;

    // active_id names the shader the other panels are looking at, so it has to
    // move off a tab that is no longer on screen. The next open one takes over;
    // with none left it is cleared, and the panels say so rather than pointing
    // at something invisible.
    if (session->active_id == id) {
        session->active_id.clear();
        for (const Document& other : session->documents) {
            if (!other.open_in_editor) continue;
            session->active_id = other.id;
            break;
        }
    }
    remember_editor_state();
}

void App::open_document(const std::string& id) {
    Document* doc = find_document(id);
    if (!doc) return;
    doc->open_in_editor = true;
    // Bring the tab forward; the editor's tab bar watches for this.
    reveal(id, 0);
    remember_editor_state();
}

void App::remember_editor_state() {
    const ProjectSession* session = active();
    if (!session) return;
    const std::string key = session->project.manifest.generic_string();
    if (key.empty()) return;  // a project with no manifest has nowhere to be filed

    ProjectEditorState state;
    state.tab_order = session->tab_order;
    state.closed_tabs = closed_documents();
    if (state.empty()) {
        settings_.project_editor_state.erase(key);
    } else {
        settings_.project_editor_state[key] = std::move(state);
    }
    editor_state_dirty_ = true;
}

const std::vector<std::string>& App::tab_order() const {
    static const std::vector<std::string> none;
    const ProjectSession* session = active();
    return session ? session->tab_order : none;
}

void App::set_tab_order(std::vector<std::string> order) {
    ProjectSession* session = active();
    // Called every frame, so the common case - nothing moved - has to be free of
    // both the copy and the settings write.
    if (!session || session->tab_order == order) return;
    session->tab_order = std::move(order);
    remember_editor_state();
}

void App::forget_stale_editor_state() {
    std::set<std::string> reachable;
    for (const auto& p : settings_.recent_projects) reachable.insert(p.generic_string());
    for (const auto& p : settings_.open_projects) reachable.insert(p.generic_string());
    auto& state = settings_.project_editor_state;
    for (auto it = state.begin(); it != state.end();) {
        it = reachable.count(it->first) != 0 ? std::next(it) : state.erase(it);
    }
}

std::vector<std::string> App::closed_documents() const {
    std::vector<std::string> closed;
    const ProjectSession* session = active();
    if (!session) return closed;
    for (const Document& doc : session->documents) {
        if (!doc.open_in_editor) closed.push_back(doc.id);
    }
    return closed;
}

void App::mark_dirty(Document& doc) {
    doc.dirty = true;
    // Cheap snapshot history so a bad edit is recoverable even after a crash.
    if (doc.undo_snapshots.empty() || doc.undo_snapshots.back() != doc.text) {
        doc.undo_snapshots.push_back(doc.text);
        const std::size_t limit = static_cast<std::size_t>(std::max(1, settings_.editor.snapshot_history));
        if (doc.undo_snapshots.size() > limit) doc.undo_snapshots.erase(doc.undo_snapshots.begin());
    }
    if (settings_.editor.compile_on_type) schedule_compile(doc, false);
}

void App::add_shader(const std::filesystem::path& source, Stage stage, const std::string& id,
                     std::optional<Language> language) {
    ProjectSession* session = active();
    if (!session) return;
    if (session->project.find_shader(id)) {
        log(Severity::Error, "a shader with id '" + id + "' already exists");
        return;
    }
    ShaderDesc desc;
    desc.language = language;
    desc.id = id;
    desc.stage = stage;
    std::error_code ec;
    const auto rel = std::filesystem::relative(source, session->project.root, ec);
    desc.path = ec ? source : rel;
    session->project.shaders.push_back(desc);

    Document doc;
    doc.id = id;
    doc.path = session->project.absolute(desc.path);
    doc.stage = stage;
    doc.open_in_editor = true;
    read_file(doc.path, doc.text);
    doc.disk_time = mtime(doc.path);
    session->documents.push_back(std::move(doc));
    session->active_id = id;
    schedule_compile(session->documents.back(), true);

    Diagnostics diags;
    save_project(session->project, diags);
    for (auto& d : diags) log(d.severity, d.format());
}

void App::remove_shader(const std::string& id, bool delete_file) {
    ProjectSession* session = active();
    if (!session) return;

    // Resolved before the descriptor goes, because it is what says which file on
    // disk this shader was.
    const ShaderDesc* desc = session->project.find_shader(id);
    const std::filesystem::path source =
        desc ? session->project.absolute(desc->path) : std::filesystem::path();
    const std::filesystem::path graph_file = session->project.root / "graphs" / (id + ".toml");

    auto& shaders = session->project.shaders;
    shaders.erase(std::remove_if(shaders.begin(), shaders.end(),
                                 [&](const ShaderDesc& s) { return s.id == id; }),
                  shaders.end());
    session->project.bindings.erase(id);
    session->project.provenance.erase(id);
    session->graphs.erase(id);

    auto& docs = session->documents;
    docs.erase(std::remove_if(docs.begin(), docs.end(),
                              [&](const Document& d) { return d.id == id; }),
               docs.end());
    if (session->active_id == id) {
        session->active_id = docs.empty() ? std::string() : docs.front().id;
    }
    // The preview panel re-defaults a choice that names nothing in the project,
    // so clearing these is enough; it picks the next shader up on the same
    // frame. The saved pipelines are cleared here too - they are persisted, so a
    // name left pointing at a deleted shader would be written back to disk.
    if (session->preview_vertex == id) session->preview_vertex.clear();
    if (session->preview_fragment == id) session->preview_fragment.clear();
    for (auto& pipeline : session->project.pipelines) {
        if (pipeline.vertex == id) pipeline.vertex.clear();
        if (pipeline.fragment == id) pipeline.fragment.clear();
        // A pass naming a shader that is gone is dropped rather than blanked:
        // a chain with a hole in it is not a chain, and an empty entry would
        // only be a target nobody writes.
        auto& passes = pipeline.passes;
        passes.erase(std::remove_if(passes.begin(), passes.end(),
                                    [&id](const PassDesc& p) { return p.shader_id == id; }),
                     passes.end());
    }

    if (preview_) preview_->remove_shader(id);
    forget_editor_state(session->key, id);

    if (delete_file) {
        std::error_code ec;
        if (!source.empty()) {
            std::filesystem::remove(source, ec);
            if (ec) {
                log(Severity::Error,
                    "could not delete " + source.string() + ": " + ec.message());
            } else {
                log(Severity::Info, "deleted " + source.string());
            }
        }
        // The graph is this shader's and nothing else reads it, so it goes with
        // the source rather than being left behind to describe a missing file.
        std::filesystem::remove(graph_file, ec);
    }

    remember_editor_state();

    Diagnostics diags;
    save_project(session->project, diags);
    for (auto& d : diags) log(d.severity, d.format());
    log(Severity::Info, "removed shader '" + id + "' from the project");
}

// ---------------------------------------------------------------------------
// Authoring: creating, renaming and duplicating shaders
// ---------------------------------------------------------------------------
void App::prompt_new_shader() {
    if (!project_open()) return;
    // Reset rather than remember: this is a form someone runs several times in a
    // row, and the second shader is not usually a variation on the first.
    new_shader_id_[0] = '\0';
    // Fragment is the stage a request for "a new shader" nearly always means,
    // and the only one the preview can draw without a second file.
    new_shader_stage_ = 1;
    new_shader_language_ = 0;
    new_shader_template_ = ShaderTemplate::Standard;
    new_shader_path_follows_id_ = true;
    new_shader_error_.clear();
    sync_new_shader_path();
    new_shader_modal_pending_ = true;
    new_shader_modal_focus_ = true;
}

void App::sync_new_shader_path() {
    if (!new_shader_path_follows_id_) return;
    const Stage stage = stage_from_index(new_shader_stage_);
    const Language language = new_shader_language_ == 0
                                  ? project().default_language
                                  : (new_shader_language_ == 2 ? Language::GLSL : Language::HLSL);
    // An empty name still gets a plausible path, so the "Creates ..." line below
    // the form is never a bare directory with an extension stuck to it.
    const std::string name = new_shader_id_[0] == '\0' ? std::string("untitled") : new_shader_id_;
    const std::string relative = default_shader_path(name, stage, language).generic_string();
    std::snprintf(new_shader_path_, sizeof(new_shader_path_), "%s", relative.c_str());
}

bool App::create_shader(const std::string& id, Stage stage, std::optional<Language> language,
                        const std::filesystem::path& relative, ShaderTemplate which,
                        std::string& out_error) {
    ProjectSession* session = active();
    if (!session) {
        out_error = "No project is open.";
        return false;
    }
    if (!valid_shader_id(id, out_error)) return false;
    if (session->project.find_shader(id)) {
        out_error = "This project already has a shader called '" + id + "'.";
        return false;
    }
    if (!valid_relative_path(relative, out_error)) return false;

    const std::filesystem::path absolute = session->project.absolute(relative);
    std::error_code ec;
    if (std::filesystem::exists(absolute, ec)) {
        // Adopting a file that is already there is a reasonable thing to want,
        // but it is not what this form says it does - and overwriting somebody's
        // shader because two ids happened to spell one filename is not
        // recoverable.
        out_error = relative.generic_string() + " already exists. Choose another name or path.";
        return false;
    }

    const Language resolved = language.value_or(session->project.default_language);
    std::string source;
    if (which == ShaderTemplate::FullscreenTriangle && stage == Stage::Vertex) {
        source = fullscreen_vertex_source(resolved);
    } else if (which != ShaderTemplate::Empty) {
        // Every stage has a standard template, and it is also what the
        // fullscreen choice falls back to where there is no such form.
        source = default_source(stage, resolved);
    }
    // An empty file is written as one rather than refused: it is a deliberate
    // choice on the form, and the compile that follows says what it thinks.
    if (!write_file(absolute, source)) {
        out_error = "Could not write " + absolute.string() + ".";
        return false;
    }

    add_shader(absolute, stage, id, language);
    if (!session->project.find_shader(id)) {
        out_error = "Could not add the shader to the project. See the diagnostics panel.";
        return false;
    }
    // Bring the new tab to the front. reveal() is what the editor's tab bar
    // watches, and line 0 means "just show the file".
    reveal(id, 0);
    log(Severity::Info, "created shader '" + id + "' at " + relative.generic_string());
    return true;
}

bool App::rename_shader(const std::string& id, const std::string& new_id,
                        const std::filesystem::path& new_relative, std::string& out_error) {
    ProjectSession* session = active();
    if (!session) {
        out_error = "No project is open.";
        return false;
    }
    ShaderDesc* desc = session->project.find_shader(id);
    if (!desc) {
        out_error = "'" + id + "' is not a shader of this project.";
        return false;
    }
    if (!valid_shader_id(new_id, out_error)) return false;
    if (new_id != id && session->project.find_shader(new_id)) {
        out_error = "This project already has a shader called '" + new_id + "'.";
        return false;
    }
    if (!valid_relative_path(new_relative, out_error)) return false;

    Document* doc = find_document(id);
    const std::filesystem::path old_absolute = session->project.absolute(desc->path);
    const std::filesystem::path new_absolute = session->project.absolute(new_relative);
    const bool moving = old_absolute.lexically_normal() != new_absolute.lexically_normal();

    // Nothing to do, and saying so is better than rewriting the manifest and
    // recompiling to arrive back where we started. Reported as success so the
    // form closes on OK rather than refusing it.
    if (!moving && new_id == id) return true;

    // Everything that can fail happens first, so a refused rename leaves the
    // project exactly as it was rather than half moved.
    if (moving) {
        std::error_code ec;
        if (std::filesystem::exists(new_absolute, ec)) {
            out_error = new_relative.generic_string() + " already exists.";
            return false;
        }
        std::filesystem::create_directories(new_absolute.parent_path(), ec);
        if (std::filesystem::exists(old_absolute, ec)) {
            std::filesystem::rename(old_absolute, new_absolute, ec);
            if (ec) {
                out_error = "Could not move the file: " + ec.message();
                return false;
            }
        } else if (doc && !write_file(new_absolute, doc->text)) {
            // Nothing to move: the shader was never written out. The buffer is
            // the only copy, so it goes to the new path directly.
            out_error = "Could not write " + new_absolute.string() + ".";
            return false;
        }
    }

    // Past this line nothing may fail. The file has already moved, so every
    // reference to the old id has to follow it or the project is inconsistent.
    // The pinned key is deliberately left on the descriptor: a key is what a
    // built pack is addressed by, and renaming a shader is a source-level change
    // that must not break a binary already shipped.
    desc->id = new_id;
    desc->path = new_relative;

    move_keyed_entry(session->project.bindings, id, new_id);
    move_keyed_entry(session->project.provenance, id, new_id);
    move_keyed_entry(session->graphs, id, new_id);
    if (const auto it = session->graphs.find(new_id); it != session->graphs.end()) {
        it->second.shader_id = new_id;
    }

    if (id != new_id) {
        // The graph is stored beside the project under the shader's id, so it
        // has to be renamed too or the next open would load it for nobody.
        std::error_code ec;
        const std::filesystem::path graphs = session->project.root / "graphs";
        if (std::filesystem::exists(graphs / (id + ".toml"), ec)) {
            std::filesystem::rename(graphs / (id + ".toml"), graphs / (new_id + ".toml"), ec);
        }

        // A scene names its materials by shader id. Left alone, every entity
        // using this shader would quietly fall back to the default material.
        bool scene_touched = false;
        auto retarget = [&](std::string& material) {
            if (material != id) return;
            material = new_id;
            scene_touched = true;
        };
        for (auto& entity : session->scene.entities) {
            if (entity.sprite) retarget(entity.sprite->material);
            if (entity.mesh) retarget(entity.mesh->material);
            if (entity.post) retarget(entity.post->material);
            if (entity.tilemap) retarget(entity.tilemap->material);
        }
        if (scene_touched) mark_scene_dirty();
    }

    if (session->preview_vertex == id) session->preview_vertex = new_id;
    if (session->preview_fragment == id) session->preview_fragment = new_id;
    // Renaming carries the id everywhere it is used, and a saved pipeline names
    // it just as surely as a binding does.
    for (auto& pipeline : session->project.pipelines) {
        if (pipeline.vertex == id) pipeline.vertex = new_id;
        if (pipeline.fragment == id) pipeline.fragment = new_id;
        for (PassDesc& pass : pipeline.passes) {
            if (pass.shader_id == id) pass.shader_id = new_id;
        }
    }
    if (session->active_id == id) session->active_id = new_id;

    if (doc) {
        doc->id = new_id;
        doc->path = new_absolute;
        doc->disk_time = mtime(new_absolute);
    }
    if (preview_ && id != new_id) {
        // The renderer keys its compiled shaders by id as well. Handing back the
        // blob the document is already holding keeps the preview drawing across
        // the rename instead of blanking until the recompile below lands.
        preview_->remove_shader(id);
        if (doc && !doc->preview_blob.empty()) {
            preview_->set_shader(new_id, doc->stage, doc->preview_blob, doc->reflection);
        }
    }
    rename_editor_state(session->key, id, new_id);

    Diagnostics diags;
    save_project(session->project, diags);
    for (auto& d : diags) log(d.severity, d.format());

    // The diagnostics on screen still name the file it used to be, and the
    // compiler queue keys its jobs by id; recompiling under the new one is what
    // makes both agree again.
    if (doc) schedule_compile(*doc, true);
    reveal(new_id, 0);

    if (id != new_id && moving) {
        log(Severity::Info, "renamed shader '" + id + "' to '" + new_id + "' and moved it to " +
                                new_relative.generic_string());
    } else if (id != new_id) {
        log(Severity::Info, "renamed shader '" + id + "' to '" + new_id + "'");
    } else {
        log(Severity::Info, "moved shader '" + id + "' to " + new_relative.generic_string());
    }
    return true;
}

bool App::duplicate_shader(const std::string& id, std::string& out_error) {
    ProjectSession* session = active();
    if (!session) {
        out_error = "No project is open.";
        return false;
    }
    const ShaderDesc* desc = session->project.find_shader(id);
    const Document* doc = find_document(id);
    if (!desc || !doc) {
        out_error = "'" + id + "' is not a shader of this project.";
        return false;
    }

    // Read out everything needed up front: add_shader() appends to both the
    // shader list and the document list, and either may reallocate under these
    // pointers.
    const Stage stage = desc->stage;
    const std::optional<Language> language = desc->language;
    const Language language_of = session->project.language_of(*desc);
    const std::string text = doc->text;
    const std::filesystem::path directory = desc->path.parent_path();

    // Keep the original's extension rather than rebuilding it from the stage:
    // a shader whose file is named unconventionally still lands beside its
    // original, spelled the same way.
    const std::string filename = desc->path.filename().string();
    const std::size_t dot = filename.find('.');
    const std::string extension =
        dot == std::string::npos ? default_extension(stage, language_of) : filename.substr(dot);

    const std::string new_id = unique_shader_id(session->project, id + "_copy");
    std::filesystem::path relative;
    for (int attempt = 1;; ++attempt) {
        const std::string suffix = attempt == 1 ? std::string() : "_" + std::to_string(attempt);
        relative = directory / (shader_basename(new_id, stage, language_of) + suffix + extension);
        std::error_code ec;
        if (!std::filesystem::exists(session->project.absolute(relative), ec)) break;
        if (attempt >= 1000) {
            out_error = "Could not find an unused filename beside " + filename + ".";
            return false;
        }
    }

    // The editor buffer rather than what is on disk: a duplicate taken mid-edit
    // is the version on screen, which is the one the user is looking at.
    const std::filesystem::path absolute = session->project.absolute(relative);
    if (!write_file(absolute, text)) {
        out_error = "Could not write " + absolute.string() + ".";
        return false;
    }

    add_shader(absolute, stage, new_id, language);
    if (!session->project.find_shader(new_id)) {
        out_error = "Could not add the copy to the project. See the diagnostics panel.";
        return false;
    }

    // Bindings describe the shader's slots, and the copy declares exactly the
    // same ones, so carrying them over means the duplicate previews identically
    // instead of coming up unbound.
    if (const auto it = session->project.bindings.find(id);
        it != session->project.bindings.end()) {
        session->project.bindings[new_id] = it->second;
        Diagnostics diags;
        save_project(session->project, diags);
        for (auto& d : diags) log(d.severity, d.format());
    }

    reveal(new_id, 0);
    log(Severity::Info, "duplicated '" + id + "' as '" + new_id + "'");
    return true;
}

void App::prompt_rename_shader(const std::string& id) {
    ProjectSession* session = active();
    if (!session) return;
    const ShaderDesc* desc = session->project.find_shader(id);
    if (!desc) return;

    rename_shader_id_ = id;
    const std::string relative = desc->path.generic_string();
    std::snprintf(rename_shader_path_, sizeof(rename_shader_path_), "%s", relative.c_str());

    // The form edits the name, so it is seeded from the file rather than from
    // the id: the file is where the name was typed, and an id written before the
    // current convention would otherwise show up in a field that is not about
    // ids at all.
    const std::string filename = desc->path.filename().string();
    const std::size_t dot = filename.find('.');
    const std::string name = dot == std::string::npos
                                 ? shader_basename(id, desc->stage,
                                                   session->project.language_of(*desc))
                                 : filename.substr(0, dot);
    std::snprintf(rename_shader_name_, sizeof(rename_shader_name_), "%s", name.c_str());

    // The filename only follows the new name while it still matches the old one
    // - rename "plasma" and plasma.frag.hlsl should move with it, but a file
    // somebody deliberately called something else is not the form's to rename.
    const std::string conventional =
        name + default_extension(desc->stage, session->project.language_of(*desc));
    rename_path_follows_id_ = filename == conventional;

    rename_error_.clear();
    rename_modal_pending_ = true;
    rename_modal_focus_ = true;
}

void App::prompt_delete_shader(const std::string& id) {
    ProjectSession* session = active();
    if (!session || !session->project.find_shader(id)) return;
    delete_shader_id_ = id;
    delete_shader_file_ = false;
    delete_modal_pending_ = true;
}

// ---------------------------------------------------------------------------
// Compilation
// ---------------------------------------------------------------------------
void App::schedule_compile(Document& doc, bool immediate) {
    ProjectSession* session = active();
    if (!session) return;
    const ShaderDesc* desc = session->project.find_shader(doc.id);
    if (!desc) return;

    CompileRequest req;
    req.id = doc.id;
    // Scopes the queue's one-job-per-id rule to this project; see CompileRequest.
    req.owner = session->key;
    req.source = doc.text;
    req.path = doc.path;
    req.stage = doc.stage;
    req.language = session->project.language_of(*desc);
    req.entry_point = desc->entry_point.empty() ? "main" : desc->entry_point;
    req.profile = desc->profile;
    // SPIR-V is what reflection is read from, so it is always asked for. The
    // preview device is a second consumer and only a Vulkan one takes SPIR-V:
    // Metal wants MSL and D3D12 wants DXIL, so the format it named at init is
    // translated in the same compile rather than leaving the panel dark.
    // A keystroke compile can be asked for diagnostics alone. Only when the user
    // opted in: the preview and the I/O panel are both fed by the artifacts a
    // check does not produce, so this trades them away for speed.
    req.check_only = !immediate && settings_.editor.check_only_while_typing;
    req.formats = req.check_only ? FORMAT_NONE : FORMAT_SPIRV;
    if (!req.check_only && preview_ && preview_->ready()) {
        req.formats |= preview_->preview_format();
    }
    req.optimization = 0;
    req.debug_info = true;
    req.warnings_as_errors = settings_.editor.warnings_as_errors;
    req.defines = desc->defines;
    for (const auto& d : settings_.languages.include_dirs) req.include_dirs.push_back(d);
    req.include_dirs.push_back(doc.path.parent_path());

    doc.compiling = true;
    const int debounce = immediate ? 0 : settings_.editor.compile_debounce_ms;
    // The key, not the session pointer: a compile can outlive the project that
    // asked for it, and the lookup on the way back is what makes that harmless.
    const std::string key = session->key;
    compiler_->submit(std::move(req), debounce,
                      [this, key](const std::string& id, CompileResult r) {
                          on_compile_finished(key, id, std::move(r));
                      });
}

void App::compile_all() {
    ProjectSession* session = active();
    if (!session) return;
    for (auto& doc : session->documents) schedule_compile(doc, true);
}

void App::on_compile_finished(const std::string& session_key, const std::string& id,
                              CompileResult result) {
    ProjectSession* session = find_session(session_key);
    if (!session) return;  // the project was closed while this compile ran

    Document* doc = nullptr;
    for (auto& d : session->documents) {
        if (d.id == id) {
            doc = &d;
            break;
        }
    }
    if (!doc) return;

    doc->compiling = false;
    doc->compiled_ok = result.ok;
    doc->ever_compiled_ok = doc->ever_compiled_ok || result.ok;
    doc->diagnostics = std::move(result.diagnostics);
    doc->last_compile_ms = result.milliseconds;

    // The local lints run on the buffer the compile was for, and append rather
    // than replace: they answer questions the compiler does not, so a shader
    // that compiled cleanly can still have something worth saying about it.
    if (const ShaderDesc* desc = session->project.find_shader(id)) {
        Diagnostics local = lint(doc->text, session->project.language_of(*desc), doc->stage);
        for (auto& d : local) d.file = doc->path.string();
        doc->diagnostics.insert(doc->diagnostics.end(), std::make_move_iterator(local.begin()),
                                std::make_move_iterator(local.end()));
    }

    if (result.ok) {
        doc->reflection = std::move(result.reflection);
        auto it = result.blobs.find(FORMAT_SPIRV);
        if (it != result.blobs.end()) doc->spirv = it->second;

        if (preview_ && preview_->ready()) {
            auto pit = result.blobs.find(preview_->preview_format());
            if (pit != result.blobs.end()) {
                doc->preview_blob = pit->second;
                // The renderer only ever holds the active project's shaders. A
                // background project's blob is kept on the document and handed
                // over when its tab comes forward.
                if (session == active()) {
                    preview_->set_shader(id, doc->stage, doc->preview_blob, doc->reflection);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------
void App::start_build(const std::string& profile_name, bool dry_run) {
    ProjectSession* session = active();
    if (build_busy_ || !session) return;
    if (build_thread_.joinable()) build_thread_.join();

    save_all();
    {
        std::lock_guard<std::mutex> lock(build_mutex_);
        session->build_progress.clear();
    }
    session->build_running = true;
    build_owner_key_ = session->key;
    build_busy_ = true;

    // The session is addressed directly rather than through active(): the user
    // is free to switch tabs while this runs, and the report belongs to the
    // project that was built. Sessions are held by pointer and close_session()
    // joins this thread before destroying one, so the pointer stays good.
    ProjectSession* owner = session;
    Project snapshot = session->project;
    build_thread_ = std::thread([this, owner, snapshot, profile_name, dry_run]() mutable {
        BuildOptions options;
        options.profile_name = profile_name;
        options.write_files = !dry_run;
        options.cache = settings_.tools.shared_compile_cache ? &cache_ : nullptr;
        options.compile_threads = settings_.tools.compile_threads;
        options.progress = [this, owner](const BuildProgress& p) {
            std::lock_guard<std::mutex> lock(build_mutex_);
            owner->build_progress.push_back(p);
        };
        auto backend = create_default_backend(settings_.tools.shadercross_dir);
        BuildReport report = build_project(snapshot, *backend, options);

        std::lock_guard<std::mutex> lock(build_mutex_);
        owner->last_report = std::move(report);
        // Keys assigned during the build belong in the live project too.
        for (const auto& s : owner->last_report.shaders) {
            if (ShaderDesc* desc = owner->project.find_shader(s.id)) {
                if (!desc->key) desc->key = s.key;
            }
        }
        owner->build_running = false;
        build_finished_ = true;
        build_busy_ = false;
    });
}

bool App::build_running() const {
    const ProjectSession* session = active();
    return session && session->build_running;
}

std::string App::building_project_name() const {
    if (!build_busy_) return {};
    for (const auto& session : sessions_) {
        if (session->build_running) return session->project.name;
    }
    return {};
}

const BuildReport& App::last_report() const {
    static const BuildReport empty;
    const ProjectSession* session = active();
    return session ? session->last_report : empty;
}

std::vector<BuildProgress> App::build_progress_snapshot() {
    std::lock_guard<std::mutex> lock(build_mutex_);
    const ProjectSession* session = active();
    return session ? session->build_progress : std::vector<BuildProgress>{};
}

// ---------------------------------------------------------------------------
// External changes and autosave
// ---------------------------------------------------------------------------
void App::poll_external_changes(float delta_seconds) {
    if (!settings_.files.watch_external_changes) return;
    watch_timer_ += delta_seconds;
    if (watch_timer_ < 1.0f) return;
    watch_timer_ = 0.0f;

    // Images are watched the same way sources are, so an export from another
    // program lands in the preview without anyone reopening anything.
    if (textures_ && textures_->ready()) {
        if (const std::size_t reloaded = textures_->refresh_changed(); reloaded > 0) {
            log(Severity::Info, std::to_string(reloaded) + " texture(s) reloaded from disk");
        }
    }

    ProjectSession* session = active();
    if (!session) return;

    // Only the project on screen. Watching every open project would reload a
    // buffer nobody is looking at, and the reload is meant to be something the
    // user sees happen.
    for (auto& doc : session->documents) {
        const auto t = mtime(doc.path);
        if (t == std::filesystem::file_time_type{} || t == doc.disk_time) continue;
        doc.disk_time = t;
        if (doc.dirty) {
            // Never clobber unsaved work; the editor shows a reload prompt.
            doc.external_change_pending = true;
        } else {
            std::string text;
            if (read_file(doc.path, text)) {
                doc.text = std::move(text);
                schedule_compile(doc, true);
                log(Severity::Info, doc.id + " reloaded from disk");
            }
        }
    }
}

void App::autosave(float delta_seconds) {
    if (settings_.files.autosave_interval_s <= 0) return;
    autosave_timer_ += delta_seconds;
    if (autosave_timer_ < static_cast<float>(settings_.files.autosave_interval_s)) return;
    autosave_timer_ = 0.0f;

    // Every open project, not just the active one: unsaved work in a background
    // tab is exactly the work an autosave exists to protect.
    for (const auto& session : sessions_) {
        for (const auto& doc : session->documents) {
            if (!doc.dirty) continue;
            const std::filesystem::path p = doc.path.string() + settings_.files.autosave_suffix;
            write_file(p, doc.text);
        }
    }
}

// ---------------------------------------------------------------------------
// Actions and shortcuts
// ---------------------------------------------------------------------------
void App::register_actions() {
    auto add = [this](std::string id, std::string label, std::string shortcut,
                      std::function<void()> run, std::function<bool()> enabled = nullptr) {
        actions_.push_back({std::move(id), std::move(label), std::move(shortcut), std::move(run),
                            std::move(enabled)});
    };
    auto has_project = [this] { return project_open(); };
    auto has_two_projects = [this] { return sessions_.size() > 1; };

    add("file.new", "New project...", "Ctrl+N", [this] { prompt_new_project(); });
    add("file.open", "Open project...", "Ctrl+O", [this] { prompt_open_project(); });
    add("file.save", "Save all", "Ctrl+S", [this] { save_all(); }, has_project);
    add("file.close", "Close project", "Ctrl+W", [this] { close_project(); }, has_project);
    // Not Ctrl+Tab: ImGui claims that one for its own window switcher whenever
    // keyboard navigation is enabled, which it is.
    add("file.next_project", "Next project", "Ctrl+PageDown", [this] { cycle_session(1); },
        has_two_projects);
    add("file.prev_project", "Previous project", "Ctrl+PageUp", [this] { cycle_session(-1); },
        has_two_projects);
    add("file.quit", "Quit", "Ctrl+Q", [this] { request_quit(); });

    auto can_build = [this] { return project_open() && !build_busy_; };
    add("build.run", "Build", "Ctrl+B",
        [this] { start_build(build_profile_choice(), false); }, can_build);
    add("build.dry", "Build (dry run)", "Ctrl+Shift+B",
        [this] { start_build(build_profile_choice(), true); }, can_build);
    add("build.compile_all", "Recompile all", "F7", [this] { compile_all(); }, has_project);

    add("shader.new", "New shader...", "Ctrl+Shift+N", [this] { prompt_new_shader(); },
        has_project);
    add("shader.close", "Close this shader tab", "", [this] {
        if (const Document* doc = active_document()) close_document(doc->id);
    }, has_project);
    add("shader.reopen_all", "Reopen every shader tab", "", [this] {
        for (const std::string& id : closed_documents()) open_document(id);
    }, [this] { return !closed_documents().empty(); });
    add("shader.rename", "Rename this shader...", "", [this] {
        if (const Document* doc = active_document()) prompt_rename_shader(doc->id);
    }, has_project);
    add("shader.duplicate", "Duplicate this shader", "", [this] {
        const Document* doc = active_document();
        if (!doc) return;
        // The palette has nowhere to put an error the way a form does, so a
        // failed duplicate says so in the diagnostics log instead.
        std::string error;
        if (!duplicate_shader(doc->id, error)) log(Severity::Error, error);
    }, has_project);
    add("shader.delete", "Delete this shader...", "", [this] {
        if (const Document* doc = active_document()) prompt_delete_shader(doc->id);
    }, has_project);
    add("file.import", "Import fullscreen shader...", "", [this] { prompt_import_shader(); },
        has_project);
    add("preview.reset", "Reset preview time", "F5", [this] { reset_preview_time(); });
    add("preview.pause", "Pause preview", "Space",
        [this] { project().preview.paused = !project().preview.paused; }, has_project);

    add("layout.write", "Layout: Write", "", [this] { apply_layout(LayoutPreset::Write); });
    add("layout.tune", "Layout: Tune", "", [this] { apply_layout(LayoutPreset::Tune); });
    add("layout.present", "Layout: Present", "", [this] { apply_layout(LayoutPreset::Present); });
    add("layout.build", "Layout: Build", "", [this] { apply_layout(LayoutPreset::Build); });
    add("layout.graph", "Layout: Graph", "", [this] { apply_layout(LayoutPreset::Graph); });
    add("layout.scene", "Layout: Scene", "", [this] { apply_layout(LayoutPreset::Scene); });
    add("layout.reset", "Layout: Reset", "", [this] { reset_layout(); });

    add("graph.toggle", "Toggle graph editor", "F2", [this] {
        // Pressing it on a panel that is open but buried should bring it back,
        // not close it: "I cannot see it" and "I am done with it" both look like
        // show_graph being true, and only one of them wants it hidden.
        if (show_graph && panel_is_focused("Graph")) {
            show_graph = false;
            return;
        }
        show_graph = true;
        request_panel_focus("Graph");
    });
    add("scene.toggle", "Toggle scene harness", "F3", [this] {
        // Its own window now, so it can be behind another one. Pressing this on
        // a scene that is open but buried should bring it back, not close it.
        if (show_scene && panel_is_focused("Scene")) {
            show_scene = false;
            return;
        }
        show_scene = true;
        request_panel_focus("Scene");
    });
    add("editor.pin", "Pin this version (A/B compare)", "F8", [this] {
        if (Document* doc = active_document()) pin_current_version(*doc);
    }, has_project);
    add("cache.clear", "Clear the compile cache", "", [this] {
        cache_.clear();
        log(Severity::Info, "compile cache cleared");
    });

    add("view.settings", "Settings", "Ctrl+,", [this] {
        show_settings = true;
        // Its own window, which means it can be behind another one. Asking for
        // it twice should bring it forward rather than do nothing visible.
        request_panel_focus("Settings");
    });
    add("view.palette", "Command palette", "Ctrl+P", [this] { open_command_palette(); });
}

void App::run_action(const std::string& id) {
    for (const auto& a : actions_) {
        if (a.id != id) continue;
        if (a.enabled && !a.enabled()) return;
        if (a.run) a.run();
        return;
    }
}

void App::handle_shortcuts() {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput && !io.KeyCtrl) return;

    const bool ctrl = io.KeyCtrl || io.KeySuper;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_N, false)) {
        // Shift is what tells the two apart, so it has to be tested here rather
        // than in a second `if` that the first one would already have consumed.
        run_action(io.KeyShift ? "shader.new" : "file.new");
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) run_action("file.open");
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) run_action("file.save");
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Q, false)) run_action("file.quit");
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_W, false)) run_action("file.close");
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_PageDown, false)) run_action("file.next_project");
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_PageUp, false)) run_action("file.prev_project");
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_P, false)) run_action("view.palette");
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Comma, false)) run_action("view.settings");
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_B, false)) {
        run_action(io.KeyShift ? "build.dry" : "build.run");
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) run_action("graph.toggle");
    if (ImGui::IsKeyPressed(ImGuiKey_F3, false)) run_action("scene.toggle");
    if (ImGui::IsKeyPressed(ImGuiKey_F7, false)) run_action("build.compile_all");
    if (ImGui::IsKeyPressed(ImGuiKey_F8, false)) run_action("editor.pin");
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) run_action("preview.reset");
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
void App::apply_layout(LayoutPreset preset) {
    // The rebuild happens in draw_dockspace, on the next frame. Rebuilding marks
    // ImGui's settings dirty like a drag does, so the preset is saved over the
    // layout file by itself and is what the next launch opens on.
    pending_layout_ = preset;
    layout_pending_ = true;
}

void App::reset_layout() {
    // No need to delete the layout file: the rebuild below is saved over it.
    apply_layout(kDefaultLayout);
    log(Severity::Info, "layout reset to the default arrangement");
}

void App::save_layout_now() {
    std::error_code ec;
    std::filesystem::create_directories(layout_path_.parent_path(), ec);
    ImGui::SaveIniSettingsToDisk(layout_path_.string().c_str());
    // Ours to clear, since ImGui only raises the flag when it has no ini file of
    // its own to write; leaving it set would save again every frame.
    ImGui::GetIO().WantSaveIniSettings = false;
}

void App::request_panel_focus(const char* name) {
    focus_panel_ = name;
    // Three frames: one for the panel to be drawn for the first time, and slack
    // for the frame a layout rebuild takes.
    focus_panel_frames_ = 3;
}

bool App::panel_is_focused(const char* name) const {
    const ImGuiWindow* window = ImGui::FindWindowByName(name);
    if (window == nullptr || !window->WasActive) return false;
    if (window->DockNode != nullptr) return window->DockNode->VisibleWindow == window;
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    return g.NavWindow != nullptr && g.NavWindow->RootWindow == window->RootWindow;
}

void App::apply_panel_focus() {
    if (focus_panel_.empty()) return;
    if (ImGui::FindWindowByName(focus_panel_.c_str()) != nullptr) {
        // Selects the tab when the panel is docked, and raises it when it is
        // floating. One call covers both because ImGui knows which it is.
        ImGui::SetWindowFocus(focus_panel_.c_str());
        focus_panel_.clear();
        focus_panel_frames_ = 0;
        return;
    }
    if (--focus_panel_frames_ <= 0) focus_panel_.clear();
}

void App::draw_dockspace() {
    const ThemeInk ink(theme_);

    // Both bars before the host. Each takes its height out of the viewport's
    // work area, and the host below is sized from what is left - so the menus,
    // the project pills and the status line can never be covered by a panel.
    draw_top_bar(ink);
    draw_status_bar(ink);

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    // The gutter: panels float a separator's width in from every edge, and the
    // host's own background is what shows between them. Painted in the darker
    // surface.base while the panels are surface.raised, which is what makes
    // them read as cards rather than as regions of one surface.
    const float gutter = ImGui::GetStyle().DockingSeparatorSize;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(gutter, gutter));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ink.base);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                             ImGuiWindowFlags_NoNavFocus;

    ImGui::Begin("##dockhost", nullptr, flags);
    ImGui::PopStyleVar(3);

    const ImGuiID dock_id = ImGui::GetID("MainDockspace");
    ImGui::DockSpace(dock_id, ImVec2(0, 0), ImGuiDockNodeFlags_PassthruCentralNode);
    // Only now: the splitters between docked panels are painted in whatever
    // the window background is while DockSpace() runs, and they are the gutter
    // too.
    ImGui::PopStyleColor();

    if (layout_pending_ || !layout_initialized_) {
        layout_pending_ = false;
        layout_initialized_ = true;

        ImGui::DockBuilderRemoveNode(dock_id);
        ImGui::DockBuilderAddNode(dock_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dock_id, viewport->WorkSize);

        // Every panel that can be docked is docked by every preset. The ones
        // absent from these lists - Graph, Scene, Settings - are windows of
        // their own and refuse docking, so naming them here would be asking for
        // something that cannot happen.
        ImGuiID center = dock_id;
        switch (pending_layout_) {
            case LayoutPreset::Write: {
                // Editor and preview share the centre as tabs, bindings beside
                // them, diagnostics underneath. Writing is one thing at a time:
                // the code fills the width, and the preview is a tab away rather
                // than a column taking a third of it permanently.
                ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.38f,
                                                            nullptr, &center);
                ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.25f,
                                                             nullptr, &center);
                // Editor first: the order these are docked in is the order the
                // tabs appear in, and this layout opens on the code.
                ImGui::DockBuilderDockWindow("Editor", center);
                ImGui::DockBuilderDockWindow("Preview", center);
                ImGui::DockBuilderDockWindow("Inputs & Outputs", right);
                ImGui::DockBuilderDockWindow("Diagnostics", bottom);
                ImGui::DockBuilderDockWindow("Build", bottom);
                break;
            }
            case LayoutPreset::Tune: {
                // Preview large, bindings prominent, editor tucked to one side.
                ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.28f,
                                                           nullptr, &center);
                ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.32f,
                                                            nullptr, &center);
                ImGui::DockBuilderDockWindow("Editor", left);
                ImGui::DockBuilderDockWindow("Preview", center);
                ImGui::DockBuilderDockWindow("Inputs & Outputs", right);
                ImGui::DockBuilderDockWindow("Diagnostics", right);
                ImGui::DockBuilderDockWindow("Build", right);
                break;
            }
            case LayoutPreset::Present: {
                // Preview only; everything else is available but hidden.
                ImGui::DockBuilderDockWindow("Preview", center);
                ImGui::DockBuilderDockWindow("Editor", center);
                ImGui::DockBuilderDockWindow("Inputs & Outputs", center);
                ImGui::DockBuilderDockWindow("Diagnostics", center);
                ImGui::DockBuilderDockWindow("Build", center);
                break;
            }
            case LayoutPreset::Graph: {
                ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.34f,
                                                            nullptr, &center);
                ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.22f,
                                                             nullptr, &center);
                ImGui::DockBuilderDockWindow("Editor", center);
                ImGui::DockBuilderDockWindow("Preview", right);
                ImGui::DockBuilderDockWindow("Inputs & Outputs", right);
                ImGui::DockBuilderDockWindow("Diagnostics", bottom);
                ImGui::DockBuilderDockWindow("Build", bottom);
                break;
            }
            case LayoutPreset::Scene: {
                ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.3f,
                                                           nullptr, &center);
                ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.24f,
                                                             nullptr, &center);
                ImGui::DockBuilderDockWindow("Editor", left);
                ImGui::DockBuilderDockWindow("Preview", center);
                ImGui::DockBuilderDockWindow("Inputs & Outputs", bottom);
                ImGui::DockBuilderDockWindow("Diagnostics", bottom);
                ImGui::DockBuilderDockWindow("Build", bottom);
                break;
            }
            case LayoutPreset::Build: {
                ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.45f,
                                                             nullptr, &center);
                ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.4f,
                                                            nullptr, &center);
                ImGui::DockBuilderDockWindow("Editor", center);
                ImGui::DockBuilderDockWindow("Preview", right);
                ImGui::DockBuilderDockWindow("Inputs & Outputs", right);
                ImGui::DockBuilderDockWindow("Build", bottom);
                ImGui::DockBuilderDockWindow("Diagnostics", bottom);
                break;
            }
        }
        ImGui::DockBuilderFinish(dock_id);
    }

    ImGui::End();
}

void App::draw_close_project_modal() {
    if (close_modal_pending_) {
        ImGui::OpenPopup("Close project");
        close_modal_pending_ = false;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    bool stay_open = true;
    if (!ImGui::BeginPopupModal("Close project", &stay_open,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        // Dismissed with Escape or the title bar's close button, which is a
        // cancel: the project stays open, and so does anything else a quit was
        // about to ask about.
        if (!ImGui::IsPopupOpen("Close project") && !close_pending_key_.empty()) {
            close_pending_key_.clear();
            quit_pending_ = false;
        }
        return;
    }

    const int index = index_of_session(close_pending_key_);
    if (index < 0) {
        // The session went away underneath the question; nothing left to ask.
        close_pending_key_.clear();
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const ProjectSession& session = *sessions_[static_cast<std::size_t>(index)];

    // Name the files rather than just their number: "3 unsaved files" is not
    // enough to decide with when one of them is the hour of work.
    std::string unsaved;
    int count = 0;
    for (const auto& doc : session.documents) {
        if (!doc.dirty) continue;
        ++count;
        if (count <= 6) unsaved += (unsaved.empty() ? "" : ", ") + doc.id;
    }
    if (count > 6) unsaved += ", and " + std::to_string(count - 6) + " more";
    if (session.scene_open && session.scene_dirty) {
        unsaved += (unsaved.empty() ? "" : ", ") + ("scene " + session.scene.name);
    }

    ImGui::Text("%s has unsaved changes.", session.project.name.c_str());
    ImGui::Spacing();
    ImGui::TextWrapped("%s", unsaved.c_str());
    ImGui::Spacing();
    ImGui::Separator();

    // Three buttons on one line while the dialog is wide enough, stacked when a
    // small font scale or a long project name has narrowed it.
    //
    // Cancel on the left and the safe, committing action on the right, as in
    // every other dialog here; Discard sits between them, away from both.
    const float wide = fitted_width(140.0f, 90.0f);
    const float narrow = fitted_width(120.0f, 80.0f);
    FlowLayout buttons;
    buttons.next(narrow);
    const bool cancel = ImGui::Button("Cancel", ImVec2(narrow, 0)) ||
                        ImGui::IsKeyPressed(ImGuiKey_Escape);
    buttons.next(narrow);
    const bool discard = ImGui::Button("Discard", ImVec2(narrow, 0));
    buttons.next(wide);
    const bool save = ImGui::Button("Save and close", ImVec2(wide, 0));

    if (save || discard) {
        if (save) {
            // save_all() writes through the active session, so the project being
            // closed has to be the one in front.
            const int previous = active_session_;
            active_session_ = index;
            const bool ok = save_all();
            active_session_ = previous;
            if (!ok) {
                // Nothing is closed on a failed save: the diagnostics say what
                // went wrong and the buffers are still there to retry from.
                log(Severity::Error,
                    "could not save " + session.project.name + "; it stays open");
                close_pending_key_.clear();
                quit_pending_ = false;
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                return;
            }
        } else {
            // Discarding the buffers has to stop the scene's own autosave-on-
            // close from writing them back out.
            sessions_[static_cast<std::size_t>(index)]->scene_dirty = false;
        }
        close_pending_key_.clear();
        close_session(index);
        ImGui::CloseCurrentPopup();
    } else if (cancel) {
        close_pending_key_.clear();
        // Cancelling one project's close cancels the whole quit: the user said
        // no to losing this work, not to being asked about the next project.
        quit_pending_ = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void App::draw_new_shader_modal() {
    if (new_shader_modal_pending_) {
        // Only on the frame the request arrives; see draw_new_project_modal().
        ImGui::OpenPopup("New shader");
        new_shader_modal_pending_ = false;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560.0f * ImGui::GetFontSize() / 15.0f, 0.0f));

    bool stay_open = true;
    if (!ImGui::BeginPopupModal("New shader", &stay_open, ImGuiWindowFlags_NoResize)) return;

    // The project can be closed from a shortcut while the form is up.
    if (!project_open()) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    ImGui::TextWrapped(
        "The file is written from the stage template, so the new shader compiles straight away "
        "and there is working code to edit rather than an empty buffer.");
    ImGui::Spacing();

    const ImGuiStyle& style = ImGui::GetStyle();
    bool submit_from_field = false;
    bool resync_path = false;

    if (ImGui::BeginTable("##new_shader_form", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::CalcTextSize("Language").x + style.ItemSpacing.x);
        ImGui::TableSetupColumn("##field", ImGuiTableColumnFlags_WidthStretch);

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Name");
        ImGui::TableNextColumn();
        if (new_shader_modal_focus_) {
            ImGui::SetKeyboardFocusHere();
            new_shader_modal_focus_ = false;
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        // No blanks: the name is the shader's id, and an id with a space in it
        // would be rejected on Create anyway.
        if (ImGui::InputText("##id", new_shader_id_, sizeof(new_shader_id_),
                             ImGuiInputTextFlags_EnterReturnsTrue |
                                 ImGuiInputTextFlags_CharsNoBlank)) {
            submit_from_field = true;
        }
        resync_path |= ImGui::IsItemEdited();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Letters, digits and underscores.\n"
                "The stage is added to make the shader's id, so one name serves every stage.");
        }

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Stage");
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(fitted_width(200.0f, 90.0f));
        const char* stages[] = {"Vertex", "Fragment", "Compute"};
        resync_path |= ImGui::Combo("##stage", &new_shader_stage_, stages, IM_ARRAYSIZE(stages));

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Language");
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(fitted_width(200.0f, 90.0f));
        // The project's own setting is named rather than left as "default", so
        // the choice can be made without going to look it up.
        const std::string inherited =
            "Project default (" + std::string(language_label(project().default_language)) + ")";
        const char* languages[] = {inherited.c_str(), "HLSL", "GLSL"};
        resync_path |=
            ImGui::Combo("##language", &new_shader_language_, languages, IM_ARRAYSIZE(languages));

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Template");
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(fitted_width(280.0f, 90.0f));
        {
            // The entries depend on the stage, so the selection is matched by
            // value rather than kept as an index: switching away from Vertex
            // must not leave "Triangle" selected under another name.
            const std::vector<TemplateOption> options =
                template_options(stage_from_index(new_shader_stage_));
            int choice = 0;
            for (std::size_t i = 0; i < options.size(); ++i) {
                if (options[i].value == new_shader_template_) choice = static_cast<int>(i);
            }
            // Falls back to the first entry - the stage template - when the
            // previous choice is not on offer for this stage.
            new_shader_template_ = options[static_cast<std::size_t>(choice)].value;

            std::vector<const char*> labels;
            labels.reserve(options.size());
            for (const auto& option : options) labels.push_back(option.label);
            if (ImGui::Combo("##template", &choice, labels.data(),
                             static_cast<int>(labels.size()))) {
                new_shader_template_ = options[static_cast<std::size_t>(choice)].value;
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Standard is the stage's own template: correct SDL register spaces, and it\n"
                "compiles as it stands. None writes an empty file for pasting into, which will\n"
                "not compile until there is something in it.");
        }

        // Before the field is drawn rather than after the table: the name, the
        // stage and the language are all above it, so the path they imply is
        // known by now and the field shows it on this frame instead of the next.
        if (resync_path) sync_new_shader_path();

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("File");
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputText("##path", new_shader_path_, sizeof(new_shader_path_),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            submit_from_field = true;
        }
        // Typing in the path field is what stops the name from rewriting it. A
        // path someone chose is not the form's to overwrite on the next
        // keystroke somewhere else.
        if (ImGui::IsItemEdited()) new_shader_path_follows_id_ = false;
        ImGui::EndTable();
    }

    // The id is derived rather than typed, so it is shown here. It is the one
    // place the id surfaces at all: everywhere else a shader goes by its name.
    const Stage stage = stage_from_index(new_shader_stage_);
    const Language resolved =
        language_from_index(new_shader_language_).value_or(project().default_language);
    const std::string id = shader_id_for(new_shader_id_, stage, resolved);

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("Creates %s",
                       project().absolute(new_shader_path_).generic_string().c_str());
    ImGui::TextWrapped("Shader id: %s", id.empty() ? "-" : id.c_str());
    ImGui::PopStyleColor();

    if (!new_shader_error_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme_vec4(theme_.diagnostic(Severity::Error)));
        ImGui::TextWrapped("%s", new_shader_error_.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Separator();
    // Cancel on the left, Create on the right: the platform's own order, and the
    // one that puts the committing button under the hand that is already there.
    const float action_width = fitted_width(120.0f, 80.0f);
    FlowLayout buttons;
    buttons.next(action_width);
    const bool cancel = ImGui::Button("Cancel", ImVec2(action_width, 0));
    buttons.next(action_width);
    const bool submit = ImGui::Button("Create", ImVec2(action_width, 0)) || submit_from_field;

    if (submit) {
        if (create_shader(id, stage, language_from_index(new_shader_language_), new_shader_path_,
                          new_shader_template_, new_shader_error_)) {
            new_shader_error_.clear();
            ImGui::CloseCurrentPopup();
        }
    }
    if (cancel) {
        new_shader_error_.clear();
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void App::draw_rename_shader_modal() {
    if (rename_modal_pending_) {
        ImGui::OpenPopup("Rename shader");
        rename_modal_pending_ = false;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560.0f * ImGui::GetFontSize() / 15.0f, 0.0f));

    bool stay_open = true;
    if (!ImGui::BeginPopupModal("Rename shader", &stay_open, ImGuiWindowFlags_NoResize)) {
        return;
    }

    // The shader can go while its form is up - another panel's delete, or the
    // project closing underneath it.
    const ShaderDesc* desc = project_open() ? project().find_shader(rename_shader_id_) : nullptr;
    if (!desc) {
        rename_shader_id_.clear();
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    const Stage stage = desc->stage;
    const Language language = project().language_of(*desc);

    ImGui::TextWrapped(
        "The name is what this shader is called in the editor and what its file is named. "
        "Renaming moves the file and updates everything that refers to the shader - bindings, "
        "scenes, its graph and the preview selection.");
    ImGui::Spacing();

    const ImGuiStyle& style = ImGui::GetStyle();
    bool submit_from_field = false;

    if (ImGui::BeginTable("##rename_shader_form", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::CalcTextSize("Language").x + style.ItemSpacing.x);
        ImGui::TableSetupColumn("##field", ImGuiTableColumnFlags_WidthStretch);

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Name");
        ImGui::TableNextColumn();
        if (rename_modal_focus_) {
            ImGui::SetKeyboardFocusHere();
            rename_modal_focus_ = false;
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputText("##new_name", rename_shader_name_, sizeof(rename_shader_name_),
                             ImGuiInputTextFlags_EnterReturnsTrue |
                                 ImGuiInputTextFlags_CharsNoBlank)) {
            submit_from_field = true;
        }
        if (ImGui::IsItemEdited() && rename_path_follows_id_) {
            // Only the filename moves; the directory the shader was filed under
            // is a decision of its own and stays put.
            const std::filesystem::path directory =
                std::filesystem::path(rename_shader_path_).parent_path();
            const std::string name =
                rename_shader_name_[0] == '\0' ? std::string("untitled") : rename_shader_name_;
            const std::string relative =
                (directory / (shader_basename(name, stage, language) +
                              default_extension(stage, language)))
                    .generic_string();
            std::snprintf(rename_shader_path_, sizeof(rename_shader_path_), "%s", relative.c_str());
        }

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("File");
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputText("##rename_path", rename_shader_path_, sizeof(rename_shader_path_),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            submit_from_field = true;
        }
        if (ImGui::IsItemEdited()) rename_path_follows_id_ = false;
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_TextDisabled]);
    if (desc->path.generic_string() !=
        std::filesystem::path(rename_shader_path_).generic_string()) {
        ImGui::TextWrapped("Moves %s to %s", desc->path.generic_string().c_str(),
                           std::filesystem::path(rename_shader_path_).generic_string().c_str());
    } else {
        ImGui::TextWrapped("The file stays at %s", desc->path.generic_string().c_str());
    }
    // The id changes as a consequence of the rename rather than being edited, so
    // it is spelled out here for the same reason the New shader form spells it
    // out: it is what the generated header enumerates.
    const std::string new_id = shader_id_for(rename_shader_name_, stage, language);
    ImGui::TextWrapped("Shader id: %s", new_id.empty() ? "-" : new_id.c_str());
    // Worth saying plainly: the one thing a rename is most likely to be feared
    // for is exactly the thing it does not touch.
    ImGui::TextWrapped(
        "The shader's key is pinned in project.toml and does not change, so a pack already "
        "shipped keeps loading.");
    ImGui::PopStyleColor();

    if (!rename_error_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme_vec4(theme_.diagnostic(Severity::Error)));
        ImGui::TextWrapped("%s", rename_error_.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Separator();
    const float action_width = fitted_width(120.0f, 80.0f);
    FlowLayout buttons;
    buttons.next(action_width);
    const bool cancel = ImGui::Button("Cancel", ImVec2(action_width, 0));
    buttons.next(action_width);
    const bool submit = ImGui::Button("Rename", ImVec2(action_width, 0)) || submit_from_field;

    if (submit) {
        if (rename_shader(rename_shader_id_, new_id, rename_shader_path_, rename_error_)) {
            rename_error_.clear();
            rename_shader_id_.clear();
            ImGui::CloseCurrentPopup();
        }
    }
    if (cancel) {
        rename_error_.clear();
        rename_shader_id_.clear();
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void App::draw_delete_shader_modal() {
    if (delete_modal_pending_) {
        ImGui::OpenPopup("Delete shader");
        delete_modal_pending_ = false;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    bool stay_open = true;
    if (!ImGui::BeginPopupModal("Delete shader", &stay_open, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    const ShaderDesc* desc = project_open() ? project().find_shader(delete_shader_id_) : nullptr;
    if (!desc) {
        delete_shader_id_.clear();
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    const std::filesystem::path relative = desc->path;
    const Document* doc = find_document(delete_shader_id_);
    const bool unsaved = doc != nullptr && doc->dirty;

    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
    ImGui::Text("Delete '%s'?", delete_shader_id_.c_str());
    ImGui::Spacing();
    ImGui::TextWrapped(
        "It is removed from the project and its editor tab closes. Its bindings and its graph go "
        "with it.");
    ImGui::Spacing();

    ImGui::Checkbox("Also delete the file", &delete_shader_file_);
    ImGui::Indent();
    ImGui::TextDisabled("%s", relative.generic_string().c_str());
    ImGui::Unindent();

    ImGui::Spacing();
    if (delete_shader_file_) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme_vec4(theme_.diagnostic(Severity::Warning)));
        ImGui::TextWrapped("The file is erased from disk. This cannot be undone.");
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("The file stays on disk and can be added back later.");
    }
    if (unsaved) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme_vec4(theme_.diagnostic(Severity::Warning)));
        ImGui::TextWrapped("This shader has unsaved edits, and they are lost either way.");
        ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();

    ImGui::Separator();
    const float action_width = fitted_width(120.0f, 80.0f);
    FlowLayout buttons;
    buttons.next(action_width);
    // Escape cancels as well, which BeginPopupModal handles on its own.
    const bool cancel = ImGui::Button("Cancel", ImVec2(action_width, 0));
    buttons.next(action_width);
    // The destructive button is coloured rather than merely labelled: it sits
    // where "Create" sits in every other dialog in the app. Mixed towards the
    // window rather than used at full strength, because the error colour is
    // tuned to be read as text on that background, and a button wants a fill
    // the label can still be read against.
    const Rgba danger = theme_.diagnostic(Severity::Error);
    const Rgba ground = theme_.role("surface.base");
    ImGui::PushStyleColor(ImGuiCol_Button, theme_vec4(color_mix(danger, ground, 0.45f)));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme_vec4(color_mix(danger, ground, 0.25f)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme_vec4(danger));
    const bool confirm = ImGui::Button("Delete", ImVec2(action_width, 0));
    ImGui::PopStyleColor(3);

    if (confirm) {
        remove_shader(delete_shader_id_, delete_shader_file_);
        delete_shader_id_.clear();
        ImGui::CloseCurrentPopup();
    }
    if (cancel) {
        delete_shader_id_.clear();
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void App::draw_menu_bar() {
    // Drawn into the top bar's menu bar, which draw_top_bar() has begun.
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New project...", "Ctrl+N")) prompt_new_project();
        if (ImGui::MenuItem("Open project...", "Ctrl+O")) prompt_open_project();
        if (ImGui::BeginMenu("Open recent", !settings_.recent_projects.empty())) {
            std::filesystem::path chosen;
            bool forget_missing = false;
            for (const auto& p : settings_.recent_projects) {
                // A project moved or deleted since last run still lists, greyed
                // out, so the entry explains itself rather than failing on click.
                std::error_code ec;
                const bool exists = std::filesystem::exists(p, ec);
                if (ImGui::MenuItem(p.string().c_str(), nullptr, false, exists)) chosen = p;
                if (!exists && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("missing on disk");
                }
                forget_missing = forget_missing || !exists;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Forget missing projects", nullptr, false, forget_missing)) {
                forget_missing_projects();
            }
            // Removing one entry is the landing screen's job - a menu that
            // closes on every click is a poor place to prune a list item by
            // item. What belongs here is the whole-list gesture.
            if (ImGui::MenuItem("Clear recent projects")) clear_recent_projects();
            ImGui::EndMenu();
            if (!chosen.empty()) open_project(chosen);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("New shader...", "Ctrl+Shift+N", false, project_open())) {
            prompt_new_shader();
        }
        // Reachable from here as well as from a tab's context menu, because
        // closing the last tab leaves nothing to right-click.
        const std::vector<std::string> closed = closed_documents();
        if (ImGui::BeginMenu("Open shader", !closed.empty())) {
            std::string chosen;
            bool open_every = false;
            for (const std::string& id : closed) {
                const Document* doc = find_document(id);
                if (!doc) continue;
                // Named by file rather than by id: the id is the manifest's
                // spelling and has no business in a menu of files to open.
                const std::string label = doc->path.filename().string() + (doc->dirty ? " *" : "");
                if (ImGui::MenuItem(label.c_str())) chosen = id;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Open all")) open_every = true;
            ImGui::EndMenu();
            // After EndMenu: opening one reveals it, which the menu that is
            // still being drawn has no business being in the middle of.
            if (!chosen.empty()) open_document(chosen);
            if (open_every) {
                for (const std::string& id : closed) open_document(id);
            }
        }
        if (ImGui::MenuItem("Import fullscreen shader...", nullptr, false, project_open())) {
            prompt_import_shader();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Save all", "Ctrl+S", false, project_open())) save_all();
        // Named, so it is clear this closes the one project in front and leaves
        // the other tabs alone.
        const std::string close_label =
            project_open() ? "Close " + project().name : std::string("Close project");
        if (ImGui::MenuItem(close_label.c_str(), "Ctrl+W", false, project_open())) {
            close_project();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Settings", "Ctrl+,", &show_settings) && show_settings) {
            request_panel_focus("Settings");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Ctrl+Q")) request_quit();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Build")) {
        const bool can = project_open() && !build_busy_;
        for (const auto& profile : project().profiles) {
            if (ImGui::MenuItem(profile.name.c_str(), nullptr, false, can)) {
                start_build(profile.name, false);
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Dry run", "Ctrl+Shift+B", false, can)) start_build(std::string(), true);
        if (ImGui::MenuItem("Recompile all", "F7", false, project_open())) compile_all();
        ImGui::EndMenu();
    }

    // One entry per open project, which is the keyboard-reachable half of the
    // tab bar and the place the switching shortcuts advertise themselves.
    if (ImGui::BeginMenu("Window")) {
        if (sessions_.empty()) {
            ImGui::MenuItem("(no project open)", nullptr, false, false);
        }
        int activate = -1;
        for (std::size_t i = 0; i < sessions_.size(); ++i) {
            const ProjectSession& session = *sessions_[i];
            std::string label = session.project.name;
            if (session.has_unsaved_changes()) label += " *";
            label += "###window_" + session.key;
            if (ImGui::MenuItem(label.c_str(), nullptr,
                                static_cast<int>(i) == active_session_)) {
                activate = static_cast<int>(i);
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Next project", "Ctrl+PageDown", false, sessions_.size() > 1)) {
            cycle_session(1);
        }
        if (ImGui::MenuItem("Previous project", "Ctrl+PageUp", false, sessions_.size() > 1)) {
            cycle_session(-1);
        }
        ImGui::EndMenu();
        if (activate >= 0) {
            activate_session(activate);
            pending_activate_key_ = sessions_[static_cast<std::size_t>(activate)]->key;
        }
    }

    if (ImGui::BeginMenu("View")) {
        // Picking a panel that is already on means "show me that one", so it is
        // raised rather than silently left wherever it was buried.
        const auto panel_item = [this](const char* label, const char* window, bool& shown) {
            if (!ImGui::MenuItem(label, nullptr, &shown)) return;
            if (shown) request_panel_focus(window);
        };
        panel_item("Editor", "Editor", show_editor);
        panel_item("Preview", "Preview", show_preview);
        panel_item("Inputs & Outputs", "Inputs & Outputs", show_io);
        panel_item("Diagnostics", "Diagnostics", show_diagnostics);
        panel_item("Build", "Build", show_build);
        panel_item("Graph", "Graph", show_graph);
        panel_item("Scene", "Scene", show_scene);
        ImGui::Separator();
        if (ImGui::MenuItem("Layout: Write")) apply_layout(LayoutPreset::Write);
        if (ImGui::MenuItem("Layout: Tune")) apply_layout(LayoutPreset::Tune);
        if (ImGui::MenuItem("Layout: Present")) apply_layout(LayoutPreset::Present);
        if (ImGui::MenuItem("Layout: Build")) apply_layout(LayoutPreset::Build);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset layout")) reset_layout();
        ImGui::EndMenu();
    }

    // Where the app describes itself rather than doing anything: both entries
    // answer a question ("which copy of this is running?", "where did it put my
    // settings?") and neither changes a thing, which is why they are here and
    // not in Settings.
    if (ImGui::BeginMenu("Info")) {
        if (ImGui::MenuItem("Configured paths...")) paths_modal_pending_ = true;
        if (ImGui::MenuItem("About app...")) about_modal_pending_ = true;
        ImGui::EndMenu();
    }

    // The backend and the build state that used to be right-aligned here are
    // in the status bar now (draw_status_bar), which has room for them.
}

// ---------------------------------------------------------------------------
// Info menu: the two dialogs that only describe things.
// ---------------------------------------------------------------------------

void App::draw_paths_modal() {
    if (paths_modal_pending_) {
        // Only on the frame the request arrives; see draw_new_project_modal().
        ImGui::OpenPopup("Configured paths");
        paths_modal_pending_ = false;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    // Wide, because these are absolute paths, and auto-height because how many
    // rows there are depends on what is open and what has been overridden.
    ImGui::SetNextWindowSize(ImVec2(760.0f * ImGui::GetFontSize() / 15.0f, 0.0f));

    bool stay_open = true;
    if (!ImGui::BeginPopupModal("Configured paths", &stay_open, ImGuiWindowFlags_NoResize)) {
        return;
    }
    // Paths are long and the columns are not: without this the ones that do not
    // fit are clipped at the window edge rather than folding onto a second line.
    // Pushed by hand rather than with TextWrapScope, because the wrap position
    // has to be popped before EndPopup() and a scope guard declared here would
    // outlive it.
    ImGui::PushTextWrapPos(0.0f);

    ImGui::TextWrapped(
        "Where this application reads and writes, resolved as it stands rather than as it is "
        "written down. Click any path to copy it; the ones that are settings are edited in "
        "Settings > Tools and Settings > Files.");
    ImGui::Spacing();

    // Sections are separate tables so each one's label column fits its own
    // labels, rather than every row in the dialog being as wide as the longest.
    bool open_table = false;
    const auto end_section = [&open_table] {
        if (open_table) ImGui::EndTable();
        open_table = false;
    };
    const auto section = [&](const char* title) {
        end_section();
        ImGui::SeparatorText(title);
        open_table = ImGui::BeginTable(title, 2, ImGuiTableFlags_SizingFixedFit);
        if (!open_table) return;
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::CalcTextSize("shadercross dir").x +
                                    ImGui::GetStyle().ItemSpacing.x * 2.0f);
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
    };

    /// One row. `instead` is what happens when the path is not set, which is the
    /// answer to the question that opened this dialog - a blank row would not be.
    const auto row = [&](const char* label, const std::filesystem::path& value,
                         const char* instead = nullptr) {
        if (!open_table) return;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(label);
        ImGui::TableNextColumn();
        if (value.empty()) {
            ImGui::TextDisabled("%s", instead ? instead : "not set");
        } else {
            // Undimmed: here the path is the content rather than an annotation
            // under something else.
            copyable_path(value.generic_string(), false);
        }
    };

    section("Application");
    row("Executable", executable_directory());
    row("Settings", settings_path_);
    row("Layout", layout_path_);
    row("Projects", projects_dir());

    section("Caches");
    row("Compiles", cache_.enabled() ? cache_.directory() : std::filesystem::path(),
        "off - Settings > Tools > Share the compile cache between projects");
    row("Downloads", assets_.directory());
    if (project_open()) row("This project", project().cache_dir());

    section("Tools");
    row("shadercross", find_shadercross_tool(settings_.tools.shadercross_dir),
        "not found - HLSL cannot be compiled");
    row("shadercross dir", settings_.tools.shadercross_dir,
        "not set - the bundled copy, then PATH");
    row("dxc", settings_.tools.dxc_path, "not set - the copy shadercross ships");
    row("glslang", settings_.tools.glslang_path, "not set - the one built in");
    row("xcrun", settings_.tools.xcrun_path, "not set - found on PATH");
    row("curl", settings_.tools.curl_path, "not set - found on PATH");

    if (project_open()) {
        section("Project");
        row("Root", project().root);
        row("Manifest", project().manifest);
        // One row per profile: they can be pointed at different directories, and
        // "where did my build go?" is one of the questions this dialog is for.
        for (const auto& profile : project().profiles) {
            const std::string label = "Output (" + profile.name + ")";
            row(label.c_str(), project().absolute(profile.output_dir));
        }
    }
    end_section();

    ImGui::Spacing();
    ImGui::Separator();
    const float action_width = fitted_width(120.0f, 80.0f);
    same_line_right_aligned(action_width);
    if (ImGui::Button("Close", ImVec2(action_width, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::PopTextWrapPos();
    ImGui::EndPopup();
}

void App::draw_about_modal() {
    if (about_modal_pending_) {
        ImGui::OpenPopup("About app");
        about_modal_pending_ = false;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560.0f * ImGui::GetFontSize() / 15.0f, 0.0f));

    bool stay_open = true;
    if (!ImGui::BeginPopupModal("About app", &stay_open, ImGuiWindowFlags_NoResize)) return;
    // Before EndPopup(), as in draw_paths_modal().
    ImGui::PushTextWrapPos(0.0f);

    // Built once per frame the dialog is open, and kept as a list of pairs so
    // that the same text can be shown and copied. Which decoders, which GPU
    // driver and which shader backend a build ended up with all depend on what
    // was found when it was configured or started, so they are reported rather
    // than assumed - and a bug report that carries them is one that can be read.
    std::vector<std::pair<std::string, std::string>> details;
    const auto detail = [&details](std::string label, std::string value) {
        details.emplace_back(std::move(label), std::move(value));
    };

    const int sdl = SDL_GetVersion();
    char sdl_version[64];
    std::snprintf(sdl_version, sizeof(sdl_version), "%d.%d.%d (built against %d.%d.%d)",
                  SDL_VERSIONNUM_MAJOR(sdl), SDL_VERSIONNUM_MINOR(sdl),
                  SDL_VERSIONNUM_MICRO(sdl), SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
                  SDL_MICRO_VERSION);

    detail("Version", SSSTUDIO_VERSION_STRING);
    detail("Shader backend", backend_name_);
    detail("Image formats",
           image_loading_available() ? supported_image_formats() : std::string("none"));
    const char* driver = device_ ? SDL_GetGPUDeviceDriver(device_) : nullptr;
    detail("GPU driver", driver ? driver : "none");
    detail("Platform", SDL_GetPlatform());
    detail("SDL", sdl_version);
    detail("Dear ImGui", IMGUI_VERSION);
    detail("Pack format", std::to_string(kPackVersionMajor) + "." +
                              std::to_string(kPackVersionMinor));
    detail("Project format", std::to_string(kProjectFormatVersion));

    ImGui::TextUnformatted("SDL Shader Studio");
    ImGui::TextWrapped(
        "An editor, preview and build tool for SDL GPU shaders: write HLSL or GLSL, watch it "
        "compile as you type, and pack the result for every format your targets need.");
    ImGui::Spacing();

    if (ImGui::BeginTable("##about", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::CalcTextSize("Shader backend").x +
                                    ImGui::GetStyle().ItemSpacing.x * 2.0f);
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
        for (const auto& [label, value] : details) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(label.c_str());
            ImGui::TableNextColumn();
            // Same gesture as the paths dialog: a value here is usually wanted
            // somewhere else, and the one place it is ever typed out is a bug
            // report.
            copyable_path(value, false);
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Separator();
    const float action_width = fitted_width(120.0f, 80.0f);
    if (ImGui::Button("Copy details", ImVec2(action_width, 0))) {
        std::string all;
        for (const auto& [label, value] : details) all += label + ": " + value + "\n";
        ImGui::SetClipboardText(all.c_str());
    }
    same_line_right_aligned(action_width);
    if (ImGui::Button("Close", ImVec2(action_width, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::PopTextWrapPos();
    ImGui::EndPopup();
}

void App::draw_new_project_modal() {
    if (new_project_modal_pending_) {
        // Only on the frame the request arrives. Opening it every frame would
        // re-open the modal the instant Escape or the close button dismissed it.
        ImGui::OpenPopup("New project");
        new_project_modal_pending_ = false;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    // A fixed width with an auto-fit height: the intro line has to wrap, and an
    // auto-fitting window would grow to hold it on one line instead.
    ImGui::SetNextWindowSize(ImVec2(560.0f * ImGui::GetFontSize() / 15.0f, 0.0f));

    // Passing p_open is what puts the close button in the title bar; ImGui
    // returns false and closes the popup itself when it is clicked.
    bool stay_open = true;
    if (!ImGui::BeginPopupModal("New project", &stay_open, ImGuiWindowFlags_NoResize)) return;

    ImGui::TextWrapped(
        "A new project starts with a vertex and a fragment shader that already compile, so "
        "there is something to build and preview straight away.");
    ImGui::Spacing();

    const ImGuiStyle& style = ImGui::GetStyle();
    const float browse_width = ImGui::CalcTextSize("Browse...").x + style.FramePadding.x * 2.0f;
    bool submit_from_field = false;

    // A two-column table keeps the labels off the fields' right edge, which is
    // where the Browse button has to live.
    if (ImGui::BeginTable("##new_project_form", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::CalcTextSize("Location").x + style.ItemSpacing.x);
        ImGui::TableSetupColumn("##field", ImGuiTableColumnFlags_WidthStretch);

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Name");
        ImGui::TableNextColumn();
        if (new_project_modal_focus_) {
            ImGui::SetKeyboardFocusHere();
            new_project_modal_focus_ = false;
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        submit_from_field |=
            ImGui::InputText("##name", new_project_name_, sizeof(new_project_name_),
                             ImGuiInputTextFlags_EnterReturnsTrue);

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Location");
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-(browse_width + style.ItemSpacing.x));
        submit_from_field |=
            ImGui::InputText("##location", new_project_location_, sizeof(new_project_location_),
                             ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        ImGui::BeginDisabled(file_dialog_.busy());
        if (ImGui::Button("Browse...")) {
            std::filesystem::path start(new_project_location_);
            std::error_code ec;
            if (start.empty() || !std::filesystem::is_directory(start, ec)) {
                start = default_browse_dir();
            }
            file_dialog_.open_folder(window_, "Choose where to put the project", start,
                                     [this](const std::filesystem::path& picked) {
                                         const std::string text = picked.string();
                                         std::snprintf(new_project_location_,
                                                       sizeof(new_project_location_), "%s",
                                                       text.c_str());
                                         new_project_error_.clear();
                                     });
        }
        ImGui::EndDisabled();

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Language");
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(fitted_width(160.0f, 80.0f));
        const char* languages[] = {"HLSL", "GLSL"};
        ImGui::Combo("##language", &new_project_language_, languages, IM_ARRAYSIZE(languages));
        ImGui::EndTable();
    }

    // The full destination is spelled out rather than implied: "location" plus
    // "name" is the one place this form can surprise someone.
    const std::filesystem::path root =
        std::filesystem::path(new_project_location_) / new_project_name_;
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("Creates %s", (root / "project.toml").string().c_str());
    ImGui::PopStyleColor();

    if (!new_project_error_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme_vec4(theme_.diagnostic(Severity::Error)));
        ImGui::TextWrapped("%s", new_project_error_.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Separator();
    // Cancel on the left, the committing button on the right: the platform's own
    // order, used by every dialog in the app.
    const float action_width = fitted_width(120.0f, 80.0f);
    FlowLayout buttons;
    buttons.next(action_width);
    const bool cancel = ImGui::Button("Cancel", ImVec2(action_width, 0));
    buttons.next(action_width);
    const bool submit = ImGui::Button("Create", ImVec2(action_width, 0)) || submit_from_field;

    if (submit) {
        const std::string name = new_project_name_;
        std::error_code ec;
        if (name.empty()) {
            new_project_error_ = "Give the project a name.";
        } else if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
            new_project_error_ = "The name cannot contain a path separator.";
        } else if (std::string(new_project_location_).empty()) {
            new_project_error_ = "Choose where the project should live.";
        } else if (name == "." || name == "..") {
            new_project_error_ = "That name would not make a folder.";
        } else if (std::filesystem::exists(root / "project.toml", ec)) {
            new_project_error_ = "A project already exists at " + root.string() + ".";
        } else if (new_project(root, name,
                               new_project_language_ == 1 ? Language::GLSL : Language::HLSL)) {
            new_project_error_.clear();
            ImGui::CloseCurrentPopup();
        } else {
            new_project_error_ = "Could not create the project. See the diagnostics panel.";
        }
    }
    if (cancel) {
        new_project_error_.clear();
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

namespace {

/// InputTextMultiline over a std::string. ImGui's own overloads take a fixed
/// buffer, and any buffer size that looked reasonable in a form would silently
/// cut a real shader in half at the moment it was pasted.
bool input_text_multiline_string(const char* label, std::string& text, const ImVec2& size,
                                 ImGuiInputTextFlags flags = 0) {
    struct Resize {
        static int callback(ImGuiInputTextCallbackData* data) {
            if (data->EventFlag != ImGuiInputTextFlags_CallbackResize) return 0;
            auto* owner = static_cast<std::string*>(data->UserData);
            owner->resize(static_cast<std::size_t>(data->BufTextLen));
            data->Buf = owner->data();
            return 0;
        }
    };
    // ImGui writes into the buffer up to the size it is given, so the string has
    // to own room for the terminator as well as the text.
    if (text.capacity() < text.size() + 1) text.reserve(text.size() + 1);
    return ImGui::InputTextMultiline(label, text.data(), text.capacity() + 1, size,
                                     flags | ImGuiInputTextFlags_CallbackResize, Resize::callback,
                                     &text);
}

/// The colour a diagnostic should be shown in inside a form, where the
/// diagnostics panel's own styling is not available.
ImVec4 severity_color(Severity severity, const ResolvedTheme& theme) {
    return theme_vec4(theme.diagnostic(severity));
}

}  // namespace

void App::prompt_import_shader() {
    import_modal_pending_ = true;
    import_modal_focus_ = true;
    import_error_.clear();
    import_scan_dirty_ = true;
}

bool App::import_fullscreen_shader(const ImportRequest& request, std::string& out_error) {
    ProjectSession* session = active();
    if (!session) {
        out_error = "No project is open.";
        return false;
    }

    ImportResult result;
    Diagnostics diags;
    const bool ok = write_import_files(session->project, request, result, diags);
    for (const auto& d : diags) log(d.severity, d.format());
    if (!ok) {
        out_error = "The import failed.";
        for (const auto& d : diags) {
            if (d.severity != Severity::Error) continue;
            out_error = d.message;
            break;
        }
        return false;
    }

    // The vertex shader goes in first so that the fragment shader it pairs with
    // finds it already there and the preview has both halves on the same frame.
    if (!result.vertex_path.empty()) {
        add_shader(session->project.absolute(result.vertex_path), Stage::Vertex, result.vertex_id,
                   Language::GLSL);
    }
    add_shader(session->project.absolute(result.shader_path), Stage::Fragment, result.shader_id,
               Language::GLSL);

    if (!result.provenance.empty()) {
        session->project.provenance[result.shader_id] = result.provenance;
    }
    if (!session->project.find_pipeline(result.pipeline.name)) {
        session->project.pipelines.push_back(result.pipeline);
    }

    // Point the preview at what was just imported rather than leaving it on
    // whatever was selected before - the active pipeline as well as the session,
    // since the pipeline is what the bar reads.
    session->preview_fragment = result.shader_id;
    session->preview_vertex = result.vertex_id;
    if (PreviewPipeline* pipeline = session->project.active_pipeline()) {
        pipeline->vertex = result.vertex_id;
        pipeline->fragment = result.shader_id;
    }

    Diagnostics save_diags;
    save_project(session->project, save_diags);
    for (const auto& d : save_diags) log(d.severity, d.format());

    log(Severity::Info, "imported " + result.shader_id + " from " +
                            (request.options.source_url.empty() ? std::string("a pasted source")
                                                                : request.options.source_url));
    return true;
}

void App::draw_import_modal() {
    if (import_modal_pending_) {
        ImGui::OpenPopup("Import fullscreen shader");
        import_modal_pending_ = false;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    const float scale = ImGui::GetFontSize() / 15.0f;
    ImGui::SetNextWindowSize(ImVec2(640.0f * scale, 0.0f));

    bool stay_open = true;
    if (!ImGui::BeginPopupModal("Import fullscreen shader", &stay_open,
                                ImGuiWindowFlags_NoResize)) {
        return;
    }

    const ImGuiStyle& style = ImGui::GetStyle();

    ImGui::TextWrapped(
        "Paste a fullscreen fragment shader written against the common web convention - one "
        "mainImage entry point and the i-prefixed uniforms. It is wrapped in a generated "
        "prelude and epilogue and added to this project as an ordinary GLSL shader.");
    ImGui::Spacing();

    if (import_modal_focus_) {
        ImGui::SetKeyboardFocusHere();
        import_modal_focus_ = false;
    }
    if (input_text_multiline_string("##source", import_source_,
                                    ImVec2(-FLT_MIN, 220.0f * scale))) {
        import_scan_dirty_ = true;
        import_error_.clear();
    }

    if (import_scan_dirty_) {
        import_scan_ = scan_fullscreen_source(import_source_);
        import_scan_dirty_ = false;
        // Offer the notice the source carried as the licence, unless the user
        // has already typed one. Losing an attribution is not recoverable, so
        // the default leans towards keeping it.
        if (import_licence_[0] == '\0' && !import_scan_.leading_comment.empty()) {
            std::snprintf(import_licence_, sizeof(import_licence_), "%s",
                          "see the notice at the top of the source");
        }
    }

    ImGui::SetNextItemOpen(import_show_common_, ImGuiCond_Once);
    if (ImGui::CollapsingHeader("Shared code (optional)")) {
        import_show_common_ = true;
        ImGui::TextWrapped(
            "Code the source expects to be in scope already. It is placed ahead of the body.");
        input_text_multiline_string("##common", import_common_, ImVec2(-FLT_MIN, 90.0f * scale));
    }

    if (ImGui::BeginTable("##import_form", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::CalcTextSize("Licence").x + style.ItemSpacing.x * 2.0f);
        ImGui::TableSetupColumn("##field", ImGuiTableColumnFlags_WidthStretch);

        const auto row = [&](const char* label, const char* id, char* buffer, std::size_t size,
                             const char* hint) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint(id, hint, buffer, size);
        };

        row("Name", "##id", import_id_, sizeof(import_id_), "imported");
        row("Source", "##url", import_url_, sizeof(import_url_), "where it came from (optional)");
        row("Author", "##author", import_author_, sizeof(import_author_), "who wrote it");
        row("Licence", "##licence", import_licence_, sizeof(import_licence_),
            "the terms it is offered under");
        ImGui::EndTable();
    }

    ImGui::Checkbox("Force the result opaque", &import_force_opaque_);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Discards the alpha the shader produced. Turn this off for a shader whose alpha\n"
            "channel carries data rather than coverage.");
    }

    // --- what the scan makes of it ----------------------------------------
    ImGui::Spacing();
    ImGui::Separator();
    if (import_source_.empty()) {
        ImGui::TextDisabled("Nothing pasted yet.");
    } else {
        for (const Diagnostic& d : import_scan_.notes) {
            ImGui::PushStyleColor(ImGuiCol_Text, severity_color(d.severity, theme_));
            ImGui::TextWrapped("%s", d.message.c_str());
            ImGui::PopStyleColor();
        }
        if (import_scan_.notes.empty()) {
            ImGui::TextDisabled("Ready to import.");
        }
    }

    if (!import_error_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, severity_color(Severity::Error, theme_));
        ImGui::TextWrapped("%s", import_error_.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Separator();
    const float action_width = fitted_width(120.0f, 80.0f);
    FlowLayout buttons;
    buttons.next(action_width);
    const bool can_import = project_open() && is_importable(import_scan_.entry);
    ImGui::BeginDisabled(!can_import);
    const bool submit = ImGui::Button("Import", ImVec2(action_width, 0));
    ImGui::EndDisabled();
    if (!project_open() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Open a project first.");
    }
    buttons.next(action_width);
    const bool cancel = ImGui::Button("Cancel", ImVec2(action_width, 0));

    if (submit) {
        ImportRequest request;
        request.source = import_source_;
        request.id = import_id_;
        request.options.common = import_common_;
        request.options.source_url = import_url_;
        request.options.author = import_author_;
        request.options.licence = import_licence_;
        request.options.force_opaque = import_force_opaque_;

        if (import_fullscreen_shader(request, import_error_)) {
            import_source_.clear();
            import_common_.clear();
            import_id_[0] = '\0';
            import_url_[0] = '\0';
            import_author_[0] = '\0';
            import_licence_[0] = '\0';
            import_error_.clear();
            import_scan_ = {};
            import_scan_dirty_ = true;
            ImGui::CloseCurrentPopup();
        }
    }
    if (cancel) {
        import_error_.clear();
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void App::draw_command_palette() {
    if (!command_palette_open_) return;
    ImGui::OpenPopup("Command palette");

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->GetCenter().x, vp->WorkPos.y + 120.0f), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Appearing);

    if (ImGui::BeginPopup("Command palette")) {
        static char filter[128] = "";
        ImGui::SetKeyboardFocusHere();
        ImGui::InputTextWithHint("##filter", "type a command", filter, sizeof(filter));

        const std::string needle = filter;
        for (const auto& action : actions_) {
            if (!needle.empty() &&
                action.label.find(needle) == std::string::npos &&
                action.id.find(needle) == std::string::npos) {
                continue;
            }
            const bool enabled = !action.enabled || action.enabled();
            ImGui::BeginDisabled(!enabled);
            if (ImGui::Selectable(action.label.c_str())) {
                if (action.run) action.run();
                command_palette_open_ = false;
                filter[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            // The shortcut sits in its own column on the right while the popup
            // is wide enough for one; on a narrow one it follows the label
            // instead of being placed at a negative offset.
            if (!action.shortcut.empty()) {
                const float shortcut_width = ImGui::CalcTextSize(action.shortcut.c_str()).x;
                const float column = ImGui::GetContentRegionAvail().x - shortcut_width;
                if (column > ImGui::GetCursorPosX()) {
                    ImGui::SameLine(column);
                } else {
                    ImGui::SameLine();
                }
                ImGui::TextDisabled("%s", action.shortcut.c_str());
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            command_palette_open_ = false;
            filter[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    } else {
        command_palette_open_ = false;
    }
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------
void App::frame(float delta_seconds) {
    // First, before any panel pushes a font of its own.
    begin_type_frame();
    compiler_->poll();
    file_dialog_.poll();

    // A failed build is worth interrupting for: bring the panel that lists the
    // errors forward rather than leaving the news behind another tab.
    if (build_finished_.exchange(false)) {
        ProjectSession* owner = find_session(build_owner_key_);
        // Only when the failure is on screen. Yanking a panel forward to show
        // another project's errors would be showing the wrong project's state.
        if (owner && owner == active() && !owner->last_report.ok) {
            show_build = true;
            raise_build_panel_ = true;
        } else if (owner && !owner->last_report.ok) {
            log(Severity::Warning, "the build of " + owner->project.name + " failed");
        }
        build_owner_key_.clear();
    }
    // Finished downloads report back here, on the thread that asked for them.
    assets_.poll();

    if (textures_) {
        // Takes delivery of whatever the decode worker finished, and lets go of
        // anything that has not been asked for lately. Before any panel resolves
        // a binding, so a texture that arrived this frame is visible this frame.
        const ProjectSession* session = active();
        textures_->tick(session ? session->preview_frame : 0,
                        settings_.tools.texture_cache_bytes);
    }
    poll_external_changes(delta_seconds);
    autosave(delta_seconds);

    // Only the project on screen advances, and only while the preview is
    // actually drawing something. A background tab picks up exactly where it was
    // left, which is what makes a paused comparison hold still - and a preview
    // that is behind another tab, closed, or reporting a compile error is no
    // different: nothing was shown, so no shader time passed.
    if (ProjectSession* session = active();
        session && !session->project.preview.paused && preview_presented_) {
        // Clamped away from zero: a shader dividing by its frame delta must not
        // see one, and a stalled frame (a modal dialog, a long compile) must not
        // hand it a delta large enough to make an integrator explode.
        const double advance =
            std::clamp(static_cast<double>(delta_seconds) * session->project.preview.speed,
                       1.0e-4, 0.25);
        session->preview_delta = advance;
        session->preview_time += advance;
        ++session->preview_frame;
    } else if (ProjectSession* session = active()) {
        session->preview_delta = 0.0;
    }
    // Cleared after the decision and before the panels run, so the flag always
    // describes the frame about to be drawn rather than accumulating.
    preview_presented_ = false;

    draw_dockspace();
    handle_shortcuts();

    if (raise_build_panel_) {
        ImGui::SetWindowFocus("Build");
        raise_build_panel_ = false;
    }

    if (show_editor) draw_editor_panel(*this);
    if (show_io) draw_io_panel(*this);
    if (show_preview) draw_preview_panel(*this);
    if (show_build) draw_build_panel(*this);
    if (show_diagnostics) draw_diagnostics_panel(*this);
    if (show_graph) draw_graph_panel(*this);
    if (show_pipeline_editor) draw_pipeline_panel(*this);
    if (show_scene) draw_scene_panel(*this);
    if (show_settings) draw_settings_panel(*this);

    // After the panels, because a window has to exist before it can be raised.
    apply_panel_focus();

    draw_new_project_modal();
    draw_import_modal();
    draw_new_shader_modal();
    draw_rename_shader_modal();
    draw_delete_shader_modal();
    draw_close_project_modal();
    draw_paths_modal();
    draw_about_modal();
    drive_quit();
    draw_command_palette();

    // Docking, split sizes and tab order are the user's arrangement, so they are
    // kept without being asked for. ImGui raises this a moment after a change
    // settles rather than on every frame of a drag, and at the end of the frame
    // is where the panels have all had their say about where they are.
    if (ImGui::GetIO().WantSaveIniSettings) save_layout_now();

    // A tab was opened or closed. Deferred to here rather than written where it
    // happened, so that closing ten tabs at once costs one write of the settings
    // file instead of ten.
    if (editor_state_dirty_) {
        editor_state_dirty_ = false;
        save_settings_now();
    }
}

// ---------------------------------------------------------------------------
// Graphs
// ---------------------------------------------------------------------------
Graph* App::graph_for(const std::string& shader_id) {
    ProjectSession* session = active();
    if (!session) return nullptr;
    auto it = session->graphs.find(shader_id);
    return it == session->graphs.end() ? nullptr : &it->second;
}

void App::create_graph(const std::string& shader_id) {
    ProjectSession* session = active();
    if (!session) return;
    const ShaderDesc* desc = session->project.find_shader(shader_id);
    Document* doc = find_document(shader_id);
    if (!desc || !doc) return;

    Graph graph =
        Graph::create_default(shader_id, desc->stage, session->project.language_of(*desc));
    session->graphs[shader_id] = std::move(graph);
    doc->generated_by_graph = true;
    on_graph_changed(shader_id);
    show_graph = true;
}

void App::detach_graph(const std::string& shader_id) {
    ProjectSession* session = active();
    Graph* graph = graph_for(shader_id);
    if (!session || !graph) return;

    // Detaching keeps the source that was last generated and stops regenerating
    // it. The graph is retained as a snapshot so the decision is reversible.
    graph->detached = true;
    if (Document* doc = find_document(shader_id)) doc->generated_by_graph = false;

    Diagnostics diags;
    save_graph(session->project.root / "graphs" / (shader_id + ".toml"), *graph, diags);
    for (auto& d : diags) log(d.severity, d.format());
    log(Severity::Info, shader_id + " detached: the text file is now authoritative");
}

void App::on_graph_changed(const std::string& shader_id) {
    ProjectSession* session = active();
    Graph* graph = graph_for(shader_id);
    Document* doc = find_document(shader_id);
    if (!session || !graph || !doc) return;

    Diagnostics diags;
    if (!graph->detached) {
        GraphCodegenOptions options;
        options.generated_by = tool_version();
        if (const ShaderDesc* desc = session->project.find_shader(shader_id)) {
            options.entry_point = desc->entry_point.empty() ? "main" : desc->entry_point;
        }
        const std::string source = generate_graph_source(*graph, options, diags);
        if (!source.empty() && source != doc->text) {
            doc->text = source;
            doc->dirty = true;
            schedule_compile(*doc, true);
        }
    }
    for (auto& d : diags) {
        if (d.severity == Severity::Error) log(d.severity, d.format());
    }

    save_graph_layout(shader_id);
}

void App::save_graph_layout(const std::string& shader_id) {
    ProjectSession* session = active();
    Graph* graph = graph_for(shader_id);
    if (!session || !graph) return;

    Diagnostics diags;
    save_graph(session->project.root / "graphs" / (shader_id + ".toml"), *graph, diags);
    for (auto& d : diags) log(d.severity, d.format());
}

// ---------------------------------------------------------------------------
// Scenes (test harness)
// ---------------------------------------------------------------------------
Scene* App::active_scene() {
    ProjectSession* session = active();
    return session && session->scene_open ? &session->scene : nullptr;
}

void App::mark_scene_dirty() {
    if (ProjectSession* session = active()) session->scene_dirty = true;
}

std::vector<std::filesystem::path> App::available_scenes() const {
    const ProjectSession* session = active();
    if (!session) return {};
    return list_scenes(session->project.root);
}

void App::create_scene_from_template(SceneTemplate which) {
    ProjectSession* session = active();
    if (!session) return;

    // Default the material to whatever fragment shader is in front of the user.
    std::string material;
    if (const Document* doc = active_document()) {
        if (doc->stage == Stage::Fragment) material = doc->id;
    }
    if (material.empty()) {
        for (const auto& shader : session->project.shaders) {
            if (shader.stage == Stage::Fragment) {
                material = shader.id;
                break;
            }
        }
    }

    session->scene = make_scene_template(which, material);
    session->scene.path = session->project.root / "scenes" / (session->scene.name + ".toml");
    session->scene_open = true;
    session->scene_dirty = true;
    session->scene_selected_entity.clear();
    show_scene = true;
}

bool App::open_scene(const std::filesystem::path& path) {
    ProjectSession* session = active();
    if (!session) return false;

    Scene loaded;
    Diagnostics diags;
    const bool ok = load_scene(path, loaded, diags);
    for (auto& d : diags) log(d.severity, d.format());
    if (!ok) return false;

    session->scene = std::move(loaded);
    session->scene_open = true;
    session->scene_dirty = false;
    session->scene_selected_entity.clear();
    return true;
}

void App::save_active_scene() {
    ProjectSession* session = active();
    if (!session || !session->scene_open) return;
    Diagnostics diags;
    if (save_scene(session->scene, diags)) session->scene_dirty = false;
    for (auto& d : diags) log(d.severity, d.format());
}

void App::close_scene() {
    ProjectSession* session = active();
    if (!session) return;
    if (session->scene_open && session->scene_dirty) save_active_scene();
    session->scene = Scene{};
    session->scene_open = false;
    session->scene_dirty = false;
    session->scene_selected_entity.clear();
}

// ---------------------------------------------------------------------------
// Snapshots and A/B compare
// ---------------------------------------------------------------------------
void App::pin_current_version(Document& doc) {
    doc.pinned_text = doc.text;
    doc.pinned_reflection = doc.reflection;
    doc.has_pinned = true;
    log(Severity::Info, "pinned the current version of " + doc.id +
                            "; the preview can now show A/B");
}

void App::clear_pinned_version(Document& doc) {
    doc.has_pinned = false;
    doc.pinned_text.clear();
    doc.pinned_spirv.clear();
}

std::string App::snapshot_diff(const Document& doc) const {
    if (doc.undo_snapshots.empty()) return {};
    const std::string& oldest = doc.undo_snapshots.front();
    return format_unified(diff_lines(oldest, doc.text), doc.id + " (snapshot)", doc.id);
}

}  // namespace ssstudio::gui
