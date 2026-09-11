#version 450

// The twilight band: a fan of coloured vertices around the eye, rotated with
// the camera and never moved by it. No fog: the game draws it without.

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_colour;

layout(push_constant) uniform Push {
    mat4 view_projection;
    vec4 colour;
}
push;

layout(location = 0) out vec4 v_colour;

void main() {
    v_colour    = in_colour;
    gl_Position = push.view_projection * vec4(in_position, 1.0);
}
