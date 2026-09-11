#include "ov/gameplay/sleep.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace ov::gameplay {

namespace {

/// Collision boxes in the registry are in 32nds of a block.
constexpr i32 kBoxUnits = 32;

[[nodiscard]] Direction clockwise(Direction d) noexcept {
    switch (d) {
    case Direction::North: return Direction::East;
    case Direction::East: return Direction::South;
    case Direction::South: return Direction::West;
    case Direction::West: return Direction::North;
    default: return d;
    }
}

struct Step {
    i32 x{0};
    i32 z{0};
};

[[nodiscard]] Step step_of(Direction d) noexcept {
    switch (d) {
    case Direction::North: return {0, -1};
    case Direction::South: return {0, 1};
    case Direction::West: return {-1, 0};
    case Direction::East: return {1, 0};
    default: return {0, 0};
    }
}

/// The yaw a horizontal direction faces: south 0, west 90, north 180, east 270.
[[nodiscard]] f32 yaw_of(Direction d) noexcept {
    switch (d) {
    case Direction::South: return 0.0F;
    case Direction::West: return 90.0F;
    case Direction::North: return 180.0F;
    case Direction::East: return 270.0F;
    default: return 0.0F;
    }
}

[[nodiscard]] f32 wrap_degrees(f32 degrees) noexcept {
    f32 wrapped = std::fmod(degrees, 360.0F);
    if (wrapped >= 180.0F) {
        wrapped -= 360.0F;
    }
    if (wrapped < -180.0F) {
        wrapped += 360.0F;
    }
    return wrapped;
}

[[nodiscard]] std::optional<Direction> direction_named(std::string_view name) noexcept {
    if (name == "north") return Direction::North;
    if (name == "south") return Direction::South;
    if (name == "east") return Direction::East;
    if (name == "west") return Direction::West;
    return std::nullopt;
}

/// Blocks whose full cube never suffocates, from the wiki's Suffocation page:
/// glass of every kind, leaves, ice, and a few see-through blocks.
[[nodiscard]] bool never_suffocates(std::string_view name) noexcept {
    return name.ends_with("glass") || name.ends_with("_leaves") || name == "minecraft:ice" ||
           name == "minecraft:frosted_ice" || name == "minecraft:glass_pane" ||
           name == "minecraft:barrier" || name == "minecraft:spawner" ||
           name.ends_with("_stained_glass") || name == "minecraft:tinted_glass";
}

}  // namespace

std::string_view bed_message_key(BedProblem problem) noexcept {
    switch (problem) {
    case BedProblem::NotPossibleNow: return "block.minecraft.bed.no_sleep";
    case BedProblem::TooFarAway: return "block.minecraft.bed.too_far_away";
    case BedProblem::Obstructed: return "block.minecraft.bed.obstructed";
    case BedProblem::NotSafe: return "block.minecraft.bed.not_safe";
    case BedProblem::Occupied: return "block.minecraft.bed.occupied";
    case BedProblem::None:
    case BedProblem::NotPossibleHere:
    case BedProblem::Other: return {};
    }
    return {};
}

SleepVerdict judge_sleep(const SleepCheck& check) noexcept {
    SleepVerdict verdict;
    if (!check.bed_works) {
        verdict.explodes = true;
        return verdict;
    }
    if (check.occupied) {
        verdict.problem = BedProblem::Occupied;
        return verdict;
    }
    if (check.sleeping_or_dead) {
        verdict.problem = BedProblem::Other;
        return verdict;
    }
    if (!check.natural) {
        verdict.problem = BedProblem::NotPossibleHere;
        return verdict;
    }
    if (!check.in_range) {
        verdict.problem = BedProblem::TooFarAway;
        return verdict;
    }
    if (check.obstructed) {
        verdict.problem = BedProblem::Obstructed;
        return verdict;
    }
    verdict.sets_spawn = true;
    if (check.is_day) {
        verdict.problem = BedProblem::NotPossibleNow;
        return verdict;
    }
    if (!check.creative && check.monsters_near) {
        verdict.problem = BedProblem::NotSafe;
    }
    return verdict;
}

i64 wake_day_time(i64 day_time) noexcept {
    const i64 next = day_time + 24000;
    // Java's `%` truncates, as C++'s does; a negative clock is not reached.
    return next - next % 24000;
}

i32 sleepers_needed(i32 active_players, i32 percentage) noexcept {
    const f32 wanted = static_cast<f32>(active_players * percentage) / 100.0F;
    const auto ceiled = static_cast<i32>(std::ceil(wanted));
    return std::max(1, ceiled);
}

bool prevents_rest(std::string_view type) noexcept {
    // The game's Monster family: every hostile that walks, swims or climbs.
    // Slimes, magma cubes, phantoms, ghasts and shulkers are enemies but not
    // of that family and do not keep anyone awake. Zombified piglins only when
    // angry, and piglins are left out with them — neither is modelled here.
    static constexpr std::array<std::string_view, 28> kMonsters{
        "minecraft:zombie",         "minecraft:husk",        "minecraft:drowned",
        "minecraft:zombie_villager", "minecraft:skeleton",   "minecraft:stray",
        "minecraft:wither_skeleton", "minecraft:creeper",    "minecraft:spider",
        "minecraft:cave_spider",    "minecraft:enderman",    "minecraft:witch",
        "minecraft:silverfish",     "minecraft:endermite",   "minecraft:blaze",
        "minecraft:guardian",       "minecraft:elder_guardian", "minecraft:vex",
        "minecraft:vindicator",     "minecraft:evoker",      "minecraft:pillager",
        "minecraft:ravager",        "minecraft:illusioner",  "minecraft:piglin_brute",
        "minecraft:warden",         "minecraft:zoglin",      "minecraft:giant",
        "minecraft:wither"};
    return std::ranges::find(kMonsters, type) != kMonsters.end();
}

// ── BedRules ────────────────────────────────────────────────────────────────

BedRules::BedRules(const registry::BlockRegistry& blocks) : blocks_{&blocks} {}

bool BedRules::is_bed(registry::BlockStateId state) const noexcept {
    return blocks_->block_name(blocks_->block_of(state)).ends_with("_bed");
}

std::optional<Bed> BedRules::bed_at(const world::LevelView& level, BlockPos clicked) const {
    const registry::BlockStateId state = level.block_at(clicked);
    if (!is_bed(state)) {
        return std::nullopt;
    }
    const registry::BlockId block    = blocks_->block_of(state);
    const auto              facing_p = blocks_->find_property(block, "facing");
    const auto              part_p   = blocks_->find_property(block, "part");
    const auto              occ_p    = blocks_->find_property(block, "occupied");
    if (!facing_p || !part_p || !occ_p) {
        return std::nullopt;
    }
    const auto facing = direction_named(blocks_->property_value(state, *facing_p));
    if (!facing) {
        return std::nullopt;
    }
    const bool head = blocks_->property_value(state, *part_p) == "head";
    Bed        bed;
    bed.facing   = *facing;
    bed.head     = head ? clicked : clicked.offset(*facing);
    bed.foot     = head ? clicked.offset(opposite(*facing)) : clicked;
    const BlockPos               other_pos = head ? bed.foot : bed.head;
    const registry::BlockStateId other     = level.block_at(other_pos);
    if (blocks_->block_of(other) != block) {
        return std::nullopt;
    }
    // The head carries the flag; the foot copies it through its shape update.
    const registry::BlockStateId head_state = head ? state : other;
    bed.occupied = blocks_->property_value(head_state, *occ_p) == "true";
    return bed;
}

bool BedRules::in_range(const Vec3d& feet, const Bed& bed) noexcept {
    const auto reachable = [&](BlockPos half) {
        const f64 cx = static_cast<f64>(half.x) + 0.5;
        const f64 cy = static_cast<f64>(half.y);
        const f64 cz = static_cast<f64>(half.z) + 0.5;
        return std::abs(feet.x - cx) <= 3.0 && std::abs(feet.y - cy) <= 2.0 &&
               std::abs(feet.z - cz) <= 3.0;
    };
    return reachable(bed.head) || reachable(bed.foot);
}

bool BedRules::suffocates(registry::BlockStateId state) const {
    const registry::BlockId block = blocks_->block_of(state);
    if (!blocks_->blocks_motion(block) || never_suffocates(blocks_->block_name(block))) {
        return false;
    }
    const auto boxes = blocks_->collision_boxes(state);
    if (boxes.size() != 1) {
        return false;
    }
    // Registry boxes are in 32nds of a block.
    const auto& box = boxes[0];
    return box.min_x <= 0 && box.min_y <= 0 && box.min_z <= 0 && box.max_x >= kBoxUnits &&
           box.max_y >= kBoxUnits && box.max_z >= kBoxUnits;
}

bool BedRules::obstructed(const world::LevelView& level, const Bed& bed) const {
    const BlockPos above_head{bed.head.x, bed.head.y + 1, bed.head.z};
    const BlockPos above_foot{bed.foot.x, bed.foot.y + 1, bed.foot.z};
    return suffocates(level.block_at(above_head)) || suffocates(level.block_at(above_foot));
}

AABB BedRules::monster_box(const Bed& bed) noexcept {
    const f64 cx = static_cast<f64>(bed.head.x) + 0.5;
    const f64 cy = static_cast<f64>(bed.head.y);
    const f64 cz = static_cast<f64>(bed.head.z) + 0.5;
    return AABB{Vec3d{cx - 8.0, cy - 5.0, cz - 8.0}, Vec3d{cx + 8.0, cy + 5.0, cz + 8.0}};
}

Vec3d BedRules::sleeping_position(const Bed& bed) noexcept {
    return Vec3d{static_cast<f64>(bed.head.x) + 0.5, static_cast<f64>(bed.head.y) + 0.6875,
                 static_cast<f64>(bed.head.z) + 0.5};
}

registry::BlockStateId BedRules::with_occupied(registry::BlockStateId state, bool occupied) const {
    const auto property = blocks_->find_property(blocks_->block_of(state), "occupied");
    if (!property) {
        return state;
    }
    for (usize i = 0; i < property->values.size(); ++i) {
        if (property->values[i] == (occupied ? "true" : "false")) {
            return blocks_->with_property(state, *property, static_cast<u16>(i));
        }
    }
    return state;
}

bool BedRules::passable(registry::BlockStateId state) const {
    return blocks_->collision_boxes(state).empty();
}

std::optional<f64> BedRules::floor_height(const world::LevelView& level, BlockPos cell) const {
    // The top of what is in the cell, or — for an empty cell — the top of the
    // block below, minus one. A floor must be under one block high to stand on.
    const auto top_of = [&](BlockPos pos) -> std::optional<f64> {
        const auto boxes = blocks_->collision_boxes(level.block_at(pos));
        if (boxes.empty()) {
            return std::nullopt;
        }
        f64 top = 0.0;
        for (const auto& box : boxes) {
            top = std::max(top, static_cast<f64>(box.max_y) / kBoxUnits);
        }
        return top;
    };
    if (const auto here = top_of(cell)) {
        return *here < 1.0 ? here : std::nullopt;
    }
    if (const auto below = top_of(BlockPos{cell.x, cell.y - 1, cell.z})) {
        const f64 height = *below - 1.0;
        return height < 1.0 && height > -1.0 ? std::optional<f64>{height} : std::nullopt;
    }
    return std::nullopt;
}

bool BedRules::fits(const world::LevelView& level, const Vec3d& feet) const {
    // The player's box, 0.6 by 1.8, against every cell it overlaps.
    const AABB box = AABB::from_entity(feet, 0.6, 1.8);
    for (i32 y = aabb_min_block(box.min.y); y <= aabb_max_block(box.max.y); ++y) {
        for (i32 z = aabb_min_block(box.min.z); z <= aabb_max_block(box.max.z); ++z) {
            for (i32 x = aabb_min_block(box.min.x); x <= aabb_max_block(box.max.x);
                 ++x) {
                const BlockPos cell{x, y, z};
                for (const auto& part : blocks_->collision_boxes(level.block_at(cell))) {
                    const auto at = [](i32 cell_coord, i8 units) {
                        return static_cast<f64>(cell_coord) + static_cast<f64>(units) / kBoxUnits;
                    };
                    const AABB solid{
                        Vec3d{at(x, part.min_x), at(y, part.min_y), at(z, part.min_z)},
                        Vec3d{at(x, part.max_x), at(y, part.max_y), at(z, part.max_z)}};
                    if (solid.intersects(box)) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

std::optional<Vec3d> BedRules::stand_up_position(const world::LevelView& level, const Bed& bed,
                                                 f32 yaw) const {
    const Direction side = clockwise(bed.facing);
    // Facing the clockwise side means standing up on the other one.
    const f32  diff   = std::abs(wrap_degrees(yaw - yaw_of(side)));
    const Step d      = step_of(bed.facing);
    const Step s      = step_of(diff < 90.0F ? opposite(side) : side);
    const std::array<Step, 12> offsets{{{s.x, s.z},
                                        {s.x - d.x, s.z - d.z},
                                        {s.x - d.x * 2, s.z - d.z * 2},
                                        {-d.x * 2, -d.z * 2},
                                        {-s.x - d.x * 2, -s.z - d.z * 2},
                                        {-s.x - d.x, -s.z - d.z},
                                        {-s.x, -s.z},
                                        {-s.x + d.x, -s.z + d.z},
                                        {d.x, d.z},
                                        {s.x + d.x, s.z + d.z},
                                        {0, 0},
                                        {-d.x, -d.z}}};
    for (const Step& o : offsets) {
        const BlockPos cell{bed.head.x + o.x, bed.head.y, bed.head.z + o.z};
        const auto     floor = floor_height(level, cell);
        if (!floor) {
            continue;
        }
        const Vec3d feet{static_cast<f64>(cell.x) + 0.5, static_cast<f64>(cell.y) + *floor,
                         static_cast<f64>(cell.z) + 0.5};
        if (fits(level, feet)) {
            return feet;
        }
    }
    return std::nullopt;
}

f32 BedRules::stand_up_yaw(const Bed& bed, const Vec3d& stand) noexcept {
    const f64 dx     = static_cast<f64>(bed.head.x) + 0.5 - stand.x;
    const f64 dz     = static_cast<f64>(bed.head.z) + 0.5 - stand.z;
    const f64 length = std::sqrt(dx * dx + dz * dz);
    if (length == 0.0) {
        return 0.0F;
    }
    const f64 degrees = std::atan2(dz / length, dx / length) * 180.0 / std::numbers::pi - 90.0;
    return wrap_degrees(static_cast<f32>(degrees));
}

}  // namespace ov::gameplay
