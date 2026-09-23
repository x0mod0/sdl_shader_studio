# The command line tool

The same build that produces the app also produces **`ssstudio`**, a command
line tool that does everything the app does except preview. It uses exactly the
same core as the app's **Build** panel, so a pack built in CI is byte-identical
to one built on your desk.

This guide walks you through a first project from scratch, then covers every
command, what a build produces, and how to load the result in your engine.

## Contents

- [Where to find it](#where-to-find-it)
- [Your first project, step by step](#your-first-project-step-by-step)
- [Command reference](#command-reference)
  - [new](#new)
  - [import](#import)
  - [check](#check)
  - [build and pack](#build-and-pack)
  - [inspect](#inspect)
  - [keys](#keys)
  - [reflect](#reflect)
  - [settings](#settings)
  - [theme](#theme)
- [What a build produces](#what-a-build-produces)
- [Loading the pack in your engine](#loading-the-pack-in-your-engine)
- [Using it in CI](#using-it-in-ci)

---

## Where to find it

After a build (see [Running the app](RUNNING_THE_APP.md)), the tool is at:

| Platform | Path |
|---|---|
| macOS, Linux, MinGW | `./build/bin/ssstudio` |
| Windows (Visual Studio) | `.\build\bin\Release\ssstudio.exe` |

The examples below write plain `ssstudio`. Either use the full path, or add
`build/bin` to your `PATH`.

If you only need the tool (on a build server, for example), configure with
`-DSSSTUDIO_BUILD_GUI=OFF`. That build needs no SDL window or ImGui.

Run `ssstudio --help` at any time for the full usage summary.

---

## Your first project, step by step

This walkthrough takes about a minute and touches every stage of the pipeline.

**1. Create a project.**

```sh
ssstudio new MyShaders
```

```
created MyShaders/project.toml
  shaders/sprite.vert.hlsl
  shaders/sprite.frag.hlsl
next: ssstudio build MyShaders --profile release
```

You now have a manifest (`project.toml`), a vertex and fragment shader written in
HLSL, and two build profiles: `debug` (SPIR-V only, no optimisation, no
compression) and `release` (SPIR-V, DXIL and MSL, optimised, LZ4-compressed).

**2. Check it compiles.**

```sh
ssstudio check MyShaders
```

```
ok: 2 shader(s), backend SDL_shadercross (bundled) + glslang
```

`check` runs the compiler front end and prints diagnostics, but writes nothing.
It is the fastest way to answer "is this valid?".

**3. Preview the keys.**

```sh
ssstudio keys MyShaders
```

```
SHADER_SPRITE_VERT = 3699809902  (0xdc869e6e)  [assigned]
SHADER_SPRITE_FRAG = 3032078713  (0xb4b9d979)  [assigned]
```

Each shader gets a **numeric key**, which is what your engine uses to find it at
runtime. `[assigned]` means the key has been worked out but not saved yet.

**4. Build.**

```sh
ssstudio build MyShaders --profile release
```

```
[compile] sprite_vert (1/2)
[compile] sprite_frag (1/2)
[pack] writing container (1/1)
[verify] re-reading the pack (1/1)
[emit] writing artifacts (1/1)

2 shader(s), spirv, dxil, msl, 8944 bytes (blobs at 71% of raw) in 0.04s
  pack: MyShaders/build/shaders.s3pack (8944 B)
  header: MyShaders/build/shaders.h (1353 B)
  loader: MyShaders/build/s3pack.h (27879 B)
  docs: MyShaders/build/SHADERS.md (8166 B)
```

Notice the `[verify]` step: after writing the pack, the build reads it back with
an independent reader to confirm it round-trips.

**5. Look at what you built.**

```sh
ssstudio inspect MyShaders/build/shaders.s3pack
```

```
layout      S3PK/v1.0/compact/by_key/table_first/key32/align16/names
shaders     2
formats     spirv, dxil, msl
compression lz4
size        8944 bytes

sprite_frag  key=3032078713 (0xb4b9d979)  fragment  entry='main'
    samplers=1 storage_tex=0 storage_buf=0 uniforms=1
    spirv: 996 B (from 1460) @240
    dxil: 2843 B (from 4292) @1248
    msl: 677 B @4096
...
```

**6. Run `keys` again.** The keys now say `[pinned]`: the build wrote them into
`project.toml`, and they will never change. That is what keeps a new pack
compatible with a game binary you have already shipped.

---

## Command reference

```
ssstudio new <dir> [--name NAME]           create a project skeleton
ssstudio import <project> <file|->         import a fullscreen shader, or a whole chain
ssstudio check <project> [--profile P]     front-end pass, diagnostics only
ssstudio build <project> [options]         compile, pack and emit artifacts
ssstudio pack <project> [options]          build without writing helper files
ssstudio reflect <shader.spv> [--stage S]  dump reflection for a SPIR-V blob
ssstudio inspect <pack file>               describe an existing pack
ssstudio keys <project> [--profile P]      show the key each shader will get
ssstudio settings [--path]                 print settings location / contents
ssstudio theme <pack> [--resolve --lint]   check a theme pack
```

Wherever a command takes `--profile`, leaving it out uses the **first** profile
in `project.toml`. For a scaffolded project, that is `debug`.

### new

```sh
ssstudio new MyShaders                     # project named after the directory
ssstudio new MyShaders --name "Water FX"   # a different display name
```

Creates the directory, a `project.toml`, and a vertex and fragment shader in
HLSL. The shaders are the starter templates, so they already follow SDL's
[register space rules](THINGS_WORTH_KNOWING.md#register-spaces).

### import

Brings in a fragment shader written in the common web convention: a `mainImage`
entry point and `i`-prefixed uniforms such as `iTime` and `iResolution`.

```sh
ssstudio import MyShaders toy.glsl                   # one shader
ssstudio import MyShaders chain.json                 # a whole multipass chain
cat toy.glsl | ssstudio import MyShaders - --id toy  # read from stdin
```

```
imported toy -> shaders/toy.frag.glsl
added fullscreen_vert -> shaders/fullscreen_vert.vert.glsl (the preview needs a vertex shader to pair with)
next: ssstudio check MyShaders
```

What happens:

- The body is kept **byte for byte**. A generated prelude declares what it
  expects and a generated epilogue calls it, so diagnostics point at lines you
  recognise.
- The first import also adds a fullscreen vertex shader (`fullscreen_vert`), so
  the preview has something to pair the fragment shader with. Later imports
  reuse it.
- The shader is registered as **GLSL**, whatever the project's default language.

**Single shader or chain?** You do not choose with a flag. A JSON file that
describes render passes is imported as a chain: each buffer pass becomes its own
shader, the inputs between them are connected, and the whole thing is added as a
new preview pipeline. Sound and cubemap passes are skipped with a warning,
because the preview has no machinery for them.

| Option | Meaning |
|---|---|
| `--id NAME` | Shader id to use. Default: the file name without any extensions (`seascape.frag.glsl` becomes `seascape`), or `imported` when reading stdin |
| `--common FILE` | Shared code to add ahead of the imported body |
| `--url URL` | Where the shader came from |
| `--author NAME` | Who wrote it |
| `--licence TEXT` | The terms it is offered under (`--license` works too) |
| `--keep-alpha` | Keep the shader's alpha. By default the result is made opaque |

`--url`, `--author` and `--licence` are recorded in the manifest and appear in
the generated `SHADERS.md`. **Fill them in** whenever you import someone else's
work, so a pack you ship keeps its attributions.

### check

```sh
ssstudio check MyShaders
ssstudio check MyShaders --profile release --quiet
```

Runs the compiler front end on every shader and prints diagnostics. Nothing is
written. With `--quiet`, a clean run skips the closing `ok:` line, so the output
is only warnings and errors, which is convenient in scripts.

### build and pack

```sh
ssstudio build MyShaders --profile release
ssstudio build MyShaders --profile release --out dist/shaders
ssstudio build MyShaders --dry-run
ssstudio pack  MyShaders --profile release
```

`build` compiles every shader, writes the pack, reads it back to verify it, and
emits whatever else the profile asks for. `pack` does the same but skips the
helper files: the header, loader, docs, embed header, reflection JSON, metadata
and CMake snippet. You get the pack (plus loose per-shader files, if the profile
emits them). Use it when your engine already has the helper files and you only
want new shader bytes.

| Option | Meaning |
|---|---|
| `--profile NAME` | Build profile to use (default: the first one) |
| `--out DIR` | Write output here instead of the profile's `output_dir`. A relative path is relative to where you run the command, not to the project |
| `--dry-run` | Compile and pack in memory, write nothing |
| `--no-verify` | Skip the round-trip check that re-reads the pack after writing |
| `--no-pin` | Do not write newly assigned keys back to `project.toml` |
| `--quiet` | Print only errors |

Builds share an on-disk compile cache across all your projects, so a shader you
have already compiled anywhere on your machine is not compiled again. Artifacts
whose bytes have not changed are not rewritten.

### inspect

```sh
ssstudio inspect MyShaders/build/shaders.s3pack
```

Describes an existing pack: its layout signature, formats, compression, size,
and for each shader its key, stage, entry point, resource counts, and every blob
with its size and offset. For compute shaders it also shows read-write resource
counts and the thread group size.

Use it to answer "what is actually in the file my game is loading?".

### keys

```sh
ssstudio keys MyShaders --profile release
```

Shows the enum name and numeric key each shader will get, and whether that key
is already `[pinned]` in the manifest or would be newly `[assigned]` by the next
build. Shaders excluded from the pack, or filtered out by the profile's stage
filter, are not listed.

### reflect

```sh
ssstudio reflect shader.spv --stage fragment
```

Prints, as JSON, what a compiled SPIR-V blob declares: its stage and entry
point, resource counts, uniform blocks (with each member's offset and size),
textures and buffers (with their set and binding), stage inputs and outputs,
and the thread group size of a compute shader. `--stage`
accepts `vertex`, `fragment` or `compute`; the default is `fragment`. This is the
same reflection that fills the app's Inputs & Outputs panel.

### settings

```sh
ssstudio settings --path   # just the location
ssstudio settings          # the whole file
```

Prints where the app keeps its settings, or the file's contents. If no settings
file exists yet, it writes one with the defaults first.

| Platform | Settings file |
|---|---|
| macOS | `~/Library/Application Support/sdl-shader-studio/settings.toml` |
| Windows | `%APPDATA%/sdl-shader-studio/settings.toml` |
| Linux | `$XDG_CONFIG_HOME/sdl-shader-studio/settings.toml`, else `~/.config/…` |

`check` and `build` read this file for one thing: the **shadercross dir**
setting. That way a terminal build uses the same compiler toolchain as the app.

### theme

```sh
ssstudio theme themes/midnight.s3theme --resolve --lint
```

Reads a theme pack, resolves it, and reports what it found. `--resolve` lists
every final colour and whether the pack **set** it or it was **derived**.
`--lint` checks readability (text against its background, for example). See
[Theming](THEMING.md#checking-a-pack-from-the-command-line) for how to read the
output.

---

## What a build produces

Each profile's `emit` list decides what is written. The file names use the
profile's `pack_basename` (`shaders` by default):

| `emit` value | File | What it is |
|---|---|---|
| `pack` | `shaders.s3pack` | The shader container your engine loads |
| `header` | `shaders.h` | An enum of shader keys, plus resource counts for each shader |
| `loader` | `s3pack.h` | A single-header C loader with an optional C++ wrapper |
| `docs` | `SHADERS.md` | Tables of every binding, create-info counts, and copy-pasteable snippets |
| `embed` | `shaders_embed.h` | The pack as a byte array, for compiling it into your binary |
| `reflection` | `shaders_reflection.json` | Reflection data for every shader, for your own tooling |
| `meta` | `shaders_meta.toml` | Build metadata |
| `cmake` | `ssstudio_shaders.cmake` | A CMake snippet for your engine's build |
| `shaders` | `<name>.<stage>.bin`, ... | Each shader as its own file, alongside the pack |

The generated `shaders.h` looks like this:

```c
typedef enum SHADER_Id {
    SHADER_SPRITE_FRAG = 0xB4B9D979u,  /* fragment, entry 'main' */
    SHADER_SPRITE_VERT = 0xDC869E6Eu,  /* vertex, entry 'main' */
    SHADER_COUNT = 2
} SHADER_Id;

#define SHADER_SPRITE_FRAG_NUM_SAMPLERS 1
#define SHADER_SPRITE_FRAG_NUM_UNIFORM_BUFFERS 1
/* ... */
```

The enum prefix (`SHADER_`) is the profile's `enum_prefix`.

**Read `SHADERS.md` after your first build.** It is generated from reflection, so
it always matches your shaders, and it contains ready-made C structs for your
uniform buffers, with padding already worked out.

---

## Loading the pack in your engine

Define `S3PACK_IMPLEMENTATION` in **exactly one** source file before including
the loader. Every other file includes it normally.

**C:**

```c
#define S3PACK_IMPLEMENTATION
#include "s3pack.h"
#include "shaders.h"

S3PACK_Pack pack;
if (!S3PACK_OpenFile(&pack, "shaders.s3pack")) {
    SDL_Log("%s", S3PACK_GetError());
}
SDL_GPUShader *vs = S3PACK_CreateShader(device, &pack, SHADER_SPRITE_VERT);
/* ... */
SDL_ReleaseGPUShader(device, vs);
S3PACK_Close(&pack);
```

**C++** (RAII wrappers that release themselves):

```cpp
auto pack = s3pack::Pack::open_file("shaders.s3pack");
if (!pack) { SDL_Log("%s", pack.error()); }

s3pack::Shader vs = pack->shader(device, SHADER_SPRITE_VERT);        // releases itself
s3pack::ComputePipeline blur = pack->compute(device, SHADER_BLUR_COMP);
SDL_DispatchGPUCompute(pass, blur.groups_for(w, 0), blur.groups_for(h, 1), 1);
```

`groups_for(total, axis)` divides your work size by the shader's thread group
size on that axis, rounding up, so you do not have to hard-code `[numthreads]` in
two places.

### Compressed packs need a decoder

The scaffolded `release` profile compresses with LZ4. The loader does not include
a decompressor, so for a compressed pack you supply one with a macro **before**
including `s3pack.h`:

```c
#include <lz4.h>
#define S3PACK_LZ4_DECODE(src, src_size, dst, dst_size) \
    LZ4_decompress_safe((const char *)(src), (char *)(dst), (int)(src_size), (int)(dst_size))

#define S3PACK_IMPLEMENTATION
#include "s3pack.h"
```

For zstd, define `S3PACK_ZSTD_DECODE(src, src_size, dst, dst_size)` the same way,
wrapping `ZSTD_decompress(dst, dst_size, src, src_size)`.

If you forget, nothing breaks silently: the loader fails with an error that says
the pack is compressed and names the macro to define. The other option is to
build with `compression = "none"`.

Other loader options you can define before the include:

| Macro | Effect |
|---|---|
| `S3PACK_NO_CPP` | Leave out the C++ wrapper, even in a C++ build |
| `S3PACK_USE_EXCEPTIONS` | The C++ wrapper throws `std::runtime_error` on failure |
| `S3PACK_ASSERT(x)` | Replace the assertion macro |
| `S3PACK_MALLOC` / `S3PACK_FREE` | Replace allocation (the default is `SDL_malloc`) |

### Why keys are numbers

Shader keys are **numeric and stable**. The first build derives a key from the
shader id and pins it in `project.toml`, so shipping a new pack to an older game
binary keeps working. Strings are never used at runtime, which means no string
comparisons and no ids to get wrong.

---

## Using it in CI

Every command returns a meaningful exit code, so it fits straight into a
pipeline:

| Exit code | Meaning |
|---|---|
| `0` | Success |
| `1` | The command ran and failed: a compile error, an unreadable file, a refused theme pack, or an unknown profile in `build` / `pack` |
| `2` | The command was used wrongly: an unknown command, a missing argument, or an unknown profile in `check` / `keys` |

> **Watch out:** a misspelled option (`--profle release`) currently prints the
> usage text and exits with `0`, so the step looks like it passed. If a CI step
> finishes suspiciously fast, check its log for `unknown option`.

A typical CI job:

```sh
cmake -B build -DSSSTUDIO_BUILD_GUI=OFF -DSSSTUDIO_BUILD_TESTS=OFF
cmake --build build -j

./build/bin/ssstudio check shaders --quiet
./build/bin/ssstudio build shaders --profile release --no-pin --quiet
```

`--no-pin` stops the CI build from writing keys back into `project.toml`. Pin new
keys by building locally and committing the manifest, so every key change is
reviewed.

`ssstudio theme` returns `0` even when `--lint` finds problems, because a
readability finding is advice, not a broken file. It prints a count of findings
at the end if you want to act on them.

---

**Next:** [What it does](WHAT_IT_DOES.md) ·
[Things worth knowing](THINGS_WORTH_KNOWING.md) ·
[Pack format specification](../PACK_FORMAT.md)
