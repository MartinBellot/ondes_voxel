#version 450

// Entities: mobs, players, dropped stacks, chest lids, sign text.
//
// Nothing is posed here. The vertex arrives in world space, already turned by
// its bone hierarchy and already shaded, because that whole computation lives
// in ov_render where a unit test can read it. What is left is the view
// projection and the fog — the same fog the terrain uses, from the same push
// block, so that a mob and the ground it stands on are never a different
// colour at the same distance.

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_colour;

layout(push_constant) uniform Push {
    mat4 view_projection;
    vec4 fog_colour;
    // xyz the camera, w where the fog starts.
    vec4 camera_and_fog_start;
    // x where the fog is complete.
    vec4 fog_end;
}
push;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_colour;
layout(location = 2) out float v_fog_distance;

void main() {
    v_uv = in_uv;
    v_colour = in_colour;

    // Cylindrical, as vanilla's has been since 1.18.1: the height difference is
    // left out, so looking up does not push the fog away. The terrain measures
    // it the same way and the two must agree.
    vec3 offset = in_position - push.camera_and_fog_start.xyz;
    v_fog_distance = length(offset.xz);

    gl_Position = push.view_projection * vec4(in_position, 1.0);
}
