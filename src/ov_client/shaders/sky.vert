#version 450

// The sky: geometry around the camera, rotated with it and never moved by it.
//
// Positions are relative to the eye, so the matrix is the projection times the
// camera's rotation alone. The fog distance is measured the way the game
// measures it for the sky pass — spherical or cylindrical, as the push
// constants say — and interpolated across each triangle, which is exactly why
// the sky disc is a fan of eight triangles and not a smooth dome: the game's
// is, and the fog gradient of its sky follows the fan's edges.

layout(location = 0) in vec3 in_position;

layout(push_constant) uniform Push {
    mat4 view_projection;
    vec4 colour;
    vec4 fog_colour;
    // x start, y end, z 1 for spherical distance and 0 for cylindrical.
    vec4 fog;
}
push;

layout(location = 0) out float v_fog_distance;

void main() {
    v_fog_distance = push.fog.z > 0.5 ? length(in_position)
                                      : max(length(in_position.xz), abs(in_position.y));
    gl_Position = push.view_projection * vec4(in_position, 1.0);
}
