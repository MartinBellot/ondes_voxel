#include "ov/gameplay/connections.hpp"

#include <algorithm>

namespace ov::gameplay {
namespace {

/// Blocks that present a full face and are refused anyway.
///
/// Measured: a fence was placed against every block state on a real 1.20.1
/// server, and these are the ones whose shape says yes and whose answer is no.
/// Leaves and shulker boxes are unsurprising once seen; `target` is not.
constexpr std::array<std::string_view, 33> kRefused{
    "minecraft:barrier",
    "minecraft:carved_pumpkin",
    "minecraft:jack_o_lantern",
    "minecraft:melon",
    "minecraft:pumpkin",
    "minecraft:target",
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
    "minecraft:shulker_box",
    "minecraft:white_shulker_box",
    "minecraft:orange_shulker_box",
    "minecraft:magenta_shulker_box",
    "minecraft:light_blue_shulker_box",
    "minecraft:yellow_shulker_box",
    "minecraft:lime_shulker_box",
    "minecraft:pink_shulker_box",
    "minecraft:gray_shulker_box",
    "minecraft:light_gray_shulker_box",
    "minecraft:cyan_shulker_box",
    "minecraft:purple_shulker_box",
    "minecraft:blue_shulker_box",
    "minecraft:brown_shulker_box",
    "minecraft:green_shulker_box",
    "minecraft:red_shulker_box",
    "minecraft:black_shulker_box",
};

/// Blocks whose support face is full although their collision box is not.
///
/// Vanilla asks the *support* shape, not the collision one, and for these two
/// they differ: mud and soul sand are a sixteenth shorter to stand on and still
/// present a whole face. Measured — they were the last two disagreements out of
/// 23358, and nothing else in the game does this.
constexpr std::array<std::string_view, 2> kSupportIsFull{"minecraft:mud", "minecraft:soul_sand"};

/// The face of a neighbour that touches a block on this side.
[[nodiscard]] registry::BlockRegistry::Face facing_back(Side side) noexcept {
    switch (side) {
        case Side::North: return registry::BlockRegistry::Face::South;
        case Side::South: return registry::BlockRegistry::Face::North;
        case Side::West: return registry::BlockRegistry::Face::East;
        default: return registry::BlockRegistry::Face::West;
    }
}

[[nodiscard]] bool is_north_south(Side side) noexcept {
    return side == Side::North || side == Side::South;
}

}  // namespace

Connections::Connections(const registry::BlockRegistry& blocks,
                         const registry::Registries&    registries)
    : blocks_{&blocks} {
    kinds_.resize(blocks.block_count(), ConnectingKind::None);
    refuses_.resize(blocks.block_count(), false);
    support_full_.resize(blocks.block_count(), false);
    post_override_.resize(blocks.block_count(), false);

    const auto block_registry = registries.find("minecraft:block");
    const auto post_tag = block_registry
                              ? registries.find_tag(*block_registry, "minecraft:wall_post_override")
                              : std::nullopt;

    for (u16 index = 0; index < blocks.block_count(); ++index) {
        const registry::BlockId block{index};
        const std::string_view  name = blocks.block_name(block);

        if (name == "minecraft:nether_brick_fence") {
            kinds_[index] = ConnectingKind::NetherBrickFence;
        } else if (name.ends_with("_fence")) {
            kinds_[index] = ConnectingKind::Fence;
        } else if (name.ends_with("_fence_gate")) {
            kinds_[index] = ConnectingKind::FenceGate;
        } else if (name.ends_with("_pane") || name == "minecraft:iron_bars") {
            kinds_[index] = ConnectingKind::Pane;
        } else if (name.ends_with("_wall")) {
            kinds_[index] = ConnectingKind::Wall;
        } else if (name.ends_with("_stairs")) {
            kinds_[index] = ConnectingKind::Stairs;
        }

        refuses_[index]      = std::ranges::find(kRefused, name) != kRefused.end();
        support_full_[index] = std::ranges::find(kSupportIsFull, name) != kSupportIsFull.end();

        if (post_tag && block_registry) {
            if (const auto id = registries.protocol_id(*block_registry, name)) {
                post_override_[index] = registries.tag_contains(*post_tag, *id);
            }
        }
    }
}

ConnectingKind Connections::kind_of(registry::BlockId block) const noexcept {
    return block.value() < kinds_.size() ? kinds_[block.value()] : ConnectingKind::None;
}

bool Connections::attaches(ConnectingKind kind, Side side,
                           registry::BlockStateId neighbour) const noexcept {
    const registry::BlockId other      = blocks_->block_of(neighbour);
    const ConnectingKind    other_kind = kind_of(other);

    // Same family first. Two wooden fences reach for each other whatever their
    // shape says; the nether brick one keeps to itself, which is the single
    // case the measurement disagreed on until it was written down.
    switch (kind) {
        case ConnectingKind::Fence:
            if (other_kind == ConnectingKind::Fence) {
                return true;
            }
            break;
        case ConnectingKind::NetherBrickFence:
            if (other_kind == ConnectingKind::NetherBrickFence) {
                return true;
            }
            break;
        case ConnectingKind::Pane:
            if (other_kind == ConnectingKind::Pane || other_kind == ConnectingKind::Wall) {
                return true;
            }
            break;
        case ConnectingKind::Wall:
            if (other_kind == ConnectingKind::Wall || other_kind == ConnectingKind::Pane) {
                return true;
            }
            break;
        default: break;
    }

    // A gate is reached for only across its axis: one facing north or south
    // lines up east and west.
    if (other_kind == ConnectingKind::FenceGate &&
        (kind == ConnectingKind::Fence || kind == ConnectingKind::NetherBrickFence)) {
        if (const auto facing = blocks_->find_property(other, "facing")) {
            const std::string_view value = blocks_->property_value(neighbour, *facing);
            const bool             gate_is_north_south = value == "north" || value == "south";
            if (gate_is_north_south != is_north_south(side)) {
                return true;
            }
        }
    }

    if (other.value() < refuses_.size() && refuses_[other.value()]) {
        return false;
    }
    if (other.value() < support_full_.size() && support_full_[other.value()]) {
        return true;
    }
    return blocks_->face_is_sturdy(neighbour, facing_back(side));
}

namespace {

/// The side index of a compass name, in the order the properties list them.
[[nodiscard]] std::optional<usize> side_index(std::string_view name) noexcept {
    for (usize i = 0; i < kSideNames.size(); ++i) {
        if (kSideNames[i] == name) {
            return i;
        }
    }
    return std::nullopt;
}

/// Turning left when looking down: north, west, south, east, north.
[[nodiscard]] std::string_view counter_clockwise(std::string_view facing) noexcept {
    if (facing == "north") {
        return "west";
    }
    if (facing == "west") {
        return "south";
    }
    if (facing == "south") {
        return "east";
    }
    return "north";
}

[[nodiscard]] std::string_view opposite(std::string_view facing) noexcept {
    if (facing == "north") {
        return "south";
    }
    if (facing == "south") {
        return "north";
    }
    if (facing == "west") {
        return "east";
    }
    return "west";
}

}  // namespace

registry::BlockStateId Connections::wall_shape(registry::BlockStateId                       state,
                                               const std::array<registry::BlockStateId, 4>& around,
                                               registry::BlockStateId above) const noexcept {
    const registry::BlockId block = blocks_->block_of(state);
    const auto              up    = blocks_->find_property(block, "up");
    if (!up) {
        return state;
    }

    // A side rises when the block above fills its own downward face: stone and
    // a bottom slab do, a wall's post and a torch do not.
    const bool raised = blocks_->face_is_sturdy(above, registry::BlockRegistry::Face::Down);

    registry::BlockStateId reshaped = state;
    std::array<bool, 4>    attached{};
    for (u8 index = 0; index < 4; ++index) {
        const auto property = blocks_->find_property(block, kSideNames[index]);
        if (!property) {
            continue;
        }
        attached[index] = attaches(ConnectingKind::Wall, static_cast<Side>(index), around[index]);
        const std::string_view wanted = !attached[index] ? "none" : (raised ? "tall" : "low");
        const auto             it     = std::ranges::find(property->values, wanted);
        if (it != property->values.end()) {
            reshaped = blocks_->with_property(
                reshaped, *property, static_cast<u16>(std::distance(property->values.begin(), it)));
        }
    }

    // The post stands unless the connections are symmetric on both axes — a
    // straight line, or a full cross — and nothing above insists.
    const registry::BlockId above_block = blocks_->block_of(above);
    bool                    post        = attached[0] != attached[1] || attached[2] != attached[3];
    if (!attached[0] && !attached[1] && !attached[2] && !attached[3]) {
        post = true;
    }
    if (above_block.value() < post_override_.size() && post_override_[above_block.value()]) {
        post = true;
    }
    if (kind_of(above_block) == ConnectingKind::Wall) {
        const auto above_up = blocks_->find_property(above_block, "up");
        if (above_up && blocks_->property_value(above, *above_up) == "true") {
            post = true;
        }
    }

    const auto it = std::ranges::find(up->values, post ? "true" : "false");
    if (it != up->values.end()) {
        reshaped = blocks_->with_property(reshaped, *up,
                                          static_cast<u16>(std::distance(up->values.begin(), it)));
    }
    return reshaped;
}

registry::BlockStateId Connections::stair_shape(
    registry::BlockStateId                       state,
    const std::array<registry::BlockStateId, 4>& around) const noexcept {
    const registry::BlockId block  = blocks_->block_of(state);
    const auto              facing = blocks_->find_property(block, "facing");
    const auto              half   = blocks_->find_property(block, "half");
    const auto              shape  = blocks_->find_property(block, "shape");
    if (!facing || !half || !shape) {
        return state;
    }
    const std::string_view mine    = blocks_->property_value(state, *facing);
    const std::string_view my_half = blocks_->property_value(state, *half);

    /// Is this neighbour a stair of the same half whose facing crosses ours?
    const auto crossing = [&](registry::BlockStateId neighbour) -> std::string_view {
        const registry::BlockId other = blocks_->block_of(neighbour);
        if (kind_of(other) != ConnectingKind::Stairs) {
            return {};
        }
        const auto other_half   = blocks_->find_property(other, "half");
        const auto other_facing = blocks_->find_property(other, "facing");
        if (!other_half || !other_facing ||
            blocks_->property_value(neighbour, *other_half) != my_half) {
            return {};
        }
        const std::string_view theirs      = blocks_->property_value(neighbour, *other_facing);
        const bool             mine_is_z   = mine == "north" || mine == "south";
        const bool             theirs_is_z = theirs == "north" || theirs == "south";
        return mine_is_z == theirs_is_z ? std::string_view{} : theirs;
    };

    /// A stair facing our way, on our half, in that direction, keeps the corner
    /// from forming. Measured: exactly one of the two crossing directions does.
    const auto blocked_from = [&](std::string_view direction) {
        const auto index = side_index(direction);
        if (!index) {
            return false;
        }
        const registry::BlockStateId neighbour = around[*index];
        const registry::BlockId      other     = blocks_->block_of(neighbour);
        if (kind_of(other) != ConnectingKind::Stairs) {
            return false;
        }
        const auto other_half   = blocks_->find_property(other, "half");
        const auto other_facing = blocks_->find_property(other, "facing");
        return other_half && other_facing &&
               blocks_->property_value(neighbour, *other_half) == my_half &&
               blocks_->property_value(neighbour, *other_facing) == mine;
    };

    const auto set_shape = [&](std::string_view value) {
        const auto it = std::ranges::find(shape->values, value);
        return it == shape->values.end()
                   ? state
                   : blocks_->with_property(
                         state, *shape, static_cast<u16>(std::distance(shape->values.begin(), it)));
    };

    const auto front_index = side_index(mine);
    const auto back_index  = side_index(opposite(mine));
    if (!front_index || !back_index) {
        return state;
    }

    // In front first, then behind: an outer corner wins over an inner one when
    // both could apply.
    if (const std::string_view theirs = crossing(around[*front_index]); !theirs.empty()) {
        if (!blocked_from(opposite(theirs))) {
            return set_shape(theirs == counter_clockwise(mine) ? "outer_left" : "outer_right");
        }
    }
    if (const std::string_view theirs = crossing(around[*back_index]); !theirs.empty()) {
        if (!blocked_from(theirs)) {
            return set_shape(theirs == counter_clockwise(mine) ? "inner_left" : "inner_right");
        }
    }
    return set_shape("straight");
}

registry::BlockStateId Connections::reshape(registry::BlockStateId                       state,
                                            const std::array<registry::BlockStateId, 4>& around,
                                            registry::BlockStateId above) const noexcept {
    const registry::BlockId block = blocks_->block_of(state);
    const ConnectingKind    kind  = kind_of(block);
    if (kind == ConnectingKind::Stairs) {
        return stair_shape(state, around);
    }
    if (kind == ConnectingKind::Wall) {
        return wall_shape(state, around, above);
    }
    if (kind != ConnectingKind::Fence && kind != ConnectingKind::NetherBrickFence &&
        kind != ConnectingKind::Pane) {
        return state;
    }

    registry::BlockStateId reshaped = state;
    for (u8 index = 0; index < 4; ++index) {
        const auto side     = static_cast<Side>(index);
        const auto property = blocks_->find_property(block, kSideNames[index]);
        if (!property) {
            continue;
        }
        const std::string_view wanted = attaches(kind, side, around[index]) ? "true" : "false";
        const auto             it     = std::ranges::find(property->values, wanted);
        if (it != property->values.end()) {
            reshaped = blocks_->with_property(
                reshaped, *property, static_cast<u16>(std::distance(property->values.begin(), it)));
        }
    }
    return reshaped;
}

}  // namespace ov::gameplay
