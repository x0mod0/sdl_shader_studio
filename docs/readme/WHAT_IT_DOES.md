# What SDL Shader Studio does

This page is a guided tour of the application. It follows the order you meet
things in: you write a shader, organise the files, watch the result, bring in
outside work, build more complex effects, and finally ship something your engine
loads. Each part explains what the feature does, why it works that way, and how
to try it.

If you have not built the app yet, start with [Running the app](RUNNING_THE_APP.md).

## Contents

- [The idea in one paragraph](#the-idea-in-one-paragraph)
- [1. Writing shaders](#1-writing-shaders)
  - [Autocomplete that reads your shader](#autocomplete-that-reads-your-shader)
  - [Completion after `.` and `:`](#completion-after--and-)
  - [Snippets and indentation](#snippets-and-indentation)
  - [HLSL and GLSL in one project](#hlsl-and-glsl-in-one-project)
- [2. Organising shaders](#2-organising-shaders)
  - [Creating a shader](#creating-a-shader)
  - [Names and ids](#names-and-ids)
  - [Tabs close, shaders stay](#tabs-close-shaders-stay)
  - [Renaming and deleting](#renaming-and-deleting)
- [3. Seeing the result](#3-seeing-the-result)
  - [Live preview](#live-preview)
  - [Preview pipelines](#preview-pipelines)
  - [Inputs and outputs from reflection](#inputs-and-outputs-from-reflection)
- [4. Bringing work in](#4-bringing-work-in)
  - [Importing a fullscreen shader](#importing-a-fullscreen-shader)
  - [Textures from a file or an address](#textures-from-a-file-or-an-address)
- [5. Going further](#5-going-further)
  - [Multipass pipelines](#multipass-pipelines)
  - [The node graph editor](#the-node-graph-editor)
  - [The scene harness](#the-scene-harness)
- [6. Shipping](#6-shipping)
  - [Build profiles and the pack](#build-profiles-and-the-pack)
  - [One file per shader](#one-file-per-shader)
  - [A container format you control](#a-container-format-you-control)
- [Keyboard reference](#keyboard-reference)

## The idea in one paragraph

SDL Shader Studio sits between your text editor and your game. It compiles what
you type, shows you the result immediately, and produces a runtime artifact your
engine loads with a few lines of C or C++. The core is deliberately narrow: it
does not want to be your engine, your asset pipeline, or your IDE. Everything
below serves one loop - **edit, see, ship** - and stays out of the way of the
rest of your toolchain.

---

## 1. Writing shaders

The editor is tabbed, with line numbers and inline diagnostics: a compile error
appears on the line that caused it, as you type, rather than in a log you have to
go and read.

### Autocomplete that reads your shader

Most editors complete from a fixed word list. This one completes from **your
shader**, and it ranks the suggestions by how hard each name would be for you to
look up:

1. **What the file declares** - locals, parameters, functions, structs and
   `#define`s. These are the names you invented, so no documentation has them.
2. **What the last compile reflected** - your uniform members and texture names.
3. **The language's built-ins** - intrinsics and types, which you could find in a
   reference if you had to.

How to drive it:

| Action | Key |
|---|---|
| Open the list | Type two letters, or press **Ctrl+Space** anywhere (also inside a finished word) |
| Move through it | **Up** / **Down** |
| Insert | **Tab** or **Enter** |
| Dismiss | **Esc** |

The list is **scoped at both ends**. A local from a function you are not in is not
offered, and nor is a helper you have not written yet. Built-ins are filtered by
the shader's stage too: there is no `ddx`, `fwidth` or `discard` in a vertex
shader, and no `barrier()` outside a compute one. Each of those would be a compile
error, and offering it would only get you to that error faster.

### Completion after `.` and `:`

The editor knows where the caret is, not only what is under it.

**After a `.`** you get that value's members and nothing else:

- For a vector, the components, sized to the vector. `myVec.` on a `float2` offers
  `x` and `y` and stops there, and it tells you the result type: `.xy` is a
  `float2`, `.x` is a `float`.
- For a struct, its fields, followed as deep as you care to go. `input.uv.` knows
  it is down to two components.
- For a name it never saw declared, nothing. It does not guess.

**After a `:`** you get HLSL semantics and nothing else, ranked by the stage the
shader is. `SV_Target` comes first in a fragment shader and `SV_DispatchThreadID`
in a compute one. `TEXCOORD0` and `NORMAL` are offered here even though the
highlighter does not colour them, because after a colon they can only be a
semantic. A GLSL file has no semantics, and the list never pretends otherwise.

> **Try it.** In a fragment shader, declare `float2 offset = uv;` and then type
> `offset.` on the next line. You should see exactly `x` and `y`. Then type `: `
> after a function parameter and `SV_Target` should be first in the list.

### Snippets and indentation

Some completions insert more than a word:

- **Control flow arrives as a whole statement.** `for` writes the complete loop,
  indented the way your editor is set, and leaves the caret inside the condition.
- **Resource declarations land in the right register space.** `Texture2D` and
  `cbuffer` write themselves into the space SDL expects for the current stage.
  (Why this matters is explained in
  [Things worth knowing](THINGS_WORTH_KNOWING.md#register-spaces).)

Indentation follows the code:

| You do | The editor does |
|---|---|
| **Enter** | Carries the current line's indentation down, and adds a level after a line that opens a scope |
| `{` then **Enter** | Puts the closing brace on its own line, with the caret between the two |
| **Tab** / **Shift+Tab** | Indents to the next stop or back out; with a selection, applies to every selected line |
| **Backspace** in leading spaces | Removes a whole level, not one space |

What "one level" means comes from two settings: **tab width** (how many columns)
and **insert spaces** (spaces or a tab character). **Auto indent** turns off the
Enter behaviour without taking Tab with it.

### HLSL and GLSL in one project

Both languages are first-class, and one project can mix them. A vertex shader in
HLSL can feed a fragment shader in GLSL. The app checks that the two stages agree
on their varyings, matching them **by location, not by name**, because that is
how the GPU connects them.

---

## 2. Organising shaders

### Creating a shader

The **`+`** beside the tab row opens a short menu: create a shader, or reopen one
you closed. You can also use **File > New shader** or **Ctrl+Shift+N**.

Creating a shader asks for four things: name, stage, language and file. The app
then writes the chosen template to disk, so the new shader **compiles and
previews the moment it appears** instead of starting as an empty buffer. Each
stage can start from its own template or from an empty file, and a vertex shader
can also start from the screen-covering triangle.

### Names and ids

A shader has a **name** and an **id**, and they are different strings.

| | Example | Who uses it |
|---|---|---|
| **Name** | `test` in `test.frag.hlsl` | You, and the editor's tabs |
| **Id** | `test_frag_hlsl` | The manifest and the generated header |

The id is the name plus the stage and language, joined by underscores exactly as
they appear in the file extension. That is what lets one name be reused:
`test.vert.hlsl`, `test.frag.hlsl` and `test.frag.glsl` can live side by side,
and the generated header still lists three distinct shaders.

You rarely need to think about the id. The **New shader** and **Rename** forms
show it before you commit, and nothing else in the editor does.

### Tabs close, shaders stay

Closing a tab is not the same as removing a shader. Close one with the cross on
its tab, or in bulk from the tab's right-click menu: **close others**, **close
to the left**, **close to the right**, **close all**.

None of these touch the project. A closed shader keeps its unsaved edits, its
diagnostics and its compiled blob, so:

- the preview keeps drawing it,
- a build still includes it,
- **Save all** still saves it,
- reopening it costs nothing.

Because nothing is lost, none of these actions asks for confirmation. Reopen a
shader from the tab context menu, the `+` menu, or **File > Open shader**. Each
of these lists only the shaders that are closed.

**Your tab layout is remembered per project**, including which tabs are closed
and the order you dragged them into. It is stored with the application settings,
next to the docking layout, and **never in `project.toml`**. Your tab
arrangement is your view of the project, not part of it, and it should not show
up as a change in someone else's checkout.

The app stores which tabs are *closed*, not which are open. So if a shader was
added since your last session (by a colleague, or by `ssstudio import`), it
arrives with an open tab after your remembered ones rather than hidden.

### Renaming and deleting

Right-click any shader tab to **save**, **rename**, **duplicate**, **copy its
path**, **point the preview at it**, or **delete** it.

- **Rename** updates the id everywhere it appears: bindings, provenance, the
  graph and its file, scene materials, and the preview selection. It does **not**
  change the shader's pinned key, so a pack you have already shipped keeps
  loading. (See [shader keys](THINGS_WORTH_KNOWING.md#shader-keys-are-numeric-and-pinned).)
- **Delete** sits at the bottom of the menu and asks for confirmation. It only
  removes the file from disk if you tick the box.

---

## 3. Seeing the result

### Live preview

Your shader renders into an offscreen target **as you type**. Compiles are
debounced, so a burst of keystrokes does not stall the UI.

| Want to... | Do this |
|---|---|
| Read the exact colour of a pixel | Hover over it |
| Freeze an animation | Pause, then scrub time |
| Compare before and after an edit | Pin the current version with **F8**, keep editing, and compare |

> **Try it.** Pin a version with F8 before a risky edit. If the result looks
> wrong, comparing against the pin tells you straight away whether your edit
> caused it.

### Preview pipelines

The preview draws a named **pipeline**: one vertex shader plus one fragment
shader. A project can keep as many pipelines as you like in its manifest, so a
pairing you want to come back to is one pick from a dropdown rather than two.

- A project with exactly one shader of each stage gets a first pipeline made
  for it, called **Default**. This happens once, and you can rename or delete it
  like any other.
- The last pipeline cannot be deleted, because a preview with no pipeline has
  nothing to draw.
- The dropdown only appears when there is more than one pipeline to choose from.

### Inputs and outputs from reflection

After each compile, the app **reflects** the shader: it reads back what the
compiled code actually declares. Every uniform member, texture and buffer then
appears in the **Inputs & Outputs** panel. Nothing is hard-coded per shader, so
the panel always matches your code.

You can bind each input in one of three ways:

1. **By hand** - type a value.
2. **To a macro** - such as `time` or `resolution`.
3. **To an expression** - for example `sin(time*2)*0.5+0.5`.

Members named `time` or `resolution` bind themselves to the matching macro.

The bindings are stored in `project.toml`. From `examples/hello-shader`:

```toml
[bindings.sprite_frag.uniforms]
"Frame.resolution" = { source = "macro", text = "resolution" }
"Frame.time"       = { source = "macro", text = "time" }
"Frame.tint"       = { source = "manual", value = [1, 1, 1, 1] }

[macros]
pulse = "sin(time*2)*0.5+0.5"
```

---

## 4. Bringing work in

### Importing a fullscreen shader

**File > Import** (or [`ssstudio import`](COMMAND_LINE.md#import)) takes a
fragment shader written in the common web convention, meaning a single
`mainImage` entry point and the `i`-prefixed uniforms, and adds it to the project
as an ordinary GLSL shader.

How it works:

- **Your code is kept byte for byte.** A generated prelude declares what the
  code expects, and a generated epilogue calls it. Diagnostics still point at
  lines you recognise.
- **It animates straight away.** Time, resolution, frame and mouse are already
  bound, so you do not have to configure any bindings.
- **Attribution travels with it.** Where the shader came from, who wrote it and
  its licence are recorded in the manifest and appear in the generated
  `SHADERS.md`. A pack you ship keeps its credits.

### Textures from a file or an address

A texture binding can name an image on disk or an `https://` address. PNG, JPEG,
WebP, BMP, GIF, TGA, QOI and SVG all load. Filter, wrap, vertical flip and sRGB
are set per binding and remembered in the manifest.

The download rules are strict on purpose:

- A downloaded image is **fetched once** and cached on disk.
- **Nothing is ever fetched on its own.** Opening a project someone sent you
  cannot make your machine contact an address they chose.
- When you are ready to share a project, **Localise** copies a downloaded image
  into it, so it no longer depends on your cache.

Editing an image in another program updates the preview without a reload. If the
file is caught half-written, the preview keeps showing the previous version
rather than flashing white.

---

## 5. Going further

### Multipass pipelines

A pipeline can run a **chain of passes** before the final one that draws what you
see. Each pass renders into a target that later passes can sample.

Things to know:

- **A buffer can sample itself one frame late.** This is how any effect that
  accumulates over time works: trails, simulations, progressive blur.
- **Buffers use a floating-point target by default**, because they often hold
  positions, counters and accumulators rather than colour.
- **Any target can be shown on screen**, so you can see what an intermediate pass
  is producing.
- **A pipeline with no passes is just one shader drawing to the screen**, exactly
  as before passes existed.

See `examples/multipass-bloom` for a worked bloom chain.

### The node graph editor

Any shader can be authored as a **graph** instead of text. There are about 45
nodes across inputs, math, utility, colour, control flow and custom code. The
graph generates readable HLSL or GLSL, with your node names kept as comments.

A few nodes worth knowing:

| Node | What it does |
|---|---|
| **Component** | Takes one channel out of a vector. The picker only offers channels the input actually has, so asking for `z` from a `float2` is refused in the graph rather than by the compiler on a line you did not write. |
| **Swizzle** | Like Component, but with several channels: type `xy`, `bgr` or `xxxx`, and the output is as wide as what you typed. |
| **Colour** | An input with a colour picker that produces one `float4`, not four channels to wire up. |
| **Constant** | The same idea for non-colour values: pick a width and type the numbers. |
| **Transform** | Emits `mul(m, v)` in HLSL and `m * v` in GLSL, so the multiplication order is correct for each language. |

The canvas pans, both side panes can be dragged to any width, and **Delete**
removes the selected node.

**Detach** at any time and the generated source becomes yours to edit by hand.
The graph is kept as a snapshot, so you can go back to it.

### The scene harness

Six built-in scenarios let you try a shader in a realistic 2D setting:

1. Sprite sheet
2. Tilemap
3. Parallax layers
4. Lit 2D
5. Post stack
6. Spinning mesh

They use hierarchical transforms, a sprite batcher, render layers and an ordered
post-processing stack.

This is a **test harness, not a runtime**. Scenes are never packed, never
exported and never referenced by generated code, so your build output is
identical whether or not the project contains scenes.

---

## 6. Shipping

### Build profiles and the pack

A **profile** describes what a build emits. It can include the pack, an id
header, the loader, documentation, an embed header, reflection JSON and
metadata. A project usually has at least two profiles, `debug` and `release`:

```toml
[[profiles]]
name = "release"
formats = ["spirv", "dxil", "msl"]
output_dir = "build"
pack_basename = "shaders"
compression = "lz4"
emit = ["pack", "header", "loader", "docs"]
```

Builds are fast for three reasons:

- **Shaders compile in parallel.**
- **The compile cache is shared across projects.** A shader you have already
  built anywhere on your machine is not compiled again.
- **Unchanged artifacts are not rewritten.** If the bytes are the same, the file
  is left alone, so its timestamp does not trigger rebuilds in your engine.

What the build produces, and how to load it from C or C++, is covered in
[The command line tool](COMMAND_LINE.md#what-a-build-produces).

### One file per shader

Sometimes you want loose files instead of (or as well as) a pack. Add
`"shaders"` to a profile's `emit` list, or tick it under the profile in the
**Build** panel, and each shader is also written out on its own next to the pack:

| Situation | `test.frag.hlsl` is written as |
|---|---|
| Default | `test.frag.bin` |
| `shader_extension = "spv"` (with or without the leading dot) | `test.frag.spv` |
| `shader_extension = ""` | `test.frag` |
| The profile builds several formats | `test.frag.spirv.bin`, `test.frag.dxil.bin`, ... |
| Two shaders share a name in different languages | Written under their ids instead, and the build tells you |

The format goes into the file name when there is more than one, because at that
point a single file per shader is no longer possible.

### A container format you control

Magic, extension, header layout, entry ordering, key width, alignment and
optional sections are all configurable. Your choices are recorded in the pack
header and built into the generated loader as a signature. A loader built for one
layout **refuses** a pack built with another rather than misreading it.

The byte-level details are in [`docs/PACK_FORMAT.md`](../PACK_FORMAT.md).

---

## Keyboard reference

| Key | Action |
|---|---|
| **Ctrl+Space** | Open autocomplete anywhere |
| **Up** / **Down**, **Tab** / **Enter**, **Esc** | Choose, insert, dismiss a completion |
| **Tab** / **Shift+Tab** | Indent / outdent (the whole selection, if any) |
| **Ctrl+Shift+N** | New shader |
| **F8** | Pin the current version for comparison |
| **Delete** (node graph) | Remove the selected node |

---

**Next:** [Running the app](RUNNING_THE_APP.md) ·
[The command line tool](COMMAND_LINE.md) ·
[Things worth knowing](THINGS_WORTH_KNOWING.md)
