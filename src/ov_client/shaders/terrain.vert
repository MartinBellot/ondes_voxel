#version 450

// Unpacks the twelve-byte terrain vertex.
//
// The layout is word-aligned so that no field straddles a 32-bit boundary and
// this is six bitfieldExtract calls with no reassembly. It has to agree with
// ov/render/terrain_vertex.hpp exactly; the comment there is the specification.

// Three scalar inputs, not one uvec3: a uvec3 attribute occupies a single
// location and is fed by R32G32B32_UINT, which is not a format every
// implementation accepts for vertex input. Three uints cost nothing and are
// reassembled here.
layout(location = 0) in uint in_word0;
layout(location = 1) in uint in_word1;
layout(location = 2) in uint in_word2;
layout(location = 3) in uint in_word3;

layout(push_constant) uniform Push {
    mat4 view_projection;
    vec4 fog_colour;
    // xyz the camera, w where the fog starts.
    vec4 camera_and_fog_start;
    // x where the fog is complete.
    vec4 fog_end;
}
push;

// The section origins, one vec4 a slot, looked up rather than pushed.
//
// A single indirect call draws every section of a layer, so there is no moment
// between two of them at which the CPU could push anything. What an indirect
// command does carry is firstInstance, and Vulkan defines gl_InstanceIndex as
// the instance number plus that — so with one instance per command it is
// exactly the slot the CPU wrote the origin into. vec4 and not vec3: std430
// aligns a vec3 array element to 16 bytes anyway, and the padding written
// explicitly is padding nobody has to remember.
// Binding 2, after the two sampled images: the pipeline layout puts every
// image before every buffer, so adding the lightmap at 1 moved this along.
layout(std430, set = 0, binding = 2) readonly buffer Sections {
    vec4 origins[];
}
sections;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out float v_brightness;
layout(location = 2) out vec3 v_tint;
/// Where to sample the lightmap: block light on x, sky light on y.
layout(location = 3) out vec2 v_light;
/// Distance for the fog, measured the way vanilla measures it.
layout(location = 4) out float v_fog_distance;

// Must match kPositionMin and kPositionScale.
const float kPositionMin   = -8.0;
const float kPositionScale = 2048.0;
const float kU16Max        = 65535.0;

const uint kFacingUnshaded = 6u;

// Vanilla's four ambient occlusion levels, evenly spaced up to full.
const float kAmbient[4] = float[](0.2, 0.4666667, 0.7333333, 1.0);

// Directional shading, per face. Measured from the game: the top of a block is
// full, the bottom half, north/south a fifth down, east/west two fifths.
// Indexed by the protocol's own face numbering, so down is 0.
const float kFaceShade[7] = float[](0.5, 1.0, 0.8, 0.8, 0.6, 0.6, 1.0);

void main() {
    uvec3 in_packed = uvec3(in_word0, in_word1, in_word2);

    float x = float(bitfieldExtract(in_packed.x, 0, 16)) / kPositionScale + kPositionMin;
    float y = float(bitfieldExtract(in_packed.x, 16, 16)) / kPositionScale + kPositionMin;
    float z = float(bitfieldExtract(in_packed.y, 0, 16)) / kPositionScale + kPositionMin;

    v_uv = vec2(float(bitfieldExtract(in_packed.y, 16, 16)) / kU16Max,
                float(bitfieldExtract(in_packed.z, 0, 16)) / kU16Max);

    uint sky    = bitfieldExtract(in_packed.z, 16, 4);
    uint block  = bitfieldExtract(in_packed.z, 20, 4);
    uint ao     = bitfieldExtract(in_packed.z, 24, 2);
    uint facing = bitfieldExtract(in_packed.z, 26, 3);

    // The biome tint, baked per block by the mesher: the average of the
    // twenty-five biome cells around it, sampled out of the pack's colormap.
    // White for the quads that declare no tint index, which is most of them.
    v_tint = vec3(float(bitfieldExtract(in_word3, 0, 8)),
                  float(bitfieldExtract(in_word3, 8, 8)),
                  float(bitfieldExtract(in_word3, 16, 8))) / 255.0;

    // The lightmap is a real 16x16 texture now, so this stage only says WHERE
    // to sample it. The half-texel offset lands on a texel centre, so that a
    // light level of 7 reads the value for 7 and not a blend of 6 and 7.
    v_light = (vec2(float(block), float(sky)) + 0.5) / 16.0;

    // What is left here is the part of brightness that belongs to the geometry
    // rather than to the light: the directional shading of the face, and the
    // ambient occlusion of the corner.
    float shade  = kFaceShade[min(facing, kFacingUnshaded)];
    v_brightness = shade * kAmbient[ao];

    vec3 origin = sections.origins[gl_InstanceIndex].xyz;
    vec3 world  = vec3(x, y, z) + origin;

    // Cylindrical, as vanilla's fog has been since 1.18.1: the larger of the
    // horizontal distance and the vertical one. Spherical fog closes in when
    // you look up, which is what the change fixed.
    vec3 relative   = world - push.camera_and_fog_start.xyz;
    v_fog_distance  = max(length(relative.xz), abs(relative.y));

    gl_Position = push.view_projection * vec4(world, 1.0);
}
