# Theme pack format (`.s3theme`, v1)

A theme pack is a TOML file that repaints the application. It is data, loaded at
runtime, and it is the only supported way to add a look that is not one of the
three built in.

The format is layered on purpose. A pack that sets twenty semantic colours gets
a complete, coherent interface, because every one of the sixty-odd widget
colours has a documented derivation from those twenty. A pack that wants to
place one specific pixel can still address that pixel directly. Neither case
pays for the other.

## What a pack may and may not change

A theme owns *chrome*: the colours and metrics of the application's own surfaces.
It does not own content, and it does not own behaviour.

| May set | May not set | Why |
|---|---|---|
| Widget and panel colours | `preview.clear_color` | The clear colour is a *project* field (`project.h`), part of what the shader is being viewed against. A theme that changed it would change the picture, not the frame. |
| Corner radii, padding, spacing | `DockingNodeHasCloseButton` | Set to `false` in `theme.cpp` because two close buttons a few pixels apart close different amounts. That is a safety decision wearing a style field's clothes. |
| Syntax colours, diagnostic ink | `ui.ui_scale`, `editor.font_size` | Accessibility settings belong to the person at the keyboard. A pack may *suggest* a font (see `[font]`); it may not resize the interface. |
| A bundled font file | Any path outside the pack directory | Portability: a pack that reads `/Users/someone/…` works on exactly one machine. |
| Preview chrome (checkerboard) | Anything under `[files]`, `[tools]`, `[build]` | Those are not looks. |

Rejected keys are a warning, not an error: the pack still loads, minus that key.
A pack is cosmetic, and refusing to start over a cosmetic mistake would be a
worse failure than the mistake.

## Where packs live

Discovery walks three roots, in this order:

| Order | Root | Purpose |
|---|---|---|
| 1 | `<app>/themes/` — beside the executable, inside `SS Studio.app/Contents/Resources/themes/` on macOS | Packs that ship with the application |
| 2 | `<settings dir>/themes/` — beside `settings.toml` and `layout.ini` | The user's own packs |
| 3 | `<project>/themes/` | Packs a project carries for the people working on it |

Later roots win. A user pack whose id matches a bundled one replaces it, and a
project pack replaces both — the same precedence the settings file already has
over built-in defaults, so there is one rule to remember rather than two.

The settings dir is `%APPDATA%/sdl-shader-studio/` on Windows,
`~/Library/Application Support/sdl-shader-studio/` on macOS, and
`$XDG_CONFIG_HOME/sdl-shader-studio/` (else `~/.config/…`) elsewhere.

### Two shapes

A pack is either one file or one directory:

```
themes/midnight.s3theme            # single file: colours and metrics only
themes/midnight.s3theme/           # directory
  theme.toml                       #   required, same schema as the file form
  fonts/Inter-Regular.ttf          #   optional, referenced by [font]
  preview.png                      #   optional, 640x400, shown in the picker
```

The id is the stem: `midnight.s3theme` and `midnight.s3theme/` are both the
theme `midnight`. Ids are matched case-insensitively and must be
`[a-z0-9][a-z0-9_-]*` after lowercasing, because the id is what
`editor.color_theme` records in `settings.toml` and a settings file should not
depend on the case of a filename.

The three built-in themes keep the ids `dark`, `light` and `classic`. A pack may
not claim those ids; it can `inherit` from them instead.

## The cascade

Four layers, resolved in order. Each may reference anything defined in a layer
above it.

```
[palette]   named literal colours          "the ten colours this theme is made of"
   |
[roles]     semantic assignments           "which of them is a surface, which is ink"
   |
[colors]    per-widget overrides           "…except this one tab, which is different"
[style]     metrics
[syntax] [diagnostics] [graph] [preview] [font]
```

Nothing is required except `[pack]`. A pack with only `[roles]` is complete. A
pack with only `[palette]` and `inherit` is complete.

What fills the gaps differs by layer:

- **Roles** a pack leaves out come from its base theme. Every chain ends at a
  built-in, and a built-in supplies a fixed value for every role except
  `ink.inverted` - so a role nobody in the chain sets keeps the built-in's value
  whatever the pack does to the roles around it. See [`[roles]`](#roles).
- **Widget colours** (`[colors]`) and the `[diagnostics]`, `[graph]` and
  `[preview]` tables are worked out from the *final* roles through the
  derivation tables below, unless some pack in the chain names them. A role
  change therefore repaints every widget that uses that role.

### Why not one flat list of sixty-three colours

Because that is what a generated theme would then have to be, and sixty-three
independently chosen colours do not look like one theme. Constraining a
generator to a small palette and a derivation is the whole point: coherence
falls out of the format instead of being asked for.

## Value grammar

### Colours

A colour is one of:

| Form | Example | Notes |
|---|---|---|
| Hex | `"#1a1b1e"`, `"#1a1b1eff"`, `"#abc"` | Exactly the spellings `color_from_hex()` already accepts, so a colour copied out of the settings panel pastes in unchanged. Three-digit form expands each digit (`#abc` = `#aabbcc`). Alpha defaults to opaque. |
| Reference | `"$accent"`, `"$palette.ink"` | A role name, or `palette.<name>`. Bare `$name` searches roles first, then palette. |
| Transform | `"alpha($accent, 40%)"` | See below. Arguments may themselves be references or transforms, nested at most four deep. |

### Transforms

Four, and no more. Each is total — no argument can make one fail — so a pack
cannot produce an unpaintable colour.

| Transform | Meaning |
|---|---|
| `alpha($c, P%)` | Replace alpha with `P%` of opaque. `alpha($c, 0%)` is invisible, not an error. |
| `lighten($c, P%)` | Add `P/100` to the Oklab **L** of `$c`, clamped to `[0,1]`. |
| `darken($c, P%)` | Subtract `P/100` from Oklab **L**. |
| `mix($a, $b, P%)` | Interpolate in Oklab; `0%` is `$a`, `100%` is `$b`. Alpha interpolates linearly. |

`lighten` and `darken` work in Oklab rather than on sRGB bytes because scaling
sRGB channels desaturates as it brightens: a saturated accent lightened 20% in
sRGB comes back grey, which is exactly the operation a hover state needs to
*not* do. Oklab's L axis is perceptual, so `lighten($accent, 8%)` is the same
visible step whether the accent is dark blue or pale yellow — which is what makes
a derived hover ramp work across every theme rather than only the one it was
tuned on.

Conversion is the standard sRGB → linear → LMS → Oklab chain:

```
linear   c <= 0.04045 ? c/12.92 : ((c+0.055)/1.055)^2.4
l m s    matrix [ 0.4122214708 0.5363325363 0.0514459929
                  0.2119034982 0.6806995451 0.1073969566
                  0.0883024619 0.2817188376 0.6299787005 ]
L a b    matrix [ 0.2104542553  0.7936177850 -0.0040720468
                  1.9779984951 -2.4285922050  0.4505937099
                  0.0259040371  0.7827717662 -0.8086757660 ] applied to cbrt(l,m,s)
```

The inverse runs the same chain backwards and clamps to `[0,1]` in sRGB, so a
transform that leaves the gamut comes back as the nearest paintable colour
rather than as an error.

### Numbers and pairs

| Form | Example | Used by |
|---|---|---|
| Number | `4.0` | scalar `[style]` keys |
| Pair | `[10, 10]` | `ImVec2` `[style]` keys, always `[x, y]` |
| Percent in a string | `"40%"` | transform arguments only |
| Direction | `"none"`, `"left"`, `"right"`, `"up"`, `"down"` | `window_menu_button_position`, `color_button_position` |

## `[pack]`

```toml
[pack]
format = 1
name = "Midnight"
author = "someone"
version = "1.0.0"
inherit = "dark"
description = "Cool, low-contrast dark theme for long sessions."
appearance = "dark"
```

| Key | Required | Meaning |
|---|---|---|
| `format` | yes | Schema version. `1` is this document. A pack declaring an unknown `format` is refused outright rather than half-read — the same rule the shader pack header follows, and for the same reason. |
| `name` | yes | Display name in the theme picker. The *id* comes from the filename, not from here, so renaming the file renames the theme and nothing silently keeps pointing at the old one. |
| `inherit` | no | `"dark"` (default), `"light"`, `"classic"`, or the id of another pack. Resolved before this pack's own layers. Cycles are refused, and the chain is capped at eight. |
| `appearance` | no | `"dark"` or `"light"`. Hints which syntax palette and which stock icons to pair with the theme; inferred from the resolved `surface.base` lightness when absent. |
| `author`, `version`, `description`, `homepage` | no | Shown in the picker's detail pane. `homepage` is displayed, never fetched. |

## `[palette]`

Literal colours, named by the theme's own vocabulary. Names are free-form
snake_case; nothing in the application looks for a particular one.

```toml
[palette]
void    = "#0b0c0f"
slate   = "#16181d"
steel   = "#22262e"
mist    = "#c7ccd6"
ice     = "#7aa2f7"
mint    = "#7fdbca"
amber   = "#e0af68"
rose    = "#f7768e"
```

Palette entries may only be hex. They are the ground the rest of the cascade
stands on, and a palette that referenced roles which referenced the palette
would be a cycle waiting to happen.

## `[roles]`

The twenty semantic slots. This is the layer a generated theme should mostly
consist of.

```toml
[roles]
"surface.base"   = "$palette.void"
"surface.raised" = "$palette.slate"
"surface.sunken" = "lighten($palette.void, 3%)"
"surface.overlay" = "alpha($palette.void, 70%)"
"ink.primary"    = "$palette.mist"
"ink.muted"      = "mix($palette.mist, $palette.slate, 55%)"
"ink.inverted"   = "$palette.void"
"line.subtle"    = "lighten($palette.slate, 6%)"
"line.strong"    = "lighten($palette.slate, 14%)"
accent           = "$palette.ice"
"accent.hover"   = "lighten($palette.ice, 8%)"
"accent.active"  = "darken($palette.ice, 6%)"
"accent.muted"   = "alpha($palette.ice, 25%)"
"accent.ink"     = "$palette.void"
"select.bg"      = "alpha($palette.ice, 35%)"
"select.ink"     = "$palette.mist"
"status.ok"      = "$palette.mint"
"status.warn"    = "$palette.amber"
"status.error"   = "$palette.rose"
"status.info"    = "$palette.ice"
```

A role this pack does not set is **not** worked out from the roles it does
set. It comes from the base theme: a parent pack's own expression for it, if
some pack in the chain has one, and otherwise the fixed value the built-in at
the end of the chain supplies (`builtin_theme_roles()` in `theme_pack.cpp`). A
pack that inherits `dark` and sets only `accent` therefore keeps dark's
`accent.hover`, `accent.active`, `accent.muted`, `accent.ink` and `select.bg`,
which were chosen for dark's accent rather than the new one. `ssstudio theme
<pack> --resolve` shows this: those roles are listed as `derived` (not set by
this pack) with dark's values.

So the last column below is a suggestion, not a fallback: an expression to write
when you set the role yourself, so it moves with the roles it belongs with. The
only role computed when absent is `ink.inverted`.

| Role | Paints | Suggested expression |
|---|---|---|
| `surface.base` | Window backgrounds, the docking ground | your ground colour, usually a palette entry |
| `surface.raised` | Panels, popups, tooltips, menu bar, table headers | `lighten($surface.base, 4%)` |
| `surface.sunken` | Input fields, the editor's text ground, scrollbar troughs | `darken($surface.base, 3%)` |
| `surface.overlay` | Modal dim, drag-and-drop dim | `alpha($surface.base, 60%)` |
| `ink.primary` | Body text | your text colour, usually a palette entry |
| `ink.muted` | Disabled text, line numbers, hints, plot axes | `mix($ink.primary, $surface.base, 55%)` |
| `ink.inverted` | Text drawn on an accent fill | usually left out: when absent it is computed as whichever of `surface.base` / `ink.primary` contrasts more with `accent`. Recomputed rather than inherited, because the right answer depends on the accent and an inherited one was chosen for a different accent |
| `line.subtle` | Borders, separators, light table borders, tree lines | `mix($surface.base, $ink.primary, 14%)` |
| `line.strong` | Strong table borders, focus outlines, resize grips | `mix($surface.base, $ink.primary, 28%)` |
| `accent` | Buttons, headers, sliders, check marks, the selected tab | your accent colour, usually a palette entry |
| `accent.hover` | Hovered variants of all of the above | `lighten($accent, 8%)` |
| `accent.active` | Held/pressed variants | `darken($accent, 6%)` |
| `accent.muted` | Docking preview, unsaved marker, nav highlight, alt table rows | `alpha($accent, 25%)` |
| `accent.ink` | Check marks, links, the tab overline | `$ink.primary` |
| `select.bg` | Text selection | `alpha($accent, 35%)` |
| `select.ink` | Text inside a selection (unused by ImGui; the editor honours it) | `$ink.primary` |
| `status.ok` | "Compiled", "up to date", success counts | `#6fbf73` |
| `status.warn` | Warnings, "modified", stale markers | `#e0af68` |
| `status.error` | Errors, the destructive button, failed builds | `#eb6a6a` |
| `status.info` | Notes, info diagnostics, plot lines | `$accent` |

The `ink.inverted` default is deliberately conditional rather than fixed: a
theme whose accent is pale yellow needs dark text on its buttons, and one whose
accent is deep blue needs light text. Picking the higher-contrast of the two
colours the theme already declares gets that right without asking the pack to
think about it.

## `[colors]`

Per-widget overrides, keyed by the ImGui colour name with the `ImGuiCol_` prefix
dropped. PascalCase, spelled exactly as ImGui spells it — this is a foreign
vocabulary, and renaming it to snake_case would break the one thing a pack author
does when stuck, which is search the ImGui documentation for the name.

```toml
[colors]
TabSelected = "$palette.steel"
DockingPreview = "alpha($palette.ice, 60%)"
```

Names retired by ImGui are accepted as aliases (`TabActive` → `TabSelected`), so
a pack written against an older build keeps working. Unknown names warn and are
ignored.

The full surface, and what each derives from when a pack does not name it:

| ImGui colour | Derivation |
|---|---|
| `Text` | `ink.primary` |
| `TextDisabled` | `ink.muted` |
| `WindowBg` | `surface.base` |
| `ChildBg` | `surface.raised` |
| `PopupBg` | `surface.raised` |
| `Border` | `line.subtle` |
| `BorderShadow` | `alpha(surface.base, 0%)` |
| `FrameBg` | `surface.sunken` |
| `FrameBgHovered` | `lighten(surface.sunken, 4%)` |
| `FrameBgActive` | `lighten(surface.sunken, 7%)` |
| `TitleBg` | `surface.base` |
| `TitleBgActive` | `surface.raised` |
| `TitleBgCollapsed` | `alpha(surface.base, 75%)` |
| `MenuBarBg` | `surface.raised` |
| `ScrollbarBg` | `alpha(surface.sunken, 60%)` |
| `ScrollbarGrab` | `line.strong` |
| `ScrollbarGrabHovered` | `lighten(line.strong, 8%)` |
| `ScrollbarGrabActive` | `accent` |
| `CheckMark` | `accent.ink` |
| `CheckboxSelectedBg` | `accent` |
| `SliderGrab` | `accent` |
| `SliderGrabActive` | `accent.active` |
| `Button` | `accent` |
| `ButtonHovered` | `accent.hover` |
| `ButtonActive` | `accent.active` |
| `Header` | `accent` |
| `HeaderHovered` | `accent.hover` |
| `HeaderActive` | `accent.active` |
| `Separator` | `line.subtle` |
| `SeparatorHovered` | `line.strong` |
| `SeparatorActive` | `accent` |
| `ResizeGrip` | `alpha(line.strong, 40%)` |
| `ResizeGripHovered` | `alpha(accent, 70%)` |
| `ResizeGripActive` | `accent` |
| `InputTextCursor` | `ink.primary` |
| `Tab` | `mix(surface.base, surface.raised, 50%)` |
| `TabHovered` | `accent.hover` |
| `TabSelected` | `accent` |
| `TabSelectedOverline` | `accent.ink` |
| `TabDimmed` | `darken(surface.base, 2%)` |
| `TabDimmedSelected` | `mix(surface.raised, accent, 30%)` |
| `TabDimmedSelectedOverline` | `accent.muted` |
| `DockingPreview` | `accent.muted` |
| `DockingEmptyBg` | `darken(surface.base, 2%)` |
| `PlotLines` | `status.info` |
| `PlotLinesHovered` | `status.warn` |
| `PlotHistogram` | `accent` |
| `PlotHistogramHovered` | `accent.hover` |
| `TableHeaderBg` | `surface.raised` |
| `TableBorderStrong` | `line.strong` |
| `TableBorderLight` | `line.subtle` |
| `TableRowBg` | `alpha(surface.base, 0%)` |
| `TableRowBgAlt` | `alpha(ink.primary, 4%)` |
| `TextLink` | `accent.ink` |
| `TextSelectedBg` | `select.bg` |
| `TreeLines` | `line.subtle` |
| `DragDropTarget` | `status.warn` |
| `DragDropTargetBg` | `alpha(status.warn, 15%)` |
| `UnsavedMarker` | `accent.muted` |
| `NavCursor` | `accent` |
| `NavWindowingHighlight` | `alpha(ink.primary, 70%)` |
| `NavWindowingDimBg` | `surface.overlay` |
| `ModalWindowDimBg` | `surface.overlay` |

## `[style]`

Metrics, snake_case, named after the `ImGuiStyle` field each one sets. Values
outside the listed range are clamped and warned about, because a
`window_rounding` of 400 is not a look anyone chose on purpose.

```toml
[style]
window_rounding = 4.0
frame_rounding = 3.0
window_padding = [10, 10]
frame_padding = [7, 4]
item_spacing = [8, 6]
window_menu_button_position = "none"
```

| Key | Type | Range | Default (built-in dark) |
|---|---|---|---|
| `alpha` | number | 0.2 – 1.0 | 1.0 |
| `disabled_alpha` | number | 0.1 – 1.0 | 0.6 |
| `window_padding` | pair | 0 – 32 | `[10, 10]` |
| `window_rounding` | number | 0 – 16 | 4.0 |
| `window_border_size` | number | 0 – 3 | 1.0 |
| `window_min_size` | pair | 32 – 512 | `[32, 32]` |
| `window_title_align` | pair | 0 – 1 | `[0, 0.5]` |
| `window_menu_button_position` | direction | — | `none` |
| `child_rounding` | number | 0 – 16 | 0.0 |
| `child_border_size` | number | 0 – 3 | 1.0 |
| `popup_rounding` | number | 0 – 16 | 0.0 |
| `popup_border_size` | number | 0 – 3 | 1.0 |
| `frame_padding` | pair | 0 – 24 | `[7, 4]` |
| `frame_rounding` | number | 0 – 16 | 3.0 |
| `frame_border_size` | number | 0 – 3 | 0.0 |
| `item_spacing` | pair | 0 – 24 | `[8, 6]` |
| `item_inner_spacing` | pair | 0 – 24 | `[4, 4]` |
| `cell_padding` | pair | 0 – 24 | `[4, 2]` |
| `indent_spacing` | number | 0 – 48 | 21.0 |
| `scrollbar_size` | number | 6 – 24 | 14.0 |
| `scrollbar_rounding` | number | 0 – 16 | 3.0 |
| `grab_min_size` | number | 4 – 32 | 12.0 |
| `grab_rounding` | number | 0 – 16 | 3.0 |
| `image_rounding` | number | 0 – 16 | 0.0 |
| `tab_rounding` | number | 0 – 16 | 3.0 |
| `tab_border_size` | number | 0 – 3 | 0.0 |
| `tab_bar_border_size` | number | 0 – 4 | 1.0 |
| `tab_bar_overline_size` | number | 0 – 4 | 2.0 |
| `menu_item_rounding` | number | 0 – 16 | 0.0 |
| `separator_size` | number | 1 – 4 | 1.0 |
| `separator_text_border_size` | number | 0 – 4 | 1.0 |
| `tree_lines_size` | number | 0 – 3 | 1.0 |
| `tree_lines_rounding` | number | 0 – 16 | 0.0 |
| `button_text_align` | pair | 0 – 1 | `[0.5, 0.5]` |
| `selectable_text_align` | pair | 0 – 1 | `[0, 0]` |
| `input_text_cursor_size` | number | 1 – 4 | 1.0 |
| `color_button_position` | direction | left/right | `right` |

Everything in `[style]` is applied *before* the UI scale, exactly where
`ScaleAllSizes()` sits in `apply_theme()` today. A pack therefore writes
unscaled numbers and the user's scale multiplies them, rather than the pack
having to know what scale it will be viewed at.

## `[syntax]`

The ten token kinds, keyed by the lower-case spelling `to_string(TokenKind)`
produces — the same keys the `[editor.syntax]` block in `settings.toml` uses, so
colours move between the two by copy and paste.

```toml
[syntax]
plain        = "$palette.mist"
comment      = "$roles.ink.muted"
preprocessor = "$palette.amber"
keyword      = "#c792ea"
type         = "$palette.ice"
intrinsic    = "$palette.mint"
number       = "#f78c6c"
string       = "#c3e88d"
operator     = "mix($palette.mist, $palette.slate, 35%)"
identifier   = "$palette.mist"
```

All ten are optional, and a pack may set some of them. A kind the pack does not
name comes from the *fallback palette*: `editor.syntax_theme` when it names one,
otherwise the palette this pack's `appearance` implies — `default` for dark,
`light` for light. One rule covers a partial `[syntax]` block and a missing one,
so a pack that names three kinds and a pack that names none behave the same way
about the rest. Never a derived value: a syntax colour guessed from a UI role is
a worse guess than a tuned palette that merely does not match the theme.

Alpha is ignored here: the editor draws text with `AddText`, and a
half-transparent keyword reads as a rendering bug rather than as a style.

## `[diagnostics]`

The severity ink, currently hardcoded in six places (`text_editor.cpp`,
`build_panel.cpp`, `editor_panel.cpp`, `graph_panel.cpp`, `scene_panel.cpp`,
`app.cpp`). Landing this section is what makes those six agree.

```toml
[diagnostics]
error = "$roles.status.error"
warning = "$roles.status.warn"
info = "$roles.status.info"
success = "$roles.status.ok"
```

| Key | Default | Also paints |
|---|---|---|
| `error` | `status.error` | Error lens, the squiggle, the destructive button's fill |
| `warning` | `status.warn` | Warning rows, "modified" markers |
| `info` | `status.info` | Notes, hints |
| `success` | `status.ok` | "Compiled", "up to date" |

## `[graph]`

The node editor draws with its own list, so its colours are its own.

```toml
[graph]
canvas = "darken($roles.surface.base, 2%)"
grid = "alpha($roles.ink.primary, 5%)"
node_bg = "alpha($palette.steel, 92%)"
node_border = "alpha(#000000, 63%)"
node_border_selected = "$palette.amber"
node_title = "$roles.ink.primary"
wire = "$roles.line.strong"
wire_active = "$roles.accent"
pin_unconnected = "$roles.line.strong"

[graph.pins]          # keyed by PinType
float = "#a0c8ff"
float2 = "#8ce6be"
float3 = "#e6c882"
float4 = "#f0a0aa"
int = "#bebebe"
bool = "#c8a0f0"
float4x4 = "#b4b4b4"
texture2d = "#78dcf0"
sampler = "#bebebe"
any = "#bebebe"

[graph.nodes]         # keyed by NodeCategory, the header band
input = "#345476"
math = "#3a4a60"
utility = "#3e5c4c"
color = "#684454"
control = "#605434"
custom = "#563e6c"
output = "#6e4a38"
```

Pin and node keys are the names `to_string()` gives the enum, which is the
spelling the rest of the application prints - so the matrix pin is `float4x4`,
not `matrix4`. A pin type or node category
added to `graph.h` later gets the neutral grey it gets today until packs name it,
which is why unknown keys here warn rather than fail: a pack written against a
newer build should still load on an older one.

## `[preview]`

Chrome only. The clear colour is the project's, and the checkerboard is drawn
*behind* the output so it shows through wherever the shader wrote alpha - which
is why `background = "color"` needs nothing here: the renderer has already
cleared to the project's colour.

```toml
[preview]
checker_a = "#2a2a30"
checker_b = "#22222a"
checker_size = 8.0        # pixels, 4 - 32
border = "$roles.line.subtle"
stats_ink = "$roles.ink.muted"
```

## `[font]`

A pack may carry a font and suggest it. The user's own `editor.font_path` always
wins, because a font choice is often an accessibility choice.

```toml
[font]
ui = "fonts/Inter-Regular.ttf"
editor = "fonts/JetBrainsMono-Regular.ttf"
ui_size = 15.0            # 11 - 22, a suggestion; ui_scale still multiplies it
editor_size = 15.0        # honoured only when the user has not set a size
```

Paths are relative to the pack directory and may not contain `..` or a drive
letter or leading separator. A single-file pack may not use `[font]` at all —
there is no directory for the path to be relative to, and letting it reach
outside would make the pack unportable. Both rules are checked before the file
is opened, not after.

## Resolution and precedence

Sources of a final colour, from weakest to strongest:

1. The derivation tables in this document: every widget colour and every
   `[diagnostics]`, `[graph]` and `[preview]` entry, evaluated against the
   final roles. Roles themselves have no derivation (see [`[roles]`](#roles)).
2. The base theme named by `inherit`, resolved recursively. A parent *pack*
   contributes its layers as written; they are merged with this pack's and
   resolved once, so a parent's `"accent.hover" = "lighten($accent, 8%)"`
   follows a child's `accent`. A *built-in* contributes fixed role values,
   which follow nothing.
3. This pack's `[palette]`, `[roles]`, then its explicit sections.
4. The user's own pinned keys in `settings.toml`, one key at a time.

Editing a single syntax colour pins that one colour; it is not a decision to
stop being themed. Only the keys actually touched survive a theme change, and
every key never touched keeps following whatever theme is active. Stated so it
generalises past syntax colours: **a key present in `settings.toml` overrides
the theme, and a key absent from it defers to the theme.** The same sentence
covers `[diagnostics]` and `[graph]` the day the settings panel can edit those.

Within one pack, an explicit `[colors]` entry always beats a derivation, and a
role always beats a palette entry of the same name. A role may reference another
role written below it - resolution retries what referred forwards - so the order
keys happen to land in a TOML table never decides whether a pack works. A cycle
is reported with the names in it, and the whole pack falls back to its base.

One rule covers what a reference means when a pack redefines the name it
mentions. While a role is being evaluated, every *other* role this pack
redefines is out of scope, which makes both readings unambiguous:

| Written | Means |
|---|---|
| `accent = "lighten($accent, 5%)"` | The **inherited** accent, lightened. This is how a pack tweaks its parent instead of restating it. |
| `accent = "$accent.hover"` together with `"accent.hover" = "$accent"` | A cycle. Neither can see the other's inherited value, so this is refused rather than resolving to whichever was read first. |

### Presence is the pin

Nothing extra needs recording, which is the point — but it costs one change in
`settings.cpp`. The `[editor.syntax]` writer emits all ten kinds on every save
today. Under this rule that would pin all ten the first time anyone touched
anything, freezing every user out of every theme's syntax colours: precisely
the failure this layering exists to avoid. The writer has to emit only the
kinds whose colour differs from the active theme's, and omit the rest.

That is the shape `ProjectEditorState` already uses, storing which tabs are
*closed* rather than which are open so a shader added later opens by default.
Storing the deviation instead of the state is what lets something new — a
shader, a theme — behave sensibly for the keys nobody has an opinion about.

### What happens to `syntax_theme`

It keeps a narrower job: the palette to fall back on when the active theme
supplies no `[syntax]` block at all - which is why Settings labels it "Fallback
palette" rather than "Palette". A theme that does supply one outranks it,
and pinned keys outrank both.

`"custom"` stops being a value it can hold, because customness is now recorded
per key by presence rather than by a flag over all ten. A settings file written
by an earlier build is migrated on first load:

| File says | Migration |
|---|---|
| `syntax_theme` names a built-in palette | Drop every key equal to that palette's value; keep the rest as pins |
| `syntax_theme = "custom"` | Compare the ten against each built-in palette, take the one matching most keys, and rewrite `syntax_theme` to its name; keep the differing keys as pins. Ties go to `default` |

Which turns "custom" into what it always meant in practice — a stock palette
with two or three colours changed — rather than into ten pins.

### What the panel owes the user

A pinned swatch has to look pinned, or the rule is invisible and a theme that
applies to eight of ten kinds reads as a bug. Three things follow: a marker on
every pinned swatch, a per-key revert beside it, and a "Reset to theme" on the
`Colors` tree node that clears the whole block.

The honest cost of choosing per-key: a pinned colour follows its user into
themes it was never chosen against. The panel should therefore flag a pinned
colour that fails the contrast thresholds below on the *active* theme — the
pack cannot be linted for a colour that is not in it.

## Validation

`ssstudio theme <pack>` resolves a pack, prints the layer each final value came
from, and reports problems — the same shape as `ssstudio inspect` for shader
packs, and the command a generator should run before shipping a theme.

Refused (the pack does not load):

- unknown or missing `format`
- unparseable TOML
- a reference cycle among roles
- an `inherit` cycle, or a chain deeper than eight
- an id colliding with `dark`, `light` or `classic`
- a `[font]` path that escapes the pack directory

Warned (the pack loads, that key does not apply):

- unknown key in any section
- a value of the wrong type
- a scalar outside its range, clamped
- a reference to a name that does not exist, left at its derived value
- `[font]` in a single-file pack

Linted (advice, printed by `--lint`):

| Check | Threshold |
|---|---|
| `ink.primary` on `surface.base` | contrast ratio ≥ 7.0 |
| `ink.muted` on `surface.base` | ≥ 4.5 |
| `ink.inverted` on `accent` | ≥ 4.5 |
| `[syntax]` colours on `surface.sunken` | ≥ 4.5, except `comment` at ≥ 3.0 |
| `[diagnostics]` colours on `surface.base` | ≥ 4.5 |
| `accent` vs `surface.raised` | ≥ 1.2, so a button is findable |

Contrast is the standard relative-luminance ratio, computed on the composited
colour: a translucent value is flattened over what sits behind it first, because
a ratio measured on unblended alpha describes a colour nobody sees.

Two things the thresholds deliberately do **not** check, both for the same
reason - luminance contrast is the wrong instrument for them:

- **One syntax colour against another.** Token kinds are told apart by hue at
  similar lightness, by design, so a pairwise luminance check flags almost every
  pair of every real palette - forty-two findings on this project's own default,
  which also spells `plain` and `identifier` as the same colour on purpose.
- **One severity against another.** Same arithmetic, same result, and the panels
  print the word "error" or "warning" beside the colour anyway.

`comment` gets the lower bar because every palette here dims comments on
purpose; holding them to the body-text ratio would flag all three shipped
palettes. The calibration rule behind all of these: a check that the built-in
themes fail is a miscalibrated check, not a discovery. All three pass clean, so
a finding means a theme is worse than what shipped before it.

## A minimal pack

Everything below is optional except `[pack]`, and this is a complete theme:

```toml
[pack]
format = 1
name = "Ash"
inherit = "dark"

[roles]
"surface.base" = "#141416"
"ink.primary" = "#d6d6d9"
accent = "#8a8f98"
```

Complete, but not yet coherent: the seventeen roles it leaves out keep dark's
fixed values, so its hover, pressed, check-mark and selection colours were
chosen for dark's accent. Deriving them from the roles they belong with fixes
that:

```toml
"accent.hover"  = "lighten($accent, 6%)"
"accent.active" = "darken($accent, 8%)"
"accent.muted"  = "alpha($accent, 30%)"
"accent.ink"    = "$ink.primary"
"select.bg"     = "alpha($accent, 35%)"
```

## Versioning

`format` is the only compatibility gate. Within `format = 1`, keys may be added
to any section — a reader that does not know a key warns and moves on, and a
pack that omits a key it has never heard of gets the derivation. That asymmetry
is what lets a pack written today load unchanged after the graph gains a pin
type, and a pack written for a later build load on this one, minus the parts
that build does not have.

A `format = 2` would mean a change that cannot be expressed that way — a new
cascade layer, or a transform whose meaning changed. It would be refused rather
than approximated, for the reason the shader pack header carries a signature:
misreading a file is worse than not reading it.

## Writing a theme

A generator producing a pack for this format should, in order:

1. Choose 6–10 palette colours and name them for what they *are*, not what they
   do. `void`, `ice`, `rose` survive being reassigned; `button_color` does not.
2. Fill in all twenty roles from that palette, using transforms rather than new
   literals wherever one colour is a variation of another. Set every role but
   `ink.inverted` (which is computed when left out), because any other role left
   out keeps the built-in base's fixed value rather than following the roles you
   did set. A theme whose hover states are `lighten($accent, 8%)`
   stays coherent when the accent changes; one with twelve hand-picked blues
   does not.
3. Set `[syntax]` from the same palette. Ten colours that share the palette are
   what makes the editor look like part of the application rather than a window
   into another one.
4. Touch `[colors]` only where a derivation is actually wrong for this theme.
   Every entry here is a place the theme will stop tracking its own roles.
5. Set `[style]` only if the theme has an opinion about geometry. Colours and
   metrics are independent, and most themes are colours.
6. Run `ssstudio theme <pack> --lint` and fix the contrast findings. The
   thresholds are not stylistic preferences; below them, text stops being
   readable for some of the people using it.

While iterating in the application, turn on **Settings → Editor → "Reload the
theme when the window regains focus"**. With the pack selected, an edit saved in
a text editor is applied as soon as you switch back: the active pack's file is
checked for a new modification time on focus, and nothing is checked at any
other moment. It watches one file - the active pack - so a pack being *added*,
or an edit to a pack that this one inherits from, still needs the picker's
"Reload themes". An edit that breaks the pack is reported in Diagnostics and
leaves the theme on screen alone.

## Where this lives

| Piece | Home |
|---|---|
| Oklab conversion, `lighten`/`darken`/`mix`, contrast | [`src/core/color.cpp`](../src/core/color.cpp) |
| Pack parse, cascade, derivation table, discovery, lint | [`src/core/theme_pack.cpp`](../src/core/theme_pack.cpp) |
| Per-key pinning and the `"custom"` migration | [`src/core/settings.cpp`](../src/core/settings.cpp) |
| Applying a resolved theme to `ImGuiStyle` | [`src/gui/theme.cpp`](../src/gui/theme.cpp) |
| Picker, pin markers, per-key revert, "Reset all to theme" | [`src/gui/panels/settings_panel.cpp`](../src/gui/panels/settings_panel.cpp) |
| `ssstudio theme` | [`src/cli/main.cpp`](../src/cli/main.cpp) |
| Focus reload: the mtime rule, then the hook | `ThemeWatch` in [`theme_pack.cpp`](../src/core/theme_pack.cpp), `App::window_focus_gained()` in [`app.cpp`](../src/gui/app.cpp) |
| Tests | [`tests/test_theme_pack.cpp`](../tests/test_theme_pack.cpp), [`tests/test_syntax.cpp`](../tests/test_syntax.cpp) |
| A worked example | [`themes/midnight.s3theme`](../themes/midnight.s3theme) |

The core/GUI boundary holds: parsing and resolving a pack produces a plain
struct of numbers, and only `src/gui` turns that into an `ImGuiStyle`. A theme
pack is therefore tested end to end on a machine with no display, which is the
rule the rest of the project already lives by.

Two details worth knowing before changing any of it:

- **The built-in themes do not go through the derivation table.** `dark`,
  `light` and `classic` are still ImGui's own styles plus the handful of
  overrides the app has always made, and `ResolvedTheme::builtin` is what tells
  `apply_theme()` to take that path. Packs exist without having changed what the
  three built-ins look like. Their entries in `builtin_theme_roles()` are the
  *inheritance basis* - what a pack gets when it inherits one - not what they
  paint.
- **`ImGuiCol_` names are matched against ImGui's own `GetStyleColorName()`**
  rather than a table of our own, so the mapping cannot disagree with the ImGui
  this build links, and a colour renamed upstream needs no change here.
