#version 450

// The interface: everything drawn in screen pixels rather than in the world.
//
// One pipeline for all of it — the hotbar, the hearts, the text, the item
// models in their cells. The positions arrive already in framebuffer pixels,
// so the GUI scale is applied on the CPU and the shader has one job: pixels to
// clip space. Vulkan's clip y points down and so does a screen pixel, so there
// is no flip here, and the absence of one is deliberate.

layout(location = 0) in vec2 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_colour;

layout(push_constant) uniform Push {
    // xy: 2 / framebuffer width and height.
    vec4 viewport;
}
push;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_colour;

void main() {
    v_uv     = in_uv;
    v_colour = in_colour;
    gl_Position = vec4(in_position * push.viewport.xy - 1.0, 0.0, 1.0);
}
