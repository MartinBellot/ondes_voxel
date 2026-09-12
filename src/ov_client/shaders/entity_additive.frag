#version 450

// The game's `rendertype_eyes` and `rendertype_energy_swirl`: light, not paint.
// The texture times the vertex colour, no lightmap and no directional shade —
// an enderman's eyes glow in the dark — faded out by the fog rather than
// coloured by it, and added to what is behind.

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

vec3 linear_to_srgb(vec3 c) {
    vec3 low  = c * 12.92;
    vec3 high = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    return mix(low, high, step(vec3(0.0031308), c));
}

void main() {
    vec4 colour = texture(u_texture, v_uv);
    colour.rgb  = linear_to_srgb(colour.rgb);
    colour *= v_colour;

    // `linear_fog_fade`: one before the fog starts, zero past its end.
    float fog_start = push.camera_and_fog_start.w;
    float fog_end   = push.fog_end.x;
    float fade      = 1.0;
    if (v_fog_distance >= fog_end) {
        fade = 0.0;
    } else if (v_fog_distance > fog_start) {
        fade = smoothstep(fog_end, fog_start, v_fog_distance);
    }

    out_colour = vec4(colour.rgb * fade, colour.a);
}
