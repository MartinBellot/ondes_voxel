#version 450

// The world, drawn in the game's own number space, handed to an sRGB
// swapchain without changing a byte.
//
// Minecraft multiplies stored texture values by its shade, its lightmap and
// its fog, and blends water over them, all on the 0..255 numbers as stored:
// nothing in 1.20.1 converts to linear light. The scene target is UNORM so
// that every one of those operations here happens on the same numbers. The
// swapchain is sRGB and encodes whatever is written to it, so this writes the
// decoded value, and the encoder gives back exactly the byte the scene holds.

layout(set = 0, binding = 0) uniform sampler2D u_scene;

layout(location = 0) out vec4 out_colour;

vec3 srgb_to_linear(vec3 c) {
    vec3 low  = c / 12.92;
    vec3 high = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(low, high, step(vec3(0.04045), c));
}

void main() {
    vec3 stored = texelFetch(u_scene, ivec2(gl_FragCoord.xy), 0).rgb;
    out_colour  = vec4(srgb_to_linear(stored), 1.0);
}
