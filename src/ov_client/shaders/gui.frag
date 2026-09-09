#version 450

layout(set = 0, binding = 0) uniform sampler2D u_texture;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_colour;

layout(location = 0) out vec4 out_colour;

void main() {
    // A solid fill is a quad on a one-texel white image, so there is no branch
    // here and no second pipeline: the texture is always sampled.
    out_colour = texture(u_texture, v_uv) * v_colour;
    if (out_colour.a <= 0.0) {
        discard;
    }
}
