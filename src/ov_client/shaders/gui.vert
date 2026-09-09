#version 450

// The interface: everything drawn in screen pixels rather than in the world.
//
// One pipeline for all of it — the hotbar, the hearts, the text, the item
// models in their cells. The positions arrive already in framebuffer pixels,
// so the GUI scale is applied on the CPU and the shader has one job: pixels to
// clip space.
//
// ⚠️ The y **is** flipped here, and it has to be. ov_rhi sets a negative-height
// viewport so that the rest of the engine can work in +Y up (see
// CommandList::set_viewport), which means clip y = −1 is the *bottom* of the
// framebuffer rather than the top. A screen pixel counts downward from the top,
// so the two disagree and this line is where they are reconciled. Without it
// the hotbar is drawn at the top of the screen and every glyph is upside down —
// which is exactly what the first screenshot showed.

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
    gl_Position = vec4(in_position.x * push.viewport.x - 1.0,
                       1.0 - in_position.y * push.viewport.y, 0.0, 1.0);
}
