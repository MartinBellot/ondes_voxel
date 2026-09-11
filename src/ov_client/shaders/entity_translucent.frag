#version 450

// The game's `rendertype_entity_translucent`: the cutout shader's arithmetic,
// with the alpha kept and blended. A slime's outer jelly, a horse's markings.

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_colour;
layout(location = 2) in float v_fog_distance;
layout(location = 3) in vec4 v_overlay;
layout(location = 4) in vec3 v_light;

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
    vec4 texel = texture(u_texture, v_uv);
    texel.rgb  = linear_to_srgb(texel.rgb);
    if (texel.a < 0.1) {
        discard;
    }

    vec4 colour = texel * v_colour;
    colour.rgb  = mix(v_overlay.rgb, colour.rgb, v_overlay.a);
    colour.rgb *= v_light;

    float fog_start = push.camera_and_fog_start.w;
    float fog_end   = push.fog_end.x;
    if (v_fog_distance > fog_start) {
        float amount =
            v_fog_distance < fog_end ? smoothstep(fog_start, fog_end, v_fog_distance) : 1.0;
        colour.rgb = mix(colour.rgb, push.fog_colour.rgb, amount);
    }

    out_colour = colour;
}
