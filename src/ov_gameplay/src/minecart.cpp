#include "ov/gameplay/minecart.hpp"

#include "ov/gameplay/entity_physics.hpp"
#include "ov/gameplay/mob_logic.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ov::gameplay {

namespace {

[[nodiscard]] i32 block_floor(f64 value) noexcept {
    return static_cast<i32>(std::floor(value));
}

[[nodiscard]] f64 horizontal(const Vec3d& v) noexcept {
    return std::sqrt(v.x * v.x + v.z * v.z);
}

/// A move whose squared length is at most this does not happen at all.
/// Measured: every lane's position freezes while its Motion still reads a few
/// millionths, and without the threshold the replay drifts 0.0077 by the end.
constexpr f64 kMinMoveSqr = 1.0e-7;

[[nodiscard]] bool worth_moving(const Vec3d& d) noexcept {
    return d.x * d.x + d.y * d.y + d.z * d.z > kMinMoveSqr;
}

struct RailHere {
    BlockPos               pos{};
    registry::BlockStateId state{};
    RailShape              shape{RailShape::NorthSouth};
};

/// The rail a cart at `position` is on: in its own cell, or the one under it.
[[nodiscard]] std::optional<RailHere> rail_at(const Rails& rails, const world::LevelView& level,
                                              Vec3d position) {
    BlockPos cell{block_floor(position.x), block_floor(position.y), block_floor(position.z)};
    if (rails.is_rail(level.block_at(cell.below()))) {
        cell = cell.below();
    }
    const registry::BlockStateId state = level.block_at(cell);
    const auto                   shape = rails.shape_of(state);
    if (!shape) {
        return std::nullopt;
    }
    return RailHere{cell, state, *shape};
}

/// The two ends of a rail as points on its centre line: x and z at the middle
/// of the cell's edge, y at the rail's surface — one sixteenth above the cell's
/// floor, a block higher at a slope's raised end.
struct Segment {
    Vec3d a;
    Vec3d b;
};

[[nodiscard]] Segment segment_of(const RailHere& rail) noexcept {
    const RailEnds ends = rail_ends(rail.shape);
    const auto     point = [&](const RailEnd& end) {
        return Vec3d{static_cast<f64>(rail.pos.x) + 0.5 + static_cast<f64>(end.dx) * 0.5,
                     static_cast<f64>(rail.pos.y) + kMinecartRailHeight + (end.rises ? 1.0 : 0.0),
                     static_cast<f64>(rail.pos.z) + 0.5 + static_cast<f64>(end.dz) * 0.5};
    };
    return Segment{point(ends.first), point(ends.second)};
}

/// How far along a segment a point lies, 0 at `a` and 1 at `b`. A straight
/// rail reads its own axis; a curve projects onto the diagonal.
[[nodiscard]] f64 progress(const RailHere& rail, const Segment& seg, Vec3d position) noexcept {
    const f64 sx = seg.b.x - seg.a.x;
    const f64 sz = seg.b.z - seg.a.z;
    if (sx == 0.0) {
        return position.z - static_cast<f64>(rail.pos.z);
    }
    if (sz == 0.0) {
        return position.x - static_cast<f64>(rail.pos.x);
    }
    return ((position.x - seg.a.x) * sx + (position.z - seg.a.z) * sz) * 2.0;
}

[[nodiscard]] f32 wrap_degrees(f32 degrees) noexcept {
    f32 out = std::fmod(degrees, 360.0F);
    if (out >= 180.0F) {
        out -= 360.0F;
    }
    if (out < -180.0F) {
        out += 360.0F;
    }
    return out;
}

void off_rail(entity::EntityState& state, const MinecartBody& body,
              const CollisionWorld& collisions) {
    (void)body;
    Vec3d v = state.velocity;
    v.x     = std::clamp(v.x, -kMinecartMaxSpeed, kMinecartMaxSpeed);
    v.z     = std::clamp(v.z, -kMinecartMaxSpeed, kMinecartMaxSpeed);
    if (state.on_ground) {
        v.x *= 0.5;
        v.y *= 0.5;
        v.z *= 0.5;
    }
    const Vec3d allowed = collisions.slide(entity_box(state), v);
    if (worth_moving(allowed)) {
        state.position.x += allowed.x;
        state.position.y += allowed.y;
        state.position.z += allowed.z;
    }
    const bool vertical = allowed.y != v.y;
    state.on_ground     = vertical && v.y < 0.0;
    if (allowed.x != v.x) {
        v.x = 0.0;
    }
    if (allowed.z != v.z) {
        v.z = 0.0;
    }
    if (vertical) {
        v.y = 0.0;
    }
    if (!state.on_ground) {
        v.x *= kMinecartAirDrag;
        v.y *= kMinecartAirDrag;
        v.z *= kMinecartAirDrag;
    }
    state.velocity = v;
}

MinecartContact on_rail(entity::EntityState& state, MinecartBody& body, const Rails& rails,
                        const world::LevelView& level, const CollisionWorld& collisions,
                        const RailHere& rail) {
    MinecartContact contact;
    contact.on_rail = true;
    contact.rail    = rail.pos;
    contact.kind    = rails.kind_of(rail.state);
    contact.powered = rails.powered(rail.state);

    const std::optional<Vec3d> before = rail_point(rails, level, state.position);

    const bool boost = contact.kind == RailKind::Powered && contact.powered;
    const bool brake = contact.kind == RailKind::Powered && !contact.powered;

    Vec3d v = state.velocity;
    f64   y = static_cast<f64>(rail.pos.y);
    switch (rail.shape) {
        case RailShape::AscendingEast:
            v.x -= kMinecartSlope;
            y += 1.0;
            break;
        case RailShape::AscendingWest:
            v.x += kMinecartSlope;
            y += 1.0;
            break;
        case RailShape::AscendingNorth:
            v.z += kMinecartSlope;
            y += 1.0;
            break;
        case RailShape::AscendingSouth:
            v.z -= kMinecartSlope;
            y += 1.0;
            break;
        default: break;
    }

    // Onto the rail, keeping the speed and the sense of travel.
    const RailEnds ends = rail_ends(rail.shape);
    f64            dx   = static_cast<f64>(ends.second.dx - ends.first.dx);
    f64            dz   = static_cast<f64>(ends.second.dz - ends.first.dz);
    const f64      len  = std::sqrt(dx * dx + dz * dz);
    if (v.x * dx + v.z * dz < 0.0) {
        dx = -dx;
        dz = -dz;
    }
    const f64 speed = std::min(2.0, horizontal(v));
    v.x             = speed * dx / len;
    v.z             = speed * dz / len;

    // A rider's push only starts a cart that is almost still.
    if (body.ridden) {
        const f64 push = body.rider_impulse.x * body.rider_impulse.x +
                         body.rider_impulse.z * body.rider_impulse.z;
        if (push > 1.0e-4 && v.x * v.x + v.z * v.z < 0.01) {
            v.x += body.rider_impulse.x * 0.1;
            v.z += body.rider_impulse.z * 0.1;
        }
    }

    if (brake) {
        if (horizontal(v) < kMinecartBrakeStop) {
            v = Vec3d{0.0, 0.0, 0.0};
        } else {
            v.x *= 0.5;
            v.y = 0.0;
            v.z *= 0.5;
        }
    }

    // Back onto the centre line, then the move.
    const Segment seg = segment_of(rail);
    const f64     t   = progress(rail, seg, state.position);
    state.position    = Vec3d{seg.a.x + (seg.b.x - seg.a.x) * t, y, seg.a.z + (seg.b.z - seg.a.z) * t};

    const f64   factor = body.ridden ? kMinecartRiderFactor : 1.0;
    const f64   cap    = body.kind == MinecartKind::Furnace ? kFurnaceMaxSpeed : kMinecartMaxSpeed;
    const Vec3d delta{std::clamp(factor * v.x, -cap, cap), 0.0, std::clamp(factor * v.z, -cap, cap)};
    const Vec3d allowed = collisions.slide(entity_box(state), delta);
    if (worth_moving(allowed)) {
        state.position.x += allowed.x;
        state.position.z += allowed.z;
    }
    state.on_ground = false;
    // Off a slope's low end, the block of height the move was made with is
    // given back — or the rail one below is never found. Measured: without
    // it the slope lane parts from vanilla at tick 11, y −42 against −42.97.
    if (is_ascending(rail.shape)) {
        const RailEnd& low = ends.first.rises ? ends.second : ends.first;
        if (block_floor(state.position.x) - rail.pos.x == low.dx &&
            block_floor(state.position.z) - rail.pos.z == low.dz) {
            state.position.y -= 1.0;
        }
    }

    // The furnace's own drag and push, then every cart's.
    if (body.kind == MinecartKind::Furnace) {
        const f64 push = body.push_x * body.push_x + body.push_z * body.push_z;
        if (push > 1.0e-7) {
            const f64 norm = std::sqrt(push);
            body.push_x /= norm;
            body.push_z /= norm;
            v = Vec3d{v.x * 0.8 + body.push_x, 0.0, v.z * 0.8 + body.push_z};
        } else {
            v = Vec3d{v.x * 0.98, 0.0, v.z * 0.98};
        }
    }
    const f64 drag = body.ridden ? kMinecartDragRidden : kMinecartDragEmpty;
    v              = Vec3d{v.x * drag, 0.0, v.z * drag};

    // A drop in height becomes speed, and the cart settles on the rail.
    if (const std::optional<Vec3d> after = rail_point(rails, level, state.position);
        after && before) {
        const f64 gain = (before->y - after->y) * kMinecartHeightToSpeed;
        const f64 h    = horizontal(v);
        if (h > 0.0) {
            v.x = v.x * (h + gain) / h;
            v.z = v.z * (h + gain) / h;
        }
        state.position.y = after->y;
    }

    // A new cell turns the velocity towards it.
    const i32 nx = block_floor(state.position.x);
    const i32 nz = block_floor(state.position.z);
    if (nx != rail.pos.x || nz != rail.pos.z) {
        const f64 h = horizontal(v);
        v.x         = h * static_cast<f64>(nx - rail.pos.x);
        v.z         = h * static_cast<f64>(nz - rail.pos.z);
    }

    if (boost) {
        const f64 h = horizontal(v);
        if (h > 0.01) {
            v.x += v.x / h * kMinecartBoost;
            v.z += v.z / h * kMinecartBoost;
        } else if (rail.shape == RailShape::EastWest) {
            if (rails.conductor(level.block_at(rail.pos.offset(-1, 0, 0)))) {
                v.x = kMinecartLaunch;
            } else if (rails.conductor(level.block_at(rail.pos.offset(1, 0, 0)))) {
                v.x = -kMinecartLaunch;
            }
        } else if (rail.shape == RailShape::NorthSouth) {
            if (rails.conductor(level.block_at(rail.pos.offset(0, 0, -1)))) {
                v.z = kMinecartLaunch;
            } else if (rails.conductor(level.block_at(rail.pos.offset(0, 0, 1)))) {
                v.z = -kMinecartLaunch;
            }
        }
    }
    state.velocity = v;
    return contact;
}

}  // namespace

std::optional<MinecartKind> minecart_kind(std::string_view type_name) noexcept {
    for (usize i = 0; i < kMinecartTypes.size(); ++i) {
        if (kMinecartTypes[i] == type_name) {
            return static_cast<MinecartKind>(i);
        }
    }
    return std::nullopt;
}

std::optional<MinecartKind> minecart_for_item(std::string_view item) noexcept {
    if (item == "minecraft:minecart") {
        return MinecartKind::Rideable;
    }
    if (item == "minecraft:chest_minecart") {
        return MinecartKind::Chest;
    }
    if (item == "minecraft:furnace_minecart") {
        return MinecartKind::Furnace;
    }
    if (item == "minecraft:tnt_minecart") {
        return MinecartKind::Tnt;
    }
    if (item == "minecraft:hopper_minecart") {
        return MinecartKind::Hopper;
    }
    if (item == "minecraft:command_block_minecart") {
        return MinecartKind::CommandBlock;
    }
    return std::nullopt;
}

std::string_view minecart_item(MinecartKind kind) noexcept {
    switch (kind) {
        case MinecartKind::Chest: return "minecraft:chest_minecart";
        case MinecartKind::Furnace: return "minecraft:furnace_minecart";
        case MinecartKind::Tnt: return "minecraft:tnt_minecart";
        case MinecartKind::Hopper: return "minecraft:hopper_minecart";
        case MinecartKind::CommandBlock: return "minecraft:command_block_minecart";
        case MinecartKind::Rideable:
        case MinecartKind::Spawner: return "minecraft:minecart";
    }
    return "minecraft:minecart";
}

std::string_view minecart_block(MinecartKind kind) noexcept {
    switch (kind) {
        case MinecartKind::Chest: return "minecraft:chest";
        case MinecartKind::Furnace: return "minecraft:furnace";
        case MinecartKind::Tnt: return "minecraft:tnt";
        case MinecartKind::Hopper: return "minecraft:hopper";
        case MinecartKind::Spawner: return "minecraft:spawner";
        case MinecartKind::CommandBlock: return "minecraft:command_block";
        case MinecartKind::Rideable: return {};
    }
    return {};
}

std::optional<Vec3d> rail_point(const Rails& rails, const world::LevelView& level,
                                Vec3d position) {
    const std::optional<RailHere> rail = rail_at(rails, level, position);
    if (!rail) {
        return std::nullopt;
    }
    const Segment seg = segment_of(*rail);
    const f64     t   = progress(*rail, seg, position);
    return Vec3d{seg.a.x + (seg.b.x - seg.a.x) * t, seg.a.y + (seg.b.y - seg.a.y) * t,
                 seg.a.z + (seg.b.z - seg.a.z) * t};
}

MinecartContact step_minecart(entity::EntityState& state, MinecartBody& body, const Rails& rails,
                              const world::LevelView& level, const CollisionWorld& collisions) {
    if (body.hurt_time > 0) {
        --body.hurt_time;
    }
    if (body.damage > 0.0F) {
        body.damage = std::max(0.0F, body.damage - 1.0F);
    }
    const Vec3d previous = state.position;
    state.velocity.y -= kMinecartGravity;

    MinecartContact contact;
    if (const std::optional<RailHere> rail = rail_at(rails, level, state.position)) {
        contact = on_rail(state, body, rails, level, collisions, *rail);
    } else {
        off_rail(state, body, collisions);
    }

    // Face the way it rolls, flipping half a turn rather than spinning round
    // at every reversal.
    state.pitch   = 0.0F;
    const f64 mdx = previous.x - state.position.x;
    const f64 mdz = previous.z - state.position.z;
    if (mdx * mdx + mdz * mdz > 0.001) {
        f32 yaw = static_cast<f32>(std::atan2(mdz, mdx) * 180.0 / std::numbers::pi);
        if (body.flipped) {
            yaw += 180.0F;
        }
        const f32 turn = wrap_degrees(yaw - state.yaw);
        if (turn < -170.0F || turn >= 170.0F) {
            yaw += 180.0F;
            body.flipped = !body.flipped;
        }
        state.yaw = std::fmod(yaw, 360.0F);
        if (state.yaw < 0.0F) {
            state.yaw += 360.0F;
        }
    }
    return contact;
}

f32 tnt_minecart_power(f64 horizontal_speed_sqr, math::LegacyRandomSource& rng) {
    const f64 speed = std::min(5.0, std::sqrt(horizontal_speed_sqr));
    const f64 draw  = rng.next_double();
    return static_cast<f32>(4.0 + draw * 1.5 * speed);
}

void MinecartLogic::tick(entity::EntityWorld& world, entity::EntityHandle self,
                         const entity::TickContext& context) {
    const MobContext*    mob   = mob_context(context);
    entity::EntityState* state = world.mutable_state(self);
    if (state == nullptr || state->removed) {
        return;
    }
    if (mob == nullptr || mob->world == nullptr || mob->level == nullptr) {
        return;
    }
    // A cart in a chunk nobody holds waits, rather than falling through
    // terrain that is not there yet.
    const BlockPos cell{block_floor(state->position.x), block_floor(state->position.y),
                        block_floor(state->position.z)};
    if (!mob->level->is_loaded(cell)) {
        return;
    }
    const MinecartContact contact = step_minecart(*state, body_, *rails_, *mob->level, *mob->world);

    if (body_.kind == MinecartKind::Furnace && body_.fuel > 0) {
        --body_.fuel;
        if (body_.fuel <= 0) {
            body_.push_x = 0.0;
            body_.push_z = 0.0;
        }
    }
    if (contact.on_rail && contact.kind == RailKind::Activator) {
        if (body_.kind == MinecartKind::Tnt && contact.powered && body_.fuse < 0) {
            body_.fuse = kTntMinecartFuse;
        }
        if (body_.kind == MinecartKind::Hopper) {
            body_.enabled = !contact.powered;
        }
    }
    if (body_.kind == MinecartKind::Tnt && body_.fuse > 0) {
        --body_.fuse;
        if (body_.fuse == 0 && events_->blasts != nullptr) {
            math::LegacyRandomSource rng{static_cast<i64>(state->network_id) * 0x5DEECE66DLL};
            const f64 speed = state->velocity.x * state->velocity.x +
                              state->velocity.z * state->velocity.z;
            events_->blasts->blasts.push_back(BlastEvents::Blast{
                Vec3d{state->position.x, state->position.y, state->position.z},
                tnt_minecart_power(speed, rng), BlockInteraction::Destroy, state->network_id});
            state->removed = true;
        }
    }
    if (contact.on_rail) {
        events_->contacts.push_back(MinecartEvents::Contact{state->network_id, contact});
    }
}

}  // namespace ov::gameplay
