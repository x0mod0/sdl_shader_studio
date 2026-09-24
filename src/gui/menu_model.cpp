#include "menu_model.h"

#include <imgui.h>

namespace ssstudio::gui {
namespace {

/// Draws one menu's entries. A pick is recorded rather than run, so nothing an
/// entry does can change the lists being walked while they are being walked.
void draw_entries(const std::vector<MenuItem>& items, std::function<void()>& picked) {
    for (const MenuItem& item : items) {
        switch (item.kind) {
            case MenuItem::Kind::Separator:
                ImGui::Separator();
                break;
            case MenuItem::Kind::Submenu:
                ImGui::PushID(item.id.c_str());
                if (ImGui::BeginMenu(item.label.c_str(), item.enabled)) {
                    if (item.entries) draw_entries(item.entries(), picked);
                    ImGui::EndMenu();
                }
                ImGui::PopID();
                break;
            case MenuItem::Kind::Action:
                ImGui::PushID(item.id.c_str());
                if (ImGui::MenuItem(item.label.c_str(),
                                    item.shortcut.empty() ? nullptr : item.shortcut.c_str(),
                                    item.checked, item.enabled)) {
                    picked = item.run;
                }
                if (!item.tooltip.empty() &&
                    ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("%s", item.tooltip.c_str());
                }
                ImGui::PopID();
                break;
        }
    }
}

}  // namespace

void draw_imgui_menus(const std::vector<Menu>& menus) {
    std::function<void()> picked;
    for (const Menu& menu : menus) {
        if (ImGui::BeginMenu(menu.title.c_str())) {
            draw_entries(menu.items, picked);
            ImGui::EndMenu();
        }
    }
    if (picked) picked();
}

}  // namespace ssstudio::gui
