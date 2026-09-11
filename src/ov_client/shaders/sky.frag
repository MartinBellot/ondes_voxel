#version 450

// A flat colour under the game's linear fog, with its smoothstep — the same
// fog function the terrain uses, fed the sky pass's own start and end.

layout(location = 0) in float v_fog_distance;

layout(push_constant) uniform Push {
    mat4 view_projection;
    vec4 colour;
    vec4 fog_colour;
    vec4 fog;
}
push;

layout(location = 0) out vec4 out_colour;

void main() {
    vec3  colour = push.colour.rgb;
    float start  = push.fog.x;
    float end    = push.fog.y;
    if (v_fog_distance > start) {
        float amount = v_fog_distance < end ? smoothstep(start, end, v_fog_distance) : 1.0;
        colour       = mix(colour, push.fog_colour.rgb, amount);
    }
    out_colour = vec4(colour, 1.0);
}
