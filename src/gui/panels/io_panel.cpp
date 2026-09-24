// The I/O panel is generated from reflection: every uniform member, texture and
// buffer the shader declares gets a row, and each row's binding says where the
// value comes from. Nothing is hardcoded per shader.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <cfloat>

#include <imgui.h>

#include "ssstudio/hash.h"

#include "app.h"
#include "panel_common.h"
#include "bindings.h"
#include "preview/renderer.h"
#include "widgets.h"

namespace ssstudio::gui {
namespace {

const char* source_label(BindingSource s) {
    switch (s) {
        case BindingSource::Default: return "default";
        case BindingSource::Manual: return "manual";
        case BindingSource::Macro: return "macro";
        case BindingSource::Expression: return "expression";
        case BindingSource::Curve: return "curve";
        case BindingSource::File: return "file";
        case BindingSource::Url: return "url";
        case BindingSource::Procedural: return "procedural";
        case BindingSource::PreviousFrame: return "previous frame";
        case BindingSource::PassOutput: return "pass output";
        case BindingSource::BuiltinMesh: return "builtin mesh";
        case BindingSource::Generated: return "generated";
        case BindingSource::Scene: return "scene";
    }
    return "default";
}

// Seconds since midnight, for the date macro. Local time, because a shader that
// uses it is almost always drawing a clock.
double seconds_into_day() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    return local.tm_hour * 3600.0 + local.tm_min * 60.0 + local.tm_sec;
}

/// One entry of the macro cheatsheet: what to type, what it evaluates to, and
/// what it means. Every name here is answered by macro_value below, and every
/// name macro_value answers is here - the table is the only place a reader can
/// find out what exists, so a macro missing from it is a macro nobody knows
/// about.
struct MacroInfo {
    const char* name;
    const char* type;
    const char* doc;
};

constexpr auto kMacros = std::to_array<MacroInfo>({
    {"time", "float", "seconds since the preview started"},
    {"sin_time", "float", "sine of time, -1 to 1"},
    {"delta_time", "float", "seconds the previous frame took"},
    {"frame", "float", "frames drawn since the preview started"},
    {"resolution", "float2", "preview size in pixels"},
    {"aspect", "float", "preview width divided by its height"},
    {"mouse", "float2", "pointer position in pixels"},
    // The web-shader convention, which the fullscreen importer targets. Listed
    // second because a shader written here should prefer the names above.
    {"iTime", "float", "seconds since the preview started"},
    {"iTimeDelta", "float", "seconds the previous frame took"},
    {"iFrameRate", "float", "frames per second, from the last frame"},
    {"iFrame", "float", "frames drawn since the preview started"},
    {"iSampleRate", "float", "audio sample rate; fixed at 44100"},
    {"iResolution", "float3", "width, height, pixel aspect (always 1)"},
    {"iMouse", "float4", "xy while held; zw the click, signed by button state"},
    {"iDate", "float4", "year, month (0-based), day, seconds into the day"},
    {"iChannelTime", "float", "per-channel time; shares the preview clock"},
    {"iChannelResolution", "float3", "per-channel size; 1x1 until textures land"},
});

/// How many rows of the cheatsheet fit before it stops being a popup and starts
/// being a window. Anything past this paginates.
constexpr int kMacrosPerPage = 8;

/// The page the cheatsheet is showing. One value for the whole panel rather than
/// one per row: only one popup can be open at a time, and it is reset as it
/// opens, so a second row never inherits the first row's page.
int& cheatsheet_page() {
    static int page = 0;
    return page;
}

/// The cheatsheet behind the (i) beside a macro field. Returns the name the user
/// picked, or empty when they picked nothing - clicking a name is the quickest
/// way to use one, and the copy button is there for putting it somewhere else.
std::string draw_macro_cheatsheet(const char* popup_id) {
    std::string picked;
    if (!ImGui::BeginPopup(popup_id)) return picked;

    ImGui::TextDisabled("Click a name to use it here, or copy it.");
    ImGui::Separator();

    constexpr int total = static_cast<int>(kMacros.size());
    constexpr int pages = (total + kMacrosPerPage - 1) / kMacrosPerPage;
    int& page = cheatsheet_page();
    page = std::clamp(page, 0, pages - 1);

    const int first = page * kMacrosPerPage;
    const int last = std::min(first + kMacrosPerPage, total);

    // Every page is laid out at the size of the widest entry in the whole table
    // and given room for a full page of rows, whether or not this page has one.
    // A popup is auto-sized, but it is only *positioned* on the frame it opens:
    // a later page that measured wider would grow to the right, straight off the
    // edge of the screen when the row it belongs to is near it. Holding the size
    // still is what keeps the placement chosen at open time correct.
    const ImGuiStyle& style = ImGui::GetStyle();
    float name_width = 0.0f;
    float type_width = 0.0f;
    float doc_width = 0.0f;
    for (const MacroInfo& macro : kMacros) {
        name_width = std::max(name_width, ImGui::CalcTextSize(macro.name).x);
        type_width = std::max(type_width, ImGui::CalcTextSize(macro.type).x);
        doc_width = std::max(doc_width, ImGui::CalcTextSize(macro.doc).x);
    }
    const float copy_width = ImGui::CalcTextSize("copy").x + style.FramePadding.x * 2.0f;
    const float row_height = ImGui::GetTextLineHeight() + style.CellPadding.y * 2.0f;
    const ImVec2 rows_size(
        copy_width + name_width + type_width + doc_width + style.CellPadding.x * 8.0f,
        static_cast<float>(kMacrosPerPage) * row_height + 2.0f);

    if (ImGui::BeginChild("##rows", rows_size, false, ImGuiWindowFlags_NoScrollbar)) {
        if (ImGui::BeginTable("##macros", 4,
                              ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("##copy", ImGuiTableColumnFlags_WidthFixed, copy_width);
            ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthFixed, name_width);
            ImGui::TableSetupColumn("##type", ImGuiTableColumnFlags_WidthFixed, type_width);
            ImGui::TableSetupColumn("##doc", ImGuiTableColumnFlags_WidthFixed, doc_width);

            for (int i = first; i < last; ++i) {
                const MacroInfo& macro = kMacros[static_cast<std::size_t>(i)];
                ImGui::TableNextRow();
                ImGui::PushID(i);

                ImGui::TableNextColumn();
                if (ImGui::SmallButton("copy")) ImGui::SetClipboardText(macro.name);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Copy \"%s\" to the clipboard", macro.name);
                }

                ImGui::TableNextColumn();
                if (ImGui::Selectable(macro.name, false, ImGuiSelectableFlags_NoAutoClosePopups)) {
                    picked = macro.name;
                    ImGui::CloseCurrentPopup();
                }

                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", macro.type);

                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", macro.doc);

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();

    // The pager only appears when there is more than one page: a "1 / 1" under a
    // list that already shows everything is a control that does nothing.
    if (pages > 1) {
        ImGui::Separator();
        ImGui::BeginDisabled(page == 0);
        if (ImGui::ArrowButton("##prev", ImGuiDir_Left)) --page;
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Text("%d / %d", page + 1, pages);
        ImGui::SameLine();
        ImGui::BeginDisabled(page + 1 >= pages);
        if (ImGui::ArrowButton("##next", ImGuiDir_Right)) ++page;
        ImGui::EndDisabled();
    }

    ImGui::EndPopup();
    return picked;
}

// Macros the preview provides for free. Names match what a shader author would
// guess, which is what auto-mapping keys off: a uniform member called `time`
// picks up the time macro without anyone binding anything.
//
// The `i`-prefixed spellings are the ones used by the common web-shader
// convention that the fullscreen importer targets, so an imported shader
// animates with no bindings configured at all. They are ordinary macros - a
// hand-written shader may use them too.
//
// `element` indexes an array member and is zero for everything else; `component`
// indexes within one element.
double macro_value(const std::string& name, std::uint32_t element, std::uint32_t component,
                   const App& app, const PreviewSettings& preview,
                   const TextureFeed* channels = nullptr) {
    const double t = app.preview_time();
    const double width = static_cast<double>(preview.width);
    const double height = static_cast<double>(preview.height);

    if (name == "time") return t;
    if (name == "sin_time") return std::sin(t);
    if (name == "delta_time") return app.preview_delta();
    if (name == "frame") return static_cast<double>(app.preview_frame());
    if (name == "resolution") {
        return component == 0 ? width : height;
    }
    if (name == "aspect") {
        return preview.height ? width / height : 1.0;
    }
    if (name == "mouse") {
        const ImVec2 p = ImGui::GetMousePos();
        return component == 0 ? p.x : p.y;
    }

    // --- the imported-shader spellings ------------------------------------
    if (name == "iTime") return t;
    if (name == "iTimeDelta") return app.preview_delta();
    if (name == "iFrameRate") {
        const double delta = app.preview_delta();
        return delta > 0.0 ? 1.0 / delta : 0.0;
    }
    if (name == "iFrame") return static_cast<double>(app.preview_frame());
    if (name == "iSampleRate") return 44100.0;
    if (name == "iResolution") {
        // The third component is the pixel aspect ratio, not the height. Getting
        // this wrong is invisible until a shader divides by it.
        switch (component) {
            case 0: return width;
            case 1: return height;
            default: return 1.0;
        }
    }
    if (name == "iMouse") {
        // xy is where the pointer was while held; zw carries the click position
        // with its sign as the state - z positive only on the frame the button
        // went down, w positive while it is held. Shaders test those signs, so
        // the negation is the contract rather than a quirk.
        const PreviewMouse& m = app.preview_mouse();
        switch (component) {
            case 0: return m.x;
            case 1: return m.y;
            case 2: return m.pressed ? m.click_x : -std::abs(m.click_x);
            default: return m.down ? m.click_y : -std::abs(m.click_y);
        }
    }
    if (name == "iDate") {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
#if defined(_WIN32)
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        switch (component) {
            case 0: return local.tm_year + 1900.0;
            // Zero-based, matching the convention rather than the calendar.
            case 1: return static_cast<double>(local.tm_mon);
            case 2: return static_cast<double>(local.tm_mday);
            default: return seconds_into_day();
        }
    }
    if (name == "iChannelTime") {
        // Every channel shares the preview clock until a channel can carry a
        // time of its own.
        return t;
    }
    if (name == "iChannelResolution") {
        // The size of whatever is actually bound to that channel. An unbound one
        // is the 1x1 stand-in and reports itself as such, which is what a shader
        // dividing by it needs to see rather than the preview's own size.
        std::uint32_t channel_width = 1;
        std::uint32_t channel_height = 1;
        if (channels) {
            const std::string channel = "iChannel" + std::to_string(element);
            if (const auto it = channels->by_name.find(channel); it != channels->by_name.end()) {
                channel_width = it->second.width;
                channel_height = it->second.height;
            }
        }
        switch (component) {
            case 0: return channel_width;
            case 1: return channel_height;
            default: return 1.0;
        }
    }
    return 0.0;
}

/// Points one texture binding at a file.
///
/// The binding is looked up again rather than captured: the picker answers on a
/// later frame, and by then the reference the row was drawn with may be gone -
/// the bindings map may have grown, or the user may have moved to another
/// project entirely. Finding it by name again is the only safe way to write the
/// answer down.
void set_texture_path(App& app, const std::string& shader_id, const std::string& key,
                      const std::filesystem::path& picked) {
    if (!app.project_open()) return;
    Project& project = app.project();
    Binding& binding = project.bindings[shader_id].textures[key];

    std::error_code ec;
    const auto relative = std::filesystem::relative(picked, project.root, ec);
    // Relative while the image is inside the project, so the project stays
    // portable. Absolute otherwise: a path climbing out through ".." is worse
    // than one that is honest about living somewhere else.
    const bool inside = !ec && !relative.empty() && relative.native().rfind("..", 0) != 0;
    binding.path = inside ? relative : picked;
    binding.source = BindingSource::File;
    binding.hash.clear();

    Diagnostics diags;
    save_project(project, diags);
}

/// Copies a downloaded file into the project and turns the binding into a file
/// one, so the project stops depending on this machine's cache.
///
/// Worth having because a build produces a pack someone ships: a project that
/// only renders on a machine with a warm cache is not one you can hand to
/// anybody, and the copy is also where an attribution for a downloaded image
/// belongs.
void localise_binding(App& app, const std::string& shader_id, const std::string& key) {
    if (!app.project_open()) return;
    Project& project = app.project();
    Binding& binding = project.bindings[shader_id].textures[key];
    const auto entry = app.assets().lookup(binding.text);
    if (!entry) return;

    const std::filesystem::path relative =
        std::filesystem::path("assets") / (key + entry->path.extension().string());
    Diagnostics diags;
    if (!app.assets().localise(*entry, project.absolute(relative), diags)) {
        for (const auto& d : diags) app.log(d.severity, d.format());
        return;
    }
    binding.source = BindingSource::File;
    binding.path = relative;
    binding.text.clear();
    binding.hash = to_hex(entry->content_hash);

    Diagnostics save_diags;
    save_project(project, save_diags);
    app.log(Severity::Info, "copied " + entry->url + " into " + relative.generic_string());
}

/// Starts a download for one binding, and writes down what came back.
void download_binding(App& app, const std::string& shader_id, const std::string& key,
                      const std::string& url) {
    app.assets().fetch(url, [&app, shader_id, key, url](FetchStatus status,
                                                        const AssetEntry& entry) {
        if (status != FetchStatus::Ready) {
            app.log(Severity::Error, url + ": " + std::string(to_string(status)));
            return;
        }
        // Looked up again rather than captured: the answer arrives frames later,
        // and by then the binding this started from may have moved or gone.
        if (!app.project_open()) return;
        Binding& binding = app.project().bindings[shader_id].textures[key];
        if (binding.text != url) return;
        binding.hash = to_hex(entry.content_hash);
        Diagnostics diags;
        save_project(app.project(), diags);
        app.log(Severity::Info, "downloaded " + url);
    });
}

/// The four sampler settings, edited in place. Written straight back into the
/// binding's `extra` map, which is what persists them.
bool sampler_state_editor(Binding& binding) {
    SamplerState state = sampler_state_from_extra(binding.extra);
    bool changed = false;

    const char* filters[] = {"nearest", "linear", "mipmap"};
    int filter = static_cast<int>(state.filter);
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::Combo("filter", &filter, filters, IM_ARRAYSIZE(filters))) {
        state.filter = static_cast<SamplerState::Filter>(filter);
        changed = true;
    }

    const char* wraps[] = {"clamp", "repeat"};
    int wrap = static_cast<int>(state.wrap);
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::Combo("wrap", &wrap, wraps, IM_ARRAYSIZE(wraps))) {
        state.wrap = static_cast<SamplerState::Wrap>(wrap);
        changed = true;
    }

    changed |= ImGui::Checkbox("flip vertically", &state.vflip);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("For an image whose convention puts the origin at the bottom left.");
    }
    changed |= ImGui::Checkbox("treat as sRGB", &state.srgb);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Let the hardware linearise the file's values when they are read.");
    }

    if (changed) sampler_state_to_extra(state, binding.extra);
    return changed;
}

/// One binding as a table row: what it is called, what type it is, where its
/// value comes from, the value itself, and whether it is locked.
///
/// `type` is the first line of the second column; `detail` goes under it, in
/// smaller type - the registers and what the binding resolved to. On a second
/// line rather than after the type, so the column stays narrow and the words
/// never wrap mid-register.
bool binding_row(App& app, const std::string& shader_id, const std::string& type,
                 const std::string& detail, const std::string& key, const UniformMember* member,
                 Binding& binding, const ThemeInk& ink, const Resource* resource = nullptr) {
    bool changed = false;
    ImGui::PushID(key.c_str());
    ImGui::TableNextRow();

    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    mono_text(key, ink.text, 12.0f);
    if (ImGui::IsItemHovered() && ImGui::GetItemRectSize().x >= ImGui::GetContentRegionAvail().x) {
        // Clipped by a narrow column; the whole name is one hover away.
        ImGui::SetTooltip("%s", key.c_str());
    }

    ImGui::TableNextColumn();
    {
        MonoScope mono(11.5f);
        colored_text(type, ink.soft);
    }
    if (!detail.empty()) {
        MonoScope mono(10.5f);
        colored_text(detail, ink.muted);
    }

    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##source", source_label(binding.source))) {
        const bool samples_a_texture =
            resource && resource->kind == ResourceKind::SampledTexture;
        std::vector<BindingSource> options = {BindingSource::Default, BindingSource::Manual,
                                              BindingSource::Macro, BindingSource::Expression,
                                              BindingSource::File};
        // Only a texture can name an address; a uniform member has nothing to
        // do with one, and offering it there would only invite the question.
        if (samples_a_texture) options.push_back(BindingSource::Url);
        // Reading another pass only means something once there is one to read.
        const PreviewPipeline* pipeline =
            app.project_open() ? app.project().active_pipeline() : nullptr;
        if (samples_a_texture && pipeline && !pipeline->passes.empty()) {
            options.push_back(BindingSource::PassOutput);
            options.push_back(BindingSource::PreviousFrame);
        }
        for (BindingSource option : options) {
            const bool selected = binding.source == option;
            if (ImGui::Selectable(source_label(option), selected)) {
                binding.source = option;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }

    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    switch (binding.source) {
        case BindingSource::Manual: {
            const int components =
                member ? static_cast<int>(std::max<std::uint32_t>(1, member->component_count())) : 1;
            binding.values.resize(static_cast<std::size_t>(components), 0.0);
            std::vector<float> temp(binding.values.begin(), binding.values.end());
            // A picker for the members that hold a colour, number fields for the
            // rest. These were told apart by component count, so a float3 called
            // "light_direction" got a colour picker and no way to type a value
            // into it; the member's name is the only thing that actually knows.
            bool edited = false;
            if ((components == 3 || components == 4) && names_a_color(key)) {
                edited = color_field("##v", temp.data(), components,
                                     ImGui::GetContentRegionAvail().x -
                                         ImGui::GetFrameHeight() -
                                         ImGui::GetStyle().ItemSpacing.x);
            } else if (components == 1) {
                edited = ImGui::DragFloat("##v", temp.data(), 0.01f);
            } else if (components == 2) {
                edited = ImGui::DragFloat2("##v", temp.data(), 0.01f);
            } else if (components == 3) {
                edited = ImGui::DragFloat3("##v", temp.data(), 0.01f);
            } else {
                edited = ImGui::DragFloat4("##v", temp.data(), 0.01f);
            }
            if (edited) {
                for (std::size_t i = 0; i < binding.values.size(); ++i) binding.values[i] = temp[i];
                changed = true;
            }
            break;
        }
        case BindingSource::Macro: {
            // The field gives up exactly the width of the button beside it, so
            // the two together still fill the column the way every other value
            // widget in this table does.
            const float info = ImGui::GetFrameHeight();
            const float gap = ImGui::GetStyle().ItemSpacing.x;
            ImGui::SetNextItemWidth(-(info + gap));

            char buffer[96];
            std::snprintf(buffer, sizeof(buffer), "%s", binding.text.c_str());
            bool edited = false;
            {
                // A macro name is something typed into a shader, so it is set
                // in the code font like the shader is.
                MonoScope mono(12.0f);
                edited = ImGui::InputTextWithHint("##macro", "time, resolution, mouse, ...", buffer,
                                                  sizeof(buffer));
            }
            if (edited) {
                binding.text = buffer;
                changed = true;
            }

            ImGui::SameLine(0.0f, gap);
            // A round outlined "i": a way to read more, not an action, so it
            // is drawn quieter than the fields beside it.
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, info * 0.5f);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, with_alpha(ink.raised, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_Border, ink.strong);
            ImGui::PushStyleColor(ImGuiCol_Text, ink.muted);
            const bool help = ImGui::Button("i", ImVec2(info, 0.0f));
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar(2);
            if (help) {
                cheatsheet_page() = 0;
                ImGui::OpenPopup("##macro_help");
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("What macros are there?");

            const std::string picked = draw_macro_cheatsheet("##macro_help");
            if (!picked.empty()) {
                binding.text = picked;
                changed = true;
            }
            break;
        }
        case BindingSource::Expression: {
            char buffer[160];
            std::snprintf(buffer, sizeof(buffer), "%s", binding.text.c_str());
            if (ImGui::InputTextWithHint("##expr", "sin(time * 2) * 0.5 + 0.5", buffer,
                                         sizeof(buffer))) {
                binding.text = buffer;
                changed = true;
            }
            break;
        }
        case BindingSource::PassOutput:
        case BindingSource::PreviousFrame: {
            const PreviewPipeline* pipeline =
                app.project_open() ? app.project().active_pipeline() : nullptr;
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##pass",
                                  binding.text.empty() ? "(none)" : binding.text.c_str())) {
                if (pipeline) {
                    for (const PassDesc& pass : pipeline->passes) {
                        if (ImGui::Selectable(pass.shader_id.c_str(),
                                              pass.shader_id == binding.text)) {
                            binding.text = pass.shader_id;
                            changed = true;
                        }
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    binding.source == BindingSource::PreviousFrame
                        ? "The frame before this one. What a buffer reading itself has to mean."
                        : "What that pass wrote earlier in this frame.");
            }
            break;
        }
        case BindingSource::Url: {
            const ImGuiStyle& style = ImGui::GetStyle();
            const bool cached = app.assets().lookup(binding.text).has_value();
            const bool busy = app.assets().in_flight(binding.text);
            // Nothing is fetched by typing, or by opening the project. A
            // download is a button someone presses, having seen the address.
            const char* action = cached ? "Localise" : "Download";
            const float action_width =
                ImGui::CalcTextSize("Localise").x + style.FramePadding.x * 2.0f;

            char buffer[2048];
            std::snprintf(buffer, sizeof(buffer), "%s", binding.text.c_str());
            ImGui::SetNextItemWidth(-(action_width + style.ItemSpacing.x));
            if (ImGui::InputTextWithHint("##url", "https://...", buffer, sizeof(buffer))) {
                binding.text = buffer;
                changed = true;
            }

            ImGui::SameLine();
            ImGui::BeginDisabled(busy || binding.text.empty());
            if (ImGui::Button(action, ImVec2(action_width, 0))) {
                if (cached) {
                    localise_binding(app, shader_id, key);
                    changed = true;
                } else {
                    download_binding(app, shader_id, key, binding.text);
                }
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(cached ? "Copy it into the project so it stops depending on "
                                           "this machine's cache."
                                         : "Fetch it once and keep it. Only https addresses.");
            }
            break;
        }
        case BindingSource::File: {
            const bool is_texture = resource && resource->kind == ResourceKind::SampledTexture;
            const ImGuiStyle& style = ImGui::GetStyle();
            const float settings_width =
                is_texture ? ImGui::CalcTextSize("sampler").x + style.FramePadding.x * 2.0f : 0.0f;
            const float browse_width =
                -(settings_width + (is_texture ? style.ItemSpacing.x : 0.0f));

            // The path is the button. A separate browse button would spend a
            // column on a word, and there is nothing else to do with the row.
            const std::string label =
                binding.path.empty() ? std::string("choose an image...")
                                     : binding.path.filename().string();
            ImGui::BeginDisabled(app.file_dialog_busy());
            if (ImGui::Button(label.c_str(), ImVec2(browse_width, 0))) {
                const std::filesystem::path start =
                    binding.path.empty() ? app.project().root
                                         : app.project().absolute(binding.path).parent_path();
                const std::vector<FileFilter> filters = {
                    {"Images", "png;jpg;jpeg;webp;bmp;gif;tga;qoi;pnm;svg"}, {"All files", "*"}};
                app.prompt_for_file("Choose an image", start, filters,
                                    [&app, shader_id, key](const std::filesystem::path& picked) {
                                        set_texture_path(app, shader_id, key, picked);
                                    });
            }
            ImGui::EndDisabled();
            if (!binding.path.empty() && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", binding.path.generic_string().c_str());
            }

            if (is_texture) {
                ImGui::SameLine();
                if (ImGui::Button("sampler")) ImGui::OpenPopup("##sampler");
                if (ImGui::BeginPopup("##sampler")) {
                    changed |= sampler_state_editor(binding);
                    ImGui::EndPopup();
                }
            }
            break;
        }
        default:
            ImGui::TextDisabled("-");
            break;
    }

    // A ghost toggle: nothing but the word while unlocked, and the accent's
    // tint while locked, so a locked row is findable down a long table.
    ImGui::TableNextColumn();
    {
        const char* label = binding.locked ? "locked" : "lock";
        FontScope small(nullptr, 11.5f);
        const float width = button_width(label);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             std::max(0.0f, ImGui::GetContentRegionAvail().x - width));
        ImGui::AlignTextToFramePadding();
        const ImU32 fill = binding.locked ? ink.accent_muted : with_alpha(ink.raised, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, fill);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              binding.locked ? with_alpha(ink.accent, 0.24f)
                                             : ImGui::GetColorU32(ImGuiCol_HeaderHovered));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, with_alpha(ink.accent, 0.3f));
        ImGui::PushStyleColor(ImGuiCol_Text, binding.locked ? ink.accent_ink : ink.muted);
        if (ImGui::SmallButton(label)) {
            binding.locked = !binding.locked;
            changed = true;
        }
        ImGui::PopStyleColor(4);
    }

    ImGui::PopID();
    (void)app;
    (void)shader_id;
    return changed;
}

// Writes one member's value into the packed uniform block at its reflected
// offset. Anything unbound stays zero rather than being skipped, so the struct
// always matches what the shader expects.
void write_member(std::vector<std::uint8_t>& bytes, const UniformMember& member,
                  const Binding& binding, App& app, const PreviewSettings& preview,
                  const TextureFeed* channels) {
    const std::uint32_t components = std::max<std::uint32_t>(1u, member.component_count());
    const std::uint32_t elements = std::max<std::uint32_t>(1u, member.array_size);
    // An array member does not pack its elements end to end: the layout pads
    // each one out to its own aligned slot, so a float[4] occupies 64 bytes and
    // not 16. The step is taken from the reflected size rather than from the
    // padding rule, so a block laid out by some other rule still lands on the
    // right bytes.
    const std::uint32_t stride = (member.array_size > 0 && member.size >= member.array_size)
                                     ? member.size / member.array_size
                                     : components * 4;

    for (std::uint32_t element = 0; element < elements; ++element) {
        for (std::uint32_t component = 0; component < components; ++component) {
            const std::uint32_t offset = member.offset + element * stride + component * 4;
            if (offset + 4 > bytes.size()) return;

            double value = 0.0;
            switch (binding.source) {
                case BindingSource::Manual: {
                    // Manual values are a flat list across the whole member, so
                    // an array's second element continues where the first ended.
                    const std::size_t flat =
                        static_cast<std::size_t>(element) * components + component;
                    if (flat < binding.values.size()) value = binding.values[flat];
                    break;
                }
                case BindingSource::Macro:
                    value = macro_value(binding.text, element, component, app, preview, channels);
                    break;
                default:
                    break;
            }

            if (member.type == ScalarType::Int || member.type == ScalarType::Bool) {
                const std::int32_t v = static_cast<std::int32_t>(value);
                std::memcpy(bytes.data() + offset, &v, 4);
            } else if (member.type == ScalarType::UInt) {
                const std::uint32_t v = static_cast<std::uint32_t>(std::max(0.0, value));
                std::memcpy(bytes.data() + offset, &v, 4);
            } else {
                const float v = static_cast<float>(value);
                std::memcpy(bytes.data() + offset, &v, 4);
            }
        }
    }
}

std::vector<std::uint8_t> pack_block(const UniformBlock& block, const ShaderBindings* bindings,
                                     App& app, const PreviewSettings& preview,
                                     const TextureFeed* channels) {
    std::vector<std::uint8_t> bytes(block.size ? block.size : 16, 0);
    for (const auto& member : block.members) {
        Binding binding;
        const std::string qualified = block.name + "." + member.name;
        if (bindings) {
            auto it = bindings->uniforms.find(qualified);
            if (it == bindings->uniforms.end()) it = bindings->uniforms.find(member.name);
            if (it != bindings->uniforms.end()) binding = it->second;
        }
        // A square matrix nobody has bound is the identity, not zero. Zero is
        // not a neutral transform, it is the one that collapses every vertex
        // onto the origin - so the preview would go black and the shader would
        // look broken when all that is missing is a binding. Matrices are also
        // left out of the auto-map below on purpose: a macro is a scalar
        // expression, and spreading one scalar across sixteen components is
        // never what the author meant.
        const bool square_matrix = member.rows > 1 && member.rows == member.cols;
        if (binding.source == BindingSource::Default && square_matrix) {
            binding.source = BindingSource::Manual;
            binding.values.assign(static_cast<std::size_t>(member.rows) * member.cols, 0.0);
            for (std::uint32_t i = 0; i < member.rows; ++i) {
                binding.values[static_cast<std::size_t>(i) * member.cols + i] = 1.0;
            }
        } else if (binding.source == BindingSource::Default &&
                   app.project().auto_map_macros_by_name) {
            // Auto-map by name: a member called `time` gets the time macro
            // unless the user bound it to something else.
            binding.source = BindingSource::Macro;
            binding.text = member.name;
        }
        write_member(bytes, member, binding, app, preview, channels);
    }
    return bytes;
}

}  // namespace

TextureFeed evaluate_textures(App& app) {
    return evaluate_textures_for(app, app.preview_fragment_choice());
}

TextureFeed evaluate_textures_for(App& app, const std::string& fragment_id) {
    TextureFeed feed;
    TextureRegistry* registry = app.textures();
    if (!registry || !registry->ready() || !app.project_open()) return feed;

    // Only the shader being asked about. Resolving every open document's
    // textures would decode images for shaders nobody is looking at.
    const Document* doc = app.find_document(fragment_id);
    if (!doc || !doc->compiled_ok) return feed;

    const auto bindings_it = app.project().bindings.find(doc->id);
    const ShaderBindings* bindings =
        bindings_it == app.project().bindings.end() ? nullptr : &bindings_it->second;

    for (const Resource& resource : doc->reflection.resources) {
        if (resource.kind != ResourceKind::SampledTexture) continue;

        // A resource declared as an array is several textures under one name,
        // each on its own slot and each bound separately.
        const std::uint32_t elements = resource_slot_count(resource);
        for (std::uint32_t element = 0; element < elements; ++element) {
            const std::string key = resource_binding_key(resource, element);
            const std::uint32_t slot = resource.binding + element;

            Binding binding;
            if (bindings) {
                const auto it = bindings->textures.find(key);
                if (it != bindings->textures.end()) binding = it->second;
            }
            const SamplerState state = sampler_state_from_extra(binding.extra);

            TextureStatus status = TextureStatus::Unbound;
            ResolvedTexture resolved;
            if (resource.dim != TextureDim::Tex2D) {
                // A 2D texture in a cube or 3D slot does not fail politely - the
                // pipeline refuses to build and the driver's reason for it is not
                // one anybody can act on. Say so here instead.
                status = TextureStatus::Unsupported;
                resolved = registry->fallback(state);
            } else if (binding.source == BindingSource::File && !binding.path.empty()) {
                resolved = registry->file(app.project().absolute(binding.path), state, status);
            } else if ((binding.source == BindingSource::PassOutput ||
                        binding.source == BindingSource::PreviousFrame) &&
                       !binding.text.empty()) {
                // Only for the row's benefit. What actually gets bound is
                // decided by the schedule, which knows which copy of a buffer
                // holds this frame's answer; this cannot and does not.
                resolved = registry->external(pass_target_key(binding.text, 0), state, status);
            } else if (binding.source == BindingSource::Url && !binding.text.empty()) {
                // Only ever a lookup. Resolving must not reach the network, or
                // opening a project someone sent would make this machine fetch
                // whatever addresses they put in it.
                if (const auto entry = app.assets().lookup(binding.text)) {
                    resolved = registry->file(entry->path, state, status);
                } else {
                    status = app.assets().in_flight(binding.text) ? TextureStatus::Loading
                                                                  : TextureStatus::NotCached;
                    resolved = registry->fallback(state);
                }
            } else {
                resolved = registry->fallback(state);
            }

            if (feed.fragment.size() <= slot) {
                feed.fragment.resize(slot + 1, registry->fallback());
            }
            feed.fragment[slot] = resolved;

            TextureBindingState& reported = feed.by_name[key];
            reported.status = status;
            reported.width = resolved.width;
            reported.height = resolved.height;
        }
    }
    return feed;
}

UniformFeed evaluate_uniforms_for(App& app, const std::string& vertex_id,
                                  const std::string& fragment_id, const TextureFeed& channels) {
    UniformFeed feed;
    if (!app.project_open()) return feed;
    const PreviewSettings& preview = app.project().preview;

    const auto pack_for = [&](const std::string& id, std::vector<std::uint8_t>& bytes,
                              bool& present) {
        const Document* doc = app.find_document(id);
        if (!doc || !doc->compiled_ok || doc->reflection.uniform_blocks.empty()) return;
        const auto it = app.project().bindings.find(doc->id);
        const ShaderBindings* bindings =
            it == app.project().bindings.end() ? nullptr : &it->second;
        // One block per stage: SDL pushes uniforms per stage at slot zero, and
        // nothing here has ever declared a second.
        bytes = pack_block(doc->reflection.uniform_blocks.front(), bindings, app, preview,
                           &channels);
        present = true;
    };

    pack_for(vertex_id, feed.vertex_bytes, feed.vertex_present);
    pack_for(fragment_id, feed.fragment_bytes, feed.fragment_present);
    return feed;
}

PassChain evaluate_pass_chain(App& app) {
    PassChain chain;
    if (!app.project_open()) return chain;

    const PreviewPipeline* pipeline = app.project().active_pipeline();
    if (!pipeline) return chain;

    chain.vertex_shader = pipeline->vertex;
    chain.image_shader = pipeline->fragment;
    chain.buffers = pipeline->passes;

    // Which sampler slots of which pass read another pass. Everything else is
    // left out, so the scheduler only ever sees what it has an opinion about.
    const auto gather = [&](const std::string& shader_id) {
        const Document* doc = app.find_document(shader_id);
        if (!doc || !doc->compiled_ok) return;
        const auto it = app.project().bindings.find(shader_id);
        if (it == app.project().bindings.end()) return;

        for (const Resource& resource : doc->reflection.resources) {
            if (resource.kind != ResourceKind::SampledTexture) continue;
            const std::uint32_t elements = resource_slot_count(resource);
            for (std::uint32_t element = 0; element < elements; ++element) {
                const auto binding = it->second.textures.find(
                    resource_binding_key(resource, element));
                if (binding == it->second.textures.end()) continue;
                if (binding->second.source != BindingSource::PassOutput &&
                    binding->second.source != BindingSource::PreviousFrame) {
                    continue;
                }
                if (binding->second.text.empty()) continue;

                PassInput input;
                input.slot = resource.binding + element;
                input.source_pass = binding->second.text;
                input.previous_frame =
                    binding->second.source == BindingSource::PreviousFrame;
                chain.inputs[shader_id].push_back(input);
            }
        }
    };

    for (const PassDesc& pass : chain.buffers) gather(pass.shader_id);
    gather(chain.image_shader);
    return chain;
}

std::map<std::string, PassFeed> evaluate_pass_feeds(App& app, const PassSchedule& schedule) {
    std::map<std::string, PassFeed> feeds;
    for (const ScheduledPass& pass : schedule.passes) {
        PassFeed feed;
        const TextureFeed textures = evaluate_textures_for(app, pass.shader_id);
        feed.textures = textures.fragment;
        feed.uniforms =
            evaluate_uniforms_for(app, schedule.vertex_shader, pass.shader_id, textures);
        feeds.emplace(pass.shader_id, std::move(feed));
    }
    return feeds;
}

UniformFeed evaluate_uniforms(App& app) {
    // Resolved once and handed down: the channel-size macros need it, and
    // resolving per uniform member would ask the registry the same question
    // dozens of times a frame.
    const TextureFeed channels = evaluate_textures(app);
    UniformFeed feed;
    if (!app.project_open()) return feed;
    const PreviewSettings& preview = app.project().preview;

    for (auto& doc : app.documents()) {
        if (!doc.compiled_ok || doc.reflection.uniform_blocks.empty()) continue;
        const ShaderBindings* bindings = nullptr;
        auto it = app.project().bindings.find(doc.id);
        if (it != app.project().bindings.end()) bindings = &it->second;

        // Slot 0 only for now: the preview pushes the first uniform block of each
        // stage, which covers everything the built-in preview meshes need.
        const UniformBlock& block = doc.reflection.uniform_blocks.front();
        if (doc.stage == Stage::Vertex && !feed.vertex_present) {
            feed.vertex_bytes = pack_block(block, bindings, app, preview, &channels);
            feed.vertex_present = true;
        } else if (doc.stage == Stage::Fragment && !feed.fragment_present) {
            feed.fragment_bytes = pack_block(block, bindings, app, preview, &channels);
            feed.fragment_present = true;
        }
    }
    return feed;
}

void draw_io_panel(App& app) {
    PanelScope panel("Inputs & Outputs", &app.show_io);
    if (!panel) return;

    Document* doc = app.active_document();
    if (!doc) {
        ImGui::TextDisabled("Select a shader in the Editor.");
        return;
    }
    if (!doc->compiled_ok) {
        ImGui::TextDisabled("%s has not compiled yet; nothing to bind.", doc->id.c_str());
        return;
    }

    ShaderBindings& bindings = app.project().bindings[doc->id];
    const Reflection& reflection = doc->reflection;
    bool changed = false;
    const ThemeInk ink(app.theme());

    const bool show_registers = app.settings().ui.show_register_hints;

    // What is being bound, and how much of it: the shader by the name its file
    // has, and the count of rows below.
    {
        std::size_t rows = 0;
        for (const auto& block : reflection.uniform_blocks) rows += block.members.size();
        for (const auto& resource : reflection.resources) {
            rows += resource.kind == ResourceKind::SampledTexture ? resource_slot_count(resource) : 1u;
        }
        const std::string name =
            shader_display_name(doc->path, doc->id) + "." + stage_suffix(doc->stage);
        mono_text(name, ink.text, 12.0f);
        ImGui::SameLine(0.0f, design_px(10.0f));
        FontScope small(nullptr, 12.0f);
        colored_text("from reflection \xC2\xB7 " + std::to_string(rows) +
                         (rows == 1 ? " binding" : " bindings"),
                     ink.muted);
    }
    ImGui::Dummy(ImVec2(0.0f, design_px(2.0f)));

    // The five columns every binding table shares. Separated by hairlines
    // rather than striped, so a row with a two-line type does not read as two.
    const auto begin_bindings_table = [&](const char* id, const char* second_column) {
        ImGui::PushStyleColor(ImGuiCol_TableBorderLight, ink.subtle);
        const bool open = ImGui::BeginTable(
            id, 5, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH,
            ImVec2(section_content_width(), 0.0f));
        ImGui::PopStyleColor();
        if (!open) return false;
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.28f);
        ImGui::TableSetupColumn(second_column, ImGuiTableColumnFlags_WidthStretch, 0.18f);
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch, 0.2f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.26f);
        ImGui::TableSetupColumn("##lock", ImGuiTableColumnFlags_WidthFixed, design_px(58.0f));
        caps_headers_row(ink);
        return true;
    };

    // What the shader's uniform blocks are called in its own language.
    const ShaderDesc* desc = app.project().find_shader(doc->id);
    const bool glsl = desc != nullptr && app.project().language_of(*desc) == Language::GLSL;

    // --- uniforms ----------------------------------------------------------
    for (const auto& block : reflection.uniform_blocks) {
        SectionHeader header;
        header.title = block.name;
        if (show_registers) {
            header.meta = "space" + std::to_string(block.set) + " \xC2\xB7 b" +
                          std::to_string(block.binding) + " \xC2\xB7 " + std::to_string(block.size) +
                          " B";
        }
        header.tag = glsl ? "uniform" : "cbuffer";
        const std::string id = "##uniforms_" + block.name;
        if (section_begin(id.c_str(), header, ink) && begin_bindings_table("##uniforms", "Type")) {
            for (const auto& member : block.members) {
                const std::string key = block.name + "." + member.name;
                changed |= binding_row(app, doc->id, member.c_type(), std::string(), key, &member,
                                       bindings.uniforms[key], ink);
            }
            ImGui::EndTable();
        }
        section_end();
    }

    // --- resources ---------------------------------------------------------
    if (!reflection.resources.empty()) {
        // How many of them someone has bound to something, and which register
        // space they share when they share one - which they almost always do.
        int bound = 0;
        bool one_space = true;
        for (const auto& resource : reflection.resources) {
            one_space = one_space && resource.set == reflection.resources.front().set;
            const auto& group = resource.kind == ResourceKind::StorageBuffer ? bindings.buffers
                                                                             : bindings.textures;
            const std::uint32_t elements =
                resource.kind == ResourceKind::SampledTexture ? resource_slot_count(resource) : 1u;
            for (std::uint32_t element = 0; element < elements; ++element) {
                const std::string key = resource.kind == ResourceKind::SampledTexture
                                            ? resource_binding_key(resource, element)
                                            : resource.name;
                const auto it = group.find(key);
                if (it != group.end() && it->second.source != BindingSource::Default) ++bound;
            }
        }

        SectionHeader header;
        header.title = "Resources";
        if (show_registers && one_space) {
            header.meta = "space" + std::to_string(reflection.resources.front().set);
        }
        header.tag = std::to_string(bound) + " bound";
        if (section_begin("##resources_section", header, ink) &&
            begin_bindings_table("##resources", "Kind")) {
            const TextureFeed resolved = evaluate_textures(app);
            for (const auto& resource : reflection.resources) {
                // An array of textures gets a row per element: they are separate
                // textures on separate slots, and there is no reason they should
                // have to agree on what they show or how they are sampled.
                const std::uint32_t elements =
                    resource.kind == ResourceKind::SampledTexture ? resource_slot_count(resource)
                                                                  : 1u;
                for (std::uint32_t element = 0; element < elements; ++element) {
                    const std::string row_key =
                        resource.kind == ResourceKind::SampledTexture
                            ? resource_binding_key(resource, element)
                            : resource.name;
                    std::string detail;
                    if (show_registers) {
                        detail = "space" + std::to_string(resource.set) + ", " +
                                 std::to_string(resource.binding + element);
                    }
                    // What the binding actually resolved to, said where the row can
                    // show it. A texture that is missing, still decoding or in a
                    // slot nothing can fill otherwise just looks like white.
                    if (const auto it = resolved.by_name.find(row_key);
                        it != resolved.by_name.end()) {
                        std::string state;
                        if (it->second.status == TextureStatus::Ready) {
                            state = std::to_string(it->second.width) + "x" +
                                    std::to_string(it->second.height);
                        } else if (it->second.status != TextureStatus::Unbound) {
                            state = std::string(to_string(it->second.status));
                        }
                        if (!state.empty()) detail += (detail.empty() ? "" : " \xC2\xB7 ") + state;
                    }
                    auto& group = resource.kind == ResourceKind::StorageBuffer ? bindings.buffers
                                                                               : bindings.textures;
                    changed |= binding_row(app, doc->id, std::string(to_string(resource.kind)),
                                           detail, row_key, nullptr, group[row_key], ink, &resource);
                }
            }
            ImGui::EndTable();
        }
        section_end();
    }

    // --- vertex inputs and outputs ----------------------------------------
    if (!reflection.vertex_inputs.empty()) {
        SectionHeader header;
        header.title = "Vertex inputs";
        header.meta = std::to_string(reflection.vertex_inputs.size()) +
                      (reflection.vertex_inputs.size() == 1 ? " attribute" : " attributes");
        header.tag = "input layout";
        header.default_open = false;
        if (section_begin("##vertex_inputs", header, ink)) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + section_content_width());
            for (const auto& input : reflection.vertex_inputs) {
                MonoScope mono(12.0f);
                ImGui::BulletText("%u  %s%s%s  ->  %s", input.location, input.name.c_str(),
                                  input.semantic.empty() ? "" : " : ", input.semantic.c_str(),
                                  input.sdl_vertex_format().c_str());
            }
            ImGui::TextDisabled(
                "The preview generates its geometry in the shader, so these are documented rather "
                "than bound. The build emits a matching SDL_GPUVertexInputState snippet.");
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0.0f, design_px(4.0f)));
        }
        section_end();
    }

    if (!reflection.outputs.empty()) {
        SectionHeader header;
        header.title = "Outputs";
        header.meta = std::to_string(reflection.outputs.size()) +
                      (reflection.outputs.size() == 1 ? " target" : " targets");
        header.tag = "SV_Target" + std::to_string(reflection.outputs.front().location);
        header.default_open = false;
        if (section_begin("##outputs", header, ink)) {
            for (const auto& output : reflection.outputs) {
                MonoScope mono(12.0f);
                ImGui::BulletText("SV_Target%u  %s (%u components)", output.location,
                                  output.name.c_str(), output.components);
            }
            ImGui::Dummy(ImVec2(0.0f, design_px(4.0f)));
        }
        section_end();
    }

    if (doc->stage == Stage::Compute) {
        ImGui::SeparatorText("Compute");
        ImGui::Text("Thread group: %u x %u x %u", reflection.compute_threads[0],
                    reflection.compute_threads[1], reflection.compute_threads[2]);
        ImGui::Text("Read-only: %u texture(s), %u buffer(s)",
                    reflection.num_readonly_storage_textures(),
                    reflection.num_readonly_storage_buffers());
        ImGui::Text("Read-write: %u texture(s), %u buffer(s)",
                    reflection.num_readwrite_storage_textures(),
                    reflection.num_readwrite_storage_buffers());
    }

    if (changed) {
        Diagnostics diags;
        save_project(app.project(), diags);
    }

}

}  // namespace ssstudio::gui
