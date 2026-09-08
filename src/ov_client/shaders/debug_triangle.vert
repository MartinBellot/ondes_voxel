#version 450

// The first thing that has to appear on screen.
//
// No vertex buffer, no descriptor set, no push constants: the positions are in
// the shader. That is deliberate — when this draws, it proves the instance, the
// device, the swapchain, dynamic rendering, the pipeline and the barriers are
// all correct, and nothing else can be blamed. Every later problem is then a
// problem with the thing that was added.

layout(location = 0) out vec3 v_colour;

vec2 kPositions[3] = vec2[](vec2(0.0, -0.6), vec2(0.6, 0.5), vec2(-0.6, 0.5));

vec3 kColours[3] = vec3[](vec3(1.0, 0.25, 0.25), vec3(0.25, 1.0, 0.25), vec3(0.25, 0.4, 1.0));

void main() {
    gl_Position = vec4(kPositions[gl_VertexIndex], 0.0, 1.0);
    v_colour    = kColours[gl_VertexIndex];
}
