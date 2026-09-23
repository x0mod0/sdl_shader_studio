# Things worth knowing, tips and tricks

This page collects what experienced users wish they had been told on day one.
The first half covers **rules** you need to follow for your shaders to work in an
SDL game. The second half covers **habits** that make shaders look better and
break less often. Each one explains the reason as well as the rule, so you can
apply it to situations this page does not cover.

## Contents

- [Things worth knowing](#things-worth-knowing)
  - [Register spaces](#register-spaces)
  - [Uniform padding](#uniform-padding)
  - [Shader keys are numeric and pinned](#shader-keys-are-numeric-and-pinned)
  - [The core links no SDL](#the-core-links-no-sdl)
  - [The preview is not your game](#the-preview-is-not-your-game)
- [Tips and tricks](#tips-and-tricks)
  - [Moving between HLSL and GLSL](#moving-between-hlsl-and-glsl)
  - [Matrix times vector](#matrix-times-vector)
  - [Prefer `saturate` and `clamp` over `if`](#prefer-saturate-and-clamp-over-if)
  - [Use `smoothstep` for edges](#use-smoothstep-for-edges)
  - [Scale by resolution, not by pixels](#scale-by-resolution-not-by-pixels)
  - [Break up `sin(time)`](#break-up-sintime)
  - [Watch precision on mobile](#watch-precision-on-mobile)
  - [Read `SHADERS.md` after your first build](#read-shadersmd-after-your-first-build)
  - [Pin a version before a big edit](#pin-a-version-before-a-big-edit)

---

## Things worth knowing

### Register spaces

**This is the mistake almost everyone makes.** SDL GPU expects each kind of
resource in a specific register space (HLSL) or descriptor set (GLSL), and the
space depends on the shader's stage. Put a resource in the wrong space and the
driver rejects the shader with an error that does not tell you why.

SDL uses fixed spaces so that one compiled shader works on Vulkan, Direct3D 12
and Metal without you describing its binding layout by hand. The price is that
you have to follow its layout:

| Stage | Sampled / read-only | Read-write | Uniform buffers |
|---|---|---|---|
| Vertex | `space0` / `set = 0` | - | `space1` / `set = 1` |
| Fragment | `space2` / `set = 2` | - | `space3` / `set = 3` |
| Compute | `space0` / `set = 0` | `space1` / `set = 1` | `space2` / `set = 2` |

In HLSL, the letter in the register says what kind of resource it is:

| Letter | Resource |
|---|---|
| `t` | Texture or read-only buffer |
| `s` | Sampler (`s0` pairs with `t0`) |
| `u` | Read-write texture or buffer |
| `b` | Uniform (constant) buffer |

**A fragment shader in HLSL** (textures in `space2`, uniforms in `space3`):

```hlsl
Texture2D<float4> albedo       : register(t0, space2);
SamplerState      albedo_sampler : register(s0, space2);

cbuffer Frame : register(b0, space3) {
    float  time;
    float2 resolution;
    float  _pad;
    float4 tint;
};
```

**The same in GLSL** (sets instead of spaces; a `sampler2D` combines the texture
and its sampler):

```glsl
layout(set = 2, binding = 0) uniform sampler2D albedo;

layout(set = 3, binding = 0) uniform Frame {
    float time;
    vec2  resolution;
    float _pad;
    vec4  tint;
} frame;
```

**A compute shader**, the one stage with read-write resources:

```hlsl
Texture2D<float4>   source      : register(t0, space0);  // read-only
RWTexture2D<float4> destination : register(u0, space1);  // read-write
cbuffer Params                  : register(b0, space2) { float strength; };
```

Within a space, SDL expects sampled textures first, then storage textures, then
storage buffers. The [SDL GPU documentation](REFERENCES.md#sdl-and-the-toolchain)
for `SDL_CreateGPUShader` is the authority on this layout.

**How the app helps:** every starter template already uses the right spaces, the
validator flags a resource in the wrong one, and the `Texture2D` and `cbuffer`
autocomplete snippets insert the correct space for the stage you are editing.

### Uniform padding

**HLSL packs constant buffers in 16-byte rows**, and a value may not straddle
two rows. Your C code does not follow that rule. If the two layouts disagree,
your game writes values at the wrong offsets and the shader reads garbage.
Nothing crashes; the picture is just wrong.

Take the `Frame` buffer from the starter templates:

```hlsl
cbuffer Frame : register(b0, space3) {
    float  time;        // offset 0
    float2 resolution;  // offset 4   (still fits in row 0)
    float  _pad;        // offset 12  (fills row 0)
    float4 tint;        // offset 16  (row 1)
};                      // 32 bytes
```

Now suppose you delete `_pad` because it looks unused:

| Member | HLSL offset | Offset in a plain C struct |
|---|---|---|
| `time` | 0 | 0 |
| `resolution` | 4 | 4 |
| `tint` | **16** (a `float4` cannot start in the last 4 bytes of a row) | **12** |

Your game would write `tint` 4 bytes too early, so the shader would read part of
`resolution` as the tint's first component. `_pad` is there to make the layouts
agree.

**Two more traps:**

- **`float3` followed by `float3`.** The first is at offset 0 and fills 12 bytes.
  The second would cross the row boundary, so HLSL moves it to offset 16. A C
  struct puts it at 12.
- **HLSL and GLSL disagree about `vec3`.** In a GLSL uniform block (std140), a
  `vec3` is aligned to 16 bytes, so in `float a; vec3 b;` the `b` starts at 16.
  In HLSL, `float3 b` after `float a` starts at 4. The same declaration has two
  layouts depending on the language.

**The safe route: let the tool write the struct.** After a build, the generated
`SHADERS.md` contains a C struct for every uniform buffer. It is built from
reflection, so its offsets match what the compiler actually produced, and any
gaps are filled with explicit padding. It also includes a size check:

```c
typedef struct Frame {
    float time;           /* offset 0, 4 bytes */
    float resolution[2];  /* offset 4, 8 bytes */
    float _pad;           /* offset 12, 4 bytes */
    float tint[4];        /* offset 16, 16 bytes */
} Frame;

SDL_COMPILE_TIME_ASSERT(Frame_size, sizeof(Frame) == 32);
```

Copy it into your engine. If the shader's layout changes and you forget to update
the struct, the `SDL_COMPILE_TIME_ASSERT` makes the mismatch a compile error in
your engine instead of a wrong picture on screen.

### Shader keys are numeric and pinned

Your game finds a shader in the pack by a **numeric key**, not by its name. The
first build derives the key from the shader's id and writes it into
`project.toml`:

```toml
[[shaders]]
id = "sprite_frag"
path = "shaders/sprite.frag.hlsl"
stage = "fragment"
key = 3032078713
```

After that, the key **never changes**, so a new pack shipped to an older game
binary keeps working. **Renaming a shader changes its id, not its key.**

Two consequences:

- **Commit `project.toml` after you build a new shader**, so the pinned key is
  shared with the whole team and survives later renames.
- **Only delete a `key` line on purpose.** The next build derives the key again,
  from the shader's *current* id. If the shader was renamed since the key was
  pinned, that gives a different key, and game binaries built against the old
  one will no longer find the shader.

`ssstudio keys <project>` shows every key and whether it is pinned yet. See
[the command line tool](COMMAND_LINE.md#keys).

### The core links no SDL

Compilation, reflection, packing and code generation run with **no GPU, no
display and no SDL install**. SDL is used only by the desktop app and by the
generated loader.

This is why:

- the whole pipeline can be tested in CI on a machine with no display,
- `ssstudio` is a small binary you can run on a build server,
- **a bug in the preview cannot affect what gets packed**, because the preview
  and the packer do not share that code.

### The preview is not your game

Everything the preview or a scene provides - macros like `time` and
`resolution`, scene state, built-in meshes - exists to answer one question:
"does this look right?". **None of it ships.** At runtime, your game must supply
those values itself, usually by filling the uniform struct from
[`SHADERS.md`](#uniform-padding) and pushing it each frame:

```c
Frame frame = {0};
frame.time = elapsed_seconds;
frame.resolution[0] = (float)width;
frame.resolution[1] = (float)height;
frame.tint[0] = frame.tint[1] = frame.tint[2] = frame.tint[3] = 1.0f;
SDL_PushGPUFragmentUniformData(command_buffer, 0, &frame, sizeof(Frame));
```

If a shader looks right in the preview but not in your game, check the values
your game pushes first.

---

## Tips and tricks

### Moving between HLSL and GLSL

The two languages mostly do the same things under different names. When porting
a shader, the problems come from **names, not meaning**:

| HLSL | GLSL | What it does |
|---|---|---|
| `lerp(a, b, t)` | `mix(a, b, t)` | Linear blend |
| `frac(x)` | `fract(x)` | Fractional part |
| `saturate(x)` | `clamp(x, 0.0, 1.0)` | Clamp to 0..1 |
| `rsqrt(x)` | `inversesqrt(x)` | 1 / sqrt(x) |
| `ddx(x)` / `ddy(x)` | `dFdx(x)` / `dFdy(x)` | Screen-space derivatives |
| `mul(m, v)` | `m * v` | Matrix times vector |
| `float2`, `float3`, `float4` | `vec2`, `vec3`, `vec4` | Vector types |
| `float4x4` | `mat4` | Matrix type |

The app's autocomplete shows the other language's spelling in its tooltip, so you
do not need to memorise this table.

### Matrix times vector

HLSL multiplies as **`mul(matrix, vector)`**; GLSL as **`matrix * vector`**:

```hlsl
output.position = mul(mvp, float4(input.position, 1.0));   // HLSL
```

```glsl
gl_Position = transform.mvp * vec4(in_position, 1.0);       // GLSL
```

Getting the order wrong does not fail to compile. It produces a transform that
looks *nearly* right, which is harder to catch than a crash. In the node graph,
the Transform node writes the correct form for each language.

### Prefer `saturate` and `clamp` over `if`

GPUs run groups of pixels together. When pixels in a group take different sides
of an `if`, the group often has to run both sides (this is called **branch
divergence**). That usually costs more than the arithmetic you were trying to
avoid.

```hlsl
// Branches:
if (brightness > 1.0) brightness = 1.0;
if (brightness < 0.0) brightness = 0.0;

// Does the same with no branch, and is typically a single instruction:
brightness = saturate(brightness);
```

### Use `smoothstep` for edges

`step` gives a hard edge: a pixel is either in or out. Hard edges shimmer and
show jagged "staircases", and it gets worse at resolutions you did not test.
`smoothstep` blends across a small band instead:

```hlsl
float d = length(uv - 0.5);                    // distance from the centre

float hard = 1.0 - step(0.3, d);               // jagged circle
float soft = 1.0 - smoothstep(0.29, 0.31, d);  // smooth circle

// Better still: make the band exactly one pixel wide at any resolution.
float w = fwidth(d);
float crisp = 1.0 - smoothstep(0.3 - w, 0.3 + w, d);
```

### Scale by resolution, not by pixels

A shader that assumes a screen size breaks on every other size. Work in
normalised coordinates and correct for the aspect ratio:

```hlsl
float2 p = uv * 2.0 - 1.0;                // -1..1 across the screen
p.x *= resolution.x / resolution.y;       // circles stay round
```

The preview lets you set an exact target size, so you can check how your shader
looks at the sizes you ship.

### Break up `sin(time)`

If every element animates with `sin(time)`, everything pulses in lockstep and
looks mechanical. Give each element its own speed or phase, for example from a
hash of its position:

```hlsl
float hash(float2 p) {
    return frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453);
}

float2 cell  = floor(uv * 10.0);                  // which grid cell this pixel is in
float  phase = hash(cell) * 6.2831853;            // a different phase per cell
float  pulse = sin(time * 2.0 + phase) * 0.5 + 0.5;
```

### Watch precision on mobile

On some mobile GPUs, a `float` in a GLSL fragment shader is only `mediump`,
which gives roughly three decimal digits of precision. Large values lose it
quickly: at a coordinate of 1000.0, the smallest step is already visible.

- **Keep UV maths near the origin.** Subtract a large offset before you do
  detailed maths on a position, not after.
- **Keep `time` small.** A value that grows for hours loses precision too. Wrap
  it at a period your animation repeats on.

### Read `SHADERS.md` after your first build

The generated `SHADERS.md` lists every binding, the create-info counts SDL needs
(`num_samplers`, `num_uniform_buffers`, ...), and copy-pasteable snippets,
including the uniform structs described [above](#uniform-padding). It is
generated from reflection, so it always matches your shader.

### Pin a version before a big edit

Press **F8** to pin the current version of the shader, then edit freely. It costs
nothing, and "what did I just break?" becomes a two-second comparison instead of
a rebuild.

---

**Next:** [HLSL and GLSL references](REFERENCES.md) ·
[What it does](WHAT_IT_DOES.md)
