#include "ov/gameplay/nether_portal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string_view>

namespace ov::gameplay {

namespace {

/// One step along the portal's plane, in the direction the frame is measured.
[[nodiscard]] constexpr BlockPos along(PortalAxis axis, i32 steps) noexcept {
    return axis == PortalAxis::X ? BlockPos{steps, 0, 0} : BlockPos{0, 0, steps};
}

/// One step across the plane — the portal's thickness.
[[nodiscard]] constexpr BlockPos across(PortalAxis axis, i32 steps) noexcept {
    return axis == PortalAxis::X ? BlockPos{0, 0, steps} : BlockPos{steps, 0, 0};
}

[[nodiscard]] constexpr BlockPos plus(BlockPos a, BlockPos b) noexcept {
    return BlockPos{a.x + b.x, a.y + b.y, a.z + b.z};
}

[[nodiscard]] constexpr BlockPos up(BlockPos a, i32 steps) noexcept {
    return BlockPos{a.x, a.y + steps, a.z};
}

/// The world border's reach, in blocks. `WorldBorder.clampToBounds` keeps a
/// position one short of the 30 000 000 edge on the positive side.
constexpr f64 kBorderMax = 29'999'983.0;
constexpr f64 kBorderMin = -29'999'984.0;

[[nodiscard]] i32 floor_to_int(f64 value) noexcept {
    return static_cast<i32>(std::floor(value));
}

}  // namespace

PortalRules::PortalRules(const registry::BlockRegistry& blocks) : blocks_(&blocks) {
    const auto find = [&](std::string_view name) {
        return blocks.find_block(name).value_or(registry::BlockId{});
    };
    obsidian_block_  = find("minecraft:obsidian");
    portal_block_    = find("minecraft:nether_portal");
    fire_block_      = find("minecraft:fire");
    soul_fire_block_ = find("minecraft:soul_fire");
    obsidian_        = blocks.default_state(obsidian_block_);

    const registry::BlockStateId portal = blocks.default_state(portal_block_);
    portal_x_                           = portal;
    portal_z_                           = portal;
    if (const auto axis = blocks.find_property(portal_block_, "axis")) {
        for (u16 index = 0; index < axis->values.size(); ++index) {
            if (axis->values[index] == "x") {
                portal_x_ = blocks.with_property(portal, *axis, index);
            } else if (axis->values[index] == "z") {
                portal_z_ = blocks.with_property(portal, *axis, index);
            }
        }
    }
}

PortalRules::PortalRules(const registry::BlockRegistry& blocks,
                         const registry::Registries&    registries)
    : PortalRules(blocks) {
    replaceable_.assign(blocks.block_count(), false);
    const auto block_registry = registries.find("minecraft:block");
    if (!block_registry) {
        return;
    }
    const auto tag = registries.find_tag(*block_registry, "minecraft:replaceable");
    if (!tag) {
        return;
    }
    for (const registry::ProtocolId id : registries.tag_members(*tag)) {
        if (const auto block = blocks.find_block(registries.entry_of(*block_registry, id))) {
            replaceable_[static_cast<usize>(block->value())] = true;
        }
    }
}

bool PortalRules::is_empty(registry::BlockStateId state) const noexcept {
    if (state == registry::kAirState) {
        return true;
    }
    const registry::BlockId block = blocks_->block_of(state);
    return blocks_->is_air(block) || block == fire_block_ || block == soul_fire_block_ ||
           block == portal_block_;
}

std::optional<PortalAxis> PortalRules::axis_of(registry::BlockStateId state) const noexcept {
    if (!is_portal(state)) {
        return std::nullopt;
    }
    return state == portal_z_ ? PortalAxis::Z : PortalAxis::X;
}

std::optional<PortalFrame> PortalRules::frame_at(const world::LevelView& level, BlockPos inside,
                                                 PortalAxis axis) const {
    const auto shape = level.shape();
    if (!is_empty(level.block_at(inside))) {
        return std::nullopt;
    }

    // Down to the floor of the inside: at most the tallest frame's worth, and
    // the block under it must be obsidian.
    BlockPos bottom = inside;
    for (i32 steps = 0; steps < kPortalMaxHeight && bottom.y > shape.min_y &&
                        is_empty(level.block_at(up(bottom, -1)));
         ++steps) {
        bottom = up(bottom, -1);
    }
    if (!is_obsidian(level.block_at(up(bottom, -1)))) {
        return std::nullopt;
    }

    // Then to the negative end of that floor row. Every block of the row has
    // obsidian under it; the row ends at an obsidian side.
    const auto row_end = [&](BlockPos from, i32 direction) -> std::optional<i32> {
        for (i32 steps = 0; steps <= kPortalMaxWidth; ++steps) {
            const BlockPos at = plus(from, along(axis, direction * steps));
            const auto     here = level.block_at(at);
            if (!is_empty(here)) {
                if (is_obsidian(here) && steps > 0) {
                    return steps - 1;
                }
                return std::nullopt;
            }
            if (!is_obsidian(level.block_at(up(at, -1)))) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    };
    const auto back    = row_end(bottom, -1);
    const auto forward = row_end(bottom, +1);
    if (!back || !forward) {
        return std::nullopt;
    }
    PortalFrame frame;
    frame.axis        = axis;
    frame.bottom_left = plus(bottom, along(axis, -*back));
    frame.width       = *back + *forward + 1;
    if (frame.width < kPortalMinWidth || frame.width > kPortalMaxWidth) {
        return std::nullopt;
    }

    // Up, row by row, until the top row is all obsidian. Both sides of every
    // row are obsidian; the corners never matter.
    i32 height = 0;
    for (; height <= kPortalMaxHeight; ++height) {
        const BlockPos row = up(frame.bottom_left, height);
        if (row.y > shape.max_y()) {
            return std::nullopt;
        }
        bool lid = true;
        for (i32 i = 0; i < frame.width; ++i) {
            if (!is_obsidian(level.block_at(plus(row, along(axis, i))))) {
                lid = false;
                break;
            }
        }
        if (lid && height > 0) {
            break;
        }
        if (!is_obsidian(level.block_at(plus(row, along(axis, -1)))) ||
            !is_obsidian(level.block_at(plus(row, along(axis, frame.width))))) {
            return std::nullopt;
        }
        for (i32 i = 0; i < frame.width; ++i) {
            const auto state = level.block_at(plus(row, along(axis, i)));
            if (!is_empty(state)) {
                return std::nullopt;
            }
            if (is_portal(state)) {
                ++frame.portal_blocks;
            }
        }
    }
    frame.height = height;
    if (frame.height < kPortalMinHeight || frame.height > kPortalMaxHeight) {
        return std::nullopt;
    }
    return frame;
}

std::optional<PortalFrame> PortalRules::find_empty_frame(const world::LevelView& level,
                                                         BlockPos            inside,
                                                         PortalAxis          preferred) const {
    const PortalAxis other = preferred == PortalAxis::X ? PortalAxis::Z : PortalAxis::X;
    for (const PortalAxis axis : {preferred, other}) {
        if (auto frame = frame_at(level, inside, axis); frame && frame->portal_blocks == 0) {
            return frame;
        }
    }
    return std::nullopt;
}

void PortalRules::fill(world::LevelWriter& level, const PortalFrame& frame) const {
    const registry::BlockStateId portal = portal_state(frame.axis);
    for (i32 y = 0; y < frame.height; ++y) {
        for (i32 i = 0; i < frame.width; ++i) {
            level.set_block(plus(up(frame.bottom_left, y), along(frame.axis, i)), portal);
        }
    }
}

bool PortalRules::light(world::LevelWriter& level, BlockPos pos) const {
    const auto frame = find_empty_frame(level, pos, PortalAxis::X);
    if (!frame) {
        return false;
    }
    fill(level, *frame);
    return true;
}

usize PortalRules::on_neighbour_changed(world::LevelWriter& level, BlockPos pos) const {
    if (is_portal(level.block_at(pos))) {
        return 0;
    }
    constexpr std::array<BlockPos, 6> kSides{BlockPos{1, 0, 0},  BlockPos{-1, 0, 0},
                                             BlockPos{0, 1, 0},  BlockPos{0, -1, 0},
                                             BlockPos{0, 0, 1},  BlockPos{0, 0, -1}};
    usize removed = 0;
    for (const BlockPos side : kSides) {
        const BlockPos neighbour = plus(pos, side);
        const auto     state     = level.block_at(neighbour);
        const auto     axis      = axis_of(state);
        if (!axis) {
            continue;
        }
        // Only a change in the portal's own plane reaches it: vertically, or
        // along its axis. A block placed against its face does nothing.
        const bool across_plane = side.y == 0 && ((*axis == PortalAxis::X && side.z != 0) ||
                                                  (*axis == PortalAxis::Z && side.x != 0));
        if (across_plane) {
            continue;
        }
        if (const auto frame = frame_at(level, neighbour, *axis); frame && frame->complete()) {
            continue;
        }
        // The cascade: every portal block joined to this one in its plane.
        std::vector<BlockPos> stack{neighbour};
        while (!stack.empty()) {
            const BlockPos at = stack.back();
            stack.pop_back();
            if (axis_of(level.block_at(at)) != axis) {
                continue;
            }
            level.set_block(at, registry::kAirState);
            ++removed;
            for (const BlockPos step : {up(at, 1), up(at, -1), plus(at, along(*axis, 1)),
                                        plus(at, along(*axis, -1))}) {
                stack.push_back(step);
            }
        }
    }
    return removed;
}

usize PortalRules::on_block_changed(world::LevelWriter& level, BlockPos pos) const {
    const auto              state = level.block_at(pos);
    const registry::BlockId block = blocks_->block_of(state);
    if (state != registry::kAirState && (block == fire_block_ || block == soul_fire_block_)) {
        if (const auto frame = find_empty_frame(level, pos, PortalAxis::X)) {
            fill(level, *frame);
            return static_cast<usize>(frame->width * frame->height);
        }
        return 0;
    }
    return on_neighbour_changed(level, pos);
}

std::optional<PortalRect> PortalRules::rectangle_at(const world::LevelView& level,
                                                    BlockPos block) const {
    const auto axis = axis_of(level.block_at(block));
    if (!axis) {
        return std::nullopt;
    }
    const auto same = [&](BlockPos at) { return axis_of(level.block_at(at)) == axis; };
    BlockPos   min = block;
    for (i32 i = 0; i < kPortalMaxHeight && same(up(min, -1)); ++i) {
        min = up(min, -1);
    }
    for (i32 i = 0; i < kPortalMaxWidth && same(plus(min, along(*axis, -1))); ++i) {
        min = plus(min, along(*axis, -1));
    }
    PortalRect rect;
    rect.min_corner = min;
    rect.axis       = *axis;
    rect.width      = 1;
    while (rect.width < kPortalMaxWidth && same(plus(min, along(*axis, rect.width)))) {
        ++rect.width;
    }
    rect.height = 1;
    while (rect.height < kPortalMaxHeight && same(up(min, rect.height))) {
        ++rect.height;
    }
    return rect;
}

bool PortalRules::can_replace(registry::BlockStateId state) const noexcept {
    if (state == registry::kAirState) {
        return true;
    }
    if (blocks_->holds_fluid(state)) {
        return false;
    }
    const registry::BlockId block = blocks_->block_of(state);
    if (!replaceable_.empty()) {
        const auto index = static_cast<usize>(block.value());
        return index < replaceable_.size() && replaceable_[index];
    }
    return blocks_->is_air(block) || block == fire_block_ || block == soul_fire_block_ ||
           blocks_->collision_boxes(state).empty();
}

bool PortalRules::is_solid(registry::BlockStateId state) const noexcept {
    return state != registry::kAirState && !blocks_->holds_fluid(state) &&
           !blocks_->collision_boxes(state).empty();
}

bool PortalRules::can_host(const world::LevelView& level, BlockPos origin, PortalAxis axis,
                           i32 offset) const {
    // Four along the axis (the two frame columns and the two inside ones),
    // solid underneath, and four of replaceable space from the floor up.
    for (i32 i = -1; i < 3; ++i) {
        for (i32 j = -1; j < 4; ++j) {
            const BlockPos at = plus(up(plus(origin, along(axis, i)), j), across(axis, offset));
            const auto     state = level.block_at(at);
            if (j < 0 ? !is_solid(state) : !can_replace(state)) {
                return false;
            }
        }
    }
    return true;
}

PortalRect PortalRules::create(world::LevelWriter& level, BlockPos target, PortalAxis axis,
                               i32 top) const {
    const auto shape = level.shape();

    // Every column within 16, from the highest it can be down to the floor of
    // the world. A candidate is the floor of a replaceable run: replaceable
    // here, not replaceable below.
    std::optional<BlockPos> best;
    std::optional<BlockPos> fallback;
    i64                     best_distance     = std::numeric_limits<i64>::max();
    i64                     fallback_distance = std::numeric_limits<i64>::max();

    // Rings of growing Chebyshev radius, so that of two equally close
    // candidates the one nearer the target column comes first. Within a ring
    // the order is x then z; that part is measured, not specified.
    for (i32 ring = 0; ring <= kCreateRadius; ++ring) {
        for (i32 dx = -ring; dx <= ring; ++dx) {
            for (i32 dz = -ring; dz <= ring; ++dz) {
                if (std::max(std::abs(dx), std::abs(dz)) != ring) {
                    continue;
                }
                const i32 x = target.x + dx;
                const i32 z = target.z + dz;
                for (i32 y = std::min(top, shape.max_y()); y > shape.min_y; --y) {
                    const BlockPos at{x, y, z};
                    if (!can_replace(level.block_at(at)) ||
                        can_replace(level.block_at(up(at, -1)))) {
                        continue;
                    }
                    if (y + 4 > top) {
                        continue;
                    }
                    if (!can_host(level, at, axis, 0)) {
                        continue;
                    }
                    const i64 ddx      = x - target.x;
                    const i64 ddy      = y - target.y;
                    const i64 ddz      = z - target.z;
                    const i64 distance = ddx * ddx + ddy * ddy + ddz * ddz;
                    if (can_host(level, at, axis, -1) && can_host(level, at, axis, 1)) {
                        if (distance < best_distance) {
                            best_distance = distance;
                            best          = at;
                        }
                    } else if (distance < fallback_distance) {
                        fallback_distance = distance;
                        fallback          = at;
                    }
                }
            }
        }
    }

    BlockPos origin{};
    if (best) {
        origin = *best;
    } else if (fallback) {
        origin = *fallback;
    } else {
        // Forced: at the target, y clamped to [70, top - 9], on a 2x3
        // platform of obsidian with three blocks of air above it.
        const i32 low  = std::max(shape.min_y + 1, 70);
        const i32 high = top - 9;
        origin         = BlockPos{target.x, std::clamp(target.y, low, std::max(low, high)), target.z};
        for (i32 i = -1; i < 2; ++i) {
            for (i32 j = 0; j < 2; ++j) {
                for (i32 k = -1; k < 3; ++k) {
                    const BlockPos at = up(plus(plus(origin, along(axis, j)), across(axis, i)), k);
                    level.set_block(at, k < 0 ? obsidian_ : registry::kAirState);
                }
            }
        }
    }

    // The frame, corners included, then the portal.
    for (i32 i = -1; i < 3; ++i) {
        for (i32 j = -1; j < 4; ++j) {
            if (i == -1 || i == 2 || j == -1 || j == 3) {
                level.set_block(up(plus(origin, along(axis, i)), j), obsidian_);
            }
        }
    }
    const registry::BlockStateId portal = portal_state(axis);
    for (i32 i = 0; i < 2; ++i) {
        for (i32 j = 0; j < 3; ++j) {
            level.set_block(up(plus(origin, along(axis, i)), j), portal);
        }
    }
    return PortalRect{origin, axis, 2, 3};
}

BlockPos scaled_target(Vec3d position, f64 scale) noexcept {
    const f64 x = std::clamp(position.x * scale, kBorderMin, kBorderMax);
    const f64 z = std::clamp(position.z * scale, kBorderMin, kBorderMax);
    return BlockPos{floor_to_int(x), floor_to_int(position.y), floor_to_int(z)};
}

std::optional<BlockPos> closest_portal(std::span<const BlockPos> candidates, BlockPos target,
                                       i32 radius) noexcept {
    std::optional<BlockPos> best;
    i64                     best_distance = 0;
    for (const BlockPos at : candidates) {
        if (std::abs(at.x - target.x) > radius || std::abs(at.z - target.z) > radius) {
            continue;
        }
        const i64 dx       = at.x - target.x;
        const i64 dy       = at.y - target.y;
        const i64 dz       = at.z - target.z;
        const i64 distance = dx * dx + dy * dy + dz * dz;
        if (!best || distance < best_distance ||
            (distance == best_distance && at.y < best->y)) {
            best          = at;
            best_distance = distance;
        }
    }
    return best;
}

Vec3d relative_position(const PortalRect& rect, Vec3d position, f64 width,
                        f64 height) noexcept {
    const bool x_axis     = rect.axis == PortalAxis::X;
    const f64  free_along = static_cast<f64>(rect.width) - width;
    const f64  free_up    = static_cast<f64>(rect.height) - height;
    const f64  min_along  = static_cast<f64>(x_axis ? rect.min_corner.x : rect.min_corner.z);
    const f64  min_across = static_cast<f64>(x_axis ? rect.min_corner.z : rect.min_corner.x);
    const f64  at_along   = x_axis ? position.x : position.z;
    const f64  at_across  = x_axis ? position.z : position.x;

    Vec3d relative{0.5, 0.0, 0.0};
    if (free_along > 0.0) {
        relative.x = std::clamp((at_along - (min_along + width / 2.0)) / free_along, 0.0, 1.0);
    }
    if (free_up > 0.0) {
        relative.y =
            std::clamp((position.y - static_cast<f64>(rect.min_corner.y)) / free_up, 0.0, 1.0);
    }
    relative.z = at_across - (min_across + 0.5);
    return relative;
}

Vec3d exit_position(const PortalRect& destination, PortalAxis source_axis, Vec3d relative,
                    f64 width, f64 height, f32& yaw) noexcept {
    if (destination.axis != source_axis) {
        yaw += 90.0F;
    }
    const f64  along  = width / 2.0 + (static_cast<f64>(destination.width) - width) * relative.x;
    const f64  rise   = (static_cast<f64>(destination.height) - height) * relative.y;
    const f64  across = 0.5 + relative.z;
    const bool x_axis = destination.axis == PortalAxis::X;
    return Vec3d{static_cast<f64>(destination.min_corner.x) + (x_axis ? along : across),
                 static_cast<f64>(destination.min_corner.y) + rise,
                 static_cast<f64>(destination.min_corner.z) + (x_axis ? across : along)};
}

}  // namespace ov::gameplay
