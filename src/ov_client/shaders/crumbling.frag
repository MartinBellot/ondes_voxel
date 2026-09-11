#version 450

// The cracks over a block being broken. Drawn with the entity vertices, over
// the terrain, with the blend DST_COLOR, SRC_COLOR — which makes the result
// 2 * crack * what is already there: a crack texel of mid grey leaves the block
// alone, a dark one darkens it, a light one brightens it. That is vanilla's
// blend, on the stored values.
//
// Since render-parity the world is composed in an RGBA8 UNORM scene target
// (scene_target.hpp), so the hardware blends the stored numbers exactly as the
// game does, and the only conversion left is the texture's: it is uploaded as
// sRGB and decodes on sampling, so the texel is encoded back to the stored
// value first — as entity.frag and terrain.frag do. (Before, on an sRGB target,
// this shader scaled by 2^1.2 to approximate a gamma-space blend.)
//
// No fog and no light: the block underneath already carries both, and a
// multiply inherits them.

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_colour;
layout(location = 2) in float v_fog_distance;

layout(set = 0, binding = 0) uniform sampler2D u_texture;

layout(location = 0) out vec4 out_colour;

vec3 linear_to_srgb(vec3 c) {
    vec3 low  = c * 12.92;
    vec3 high = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    return mix(low, high, step(vec3(0.0031308), c));
}

void main() {
    vec4 texel = texture(u_texture, v_uv) * v_colour;
    if (texel.a < 0.1) {
        discard;
    }
    out_colour = vec4(linear_to_srgb(texel.rgb), 1.0);
}
