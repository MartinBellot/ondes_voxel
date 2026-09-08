#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 1) in float v_brightness;
layout(location = 2) in flat uint v_tint;

layout(set = 0, binding = 0) uniform sampler2D u_atlas;

layout(location = 0) out vec4 out_colour;

// Placeholders for the biome colours the tint channel names. The real values
// come from the colormap textures in the pack, sampled by temperature and
// humidity; these are the plains entry of each, so that a grass block is green
// rather than grey while that is being built.
const vec3 kTint[4] = vec3[](vec3(1.0), vec3(0.569, 0.741, 0.349), vec3(0.475, 0.702, 0.318),
                             vec3(0.247, 0.463, 0.894));

void main() {
    vec4 texel = texture(u_atlas, v_uv);

    // The cutout layer's whole purpose. The threshold is vanilla's: anything
    // below it is a hole, not a faint pixel, and blending it instead leaves
    // grey fringes around every leaf.
    if (texel.a < 0.1) {
        discard;
    }

    out_colour = vec4(texel.rgb * kTint[v_tint] * v_brightness, texel.a);
}
