#include "ov/render/baked_model.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace ov::render {

namespace {

constexpr f32 kBlockUnits = 16.0F;

/// The model format's degrees are clockwise about the positive axis, which is
/// the opposite sense to the usual right-handed rotation matrix. That is not a
/// preference: `minecraft:furnace` faces north at y=0 and east at y=90, so
/// rotate_y(90) has to send -Z to +X, and `minecraft:observer` faces down at
/// x=90, so rotate_x(90) has to send -Z to -Y. Writing the matrices the other
/// way round puts every directional block on the wrong wall.
[[nodiscard]] Vec3f rotate_x(Vec3f p, f32 radians) {
    const f32 c = std::cos(radians);
    const f32 s = std::sin(radians);
    return {p.x, p.y * c + p.z * s, -p.y * s + p.z * c};
}

[[nodiscard]] Vec3f rotate_y(Vec3f p, f32 radians) {
    const f32 c = std::cos(radians);
    const f32 s = std::sin(radians);
    return {p.x * c - p.z * s, p.y, p.x * s + p.z * c};
}

[[nodiscard]] Vec3f rotate_z(Vec3f p, f32 radians) {
    const f32 c = std::cos(radians);
    const f32 s = std::sin(radians);
    return {p.x * c + p.y * s, -p.x * s + p.y * c, p.z};
}

[[nodiscard]] f32 to_radians(f32 degrees) {
    return degrees * (std::numbers::pi_v<f32> / 180.0F);
}

/// The blockstate rotation: x first, then y, both about the block's centre.
[[nodiscard]] Vec3f apply_variant_rotation(Vec3f p, i32 x_degrees, i32 y_degrees) {
    constexpr Vec3f centre{8.0F, 8.0F, 8.0F};
    Vec3f           local = p - centre;
    if (x_degrees != 0) {
        local = rotate_x(local, to_radians(static_cast<f32>(x_degrees)));
    }
    if (y_degrees != 0) {
        local = rotate_y(local, to_radians(static_cast<f32>(y_degrees)));
    }
    return local + centre;
}

/// Same rotation applied to a direction rather than a point.
[[nodiscard]] Vec3f rotate_vector(Vec3f v, i32 x_degrees, i32 y_degrees) {
    Vec3f out = v;
    if (x_degrees != 0) {
        out = rotate_x(out, to_radians(static_cast<f32>(x_degrees)));
    }
    if (y_degrees != 0) {
        out = rotate_y(out, to_radians(static_cast<f32>(y_degrees)));
    }
    return out;
}

[[nodiscard]] Direction nearest_direction(Vec3f v) {
    Direction best  = Direction::Down;
    f32       score = -1e30F;
    for (u8 i = 0; i < kDirectionCount; ++i) {
        const auto  direction = static_cast<Direction>(i);
        const Vec3i offset    = direction_offset(direction);
        const f32   dot = v.x * static_cast<f32>(offset.x) + v.y * static_cast<f32>(offset.y) +
                          v.z * static_cast<f32>(offset.z);
        if (dot > score) {
            score = dot;
            best  = direction;
        }
    }
    return best;
}

/// The world axis that texture u runs along for each face, and the one v runs
/// along, as unit vectors in the model's 0..16 space.
///
/// These encode the uv mapping derived in docs/PROVENANCE.md:
///   down, up : u = x,      v = z
///   north    : u = 16 - x, v = 16 - y
///   south    : u = x,      v = 16 - y
///   west     : u = z,      v = 16 - y
///   east     : u = 16 - z, v = 16 - y
struct FaceBasis {
    Vec3f u_axis;
    Vec3f v_axis;
};

[[nodiscard]] FaceBasis face_basis(Direction direction) {
    switch (direction) {
        case Direction::Down: return {{1, 0, 0}, {0, 0, 1}};
        case Direction::Up: return {{1, 0, 0}, {0, 0, 1}};
        case Direction::North: return {{-1, 0, 0}, {0, -1, 0}};
        case Direction::South: return {{1, 0, 0}, {0, -1, 0}};
        case Direction::West: return {{0, 0, 1}, {0, -1, 0}};
        case Direction::East: return {{0, 0, -1}, {0, -1, 0}};
    }
    return {{1, 0, 0}, {0, 0, 1}};
}

/// Value of u (or v) at a point, given the axis it runs along. An axis of -x
/// means u = 16 - x, which is what the negative unit vector encodes.
[[nodiscard]] f32 axis_value(Vec3f axis, Vec3f point) {
    const f32 projected = axis.x * point.x + axis.y * point.y + axis.z * point.z;
    // A negative axis measures back from the far edge of the block.
    const f32 offset = (axis.x + axis.y + axis.z) < 0.0F ? kBlockUnits : 0.0F;
    return offset + projected;
}

/// The four corners of one face of a cuboid, in the parametric order
/// (s,t) = (0,0), (0,1), (1,1), (1,0) where s runs along u and t along v.
struct FaceCorners {
    std::array<Vec3f, 4>              positions;
    std::array<std::array<f32, 2>, 4> st;
    std::array<f32, 4>                default_uv;
};

[[nodiscard]] FaceCorners face_corners(const Element& element, Direction direction) {
    const auto  basis  = face_basis(direction);
    const Vec3i normal = direction_offset(direction);

    // The face lies at whichever of from/to the normal points to.
    const auto pick = [](i32 sign, f32 low, f32 high) { return sign > 0 ? high : low; };

    const f32 u_lo =
        std::min(axis_value(basis.u_axis, element.from), axis_value(basis.u_axis, element.to));
    const f32 u_hi =
        std::max(axis_value(basis.u_axis, element.from), axis_value(basis.u_axis, element.to));
    const f32 v_lo =
        std::min(axis_value(basis.v_axis, element.from), axis_value(basis.v_axis, element.to));
    const f32 v_hi =
        std::max(axis_value(basis.v_axis, element.from), axis_value(basis.v_axis, element.to));

    FaceCorners corners;
    corners.default_uv = {u_lo, v_lo, u_hi, v_hi};
    corners.st         = {{{0.0F, 0.0F}, {0.0F, 1.0F}, {1.0F, 1.0F}, {1.0F, 0.0F}}};

    for (usize i = 0; i < 4; ++i) {
        const f32 u = u_lo + corners.st[i][0] * (u_hi - u_lo);
        const f32 v = v_lo + corners.st[i][1] * (v_hi - v_lo);

        // Invert the uv mapping to recover the position: u and v each pin one
        // axis, and the normal pins the third.
        Vec3f      position{};
        const auto set_axis = [&position](Vec3f axis, f32 value) {
            const f32 magnitude = (axis.x + axis.y + axis.z) < 0.0F ? kBlockUnits - value : value;
            if (axis.x != 0.0F) {
                position.x = magnitude;
            } else if (axis.y != 0.0F) {
                position.y = magnitude;
            } else {
                position.z = magnitude;
            }
        };
        set_axis(basis.u_axis, u);
        set_axis(basis.v_axis, v);

        if (normal.x != 0) {
            position.x = pick(normal.x, element.from.x, element.to.x);
        } else if (normal.y != 0) {
            position.y = pick(normal.y, element.from.y, element.to.y);
        } else {
            position.z = pick(normal.z, element.from.z, element.to.z);
        }
        corners.positions[i] = position;
    }
    return corners;
}

/// Rotate parametric texture coordinates a quarter turn at a time. Four turns
/// are the identity and two turns are (1-s, 1-t), which is what the tests pin.
[[nodiscard]] std::array<f32, 2> rotate_st(std::array<f32, 2> st, i32 quarter_turns) {
    auto out = st;
    for (i32 i = 0; i < ((quarter_turns % 4) + 4) % 4; ++i) {
        out = {out[1], 1.0F - out[0]};
    }
    return out;
}

/// uvlock: keep the texture aligned to the world instead of letting it turn
/// with the block.
///
/// The face's u and v run along two world axes. The blockstate rotation turns
/// those axes; uvlock says to use the axes the *rotated* face would have had
/// on its own instead. Since both bases are axis-aligned, the correction is an
/// integer remapping of (s, t), which is what this computes.
[[nodiscard]] std::array<f32, 2> apply_uvlock(std::array<f32, 2> st, Direction original,
                                              Direction rotated, i32 x_degrees, i32 y_degrees) {
    const auto before = face_basis(original);
    const auto after  = face_basis(rotated);

    const Vec3f rotated_u = rotate_vector(before.u_axis, x_degrees, y_degrees);
    const Vec3f rotated_v = rotate_vector(before.v_axis, x_degrees, y_degrees);

    const auto component = [](Vec3f value, Vec3f axis) {
        return value.x * axis.x + value.y * axis.y + value.z * axis.z;
    };

    // rotated_u = a * after.u + b * after.v, and likewise for rotated_v. Every
    // coefficient is 0 or ±1 because the rotation is a multiple of 90°.
    const f32 a = std::round(component(rotated_u, after.u_axis));
    const f32 b = std::round(component(rotated_u, after.v_axis));
    const f32 c = std::round(component(rotated_v, after.u_axis));
    const f32 d = std::round(component(rotated_v, after.v_axis));

    f32 s = a * st[0] + c * st[1];
    f32 t = b * st[0] + d * st[1];
    // A negative coefficient sends the unit square to [-1, 0]; shift it back.
    if (a + c < 0.0F) {
        s += 1.0F;
    }
    if (b + d < 0.0F) {
        t += 1.0F;
    }
    return {s, t};
}

}  // namespace

std::array<f32, 4> default_face_uv(const Element& element, Direction direction) {
    return face_corners(element, direction).default_uv;
}

Direction rotate_direction(Direction direction, i32 x_degrees, i32 y_degrees) {
    const Vec3i offset  = direction_offset(direction);
    const Vec3f rotated = rotate_vector(
        {static_cast<f32>(offset.x), static_cast<f32>(offset.y), static_cast<f32>(offset.z)},
        x_degrees, y_degrees);
    return nearest_direction(rotated);
}

BakedModel bake(const Model& model, const ModelVariant& variant) {
    BakedModel baked;
    baked.ambient_occlusion = model.ambient_occlusion;

    for (const auto& element : model.elements) {
        for (u8 index = 0; index < kDirectionCount; ++index) {
            const auto  direction = static_cast<Direction>(index);
            const auto& face      = element.faces[index];
            if (!face) {
                continue;
            }

            const auto corners = face_corners(element, direction);
            const auto uv      = face->uv.value_or(corners.default_uv);

            BakedQuad quad;
            quad.sprite     = face->sprite;
            quad.tint_index = face->tint_index;
            quad.shade      = element.shade;
            if (face->cullface) {
                quad.cullface = rotate_direction(*face->cullface, variant.x, variant.y);
            }

            const auto rotated_direction = rotate_direction(direction, variant.x, variant.y);
            const i32  quarter_turns     = face->rotation / 90;

            for (usize corner = 0; corner < 4; ++corner) {
                Vec3f position = corners.positions[corner];

                // Element rotation first: it is inside the model and happens
                // before the blockstate ever turns the block.
                if (element.rotation) {
                    const auto& rotation = *element.rotation;
                    const f32   radians  = to_radians(rotation.angle);
                    Vec3f       local    = position - rotation.origin;
                    switch (rotation.axis) {
                        case Axis::X: local = rotate_x(local, radians); break;
                        case Axis::Y: local = rotate_y(local, radians); break;
                        case Axis::Z: local = rotate_z(local, radians); break;
                    }
                    if (rotation.rescale) {
                        // A rotated element no longer reaches its neighbours;
                        // rescale stretches it back out along the two axes it
                        // turned in. 1/cos is exactly the factor that puts the
                        // corner back on the block edge.
                        const f32 factor = 1.0F / std::cos(radians);
                        switch (rotation.axis) {
                            case Axis::X:
                                local.y *= factor;
                                local.z *= factor;
                                break;
                            case Axis::Y:
                                local.x *= factor;
                                local.z *= factor;
                                break;
                            case Axis::Z:
                                local.x *= factor;
                                local.y *= factor;
                                break;
                        }
                    }
                    position = local + rotation.origin;
                }

                position = apply_variant_rotation(position, variant.x, variant.y);

                auto st = rotate_st(corners.st[corner], quarter_turns);
                if (variant.uvlock && (variant.x != 0 || variant.y != 0)) {
                    st = apply_uvlock(st, direction, rotated_direction, variant.x, variant.y);
                }

                quad.vertices[corner].position = position * (1.0F / kBlockUnits);
                quad.vertices[corner].u        = uv[0] + st[0] * (uv[2] - uv[0]);
                quad.vertices[corner].v        = uv[1] + st[1] * (uv[3] - uv[1]);
            }

            // Wind the quad so its normal points out of the block. The
            // parametric corner order is outward for some faces and inward for
            // others, because the uv mapping is not a consistent handedness —
            // up and down share one, which is a vanilla quirk, not ours.
            const Vec3f edge1     = quad.vertices[1].position - quad.vertices[0].position;
            const Vec3f edge2     = quad.vertices[3].position - quad.vertices[0].position;
            const Vec3f normal    = edge1.cross(edge2);
            const Vec3i outward   = direction_offset(rotated_direction);
            const f32   alignment = normal.x * static_cast<f32>(outward.x) +
                                    normal.y * static_cast<f32>(outward.y) +
                                    normal.z * static_cast<f32>(outward.z);
            if (alignment < 0.0F) {
                std::swap(quad.vertices[1], quad.vertices[3]);
            }

            // Facing comes from the quad's own normal, not from the face it
            // started as: a cross model's 45° elements face halfway between two
            // axes, and shading has to pick the nearer one.
            const Vec3f final_normal =
                (quad.vertices[1].position - quad.vertices[0].position)
                    .cross(quad.vertices[3].position - quad.vertices[0].position);
            quad.facing = nearest_direction(final_normal);

            baked.quads.push_back(std::move(quad));
        }
    }

    return baked;
}

}  // namespace ov::render
