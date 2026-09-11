#include "ov/gameplay/rails.hpp"

#include <algorithm>

namespace ov::gameplay {

namespace {

struct Step {
    i32 dx;
    i32 dz;
};

constexpr Step kNorth{0, -1};
constexpr Step kSouth{0, 1};
constexpr Step kWest{-1, 0};
constexpr Step kEast{1, 0};

[[nodiscard]] constexpr bool north_south_axis(RailShape shape) noexcept {
    return shape == RailShape::NorthSouth || shape == RailShape::AscendingNorth ||
           shape == RailShape::AscendingSouth;
}

[[nodiscard]] constexpr bool east_west_axis(RailShape shape) noexcept {
    return shape == RailShape::EastWest || shape == RailShape::AscendingEast ||
           shape == RailShape::AscendingWest;
}

/// The shape joining two horizontal directions, or nothing when they are the
/// same direction.
[[nodiscard]] std::optional<RailShape> joining(Step a, Step b, bool curves) noexcept {
    const bool n = (a.dz < 0) || (b.dz < 0);
    const bool s = (a.dz > 0) || (b.dz > 0);
    const bool w = (a.dx < 0) || (b.dx < 0);
    const bool e = (a.dx > 0) || (b.dx > 0);
    if ((n || s) && !w && !e) {
        return RailShape::NorthSouth;
    }
    if ((w || e) && !n && !s) {
        return RailShape::EastWest;
    }
    if (!curves) {
        return std::nullopt;
    }
    if (s && e) {
        return RailShape::SouthEast;
    }
    if (s && w) {
        return RailShape::SouthWest;
    }
    if (n && w) {
        return RailShape::NorthWest;
    }
    if (n && e) {
        return RailShape::NorthEast;
    }
    return std::nullopt;
}

}  // namespace

RailEnds rail_ends(RailShape shape) noexcept {
    switch (shape) {
        case RailShape::NorthSouth: return {{0, -1, false}, {0, 1, false}};
        case RailShape::EastWest: return {{-1, 0, false}, {1, 0, false}};
        case RailShape::AscendingEast: return {{-1, 0, false}, {1, 0, true}};
        case RailShape::AscendingWest: return {{-1, 0, true}, {1, 0, false}};
        case RailShape::AscendingNorth: return {{0, -1, true}, {0, 1, false}};
        case RailShape::AscendingSouth: return {{0, -1, false}, {0, 1, true}};
        case RailShape::SouthEast: return {{0, 1, false}, {1, 0, false}};
        case RailShape::SouthWest: return {{0, 1, false}, {-1, 0, false}};
        case RailShape::NorthWest: return {{0, -1, false}, {-1, 0, false}};
        case RailShape::NorthEast: return {{0, -1, false}, {1, 0, false}};
    }
    return {{0, -1, false}, {0, 1, false}};
}

Rails::Rails(const registry::BlockRegistry& blocks, const Signals& signals)
    : blocks_{&blocks}, signals_{&signals} {
    const auto resolve = [&](std::string_view name) {
        const auto id = blocks.find_block(name);
        return id ? *id : registry::BlockId{0};
    };
    plain_     = resolve("minecraft:rail");
    powered_   = resolve("minecraft:powered_rail");
    detector_  = resolve("minecraft:detector_rail");
    activator_ = resolve("minecraft:activator_rail");
    water_     = resolve("minecraft:water");
}

RailKind Rails::kind_of_block(registry::BlockId block) const noexcept {
    if (block.value() == 0) {
        return RailKind::None;
    }
    if (block == plain_) {
        return RailKind::Plain;
    }
    if (block == powered_) {
        return RailKind::Powered;
    }
    if (block == detector_) {
        return RailKind::Detector;
    }
    if (block == activator_) {
        return RailKind::Activator;
    }
    return RailKind::None;
}

RailKind Rails::kind_of(registry::BlockStateId state) const noexcept {
    return kind_of_block(blocks_->block_of(state));
}

std::optional<RailShape> Rails::shape_of(registry::BlockStateId state) const noexcept {
    if (!is_rail(state)) {
        return std::nullopt;
    }
    const auto property = blocks_->find_property(blocks_->block_of(state), "shape");
    if (!property) {
        return std::nullopt;
    }
    const std::string_view value = blocks_->property_value(state, *property);
    for (usize i = 0; i < kRailShapeNames.size(); ++i) {
        if (kRailShapeNames[i] == value) {
            return static_cast<RailShape>(i);
        }
    }
    return std::nullopt;
}

registry::BlockStateId Rails::with_shape(registry::BlockStateId state,
                                         RailShape              shape) const noexcept {
    const auto property = blocks_->find_property(blocks_->block_of(state), "shape");
    if (!property) {
        return state;
    }
    const std::string_view wanted = kRailShapeNames[static_cast<usize>(shape)];
    for (u16 i = 0; i < property->values.size(); ++i) {
        if (property->values[i] == wanted) {
            return blocks_->with_property(state, *property, i);
        }
    }
    // A curve asked of a straight-only rail: nothing to write.
    return state;
}

bool Rails::powered(registry::BlockStateId state) const noexcept {
    return signals_->flag_of(state, "powered");
}

registry::BlockStateId Rails::with_powered(registry::BlockStateId state, bool on) const noexcept {
    return signals_->with_flag(state, "powered", on);
}

bool Rails::waterlogged(registry::BlockStateId state) const noexcept {
    return signals_->flag_of(state, "waterlogged");
}

registry::BlockStateId Rails::placement_state(const world::LevelView& level, BlockPos pos,
                                              registry::BlockStateId state,
                                              bool facing_east_west) const noexcept {
    registry::BlockStateId out =
        with_shape(state, facing_east_west ? RailShape::EastWest : RailShape::NorthSouth);
    const registry::BlockStateId there = level.block_at(pos);
    if (blocks_->block_of(there) == water_ && water_.value() != 0) {
        const auto level_property = blocks_->find_property(water_, "level");
        if (level_property && blocks_->property_value(there, *level_property) == "0") {
            out = signals_->with_flag(out, "waterlogged", true);
        }
    }
    return out;
}

bool Rails::rigid_top(registry::BlockStateId state) const noexcept {
    // The top face rasterised on the 32×32 grid the shapes use, one bit per
    // cell. Only boxes that reach the top of the cell count.
    std::array<u32, 32> rows{};
    for (const registry::BlockRegistry::Box& box : blocks_->collision_boxes(state)) {
        if (box.max_y != 32) {
            continue;
        }
        const i32 x0 = std::max<i32>(box.min_x, 0);
        const i32 x1 = std::min<i32>(box.max_x, 32);
        const i32 z0 = std::max<i32>(box.min_z, 0);
        const i32 z1 = std::min<i32>(box.max_z, 32);
        if (x0 >= x1 || z0 >= z1) {
            continue;
        }
        const u64 wide = ((u64{1} << static_cast<u32>(x1)) - 1U) & ~((u64{1} << x0) - 1U);
        for (i32 z = z0; z < z1; ++z) {
            rows[static_cast<usize>(z)] |= static_cast<u32>(wide);
        }
    }
    // The rim: two sixteenths, four thirty-seconds, all round.
    constexpr u32 kRing = 0x0000000FU | 0xF0000000U;
    for (usize z = 0; z < rows.size(); ++z) {
        const u32 required = (z < 4 || z >= 28) ? 0xFFFFFFFFU : kRing;
        if ((rows[z] & required) != required) {
            return false;
        }
    }
    return true;
}

bool Rails::supported(const world::LevelView& level, BlockPos pos,
                      registry::BlockStateId state) const noexcept {
    if (!rigid_top(level.block_at(pos.below()))) {
        return false;
    }
    const auto shape = shape_of(state);
    if (shape && is_ascending(*shape)) {
        const RailEnds ends = rail_ends(*shape);
        const RailEnd& up   = ends.first.rises ? ends.first : ends.second;
        if (!rigid_top(level.block_at(pos.offset(up.dx, 0, up.dz)))) {
            return false;
        }
    }
    return true;
}

Rails::Found Rails::find_rail(const world::LevelView& level, BlockPos pos, i32 dx,
                              i32 dz) const noexcept {
    const BlockPos side = pos.offset(dx, 0, dz);
    for (const BlockPos candidate : {side, side.above(), side.below()}) {
        if (is_rail(level.block_at(candidate))) {
            return Found{true, candidate};
        }
    }
    return Found{};
}

i32 Rails::connection_count(const world::LevelView& level, BlockPos pos,
                            registry::BlockStateId state) const noexcept {
    const auto shape = shape_of(state);
    if (!shape) {
        return 0;
    }
    const RailEnds ends = rail_ends(*shape);
    i32            count = 0;
    for (const RailEnd& end : {ends.first, ends.second}) {
        if (find_rail(level, pos, end.dx, end.dz).present) {
            ++count;
        }
    }
    return count;
}

bool Rails::connects_towards(const world::LevelView& level, BlockPos pos,
                             registry::BlockStateId state, i32 dx, i32 dz) const noexcept {
    const auto shape = shape_of(state);
    if (!shape) {
        return false;
    }
    const RailEnds ends = rail_ends(*shape);
    for (const RailEnd& end : {ends.first, ends.second}) {
        if (end.dx == dx && end.dz == dz) {
            return find_rail(level, pos, dx, dz).present;
        }
    }
    return false;
}

bool Rails::can_take(const world::LevelView& level, BlockPos neighbour, i32 dx_to_us,
                     i32 dz_to_us) const noexcept {
    const registry::BlockStateId state = level.block_at(neighbour);
    if (!is_rail(state)) {
        return false;
    }
    return connects_towards(level, neighbour, state, dx_to_us, dz_to_us) ||
           connection_count(level, neighbour, state) < 2;
}

RailShape Rails::choose_shape(const RedstoneWorld& world, BlockPos pos,
                              registry::BlockStateId state, bool powered_now) const {
    const RailKind kind     = kind_of(state);
    const bool     straight = straight_only(kind);
    const RailShape current = shape_of(state).value_or(RailShape::NorthSouth);

    const auto look = [&](Step step, bool& connects, bool& rises) {
        const Found found = find_rail(world, pos, step.dx, step.dz);
        connects          = found.present && can_take(world, found.pos, -step.dx, -step.dz);
        rises             = connects && found.pos.y == pos.y + 1;
    };
    bool n = false;
    bool s = false;
    bool w = false;
    bool e = false;
    bool up_n = false;
    bool up_s = false;
    bool up_w = false;
    bool up_e = false;
    look(kNorth, n, up_n);
    look(kSouth, s, up_s);
    look(kWest, w, up_w);
    look(kEast, e, up_e);

    std::optional<RailShape> shape;
    if ((n || s) && !w && !e) {
        shape = RailShape::NorthSouth;
    }
    if ((w || e) && !n && !s) {
        shape = RailShape::EastWest;
    }
    if (!straight) {
        if (s && e && !n && !w) {
            shape = RailShape::SouthEast;
        }
        if (s && w && !n && !e) {
            shape = RailShape::SouthWest;
        }
        if (n && w && !s && !e) {
            shape = RailShape::NorthWest;
        }
        if (n && e && !s && !w) {
            shape = RailShape::NorthEast;
        }
    }
    if (!shape && (n || s || w || e)) {
        if (straight) {
            // Both axes offered: the axis the rail already had. Measured —
            // 225 of the 256 arrangements a powered rail set north-south
            // differ from "east-west wins", and none from this.
            shape = north_south_axis(current) ? RailShape::NorthSouth : RailShape::EastWest;
        } else {
            // Three or four ends offered. The later line wins: south-east
            // unpowered, measured in every arrangement; north-west powered,
            // the wiki's "a rail's preference inverts", not measured.
            if (powered_now) {
                if (s && e) {
                    shape = RailShape::SouthEast;
                }
                if (w && s) {
                    shape = RailShape::SouthWest;
                }
                if (e && n) {
                    shape = RailShape::NorthEast;
                }
                if (n && w) {
                    shape = RailShape::NorthWest;
                }
            } else {
                if (n && w) {
                    shape = RailShape::NorthWest;
                }
                if (e && n) {
                    shape = RailShape::NorthEast;
                }
                if (w && s) {
                    shape = RailShape::SouthWest;
                }
                if (s && e) {
                    shape = RailShape::SouthEast;
                }
            }
        }
    }
    if (shape == RailShape::NorthSouth) {
        if (up_n) {
            shape = RailShape::AscendingNorth;
        }
        if (up_s) {
            shape = RailShape::AscendingSouth;
        }
    } else if (shape == RailShape::EastWest) {
        if (up_e) {
            shape = RailShape::AscendingEast;
        }
        if (up_w) {
            shape = RailShape::AscendingWest;
        }
    }
    return shape.value_or(current);
}

void Rails::connect_to(RedstoneWorld& world, BlockPos neighbour, BlockPos us) const {
    const registry::BlockStateId state = world.block_at(neighbour);
    const auto                   shape = shape_of(state);
    if (!shape) {
        return;
    }
    const Step toward{us.x > neighbour.x ? 1 : (us.x < neighbour.x ? -1 : 0),
                      us.z > neighbour.z ? 1 : (us.z < neighbour.z ? -1 : 0)};
    // Even a rail already pointing this way is recomputed: a lone rail one
    // block below the new one points at it and must still rise to it
    // (measured: 468 neighbour cells of the placement table).
    // The end it keeps: the other one of its ends that still leads to a rail.
    std::optional<Step> kept;
    const RailEnds      ends = rail_ends(*shape);
    for (const RailEnd& end : {ends.first, ends.second}) {
        if (end.dx == toward.dx && end.dz == toward.dz) {
            continue;
        }
        if (find_rail(world, neighbour, end.dx, end.dz).present) {
            kept = Step{end.dx, end.dz};
            break;
        }
    }
    const bool curves = !straight_only(kind_of(state));
    std::optional<RailShape> next;
    if (kept) {
        next = joining(*kept, toward, curves);
    }
    if (!next) {
        // Alone, or a straight-only rail offered a corner: straight towards
        // the new rail. The second case is not measured.
        next = toward.dx != 0 ? RailShape::EastWest : RailShape::NorthSouth;
    }
    // Rise towards a connected rail one block up.
    const auto rises = [&](Step step) {
        const bool connected = (step.dx == toward.dx && step.dz == toward.dz) ||
                               (kept && step.dx == kept->dx && step.dz == kept->dz);
        if (!connected) {
            return false;
        }
        const Found found = find_rail(world, neighbour, step.dx, step.dz);
        return found.present && found.pos.y == neighbour.y + 1;
    };
    if (*next == RailShape::NorthSouth) {
        if (rises(kNorth)) {
            next = RailShape::AscendingNorth;
        }
        if (rises(kSouth)) {
            next = RailShape::AscendingSouth;
        }
    } else if (*next == RailShape::EastWest) {
        if (rises(kEast)) {
            next = RailShape::AscendingEast;
        }
        if (rises(kWest)) {
            next = RailShape::AscendingWest;
        }
    }
    const registry::BlockStateId after = with_shape(state, *next);
    if (after != state) {
        world.set_block(neighbour, after);
    }
}

void Rails::apply_shape(RedstoneWorld& world, BlockPos pos, registry::BlockStateId state,
                        RailShape shape, bool force_write) const {
    const registry::BlockStateId after = with_shape(state, shape);
    if (after != state || force_write) {
        world.set_block(pos, after);
    }
    const auto written = shape_of(after);
    if (!written) {
        return;
    }
    const RailEnds ends = rail_ends(*written);
    for (const RailEnd& end : {ends.first, ends.second}) {
        const Found found = find_rail(world, pos, end.dx, end.dz);
        if (found.present && can_take(world, found.pos, -end.dx, -end.dz)) {
            connect_to(world, found.pos, pos);
        }
    }
}

void Rails::on_placed(RedstoneWorld& world, BlockPos pos) const {
    const registry::BlockStateId state = world.block_at(pos);
    const RailKind               kind  = kind_of(state);
    if (kind == RailKind::None) {
        return;
    }
    const bool signal = kind == RailKind::Plain && signals_->has_neighbour_signal(world, pos);
    apply_shape(world, pos, state, choose_shape(world, pos, state, signal), false);
    if (kind == RailKind::Powered || kind == RailKind::Activator) {
        const registry::BlockStateId now  = world.block_at(pos);
        const bool                   want = wants_power(world, pos, now);
        if (want != powered(now)) {
            world.set_block(pos, with_powered(now, want));
        }
    }
}

bool Rails::beside_component(const RedstoneWorld& world, BlockPos pos) const {
    for (u8 i = 0; i < kDirectionCount; ++i) {
        const registry::BlockStateId there = world.block_at(pos.offset(static_cast<Direction>(i)));
        const registry::BlockId      block = blocks_->block_of(there);
        if (block == signals_->wire_block() || signals_->kind_of(block) != SignalKind::None) {
            return true;
        }
    }
    return false;
}

RailUpdate Rails::neighbour_changed(RedstoneWorld& world, BlockPos pos) const {
    RailUpdate                   update;
    const registry::BlockStateId state = world.block_at(pos);
    const RailKind               kind  = kind_of(state);
    if (kind == RailKind::None) {
        return update;
    }
    if (!supported(world, pos, state)) {
        const registry::BlockStateId left =
            waterlogged(state) ? blocks_->default_state(water_) : registry::kAirState;
        world.set_block(pos, left);
        update.broke = true;
        update.was   = state;
        return update;
    }
    switch (kind) {
        case RailKind::Plain: {
            i32 around = 0;
            for (const Step step : {kNorth, kSouth, kWest, kEast}) {
                around += find_rail(world, pos, step.dx, step.dz).present ? 1 : 0;
            }
            if (around == 3 && beside_component(world, pos)) {
                const bool      signal = signals_->has_neighbour_signal(world, pos);
                const RailShape shape  = choose_shape(world, pos, state, signal);
                if (shape_of(state) != shape) {
                    apply_shape(world, pos, state, shape, false);
                    update.reshaped = true;
                }
            }
            break;
        }
        case RailKind::Powered:
        case RailKind::Activator: {
            const bool want = wants_power(world, pos, state);
            if (want != powered(state)) {
                world.set_block(pos, with_powered(state, want));
                update.power_changed = true;
            }
            break;
        }
        case RailKind::Detector:
        case RailKind::None: break;
    }
    return update;
}

bool Rails::line_powered(const RedstoneWorld& world, BlockPos pos, RailKind kind, i32 dx, i32 dz,
                         i32 depth) const {
    if (depth >= kPowerReach) {
        return false;
    }
    const auto shape = shape_of(world.block_at(pos));
    if (!shape) {
        return false;
    }
    bool           rises = false;
    const RailEnds ends  = rail_ends(*shape);
    for (const RailEnd& end : {ends.first, ends.second}) {
        if (end.dx == dx && end.dz == dz) {
            rises = end.rises;
        }
    }
    const BlockPos level_step = pos.offset(dx, rises ? 1 : 0, dz);
    const BlockPos lower_step = level_step.below();
    const usize    tries      = rises ? 1 : 2;
    for (usize t = 0; t < tries; ++t) {
        const BlockPos               next  = t == 0 ? level_step : lower_step;
        const registry::BlockStateId there = world.block_at(next);
        if (kind_of(there) != kind) {
            continue;
        }
        const auto next_shape = shape_of(there);
        if (!next_shape) {
            continue;
        }
        // A line along x does not continue into a rail along z, or back.
        if (dx != 0 ? !east_west_axis(*next_shape) : !north_south_axis(*next_shape)) {
            continue;
        }
        if (!powered(there)) {
            continue;
        }
        if (signals_->has_neighbour_signal(world, next) ||
            line_powered(world, next, kind, dx, dz, depth + 1)) {
            return true;
        }
    }
    return false;
}

bool Rails::wants_power(const RedstoneWorld& world, BlockPos pos,
                        registry::BlockStateId state) const {
    if (signals_->has_neighbour_signal(world, pos)) {
        return true;
    }
    const auto shape = shape_of(state);
    if (!shape) {
        return false;
    }
    const RailKind kind = kind_of(state);
    const RailEnds ends = rail_ends(*shape);
    return line_powered(world, pos, kind, ends.first.dx, ends.first.dz, 0) ||
           line_powered(world, pos, kind, ends.second.dx, ends.second.dz, 0);
}

bool Rails::set_detector(RedstoneWorld& world, BlockPos pos, bool occupied) const {
    const registry::BlockStateId state = world.block_at(pos);
    if (kind_of(state) != RailKind::Detector || powered(state) == occupied) {
        return false;
    }
    world.set_block(pos, with_powered(state, occupied));
    return true;
}

}  // namespace ov::gameplay
