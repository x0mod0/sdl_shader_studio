# Theming

SDL Shader Studio can be customised at three levels, from a single click to a
complete repaint. This guide starts with the simplest level, then walks you
through writing your own **theme pack** and explains the ideas that keep a theme
consistent when you change it.

## Contents

- [Three levels of customisation](#three-levels-of-customisation)
- [Level 1: built-in themes and palettes](#level-1-built-in-themes-and-palettes)
- [Level 2: your own syntax colours](#level-2-your-own-syntax-colours)
- [Level 3: theme packs](#level-3-theme-packs)
  - [The idea: roles, not colours](#the-idea-roles-not-colours)
  - [Your first theme pack](#your-first-theme-pack)
  - [Growing it: palette, roles, overrides](#growing-it-palette-roles-overrides)
  - [Writing colour values](#writing-colour-values)
  - [How the layers stack](#how-the-layers-stack)
  - [Checking a pack from the command line](#checking-a-pack-from-the-command-line)
  - [A fast edit loop](#a-fast-edit-loop)
  - [Where packs live](#where-packs-live)
  - [Shipping a font with a pack](#shipping-a-font-with-a-pack)
  - [What a theme may not change](#what-a-theme-may-not-change)
- [Further reading](#further-reading)

---

## Three levels of customisation

| Level | What you change | Where | Effort |
|---|---|---|---|
| 1 | Pick an interface theme and a syntax palette | Settings | One click |
| 2 | Individual syntax colours | Settings, or `[editor.syntax]` in the settings file | A few minutes |
| 3 | The whole interface: widgets, editor, node graph, preview frame | A theme pack file | As much as you like |

Levels 1 and 2 only cover the syntax colours and the choice of built-in theme.
Every other interface colour comes from the theme, so to change those you need a
theme pack (level 3).

---

## Level 1: built-in themes and palettes

The app has three built-in **interface themes**:

| Theme | Look |
|---|---|
| `dark` | The default |
| `light` | A pale interface |
| `classic` | Dear ImGui's classic colours |

and three built-in **syntax palettes** for the editor:

| Palette | Designed for |
|---|---|
| `default` | The dark theme's editor background |
| `light` | The light and classic themes, where the editor background is pale |
| `mono` | Structure only: comments fade back, everything else is one colour. For anyone who finds a full palette distracting |

Pick both in **Settings > Editor**. They are stored in the settings file as:

```toml
[editor]
color_theme = "dark"
syntax_theme = "default"   # used when the theme sets no syntax colours
```

The syntax palette only applies when the active theme does not set its own
syntax colours. A theme pack that includes a `[syntax]` table takes precedence.

---

## Level 2: your own syntax colours

The editor colours ten kinds of token:

| Token kind | What it covers |
|---|---|
| `plain` | Whitespace, and anything the highlighter has no opinion about |
| `comment` | Comments |
| `preprocessor` | The `#` and the directive name after it |
| `keyword` | Language keywords |
| `type` | Type names |
| `intrinsic` | Built-in functions provided by the driver |
| `number` | Numeric literals |
| `string` | String literals |
| `operator` | Operators and punctuation |
| `identifier` | Every name your shader introduces |

Change any of them in **Settings > Editor**. You can also edit the settings file
by hand (`ssstudio settings --path` tells you where it is):

```toml
[editor.syntax]  # colors you changed yourself; the theme supplies the rest
comment = "#6b7a8f"
number  = "#f78c6c"
```

**An edited colour is pinned.** If you change one colour, that colour stops
following the theme, and the other nine keep following it. Settings marks pinned
colours with `*`, puts a **revert** button beside each one, and offers **Reset
all to theme**. Nothing you changed is silently thrown away, and nothing you did
not change gets stuck.

This is why the settings file only lists the colours you edited, not all ten.
If it listed all ten, they would all count as pinned, and switching themes would
no longer change any of them.

---

## Level 3: theme packs

A **theme pack** is a TOML file with the extension `.s3theme`. It can repaint
everything the application draws for itself: widgets and panels, the editor's
syntax and diagnostic colours, the node graph, and the frame around the preview.

### The idea: roles, not colours

The interface has **sixty-three** widget colours. Setting each one by hand would
be tedious, and the result would drift out of step the moment you changed one.

Instead, a pack sets **twenty semantic roles**, and every widget colour is
derived from them:

| Role group | Roles | What they paint |
|---|---|---|
| Surfaces | `surface.base`, `surface.raised`, `surface.sunken`, `surface.overlay` | Window backgrounds, panels and popups, input fields and the editor background, modal dimming |
| Ink | `ink.primary`, `ink.muted`, `ink.inverted` | Body text, secondary text and line numbers, text on an accent fill |
| Lines | `line.subtle`, `line.strong` | Borders and separators, focus outlines |
| Accent | `accent`, `accent.hover`, `accent.active`, `accent.muted`, `accent.ink` | Buttons, headers, sliders, the selected tab, and their hover and pressed states |
| Selection | `select.bg`, `select.ink` | Text selection |
| Status | `status.ok`, `status.warn`, `status.error`, `status.info` | Build results, diagnostics |

Widgets follow their roles. Set `accent`, and every button, header, slider and
selected tab uses it.

**Roles you leave out come from the theme you inherit from.** If you set
`accent` but not `accent.hover`, hovered buttons keep the parent theme's hover
colour, which was chosen for a different accent. So when you change a role,
also set the roles that belong with it, usually by deriving them:

```toml
accent          = "#7aa2f7"
"accent.hover"  = "lighten($accent, 6%)"
"accent.active" = "darken($accent, 8%)"
"accent.muted"  = "alpha($accent, 30%)"
"accent.ink"    = "$ink.primary"
"select.bg"     = "alpha($accent, 35%)"
```

The one exception is `ink.inverted` (text drawn on an accent fill). If you do
not set it, it is recalculated from your accent as whichever of `surface.base`
and `ink.primary` contrasts more with it, so button text stays readable.

### Your first theme pack

**1. Find your themes folder.** It is next to your settings file. Open it from
**Settings > Editor > Open folder**, which also creates it if needed, or go
there yourself:

| Platform | Folder |
|---|---|
| macOS | `~/Library/Application Support/sdl-shader-studio/themes/` |
| Windows | `%APPDATA%/sdl-shader-studio/themes/` |
| Linux | `$XDG_CONFIG_HOME/sdl-shader-studio/themes/`, else `~/.config/sdl-shader-studio/themes/` |

**2. Create `ash.s3theme` there.** This is a complete, valid theme:

```toml
[pack]
format = 1
name = "Ash"
inherit = "dark"

[roles]
"surface.base" = "#141416"
"ink.primary"  = "#d6d6d9"
accent         = "#8a8f98"
```

Three colours are enough for a working theme. `inherit = "dark"` starts from the
built-in dark theme: the roles you did not set keep dark's values (apart from
`ink.inverted`, which is recalculated), and all sixty-three widget colours are
then worked out from the resulting twenty roles. Run [`ssstudio theme ash.s3theme --resolve`](#checking-a-pack-from-the-command-line)
to see which values you set and which were filled in for you.

**3. Load it.** In **Settings > Editor**, click **Reload themes**, then choose
*Ash* from the Theme list.

**The file name is the theme's id.** `ash.s3theme` is the theme `ash`, and that
id is what your settings file records. Ids are matched without regard to case.
The names `dark`, `light` and `classic` are reserved for the built-ins: a pack
cannot use them, but it can inherit from them.

> **Shortcut:** copy `themes/midnight.s3theme` from this repository into your
> themes folder, rename it, and edit it. It is a small, complete, real theme.

### Growing it: palette, roles, overrides

A fuller pack is built in layers. Each layer refers to the one above it:

```toml
[pack]
format = 1
name = "Midnight"
inherit = "dark"

# 1. The handful of colours the theme is made from.
[palette]
void  = "#0e1016"
slate = "#161922"
mist  = "#c8cede"
ice   = "#7aa2f7"

# 2. Which of them plays which part.
[roles]
"surface.base"   = "$palette.void"
"surface.raised" = "$palette.slate"
"ink.primary"    = "$palette.mist"
accent           = "$palette.ice"
"accent.hover"   = "lighten($palette.ice, 6%)"

# 3. Editor token colours.
[syntax]
comment = "$roles.ink.muted"
type    = "$palette.ice"

# 4. Exceptions: one specific widget that should differ from its role.
[colors]
TabSelected = "$palette.slate"
```

- **`[palette]`** names your raw colours. Nothing reads it directly; it exists
  so you can refer to `ice` rather than repeating `#7aa2f7`.
- **`[roles]`** assigns palette colours to the twenty roles. Most of a good theme
  lives here.
- **`[syntax]`**, **`[diagnostics]`**, **`[graph]`** and **`[preview]`** colour
  the editor, error ink, node graph and preview frame.
- **`[colors]`** overrides individual widgets, by their Dear ImGui name. Use it
  sparingly, for the one widget a role gets wrong.
- **`[style]`** sets metrics such as corner radii, padding and spacing.

`themes/showcase.s3theme` uses every key the format has, in one working file.
Copy sections from it rather than writing them from scratch.

### Writing colour values

A value is a hex colour, a reference, or a transform:

```toml
"#1a1b1e"                          # also #abc and #1a1b1eff (with alpha)
"$accent"                          # a role
"$roles.ink.muted"                 # a role, spelled out
"$palette.ice"                     # a palette entry
"alpha($accent, 40%)"              # replace the alpha
"lighten($accent, 8%)"             # lighter, same hue and saturation
"darken($accent, 6%)"              # darker, same hue and saturation
"mix($accent, $surface.base, 30%)" # blend two colours
```

Those four transforms are the whole vocabulary, and they can be nested.

**Why `lighten` looks right.** `lighten` and `darken` work in a *perceptual*
colour space, not on raw sRGB values. Brightening the sRGB channels directly
also washes out the colour, so a saturated accent "lightened" that way turns
grey, which is exactly what a hover state should not do. Because the step is
perceptual, `lighten($accent, 8%)` looks like the same amount of change whether
your accent is deep blue or pale yellow. That is what keeps a derived theme
consistent when you change the accent underneath it.

**Referring to a role you are redefining.** Inside your pack, a role that refers
to itself means the *inherited* value:

```toml
[roles]
accent = "lighten($accent, 5%)"    # the parent theme's accent, a bit lighter
```

While one role is being worked out, the other roles your pack redefines are out
of scope. So two roles that point at each other are reported as a cycle rather
than silently resolving to whichever was read first.

### How the layers stack

When the app resolves a theme, four layers are applied in order, and a later
layer overrides an earlier one:

```
1  derivation   every widget colour from its role:            built in
                Button = $accent, ButtonHovered = $accent.hover
2  inherit      the base theme named in [pack]
3  the pack     [palette] -> [roles] -> everything else
4  your pins    colours you edited yourself in Settings
```

What layer 2 hands down depends on what you inherit from:

- **A built-in theme** (`dark`, `light`, `classic`) gives a fixed value for
  every role except `ink.inverted`. Those values do not change when your pack
  changes other roles.
- **Another pack** gives its role expressions as written, and they are worked
  out together with yours. If the parent pack says
  `"accent.hover" = "lighten($accent, 8%)"` and your pack changes `accent`,
  your hover colour follows your accent.

Roles are merged first (the parent's, then yours), and widget colours are worked
out once from the final set of roles. That is why changing a role in your pack
repaints every widget that uses it, even widgets your pack never mentions.

Layer 4 is the [pinning from level 2](#level-2-your-own-syntax-colours): your own
edits always win over any theme, one colour at a time.

### Checking a pack from the command line

The [`ssstudio`](COMMAND_LINE.md#theme) tool can check a pack without starting
the app:

```sh
ssstudio theme ash.s3theme --resolve --lint
```

```
id          ash
name        Ash
format      1
inherit     dark
appearance  dark (inferred)
set by pack 3 role(s), 0 color(s), 0 syntax, 0 metric(s)
resolved    20 role(s), 63 widget color(s)

roles
  surface.base      #141416  set
  surface.raised    #1C1C1F  derived
  ...
  accent            #8A8F98  set
  accent.hover      #3F4558  derived
  ...

lint
  nothing to report
```

- **`--resolve`** lists every final value, and says whether you **set** it or
  it was **derived** (filled in from the parent theme or calculated). That is
  the question you usually have while writing a theme: "where did this colour
  come from?"
- **`--lint`** checks that text is readable: text against its background, syntax
  colours against the editor background, the accent against the panels behind it.

Here is what a lint finding looks like, for a pack whose text is too dark for
its background:

```
lint
  warning: ink.primary on surface.base contrast is 1.43:1, below the 7.00:1 this check wants
  warning: ink.muted on surface.base contrast is 4.13:1, below the 4.50:1 this check wants
  warning: ink.inverted on accent contrast is 1.40:1, below the 4.50:1 this check wants

3 finding(s) worth fixing
```

The thresholds are set so that all three built-in themes pass. A finding means
your theme is *less readable than what ships*, not merely unusual. Both packs in
`themes/` pass with nothing to report.

**Refused versus warned.** Some mistakes stop a pack loading, and the previous
theme stays in place:

- TOML that does not parse
- an unknown `format`
- an id that collides with a built-in theme
- a reference cycle
- a font path that leads outside the pack's directory

Anything else (an unknown key, a value of the wrong type, a number out of range)
is a warning about that one key, and the rest of the pack still loads. A cosmetic
mistake should never cost you your session.

### A fast edit loop

Turn on **Settings > Editor > Reload the theme when the window regains focus**.
With your pack selected, save it in your text editor and switch back to the app:
the change is applied.

The app checks only that one file's timestamp, and only when the window regains
focus. A pack you *add*, or a pack your theme inherits from, still needs **Reload
themes**. An edit that breaks the pack is reported in **Diagnostics**, and the
theme you are looking at stays as it was.

### Where packs live

The app searches three folders. A later folder wins when two packs have the same
id:

| Order | Folder | Use it for |
|---|---|---|
| 1 | Next to the application (`SS Studio.app/Contents/Resources/themes/` on macOS) | Packs that ship with the app |
| 2 | Next to your settings file | Your own packs |
| 3 | `<project>/themes/` | A theme your team shares along with the shaders |

### Shipping a font with a pack

To bundle a font, a pack must be a **directory** rather than a single file,
because the font path is relative to the pack's own folder:

```
themes/mine.s3theme/
  theme.toml          # the pack, same format as the single-file form
  fonts/Inter.ttf
  preview.png         # optional, 640x400, shown in the theme picker
```

A font path may not contain `..` or start with a separator. A pack that reads a
file from somebody's home directory would only work on one machine.

**Your own font setting always wins** over a pack's suggestion, because the
choice of font is often an accessibility choice.

### What a theme may not change

A theme owns the application's *chrome*: its own colours and spacing. It does
not own your *content* or the app's *behaviour*:

| A theme cannot set | Because |
|---|---|
| The preview's clear colour | It belongs to the project. Changing it would change the picture, not the frame around it |
| UI scale and editor font size | They belong to the person using the app |
| File, tool and build settings | They are not part of how the app looks |

Keys like these are ignored with a warning rather than applied.

---

## Further reading

- [`themes/README.md`](../../themes/README.md) - a short practical guide to the
  packs that ship with the app.
- [`themes/midnight.s3theme`](../../themes/midnight.s3theme) - a normal theme:
  small palette, twenty roles, two overrides.
- [`themes/showcase.s3theme`](../../themes/showcase.s3theme) - every key the
  format has, in one lint-clean file.
- [`docs/THEME_PACK_FORMAT.md`](../THEME_PACK_FORMAT.md) - the full
  specification, including every widget colour and its derivation.

---

**Next:** [What it does](WHAT_IT_DOES.md) ·
[The command line tool](COMMAND_LINE.md)
