# Themes

This directory holds theme packs. A pack is a TOML file that repaints the
application: the interface, the editor's syntax and diagnostic colours, the node
graph, and the chrome around the preview.

Four packs ship here:

| File | What it is |
|---|---|
| `tide-dark.s3theme/` | The application's own look, and what a fresh install opens with. A directory pack, so it can name its fonts - see the README in its `fonts/` folder. |
| `tide-light.s3theme/` | Tide on a light ground. Inherits Tide Dark and redefines only its palette and roles, which is enough to carry every derived colour across. |
| `midnight.s3theme` | A normal theme. Small palette, twenty roles, two overrides. What a theme actually looks like. |
| `showcase.s3theme` | The reference. Every key the format has, in one working, lint-clean file - copy a section out of it rather than reading the specification. |

The byte-level specification is [`../docs/THEME_PACK_FORMAT.md`](../docs/THEME_PACK_FORMAT.md).
What follows is how to use it.

## Where packs go

Three directories are searched, in this order, and a later one wins if two packs
claim the same name:

1. **Beside the application** - this directory. Packs that ship with it.
2. **Beside your settings file** - your own packs. Settings → Theme → **Open
   folder** creates it and opens it for you.
   - macOS `~/Library/Application Support/sdl-shader-studio/themes/`
   - Windows `%APPDATA%/sdl-shader-studio/themes/`
   - Linux `$XDG_CONFIG_HOME/sdl-shader-studio/themes/`, else `~/.config/…`
3. **Inside a project** - `<project>/themes/`, for a theme a team shares along
   with the shaders.

The pack's **name comes from its filename**: `midnight.s3theme` is the theme
`midnight`, and that is what your settings file records. `dark`, `light` and
`classic` are the three built-in themes; a pack cannot claim those names, but it
can inherit from them.

## Making one

```bash
cp midnight.s3theme ~/Library/Application\ Support/sdl-shader-studio/themes/mine.s3theme
```

Then in the app: Settings → Theme → **Reload themes**, and pick the *Mine* card
from the list of themes.

A pack needs only a `[pack]` table to be valid. This is a complete theme:

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

The sixty-three widget colours all follow from twenty semantic roles, so the
three roles above repaint every widget that uses them. Set `accent` and every
button, header, slider and selected tab follows.

The other *roles* do not follow. The seventeen this pack leaves out keep the
fixed values `dark` gives them, so Ash's hovered and pressed buttons, its check
marks and its text selection are still dark's, chosen for dark's accent rather
than this one. (`ink.inverted` is the one exception: left out, it is recomputed
against your accent.) When you change a role, set the roles that belong with it
too, derived from it rather than picked separately - which is what
`midnight.s3theme` does:

```toml
accent          = "#8a8f98"
"accent.hover"  = "lighten($accent, 6%)"
"accent.active" = "darken($accent, 8%)"
"accent.muted"  = "alpha($accent, 30%)"
"accent.ink"    = "$ink.primary"
"select.bg"     = "alpha($accent, 35%)"
```

`--resolve` (below) lists which roles you set and which came from somewhere
else, which is the quickest way to spot one you forgot.

## The four layers

Later layers override earlier ones, and each may reference anything above it.

```
1  derivation    every widget colour from a role:               built in
                 a hovered button is $accent.hover
2  inherit       the base theme named in [pack]
3  the pack      [palette] -> [roles] -> everything else
4  your pins     colours you edited yourself, one at a time
```

What layer 2 hands down depends on what you inherit from:

- **A built-in** (`dark`, `light`, `classic`) hands down a fixed value for
  every role except `ink.inverted`. Those values do not react to anything your
  pack sets.
- **Another pack** hands down its layers as written, and they are resolved
  together with yours. If the parent says
  `"accent.hover" = "lighten($accent, 8%)"` and your pack changes `accent`,
  your hover follows your accent.

Layer 4 is worth knowing about: if you nudge one syntax colour in Settings, that
one colour is *pinned* and stops following themes - the other nine keep
following. Settings marks pinned colours with `*` and offers a **revert** beside
each, plus **Reset all to theme**. Nothing you changed is ever silently
discarded, and nothing you did not change is ever left behind.

## Writing values

A colour is hex, a reference, or a transform:

```toml
"#1a1b1e"                      # also #abc and #1a1b1eff
"$accent"                      # a role; $roles.name and $palette.name are explicit
"alpha($accent, 40%)"          # replace the alpha
"lighten($accent, 8%)"         # perceptual lightness, so hue and saturation survive
"darken($accent, 6%)"
"mix($accent, $surface.base, 30%)"
```

Those four transforms are the whole vocabulary, and they nest. `lighten` and
`darken` work in a perceptual colour space rather than on sRGB bytes, which
matters more than it sounds: scaling sRGB channels desaturates as it brightens,
so a saturated accent lightened the naive way comes back grey - exactly what a
hover state must not do. One `lighten($accent, 8%)` is therefore the same visible
step whether your accent is deep blue or pale yellow, which is why a derived
theme holds together when you change the accent underneath it.

Referring to a name your own pack redefines has one rule:

```toml
[roles]
accent = "lighten($accent, 5%)"    # the INHERITED accent, lightened
```

While a role is being worked out, every *other* role you redefine is out of
scope - so the line above means "my parent's accent, a bit lighter", and two
roles pointing at each other is reported as the cycle it is instead of resolving
to whichever was read first.

## Checking one

```bash
ssstudio theme mine.s3theme --resolve --lint
```

`--resolve` prints every final value and whether you **set** it or it was
**derived** - handed down by the base theme or worked out from a role - which is
the question you actually have while writing one. `--lint` checks that text can
be read: ink against its surface, syntax colours against the editor's ground,
the accent against the panels behind it.

The thresholds are calibrated so all three built-in themes pass clean, so a
finding means the theme is *less* readable than what shipped - not that it is
unusual. Both packs here report nothing.

Refused outright (the pack does not load, and the previous theme stays):
unparseable TOML, an unknown `format`, a name that collides with a built-in, a
cycle, or a font path that escapes the pack directory. Everything else - an
unknown key, a value of the wrong type, a number out of range - is a warning
against that one key, and the rest of the pack loads. A cosmetic mistake should
never cost you the session.

## Iterating

Turn on Settings → Theme → **Reload the theme when the window regains focus**.
With your pack selected, save the file in a text editor and switch back to the
app: the change is applied. It checks that one file's timestamp when the window
regains focus and at no other time, so a pack you *add* - or a pack your theme
inherits from - still needs **Reload themes**. An edit that breaks the pack is
reported in Diagnostics and leaves what you are looking at alone.

## Carrying a font

`[font]` needs the directory form of a pack, because its paths are relative to
the pack's own directory:

```
themes/mine.s3theme/
  theme.toml          # the pack, same schema as the single file
  fonts/Inter.ttf
  preview.png         # optional, 640x400, shown in the picker
```

A font path may not contain `..` or a leading separator - a pack that reads
somebody's home directory works on exactly one machine. Your own font setting
always wins over a pack's suggestion, because a font choice is often an
accessibility choice.

## What a theme may not change

A theme owns chrome. It does not own content, and it does not own behaviour: the
preview's clear colour belongs to the project, because it changes the picture
rather than the frame; the UI scale and editor font size belong to whoever is at
the keyboard. Keys like those are ignored with a warning rather than honoured.
