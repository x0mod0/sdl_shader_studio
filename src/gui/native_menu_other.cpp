// Platforms without a system menu bar keep the menus in the window's top bar.
#include "native_menu.h"

namespace ssstudio::gui::native_menu {

bool available() { return false; }

void publish(std::vector<Menu>) {}

void run_picked() {}

}  // namespace ssstudio::gui::native_menu
