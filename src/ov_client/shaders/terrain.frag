#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 1) in float v_brightness;
layout(location = 2) in vec3 v_tint;

layout(set = 0, binding = 0) uniform sampler2D u_atlas;

layout(location = 0) out vec4 out_colour;

void main() {
    vec4 texel = texture(u_atlas, v_uv);

    // The cutout layer's whole purpose. The threshold is vanilla's: anything
    // below it is a hole, not a faint pixel, and blending it instead leaves
    // grey fringes around every leaf.
    if (texel.a < 0.1) {
        discard;
    }

    out_colour = vec4(texel.rgb * v_tint * v_brightness, texel.a);
}
