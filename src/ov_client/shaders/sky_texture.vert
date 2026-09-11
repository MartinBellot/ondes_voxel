#version 450

// The sun and the moon: textured quads 100 blocks out, turned by the time of
// day on the CPU (render::sun_quad, moon_quad) and by the camera here.

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec2 in_uv;

layout(push_constant) uniform Push {
    mat4 view_projection;
    vec4 colour;
}
push;

layout(location = 0) out vec2 v_uv;

void main() {
    v_uv        = in_uv;
    gl_Position = push.view_projection * vec4(in_position, 1.0);
}
