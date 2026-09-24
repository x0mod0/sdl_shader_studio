// The app's menus in the platform's own menu bar, where it has one.
//
// macOS has a single menu bar at the top of the screen, shared by every window
// of the app in front, and a Mac app that drew File and View inside its window
// as well would have them twice. Neither SDL nor ImGui can put anything there,
// so native_menu_mac.mm does it with AppKit. Elsewhere available() says no and
// the menus stay in the window's top bar, drawn by ImGui.
//
// Keyboard shortcuts are not handled here on any platform. The menus show them;
// App::handle_shortcuts() is what acts on them, the same way everywhere.
#ifndef SSSTUDIO_GUI_NATIVE_MENU_H
#define SSSTUDIO_GUI_NATIVE_MENU_H

#include <vector>

#include "menu_model.h"

namespace ssstudio::gui::native_menu {

/// Whether the menus belong in the system menu bar rather than the window.
bool available();

/// Makes the system menu bar show `menus`. Meant to be called every frame: the
/// bar itself is only rebuilt when something it shows has changed, and the
/// latest description is kept for picks to be looked up in.
void publish(std::vector<Menu> menus);

/// Runs whatever was picked from the system menu bar since the last call, in
/// the order it was picked.
///
/// A menu is chosen from while the app is not drawing - AppKit runs the menu
/// while the event loop waits - so the choice is queued, and run here, inside
/// a frame, where picking from the ImGui menus would have run it too.
void run_picked();

}  // namespace ssstudio::gui::native_menu

#endif  // SSSTUDIO_GUI_NATIVE_MENU_H
