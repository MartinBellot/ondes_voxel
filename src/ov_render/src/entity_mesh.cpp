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

/// R = Rz · Ry · Rx, written out so each trigonometric call happens once.
///
/// Z, then Y, then X, which is the order the game composes a part's rotation in
/// (`Quaternionf.rotationZYX`). A different order is not a different
/// convention, it is a different pose: a head that both turns and tips ends up
/// somewhere else.
[[nodiscard]] std::array<Vec3f, 3> rotation_basis(Vec3f degrees) noexcept {
    const f32 cx = std::cos(to_radians(degrees.x));
    const f32 sx = std::sin(to_radians(degrees.x));
    const f32 cy = std::cos(to_radians(degrees.y));
    const f32 sy = std::sin(to_radians(degrees.y));
    const f32 cz = std::cos(to_radians(degrees.z));
    const f32 sz = std::sin(to_radians(degrees.z));
    return {Vec3f{cz * cy, sz * cy, -sy},
            Vec3f{cz * sy * sx - sz * cx, sz * sy * sx + cz * cx, cy * sx},
            Vec3f{cz * sy * cx + sz * sx, sz * sy * cx - cz * sx, cy * cx}};
}

/// A bone's own transform: rotate and scale about its pivot, then slide.
[[nodiscard]] Affine bone_local(const EntityBone& bone, const BonePose& pose) noexcept {
    const Vec3f degrees = bone.rest_rotation + pose.rotation;
    const Vec3f scale{bone.rest_scale.x * pose.scale.x, bone.rest_scale.y * pose.scale.y,
                      bone.rest_scale.z * pose.scale.z};
    Affine result;
    result.basis    = rotation_basis(degrees);
    result.basis[0] = result.basis[0] * scale.x;
    result.basis[1] = result.basis[1] * scale.y;
    result.basis[2] = result.basis[2] * scale.z;
    // Turn and scale about the pivot: take it to the origin, transform, put it
    // back — then the animation's slide.
    result.offset = bone.pivot - result.apply_direction(bone.pivot) + pose.offset;
    return result;
}

/// Model space to world space. See the header: this is a reflection.
[[nodiscard]] Affine model_to_world(const EntityPlacement& placement) noexcept {
    // Innermost first: the origin, the scale and the X flip, the model's own
    // yaw, the roll, the body's yaw, the feet.
    const f32 unit = placement.scale / kUnitsPerBlock;

    Affine flip;
    flip.basis[0] = Vec3f{-unit, 0.0F, 0.0F};
    flip.basis[1] = Vec3f{0.0F, unit, 0.0F};
    flip.basis[2] = Vec3f{0.0F, 0.0F, unit};
    flip.offset   = Vec3f{-placement.origin.x * unit, placement.origin.y * unit,
                        placement.origin.z * unit};

    Affine model_yaw;
    model_yaw.basis = rotation_basis(Vec3f{0.0F, placement.model_yaw, 0.0F});

    Affine roll;
    roll.basis = rotation_basis(Vec3f{0.0F, 0.0F, placement.roll});

    Affine yaw;
    yaw.basis  = rotation_basis(Vec3f{0.0F, 180.0F - placement.body_yaw, 0.0F});
    yaw.offset = placement.position + Vec3f{0.0F, placement.lift, 0.0F};

    if (placement.has_model_basis) {
        Affine pre;
        pre.basis  = placement.model_basis;
        pre.offset = placement.model_offset;
        return pre.then(flip).then(model_yaw).then(roll).then(yaw);
    }
    if (placement.model_pitch == 0.0F) {
        return flip.then(model_yaw).then(roll).then(yaw);
    }
    // About the model's origin, before the origin is added: the game's sense
    // about x is this project's reversed (its y points down).
    Affine pitch;
    pitch.basis = rotation_basis(Vec3f{-placement.model_pitch, 0.0F, 0.0F});
    return pitch.then(flip).then(model_yaw).then(roll).then(yaw);
}

/// One of a box's six faces, as the net lays it out. Format 1 only.
struct FaceLayout {
    Vec3f normal;
    f32   u_offset;
    f32   v_offset;
    f32   u_size;
    f32   v_size;
};

struct Corner {
    u8 x;
    u8 y;
    u8 z;
    u8 u;
    u8 v;
};

struct Face {
    FaceLayout            layout;
    std::array<Corner, 4> corners;
};

/// The net every Minecraft skin uses: a row of the top and bottom faces `d`
/// texels tall, then a row of the four sides `h` texels tall, in the order
/// right, front, left, back. Corners counter-clockwise seen from outside.
[[nodiscard]] std::array<Face, 6> box_faces(f32 w, f32 h, f32 d) noexcept {
    return {{
        {{Vec3f{-1.0F, 0.0F, 0.0F}, 0.0F, d, d, h},
         {{{0, 0, 0, 1, 1}, {0, 0, 1, 0, 1}, {0, 1, 1, 0, 0}, {0, 1, 0, 1, 0}}}},
        {{Vec3f{0.0F, 0.0F, -1.0F}, d, d, w, h},
         {{{0, 1, 0, 0, 0}, {1, 1, 0, 1, 0}, {1, 0, 0, 1, 1}, {0, 0, 0, 0, 1}}}},
        {{Vec3f{1.0F, 0.0F, 0.0F}, d + w, d, d, h},
         {{{1, 0, 1, 1, 1}, {1, 0, 0, 0, 1}, {1, 1, 0, 0, 0}, {1, 1, 1, 1, 0}}}},
        {{Vec3f{0.0F, 0.0F, 1.0F}, d + w + d, d, w, h},
         {{{1, 1, 1, 0, 0}, {0, 1, 1, 1, 0}, {0, 0, 1, 1, 1}, {1, 0, 1, 0, 1}}}},
        {{Vec3f{0.0F, 1.0F, 0.0F}, d, 0.0F, w, d},
         {{{0, 1, 1, 0, 0}, {1, 1, 1, 1, 0}, {1, 1, 0, 1, 1}, {0, 1, 0, 0, 1}}}},
        {{Vec3f{0.0F, -1.0F, 0.0F}, d + w, 0.0F, w, d},
         {{{0, 0, 0, 0, 0}, {1, 0, 0, 1, 0}, {1, 0, 1, 1, 1}, {0, 0, 1, 0, 1}}}},
    }};
}

[[nodiscard]] u8 to_byte(f32 value) noexcept {
    return static_cast<u8>(std::clamp(value * 255.0F + 0.5F, 0.0F, 255.0F));
}

[[nodiscard]] std::array<u8, 4> bytes_of_argb(u32 argb) noexcept {
    return {static_cast<u8>((argb >> 16U) & 0xFFU), static_cast<u8>((argb >> 8U) & 0xFFU),
            static_cast<u8>(argb & 0xFFU), static_cast<u8>((argb >> 24U) & 0xFFU)};
}

[[nodiscard]] std::array<u8, 4> bytes_of_rgb(u32 rgb) noexcept {
    return {static_cast<u8>((rgb >> 16U) & 0xFFU), static_cast<u8>((rgb >> 8U) & 0xFFU),
            static_cast<u8>(rgb & 0xFFU), 255};
}

/// Every bone's transform to model space, and whether it is hidden, composed
/// in declaration order. The parse guarantees a parent comes first, so this
/// needs no stack — and a fixed array rather than a vector, because this runs
/// once per entity per frame and a frame allocates nothing once warm.
struct Posed {
    std::array<Affine, kMaxBones> transforms{};
    std::array<bool, kMaxBones>   hidden{};
};

[[nodiscard]] bool pose_bones(const EntityModel& model, std::span<const BonePose> poses,
                              Posed& posed) noexcept {
    if (poses.size() != model.bones.size() || model.bones.size() > kMaxBones) {
        return false;
    }
    for (usize index = 0; index < model.bones.size(); ++index) {
        const EntityBone& bone  = model.bones[index];
        const Affine      local = bone_local(bone, poses[index]);
        const bool        hidden_here = !bone.visible || poses[index].hidden;
        if (bone.parent >= 0) {
            const auto parent        = static_cast<usize>(bone.parent);
            posed.transforms[index] = local.then(posed.transforms[parent]);
            posed.hidden[index]     = hidden_here || posed.hidden[parent];
        } else {
            posed.transforms[index] = local;
            posed.hidden[index]     = hidden_here;
        }
    }
    return true;
}

}  // namespace

f32 entity_shade(Vec3f world_normal) noexcept {
    // The game's two entity lights, in the world. Normalised here once; the
    // shader normalises them again, to the same numbers.
    static const Vec3f kLight0 = Vec3f{0.2F, 1.0F, -0.7F}.normalized();
    static const Vec3f kLight1 = Vec3f{-0.2F, 1.0F, 0.7F}.normalized();
    constexpr f32      kPower   = 0.6F;
    constexpr f32      kAmbient = 0.4F;
    const f32 light0 = std::max(0.0F, kLight0.dot(world_normal));
    const f32 light1 = std::max(0.0F, kLight1.dot(world_normal));
    return std::min(1.0F, (light0 + light1) * kPower + kAmbient);
}

u32 emit_entity(const EntityModel& model, std::span<const BonePose> poses,
                const EntityPlacement& placement, std::vector<EntityVertex>& out) {
    Posed posed;
    if (!pose_bones(model, poses, posed)) {
        return 0;
    }

    const Affine to_world = model_to_world(placement);

    const f32 tint_a = static_cast<f32>((placement.tint >> 24U) & 0xFFU) / 255.0F;
    const f32 tint_r = static_cast<f32>((placement.tint >> 16U) & 0xFFU) / 255.0F;
    const f32 tint_g = static_cast<f32>((placement.tint >> 8U) & 0xFFU) / 255.0F;
    const f32 tint_b = static_cast<f32>(placement.tint & 0xFFU) / 255.0F;
    const std::array<u8, 4> overlay = bytes_of_argb(placement.overlay);
    const std::array<u8, 4> light   = bytes_of_rgb(placement.light);

    const f32 rect_w = placement.uv.u1 - placement.uv.u0;
    const f32 rect_h = placement.uv.v1 - placement.uv.v0;
    const auto map_u = [&](f32 texel_u) {
        return placement.uv.u0 + (texel_u / model.texture_width + placement.uv_scroll_u) * rect_w;
    };
    const auto map_v = [&](f32 texel_v) {
        return placement.uv.v0 + (texel_v / model.texture_height + placement.uv_scroll_v) * rect_h;
    };
    const auto colour_for = [&](Vec3f world_normal) {
        const f32 shade = placement.shade ? entity_shade(world_normal.normalized()) : 1.0F;
        return std::array<u8, 4>{to_byte(tint_r * shade), to_byte(tint_g * shade),
                                 to_byte(tint_b * shade), to_byte(tint_a)};
    };

    u32 quads = 0;

    for (usize index = 0; index < model.bones.size(); ++index) {
        const EntityBone& bone = model.bones[index];
        if (!bone.render || posed.hidden[index]) {
            continue;
        }
        const Affine bone_to_world = posed.transforms[index].then(to_world);

        // Format 2: the game's own faces, already wound for the reflection.
        for (const EntityQuad& quad : bone.quads) {
            const std::array<u8, 4> colour =
                colour_for(bone_to_world.apply_direction(quad.normal));
            for (usize corner = 0; corner < 4; ++corner) {
                const Vec3f  world = bone_to_world.apply(quad.position[corner]);
                EntityVertex vertex;
                vertex.x       = world.x;
                vertex.y       = world.y;
                vertex.z       = world.z;
                vertex.u       = map_u(quad.u[corner]);
                vertex.v       = map_v(quad.v[corner]);
                vertex.colour  = colour;
                vertex.overlay = overlay;
                vertex.light   = light;
                out.push_back(vertex);
            }
            ++quads;
        }

        // Format 1: a box unfolded from its net.
        for (const EntityCube& cube : bone.cubes) {
            const f32 w = cube.size.x;
            const f32 h = cube.size.y;
            const f32 d = cube.size.z;

            const Vec3f low{cube.origin.x - cube.inflate, cube.origin.y - cube.inflate,
                            cube.origin.z - cube.inflate};
            const Vec3f high{cube.origin.x + w + cube.inflate, cube.origin.y + h + cube.inflate,
                             cube.origin.z + d + cube.inflate};

            for (const Face& face : box_faces(w, h, d)) {
                const std::array<u8, 4> colour =
                    colour_for(bone_to_world.apply_direction(face.layout.normal));

                std::array<EntityVertex, 4> quad{};
                for (usize corner_index = 0; corner_index < 4; ++corner_index) {
                    const Corner& corner = face.corners[corner_index];
                    const Vec3f   model_point{corner.x != 0 ? high.x : low.x,
                                            corner.y != 0 ? high.y : low.y,
                                            corner.z != 0 ? high.z : low.z};
                    const Vec3f   world_point = bone_to_world.apply(model_point);

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

                    quad[corner_index].x       = world_point.x;
                    quad[corner_index].y       = world_point.y;
                    quad[corner_index].z       = world_point.z;
                    quad[corner_index].u       = map_u(texel_u);
                    quad[corner_index].v       = map_v(texel_v);
                    quad[corner_index].colour  = colour;
                    quad[corner_index].overlay = overlay;
                    quad[corner_index].light   = light;
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

    Posed posed;
    if (!pose_bones(model, poses, posed)) {
        min = placement.position;
        max = placement.position;
        return;
    }
    const Affine to_world = model_to_world(placement);

    const auto include = [&](Vec3f world) {
        min.x = std::min(min.x, world.x);
        min.y = std::min(min.y, world.y);
        min.z = std::min(min.z, world.z);
        max.x = std::max(max.x, world.x);
        max.y = std::max(max.y, world.y);
        max.z = std::max(max.z, world.z);
    };

    bool any = false;
    for (usize index = 0; index < model.bones.size(); ++index) {
        const EntityBone& bone = model.bones[index];
        if (!bone.render || posed.hidden[index]) {
            continue;
        }
        const Affine bone_to_world = posed.transforms[index].then(to_world);
        for (const EntityQuad& quad : bone.quads) {
            for (const Vec3f& corner : quad.position) {
                include(bone_to_world.apply(corner));
            }
            any = true;
        }
        for (const EntityCube& cube : bone.cubes) {
            const Vec3f low{cube.origin.x - cube.inflate, cube.origin.y - cube.inflate,
                            cube.origin.z - cube.inflate};
            const Vec3f high{cube.origin.x + cube.size.x + cube.inflate,
                             cube.origin.y + cube.size.y + cube.inflate,
                             cube.origin.z + cube.size.z + cube.inflate};
            for (u32 corner = 0; corner < 8; ++corner) {
                include(bone_to_world.apply(Vec3f{(corner & 1U) != 0 ? high.x : low.x,
                                                  (corner & 2U) != 0 ? high.y : low.y,
                                                  (corner & 4U) != 0 ? high.z : low.z}));
            }
            any = true;
        }
    }

    if (!any) {
        min = placement.position;
        max = placement.position;
    }
}

bool bone_frame(const EntityModel& model, std::span<const BonePose> poses,
                const EntityPlacement& placement, i32 bone, BoneFrame& out) {
    if (bone < 0 || static_cast<usize>(bone) >= model.bones.size()) {
        return false;
    }
    Posed posed;
    if (!pose_bones(model, poses, posed)) {
        return false;
    }
    const auto   index         = static_cast<usize>(bone);
    const Affine bone_to_world = posed.transforms[index].then(model_to_world(placement));
    out.origin                 = bone_to_world.apply(model.bones[index].pivot);
    // Directions in model units map to world blocks through the /16 in the
    // placement; the frame's axes are given per model unit times 16, so that
    // BoneFrame::at can take model units.
    out.axis_x = bone_to_world.apply_direction(Vec3f{1.0F, 0.0F, 0.0F}) * kUnitsPerBlock;
    out.axis_y = bone_to_world.apply_direction(Vec3f{0.0F, 1.0F, 0.0F}) * kUnitsPerBlock;
    out.axis_z = bone_to_world.apply_direction(Vec3f{0.0F, 0.0F, 1.0F}) * kUnitsPerBlock;
    return !posed.hidden[index];
}

void emit_quad(const std::array<Vec3f, 4>& corners, const std::array<f32, 4>& u,
               const std::array<f32, 4>& v, u32 tint, std::vector<EntityVertex>& out, u32 light,
               u32 overlay) {
    const std::array<u8, 4> colour        = bytes_of_argb(tint);
    const std::array<u8, 4> overlay_bytes = bytes_of_argb(overlay);
    const std::array<u8, 4> light_bytes   = bytes_of_rgb(light);
    for (usize index = 0; index < 4; ++index) {
        EntityVertex vertex;
        vertex.x       = corners[index].x;
        vertex.y       = corners[index].y;
        vertex.z       = corners[index].z;
        vertex.u       = u[index];
        vertex.v       = v[index];
        vertex.colour  = colour;
        vertex.overlay = overlay_bytes;
        vertex.light   = light_bytes;
        out.push_back(vertex);
    }
}

}  // namespace ov::render
