#include "ov/gameplay/pathfinding.hpp"

#include <algorithm>
#include <cmath>

namespace ov::gameplay {
namespace {

/// A key for one block position, collision-free over the whole world.
///
/// 26 bits each for x and z covers ±33 million blocks, which is well past the
/// world border; 12 bits of y covers -2048..2047, well past the build height.
/// Packing rather than hashing three ints because the table below compares
/// keys, and two positions that share a hash but not a key would make a mob
/// path through a wall exactly once in a very long while.
[[nodiscard]] u64 pack(BlockPos pos) noexcept {
    const u64 x = static_cast<u64>(pos.x + (1 << 25)) & 0x3FFFFFFULL;
    const u64 z = static_cast<u64>(pos.z + (1 << 25)) & 0x3FFFFFFULL;
    const u64 y = static_cast<u64>(pos.y + 2048) & 0xFFFULL;
    return (x << 38) | (z << 12) | y;
}

/// Splitmix-style finaliser. The key is dense in the low bits — neighbouring
/// blocks differ by one — and linear probing on dense keys degenerates into a
/// linear scan.
[[nodiscard]] u64 scatter(u64 key) noexcept {
    key ^= key >> 33;
    key *= 0xFF51AFD7ED558CCDULL;
    key ^= key >> 33;
    key *= 0xC4CEB9FE1A85EC53ULL;
    key ^= key >> 33;
    return key;
}

[[nodiscard]] f32 distance(BlockPos a, BlockPos b) noexcept {
    const f32 dx = static_cast<f32>(a.x - b.x);
    const f32 dy = static_cast<f32>(a.y - b.y);
    const f32 dz = static_cast<f32>(a.z - b.z);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// ── Reading blocks ──────────────────────────────────────────────────────────
//
// Everything below asks the block registry rather than a table written here.
// The registry's answers were measured on a real 1.20.1 server (see
// registry/block_states.hpp), so "does this stop movement" is the game's
// answer and not a guess about what sounds solid.

[[nodiscard]] bool is_air_at(const world::LevelView& level, BlockPos pos) {
    const registry::BlockRegistry& blocks = level.blocks();
    return blocks.is_air(blocks.block_of(level.block_at(pos)));
}

[[nodiscard]] bool holds_water(const world::LevelView& level, BlockPos pos) {
    const registry::BlockRegistry& blocks = level.blocks();
    const registry::BlockStateId   state  = level.block_at(pos);
    if (!blocks.holds_fluid(state)) {
        return false;
    }
    const std::string_view name = blocks.block_name(blocks.block_of(state));
    return name != "minecraft:lava";
}

[[nodiscard]] bool is_lava(const world::LevelView& level, BlockPos pos) {
    const registry::BlockRegistry& blocks = level.blocks();
    return blocks.block_name(blocks.block_of(level.block_at(pos))) == "minecraft:lava";
}

/// Does this block's collision shape leave the cube upward?
///
/// This is how a fence is recognised without a hard-coded list: a fence, a wall
/// and a shut fence gate all collide up to y = 1.5, and nothing a mob may walk
/// through does. The boxes come from the registry in units of a thirty-second
/// of a block, so the cube's top is 32 and anything above it is a fence.
[[nodiscard]] bool is_fence_like(const registry::BlockRegistry& blocks,
                                 registry::BlockStateId         state) {
    for (const registry::BlockRegistry::Box& box : blocks.collision_boxes(state)) {
        if (box.max_y > 32) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool has_collision(const registry::BlockRegistry& blocks,
                                 registry::BlockStateId         state) {
    return !blocks.collision_boxes(state).empty();
}

/// A floor a mob can stand on: something that stops motion, at all.
[[nodiscard]] bool is_floor(const world::LevelView& level, BlockPos pos) {
    const registry::BlockRegistry& blocks = level.blocks();
    const registry::BlockStateId   state  = level.block_at(pos);
    const registry::BlockId        block  = blocks.block_of(state);
    if (!blocks.motion_measured(block)) {
        // Refused by name rather than assumed solid or assumed air. Seven
        // blocks in the game are in this state — the body segments of vertical
        // plants — and none of them is ever the top of a column, so a mob
        // standing on one is a bug in the caller.
        return false;
    }
    return blocks.blocks_motion(block) && has_collision(blocks, state);
}

/// A named list, and it is a list on purpose.
///
/// These are the blocks that hurt a body standing in them. Recognised by
/// registry name because nothing measured about a block says "this is painful"
/// — hardness does not, the collision shape does not, and light emission does
/// not. Naming them is honest; deriving them from something that happens to
/// correlate would not be.
[[nodiscard]] bool is_painful(std::string_view name) {
    return name == "minecraft:fire" || name == "minecraft:soul_fire" ||
           name == "minecraft:lava" || name == "minecraft:cactus" ||
           name == "minecraft:magma_block" || name == "minecraft:campfire" ||
           name == "minecraft:soul_campfire" || name == "minecraft:sweet_berry_bush" ||
           name == "minecraft:wither_rose" || name == "minecraft:powder_snow";
}

[[nodiscard]] bool is_sticky(std::string_view name) {
    return name == "minecraft:honey_block" || name == "minecraft:soul_sand" ||
           name == "minecraft:slime_block" || name == "minecraft:cobweb";
}

[[nodiscard]] bool ends_with(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

/// Whether a door-shaped block is standing open, from its own `open` property.
[[nodiscard]] bool door_is_open(const registry::BlockRegistry& blocks,
                                registry::BlockStateId         state) {
    const registry::BlockId block = blocks.block_of(state);
    const auto              open  = blocks.find_property(block, "open");
    if (!open) {
        return false;
    }
    return blocks.property_value(state, *open) == "true";
}

/// What one block *is*, ignoring everything around it.
///
/// The neighbourhood — is there a floor, is there headroom — is the caller's
/// job, because it depends on the mob's size and this does not.
[[nodiscard]] PathNodeType classify_block(const world::LevelView& level, BlockPos pos,
                                          const PathAbilities& abilities) {
    const registry::BlockRegistry& blocks = level.blocks();
    const registry::BlockStateId   state  = level.block_at(pos);
    const registry::BlockId        block  = blocks.block_of(state);
    const std::string_view         name   = blocks.block_name(block);

    if (blocks.is_air(block)) {
        return PathNodeType::Open;
    }
    if (is_lava(level, pos)) {
        return PathNodeType::Lava;
    }
    if (holds_water(level, pos)) {
        return abilities.enters_water ? PathNodeType::Water : PathNodeType::Blocked;
    }
    if (is_painful(name)) {
        return PathNodeType::DamageFire;
    }
    if (ends_with(name, "_door")) {
        if (door_is_open(blocks, state)) {
            return PathNodeType::DoorOpen;
        }
        if (name == "minecraft:iron_door") {
            return PathNodeType::DoorIronClosed;
        }
        return abilities.opens_doors ? PathNodeType::DoorWoodClosed
                                     : PathNodeType::DoorIronClosed;
    }
    if (is_fence_like(blocks, state)) {
        return PathNodeType::Fence;
    }
    if (ends_with(name, "_rail")) {
        return PathNodeType::Rail;
    }
    if (name == "minecraft:rail") {
        return PathNodeType::Rail;
    }
    if (ends_with(name, "_pressure_plate") || ends_with(name, "_trapdoor") ||
        name == "minecraft:tripwire") {
        return PathNodeType::Trapdoor;
    }
    if (is_sticky(name)) {
        return PathNodeType::StickyHoney;
    }
    if (blocks.is_leaves(block)) {
        return PathNodeType::Leaves;
    }
    if (has_collision(blocks, state)) {
        return PathNodeType::Blocked;
    }
    // No collision, not a fluid, not on any list: grass, a flower, a torch, a
    // sign. A body passes through these.
    return PathNodeType::Open;
}

/// Is a mob of this size next to something that hurts?
[[nodiscard]] bool near_danger(const world::LevelView& level, BlockPos pos, MobSize size) {
    const registry::BlockRegistry& blocks = level.blocks();
    for (i32 dx = -1; dx <= size.width; ++dx) {
        for (i32 dz = -1; dz <= size.width; ++dz) {
            for (i32 dy = 0; dy < size.height; ++dy) {
                if (dx >= 0 && dx < size.width && dz >= 0 && dz < size.width) {
                    continue;  // the body's own column, already classified
                }
                const BlockPos probe{pos.x + dx, pos.y + dy, pos.z + dz};
                const registry::BlockStateId state = level.block_at(probe);
                const std::string_view name = blocks.block_name(blocks.block_of(state));
                if (is_painful(name)) {
                    return true;
                }
            }
        }
    }
    return false;
}

}  // namespace

std::string_view to_string(PathNodeType type) noexcept {
    switch (type) {
        case PathNodeType::Blocked: return "blocked";
        case PathNodeType::Open: return "open";
        case PathNodeType::Walkable: return "walkable";
        case PathNodeType::Fence: return "fence";
        case PathNodeType::Water: return "water";
        case PathNodeType::WaterBorder: return "water_border";
        case PathNodeType::Lava: return "lava";
        case PathNodeType::DoorOpen: return "door_open";
        case PathNodeType::DoorWoodClosed: return "door_wood_closed";
        case PathNodeType::DoorIronClosed: return "door_iron_closed";
        case PathNodeType::DamageFire: return "damage_fire";
        case PathNodeType::DangerFire: return "danger_fire";
        case PathNodeType::Rail: return "rail";
        case PathNodeType::Leaves: return "leaves";
        case PathNodeType::Trapdoor: return "trapdoor";
        case PathNodeType::StickyHoney: return "sticky_honey";
    }
    return "?";
}

f32 path_malus(PathNodeType type) noexcept {
    switch (type) {
        // Impassable. A negative malus is the single statement of that, so
        // nothing can disagree with a separate `is_passable` table.
        case PathNodeType::Blocked:
        case PathNodeType::Fence:
        case PathNodeType::Lava:
        case PathNodeType::DoorIronClosed: return -1.0F;

        case PathNodeType::Walkable:
        case PathNodeType::Open: return 0.0F;

        // Free enough not to cause a detour, expensive enough to break a tie.
        case PathNodeType::Rail:
        case PathNodeType::DoorOpen: return 0.0F;

        // Worth a short detour: stepping on one is an event, and honey costs
        // real speed.
        case PathNodeType::Trapdoor: return 0.5F;
        case PathNodeType::StickyHoney: return 8.0F;
        case PathNodeType::Leaves: return 0.0F;

        // Water. The number that carries the maze oracle's ordering result:
        // a zombie offered four blocks of water against a dry detour takes the
        // detour while it is short and the water once it is long, and 8 puts
        // the crossover in the measured window. See docs/provenance/mobs.md.
        case PathNodeType::Water: return 8.0F;
        case PathNodeType::WaterBorder: return 4.0F;

        // Standing in fire against standing next to it. The second is cheaper
        // so a route skirts a fire rather than hugging it, which is visibly
        // what vanilla mobs do.
        case PathNodeType::DamageFire: return 16.0F;
        case PathNodeType::DangerFire: return 8.0F;

        // A shut wooden door: passable to something that opens doors, and dear
        // enough that it goes round if there is a way round.
        case PathNodeType::DoorWoodClosed: return 4.0F;
    }
    return -1.0F;
}

MobSize MobSize::from_box(f32 box_width, f32 box_height) noexcept {
    const i32 w = static_cast<i32>(std::ceil(static_cast<f64>(box_width)));
    const i32 h = static_cast<i32>(std::ceil(static_cast<f64>(box_height)));
    return MobSize{w < 1 ? 1 : w, h < 1 ? 1 : h};
}

BlockPos NodeEvaluator::start_node(const world::LevelView&, BlockPos feet, MobSize,
                                   const PathAbilities&) const {
    return feet;
}

// ── WalkNodeEvaluator ───────────────────────────────────────────────────────

PathNodeType WalkNodeEvaluator::type_at(const world::LevelView& level, BlockPos pos, MobSize size,
                                        const PathAbilities& abilities) const {
    // Every block the body would occupy. The worst answer wins: a mob standing
    // half in a wall is standing in a wall.
    PathNodeType worst = PathNodeType::Open;
    bool         any_water = false;
    for (i32 dx = 0; dx < size.width; ++dx) {
        for (i32 dz = 0; dz < size.width; ++dz) {
            for (i32 dy = 0; dy < size.height; ++dy) {
                const PathNodeType here =
                    classify_block(level, {pos.x + dx, pos.y + dy, pos.z + dz}, abilities);
                if (here == PathNodeType::Water) {
                    any_water = true;
                    continue;
                }
                if (here == PathNodeType::Open) {
                    continue;
                }
                if (!is_passable(here)) {
                    return here;  // one blocked cell settles it
                }
                if (path_malus(here) > path_malus(worst)) {
                    worst = here;
                }
            }
        }
    }
    if (any_water && worst == PathNodeType::Open) {
        worst = PathNodeType::Water;
    }

    // A floor under every column of the body. Without it the position is Open:
    // somewhere a flyer or a faller may be, and a walker may not stand.
    bool floored = false;
    for (i32 dx = 0; dx < size.width && !floored; ++dx) {
        for (i32 dz = 0; dz < size.width && !floored; ++dz) {
            floored = is_floor(level, {pos.x + dx, pos.y - 1, pos.z + dz});
        }
    }
    if (!floored) {
        return worst == PathNodeType::Open ? PathNodeType::Open : worst;
    }
    if (worst == PathNodeType::Open) {
        return near_danger(level, pos, size) ? PathNodeType::DangerFire : PathNodeType::Walkable;
    }
    return worst;
}

usize WalkNodeEvaluator::neighbours(const world::LevelView& level, BlockPos from, MobSize size,
                                    const PathAbilities& abilities,
                                    std::span<PathStep>  out) const {
    static constexpr i32 kDx[4] = {0, 0, -1, 1};
    static constexpr i32 kDz[4] = {-1, 1, 0, 0};

    usize count = 0;

    // Is a cardinal direction usable at all, and at which height? Recorded so
    // the diagonals below can require both of their components.
    bool open_cardinal[4] = {false, false, false, false};

    for (i32 dir = 0; dir < 4; ++dir) {
        const i32 nx = from.x + kDx[dir];
        const i32 nz = from.z + kDz[dir];

        // Same level, then a step up, then a fall. The order is the priority:
        // walking is preferred to jumping is preferred to dropping, which is
        // what keeps a mob on a flat floor instead of hopping down a staircase
        // it did not need to take.
        bool placed = false;
        for (i32 dy = 0; dy <= abilities.step_up && !placed; ++dy) {
            const BlockPos     candidate{nx, from.y + dy, nz};
            const PathNodeType type = type_at(level, candidate, size, abilities);
            if (type != PathNodeType::Walkable && !is_passable(type)) {
                break;  // solid: a higher step is behind the same wall
            }
            if (type == PathNodeType::Open) {
                continue;  // nothing to stand on here; try higher
            }
            if (dy > 0) {
                // Jumping needs the ceiling above the *current* position to be
                // out of the way, or the mob bangs its head and stays put.
                bool headroom = true;
                for (i32 hx = 0; hx < size.width && headroom; ++hx) {
                    for (i32 hz = 0; hz < size.width && headroom; ++hz) {
                        const BlockPos over{from.x + hx, from.y + size.height + dy - 1,
                                            from.z + hz};
                        headroom = is_passable(classify_block(level, over, abilities));
                    }
                }
                if (!headroom) {
                    break;
                }
            }
            out[count++] = PathStep{candidate, type};
            open_cardinal[dir] = true;
            placed             = true;
        }

        if (placed) {
            continue;
        }

        // Nothing to stand on level or above: look down for a landing.
        for (i32 drop = 1; drop <= abilities.max_fall; ++drop) {
            const BlockPos     candidate{nx, from.y - drop, nz};
            const PathNodeType type = type_at(level, candidate, size, abilities);
            if (type == PathNodeType::Open) {
                continue;
            }
            if (!is_passable(type)) {
                break;
            }
            out[count++]       = PathStep{candidate, type};
            open_cardinal[dir] = true;
            break;
        }
    }

    // Diagonals, at the same height only, and only when both of the cardinals
    // that bracket them are open. Cutting a corner past a block is how a mob
    // ends up clipped into a wall — vanilla refuses it, and so does this.
    static constexpr i32 kPairs[4][2] = {{0, 2}, {0, 3}, {1, 2}, {1, 3}};
    for (const auto& pair : kPairs) {
        if (!open_cardinal[pair[0]] || !open_cardinal[pair[1]]) {
            continue;
        }
        const BlockPos candidate{from.x + kDx[pair[0]] + kDx[pair[1]], from.y,
                                 from.z + kDz[pair[0]] + kDz[pair[1]]};
        const PathNodeType type = type_at(level, candidate, size, abilities);
        if (type == PathNodeType::Open || !is_passable(type)) {
            continue;
        }
        out[count++] = PathStep{candidate, type};
    }
    return count;
}

// ── SwimNodeEvaluator ───────────────────────────────────────────────────────

PathNodeType SwimNodeEvaluator::type_at(const world::LevelView& level, BlockPos pos, MobSize size,
                                        const PathAbilities& abilities) const {
    bool all_water = true;
    for (i32 dx = 0; dx < size.width; ++dx) {
        for (i32 dz = 0; dz < size.width; ++dz) {
            for (i32 dy = 0; dy < size.height; ++dy) {
                const BlockPos cell{pos.x + dx, pos.y + dy, pos.z + dz};
                if (holds_water(level, cell)) {
                    continue;
                }
                all_water = false;
                const PathNodeType here = classify_block(level, cell, abilities);
                if (here != PathNodeType::Open) {
                    return PathNodeType::Blocked;
                }
            }
        }
    }
    if (all_water) {
        // At the surface? A swimmer may go there; leaving it is another matter.
        return holds_water(level, {pos.x, pos.y + size.height, pos.z})
                   ? PathNodeType::Water
                   : PathNodeType::WaterBorder;
    }
    return allow_breaching_ ? PathNodeType::Open : PathNodeType::Blocked;
}

usize SwimNodeEvaluator::neighbours(const world::LevelView& level, BlockPos from, MobSize size,
                                   const PathAbilities& abilities,
                                   std::span<PathStep>  out) const {
    static constexpr i32 kDx[6] = {0, 0, -1, 1, 0, 0};
    static constexpr i32 kDy[6] = {0, 0, 0, 0, -1, 1};
    static constexpr i32 kDz[6] = {-1, 1, 0, 0, 0, 0};
    usize count = 0;
    for (i32 dir = 0; dir < 6; ++dir) {
        const BlockPos     candidate{from.x + kDx[dir], from.y + kDy[dir], from.z + kDz[dir]};
        const PathNodeType type = type_at(level, candidate, size, abilities);
        if (!is_passable(type)) {
            continue;
        }
        if (type == PathNodeType::Open && !allow_breaching_) {
            continue;
        }
        out[count++] = PathStep{candidate, type};
    }
    return count;
}

BlockPos SwimNodeEvaluator::start_node(const world::LevelView& level, BlockPos feet, MobSize size,
                                       const PathAbilities& abilities) const {
    // A fish resting on the sea floor starts from the water, not from the sand
    // its feet are in. Climbing at most the mob's own height, so a swimmer
    // stranded on land does not teleport its search into the sky.
    BlockPos pos = feet;
    for (i32 step = 0; step <= size.height; ++step) {
        if (is_passable(type_at(level, pos, size, abilities))) {
            return pos;
        }
        pos = pos.above();
    }
    return feet;
}

// ── FlyNodeEvaluator ────────────────────────────────────────────────────────

PathNodeType FlyNodeEvaluator::type_at(const world::LevelView& level, BlockPos pos, MobSize size,
                                       const PathAbilities& abilities) const {
    for (i32 dx = 0; dx < size.width; ++dx) {
        for (i32 dz = 0; dz < size.width; ++dz) {
            for (i32 dy = 0; dy < size.height; ++dy) {
                const BlockPos     cell{pos.x + dx, pos.y + dy, pos.z + dz};
                const PathNodeType here = classify_block(level, cell, abilities);
                if (here == PathNodeType::Open) {
                    continue;
                }
                // Water is a wall to a flyer, and so is everything solid. The
                // only thing that is neither is empty air.
                return here == PathNodeType::Water ? PathNodeType::Blocked : here;
            }
        }
    }
    return near_danger(level, pos, size) ? PathNodeType::DangerFire : PathNodeType::Open;
}

usize FlyNodeEvaluator::neighbours(const world::LevelView& level, BlockPos from, MobSize size,
                                  const PathAbilities& abilities, std::span<PathStep> out) const {
    usize count = 0;
    for (i32 dy = -1; dy <= 1; ++dy) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            for (i32 dz = -1; dz <= 1; ++dz) {
                if (dx == 0 && dy == 0 && dz == 0) {
                    continue;
                }
                // Cardinals, verticals, and the eight horizontal diagonals —
                // but not the corner diagonals of the cube, which would let a
                // flyer thread a gap of exactly nothing.
                if (dx != 0 && dz != 0 && dy != 0) {
                    continue;
                }
                if (count >= NodeEvaluator::kMaxNeighbours) {
                    return count;
                }
                const BlockPos     candidate{from.x + dx, from.y + dy, from.z + dz};
                const PathNodeType type = type_at(level, candidate, size, abilities);
                if (!is_passable(type)) {
                    continue;
                }
                out[count++] = PathStep{candidate, type};
            }
        }
    }
    return count;
}

// ── PathFinder ──────────────────────────────────────────────────────────────

namespace {
[[nodiscard]] usize next_power_of_two(usize value) noexcept {
    usize result = 1;
    while (result < value) {
        result <<= 1U;
    }
    return result;
}
}  // namespace

PathFinder::PathFinder(usize capacity) : capacity_{capacity} {
    nodes_.reserve(capacity);
    heap_.reserve(capacity);
    // Twice the node count, rounded up to a power of two: a linear-probing
    // table above about a 0.7 load factor spends its time probing.
    table_.assign(next_power_of_two(capacity * 2), Slot{});
}

i32 PathFinder::lookup(u64 key) const noexcept {
    const usize mask = table_.size() - 1;
    usize       slot = static_cast<usize>(scatter(key)) & mask;
    for (usize probe = 0; probe < table_.size(); ++probe) {
        const Slot& entry = table_[slot];
        if (entry.generation != generation_) {
            return -1;
        }
        if (entry.key == key) {
            return entry.node;
        }
        slot = (slot + 1) & mask;
    }
    return -1;
}

void PathFinder::insert(u64 key, i32 node) noexcept {
    const usize mask = table_.size() - 1;
    usize       slot = static_cast<usize>(scatter(key)) & mask;
    for (usize probe = 0; probe < table_.size(); ++probe) {
        Slot& entry = table_[slot];
        if (entry.generation != generation_ || entry.key == key) {
            entry.key        = key;
            entry.generation = generation_;
            entry.node       = node;
            return;
        }
        slot = (slot + 1) & mask;
    }
}

void PathFinder::push(i32 node) {
    heap_.push_back(node);
    usize child = heap_.size() - 1;
    while (child > 0) {
        const usize parent = (child - 1) / 2;
        if (nodes_[static_cast<usize>(heap_[parent])].f <=
            nodes_[static_cast<usize>(heap_[child])].f) {
            break;
        }
        std::swap(heap_[parent], heap_[child]);
        child = parent;
    }
}

i32 PathFinder::pop() {
    const i32 top = heap_.front();
    heap_.front() = heap_.back();
    heap_.pop_back();
    usize parent = 0;
    while (true) {
        const usize left  = parent * 2 + 1;
        const usize right = left + 1;
        usize       best  = parent;
        if (left < heap_.size() && nodes_[static_cast<usize>(heap_[left])].f <
                                       nodes_[static_cast<usize>(heap_[best])].f) {
            best = left;
        }
        if (right < heap_.size() && nodes_[static_cast<usize>(heap_[right])].f <
                                        nodes_[static_cast<usize>(heap_[best])].f) {
            best = right;
        }
        if (best == parent) {
            break;
        }
        std::swap(heap_[parent], heap_[best]);
        parent = best;
    }
    return top;
}

bool PathFinder::find(const NodeEvaluator& evaluator, const world::LevelView& level, BlockPos from,
                      BlockPos to, MobSize size, const PathAbilities& abilities, f32 max_range,
                      Path& out) {
    out.clear();
    nodes_.clear();
    heap_.clear();
    ++generation_;
    visited_ = 0;

    const BlockPos start = evaluator.start_node(level, from, size, abilities);

    Node first;
    first.pos    = start;
    first.g      = 0.0F;
    first.f      = distance(start, to);
    first.parent = -1;
    first.type   = evaluator.type_at(level, start, size, abilities);
    nodes_.push_back(first);
    insert(pack(start), 0);
    push(0);

    // The closest node seen, so a search that fails still says how near it got.
    i32 best       = 0;
    f32 best_score = first.f;
    i32 reached    = -1;

    PathStep scratch[NodeEvaluator::kMaxNeighbours];

    while (!heap_.empty()) {
        const i32 current = pop();
        Node&     node    = nodes_[static_cast<usize>(current)];
        if (node.closed) {
            continue;
        }
        node.closed = true;
        ++visited_;

        if (node.pos == to) {
            reached = current;
            best    = current;
            break;
        }
        {
            const f32 score = distance(node.pos, to);
            if (score < best_score) {
                best_score = score;
                best       = current;
            }
        }
        if (nodes_.size() + NodeEvaluator::kMaxNeighbours > capacity_) {
            break;  // budget spent: keep the best partial route
        }

        const BlockPos here = node.pos;
        const f32      g    = node.g;
        const usize    n = evaluator.neighbours(level, here, size, abilities, scratch);
        for (usize i = 0; i < n; ++i) {
            const PathStep& step = scratch[i];
            if (distance(start, step.pos) > max_range) {
                continue;
            }
            const f32 malus = path_malus(step.type);
            if (malus < 0.0F) {
                continue;
            }
            const f32 tentative = g + distance(here, step.pos) + malus;
            const u64 key       = pack(step.pos);
            const i32 existing  = lookup(key);
            if (existing >= 0) {
                Node& other = nodes_[static_cast<usize>(existing)];
                if (other.closed || tentative >= other.g) {
                    continue;
                }
                other.g      = tentative;
                other.f      = tentative + distance(step.pos, to);
                other.parent = current;
                other.type   = step.type;
                push(existing);
                continue;
            }
            Node fresh;
            fresh.pos    = step.pos;
            fresh.g      = tentative;
            fresh.f      = tentative + distance(step.pos, to);
            fresh.parent = current;
            fresh.type   = step.type;
            nodes_.push_back(fresh);
            const i32 index = static_cast<i32>(nodes_.size()) - 1;
            insert(key, index);
            push(index);
        }
    }

    // Walk the parents back, then reverse. Reserving once and reversing in
    // place rather than inserting at the front, which is quadratic on a route
    // that can be two hundred blocks long.
    const i32 tail = reached >= 0 ? reached : best;
    for (i32 walk = tail; walk >= 0; walk = nodes_[static_cast<usize>(walk)].parent) {
        const Node& node = nodes_[static_cast<usize>(walk)];
        out.steps.push_back(PathStep{node.pos, node.type});
    }
    std::reverse(out.steps.begin(), out.steps.end());
    out.reached = reached >= 0;
    out.visited = visited_;
    return out.reached;
}

// ── PathFollower ────────────────────────────────────────────────────────────

void PathFollower::set(const Path& path) {
    steps_.assign(path.steps.begin(), path.steps.end());
    index_ = 0;
}

bool PathFollower::next_waypoint(const Vec3d& position, f32 width, Vec3d& out) {
    // Half the body plus a little, and not less than a third of a block. A
    // tolerance that scales purely with width would give a chicken a target it
    // can never be "at", and it would dither on every corner forever.
    const f64 tolerance = std::max(0.35, static_cast<f64>(width) * 0.5);
    while (index_ < steps_.size()) {
        const BlockPos step   = steps_[index_].pos;
        const f64      target_x = static_cast<f64>(step.x) + 0.5;
        const f64      target_z = static_cast<f64>(step.z) + 0.5;
        const f64      dx     = position.x - target_x;
        const f64      dz     = position.z - target_z;
        const f64      dy     = position.y - static_cast<f64>(step.y);
        if (dx * dx + dz * dz < tolerance * tolerance && std::abs(dy) < 1.0) {
            ++index_;
            continue;
        }
        out = Vec3d{target_x, static_cast<f64>(step.y), target_z};
        return true;
    }
    return false;
}

}  // namespace ov::gameplay
