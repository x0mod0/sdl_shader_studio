# Texture arrays

A manual check that a binding array reaches every one of its slots, in both
languages, and a place to see what each language's compiler makes of one.

```sh
./scripts/run-macos.sh examples/texture-arrays
```

## What you should see

Three vertical bands. Left to right: **red with one bar, green with two, blue
with three**.

- A different order means a slot went somewhere it should not have.
- Three identical bands mean only the first element is being bound.
- All white means nothing is bound at all - the stand-in is a white pixel.

Switch between the **HLSL array** and **GLSL array** pipelines above the preview.
Both should look the same. They are the same picture written twice because the
two compilers describe an array so differently that it is worth seeing both agree
on screen.

## The thing worth knowing

`Texture2D channels[3]` and `uniform sampler2D channels[3]` are three separate
textures, not one texture with three layers. Reflection says so very differently
depending on which compiler produced the module:

| | What reflection reports |
|---|---|
| HLSL, through DXC | **Three** resources, already named `channels[0]`, `channels[1]`, `channels[2]`, each with no array size, at registers t0/t1/t2 |
| GLSL, through glslang | **One** resource named `channels` with an array size of 3, at binding 0 |

Both come out as three sampler slots, and both end up spelled the same way in
`project.toml` - `"channels[0]"` and so on - because a binding key is the
resource name when there is no array size and the name plus an index when there
is. The two paths meet in the middle, which is why the same bindings work for
either shader here.

One texture with three layers is `sampler2DArray`, which is a different thing and
is not supported by the preview: it reports `unsupported` in its row rather than
being handed to the driver. It still compiles, packs and ships correctly.

## A note on two toolchain limits

Both of these used to stop this project dead, and both are worked around now, but
they are worth knowing about because they are not this project's doing.

**DXC cannot emit debug info for a texture array.** Its SPIR-V legalizer gives
up on the `DebugGlobalVariable` describing one:

```
failed to legalize SPIR-V: Variable cannot be replaced: invalid instruction
  %100 = OpExtInst %void %2 DebugGlobalVariable ... %channels ...
```

Debug info is a convenience and must never decide whether a shader compiles, so
a legalization failure is now retried once without it. The compile succeeds and
says so in the diagnostics. The GLSL shader here was never affected.

**Metal needs a newer language version than the default.** Transpiling an array
of textures to MSL fails with "MSL 2.0 or greater is required for arrays of
textures", because SDL_shadercross defaults to 1.2. Version 2.0 is now asked for
whenever MSL is produced - the lowest that can express one, so the oldest
machines that could run this still can.
