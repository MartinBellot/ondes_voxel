#version 450

// Rain and snow: the entity vertex format and the entity push block, blended
// instead of cut out. A sheet of rain is translucent everywhere — the texture's
// own alpha times the column's, which fades with distance and with the rain
// level — so it is drawn after everything opaque, without writing depth.

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
    vec4  texel = texture(u_texture, v_uv);
    float alpha = texel.a * v_colour.a;
    if (alpha < 0.004) {
        discard;
    }
    vec3 colour = texel.rgb * v_colour.rgb;

    float fog_start = push.camera_and_fog_start.w;
    float fog_end   = push.fog_end.x;
    if (v_fog_distance > fog_start) {
        float amount =
            v_fog_distance < fog_end ? smoothstep(fog_start, fog_end, v_fog_distance) : 1.0;
        colour = mix(colour, push.fog_colour.rgb, amount);
    }
    out_colour = vec4(colour, alpha);
}
