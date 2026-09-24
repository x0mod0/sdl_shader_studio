// The app's menus in the macOS menu bar.
//
// SDL puts two menus there when it starts - the application menu (About, Hide,
// Quit) and a Window menu (Close, Minimize, Zoom, Full Screen) - and offers no
// way to add more. This file adds the app's own around them: File, Build, View
// and Info as menus of their own, the app's Window entries inside the Window
// menu SDL made rather than in a second one, and Settings in the application
// menu, in the placeholder SDL leaves there for it.
//
// Shortcuts are shown here and never acted on. SDL hands every key press to the
// app before AppKit gets to look for a menu item with that shortcut
// (SDL3Application's sendEvent: does its own handling first), so by the time a
// menu item would react, App::handle_shortcuts() has already run the action.
// Were the item to run it as well, every shortcut would happen twice - and
// Cmd+, would open Settings and close it again in the same keystroke.
#import <AppKit/AppKit.h>
#include <objc/runtime.h>  // sel_isEqual

#include <algorithm>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "native_menu.h"

using ssstudio::gui::Menu;
using ssstudio::gui::MenuItem;

/// What every menu item this file makes reports its pick to, and the delegate
/// that fills a submenu each time it opens.
@interface SSMenuTarget : NSObject <NSMenuDelegate>
- (void)pick:(NSMenuItem*)sender;
@end

namespace {

/// Marks every item this file puts in the bar, so a rebuild can take them out
/// again - including the ones inside SDL's Window menu - and leave SDL's own
/// where they are.
constexpr NSInteger kOwnTag = 0x53534D4E;  // "SSMN"

/// The keys that choose the highlighted item of an open menu, as virtual key
/// codes (kVK_Return, kVK_ANSI_KeypadEnter and kVK_Space in Carbon's Events.h).
constexpr unsigned short kReturnKey = 36;
constexpr unsigned short kKeypadEnterKey = 76;
constexpr unsigned short kSpaceKey = 49;

/// The title of the app's menu whose entries go into SDL's Window menu.
constexpr const char* kWindowMenuTitle = "Window";

struct State {
    /// Created on the first publish, and kept: every item holds it only weakly.
    SSMenuTarget* target = nil;

    /// The menus as last published: what picks, enabled states and submenus
    /// are looked up in, by id. Replaced every frame, so a pick always runs the
    /// newest version of what it picked.
    std::vector<Menu> menus;

    /// The entries of every submenu opened since the last rebuild, keyed by the
    /// submenu's id, as produced when it opened - so a pick from one can find
    /// what it picked.
    std::map<std::string, std::vector<MenuItem>> opened;

    /// Everything about `menus` the bar shows, as one string. Only a change to
    /// it rebuilds the bar: the app publishes every frame, and a menu bar
    /// rebuilt sixty times a second would be work for nothing.
    std::string shown;

    /// The menu titles in the order the bar was laid out for, and the menus
    /// this file added to it, by title. A change of titles lays the bar out
    /// again; anything else only refills the menus.
    std::vector<std::string> laid_out;
    std::map<std::string, NSMenu*> own_menus;

    /// Picks waiting for run_picked().
    std::vector<std::function<void()>> picked;
};

State& state() {
    static State s;
    return s;
}

NSString* ns(const std::string& text) {
    // nil for text that is not UTF-8, which AppKit would not take either.
    NSString* out = [NSString stringWithUTF8String:text.c_str()];
    return out ? out : @"";
}

/// A label the way a Mac menu spells it: "..." is one character there.
NSString* title_of(const std::string& label) {
    std::string text = label;
    if (text.size() >= 3 && text.compare(text.size() - 3, 3, "...") == 0) {
        text.replace(text.size() - 3, 3, "…");
    }
    return ns(text);
}

/// Whether an entry can be picked, or a submenu opened.
bool usable(const MenuItem& item) {
    switch (item.kind) {
        case MenuItem::Kind::Action:
            return item.enabled && static_cast<bool>(item.run);
        case MenuItem::Kind::Submenu:
            return item.enabled && static_cast<bool>(item.entries);
        case MenuItem::Kind::Separator:
            break;
    }
    return false;
}

/// Everything the bar shows of `items`, appended to `out`.
void describe(const std::vector<MenuItem>& items, std::string& out) {
    for (const MenuItem& item : items) {
        out += static_cast<char>('0' + static_cast<int>(item.kind));
        out += static_cast<char>('0' + static_cast<int>(item.role));
        out += usable(item) ? '1' : '0';
        out += item.checked ? '1' : '0';
        for (const std::string* part : {&item.id, &item.label, &item.shortcut, &item.tooltip}) {
            out += *part;
            out += '\x1f';
        }
    }
}

std::string describe(const std::vector<Menu>& menus) {
    std::string out;
    for (const Menu& menu : menus) {
        out += menu.title;
        out += '\x1e';
        describe(menu.items, out);
        out += '\x1d';
    }
    return out;
}

const MenuItem* find_in(const std::vector<MenuItem>& items, const std::string& id) {
    const auto found = std::find_if(items.begin(), items.end(), [&](const MenuItem& item) {
        return item.kind != MenuItem::Kind::Separator && item.id == id;
    });
    return found == items.end() ? nullptr : &*found;
}

/// The entry with this id: in the menus themselves, or in a submenu opened
/// since the last rebuild.
const MenuItem* find(const State& s, const std::string& id) {
    for (const Menu& menu : s.menus) {
        if (const MenuItem* item = find_in(menu.items, id)) return item;
    }
    for (const auto& submenu : s.opened) {
        if (const MenuItem* item = find_in(submenu.second, id)) return item;
    }
    return nullptr;
}

/// The entry an item in the bar stands for, found by the id it carries.
const MenuItem* entry_of(const State& s, NSMenuItem* item) {
    NSString* ident = item.representedObject;
    if (![ident isKindOfClass:[NSString class]]) return nullptr;
    return find(s, ident.UTF8String);
}

/// The first entry in the menus that plays `role`.
const MenuItem* find_role(const State& s, MenuItem::Role role) {
    for (const Menu& menu : s.menus) {
        for (const MenuItem& item : menu.items) {
            if (item.role == role) return &item;
        }
    }
    return nullptr;
}

/// A shortcut the way a menu item carries it.
struct KeyEquivalent {
    NSString* key = @"";
    NSEventModifierFlags modifiers = 0;
};

/// A named key ("PageDown", "F7") as the character AppKit stands for it with,
/// or 0 for a name it has none for.
unichar named_key(const std::string& name) {
    static const std::map<std::string, unichar> kNamed = {
        {"PageUp", NSPageUpFunctionKey},  {"PageDown", NSPageDownFunctionKey},
        {"Home", NSHomeFunctionKey},      {"End", NSEndFunctionKey},
        {"Up", NSUpArrowFunctionKey},     {"Down", NSDownArrowFunctionKey},
        {"Left", NSLeftArrowFunctionKey}, {"Right", NSRightArrowFunctionKey},
        {"Delete", NSDeleteFunctionKey},  {"Backspace", NSBackspaceCharacter},
        {"Tab", NSTabCharacter},          {"Enter", NSCarriageReturnCharacter},
        {"Escape", 0x1B},                 {"Space", ' '},
    };
    if (const auto found = kNamed.find(name); found != kNamed.end()) return found->second;
    if (name.size() >= 2 && name.size() <= 3 && name[0] == 'F' &&
        std::all_of(name.begin() + 1, name.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        const int number = std::stoi(name.substr(1));
        if (number >= 1 && number <= 35) return static_cast<unichar>(NSF1FunctionKey + number - 1);
    }
    return 0;
}

/// A shortcut as the app spells it ("Ctrl+Shift+N", "Ctrl+,", "F7") as a menu
/// item shows it. The app's Ctrl is the Command key here: ImGui reads Command
/// as Ctrl on macOS, which is why Cmd+N is what handle_shortcuts() answers to.
/// A shortcut this cannot spell comes back empty - no shortcut shown is better
/// than a wrong one.
KeyEquivalent key_equivalent(const std::string& shortcut) {
    // Split on '+', where a '+' with nothing before it is the key itself.
    std::vector<std::string> parts;
    std::string part;
    for (const char c : shortcut) {
        if (c == '+' && !part.empty()) {
            parts.push_back(part);
            part.clear();
        } else {
            part += c;
        }
    }
    if (!part.empty()) parts.push_back(part);
    if (parts.empty()) return {};

    KeyEquivalent out;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        const std::string& modifier = parts[i];
        if (modifier == "Ctrl" || modifier == "Cmd" || modifier == "Super") {
            out.modifiers |= NSEventModifierFlagCommand;
        } else if (modifier == "Shift") {
            out.modifiers |= NSEventModifierFlagShift;
        } else if (modifier == "Alt" || modifier == "Option") {
            out.modifiers |= NSEventModifierFlagOption;
        } else {
            return {};
        }
    }

    // A letter is given lower case, with Shift spelled out as a modifier: an
    // upper case key equivalent would add a Shift of its own.
    NSString* key = ns(parts.back());
    if (key.length == 1) {
        out.key = key.lowercaseString;
    } else if (const unichar named = named_key(parts.back())) {
        out.key = [NSString stringWithCharacters:&named length:1];
    } else {
        return {};
    }
    return out;
}

/// Whether an item that was just chosen was chosen by its shortcut rather than
/// from the open menu. Both can come from a key press - Return chooses the
/// highlighted item of an open menu - so it comes down to which key it was.
bool chosen_by_shortcut(NSMenuItem* item) {
    if (item.keyEquivalent.length == 0) return false;
    NSEvent* event = NSApp.currentEvent;
    if (event.type != NSEventTypeKeyDown) return false;
    const unsigned short key = event.keyCode;
    return key != kReturnKey && key != kKeypadEnterKey && key != kSpaceKey;
}

/// The application menu, first in the bar and named after the app.
NSMenu* app_menu() {
    NSMenu* bar = NSApp.mainMenu;
    return bar.numberOfItems > 0 ? [bar itemAtIndex:0].submenu : nil;
}

/// The place SDL leaves for Settings in the application menu: an item with
/// Cmd+, as its shortcut and, until this file gives it one, nothing to do.
NSMenuItem* settings_slot() {
    for (NSMenuItem* item in app_menu().itemArray) {
        if ([item.keyEquivalent isEqualToString:@","]) return item;
    }
    return nil;
}

/// SDL's Quit, in the application menu. It asks the app to quit the way File >
/// Quit does - SDL makes it an SDL_EVENT_QUIT, and the main loop a
/// request_quit() - and it is left as SDL made it, so Cmd+Q keeps working the
/// way it always has, dialogs up or not.
NSMenuItem* quit_item() {
    for (NSMenuItem* item in app_menu().itemArray) {
        if (item.action == @selector(terminate:)) return item;
    }
    return nil;
}

/// Whether an entry is shown in the menu the app put it in. Settings and Quit
/// are not, on a Mac, when the application menu has a place for them.
bool shown_in_place(const MenuItem& item) {
    switch (item.role) {
        case MenuItem::Role::Settings:
            return settings_slot() == nil;
        case MenuItem::Role::Quit:
            return quit_item() == nil;
        case MenuItem::Role::Normal:
            break;
    }
    return true;
}

NSMenuItem* own_separator() {
    NSMenuItem* item = [NSMenuItem separatorItem];
    item.tag = kOwnTag;
    return item;
}

/// Points an item at `entry`: its shortcut, state and the id a pick is looked
/// up by.
void wire(NSMenuItem* item, const MenuItem& entry, SSMenuTarget* target) {
    const KeyEquivalent key = key_equivalent(entry.shortcut);
    item.keyEquivalent = key.key;
    item.keyEquivalentModifierMask = key.modifiers;
    item.target = target;
    item.action = @selector(pick:);
    item.state = entry.checked ? NSControlStateValueOn : NSControlStateValueOff;
    item.representedObject = ns(entry.id);
    item.toolTip = entry.tooltip.empty() ? nil : ns(entry.tooltip);
}

NSMenuItem* make_item(const State& s, const MenuItem& entry) {
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title_of(entry.label)
                                                  action:nil
                                           keyEquivalent:@""];
    if (entry.kind == MenuItem::Kind::Submenu) {
        // Empty until it opens: menuNeedsUpdate: fills it then, from whatever
        // the entries are at that moment.
        NSMenu* submenu = [[NSMenu alloc] initWithTitle:item.title];
        submenu.autoenablesItems = NO;
        submenu.delegate = s.target;
        item.submenu = submenu;
        item.representedObject = ns(entry.id);
    } else {
        wire(item, entry, s.target);
    }
    item.enabled = usable(entry);
    item.tag = kOwnTag;
    return item;
}

/// Adds `entries` to `menu` from index `at` on, and returns the index after
/// the last one added. Separators are only added between entries that are
/// shown, so the ones that leave elsewhere take no dividing lines with them.
NSInteger fill(const State& s, NSMenu* menu, NSInteger at, const std::vector<MenuItem>& entries) {
    bool any = false;
    bool separate = false;
    for (const MenuItem& entry : entries) {
        if (entry.kind == MenuItem::Kind::Separator) {
            separate = any;
            continue;
        }
        if (!shown_in_place(entry)) continue;
        if (separate) {
            [menu insertItem:own_separator() atIndex:at++];
            separate = false;
        }
        [menu insertItem:make_item(s, entry) atIndex:at++];
        any = true;
    }
    return at;
}

/// Where the app's entries go in SDL's Window menu: after SDL's own (Close,
/// Minimize, Zoom, Full Screen) and before what macOS adds at the end by itself
/// (Bring All to Front, the list of open windows).
NSInteger window_entries_at(NSMenu* menu) {
    const SEL sdl_actions[] = {@selector(performClose:), @selector(performMiniaturize:),
                               @selector(performZoom:), @selector(toggleFullScreen:)};
    NSInteger at = 0;
    for (NSInteger i = 0; i < menu.numberOfItems; ++i) {
        const SEL action = [menu itemAtIndex:i].action;
        for (const SEL sdl_action : sdl_actions) {
            if (sel_isEqual(action, sdl_action)) at = i + 1;
        }
    }
    return at;
}

/// Puts the menus in the bar in the app's order, after the application menu:
/// its own menus added, and SDL's Window menu moved to where the app's is.
void lay_out(State& s) {
    NSMenu* bar = NSApp.mainMenu;
    NSMenu* windows = NSApp.windowsMenu;
    for (const auto& own : s.own_menus) {
        const NSInteger index = [bar indexOfItemWithSubmenu:own.second];
        if (index >= 0) [bar removeItemAtIndex:index];
    }
    s.own_menus.clear();
    s.laid_out.clear();

    NSInteger at = 1;
    for (const Menu& menu : s.menus) {
        s.laid_out.push_back(menu.title);
        if (windows && menu.title == kWindowMenuTitle) {
            const NSInteger index = [bar indexOfItemWithSubmenu:windows];
            if (index < 0) continue;
            NSMenuItem* item = [bar itemAtIndex:index];
            [bar removeItemAtIndex:index];
            if (index < at) --at;
            [bar insertItem:item atIndex:std::min(at, bar.numberOfItems)];
            ++at;
            continue;
        }
        NSMenu* own = [[NSMenu alloc] initWithTitle:ns(menu.title)];
        // Every entry says whether it can be picked; AppKit's own guess, from
        // whether something answers the item's action, would enable them all.
        own.autoenablesItems = NO;
        NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:own.title action:nil keyEquivalent:@""];
        item.submenu = own;
        item.tag = kOwnTag;
        [bar insertItem:item atIndex:std::min(at, bar.numberOfItems)];
        ++at;
        s.own_menus[menu.title] = own;
    }
}

/// Takes the app's shortcuts back from the items SDL put in the bar, and the
/// ones macOS adds to them - SDL's Close has Cmd+W, and macOS gives it a Close
/// All on Cmd+Option+W.
///
/// Such an item would act on top of the app: every key reaches
/// handle_shortcuts() whatever the menus do with it, so Cmd+W closed the
/// project in front and, through SDL's Close, asked the whole app to quit as
/// well - and with nothing unsaved, it did. macOS also lets only the first item
/// to claim Cmd+W show it, which would leave File's Close without its shortcut.
/// Option is ignored in the comparison because handle_shortcuts() ignores it.
/// The items keep working from the menu; only the shortcut goes.
///
/// Quit's shortcut is not taken back: SDL's Quit is the app's Quit on a Mac
/// (see quit_item), so its Cmd+Q is the one the app wants.
void release_claimed_shortcuts(const State& s) {
    std::vector<std::pair<std::string, bool>> claimed;
    for (const Menu& menu : s.menus) {
        for (const MenuItem& entry : menu.items) {
            if (entry.kind != MenuItem::Kind::Action || !shown_in_place(entry)) continue;
            const KeyEquivalent key = key_equivalent(entry.shortcut);
            if (key.key.length == 0 || !(key.modifiers & NSEventModifierFlagCommand)) continue;
            claimed.emplace_back(key.key.UTF8String, (key.modifiers & NSEventModifierFlagShift) != 0);
        }
    }
    NSMenu* const sdl_menus[] = {app_menu(), NSApp.windowsMenu};
    for (NSMenu* menu : sdl_menus) {
        for (NSMenuItem* item in menu.itemArray) {
            if (item.tag == kOwnTag || item.target == s.target) continue;
            NSString* key = item.keyEquivalent;
            const NSEventModifierFlags mask = item.keyEquivalentModifierMask;
            if (key.length == 0 || !(mask & NSEventModifierFlagCommand)) continue;
            // An upper case key equivalent carries a Shift of its own.
            NSString* lower = key.lowercaseString;
            const bool shift = (mask & NSEventModifierFlagShift) || ![lower isEqualToString:key];
            const std::pair<std::string, bool> pressed(lower.UTF8String, shift);
            if (std::find(claimed.begin(), claimed.end(), pressed) != claimed.end()) {
                item.keyEquivalent = @"";
            }
        }
    }
}

/// Gives SDL's Settings placeholder the app's Settings entry to run, named the
/// way Mac apps name it.
void fill_settings_slot(const State& s) {
    NSMenuItem* slot = settings_slot();
    const MenuItem* settings = find_role(s, MenuItem::Role::Settings);
    if (!slot || !settings) return;
    NSString* title = title_of(settings->label);
    slot.title = [title hasSuffix:@"…"] ? title : [title stringByAppendingString:@"…"];
    wire(slot, *settings, s.target);
}

void rebuild(State& s) {
    // Picks from a submenu happen while it is open, and it cannot be open
    // while the app is publishing, so nothing still needs these.
    s.opened.clear();

    std::vector<std::string> titles;
    for (const Menu& menu : s.menus) titles.push_back(menu.title);
    if (titles != s.laid_out) lay_out(s);

    NSMenu* windows = NSApp.windowsMenu;
    if (windows) {
        for (NSMenuItem* item in [windows.itemArray copy]) {
            if (item.tag == kOwnTag) [windows removeItem:item];
        }
    }
    // Before the app's items go in: a shortcut only shows on an item added
    // after whatever held it has let go.
    release_claimed_shortcuts(s);

    for (const Menu& menu : s.menus) {
        if (windows && menu.title == kWindowMenuTitle) {
            const NSInteger at = window_entries_at(windows);
            NSMenuItem* separator = at > 0 ? own_separator() : nil;
            if (separator) [windows insertItem:separator atIndex:at];
            const NSInteger start = separator ? at + 1 : at;
            if (fill(s, windows, start, menu.items) == start && separator) {
                [windows removeItem:separator];
            }
            continue;
        }
        const auto own = s.own_menus.find(menu.title);
        if (own == s.own_menus.end()) continue;
        [own->second removeAllItems];
        fill(s, own->second, 0, menu.items);
    }

    fill_settings_slot(s);
}

}  // namespace

@implementation SSMenuTarget

- (void)pick:(NSMenuItem*)sender {
    // The shortcut was pressed rather than the item chosen: handle_shortcuts()
    // has run it already (see the top of this file).
    if (chosen_by_shortcut(sender)) return;
    State& s = state();
    const MenuItem* entry = entry_of(s, sender);
    if (entry && entry->kind == MenuItem::Kind::Action && usable(*entry)) {
        s.picked.push_back(entry->run);
    }
}

// Asked about the entries that sit in SDL's menus, which enable their items
// this way; the app's own menus are told directly (autoenablesItems = NO).
- (BOOL)validateMenuItem:(NSMenuItem*)item {
    const MenuItem* entry = entry_of(state(), item);
    return entry && usable(*entry);
}

- (void)menuNeedsUpdate:(NSMenu*)menu {
    State& s = state();
    [menu removeAllItems];
    NSMenu* parent = menu.supermenu;
    const NSInteger index = parent ? [parent indexOfItemWithSubmenu:menu] : -1;
    if (index < 0) return;
    const MenuItem* entry = entry_of(s, [parent itemAtIndex:index]);
    if (!entry || !usable(*entry)) return;
    // Copied out before `opened` changes, which is where the entry may live.
    const std::string id = entry->id;
    const std::function<std::vector<MenuItem>()> produce = entry->entries;
    std::vector<MenuItem>& entries = s.opened[id];
    entries = produce();
    fill(s, menu, 0, entries);
}

// Without this, AppKit fills every submenu on every key press to search it for
// the shortcut pressed - which for Open recent means a look at the disk per
// keystroke. None of their entries has a shortcut to find.
- (BOOL)menuHasKeyEquivalent:(NSMenu*)menu
                    forEvent:(NSEvent*)event
                      target:(id _Nullable* _Nonnull)target
                      action:(SEL _Nullable* _Nonnull)action {
    (void)menu;
    (void)event;
    (void)target;
    (void)action;
    return NO;
}

@end

namespace ssstudio::gui::native_menu {

bool available() {
    // SDL makes the bar as it starts up (Cocoa_RegisterApp in SDL_Init). An app
    // started some way that left it without one keeps its menus in the window.
    return NSApp != nil && NSApp.mainMenu != nil;
}

void publish(std::vector<Menu> menus) {
    if (!available()) return;
    State& s = state();
    if (!s.target) s.target = [[SSMenuTarget alloc] init];
    std::string shown = describe(menus);
    s.menus = std::move(menus);
    if (shown == s.shown) return;
    s.shown = std::move(shown);
    rebuild(s);
}

void run_picked() {
    // Swapped out first, so a pick that leads to another cannot run it here.
    std::vector<std::function<void()>> picked;
    picked.swap(state().picked);
    for (const std::function<void()>& run : picked) run();
}

}  // namespace ssstudio::gui::native_menu
