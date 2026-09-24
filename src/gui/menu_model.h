// The application's menus, described rather than drawn.
//
// One description, two ways to show it: drawn with ImGui into the window's top
// bar, or handed to the platform's own menu bar - on macOS, the menus belong at
// the top of the screen, and a window that carried its own menu bar there would
// have two. Keeping the menus as data is what lets both show exactly the same
// items, enabled and checked the same way, running the same code.
#ifndef SSSTUDIO_GUI_MENU_MODEL_H
#define SSSTUDIO_GUI_MENU_MODEL_H

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ssstudio::gui {

/// One entry in a menu.
struct MenuItem {
    enum class Kind {
        /// Runs `run` when picked.
        Action,
        /// A dividing line.
        Separator,
        /// Opens a further menu, whose entries `entries` produces.
        Submenu,
    };
    Kind kind = Kind::Action;

    /// What the entry is, for a platform that has a place of its own for it.
    /// macOS keeps Settings and Quit in the application menu, under the app's
    /// name, rather than in File; elsewhere the role changes nothing. (Qt calls
    /// the same idea a menu role.)
    enum class Role {
        Normal,
        Settings,
        Quit,
    };
    Role role = Role::Normal;

    /// Stable identity, unique among the menus. What a native menu reports a
    /// pick by, and what ImGui keys the item's widget on - so two entries may
    /// share a label (two projects of one name) without sharing an id.
    std::string id;
    std::string label;
    /// The shortcut the way the app spells it everywhere: "Ctrl+Shift+N",
    /// "F7". Shown beside the label; empty for none. On macOS "Ctrl" is the
    /// Command key, as it is for every shortcut ImGui handles there.
    std::string shortcut;
    /// Shown on hover, when not empty.
    std::string tooltip;
    bool enabled = true;
    /// Draws a check mark: for entries that show a state as well as change it.
    bool checked = false;

    /// What picking the entry does. Always run outside the menu's own drawing,
    /// after the frame's menus are done, so it may change anything the menus
    /// were built from.
    std::function<void()> run;

    /// A submenu's entries, produced when it opens rather than when the menus
    /// are built. The lists behind them - recent projects, closed shaders -
    /// cost a look at the disk to describe, which is not worth paying every
    /// frame for a menu nobody has opened.
    std::function<std::vector<MenuItem>()> entries;
};

/// One menu in the bar.
struct Menu {
    std::string title;
    std::vector<MenuItem> items;
};

inline MenuItem menu_action(std::string id, std::string label, std::string shortcut,
                            std::function<void()> run, bool enabled = true, bool checked = false) {
    MenuItem item;
    item.id = std::move(id);
    item.label = std::move(label);
    item.shortcut = std::move(shortcut);
    item.run = std::move(run);
    item.enabled = enabled;
    item.checked = checked;
    return item;
}

/// `item`, marked as playing `role`.
inline MenuItem with_role(MenuItem item, MenuItem::Role role) {
    item.role = role;
    return item;
}

inline MenuItem menu_separator() {
    MenuItem item;
    item.kind = MenuItem::Kind::Separator;
    return item;
}

inline MenuItem menu_submenu(std::string id, std::string label,
                             std::function<std::vector<MenuItem>()> entries, bool enabled = true) {
    MenuItem item;
    item.kind = MenuItem::Kind::Submenu;
    item.id = std::move(id);
    item.label = std::move(label);
    item.entries = std::move(entries);
    item.enabled = enabled;
    return item;
}

/// Draws the menus into the current ImGui menu bar, and runs whatever was
/// picked once they are all drawn.
void draw_imgui_menus(const std::vector<Menu>& menus);

}  // namespace ssstudio::gui

#endif  // SSSTUDIO_GUI_MENU_MODEL_H
