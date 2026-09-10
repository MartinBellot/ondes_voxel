#include "ov/gameplay/explosion.hpp"

#include "ov/math/raycast.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace ov::gameplay {
namespace {

/// The step, widened the way the game widens it.
///
/// The game's step is the float `0.3F` and it is added to a double coordinate,
/// so what is actually added is `0.30000001192092896`, not `0.3`. Over the
/// twenty-odd steps of a ray the difference is a ten-thousandth of a block —
/// which is nothing until a sample point sits on a block boundary, and then it
/// is a whole cell.
[[nodiscard]] constexpr f64 widened(f32 value) noexcept { return static_cast<f64>(value); }

/// Pack a position into one integer so the set of taken blocks can be sorted
/// and deduplicated without a hash table in the hot path.
[[nodiscard]] constexpr i64 packed(BlockPos pos) noexcept {
    // 21 bits of x and z, 22 of y: enough for any world the format allows, and
    // it keeps the ordering meaningful, which is what makes the result stable.
    return (static_cast<i64>(pos.x + (1 << 20)) << 42) |
           (static_cast<i64>(pos.z + (1 << 20)) << 21) |
           static_cast<i64>(pos.y + (1 << 20));
}

[[nodiscard]] constexpr BlockPos unpacked(i64 key) noexcept {
    return BlockPos{static_cast<i32>((key >> 42) & 0x1FFFFF) - (1 << 20),
                    static_cast<i32>(key & 0x1FFFFF) - (1 << 20),
                    static_cast<i32>((key >> 21) & 0x1FFFFF) - (1 << 20)};
}

/// Does the segment from `from` to `to` meet this box?
///
/// The slab test, clipped to the segment rather than to the whole ray: a box
/// behind the sample point or past the centre does not block anything.
[[nodiscard]] bool segment_hits(const AABB& box, const Vec3d& from, const Vec3d& delta) noexcept {
    f64 enter = 0.0;
    f64 exit  = 1.0;
    const std::array<f64, 3> origin{from.x, from.y, from.z};
    const std::array<f64, 3> step{delta.x, delta.y, delta.z};
    const std::array<f64, 3> low{box.min.x, box.min.y, box.min.z};
    const std::array<f64, 3> high{box.max.x, box.max.y, box.max.z};
    for (usize axis = 0; axis < 3; ++axis) {
        if (std::abs(step[axis]) < 1e-12) {
            if (origin[axis] < low[axis] || origin[axis] > high[axis]) {
                return false;
            }
            continue;
        }
        f64 t0 = (low[axis] - origin[axis]) / step[axis];
        f64 t1 = (high[axis] - origin[axis]) / step[axis];
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        enter = std::max(enter, t0);
        exit  = std::min(exit, t1);
        if (enter > exit) {
            return false;
        }
    }
    return true;
}

}  // namespace

Explosions::Explosions(const registry::BlockRegistry& blocks, ExplosionConstants constants) noexcept
    : blocks_{&blocks}, constants_{constants} {}

i32 Explosions::ray_count() const noexcept {
    const i32 n = constants_.grid;
    if (n < 2) {
        return 0;
    }
    return n * n * n - (n - 2) * (n - 2) * (n - 2);
}

f32 Explosions::resistance_of(registry::BlockStateId state) const noexcept {
    const registry::BlockId block = blocks_->block_of(state);
    f32                     value = blocks_->blast_resistance(block);
    if (blocks_->holds_fluid(state)) {
        value = std::max(value, constants_.fluid_resistance);
    }
    return value;
}

bool Explosions::resistance_measured(registry::BlockStateId state) const noexcept {
    if (blocks_->holds_fluid(state)) {
        return true;
    }
    return blocks_->blast_resistance(blocks_->block_of(state)) >= 0.0F;
}

void Explosions::collect_blocks(const world::LevelView& level, const ExplosionSpec& spec,
                                math::LegacyRandomSource& rng, std::vector<BlockPos>& out) const {
    if (spec.interaction == BlockInteraction::Keep) {
        return;
    }
    const i32 n = constants_.grid;
    if (n < 2) {
        return;
    }
    const auto last  = static_cast<f32>(n - 1);
    const auto shape = level.shape();

    // One key per cell a ray takes; sorted and deduplicated at the end. Vanilla
    // keeps a HashSet and then shuffles it, so its own order is not something
    // anything can reproduce — ours is sorted, which is reproducible, and the
    // set is identical either way.
    std::vector<i64> keys;
    keys.reserve(1024);

    for (i32 j = 0; j < n; ++j) {
        for (i32 k = 0; k < n; ++k) {
            for (i32 l = 0; l < n; ++l) {
                const bool outer = j == 0 || j == n - 1 || k == 0 || k == n - 1 || l == 0 ||
                                   l == n - 1;
                if (!outer) {
                    continue;
                }
                // Computed in float and then widened, as the game does. Doing
                // the whole thing in double moves the direction in the last
                // places and, at the rim, moves whole cells.
                f64 dir_x = static_cast<f64>(static_cast<f32>(j) / last * 2.0F - 1.0F);
                f64 dir_y = static_cast<f64>(static_cast<f32>(k) / last * 2.0F - 1.0F);
                f64 dir_z = static_cast<f64>(static_cast<f32>(l) / last * 2.0F - 1.0F);
                const f64 length = std::sqrt(dir_x * dir_x + dir_y * dir_y + dir_z * dir_z);
                dir_x /= length;
                dir_y /= length;
                dir_z /= length;

                // ⚠ Two draws in one expression would be at the compiler's
                //   mercy: C++ does not sequence the operands of an arithmetic
                //   operator, Java does. One named draw, in order.
                const f32 roll   = rng.next_float();
                f32       energy = spec.power * (constants_.energy_low +
                                           roll * constants_.energy_span);

                f64 x = spec.centre.x;
                f64 y = spec.centre.y;
                f64 z = spec.centre.z;
                for (; energy > 0.0F; energy -= constants_.step_cost) {
                    const BlockPos pos{floor_to_block(x), floor_to_block(y), floor_to_block(z)};
                    if (pos.y < shape.min_y || pos.y > shape.max_y()) {
                        break;
                    }
                    const registry::BlockStateId state = level.block_at(pos);
                    const registry::BlockId      block = blocks_->block_of(state);
                    // Air with no fluid in it is charged for nothing at all —
                    // not for zero resistance, for nothing. The difference is
                    // the 0.3 bias, which air would otherwise pay.
                    if (!blocks_->is_air(block) || blocks_->holds_fluid(state)) {
                        energy -= (resistance_of(state) + constants_.resistance_bias) *
                                  constants_.resistance_scale;
                    }
                    // Vanilla adds the position whatever is in it, air
                    // included, and filters air out when it finalises. The set
                    // of *destroyed* blocks is the same either way; the
                    // difference is that vanilla's list still carries the air
                    // cells, and that is where a fire-making charge puts its
                    // fire. Named here because the list this returns cannot be
                    // used for that.
                    if (energy > 0.0F && !blocks_->is_air(block) &&
                        blocks_->blast_resistance(block) >= 0.0F) {
                        keys.push_back(packed(pos));
                    }
                    x += dir_x * widened(constants_.step);
                    y += dir_y * widened(constants_.step);
                    z += dir_z * widened(constants_.step);
                }
            }
        }
    }

    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    out.reserve(out.size() + keys.size());
    for (const i64 key : keys) {
        out.push_back(unpacked(key));
    }
}

bool Explosions::can_drop_from_explosion(registry::BlockStateId state) const noexcept {
    const registry::BlockId block = blocks_->block_of(state);
    if (blocks_->is_air(block)) {
        return false;
    }
    // A block the pack never measured is refused rather than exploded: it would
    // otherwise be the most fragile block in the game.
    return blocks_->blast_resistance(block) >= 0.0F;
}

f32 Explosions::drop_chance(const ExplosionSpec& spec) const noexcept {
    if (spec.interaction != BlockInteraction::DestroyWithDecay || spec.power <= 0.0F) {
        return 1.0F;
    }
    return 1.0F / spec.power;
}

bool Explosions::rolls_drop(const ExplosionSpec& spec, math::LegacyRandomSource& rng) const {
    const f32 chance = drop_chance(spec);
    if (chance >= 1.0F) {
        return true;
    }
    return rng.next_float() < chance;
}

AABB Explosions::entity_search_box(const ExplosionSpec& spec) const noexcept {
    const f64 reach = static_cast<f64>(spec.power) *
                          static_cast<f64>(constants_.entity_radius_factor) +
                      1.0;
    return AABB{spec.centre - Vec3d{reach, reach, reach},
                spec.centre + Vec3d{reach, reach, reach}};
}

f64 Explosions::seen_percent(const world::LevelView& level, const Vec3d& centre,
                             const AABB& box) const {
    const f64 step_x = 1.0 / ((box.max.x - box.min.x) * 2.0 + 1.0);
    const f64 step_y = 1.0 / ((box.max.y - box.min.y) * 2.0 + 1.0);
    const f64 step_z = 1.0 / ((box.max.z - box.min.z) * 2.0 + 1.0);
    if (step_x < 0.0 || step_y < 0.0 || step_z < 0.0) {
        return 0.0;
    }
    // The half-cell the sample grid is nudged by on the two horizontal axes,
    // and only on those two. It is not symmetry for its own sake: without it
    // the grid's last column sits exactly on the box's far face.
    const f64 offset_x = (1.0 - std::floor(1.0 / step_x) * step_x) / 2.0;
    const f64 offset_z = (1.0 - std::floor(1.0 / step_z) * step_z) / 2.0;

    i32 clear = 0;
    i32 total = 0;
    // The loop counters are floats in the game and the accumulated error
    // decides how many samples there are. Keeping them float keeps the count.
    for (f32 fx = 0.0F; fx <= 1.0F; fx = static_cast<f32>(static_cast<f64>(fx) + step_x)) {
        for (f32 fy = 0.0F; fy <= 1.0F; fy = static_cast<f32>(static_cast<f64>(fy) + step_y)) {
            for (f32 fz = 0.0F; fz <= 1.0F; fz = static_cast<f32>(static_cast<f64>(fz) + step_z)) {
                const Vec3d from{
                    std::lerp(box.min.x, box.max.x, static_cast<f64>(fx)) + offset_x,
                    std::lerp(box.min.y, box.max.y, static_cast<f64>(fy)),
                    std::lerp(box.min.z, box.max.z, static_cast<f64>(fz)) + offset_z};
                ++total;
                if (!clipped(level, from, centre)) {
                    ++clear;
                }
            }
        }
    }
    return total == 0 ? 0.0 : static_cast<f64>(clear) / static_cast<f64>(total);
}

bool Explosions::clipped(const world::LevelView& level, const Vec3d& from,
                         const Vec3d& to) const {
    const Vec3d delta = to - from;
    const f64   length = delta.length();
    if (length <= 0.0) {
        return false;
    }
    return raycast_voxels(from, delta, length, [&](BlockPos pos) {
        const registry::BlockStateId state = level.block_at(pos);
        for (const auto& shape : blocks_->collision_boxes(state)) {
            const AABB world_box{
                Vec3d{static_cast<f64>(pos.x) + static_cast<f64>(shape.min_x) / 32.0,
                      static_cast<f64>(pos.y) + static_cast<f64>(shape.min_y) / 32.0,
                      static_cast<f64>(pos.z) + static_cast<f64>(shape.min_z) / 32.0},
                Vec3d{static_cast<f64>(pos.x) + static_cast<f64>(shape.max_x) / 32.0,
                      static_cast<f64>(pos.y) + static_cast<f64>(shape.max_y) / 32.0,
                      static_cast<f64>(pos.z) + static_cast<f64>(shape.max_z) / 32.0}};
            if (segment_hits(world_box, from, delta)) {
                return true;
            }
        }
        return false;
    }).has_value();
}

ExplosionHit Explosions::hit_entity(const world::LevelView& level, const ExplosionSpec& spec,
                                    const AABB& box, const Vec3d& position, f64 eye_y,
                                    f64 knockback_dampener) const {
    ExplosionHit hit{};
    const f64    reach = static_cast<f64>(spec.power) *
                      static_cast<f64>(constants_.entity_radius_factor);
    if (reach <= 0.0) {
        return hit;
    }
    const Vec3d to_feet = position - spec.centre;
    const f64   scaled  = to_feet.length() / reach;
    if (scaled > 1.0) {
        return hit;
    }
    // The direction is taken from the **eyes**, the distance from the feet.
    // Using one point for both is the mistake that makes a tall mob take its
    // damage sideways.
    f64       dx     = position.x - spec.centre.x;
    f64       dy     = eye_y - spec.centre.y;
    f64       dz     = position.z - spec.centre.z;
    const f64 length = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (length == 0.0) {
        return hit;
    }
    dx /= length;
    dy /= length;
    dz /= length;

    hit.exposure = seen_percent(level, spec.centre, box);
    hit.impact   = (1.0 - scaled) * hit.exposure;
    hit.damage   = static_cast<f32>(static_cast<i32>(
        (hit.impact * hit.impact + hit.impact) / 2.0 *
            static_cast<f64>(constants_.damage_scale) * 2.0 * static_cast<f64>(spec.power) +
        1.0));
    const f64 push = hit.impact * (1.0 - std::clamp(knockback_dampener, 0.0, 1.0));
    hit.impulse    = Vec3d{dx * push, dy * push, dz * push};
    hit.touched    = true;
    return hit;
}

bool Explosions::rolls_fire(const ExplosionSpec& spec, math::LegacyRandomSource& rng) const {
    if (!spec.fire) {
        return false;
    }
    return rng.next_int(3) == 0;
}

i32 Explosions::chained_fuse(i32 full_fuse, math::LegacyRandomSource& rng) const {
    if (full_fuse < 8) {
        return full_fuse;
    }
    // `random(fuse / 4) + fuse / 8`, in that order: the draw comes first in
    // the game and the sum is written the other way round, which is exactly the
    // shape that gets reversed by accident.
    const i32 spread = rng.next_int(full_fuse / 4);
    return spread + full_fuse / 8;
}

std::span<const std::pair<std::string_view, f32>> explosion_sources() noexcept {
    static constexpr std::array<std::pair<std::string_view, f32>, 8> kSources{{
        {"minecraft:tnt", kTntPower},
        {"minecraft:creeper", kCreeperPower},
        {"minecraft:charged_creeper", kChargedCreeperPower},
        {"minecraft:bed", kBedPower},
        {"minecraft:respawn_anchor", kRespawnAnchorPower},
        {"minecraft:end_crystal", kEndCrystalPower},
        {"minecraft:fireball", kGhastFireballPower},
        {"minecraft:wither_skull", kWitherSkullPower},
    }};
    return kSources;
}

}  // namespace ov::gameplay
