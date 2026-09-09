#include "ov/render/entity_mesh.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace ov::render {

namespace {

constexpr f32 kUnitsPerBlock = 16.0F;

[[nodiscard]] f32 to_radians(f32 degrees) noexcept {
    return degrees * (std::numbers::pi_v<f32> / 180.0F);
}

/// An affine transform in model space: a 3x3 basis and a translation.
///
/// Not Mat4, on purpose. A bone chain composes a few hundred times a frame and
/// the fourth row would be three multiplies and an add of nothing.
struct Affine {
    std::array<Vec3f, 3> basis{Vec3f{1.0F, 0.0F, 0.0F}, Vec3f{0.0F, 1.0F, 0.0F},
                               Vec3f{0.0F, 0.0F, 1.0F}};
    Vec3f                offset{};

    [[nodiscard]] Vec3f apply(Vec3f point) const noexcept {
        return Vec3f{basis[0].x * point.x + basis[1].x * point.y + basis[2].x * point.z + offset.x,
                     basis[0].y * point.x + basis[1].y * point.y + basis[2].y * point.z + offset.y,
                     basis[0].z * point.x + basis[1].z * point.y + basis[2].z * point.z + offset.z};
    }

    [[nodiscard]] Vec3f apply_direction(Vec3f direction) const noexcept {
        return Vec3f{
            basis[0].x * direction.x + basis[1].x * direction.y + basis[2].x * direction.z,
            basis[0].y * direction.x + basis[1].y * direction.y + basis[2].y * direction.z,
            basis[0].z * direction.x + basis[1].z * direction.y + basis[2].z * direction.z};
    }

    [[nodiscard]] Affine then(const Affine& outer) const noexcept {
        Affine result;
        for (usize column = 0; column < 3; ++column) {
            result.basis[column] = outer.apply_direction(basis[column]);
        }
        result.offset = outer.apply(offset);
        return result;
    }
};

/// The rotation a bone applies about its own pivot.
///
/// Z, then Y, then X, which is the order the game composes them in. A different
/// order is not a different convention, it is a different pose: a head that
/// both turns and tips ends up somewhere else.
[[nodiscard]] Affine bone_rotation(Vec3f pivot, Vec3f degrees) noexcept {
    const f32 cx = std::cos(to_radians(degrees.x));
    const f32 sx = std::sin(to_radians(degrees.x));
    const f32 cy = std::cos(to_radians(degrees.y));
    const f32 sy = std::sin(to_radians(degrees.y));
    const f32 cz = std::cos(to_radians(degrees.z));
    const f32 sz = std::sin(to_radians(degrees.z));

    // R = Rz * Ry * Rx, written out rather than multiplied, so that the six
    // trigonometric calls happen once each.
    Affine result;
    result.basis[0] = Vec3f{cz * cy, sz * cy, -sy};
    result.basis[1] = Vec3f{cz * sy * sx - sz * cx, sz * sy * sx + cz * cx, cy * sx};
    result.basis[2] = Vec3f{cz * sy * cx + sz * sx, sz * sy * cx - cz * sx, cy * cx};

    // Rotate about the pivot: translate it to the origin, turn, put it back.
    result.offset = pivot - result.apply_direction(pivot);
    return result;
}

/// Model space to world space. See the header: this is a reflection.
[[nodiscard]] Affine model_to_world(const EntityPlacement& placement) noexcept {
    const f32 yaw   = to_radians(placement.body_yaw);
    const f32 sin_y = std::sin(yaw);
    const f32 cos_y = std::cos(yaw);
    const f32 unit  = placement.scale / kUnitsPerBlock;

    Affine result;
    result.basis[0] = Vec3f{cos_y * unit, 0.0F, sin_y * unit};
    result.basis[1] = Vec3f{0.0F, unit, 0.0F};
    result.basis[2] = Vec3f{sin_y * unit, 0.0F, -cos_y * unit};
    result.offset   = placement.position;
    return result;
}

/// One of a box's six faces, as the net lays it out.
struct FaceLayout {
    /// The face's outward normal in model space.
    Vec3f normal;
    /// Where its rectangle starts on the net, relative to the cube's uv, in
    /// texels, and how big it is.
    f32 u_offset;
    f32 v_offset;
    f32 u_size;
    f32 v_size;
};

/// The four corners of a face, counter-clockwise seen from **outside** in model
/// space, paired with the corner of the net rectangle each one takes.
///
/// The net is the one every Minecraft skin uses: a row of the top and bottom
/// faces `d` texels tall, then a row of the four sides `h` texels tall, in the
/// order right, front, left, back — reading a head's texture left to right
/// gives the right side of the face, the face, the left side, the back of the
/// head, which is exactly what `steve.png` shows.
struct Corner {
    /// 0 or 1 on each axis of the cube's box.
    u8 x;
    u8 y;
    u8 z;
    /// 0 or 1 on each axis of the net rectangle.
    u8 u;
    u8 v;
};

struct Face {
    FaceLayout            layout;
    std::array<Corner, 4> corners;
};

/// `w`, `h`, `d` stand in for the cube's size below; the offsets are filled in
/// per cube because they depend on it.
[[nodiscard]] std::array<Face, 6> box_faces(f32 w, f32 h, f32 d) noexcept {
    return {{
        // Right (−X): first side panel. u runs from the back of the cube to
        // its front, v downwards.
        {{Vec3f{-1.0F, 0.0F, 0.0F}, 0.0F, d, d, h},
         {{{0, 0, 0, 1, 1}, {0, 0, 1, 0, 1}, {0, 1, 1, 0, 0}, {0, 1, 0, 1, 0}}}},
        // Front (−Z): the face. u from the entity's right to its left.
        {{Vec3f{0.0F, 0.0F, -1.0F}, d, d, w, h},
         {{{0, 1, 0, 0, 0}, {1, 1, 0, 1, 0}, {1, 0, 0, 1, 1}, {0, 0, 0, 0, 1}}}},
        // Left (+X).
        {{Vec3f{1.0F, 0.0F, 0.0F}, d + w, d, d, h},
         {{{1, 0, 1, 1, 1}, {1, 0, 0, 0, 1}, {1, 1, 0, 0, 0}, {1, 1, 1, 1, 0}}}},
        // Back (+Z), read the other way round, as the fold demands.
        {{Vec3f{0.0F, 0.0F, 1.0F}, d + w + d, d, w, h},
         {{{1, 1, 1, 0, 0}, {0, 1, 1, 1, 0}, {0, 0, 1, 1, 1}, {1, 0, 1, 0, 1}}}},
        // Top (+Y): v = 0 is the back edge, v = d the front, so that the panel
        // folds down onto the face below it.
        {{Vec3f{0.0F, 1.0F, 0.0F}, d, 0.0F, w, d},
         {{{0, 1, 1, 0, 0}, {1, 1, 1, 1, 0}, {1, 1, 0, 1, 1}, {0, 1, 0, 0, 1}}}},
        // Bottom (−Y): mirrored in z against the top, which is the convention
        // the game's own textures are drawn to.
        {{Vec3f{0.0F, -1.0F, 0.0F}, d + w, 0.0F, w, d},
         {{{0, 0, 0, 0, 0}, {1, 0, 0, 1, 0}, {1, 0, 1, 1, 1}, {0, 0, 1, 0, 1}}}},
    }};
}

[[nodiscard]] u8 to_byte(f32 value) noexcept {
    return static_cast<u8>(std::clamp(value * 255.0F + 0.5F, 0.0F, 255.0F));
}

}  // namespace

f32 face_shade(Vec3f world_normal) noexcept {
    // The five values the terrain uses, chosen by the dominant axis. A bone
    // turned halfway between two faces takes the nearer one's shade, which is
    // what vanilla's flat lighting does for a rotated block model too.
    const f32 ax = std::abs(world_normal.x);
    const f32 ay = std::abs(world_normal.y);
    const f32 az = std::abs(world_normal.z);
    if (ay >= ax && ay >= az) {
        return world_normal.y >= 0.0F ? 1.0F : 0.5F;
    }
    if (az >= ax) {
        return 0.8F;
    }
    return 0.6F;
}

u32 emit_entity(const EntityModel& model, std::span<const BonePose> poses,
                const EntityPlacement& placement, std::vector<EntityVertex>& out) {
    if (poses.size() != model.bones.size()) {
        return 0;
    }

    const Affine to_world = model_to_world(placement);

    // One transform per bone, composed in declaration order. The loader
    // guarantees a parent comes first, so this needs no stack — and a fixed
    // array rather than a vector, because this runs once per entity per frame
    // and the tick's no-allocation rule is a good rule for a frame too.
    if (model.bones.size() > kMaxBones) {
        return 0;
    }
    std::array<Affine, kMaxBones> transforms{};

    const f32 tint_a = static_cast<f32>((placement.tint >> 24U) & 0xFFU) / 255.0F;
    const f32 tint_r = static_cast<f32>((placement.tint >> 16U) & 0xFFU) / 255.0F;
    const f32 tint_g = static_cast<f32>((placement.tint >> 8U) & 0xFFU) / 255.0F;
    const f32 tint_b = static_cast<f32>(placement.tint & 0xFFU) / 255.0F;

    u32 quads = 0;

    for (usize index = 0; index < model.bones.size(); ++index) {
        const EntityBone& bone  = model.bones[index];
        const BonePose&   pose  = poses[index];
        Affine            local = bone_rotation(bone.pivot, pose.rotation);
        local.offset            = local.offset + pose.offset;

        transforms[index] = bone.parent >= 0
                                ? local.then(transforms[static_cast<usize>(bone.parent)])
                                : local;

        if (!bone.render) {
            continue;
        }

        const Affine& bone_to_model = transforms[index];

        for (const EntityCube& cube : bone.cubes) {
            const f32 w = cube.size.x;
            const f32 h = cube.size.y;
            const f32 d = cube.size.z;

            const Vec3f low{cube.origin.x - cube.inflate, cube.origin.y - cube.inflate,
                            cube.origin.z - cube.inflate};
            const Vec3f high{cube.origin.x + w + cube.inflate, cube.origin.y + h + cube.inflate,
                             cube.origin.z + d + cube.inflate};

            for (const Face& face : box_faces(w, h, d)) {
                const Vec3f world_normal =
                    to_world.apply_direction(bone_to_model.apply_direction(face.layout.normal));
                const f32 shade = face_shade(world_normal.normalized());

                std::array<EntityVertex, 4> quad{};
                for (usize corner_index = 0; corner_index < 4; ++corner_index) {
                    const Corner& corner = face.corners[corner_index];
                    const Vec3f   model_point{corner.x != 0 ? high.x : low.x,
                                            corner.y != 0 ? high.y : low.y,
                                            corner.z != 0 ? high.z : low.z};
                    const Vec3f   world_point =
                        to_world.apply(bone_to_model.apply(model_point));

                    f32 texel_u = cube.uv_u + face.layout.u_offset +
                                  (corner.u != 0 ? face.layout.u_size : 0.0F);
                    const f32 texel_v = cube.uv_v + face.layout.v_offset +
                                        (corner.v != 0 ? face.layout.v_size : 0.0F);
                    if (cube.mirror) {
                        // A mirrored cube reads its net right to left, about
                        // the middle of the whole net rather than of the face:
                        // that is what swaps the left and right panels as well
                        // as flipping each one.
                        const f32 net_width = 2.0F * (w + d);
                        texel_u             = 2.0F * cube.uv_u + net_width - texel_u;
                    }

                    quad[corner_index].x      = world_point.x;
                    quad[corner_index].y      = world_point.y;
                    quad[corner_index].z      = world_point.z;
                    quad[corner_index].u      = texel_u / model.texture_width;
                    quad[corner_index].v      = texel_v / model.texture_height;
                    quad[corner_index].colour = {to_byte(tint_r * shade), to_byte(tint_g * shade),
                                                 to_byte(tint_b * shade), to_byte(tint_a)};
                }

                // Reversed: the corners above are counter-clockwise seen from
                // outside in model space, and model space reaches the world
                // through a reflection.
                out.push_back(quad[3]);
                out.push_back(quad[2]);
                out.push_back(quad[1]);
                out.push_back(quad[0]);
                ++quads;
            }
        }
    }

    return quads;
}

void entity_bounds(const EntityModel& model, std::span<const BonePose> poses,
                   const EntityPlacement& placement, Vec3f& min, Vec3f& max) {
    constexpr f32 kBig = std::numeric_limits<f32>::max();
    min                = Vec3f{kBig, kBig, kBig};
    max                = Vec3f{-kBig, -kBig, -kBig};
    if (poses.size() != model.bones.size()) {
        min = placement.position;
        max = placement.position;
        return;
    }

    if (model.bones.size() > kMaxBones) {
        min = placement.position;
        max = placement.position;
        return;
    }
    const Affine                  to_world = model_to_world(placement);
    std::array<Affine, kMaxBones> transforms{};

    bool any = false;
    for (usize index = 0; index < model.bones.size(); ++index) {
        const EntityBone& bone  = model.bones[index];
        Affine            local = bone_rotation(bone.pivot, poses[index].rotation);
        local.offset            = local.offset + poses[index].offset;
        transforms[index] = bone.parent >= 0
                                ? local.then(transforms[static_cast<usize>(bone.parent)])
                                : local;
        if (!bone.render) {
            continue;
        }
        for (const EntityCube& cube : bone.cubes) {
            const Vec3f low{cube.origin.x - cube.inflate, cube.origin.y - cube.inflate,
                            cube.origin.z - cube.inflate};
            const Vec3f high{cube.origin.x + cube.size.x + cube.inflate,
                             cube.origin.y + cube.size.y + cube.inflate,
                             cube.origin.z + cube.size.z + cube.inflate};
            for (u32 corner = 0; corner < 8; ++corner) {
                const Vec3f point{(corner & 1U) != 0 ? high.x : low.x,
                                  (corner & 2U) != 0 ? high.y : low.y,
                                  (corner & 4U) != 0 ? high.z : low.z};
                const Vec3f world = to_world.apply(transforms[index].apply(point));
                min.x             = std::min(min.x, world.x);
                min.y             = std::min(min.y, world.y);
                min.z             = std::min(min.z, world.z);
                max.x             = std::max(max.x, world.x);
                max.y             = std::max(max.y, world.y);
                max.z             = std::max(max.z, world.z);
                any               = true;
            }
        }
    }

    if (!any) {
        min = placement.position;
        max = placement.position;
    }
}

void emit_quad(const std::array<Vec3f, 4>& corners, const std::array<f32, 4>& u,
               const std::array<f32, 4>& v, u32 tint, std::vector<EntityVertex>& out) {
    const u8 alpha = static_cast<u8>((tint >> 24U) & 0xFFU);
    const u8 red   = static_cast<u8>((tint >> 16U) & 0xFFU);
    const u8 green = static_cast<u8>((tint >> 8U) & 0xFFU);
    const u8 blue  = static_cast<u8>(tint & 0xFFU);
    for (usize index = 0; index < 4; ++index) {
        EntityVertex vertex;
        vertex.x      = corners[index].x;
        vertex.y      = corners[index].y;
        vertex.z      = corners[index].z;
        vertex.u      = u[index];
        vertex.v      = v[index];
        vertex.colour = {red, green, blue, alpha};
        out.push_back(vertex);
    }
}

}  // namespace ov::render
