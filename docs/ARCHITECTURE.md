# Architecture

## The one rule that shapes everything

**The core library does not link SDL.** `src/core` uses plain `<cstdint>`; SDL
types appear only in the desktop app and in the *generated* loader header. The
consequences are worth the constraint:

- the whole pipeline (compile → reflect → pack → generate) is testable on a
  machine with no GPU, no display and no SDL install;
- the CLI is a small binary usable in CI;
- a bug in the preview can never affect what gets packed.

## Modules

```
types / hash          enums, diagnostics, FNV-1a
reflection            backend-neutral description of a compiled shader
spirv_reflect         SPIR-V -> Reflection (self-contained, no SPIRV-Cross)
compiler              ICompilerBackend + shadercross backends + async service
process               run a tool and collect its output (the shadercross CLI)
pack_format / packer  the container: layout descriptor, writer
pack_reader           the reader, written independently of the writer
compress              LZ4/zstd shims, optional at build time
keys                  key assignment strategies
codegen               id header, loader, docs, snippets, metadata
project / settings    TOML persistence
build                 orchestration shared by the GUI and the CLI

graph / graph_codegen node graph model, validation, HLSL+GLSL generation
graph_io              graph TOML persistence
glslang_backend       GLSL front end + the language-routing composite backend
templates             starter sources, cross-stage varying validation
completion            autocomplete candidates from intrinsics and reflection
cache                 shared on-disk compile cache
diff                  line diff for snapshots and A/B compare
```

Dependencies point one way: `build` knows about everything below it, nothing
below it knows about `build`. `codegen` never calls the compiler; `packer` never
reads a file.

## Decisions worth remembering

**The reader is not the writer's inverse function.** `pack_reader.cpp` was
written from the format description rather than by reusing the writer's helpers,
so a mistake in either shows up as a failing round trip instead of cancelling
itself out. The loader in `templates/s3pack.h` is a third independent
implementation, and the test suite feeds it packs the writer produced.

**Reflection drives the UI, the docs and the pack.** The I/O panel has no
per-shader special cases: it iterates the reflected uniform members and
resources. `SHADERS.md` and the Build panel's snippets come from the same data,
so they cannot drift from each other.

**Compute storage counts are split at pack time.** SDL wants read-only and
read-write counts separately for compute pipelines. Rather than making the loader
parse reflection at runtime, the packer stores both in the entry. Graphics
shaders put everything in the read-only fields, which is what SDL expects there.

**Keys are pinned, not derived at load time.** The default `explicit` strategy
derives a key from the shader id on the first build, writes it into
`project.toml`, and never recomputes it. Renaming a shader after that changes its
id but not its key unless the pin is removed deliberately. Pinned keys always win
over any strategy, so switching strategies never renumbers shipped shaders.

**The container format is data, not code.** `PackLayout` describes the format;
the writer, reader, loader and docs all read from it. Adding a knob means adding
a field and handling it in those four places, not forking the format.

**Compilation is debounced and cancellable.** `CompilerService` holds one queued
job per shader id *per owner*; a new keystroke replaces the pending job rather
than queueing another. The owner is the project session that asked, so two open
projects that both contain a shader called `plasma` do not cancel each other's
compiles - and because the owner is not part of `cache_key()`, they still share
a cached result when the source matches. Results are delivered on the UI thread
via `poll()`, so no callback ever touches ImGui state from a worker.

**Every project is a session; the UI reads exactly one of them.** `App` owns a
vector of `ProjectSession`, one per open project, and an index saying which is
active. A session holds all of that project's state: its `Project`, its
documents, its graphs, its scene, its preview clock, its last build report, and
the panel selections that name something inside it. Every `App` accessor the
panels use - `project()`, `documents()`, `last_report()`, `active_scene()` -
forwards to the active session, so switching the project tab moves the whole
window at once and no panel can be showing one project while another is being
edited. The three things that cannot be duplicated - the compiler service, the
compile cache and the `PreviewRenderer` - are shared, and each is made
session-safe rather than session-owned: the compiler by the owner field above,
the renderer by being emptied and refilled from the active session's retained
SPIR-V on every switch. Sessions are held by `unique_ptr` because the build
thread and the compile callbacks both refer to one that must not move when
another project is opened.

**Panels ask whether the next widget fits, rather than assuming it does.**
ImGui's `SameLine()` continues a row with no idea how wide the row is allowed to
get, so a panel dragged narrow silently clips whatever hangs over its right
edge. `FlowLayout` in `panels/panel_common.h` inverts that: the caller says how
wide the widget it is about to place will be, and the row continues only while
that still fits, otherwise folding onto the next line. Widths that should give
way rather than wrap go through `fitted_width()`, and side panes through
`side_pane_width()` - which is why the graph panel's canvas can no longer be
handed a negative width by a fixed 190px library beside it. Every toolbar in the
GUI is built this way; a fixed pixel width not wrapped in one of those helpers is
a bug waiting for someone with a narrow window.

**Failure is visible, never silent.** A missing codec, a missing translation
format, a mismatched layout and an absent toolchain all produce diagnostics that
name the fix. The packer refuses to emit a pack it cannot read back.

## The preview is not the product

`src/gui/preview` renders into an offscreen texture using shaders compiled to
SPIR-V for the editor's benefit. Nothing it produces is packed, exported or
referenced by generated code. That boundary is the same one drawn around the
scene layer: the preview exists to answer "does this look right", and the build
output is identical whether or not you ever open it.

## Testing

`tests/` uses a small in-tree harness (no dependency, one binary):

- `test_pack_roundtrip` — writer/reader agreement across all layout permutations
- `test_loader` — the generated loader, compiled against `tests/stub/SDL3/SDL.h`,
  reading packs the writer produced and reporting the create-info it would use
- `test_keys` — stability, collisions, pinning, 16-bit narrowing
- `test_reflection` — binding-model validation and the SPIR-V parser
- `test_codegen` — header/doc/snippet output

The SDL stub is only ever compiled into the test binary. It exists so a format
change that breaks the loader fails CI rather than a user's game.

## Language, graph and scene decisions

**Language routing is a backend, not a branch.** `CompositeBackend` picks a front
end by `Language`. GLSL compiles through glslang to SPIR-V, and when a profile
also wants DXIL/DXBC/MSL the SPIR-V is handed to the shadercross translator - the
same one HLSL uses. A GLSL shader therefore ships to exactly the platforms an HLSL
one does, and everything downstream stays language-agnostic.

**The shader toolchain is a process boundary, not just a library call.** There are
two shadercross backends behind the same interface: one that calls the linked
library, one that runs the `shadercross` tool. They exist because a prebuilt
SDL_shadercross carries its own SDL3, and two SDL3s in one process do not share
the error buffer the compiler reports through - so the linked build of that copy
loses every message, file and line. The build picks between them (see the README);
`ISpirvTranslator` is what lets either of them serve the GLSL front end.

**Varyings are matched by location, never by name.** HLSL semantics and GLSL
names will never agree, and the hardware only cares about the location. That one
choice is what makes a mixed-language vertex/fragment pair work.

**Graphs generate source; they do not compile.** A graph produces text, which
enters the normal `CompileRequest` path. So the graph editor cannot introduce a
second way for a shader to reach the packer, and detaching a graph is just a
matter of keeping the last generated file.

**The cache re-derives reflection from the cached SPIR-V.** Storing reflection
and trusting it on load would let a cache hit and a cold build diverge silently.
Storing the SPIR-V and re-reflecting costs microseconds and removes the class of
bug entirely. Failed compiles are never cached, since a stale error is worse than
a recompile.

**Parallel compiles produce sequential output.** Work is claimed by index, results
are written into a slot, and merging happens in declaration order afterwards.
Diagnostics and pack contents therefore never depend on thread scheduling.

**The scene layer's boundary is mechanical, not a convention.** Scene code lives
under `src/gui` and is never compiled into `ssstudio_core`, so the packer and the
code generator cannot reference it even by accident. A scene names a shader by
string id and carries nothing else. The test target compiles `scene.cpp` and
`batcher.cpp` directly rather than linking a GUI library, which is what lets the
behaviour be tested without weakening the boundary.


## Importing, textures and multipass

These three arrived together and lean on each other, so it is worth reading them
as one thing.

**An import is a wrapping problem, not a translation one.** A fullscreen fragment
shader written to the common web convention - one `mainImage` entry point and the
`i`-prefixed uniforms - declares no version, no descriptor sets and no output
variable, so it does not compile as Vulkan GLSL on its own. `shader_import`
therefore keeps the body byte for byte and puts a generated prelude and epilogue
around it, which is what lets a compiler diagnostic still point at code the
author recognises. The uniforms are reached by the macro auto-mapping that was
already there: a member called `iTime` finds the `iTime` macro by name, so an
imported shader animates with nothing configured.

**A texture binding names a file or an address, and nothing fetches on its own.**
`TextureRegistry` decodes on a worker and uploads on the device thread, because a
large image takes long enough to decode to drop frames. `AssetCache` keeps
downloads as plain files with a sidecar, and only `fetch` touches the network -
resolution never does. That separation is the whole reason opening a project
somebody sent cannot make this machine talk to an address they chose.

**A multipass chain is decided before it is drawn.** `pass_graph` works out the
whole frame - how many targets, which one each pass writes, which reads cross a
frame boundary, which pairs alternate - as a pure function over plain data. The
renderer executes that list and decides nothing, which is what lets the hard half
be tested with no GPU, no display and no SDL.

### Decisions worth remembering from that work

**The ordinary case is not a special case.** A pipeline with no passes schedules
to exactly one pass into one target, so a single-shader preview goes down the
same road as a chain rather than round it. There is no simpler path to keep in
step with the general one.

**Naming a thing once removes error cases rather than checking them.** A
pipeline's final pass is named by its `fragment` and nowhere else, so "no output
pass" and "two output passes" cannot be written down. And because a chain is an
ordered list rather than a graph, a same-frame read is by definition of an
earlier pass - so a cycle cannot be expressed, and a pass sampling the target it
writes cannot arise. Those are structural, not validated.

**A buffer gets a second target only when something reads it backwards.** A chain
nobody reads backwards costs exactly one target per pass.

**sRGB is asked of the hardware, not done to the pixels.** Linearising eight-bit
values on the CPU destroys precision in exactly the darks that sRGB encoding
exists to protect, so a texture marked sRGB gets an sRGB format and the hardware
converts on read.

**`run_process` spawns an exact path and does not search `PATH`.** That is right
for a toolchain that is looked up once and configured, and wrong for a tool that
is simply expected to be on the machine, so `AssetCache` resolves curl itself.

**Two compilers describe a binding array completely differently.** DXC flattens
`Texture2D x[3]` into three resources already named `x[0]`, `x[1]`, `x[2]`;
glslang keeps one resource with an array size of three. Both end up as the same
binding keys, because a key is the resource name when there is no array size and
the name plus an index when there is. `examples/texture-arrays` exists to show
the two agreeing on screen.

## What the preview deliberately will not do

`sampler2DArray`, `samplerCube` and `sampler3D` are reported as `unsupported` in
their Inputs & Outputs row rather than handed to the driver, which refuses them
with a message nobody can act on. Reflection, the packer, the loader and the
generated documentation all handle them correctly, so a shader using one builds
and ships - only the preview will not bind it. Each needs a second upload path in
`TextureRegistry`, which is built around `SDL_GPU_TEXTURETYPE_2D`; cubemaps are
worth doing alongside cubemap passes rather than on their own.

**Debug info never decides whether a shader compiles.** DXC's SPIR-V legalizer
gives up on some constructs only when debug info is on - an array of textures
among them - so a legalization failure is retried once without it, and the
diagnostics say that is what happened. Recognising that particular failure is
what keeps the retry off the ordinary error path, which compile-on-type spends
most of its time in.

**MSL is asked for at version 2.0.** SDL_shadercross defaults to 1.2, which
cannot express an array of textures. Two is the lowest version that can, so it is
what is requested: raising the floor further would rule out machines for nothing.
