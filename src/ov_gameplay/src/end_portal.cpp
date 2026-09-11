#include "ov/gameplay/end_portal.hpp"

#include "ov/math/random.hpp"

#include <cmath>
#include <numbers>
#include <utility>

namespace ov::gameplay {

namespace {

/// One side of the ring: where its three frames stand relative to the centre,
/// and the way they must face — the inside.
struct Side {
    i32              dx;  ///< offset of the side's middle frame from the centre
    i32              dz;
    i32              along_x;  ///< the direction the three frames run in
    i32              along_z;
    std::string_view facing;
};

constexpr std::array<Side, 4> kSides{{
    {0, -2, 1, 0, "south"},  // the north side faces south
    {0, 2, 1, 0, "north"},   // the south side faces north
    {-2, 0, 0, 1, "east"},   // the west side faces east
    {2, 0, 0, 1, "west"},    // the east side faces west
}};

}  // namespace

EndPortalRules::EndPortalRules(const registry::BlockRegistry& blocks) : blocks_(&blocks) {
    const auto frame    = blocks.find_block("minecraft:end_portal_frame");
    const auto portal   = blocks.find_block("minecraft:end_portal");
    const auto obsidian = blocks.find_block("minecraft:obsidian");
    if (!frame || !portal || !obsidian) {
        return;
    }
    frame_block_  = *frame;
    portal_block_ = *portal;
    frame_        = blocks.default_state(*frame);
    portal_       = blocks.default_state(*portal);
    obsidian_     = blocks.default_state(*obsidian);
    valid_        = blocks.find_property(*frame, "eye").has_value() &&
             blocks.find_property(*frame, "facing").has_value();

    const auto bedrock   = blocks.find_block("minecraft:bedrock");
    const auto end_stone = blocks.find_block("minecraft:end_stone");
    const auto egg       = blocks.find_block("minecraft:dragon_egg");
    const auto torch     = blocks.find_block("minecraft:wall_torch");
    const auto gateway   = blocks.find_block("minecraft:end_gateway");
    if (!bedrock || !end_stone || !egg || !torch || !gateway) {
        valid_ = false;
        return;
    }
    gateway_   = blocks.default_state(*gateway);
    bedrock_   = blocks.default_state(*bedrock);
    end_stone_ = blocks.default_state(*end_stone);
    egg_       = blocks.default_state(*egg);
    const auto facing = blocks.find_property(*torch, "facing");
    constexpr std::array<std::string_view, 4> kFacings{"north", "east", "south", "west"};
    for (usize side = 0; side < 4; ++side) {
        torches_[side] = blocks.default_state(*torch);
        if (!facing) {
            continue;
        }
        for (usize i = 0; i < facing->values.size(); ++i) {
            if (facing->values[i] == kFacings[side]) {
                torches_[side] = blocks.with_property(torches_[side], *facing, static_cast<u16>(i));
            }
        }
    }
}

void EndPortalRules::build_exit_portal(world::LevelWriter& level, BlockPos origin,
                                       bool active) const {
    if (!valid_) {
        return;
    }
    for (i32 z = origin.z - 4; z <= origin.z + 4; ++z) {
        for (i32 y = origin.y - 1; y <= origin.y + 32; ++y) {
            for (i32 x = origin.x - 4; x <= origin.x + 4; ++x) {
                const f64 dx     = static_cast<f64>(x - origin.x);
                const f64 dy     = static_cast<f64>(y - origin.y);
                const f64 dz     = static_cast<f64>(z - origin.z);
                const f64 square = dx * dx + dy * dy + dz * dz;
                const bool inner = square < 2.5 * 2.5;
                if (!inner && !(square < 3.5 * 3.5)) {
                    continue;
                }
                const BlockPos at{x, y, z};
                if (y < origin.y) {
                    level.set_block(at, inner ? bedrock_ : end_stone_);
                } else if (y > origin.y) {
                    level.set_block(at, registry::kAirState);
                } else if (!inner) {
                    level.set_block(at, bedrock_);
                } else {
                    level.set_block(at, active ? portal_ : registry::kAirState);
                }
            }
        }
    }
    for (i32 i = 0; i < 4; ++i) {
        level.set_block(BlockPos{origin.x, origin.y + i, origin.z}, bedrock_);
    }
    // North, east, south, west of the pillar, each torch facing away from it.
    constexpr std::array<std::array<i32, 2>, 4> kPillarSides{{{0, -1}, {1, 0}, {0, 1}, {-1, 0}}};
    for (usize side = 0; side < 4; ++side) {
        level.set_block(BlockPos{origin.x + kPillarSides[side][0], origin.y + 2,
                                 origin.z + kPillarSides[side][1]},
                        torches_[side]);
    }
}

void EndPortalRules::build_gateway(world::LevelWriter& level, BlockPos pos) const {
    if (!valid_) {
        return;
    }
    for (i32 z = pos.z - 1; z <= pos.z + 1; ++z) {
        for (i32 y = pos.y - 2; y <= pos.y + 2; ++y) {
            for (i32 x = pos.x - 1; x <= pos.x + 1; ++x) {
                const bool same_x = x == pos.x;
                const bool same_y = y == pos.y;
                const bool same_z = z == pos.z;
                const bool ends   = std::abs(y - pos.y) == 2;
                registry::BlockStateId state = registry::kAirState;
                if (same_x && same_y && same_z) {
                    state = gateway_;
                } else if (same_y) {
                    state = registry::kAirState;
                } else if ((ends && same_x && same_z) || ((same_x || same_z) && !ends)) {
                    state = bedrock_;
                }
                level.set_block(BlockPos{x, y, z}, state);
            }
        }
    }
}

BlockPos EndPortalRules::exit_portal_origin(const world::LevelView& level, i32 top) const {
    BlockPos at{0, top - 1, 0};
    while (blocks_->block_of(level.block_at(at)) == blocks_->block_of(bedrock_) && at.y > 0) {
        --at.y;
    }
    return at;
}

std::array<i32, 20> end_gateway_indices(i64 seed) {
    std::array<i32, 20> order{};
    for (i32 i = 0; i < 20; ++i) {
        order[static_cast<usize>(i)] = i;
    }
    math::LegacyRandomSource random{seed};
    for (i32 i = 20; i > 1; --i) {
        const i32 j = random.next_int(i);
        std::swap(order[static_cast<usize>(i - 1)], order[static_cast<usize>(j)]);
    }
    return order;
}

BlockPos end_gateway_position(i32 index) {
    const f64 angle = 2.0 * (-std::numbers::pi + 0.15707963267948966 * static_cast<f64>(index));
    const f64 x     = 96.0 * std::cos(angle);
    const f64 z     = 96.0 * std::sin(angle);
    return BlockPos{static_cast<i32>(std::floor(x)), 75, static_cast<i32>(std::floor(z))};
}

std::array<BlockPos, 20> end_gateway_order(i64 seed) {
    const std::array<i32, 20> order = end_gateway_indices(seed);
    std::array<BlockPos, 20>  positions{};
    for (usize taken = 0; taken < 20; ++taken) {
        // From the back of the list.
        positions[taken] = end_gateway_position(order[19 - taken]);
    }
    return positions;
}

bool EndPortalRules::has_eye(registry::BlockStateId state) const noexcept {
    if (!is_frame(state)) {
        return false;
    }
    const auto eye = blocks_->find_property(frame_block_, "eye");
    return eye && blocks_->property_value(state, *eye) == "true";
}

bool EndPortalRules::frame_faces(registry::BlockStateId state, std::string_view facing) const {
    if (!is_frame(state)) {
        return false;
    }
    const auto property = blocks_->find_property(frame_block_, "facing");
    return property && blocks_->property_value(state, *property) == facing;
}

registry::BlockStateId EndPortalRules::frame_state(std::string_view facing, bool eye) const {
    auto       state    = frame_;
    const auto property = blocks_->find_property(frame_block_, "facing");
    const auto eyes     = blocks_->find_property(frame_block_, "eye");
    if (!property || !eyes) {
        return state;
    }
    for (usize i = 0; i < property->values.size(); ++i) {
        if (property->values[i] == facing) {
            state = blocks_->with_property(state, *property, static_cast<u16>(i));
        }
    }
    for (usize i = 0; i < eyes->values.size(); ++i) {
        if (eyes->values[i] == (eye ? "true" : "false")) {
            state = blocks_->with_property(state, *eyes, static_cast<u16>(i));
        }
    }
    return state;
}

std::optional<BlockPos> EndPortalRules::complete_ring(const world::LevelView& level,
                                                      BlockPos frame) const {
    if (!valid_) {
        return std::nullopt;
    }
    const auto here = level.block_at(frame);
    if (!has_eye(here)) {
        return std::nullopt;
    }
    // The frame's own facing says which side it is on; its place along that
    // side is one of three, so three centres are candidates.
    for (const Side& side : kSides) {
        if (!frame_faces(here, side.facing)) {
            continue;
        }
        for (i32 slot = -1; slot <= 1; ++slot) {
            const BlockPos centre{frame.x - side.dx - side.along_x * slot, frame.y,
                                  frame.z - side.dz - side.along_z * slot};
            bool whole = true;
            for (const Side& check : kSides) {
                for (i32 k = -1; k <= 1 && whole; ++k) {
                    const BlockPos at{centre.x + check.dx + check.along_x * k, centre.y,
                                      centre.z + check.dz + check.along_z * k};
                    const auto state = level.block_at(at);
                    whole = has_eye(state) && frame_faces(state, check.facing);
                }
            }
            if (whole) {
                return centre;
            }
        }
    }
    return std::nullopt;
}

void EndPortalRules::open(world::LevelWriter& level, BlockPos centre) const {
    for (i32 dx = -1; dx <= 1; ++dx) {
        for (i32 dz = -1; dz <= 1; ++dz) {
            level.set_block(BlockPos{centre.x + dx, centre.y, centre.z + dz}, portal_);
        }
    }
}

EyeOutcome EndPortalRules::use_eye(world::LevelWriter& level, BlockPos pos) const {
    EyeOutcome outcome;
    if (!valid_) {
        return outcome;
    }
    const auto state = level.block_at(pos);
    if (!is_frame(state) || has_eye(state)) {
        return outcome;
    }
    const auto eye = blocks_->find_property(frame_block_, "eye");
    for (usize i = 0; i < eye->values.size(); ++i) {
        if (eye->values[i] == "true") {
            level.set_block(pos, blocks_->with_property(state, *eye, static_cast<u16>(i)));
        }
    }
    outcome.result = EyeUse::Inserted;
    if (const auto centre = complete_ring(level, pos)) {
        open(level, *centre);
        outcome.result        = EyeUse::Activated;
        outcome.portal_centre = *centre;
    }
    return outcome;
}

bool EndPortalRules::box_in_portal(const world::LevelView& level, const AABB& box) const {
    if (!valid_) {
        return false;
    }
    const i32 x0 = static_cast<i32>(std::floor(box.min.x));
    const i32 x1 = static_cast<i32>(std::floor(box.max.x));
    const i32 y0 = static_cast<i32>(std::floor(box.min.y));
    const i32 y1 = static_cast<i32>(std::floor(box.max.y));
    const i32 z0 = static_cast<i32>(std::floor(box.min.z));
    const i32 z1 = static_cast<i32>(std::floor(box.max.z));
    for (i32 x = x0; x <= x1; ++x) {
        for (i32 z = z0; z <= z1; ++z) {
            for (i32 y = y0; y <= y1; ++y) {
                if (!level.shape().contains_y(y) || !is_portal(level.block_at(BlockPos{x, y, z}))) {
                    continue;
                }
                // Strict overlap with the slab, on every axis.
                const f64 low  = static_cast<f64>(y) + kEndPortalShapeLow;
                const f64 high = static_cast<f64>(y) + kEndPortalShapeHigh;
                if (box.max.y > low && box.min.y < high && box.max.x > static_cast<f64>(x) &&
                    box.min.x < static_cast<f64>(x + 1) && box.max.z > static_cast<f64>(z) &&
                    box.min.z < static_cast<f64>(z + 1)) {
                    return true;
                }
            }
        }
    }
    return false;
}

void EndPortalRules::build_platform(world::LevelWriter& level, BlockPos spawn) const {
    // Obsidian under the spawn point, air in it and on the two layers above.
    // Measured on the real server: 25 obsidian at y = 48, the player arriving
    // at y = 49.
    for (i32 dz = -2; dz <= 2; ++dz) {
        for (i32 dx = -2; dx <= 2; ++dx) {
            for (i32 dy = -1; dy <= 2; ++dy) {
                level.set_block(BlockPos{spawn.x + dx, spawn.y + dy, spawn.z + dz},
                                dy == -1 ? obsidian_ : registry::kAirState);
            }
        }
    }
}

}  // namespace ov::gameplay
