#include "ov/gameplay/falling_block.hpp"

#include "ov/gameplay/entity_physics.hpp"
#include "ov/gameplay/mob_logic.hpp"
#include "ov/math/raycast.hpp"

#include <array>
#include <cmath>

namespace ov::gameplay {
namespace {

/// The falling blocks that are not concrete powder, by name. The sixteen
/// powders are found by suffix, because they are sixteen of one thing.
constexpr std::array<std::string_view, 9> kFalling{
    "minecraft:sand",          "minecraft:red_sand",        "minecraft:gravel",
    "minecraft:anvil",         "minecraft:chipped_anvil",   "minecraft:damaged_anvil",
    "minecraft:dragon_egg",    "minecraft:suspicious_sand", "minecraft:suspicious_gravel",
};

/// What this module recognises and does not carry out. Each one falls — that
/// part is the same rule for all of them — and then does something more that
/// is not here.
constexpr std::array<std::string_view, 6> kUnimplemented{
    // An anvil hurts what it lands on and may crack a stage on landing.
    "minecraft:anvil",
    "minecraft:chipped_anvil",
    "minecraft:damaged_anvil",
    // Clicking a dragon egg teleports it; falling it does do.
    "minecraft:dragon_egg",
    // The brush reward lives in a block entity this server does not carry.
    "minecraft:suspicious_sand",
    "minecraft:suspicious_gravel",
};

constexpr std::string_view kPowderSuffix = "_concrete_powder";

[[nodiscard]] bool ends_with(std::string_view text, std::string_view suffix) noexcept {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

}  // namespace

// ── The step ────────────────────────────────────────────────────────────────

entity::EntityState step_block_entity(const entity::EntityState& state,
                                      const BlockEntityMotion& motion, const CollisionWorld& world,
                                      bool gravity) {
    entity::EntityState next = state;
    Vec3d               v    = state.velocity;
    if (gravity) {
        v.y -= motion.gravity;
    }

    const Vec3d allowed = world.slide(entity_box(state), v);
    next.position.x += allowed.x;
    next.position.y += allowed.y;
    next.position.z += allowed.z;

    // A collision on an axis stops that axis. Vertically that is measured: a
    // TNT resting on stone reads a Motion of exactly 0.0 on y, tick after
    // tick, where a bounce would read +0.0196.
    const bool vertical = allowed.y != v.y;
    next.on_ground      = vertical && v.y < 0.0;
    if (allowed.x != v.x) {
        v.x = 0.0;
    }
    if (allowed.z != v.z) {
        v.z = 0.0;
    }
    if (vertical) {
        v.y = 0.0;
    }

    // Drag after the move, on every axis alike, then the ground's own share
    // on the horizontal. The order is what the Motion tag shows: -0.0392
    // after one tick of a fall is (0 - 0.04) × 0.98, not 0.98 × 0 - 0.04.
    v.x *= motion.drag;
    v.y *= motion.drag;
    v.z *= motion.drag;
    if (next.on_ground) {
        v.x *= motion.ground_horizontal;
        v.z *= motion.ground_horizontal;
    }
    next.velocity = v;
    return next;
}

// ── The rules ───────────────────────────────────────────────────────────────

FallingBlocks::FallingBlocks(const registry::BlockRegistry& blocks,
                             const registry::Registries&    registries)
    : blocks_{&blocks} {
    const usize count = blocks.block_count();
    falls_.assign(count, 0);
    free_.assign(count, 0);
    replaceable_.assign(count, 0);
    hardened_.assign(count, registry::BlockStateId{0});
    vanishes_.assign(count, 0);

    // `#minecraft:replaceable`, the tag 1.20 introduced for exactly this
    // question. Members are wire ids of minecraft:block; the bridge back to our
    // block ids is the name, the same bridge breaking.cpp uses.
    if (const auto block_registry = registries.find("minecraft:block")) {
        if (const auto tag = registries.find_tag(*block_registry, "minecraft:replaceable")) {
            for (const registry::ProtocolId member : registries.tag_members(*tag)) {
                const std::string_view name = registries.entry_of(*block_registry, member);
                if (const auto block = blocks.find_block(name)) {
                    replaceable_[block->value()] = 1;
                }
            }
        }
    }

    const auto id_of = [&](std::string_view name) {
        return blocks.find_block(name).value_or(registry::BlockId{0});
    };
    water_ = id_of("minecraft:water");
    snow_  = id_of("minecraft:snow");
    const registry::BlockId lava        = id_of("minecraft:lava");
    const registry::BlockId fire        = id_of("minecraft:fire");
    const registry::BlockId soul_fire   = id_of("minecraft:soul_fire");

    for (usize index = 0; index < count; ++index) {
        const registry::BlockId block{static_cast<u16>(index)};
        const std::string_view  name = blocks.block_name(block);
        bool                    falls = false;
        for (const std::string_view candidate : kFalling) {
            falls = falls || name == candidate;
        }
        if (ends_with(name, kPowderSuffix)) {
            falls = true;
            // white_concrete_powder hardens into white_concrete: the name
            // without its last word, and refused rather than guessed if the
            // registry has no such block.
            const std::string_view base = name.substr(0, name.size() - std::string_view{"_powder"}.size());
            if (const auto concrete = blocks.find_block(base)) {
                hardened_[index] = blocks.default_state(*concrete);
            }
        }
        falls_[index]    = falls ? 1 : 0;
        vanishes_[index] = (name == "minecraft:suspicious_sand" ||
                            name == "minecraft:suspicious_gravel")
                               ? 1
                               : 0;
        free_[index] = (blocks.is_air(block) || block == water_ || block == lava ||
                        block == fire || block == soul_fire || replaceable_[index] != 0)
                           ? 1
                           : 0;
    }
}

bool FallingBlocks::falls(registry::BlockStateId state) const noexcept {
    const registry::BlockId block = blocks_->block_of(state);
    return block.value() < falls_.size() && falls_[block.value()] != 0;
}

bool FallingBlocks::is_free(registry::BlockStateId state) const noexcept {
    const registry::BlockId block = blocks_->block_of(state);
    return block.value() < free_.size() && free_[block.value()] != 0;
}

bool FallingBlocks::replaceable(registry::BlockStateId state) const noexcept {
    const registry::BlockId block = blocks_->block_of(state);
    if (block.value() >= replaceable_.size() || replaceable_[block.value()] == 0) {
        return false;
    }
    // A snow layer is in the tag, and a landing block replaces only the
    // thinnest one: eight layers of snow are a full block and a sand block
    // lands on top of them. From the wiki's Snow article, not measured here.
    if (block == snow_) {
        const auto layers = blocks_->find_property(block, "layers");
        return !layers || blocks_->property_value(state, *layers) == "1";
    }
    return true;
}

std::optional<registry::BlockStateId> FallingBlocks::hardened(
    registry::BlockStateId state) const noexcept {
    const registry::BlockId block = blocks_->block_of(state);
    if (block.value() >= hardened_.size() || hardened_[block.value()].value() == 0) {
        return std::nullopt;
    }
    return hardened_[block.value()];
}

bool FallingBlocks::is_water(registry::BlockStateId state) const noexcept {
    const registry::BlockId block = blocks_->block_of(state);
    if (block == water_) {
        return true;
    }
    // A waterlogged block, kelp, seagrass: anything holding a fluid that is
    // not lava. Lava holds a fluid too and hardens nothing.
    return blocks_->holds_fluid(state) && blocks_->block_name(block) != "minecraft:lava";
}

bool FallingBlocks::touches_water(const world::LevelView& level, BlockPos pos) const {
    // Five faces, measured one at a time with the water placed last: above
    // and the four sides harden the powder, below does not — with water under
    // it the block falls instead.
    constexpr std::array<Direction, 5> kFaces{Direction::Up, Direction::North, Direction::South,
                                              Direction::West, Direction::East};
    for (const Direction face : kFaces) {
        if (is_water(level.block_at(pos.offset(face)))) {
            return true;
        }
    }
    return false;
}

bool FallingBlocks::lands_in_water(registry::BlockStateId state) const noexcept {
    return hardened(state).has_value();
}

bool FallingBlocks::neighbour_changed(world::LevelWriter& level, BlockPos pos) const {
    const registry::BlockStateId state = level.block_at(pos);
    if (!falls(state)) {
        return false;
    }
    if (const auto concrete = hardened(state); concrete && touches_water(level, pos)) {
        level.set_block(pos, *concrete);
        return true;
    }
    const BlockPos below = pos.offset(Direction::Down);
    if (!is_free(level.block_at(below))) {
        // Supported. Vanilla schedules on every neighbour change and lets the
        // tick decide; scheduling only when the tick could do something gives
        // the same world and keeps a desert's worth of sand out of the queue
        // every time water flows past it.
        return false;
    }
    const std::string_view name = blocks_->block_name(blocks_->block_of(state));
    if (level.has_scheduled_tick(pos, name, world::TickQueue::Block)) {
        return false;
    }
    level.schedule_tick(pos, name, kFallDelayTicks, world::TickQueue::Block,
                        world::TickPriority::Normal);
    return true;
}

std::optional<FallStart> FallingBlocks::tick(world::LevelWriter& level, BlockPos pos,
                                             std::string_view what) const {
    const registry::BlockStateId state = level.block_at(pos);
    if (!falls(state) || blocks_->block_name(blocks_->block_of(state)) != what) {
        // The block changed since the tick was asked for. Nothing to do, and
        // nothing wrong: vanilla drops such a tick in the same way.
        return std::nullopt;
    }
    if (!is_free(level.block_at(pos.offset(Direction::Down)))) {
        return std::nullopt;
    }
    level.set_block(pos, registry::kAirState);
    return FallStart{pos, state};
}

Landing FallingBlocks::land(world::LevelWriter& level, BlockPos cell,
                            registry::BlockStateId state, bool in_water) const {
    const registry::BlockStateId there = level.block_at(cell);
    const bool                   vanish =
        blocks_->block_of(state).value() < vanishes_.size() &&
        vanishes_[blocks_->block_of(state).value()] != 0;
    if (!replaceable(there)) {
        // Measured: sand onto a torch leaves the torch and one sand item.
        return vanish ? Landing::Vanished : Landing::Dropped;
    }
    // Resting on something that is itself free — the top of a mob, a slab of
    // air under a snow layer — is not a landing: the block would float. The
    // one exception is a powder that stopped in water, which is exactly where
    // it is meant to harden.
    if (!in_water && is_free(level.block_at(cell.offset(Direction::Down)))) {
        return vanish ? Landing::Vanished : Landing::Dropped;
    }
    registry::BlockStateId placed = state;
    if (const auto concrete = hardened(state);
        concrete && (in_water || is_water(there) || touches_water(level, cell))) {
        placed = *concrete;
    }
    level.set_block(cell, placed);
    return Landing::Placed;
}

std::string_view FallingBlocks::item_of(registry::BlockStateId state) const {
    return blocks_->block_name(blocks_->block_of(state));
}

std::span<const std::string_view> FallingBlocks::unimplemented() noexcept {
    return kUnimplemented;
}

// ── The entity ──────────────────────────────────────────────────────────────

void FallingBlockLogic::tick(entity::EntityWorld& world, entity::EntityHandle self,
                             const entity::TickContext& context) {
    const MobContext* mob = mob_context(context);
    if (mob == nullptr || mob->world == nullptr) {
        return;
    }
    entity::EntityState* state = world.mutable_state(self);
    if (state == nullptr || state->removed) {
        return;
    }

    ++time_;
    *state = step_block_entity(*state, motion_, *mob->world);

    const BlockPos cell{floor_to_block(state->position.x), floor_to_block(state->position.y),
                        floor_to_block(state->position.z)};

    // Measured: a powder dropped into a pool four deep hardened in the top
    // water cell, while sand went to the floor. So a powder stops as soon as
    // the cell it is in holds water, grounded or not.
    bool in_water = false;
    if (mob->level != nullptr && rules_->lands_in_water(state_)) {
        in_water = rules_->is_water(mob->level->block_at(cell));
    }

    if (state->on_ground || in_water) {
        events_->landed.push_back(
            FallingEvents::Landed{state->network_id, cell, state_, in_water, false});
        state->removed = true;
        return;
    }

    const world::WorldShape shape = mob->level != nullptr ? mob->level->shape()
                                                          : world::WorldShape::overworld();
    const bool outside = cell.y < shape.min_y || cell.y > shape.max_y();
    if (time_ > kFallingBlockMaxTicks || (time_ > 100 && outside)) {
        events_->landed.push_back(
            FallingEvents::Landed{state->network_id, cell, state_, false, true});
        state->removed = true;
    }
}

}  // namespace ov::gameplay
