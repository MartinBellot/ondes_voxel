#include "ov/gameplay/signal.hpp"

#include <algorithm>
#include <array>

namespace ov::gameplay {
namespace {

/// Conducting redstone is a full collision cube, plus and minus these two lists.
///
/// Measured, block by block, on a real 1.20.1 server: a lever was stood on each
/// of the 1003 blocks in turn and a lone wire beside it read back out of the
/// save, which is 15 exactly when the block relays the lever's strong power.
/// 794 of the 830 blocks that survived the probe agree with the plain
/// full-cube rule. These are the 36 that do not, and no property of a block
/// predicts which side it falls on.
///
/// The surprises, in both directions: a redstone lamp, a slime block, a barrier
/// and every shulker box conduct, which "is it see-through" would get wrong;
/// mud and soul sand conduct without being full cubes at all, and they are the
/// same two blocks whose *support* face is full while their collision box is
/// not — see connections.cpp, which found them independently.
/// See docs/provenance/redstone.md.
constexpr std::array<std::string_view, 34> kNonConductors{
    "minecraft:glass",
    "minecraft:tinted_glass",
    "minecraft:white_stained_glass",
    "minecraft:orange_stained_glass",
    "minecraft:magenta_stained_glass",
    "minecraft:light_blue_stained_glass",
    "minecraft:yellow_stained_glass",
    "minecraft:lime_stained_glass",
    "minecraft:pink_stained_glass",
    "minecraft:gray_stained_glass",
    "minecraft:light_gray_stained_glass",
    "minecraft:cyan_stained_glass",
    "minecraft:purple_stained_glass",
    "minecraft:blue_stained_glass",
    "minecraft:brown_stained_glass",
    "minecraft:green_stained_glass",
    "minecraft:red_stained_glass",
    "minecraft:black_stained_glass",
    "minecraft:acacia_leaves",
    "minecraft:azalea_leaves",
    "minecraft:birch_leaves",
    "minecraft:cherry_leaves",
    "minecraft:dark_oak_leaves",
    "minecraft:flowering_azalea_leaves",
    "minecraft:jungle_leaves",
    "minecraft:mangrove_leaves",
    "minecraft:oak_leaves",
    "minecraft:spruce_leaves",
    "minecraft:ice",
    "minecraft:frosted_ice",
    "minecraft:sea_lantern",
    "minecraft:glowstone",
    "minecraft:beacon",
    "minecraft:observer",
};

/// Blocks that conduct although their collision box is not a whole cube.
///
/// Exactly two, and they are the same pair whose support face is full while
/// their collision box is a sixteenth short.
constexpr std::array<std::string_view, 2> kConductsAnyway{"minecraft:mud", "minecraft:soul_sand"};

/// Blocks whose conducting depends on their state rather than their block.
///
/// An **extended** piston conducts nothing — measured, by powering one from the
/// side with a repeater and reading a wire beyond it: 0. A retracted one could
/// not be measured the same way, because the probe that powers it is also what
/// makes it extend; it is treated as the full cube it is, and that assumption is
/// named in docs/provenance/redstone.md rather than hidden here.
constexpr std::array<std::string_view, 2> kNotWhenExtended{"minecraft:piston",
                                                           "minecraft:sticky_piston"};

struct SourceEntry {
    std::string_view name;
    SignalKind       kind;
};

/// Every block this model knows how to ask for a signal.
///
/// Written out one by one rather than pattern-matched on the name: "everything
/// ending in _button" is right until someone adds a block that ends in _button
/// and is not one.
constexpr std::array<SourceEntry, 47> kSources{{
    {"minecraft:redstone_block", SignalKind::RedstoneBlock},
    {"minecraft:redstone_torch", SignalKind::Torch},
    {"minecraft:redstone_wall_torch", SignalKind::WallTorch},
    {"minecraft:lever", SignalKind::Attached},
    {"minecraft:stone_button", SignalKind::Attached},
    {"minecraft:polished_blackstone_button", SignalKind::Attached},
    {"minecraft:oak_button", SignalKind::Attached},
    {"minecraft:spruce_button", SignalKind::Attached},
    {"minecraft:birch_button", SignalKind::Attached},
    {"minecraft:jungle_button", SignalKind::Attached},
    {"minecraft:acacia_button", SignalKind::Attached},
    {"minecraft:dark_oak_button", SignalKind::Attached},
    {"minecraft:mangrove_button", SignalKind::Attached},
    {"minecraft:cherry_button", SignalKind::Attached},
    {"minecraft:bamboo_button", SignalKind::Attached},
    {"minecraft:crimson_button", SignalKind::Attached},
    {"minecraft:warped_button", SignalKind::Attached},
    {"minecraft:stone_pressure_plate", SignalKind::PressurePlate},
    {"minecraft:polished_blackstone_pressure_plate", SignalKind::PressurePlate},
    {"minecraft:oak_pressure_plate", SignalKind::PressurePlate},
    {"minecraft:spruce_pressure_plate", SignalKind::PressurePlate},
    {"minecraft:birch_pressure_plate", SignalKind::PressurePlate},
    {"minecraft:jungle_pressure_plate", SignalKind::PressurePlate},
    {"minecraft:acacia_pressure_plate", SignalKind::PressurePlate},
    {"minecraft:dark_oak_pressure_plate", SignalKind::PressurePlate},
    {"minecraft:mangrove_pressure_plate", SignalKind::PressurePlate},
    {"minecraft:cherry_pressure_plate", SignalKind::PressurePlate},
    {"minecraft:bamboo_pressure_plate", SignalKind::PressurePlate},
    {"minecraft:crimson_pressure_plate", SignalKind::PressurePlate},
    {"minecraft:warped_pressure_plate", SignalKind::PressurePlate},
    {"minecraft:light_weighted_pressure_plate", SignalKind::WeightedPlate},
    {"minecraft:heavy_weighted_pressure_plate", SignalKind::WeightedPlate},
    {"minecraft:tripwire_hook", SignalKind::TripwireHook},
    {"minecraft:repeater", SignalKind::Diode},
    {"minecraft:comparator", SignalKind::Diode},
    {"minecraft:observer", SignalKind::Observer},
    {"minecraft:daylight_detector", SignalKind::Analogue},
    {"minecraft:target", SignalKind::Analogue},
    {"minecraft:sculk_sensor", SignalKind::Analogue},
    {"minecraft:calibrated_sculk_sensor", SignalKind::Analogue},
    {"minecraft:detector_rail", SignalKind::DetectorRail},
    {"minecraft:redstone_wire", SignalKind::Wire},
    {"minecraft:lightning_rod", SignalKind::LightningRod},
    {"minecraft:trapped_chest", SignalKind::Analogue},
    {"minecraft:lectern", SignalKind::Analogue},
    {"minecraft:jukebox", SignalKind::None},
    {"minecraft:powered_rail", SignalKind::None},
}};

/// The four side properties of a wire, in `Direction` order for the horizontals.
[[nodiscard]] std::string_view wire_side_name(Direction d) noexcept {
    switch (d) {
        case Direction::North: return "north";
        case Direction::South: return "south";
        case Direction::West: return "west";
        case Direction::East: return "east";
        default: return {};
    }
}

}  // namespace

Signals::Signals(const registry::BlockRegistry& blocks, const registry::Registries& registries)
    : blocks_{&blocks} {
    (void)registries;

    kinds_.assign(blocks.block_count(), SignalKind::None);
    for (const SourceEntry& entry : kSources) {
        if (const auto id = blocks.find_block(entry.name)) {
            kinds_[id->value()] = entry.kind;
        }
    }
    if (const auto id = blocks.find_block("minecraft:redstone_wire")) {
        wire_ = *id;
    }

    // Per state: is the collision shape a full cube? The boxes are in
    // thirty-seconds of a block, so a whole cube is one box from 0 to 32.
    full_cube_.assign(blocks.state_count(), false);
    conductor_.assign(blocks.state_count(), false);

    std::vector<bool> never(blocks.block_count(), false);
    std::vector<bool> always(blocks.block_count(), false);
    std::vector<bool> not_extended(blocks.block_count(), false);
    const auto        mark = [&](std::span<const std::string_view> names, std::vector<bool>& into) {
        for (const std::string_view name : names) {
            if (const auto id = blocks.find_block(name)) {
                into[id->value()] = true;
            }
        }
    };
    mark(kNonConductors, never);
    mark(kConductsAnyway, always);
    mark(kNotWhenExtended, not_extended);

    for (usize index = 0; index < blocks.state_count(); ++index) {
        const registry::BlockStateId state{static_cast<u16>(index)};
        const registry::BlockId      block = blocks.block_of(state);
        const auto                   boxes = blocks.collision_boxes(state);
        const bool                   full =
            boxes.size() == 1 && boxes[0].min_x == 0 && boxes[0].min_y == 0 &&
            boxes[0].min_z == 0 && boxes[0].max_x == 32 && boxes[0].max_y == 32 &&
            boxes[0].max_z == 32;
        full_cube_[index] = full;

        bool conducts = always[block.value()] || (full && !never[block.value()]);
        if (conducts && not_extended[block.value()] && flag_of(state, "extended")) {
            conducts = false;
        }
        conductor_[index] = conducts;
    }

    // Every block vanilla powers that this model does not answer for. Named so
    // a gap fails a test with a block name in it rather than behaving as zero.
    for (usize block = 0; block < blocks.block_count(); ++block) {
        const registry::BlockId id{static_cast<u16>(block)};
        if (kinds_[block] != SignalKind::None) {
            continue;
        }
        if (blocks.find_property(id, "powered").has_value() ||
            blocks.find_property(id, "power").has_value()) {
            unhandled_.push_back(blocks.block_name(id));
        }
    }
    std::ranges::sort(unhandled_);
}

SignalKind Signals::kind_of(registry::BlockId block) const noexcept {
    return block.value() < kinds_.size() ? kinds_[block.value()] : SignalKind::None;
}

bool Signals::is_conductor(registry::BlockStateId state) const noexcept {
    return state.value() < conductor_.size() && conductor_[state.value()];
}

bool Signals::is_full_cube(registry::BlockStateId state) const noexcept {
    return state.value() < full_cube_.size() && full_cube_[state.value()];
}

i32 Signals::power_of(registry::BlockStateId state) const noexcept {
    const auto block = blocks_->block_of(state);
    const auto prop  = blocks_->find_property(block, "power");
    if (!prop.has_value()) {
        return -1;
    }
    return static_cast<i32>(blocks_->property_index(state, *prop));
}

registry::BlockStateId Signals::with_power(registry::BlockStateId state, i32 power) const noexcept {
    const auto block = blocks_->block_of(state);
    const auto prop  = blocks_->find_property(block, "power");
    if (!prop.has_value()) {
        return state;
    }
    return blocks_->with_property(state, *prop, static_cast<u16>(std::clamp(power, 0, 15)));
}

bool Signals::flag_of(registry::BlockStateId state, std::string_view name) const noexcept {
    const auto block = blocks_->block_of(state);
    const auto prop  = blocks_->find_property(block, name);
    if (!prop.has_value()) {
        return false;
    }
    return blocks_->property_value(state, *prop) == "true";
}

registry::BlockStateId Signals::with_flag(registry::BlockStateId state, std::string_view name,
                                          bool value) const noexcept {
    const auto block = blocks_->block_of(state);
    const auto prop  = blocks_->find_property(block, name);
    if (!prop.has_value()) {
        return state;
    }
    // The values are declared "true" then "false", so the index is the negation
    // of the boolean. Read from the table rather than assumed, because a block
    // that declared them the other way round would be invisible otherwise.
    for (u16 i = 0; i < prop->values.size(); ++i) {
        if ((prop->values[i] == "true") == value) {
            return blocks_->with_property(state, *prop, i);
        }
    }
    return state;
}

std::optional<Direction> Signals::facing_of(registry::BlockStateId state) const noexcept {
    const auto block = blocks_->block_of(state);
    const auto prop  = blocks_->find_property(block, "facing");
    if (!prop.has_value()) {
        return std::nullopt;
    }
    return direction_from_name(blocks_->property_value(state, *prop));
}

Direction Signals::attached_direction(registry::BlockStateId state) const noexcept {
    const auto block = blocks_->block_of(state);
    const auto face  = blocks_->find_property(block, "face");
    if (face.has_value()) {
        const std::string_view value = blocks_->property_value(state, *face);
        if (value == "floor") {
            return Direction::Up;
        }
        if (value == "ceiling") {
            return Direction::Down;
        }
    }
    // A wall switch, and every block that only has `facing`: the hook, the rod.
    return facing_of(state).value_or(Direction::Up);
}

i32 Signals::diode_output(registry::BlockStateId state) const noexcept {
    const auto block = blocks_->block_of(state);
    if (blocks_->block_name(block) == "minecraft:comparator") {
        // A comparator's output is not a property. Vanilla keeps it in the
        // block entity, and so do we — this reads the on/off flag only, which
        // is all a repeater ever needs and all a comparator's *neighbours* can
        // see without asking the world.
        return flag_of(state, "powered") ? 15 : 0;
    }
    return flag_of(state, "powered") ? 15 : 0;
}

i32 Signals::wire_signal(const RedstoneWorld& world, BlockPos pos, registry::BlockStateId state,
                         Direction from) const {
    const i32 power = power_of(state);
    if (power <= 0) {
        return 0;
    }
    // `from` pointing up means the block being powered is the one **below** the
    // wire, and a wire always powers what it stands on whatever its shape says.
    if (from == Direction::Up) {
        return power;
    }
    const Direction towards = opposite(from);
    const auto      side    = wire_side_name(towards);
    if (side.empty()) {
        // `from` is Down: the block above a wire is never powered by it.
        return 0;
    }
    const auto block = blocks_->block_of(state);
    const auto prop  = blocks_->find_property(block, side);
    if (!prop.has_value()) {
        return 0;
    }
    (void)pos;
    (void)world;
    return blocks_->property_value(state, *prop) == "none" ? 0 : power;
}

i32 Signals::weak_signal(const RedstoneWorld& world, BlockPos pos, Direction from,
                         bool wires_signal) const {
    const registry::BlockStateId state = world.block_at(pos);
    const registry::BlockId      block = blocks_->block_of(state);

    switch (kind_of(block)) {
        case SignalKind::None: return 0;

        case SignalKind::RedstoneBlock: return 15;

        case SignalKind::Torch:
            // Not through the face it stands on: the block under a torch is not
            // weakly powered by it, which is the whole reason a torch on a
            // block does not power that block.
            return flag_of(state, "lit") && from != Direction::Up ? 15 : 0;

        case SignalKind::WallTorch:
            return flag_of(state, "lit") && facing_of(state) != std::optional{from} ? 15 : 0;

        case SignalKind::Attached:
        case SignalKind::PressurePlate:
        case SignalKind::TripwireHook:
        case SignalKind::LightningRod: return flag_of(state, "powered") ? 15 : 0;

        case SignalKind::DetectorRail: return flag_of(state, "powered") ? 15 : 0;

        case SignalKind::WeightedPlate:
        case SignalKind::Analogue: return std::max(0, power_of(state));

        case SignalKind::Diode:
        case SignalKind::Observer:
            // A diode and an observer give the same thing weakly and strongly:
            // out of one face only.
            return strong_signal(world, pos, from, wires_signal);

        case SignalKind::Wire:
            return wires_signal ? wire_signal(world, pos, state, from) : 0;
    }
    return 0;
}

i32 Signals::strong_signal(const RedstoneWorld& world, BlockPos pos, Direction from,
                           bool wires_signal) const {
    const registry::BlockStateId state = world.block_at(pos);
    const registry::BlockId      block = blocks_->block_of(state);

    switch (kind_of(block)) {
        case SignalKind::None:
        case SignalKind::RedstoneBlock:
        case SignalKind::Analogue:
            // The redstone block powers everything around it and strengthens
            // nothing: a block beside one is not itself a source.
            return 0;

        case SignalKind::Torch:
        case SignalKind::WallTorch:
            // Only upwards: `from == Down` means the block being powered is
            // above the torch.
            return flag_of(state, "lit") && from == Direction::Down ? 15 : 0;

        case SignalKind::Attached:
            return flag_of(state, "powered") && attached_direction(state) == from ? 15 : 0;

        case SignalKind::PressurePlate:
            return flag_of(state, "powered") && from == Direction::Up ? 15 : 0;

        case SignalKind::WeightedPlate:
            return from == Direction::Up ? std::max(0, power_of(state)) : 0;

        case SignalKind::DetectorRail:
            return flag_of(state, "powered") && from == Direction::Up ? 15 : 0;

        case SignalKind::TripwireHook:
        case SignalKind::LightningRod:
            return flag_of(state, "powered") && facing_of(state) == std::optional{from} ? 15 : 0;

        case SignalKind::Diode: {
            // `facing` names the side the input comes from, so the output goes
            // out of the opposite face — and the query direction, which points
            // from the powered block back to this one, is `facing` again.
            if (facing_of(state) != std::optional{from}) {
                return 0;
            }
            if (blocks_->block_name(block) == "minecraft:comparator") {
                const i32 stored = world.container_signal(pos);
                return stored >= 0 ? stored : diode_output(state);
            }
            return diode_output(state);
        }

        case SignalKind::Observer:
            return flag_of(state, "powered") && facing_of(state) == std::optional{from} ? 15 : 0;

        case SignalKind::Wire:
            return wires_signal ? wire_signal(world, pos, state, from) : 0;
    }
    return 0;
}

i32 Signals::signal_at(const RedstoneWorld& world, BlockPos pos, Direction from,
                       bool wires_signal) const {
    const i32 weak = weak_signal(world, pos, from, wires_signal);
    if (!is_conductor(world.block_at(pos))) {
        return weak;
    }
    // A solid block that anything strongly powers becomes a source of its own.
    // This is the step that makes "wire into a wall, repeater out of the wall"
    // work, and leaving it out breaks more circuits than any other omission.
    return std::max(weak, direct_signal_to(world, pos, wires_signal));
}

i32 Signals::best_neighbour_signal(const RedstoneWorld& world, BlockPos pos,
                                   bool wires_signal) const {
    i32 best = 0;
    for (u8 i = 0; i < kDirectionCount; ++i) {
        const auto d = static_cast<Direction>(i);
        best         = std::max(best, signal_at(world, pos.offset(d), d, wires_signal));
        if (best >= 15) {
            return 15;
        }
    }
    return best;
}

i32 Signals::direct_signal_to(const RedstoneWorld& world, BlockPos pos, bool wires_signal) const {
    i32 best = 0;
    for (u8 i = 0; i < kDirectionCount; ++i) {
        const auto d = static_cast<Direction>(i);
        best         = std::max(best, strong_signal(world, pos.offset(d), d, wires_signal));
        if (best >= 15) {
            return 15;
        }
    }
    return best;
}

bool Signals::has_neighbour_signal(const RedstoneWorld& world, BlockPos pos) const {
    return best_neighbour_signal(world, pos, true) > 0;
}

}  // namespace ov::gameplay
