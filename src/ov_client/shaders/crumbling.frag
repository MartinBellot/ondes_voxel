#version 450

// The cracks over a block being broken. Drawn with the entity vertices, over
// the terrain, with the blend DST_COLOR, SRC_COLOR — which makes the result
// 2 * crack * what is already there: a crack texel of mid grey leaves the block
// alone, a dark one darkens it, a light one brightens it. That is vanilla's
// blend; the one difference is where it happens.
//
// Vanilla blends in gamma space, on the sRGB values as they are stored. This
// target is sRGB, so the hardware blends in *linear* space and encodes after.
// With sRGB taken as a 2.2 power, linear(2 s d) = 2^2.2 linear(s) linear(d),
// and the blend computes 2 * src * linear(d): so src = 2^1.2 * linear(s)
// lands within a few percent of vanilla. The texture is uploaded as sRGB, so
// what the sampler returns is already linear(s). Faithful's ten stages hold
// two greys, 61 and 155; neither reaches the clamp (155 gives 0.75).
//
// No fog and no light: the block underneath already carries both, and a
// multiply inherits them.

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_colour;
layout(location = 2) in float v_fog_distance;

layout(set = 0, binding = 0) uniform sampler2D u_texture;

layout(location = 0) out vec4 out_colour;

const float kGammaToLinearTwo = 2.2973967;  // 2^1.2

void main() {
    vec4 texel = texture(u_texture, v_uv) * v_colour;
    if (texel.a < 0.1) {
        discard;
    }
    out_colour = vec4(min(texel.rgb * kGammaToLinearTwo, vec3(1.0)), 1.0);
}
