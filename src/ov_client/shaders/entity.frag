#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_colour;
layout(location = 2) in float v_fog_distance;

layout(set = 0, binding = 0) uniform sampler2D u_texture;

layout(push_constant) uniform Push {
    mat4 view_projection;
    vec4 fog_colour;
    vec4 camera_and_fog_start;
    vec4 fog_end;
}
push;

layout(location = 0) out vec4 out_colour;

void main() {
    vec4 texel = texture(u_texture, v_uv);

    // Every entity surface is a cutout, not a blend. A skin's second layer, the
    // text on a sign and a dropped item's sprite are all either drawn or not
    // drawn at a texel; the threshold is the terrain's, and blending instead
    // would need a depth sort a hundred moving mobs cannot be given cheaply.
    if (texel.a * v_colour.a < 0.1) {
        discard;
    }

    // The vertex colour already carries the face's flat shade and the lightmap
    // sample for the entity's block, both folded in on the CPU: a mob is lit as
    // a whole, and one lookup an entity is cheaper than one a fragment.
    vec3 colour = texel.rgb * v_colour.rgb;

    float fog_start = push.camera_and_fog_start.w;
    float fog_end = push.fog_end.x;
    if (v_fog_distance > fog_start) {
        float amount =
            v_fog_distance < fog_end ? smoothstep(fog_start, fog_end, v_fog_distance) : 1.0;
        colour = mix(colour, push.fog_colour.rgb, amount);
    }

    out_colour = vec4(colour, 1.0);
}
