/// Which of a tab strip's tabs a "close" command applies to.
///
/// Header-only and free of ImGui on purpose: this is the whole of the rule
/// behind "close others", "close to the left" and the rest, and it is the part
/// worth testing without a window on screen.
#ifndef SSSTUDIO_GUI_TAB_SCOPE_H
#define SSSTUDIO_GUI_TAB_SCOPE_H

#include <algorithm>
#include <string>
#include <vector>

namespace ssstudio::gui {

/// The tabs one of the close commands names, relative to the tab the command was
/// asked for.
enum class CloseScope {
    /// Every tab except the one the menu belongs to.
    Others,
    /// The tabs shown before it.
    Left,
    /// The tabs shown after it.
    Right,
    /// Every tab, the one the menu belongs to included.
    All,
};

/// The tabs `scope` names, in the order they are shown.
///
/// `order` is the ids of the tabs on screen, left to right - which is not
/// necessarily the order their documents sit in, because the tab strip can be
/// reordered by dragging and "to the right" has to mean what it says on screen.
///
/// An `anchor` that names no tab in `order` has nothing to its left or right, so
/// Left and Right come back empty; Others and All do not depend on finding it
/// and still answer.
inline std::vector<std::string> tabs_in_scope(const std::vector<std::string>& order,
                                              const std::string& anchor, CloseScope scope) {
    std::vector<std::string> picked;
    const auto at = std::find(order.begin(), order.end(), anchor);
    for (auto it = order.begin(); it != order.end(); ++it) {
        switch (scope) {
            case CloseScope::All:
                picked.push_back(*it);
                break;
            case CloseScope::Others:
                if (it != at) picked.push_back(*it);
                break;
            case CloseScope::Left:
                if (at != order.end() && it < at) picked.push_back(*it);
                break;
            case CloseScope::Right:
                if (at != order.end() && it > at) picked.push_back(*it);
                break;
        }
    }
    return picked;
}

/// The order to show one project's tabs in, given what the editor remembers.
///
/// `remembered` is the order it was left in, `present` the shaders the project
/// actually has now, in project order. The two disagree whenever a project has
/// changed between sessions, and the rule is that what is remembered wins for
/// the shaders it names, while everything else keeps its project order behind
/// them - so a shader added since arrives at the end rather than nowhere, and
/// one that has been deleted simply drops out.
inline std::vector<std::string> merge_tab_order(const std::vector<std::string>& remembered,
                                                const std::vector<std::string>& present) {
    std::vector<std::string> merged;
    merged.reserve(present.size());
    for (const std::string& id : remembered) {
        // Only shaders that are still here, and never twice, however the
        // remembered list got that way.
        if (std::find(present.begin(), present.end(), id) == present.end()) continue;
        if (std::find(merged.begin(), merged.end(), id) != merged.end()) continue;
        merged.push_back(id);
    }
    for (const std::string& id : present) {
        if (std::find(merged.begin(), merged.end(), id) == merged.end()) merged.push_back(id);
    }
    return merged;
}

}  // namespace ssstudio::gui

#endif  // SSSTUDIO_GUI_TAB_SCOPE_H
