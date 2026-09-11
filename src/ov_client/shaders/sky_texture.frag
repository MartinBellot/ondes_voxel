#version 450

// Added to the sky, not painted over it: the moon's dark side shows the sky
// through it. The texture is sRGB, the scene target UNORM and the game's
// arithmetic is on stored values, so the sample is encoded back first
// (ov/client/scene_target.hpp).

layout(set = 0, binding = 0) uniform sampler2D u_texture;

layout(location = 0) in vec2 v_uv;

layout(push_constant) uniform Push {
    mat4 view_projection;
    // rgb multiplies the texture, a is the rain's dimming (1 in clear weather).
    vec4 colour;
}
push;

layout(location = 0) out vec4 out_colour;

vec3 linear_to_srgb(vec3 c) {
    vec3 low  = c * 12.92;
    vec3 high = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    return mix(low, high, step(vec3(0.0031308), c));
}

void main() {
    vec4 texel = texture(u_texture, v_uv);
    out_colour = vec4(linear_to_srgb(texel.rgb) * push.colour.rgb, texel.a * push.colour.a);
}
