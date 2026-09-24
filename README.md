# SDL Shader Studio

A cross-platform desktop tool for writing, testing and packing shaders for the
SDL 3.4 GPU API. Write HLSL, see it live, and get a pack plus a single-header
loader you can drop into an SDL project.

<table>
  <tr>
    <td width="50%" align="center" valign="top">
      <img src="assets/screenshots/preview_01.png" alt="Live preview of a multipass bloom pipeline, with the Inputs &amp; Outputs panel">
      <br><sub><b>Live preview</b> - a multipass bloom pipeline, with its reflected inputs</sub>
    </td>
    <td width="50%" align="center" valign="top">
      <img src="assets/screenshots/preview_02.png" alt="HLSL editor showing a blur pass, with the Build panel below">
      <br><sub><b>Editor</b> - an HLSL blur pass, its bindings, and the Build panel</sub>
    </td>
  </tr>
  <tr>
    <td width="50%" align="center" valign="top">
      <img src="assets/screenshots/preview_03.png" alt="Node graph editor generating a fragment shader">
      <br><sub><b>Node graph</b> - a fragment shader authored as a graph</sub>
    </td>
    <td width="50%" align="center" valign="top">
      <img src="assets/screenshots/preview_04.png" alt="Settings window in the light theme">
      <br><sub><b>Settings</b> - editor options in the light theme</sub>
    </td>
  </tr>
</table>

---

## Contents

- [About the app](#about-the-app)
- [What it does](#what-it-does)
- [Running the app](#running-the-app)
- [The command line tool](#the-command-line-tool)
- [The pack format is yours](#the-pack-format-is-yours)
- [Theming](#theming)
- [Things worth knowing, tips and tricks](#things-worth-knowing-tips-and-tricks)
- [HLSL and GLSL references](#hlsl-and-glsl-references)

## About the app

SDL Shader Studio sits between your text editor and your game. It compiles what
you type, shows you the result immediately, and produces a runtime artifact your
engine loads with a few lines of C or C++. The core is deliberately narrow: it
does not want to be your engine, your asset pipeline, or your IDE.

## What it does

A tabbed HLSL and GLSL editor with autocomplete that reads your own shader, a
live preview that redraws as you type, and an Inputs & Outputs panel filled in
from reflection, so it always matches your code. Beyond that: importing
fullscreen shaders, textures from files or URLs, multipass pipelines, a node
graph editor, a 2D scene harness, and parallel, cached builds that produce a pack
in a container format you configure.

**Guide:** [What it does](https://github.com/x0mod0/sdl_shader_studio/blob/develop/docs/readme/WHAT_IT_DOES.md) - a tour of every feature, in
the order you will use them.

## Running the app

On each platform, one script checks your environment, builds only when something
has changed, and launches the app: `./scripts/run-macos.sh`,
`./scripts/run-linux.sh` or `.\scripts\run-windows.ps1`. You need a C++20
compiler, CMake 3.21+, Git, and a GPU with Vulkan, Metal or D3D12. All other
dependencies are fetched automatically.

**Guide:** [Running the app](https://github.com/x0mod0/sdl_shader_studio/blob/develop/docs/readme/RUNNING_THE_APP.md) - the manual build for
each platform, configure options, how SDL_shadercross is found, and
troubleshooting.

## The command line tool

The same build produces `ssstudio`, which does everything the app does except
preview: create projects, import shaders, check, build, inspect packs and keys,
and check theme packs. It uses the same core as the app, so a pack built in CI
is byte-identical to one built on your desk.

**Guide:** [The command line tool](https://github.com/x0mod0/sdl_shader_studio/blob/develop/docs/readme/COMMAND_LINE.md) - a step-by-step first
project, every command and option, what a build produces, loading the pack from
C and C++, and using it in CI.

## The pack format is yours

Magic, file extension, header preset, entry ordering, blob/table order, key
width, alignment and the optional name/reflection/user sections are all
configurable in Settings → Formats or per build profile (see
`examples/hello-shader/project.toml`). Whatever you choose is recorded in the
header and baked into the generated loader as a signature, so a loader built for
one layout refuses a pack built with another instead of misreading it.

See [`docs/PACK_FORMAT.md`](docs/PACK_FORMAT.md) for the byte-level specification.

## Theming

The app opens in **Tide Dark**, one of two theme packs it ships with, set in
IBM Plex Sans and JetBrains Mono (both under the SIL Open Font License; the
licences are in `themes/tide-dark.s3theme/fonts/`). Choose it, Tide Light or one
of three built-in interface themes, pick one of three syntax palettes, or change
individual syntax colours. For anything more, write a **theme pack**: a small
TOML file that sets twenty semantic colour roles, from which all sixty-three
widget colours are derived. `ssstudio theme` checks a pack for problems,
including readability, before you use it.

**Guide:** [Theming](https://github.com/x0mod0/sdl_shader_studio/blob/develop/docs/readme/THEMING.md) - from picking a theme to writing,
checking and sharing your own pack.

## Things worth knowing, tips and tricks

A few rules decide whether a shader works in an SDL game: which register space
each resource goes in, how uniform buffers are padded, and why shader keys are
pinned numbers. A few habits make shaders look better and break less often:
getting HLSL and GLSL names right, avoiding branches, using smooth edges, and
scaling by resolution.

**Guide:** [Things worth knowing, tips and tricks](https://github.com/x0mod0/sdl_shader_studio/blob/develop/docs/readme/THINGS_WORTH_KNOWING.md) -
each rule and habit, with the reason behind it and a worked example.

## HLSL and GLSL references

A short, curated list of outside references: the SDL GPU documentation, the
SDL_shadercross translation layer, the HLSL and GLSL language references, and
two of the best resources for learning shader techniques.

**Guide:** [HLSL and GLSL references](https://github.com/x0mod0/sdl_shader_studio/blob/develop/docs/readme/REFERENCES.md) - each reference
with what it is for and when to use it, plus a suggested learning path.
