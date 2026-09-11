#version 450

// One triangle over the whole screen, from nothing but gl_VertexIndex.
//
// The fragment stage reads the scene by gl_FragCoord, pixel for pixel, so the
// only thing this stage has to get right is covering every pixel once. The
// viewport's negative height (ov_rhi) flips y, and the fetch by fragment
// coordinate does not care: both images are addressed the same way.

void main() {
    vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
