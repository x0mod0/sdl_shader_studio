# multipass-bloom

A three-pass pipeline: two buffer passes render into offscreen targets, and a
third draws what you see from both of them.

```
trails_frag    -> rgba16f   emitters, drawn over a faded copy of its own last frame
blur_frag      -> rgba16f   reads trails_frag, spreads whatever is brighter than 1
composite_frag -> screen    reads both, tonemaps rgba16f down to rgba8
```

Open it in the app, or build it from the command line:

```
ssstudio build examples/multipass-bloom --profile debug
```

## What each pass is for

**`trails_frag`** is the accumulation buffer. Its `feedback` sampler is bound to
`trails_frag` itself with `source = "previous_frame"`, which is what makes the
emitters leave trails rather than dots. A pass cannot sample the target it draws
into, so a read of itself can only mean the frame before - the scheduler works
that out from the order, gives this pass two targets instead of one, and
alternates which is written by frame parity. Nothing in `project.toml` asks for
the second target or the alternation; both are consequences of the binding.

It writes values well above 1, which is why its target is `rgba16f`. Eight bits
per channel would clamp the emitter cores flat and leave the next pass nothing to
work with.

**`blur_frag`** is the reason the chain exists. A blur needs every neighbouring
pixel's finished value, and a fragment shader cannot see what the rest of its own
draw produced - so the thing being blurred has to be a target somebody else has
already finished writing. It reads `trails_frag` with `source = "pass_output"`:
an ordinary forward read of a pass that already ran this frame, so it needs one
target and no parity.

**`composite_frag`** is named as the pipeline's `fragment` rather than as one of
its `passes`, which is how a chain says which pass is final - exactly once, so it
can be neither missing nor doubled. It samples the sharp buffer and the blurred
one, adds them, and tonemaps. The presented target is always `rgba8`, so this is
the only pass that can bring the range down, and the only one that knows it is
last.

## Reading the schedule

`Buffers` in the preview panel puts any intermediate target on screen, which is
the quickest way to see that `blur_frag` really is a blurred `trails_frag` and
not something the composite is faking. The resolved schedule for one frame is
whatever `build_pass_schedule()` returns for the chain - four targets here: two
copies of `trails_frag`, one `blur_frag`, and the presented image.

