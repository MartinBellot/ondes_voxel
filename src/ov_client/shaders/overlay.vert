#version 450

// The lines the game draws that are not surfaces: the wireframe around the
// block being aimed at, and the crosshair.
//
// One shader for both, and the difference is a flag rather than a pipeline: a
// world-space line goes through the view projection, a screen-space one does
// not. Two pipelines for two multiplications would be two pipelines to keep in
// step.

layout(location = 0) in vec3 in_position;

layout(push_constant) uniform Push {
    mat4 view_projection;
    vec4 colour;
    // x: 0 for a line already in clip space, 1 for one in world space.
    vec4 flags;
}
push;

layout(location = 0) out vec4 v_colour;

void main() {
    v_colour = push.colour;
    gl_Position = push.flags.x > 0.5 ? push.view_projection * vec4(in_position, 1.0)
                                     : vec4(in_position, 1.0);
}
