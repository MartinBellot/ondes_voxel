#include "ov/gameplay/projectile.hpp"

#include "ov/gameplay/entity_physics.hpp"
#include "ov/gameplay/mob_logic.hpp"
#include "ov/math/raycast.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace ov::gameplay {
namespace {

/// The float sine table the game reads every angle out of.
///
/// The same table as `mth_sin` in ov_worldgen (carver.cpp), which is layer 10
/// and out of reach from here. A 256 KB constant built once, not state.
[[nodiscard]] const std::array<f32, 65536>& sin_table() noexcept {
    static const std::array<f32, 65536> table = [] {
        std::array<f32, 65536> values{};
        for (usize i = 0; i < values.size(); ++i) {
            values[i] =
                static_cast<f32>(std::sin(static_cast<f64>(i) * 3.141592653589793 * 2.0 / 65536.0));
        }
        return values;
    }();
    return table;
}

/// Java's float-to-int, which saturates where C++ is undefined.
[[nodiscard]] usize sin_index(f32 scaled) noexcept {
    constexpr f32 kIntMax = 2147483648.0F;
    if (!(scaled > -kIntMax) || !(scaled < kIntMax)) {
        return scaled > 0.0F ? 0xFFFFU : 0U;
    }
    return static_cast<usize>(static_cast<u32>(static_cast<i32>(scaled)) & 0xFFFFU);
}

[[nodiscard]] f32 table_sin(f32 value) noexcept {
    return sin_table()[sin_index(value * 10430.378F)];
}

[[nodiscard]] f32 table_cos(f32 value) noexcept {
    return sin_table()[sin_index(value * 10430.378F + 16384.0F)];
}

constexpr f32 kDegreesToRadians = 0.017453292F;

/// A box the size of this projectile at `position`.
[[nodiscard]] AABB box_at(const Vec3d& position, const ProjectileMotion& motion) noexcept {
    return AABB::from_entity(position, motion.width, motion.height);
}

/// The protocol's face index for leaving an axis in a direction.
[[nodiscard]] i32 face_of(int axis, bool from_below) noexcept {
    // -Y 0, +Y 1, -Z 2, +Z 3, -X 4, +X 5. Entering a box moving +x crosses
    // its -X face.
    switch (axis) {
        case 0: return from_below ? 4 : 5;
        case 1: return from_below ? 0 : 1;
        default: return from_below ? 2 : 3;
    }
}

struct SlabHit {
    f64 t{0.0};
    i32 face{1};
};

/// Slab test. `inside` entries are reported at t = 0.
[[nodiscard]] std::optional<SlabHit> slab(const AABB& box, const Vec3d& from,
                                          const Vec3d& to) noexcept {
    const f64 d[3]  = {to.x - from.x, to.y - from.y, to.z - from.z};
    const f64 o[3]  = {from.x, from.y, from.z};
    const f64 lo[3] = {box.min.x, box.min.y, box.min.z};
    const f64 hi[3] = {box.max.x, box.max.y, box.max.z};
    f64       t0    = 0.0;
    f64       t1    = 1.0;
    i32       face  = -1;
    for (int axis = 0; axis < 3; ++axis) {
        if (d[axis] == 0.0) {
            if (o[axis] < lo[axis] || o[axis] > hi[axis]) {
                return std::nullopt;
            }
            continue;
        }
        f64        near = (lo[axis] - o[axis]) / d[axis];
        f64        far  = (hi[axis] - o[axis]) / d[axis];
        const bool rising = d[axis] > 0.0;
        if (!rising) {
            std::swap(near, far);
        }
        if (near > t0) {
            t0   = near;
            face = face_of(axis, rising);
        }
        t1 = std::min(t1, far);
        if (t0 > t1) {
            return std::nullopt;
        }
    }
    if (face < 0) {
        // The segment starts inside. The face is the one it is heading out of,
        // reversed — the side a player would see it poke into.
        const f64 ax = std::abs(d[0]);
        const f64 ay = std::abs(d[1]);
        const f64 az = std::abs(d[2]);
        if (ax >= ay && ax >= az) {
            face = face_of(0, d[0] > 0.0);
        } else if (ay >= az) {
            face = face_of(1, d[1] > 0.0);
        } else {
            face = face_of(2, d[2] > 0.0);
        }
    }
    return SlabHit{t0, face};
}

constexpr std::array<std::string_view, 8> kTypeNames{
    "minecraft:arrow",       "minecraft:spectral_arrow", "minecraft:trident",
    "minecraft:snowball",    "minecraft:egg",            "minecraft:ender_pearl",
    "minecraft:experience_bottle", "minecraft:potion",
};

constexpr std::array<std::string_view, 12> kGaps{
    "piercing: an arrow stops at its first hit",
    "multishot: a crossbow fires one arrow",
    "flame and fire arrows: this server has no fire on entities",
    // ── brewing ── applied since brewing_session, to players only
    "tipped arrows: the potion reaches players only, mobs carry no effects",
    "spectral arrow: glowing reaches players only",
    "loyalty: a trident does not come back",
    "riptide and channeling",
    "ender pearl: no endermite",
    "splash and lingering potions reach players only, mobs carry no effects",
    "experience bottle: breaks, and drops no orbs",
    "an arrow whose block is removed does not fall again",
    "arrows stuck in a mob (the arrow count) are not shown",
};

}  // namespace

std::optional<ProjectileKind> projectile_kind(std::string_view type_name) noexcept {
    for (usize i = 0; i < kTypeNames.size(); ++i) {
        if (kTypeNames[i] == type_name) {
            return static_cast<ProjectileKind>(i);
        }
    }
    return std::nullopt;
}

std::string_view projectile_type_name(ProjectileKind kind) noexcept {
    return kTypeNames[static_cast<usize>(kind)];
}

ProjectileMotion projectile_motion(ProjectileKind kind) noexcept {
    // Every value measured by scripts/measure_projectiles.py flight. The box
    // sizes are the measured entity table's (docs/provenance: measure_entities).
    ProjectileMotion motion;
    switch (kind) {
        case ProjectileKind::Arrow:
        case ProjectileKind::SpectralArrow:
            motion.gravity    = static_cast<f64>(0.05F);
            motion.water_drag = static_cast<f64>(0.6F);
            motion.width      = 0.5;
            motion.height     = 0.5;
            break;
        case ProjectileKind::Trident:
            motion.gravity    = static_cast<f64>(0.05F);
            motion.water_drag = static_cast<f64>(0.99F);
            motion.width      = 0.5;
            motion.height     = 0.5;
            break;
        case ProjectileKind::Snowball:
        case ProjectileKind::Egg:
        case ProjectileKind::EnderPearl:
            motion.gravity    = static_cast<f64>(0.03F);
            motion.water_drag = static_cast<f64>(0.8F);
            motion.width      = 0.25;
            motion.height     = 0.25;
            break;
        case ProjectileKind::ExperienceBottle:
            motion.gravity    = static_cast<f64>(0.07F);
            motion.water_drag = static_cast<f64>(0.8F);
            motion.width      = 0.25;
            motion.height     = 0.25;
            break;
        case ProjectileKind::Potion:
            motion.gravity    = static_cast<f64>(0.05F);
            motion.water_drag = static_cast<f64>(0.8F);
            motion.width      = 0.25;
            motion.height     = 0.25;
            break;
    }
    return motion;
}

// ── Rays ────────────────────────────────────────────────────────────────────

std::optional<f64> clip_box(const AABB& box, const Vec3d& from, const Vec3d& to) noexcept {
    if (const auto hit = slab(box, from, to)) {
        return hit->t;
    }
    return std::nullopt;
}

std::optional<BlockClip> clip_blocks(const CollisionWorld& world, const Vec3d& from,
                                     const Vec3d& to) {
    const Vec3d delta  = to - from;
    const f64   length = delta.length();
    if (length <= 0.0) {
        return std::nullopt;
    }
    std::optional<BlockClip> best;
    std::vector<AABB>        boxes;
    // One cell at a time, in the order the segment enters them; the first cell
    // whose shape the segment touches is the answer. A shape that pokes out of
    // its cell (a fence) is only tested from its own cell — the game does the
    // same.
    (void)raycast_voxels(from, delta, length, [&](BlockPos cell) {
        boxes.clear();
        world.boxes_at(cell.x, cell.y, cell.z, boxes);
        std::optional<SlabHit> nearest;
        for (const AABB& box : boxes) {
            if (const auto hit = slab(box, from, to); hit && (!nearest || hit->t < nearest->t)) {
                nearest = hit;
            }
        }
        if (!nearest) {
            return false;
        }
        best = BlockClip{from + delta * nearest->t, cell, nearest->face};
        return true;
    });
    return best;
}

bool box_in_water(const world::LevelView& level, const AABB& box, registry::BlockId water) {
    if (water.value() == 0) {
        return false;
    }
    const registry::BlockRegistry& blocks = level.blocks();
    for (i32 x = aabb_min_block(box.min.x); x <= aabb_max_block(box.max.x); ++x) {
        for (i32 y = aabb_min_block(box.min.y); y <= aabb_max_block(box.max.y); ++y) {
            for (i32 z = aabb_min_block(box.min.z); z <= aabb_max_block(box.max.z); ++z) {
                // ── implicit water ── a waterlogged block, seagrass or kelp
                // puts an arrow out as water does.
                if (blocks.fluid(level.block_at(BlockPos{x, y, z})).is_water()) {
                    return true;
                }
            }
        }
    }
    return false;
}

// ── The step ────────────────────────────────────────────────────────────────

std::optional<ProjectileEvent> step_projectile(entity::EntityState& state, ProjectileData& data,
                                               const CollisionWorld&   world,
                                               const world::LevelView* level,
                                               const ProjectileWorld&  targets) {
    const ProjectileMotion motion = projectile_motion(data.kind);

    if (data.in_ground) {
        ++data.life;
        if (data.life >= kArrowDespawnTicks) {
            return ProjectileEvent{ProjectileEvent::Kind::Expired, state.network_id, 0, false,
                                   state.position, state.velocity};
        }
        return std::nullopt;
    }
    ++data.age;

    // The water is read where the projectile starts the tick, before it moves.
    const bool in_water =
        level != nullptr && box_in_water(*level, box_at(state.position, motion), targets.water);

    const Vec3d from = state.position;
    const Vec3d v    = state.velocity;
    const Vec3d to   = from + v;

    const std::optional<BlockClip> block = clip_blocks(world, from, to);
    const Vec3d                    end   = block ? block->point : to;

    // Entities: the nearest target whose box, grown by 0.3, the segment enters
    // before any block does.
    const ProjectileTarget* hit_target = nullptr;
    f64                     hit_t      = std::numeric_limits<f64>::infinity();
    if (!data.dealt_damage) {
        for (const ProjectileTarget& target : targets.targets) {
            if (target.ignored || target.network_id == state.network_id) {
                continue;
            }
            if (target.network_id == data.owner && data.owner != 0 && !data.left_owner) {
                continue;
            }
            if (const auto t = clip_box(target.box.inflated(kTargetInflate), from, end);
                t && *t < hit_t) {
                hit_t      = *t;
                hit_target = &target;
            }
        }
    }

    // Has it cleared its owner? Checked against the unmoved box, so a shot
    // does not count as having left before it has moved at all.
    if (!data.left_owner && data.owner != 0) {
        bool inside = false;
        for (const ProjectileTarget& target : targets.targets) {
            if (target.network_id == data.owner &&
                target.box.inflated(1.0).intersects(box_at(to, motion))) {
                inside = true;
            }
        }
        data.left_owner = !inside;
    }

    std::optional<ProjectileEvent> event;
    if (hit_target != nullptr) {
        const Vec3d point = from + (end - from) * hit_t;
        const bool  arrow = is_arrow_like(data.kind);
        event = ProjectileEvent{arrow ? ProjectileEvent::Kind::HitEntity
                                      : ProjectileEvent::Kind::Broke,
                                state.network_id, hit_target->network_id, hit_target->player,
                                point, v};
        event->from = from;  // ── brewing ──
        if (!arrow) {
            state.position = point;
            state.removed  = true;
            return event;
        }
        // The arrow stays where it met the target until the caller has
        // decided whether the hit landed: then it is removed or bounces.
        state.position = point;
        return event;
    }

    if (block) {
        if (!is_arrow_like(data.kind)) {
            event = ProjectileEvent{ProjectileEvent::Kind::Broke, state.network_id, 0, false,
                                    block->point, v};
            event->from = from;  // ── brewing ──
            state.position = block->point;
            state.removed  = true;
            return event;
        }
        // Stuck: backed off the face by `0.05F` along the way it came, so its
        // tip is in the block and its body out of it. Measured: an arrow
        // falling onto grass at -60 rests at -59.94999999925494, which is
        // -60 + 0.05F; into a wall at x = 10 on a slight fall, 9.950088916881048.
        //
        // The Motion it keeps is what it travelled on its last tick, then
        // dragged and pulled like any other tick: -1.0294762709270207 for the
        // fall, (1.1058299946022032, -0.11603773069454587) for the wall — both
        // reproduced to every digit by this.
        const Vec3d travelled = block->point - from;
        const f64   len       = travelled.length();
        const Vec3d back =
            len > 0.0 ? travelled * (static_cast<f64>(0.05F) / len) : Vec3d{};
        state.position = block->point - back;
        state.velocity = travelled * (in_water ? motion.water_drag : motion.air_drag);
        state.velocity.y -= motion.gravity;
        data.in_ground        = true;
        data.life             = 0;
        data.critical         = false;
        data.stuck            = block->block;
        if (level != nullptr) {
            data.stuck_state = level->block_at(block->block);
        }
        return ProjectileEvent{ProjectileEvent::Kind::Stuck, state.network_id, 0, false,
                               state.position, v};
    }

    // Nothing in the way: move by the old velocity, then drag, then gravity.
    state.position = to;
    const f64 drag = in_water ? motion.water_drag : motion.air_drag;
    state.velocity = v * drag;
    state.velocity.y -= motion.gravity;
    return std::nullopt;
}

void ProjectileLogic::tick(entity::EntityWorld& world, entity::EntityHandle self,
                           const entity::TickContext& context) {
    entity::EntityState* state = world.mutable_state(self);
    if (state == nullptr || state->removed) {
        return;
    }
    const MobContext* mob = mob_context(context);
    if (mob == nullptr || mob->world == nullptr) {
        return;
    }
    auto event = step_projectile(*state, data_, *mob->world, mob->level, *world_);
    if (event) {
        event->projectile_kind = data_.kind;
        event->owner           = data_.owner;
        event->owner_is_player = data_.owner_is_player;
        world_->events.events.push_back(*event);
        if (event->kind == ProjectileEvent::Kind::Expired) {
            state->removed = true;
        }
    }
}

void resolve_entity_hit(entity::EntityState& state, ProjectileData& data,
                        bool hurt_landed) noexcept {
    if (data.kind == ProjectileKind::Trident) {
        // A trident that hits keeps falling, slowed almost to a stop, and
        // never deals damage again.
        data.dealt_damage = true;
        state.velocity    = Vec3d{state.velocity.x * -0.01, state.velocity.y * -0.1,
                                  state.velocity.z * -0.01};
        return;
    }
    if (hurt_landed) {
        state.removed = true;
        return;
    }
    // Refused — the target's window was open, or it could not be hurt: the
    // arrow comes back a tenth as fast.
    state.velocity = state.velocity * -0.1;
}

// ── Damage ──────────────────────────────────────────────────────────────────

i32 arrow_damage(const Vec3d& velocity, f64 base_damage, bool critical,
                 math::LegacyRandomSource& random) noexcept {
    const f64 raw   = std::clamp(velocity.length() * base_damage, 0.0, 2.147483647e9);
    i32       total = static_cast<i32>(std::ceil(raw));
    if (critical) {
        const i32 bonus = random.next_int(total / 2 + 2);
        total           = static_cast<i32>(std::min<i64>(static_cast<i64>(total) + bonus,
                                                         std::numeric_limits<i32>::max()));
    }
    return total;
}

f32 snowball_damage(bool target_is_blaze) noexcept {
    return target_is_blaze ? 3.0F : 0.0F;
}

// ── Shooting ────────────────────────────────────────────────────────────────

Vec3d look_direction(f32 yaw, f32 pitch) noexcept {
    const f32 yaw_rad   = yaw * kDegreesToRadians;
    const f32 pitch_rad = pitch * kDegreesToRadians;
    const f32 x         = -table_sin(yaw_rad) * table_cos(pitch_rad);
    const f32 y         = -table_sin(pitch_rad);
    const f32 z         = table_cos(yaw_rad) * table_cos(pitch_rad);
    return Vec3d{static_cast<f64>(x), static_cast<f64>(y), static_cast<f64>(z)};
}

Vec3d shoot_velocity(const Vec3d& direction, f32 speed, f32 inaccuracy,
                     math::LegacyRandomSource& random) noexcept {
    const f64 length = direction.length();
    Vec3d     unit   = length > 0.0 ? direction * (1.0 / length) : Vec3d{};
    const f64 spread = 0.0172275 * static_cast<f64>(inaccuracy);
    // A triangular draw is two uniform draws subtracted. Named and in order:
    // C++ does not sequence the operands of `a - b` and Java does.
    const f64 ax = random.next_double();
    const f64 bx = random.next_double();
    const f64 ay = random.next_double();
    const f64 by = random.next_double();
    const f64 az = random.next_double();
    const f64 bz = random.next_double();
    unit.x += spread * (ax - bx);
    unit.y += spread * (ay - by);
    unit.z += spread * (az - bz);
    return unit * static_cast<f64>(speed);
}

f32 bow_power(i32 ticks) noexcept {
    f32 f = static_cast<f32>(ticks) / 20.0F;
    f     = (f * f + f * 2.0F) / 3.0F;
    return std::min(f, 1.0F);
}

i32 crossbow_charge_ticks(u8 quick_charge) noexcept {
    return std::max(0, 25 - 5 * static_cast<i32>(quick_charge));
}

f32 skeleton_inaccuracy(i32 difficulty) noexcept {
    return static_cast<f32>(14 - 4 * difficulty);
}

Vec3d skeleton_aim(const Vec3d& arrow_start, const Vec3d& target_feet, f64 target_height,
                   i32 difficulty, math::LegacyRandomSource& random) noexcept {
    const f64 dx = target_feet.x - arrow_start.x;
    const f64 dy = target_feet.y + target_height / 3.0 - arrow_start.y;
    const f64 dz = target_feet.z - arrow_start.z;
    const f64 h  = std::sqrt(dx * dx + dz * dz);
    return shoot_velocity(Vec3d{dx, dy + h * 0.2, dz}, kSkeletonArrowSpeed,
                          skeleton_inaccuracy(difficulty), random);
}

i32 egg_chickens(math::LegacyRandomSource& random) noexcept {
    if (random.next_int(8) != 0) {
        return 0;
    }
    return random.next_int(32) == 0 ? 4 : 1;
}

std::span<const std::string_view> projectile_gaps() noexcept {
    return kGaps;
}

}  // namespace ov::gameplay
