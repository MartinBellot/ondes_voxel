#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 1) in float v_brightness;
layout(location = 2) in vec3 v_tint;
layout(location = 3) in vec2 v_light;
layout(location = 4) in float v_fog_distance;

layout(set = 0, binding = 0) uniform sampler2D u_atlas;
// Vanilla's 16x16 lightmap, rebuilt on the CPU every frame: block light across,
// sky light down. Everything about the time of day, the warmth of a torch and
// the brightness slider is in here rather than in this shader.
layout(set = 0, binding = 1) uniform sampler2D u_lightmap;

layout(push_constant) uniform Push {
    mat4 view_projection;
    vec4 fog_colour;
    vec4 camera_and_fog_start;
    vec4 fog_end;
}
push;

layout(location = 0) out vec4 out_colour;

void main() {
    vec4 texel = texture(u_atlas, v_uv);

    // The cutout layer's whole purpose. The threshold is vanilla's: anything
    // below it is a hole, not a faint pixel, and blending it instead leaves
    // grey fringes around every leaf.
    if (texel.a < 0.1) {
        discard;
    }

    vec3 light  = texture(u_lightmap, v_light).rgb;
    vec3 colour = texel.rgb * v_tint * light * v_brightness;

    // Vanilla's linear fog, with its own smoothstep: a hard lerp leaves a
    // visible ring on the ground at the point the fog starts.
    float fog_start = push.camera_and_fog_start.w;
    float fog_end   = push.fog_end.x;
    if (v_fog_distance > fog_start) {
        float amount = v_fog_distance < fog_end ? smoothstep(fog_start, fog_end, v_fog_distance)
                                                : 1.0;
        colour = mix(colour, push.fog_colour.rgb, amount);
    }

    out_colour = vec4(colour, texel.a);
}
