#version 450
// The same three textures under one name, in GLSL.
//
// glslang does not flatten this: the compiled module declares one resource
// called `channels` with an array size of three, occupying bindings 0, 1 and 2.
// The panel still shows three rows, and the manifest still spells them
// channels[0..2] - the two languages arrive at the same place from either side.
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D channels[3];

layout(set = 3, binding = 0) uniform Frame {
    vec2 resolution;
};

void main() {
    vec2 uv = gl_FragCoord.xy / max(resolution, vec2(1.0));
    vec2 local = vec2(fract(uv.x * 3.0), uv.y);

    int band = clamp(int(uv.x * 3.0), 0, 2);
    if (band == 0) out_color = texture(channels[0], local);
    else if (band == 1) out_color = texture(channels[1], local);
    else out_color = texture(channels[2], local);
}
