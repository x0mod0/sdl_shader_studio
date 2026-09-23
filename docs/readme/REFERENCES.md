# HLSL and GLSL references

Outside material worth keeping open while you write shaders. The list is short on
purpose: each entry says what it is good for and when to reach for it, so you can
go to the right place instead of searching.

## Contents

- [A suggested learning path](#a-suggested-learning-path)
- [SDL and the toolchain](#sdl-and-the-toolchain)
- [Language references](#language-references)
- [Learning shader techniques](#learning-shader-techniques)
- [In this repository](#in-this-repository)

---

## A suggested learning path

If you are new to shaders, this order works well:

1. **Learn to think in fragments** with *The Book of Shaders*. It teaches the
   central idea: one small program runs for every pixel at the same time.
2. **Write a few in the app.** Start from a template, change it, and watch the
   preview. Read [Things worth knowing](THINGS_WORTH_KNOWING.md) before your
   first build.
3. **Look things up as you go** in the HLSL or GLSL reference, whichever language
   you chose.
4. **Read the SDL GPU documentation** when you start loading shaders into your
   own game. That is where the binding model comes from.
5. **Go deeper** with Inigo Quilez's articles once the basics feel comfortable.

If you already write shaders for another engine, start with step 4 and the
[register space rules](THINGS_WORTH_KNOWING.md#register-spaces). They are the
part of SDL that is most likely to differ from what you know.

---

## SDL and the toolchain

### SDL GPU documentation

<https://wiki.libsdl.org/SDL3/CategoryGPU>

The official reference for the API your game uses to run these shaders. Start
with **`SDL_CreateGPUShader`** and **`SDL_CreateGPUComputePipeline`**. The notes
on the binding model in those pages are the authority for the
[register space table](THINGS_WORTH_KNOWING.md#register-spaces). If this
project's docs and SDL's ever disagree, SDL is right.

**Reach for it when:** you are writing the engine side (creating pipelines,
binding textures, pushing uniforms), or a shader loads in the preview but not in
your game.

### SDL_shadercross

<https://github.com/libsdl-org/SDL_shadercross>

The translation layer this tool uses to turn HLSL into SPIR-V, DXIL, DXBC and
MSL. See [Running the app](RUNNING_THE_APP.md#how-sdl_shadercross-is-reached)
for how the app finds and runs it.

**Reach for it when:** a compile error comes from the translation step, or you
want to know which HLSL features survive translation to each backend.

---

## Language references

### HLSL reference (Microsoft)

<https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-reference>

The complete language reference. The two most useful parts are the **intrinsic
function list** (what `lerp`, `saturate` and `ddx` do exactly) and the **packing
rules for constant buffers**, which explain the
[uniform padding](THINGS_WORTH_KNOWING.md#uniform-padding) rules in full.

**Reach for it when:** you need an intrinsic's exact behaviour, or a uniform
buffer's layout does not match your C struct.

### Khronos GLSL wiki and specification

<https://www.khronos.org/opengl/wiki/OpenGL_Shading_Language>

The GLSL reference. This project uses **Vulkan-dialect GLSL** (`#version 450`),
so the most important part for you is the `layout(set = ..., binding = ...)`
syntax that places resources where SDL expects them.

**Reach for it when:** you write GLSL, need the std140 layout rules for uniform
blocks, or are porting a shader written for OpenGL.

---

## Learning shader techniques

### The Book of Shaders

<https://thebookofshaders.com/>

The best gentle introduction to thinking in fragment shaders: shapes, colour,
patterns, noise, and randomness, with live examples on every page. It is written
in GLSL, but the ideas carry over to HLSL directly (see the
[naming table](THINGS_WORTH_KNOWING.md#moving-between-hlsl-and-glsl)).

**Reach for it when:** you are starting out, or want to understand a technique
rather than copy it.

### Inigo Quilez's articles

<https://iquilezles.org/articles/>

Dense, precise articles on signed distance fields, noise, smooth minimum,
colour palettes and much more, by one of the best-known authors in the field.
Worth the time they take.

**Reach for it when:** you want a specific technique done well, such as a
distance function for a shape, a better noise, or a procedural palette.

---

## In this repository

| Document | What it covers |
|---|---|
| [`docs/ARCHITECTURE.md`](../ARCHITECTURE.md) | How the code is organised, and why the core links no SDL |
| [`docs/PACK_FORMAT.md`](../PACK_FORMAT.md) | The byte-level specification of the `.s3pack` container |
| [`docs/THEME_PACK_FORMAT.md`](../THEME_PACK_FORMAT.md) | The full theme pack specification |
| `examples/hello-shader` | The smallest complete project: one vertex and one fragment shader |
| `examples/multipass-bloom` | A three-pass pipeline: two buffer passes feeding the pass you see |
| `examples/texture-arrays` | A binding array of textures, in both HLSL and GLSL |

---

**Next:** [Things worth knowing](THINGS_WORTH_KNOWING.md) ·
[What it does](WHAT_IT_DOES.md)
