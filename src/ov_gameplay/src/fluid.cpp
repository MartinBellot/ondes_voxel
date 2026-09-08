#include "ov/gameplay/fluid.hpp"

#include <algorithm>
#include <array>
#include <cstddef>

namespace ov::gameplay {
namespace {

/// The hole search's working grid.
///
/// The search never leaves a square of side 2*5+1, so the whole thing fits in a
/// fixed array and the tick allocates nothing. Indices are (dx + R, dz + R).
constexpr i32   kR    = FluidConstants::kSearchRadius;
constexpr usize kSide = static_cast<usize>(2 * kR + 1);
constexpr usize kCells = kSide * kSide;
constexpr i32   kUnreached = 0x7FFF;

[[nodiscard]] constexpr usize cell_index(i32 dx, i32 dz) noexcept {
    return static_cast<usize>(dz + kR) * kSide + static_cast<usize>(dx + kR);
}

}  // namespace

FluidRules::FluidRules(const registry::BlockRegistry& blocks) : blocks_{&blocks} {
    const auto resolve = [&](std::string_view name) -> registry::BlockStateId {
        const auto block = blocks.find_block(name);
        // A missing block is refused loudly rather than silently defaulted to
        // air: a pack without stone would otherwise make every lava/water
        // meeting quietly vanish, and nothing would say why.
        return block ? blocks.default_state(*block) : registry::BlockStateId{0};
    };

    const auto water = blocks.find_block("minecraft:water");
    const auto lava  = blocks.find_block("minecraft:lava");
    if (water) {
        water_ = *water;
        if (const auto property = blocks.find_property(water_, "level")) {
            water_level_ = *property;
        }
    }
    if (lava) {
        lava_ = *lava;
        if (const auto property = blocks.find_property(lava_, "level")) {
            lava_level_ = *property;
        }
    }

    air_         = registry::BlockStateId{0};
    stone_       = resolve("minecraft:stone");
    cobblestone_ = resolve("minecraft:cobblestone");
    obsidian_    = resolve("minecraft:obsidian");

    // The waterlogged stride per block, resolved once. Zero means "this block
    // does not waterlog", which is safe because a real stride is never zero.
    waterlogged_stride_.assign(blocks.block_count(), 0);
    for (usize i = 0; i < blocks.block_count(); ++i) {
        const registry::BlockId block{static_cast<u16>(i)};
        if (const auto property = blocks.find_property(block, "waterlogged")) {
            waterlogged_stride_[i] = property->stride;
        }
    }
}

FluidState FluidRules::fluid_at(registry::BlockStateId state) const noexcept {
    if (blocks_ == nullptr || !blocks_->is_valid(state)) {
        return {};
    }
    const registry::BlockId block = blocks_->block_of(state);

    if (block == water_ || block == lava_) {
        const registry::PropertyView& property = block == water_ ? water_level_ : lava_level_;
        const u16 level = blocks_->property_index(state, property);
        // Disk stores `level = falling ? 8 : amount`. Anything at or above 8 is
        // falling; below it, the value is the amount itself.
        if (level >= 8) {
            return FluidState{block == water_ ? FluidKind::Water : FluidKind::Lava, 0, true};
        }
        return FluidState{block == water_ ? FluidKind::Water : FluidKind::Lava,
                          static_cast<u8>(level), false};
    }

    // A waterlogged block holds a water **source**. Measured: a waterlogged
    // slab alone on a floor grows the same diamond a source does.
    const u16 stride = waterlogged_stride_[block.value()];
    if (stride != 0) {
        const auto property = blocks_->find_property(block, "waterlogged");
        if (property && blocks_->property_value(state, *property) == "true") {
            return FluidState{FluidKind::Water, 0, false};
        }
    }
    return {};
}

registry::BlockStateId FluidRules::state_for(FluidState fluid) const noexcept {
    if (blocks_ == nullptr || fluid.empty()) {
        return air_;
    }
    const registry::BlockId       block    = fluid.kind == FluidKind::Water ? water_ : lava_;
    const registry::PropertyView& property = fluid.kind == FluidKind::Water ? water_level_
                                                                            : lava_level_;
    const u16 level = fluid.falling ? 8 : fluid.amount;
    return blocks_->with_property(blocks_->default_state(block), property, level);
}

i32 FluidRules::tick_delay(FluidKind kind, world::DimensionTraits traits) const noexcept {
    if (kind == FluidKind::Water) {
        return FluidConstants::kWaterDelay;
    }
    return traits.ultrawarm ? FluidConstants::kLavaDelayNether : FluidConstants::kLavaDelayNormal;
}

u8 FluidRules::drop_off(FluidKind kind, world::DimensionTraits traits) const noexcept {
    if (kind == FluidKind::Water) {
        return FluidConstants::kWaterDropOff;
    }
    return traits.ultrawarm ? FluidConstants::kLavaDropOffNether
                            : FluidConstants::kLavaDropOffNormal;
}

std::string_view FluidRules::tick_name(FluidState fluid) noexcept {
    switch (fluid.kind) {
        case FluidKind::Water:
            return fluid.is_source() ? "minecraft:water" : "minecraft:flowing_water";
        case FluidKind::Lava:
            return fluid.is_source() ? "minecraft:lava" : "minecraft:flowing_lava";
        case FluidKind::None: break;
    }
    return "minecraft:empty";
}

bool FluidRules::can_hold(registry::BlockStateId state, FluidKind /*kind*/) const noexcept {
    if (blocks_ == nullptr || !blocks_->is_valid(state)) {
        return false;
    }
    const registry::BlockId block = blocks_->block_of(state);
    if (blocks_->is_air(block)) {
        return true;
    }
    if (block == water_ || block == lava_) {
        return true;
    }
    // Anything with a collision box stops the flow, waterloggable or not.
    // Measured: a slab in the path of a flow stays dry and the water goes
    // around it, which is also why waterlogging is not part of spreading.
    // Something already holding a fluid is left alone too — kelp and seagrass
    // are water, not a place to put more of it.
    return blocks_->collision_boxes(state).empty() && !blocks_->holds_fluid(state);
}

bool FluidRules::can_flow_through(const world::LevelView& level, BlockPos /*from*/, BlockPos to,
                                  Direction /*direction*/, FluidKind kind) const {
    if (!level.shape().contains_y(to.y)) {
        return false;
    }
    // Flowing into a chunk that is not loaded would make the world depend on
    // where the player happened to walk. Refused, and named rather than
    // silently treated as solid.
    if (!level.is_loaded(to)) {
        return false;
    }
    return can_hold(level.block_at(to), kind);
}

bool FluidRules::becomes_source(const world::LevelView& level, BlockPos pos,
                                FluidKind kind) const {
    // Overworld water only. Lava has no such rule — measured: two lava sources
    // one block apart leave the gap between them flowing, never a source.
    if (kind != FluidKind::Water) {
        return false;
    }
    int sources = 0;
    for (Direction direction : kHorizontal) {
        const FluidState neighbour = fluid_at(level.block_at(pos.offset(direction)));
        if (neighbour.kind == FluidKind::Water && neighbour.is_source()) {
            ++sources;
        }
    }
    if (sources < 2) {
        return false;
    }
    // Something has to hold it up: a solid floor, or more of the same water.
    const BlockPos               below      = pos.below();
    const registry::BlockStateId below_state = level.block_at(below);
    const FluidState             below_fluid = fluid_at(below_state);
    if (below_fluid.kind == FluidKind::Water && below_fluid.is_source()) {
        return true;
    }
    return !blocks_->collision_boxes(below_state).empty();
}

FluidState FluidRules::computed_state(const world::LevelView& level, BlockPos pos) const {
    const FluidState current = fluid_at(level.block_at(pos));
    const FluidState above   = fluid_at(level.block_at(pos.above()));

    // Which fluid this position is about, decided **before** anything is
    // counted. A position never blends: water and lava meeting make a block,
    // not an average. Letting both kinds into the same tally is not a rounding
    // error — a water flow beside a lava source computes lava as the stronger
    // feed (its drop-off is 2, so a source reads as a level-2 neighbour) and
    // the water quietly turns into lava.
    const FluidKind kind = current.kind != FluidKind::None ? current.kind : above.kind;
    if (kind == FluidKind::None) {
        return {};
    }

    // The same fluid overhead makes this one falling, whatever its neighbours
    // say, and a falling fluid is full.
    if (above.kind == kind) {
        return FluidState{kind, 0, true};
    }

    u8 best_amount = 0xFF;
    for (Direction direction : kHorizontal) {
        const FluidState neighbour = fluid_at(level.block_at(pos.offset(direction)));
        if (neighbour.kind != kind) {
            continue;
        }
        // A falling neighbour feeds at full strength, which is why a shaft's
        // landing spreads level 1 rather than level 8 outwards.
        const u8  step = drop_off(kind, level.traits());
        const u32 candidate =
            neighbour.falling ? step : static_cast<u32>(neighbour.amount) + step;
        if (candidate > FluidConstants::kMinAmount) {
            continue;
        }
        best_amount = static_cast<u8>(std::min<u32>(best_amount, candidate));
    }

    if (best_amount == 0xFF) {
        return {};
    }
    if (becomes_source(level, pos, kind)) {
        return FluidState{kind, 0, false};
    }
    return FluidState{kind, best_amount, false};
}

void FluidRules::spread_directions(const world::LevelView& level, BlockPos pos, FluidState fluid,
                                   std::array<bool, 4>& out) const {
    out = {false, false, false, false};
    if (fluid.empty()) {
        return;
    }

    // Which of the four the fluid can enter at all.
    std::array<bool, 4> enterable{};
    bool                any = false;
    for (usize i = 0; i < kHorizontal.size(); ++i) {
        const BlockPos target = pos.offset(kHorizontal[i]);
        enterable[i] = can_flow_through(level, pos, target, kHorizontal[i], fluid.kind);
        any          = any || enterable[i];
    }
    if (!any) {
        return;
    }

    // ── The hole search ────────────────────────────────────────────────────
    //
    // A breadth-first walk of the horizontal plane, from `pos`, through cells
    // the fluid could occupy, out to radius 5. The first cell reached that has
    // nothing holding it up is a hole; every first step lying on a shortest
    // path to the nearest such hole is a direction the fluid takes.
    //
    // This is what makes a stream find its way instead of pooling. Measured: a
    // hole four blocks east makes a one-block-wide line east and leaves the
    // other three sides dry; a hole behind a two-block wall makes the water
    // step *south first*, away from the hole, and come round.
    std::array<i32, kCells> distance{};
    std::array<u8, kCells>  origin{};  // bitmask of the first steps reaching a cell
    distance.fill(kUnreached);
    origin.fill(0);

    // A small ring buffer, sized for the whole square so it can never wrap
    // into live entries.
    std::array<i32, kCells> queue_dx{};
    std::array<i32, kCells> queue_dz{};
    usize                   head = 0;
    usize                   tail = 0;

    for (usize i = 0; i < kHorizontal.size(); ++i) {
        if (!enterable[i]) {
            continue;
        }
        const Vec3i offset = direction_offset(kHorizontal[i]);
        const usize index  = cell_index(offset.x, offset.z);
        if (distance[index] == kUnreached) {
            distance[index]  = 1;
            queue_dx[tail]   = offset.x;
            queue_dz[tail]   = offset.z;
            ++tail;
        }
        origin[index] |= static_cast<u8>(1U << i);
    }

    i32 best = kUnreached;
    while (head < tail) {
        const i32   dx    = queue_dx[head];
        const i32   dz    = queue_dz[head];
        ++head;
        const usize index = cell_index(dx, dz);
        const i32   here  = distance[index];

        if (here > best) {
            continue;
        }

        const BlockPos cell = pos.offset(dx, 0, dz);
        // A hole: the fluid can leave downwards from here.
        if (can_flow_through(level, cell, cell.below(), Direction::Down, fluid.kind)) {
            if (here < best) {
                best = here;
            }
            // No point walking past a hole: anything further is a longer path.
            continue;
        }
        if (here >= kR) {
            continue;
        }

        for (Direction direction : kHorizontal) {
            const Vec3i step = direction_offset(direction);
            const i32   nx   = dx + step.x;
            const i32   nz   = dz + step.z;
            if (nx < -kR || nx > kR || nz < -kR || nz > kR) {
                continue;
            }
            const BlockPos next = pos.offset(nx, 0, nz);
            if (!can_flow_through(level, cell, next, direction, fluid.kind)) {
                continue;
            }
            const usize next_index = cell_index(nx, nz);
            if (distance[next_index] == kUnreached) {
                distance[next_index] = here + 1;
                queue_dx[tail]       = nx;
                queue_dz[tail]       = nz;
                ++tail;
            }
            // Every equally short way of arriving contributes its first step,
            // which is what makes the water fill the whole rectangle between
            // itself and a hole placed diagonally.
            if (distance[next_index] == here + 1) {
                origin[next_index] |= origin[index];
            }
        }
    }

    if (best == kUnreached) {
        // Nothing within reach falls away, so the fluid spreads every way it
        // can and the puddle becomes a disc. Measured: a hole at distance 6 is
        // not seen and the diamond comes back.
        out = enterable;
        return;
    }

    // Collect the first steps of every shortest path to a hole at `best`.
    u8 mask = 0;
    for (i32 dz = -kR; dz <= kR; ++dz) {
        for (i32 dx = -kR; dx <= kR; ++dx) {
            const usize index = cell_index(dx, dz);
            if (distance[index] != best) {
                continue;
            }
            const BlockPos cell = pos.offset(dx, 0, dz);
            if (can_flow_through(level, cell, cell.below(), Direction::Down, fluid.kind)) {
                mask |= origin[index];
            }
        }
    }
    for (usize i = 0; i < out.size(); ++i) {
        out[i] = (mask & (1U << i)) != 0;
    }
}

bool FluidRules::harden_lava(world::LevelWriter& level, BlockPos pos) const {
    const FluidState existing = fluid_at(level.block_at(pos));
    if (existing.kind != FluidKind::Lava) {
        return false;
    }
    // The whole measured table in two lines: a lava **source** met by water
    // becomes obsidian, **flowing** lava becomes cobblestone. One scenario
    // shows both at once — water poured over a settled lava pool leaves an
    // obsidian centre inside a ring of cobblestone.
    level.set_block(pos, existing.is_source() ? obsidian_ : cobblestone_);
    return true;
}

bool FluidRules::try_mix(world::LevelWriter& level, BlockPos pos, FluidKind arriving) const {
    const FluidState existing = fluid_at(level.block_at(pos));
    if (existing.kind == FluidKind::None || existing.kind == arriving) {
        return false;
    }
    if (arriving == FluidKind::Water && existing.kind == FluidKind::Lava) {
        return harden_lava(level, pos);
    }
    if (arriving == FluidKind::Lava && existing.kind == FluidKind::Water) {
        // Lava arriving where water is turns the **water** into stone and does
        // not itself advance. Measured both ways round: a lava source resting
        // directly on a water source, and a falling lava column landing in one,
        // both leave stone where the water was and the lava untouched above it.
        level.set_block(pos, stone_);
        return true;
    }
    return false;
}

void FluidRules::flow_into(world::LevelWriter& level, BlockPos pos, FluidState fluid) const {
    if (fluid.empty() || !level.shape().contains_y(pos.y) || !level.is_loaded(pos)) {
        return;
    }
    // The mixing table runs before the move: what is already there decides what
    // gets made, and the fluid does not arrive.
    if (try_mix(level, pos, fluid.kind)) {
        return;
    }
    const registry::BlockStateId existing = level.block_at(pos);
    if (!can_hold(existing, fluid.kind)) {
        return;
    }
    // Never write a thinner fluid over a thicker one. A flow reaching a
    // position that already holds more fluid — a source above all — has nothing
    // to add, and writing anyway destroys the source that was feeding the whole
    // pool. Weakening travels the other way, through each cell recomputing
    // itself in `tick`, which is what lets a broken dam drain.
    const FluidState already = fluid_at(existing);
    if (already.kind == fluid.kind && already.height() >= fluid.height()) {
        return;
    }
    level.set_block(pos, state_for(fluid));
    level.schedule_tick(pos, tick_name(fluid), tick_delay(fluid.kind, level.traits()),
                        world::TickQueue::Fluid);
}

void FluidRules::spread(world::LevelWriter& level, BlockPos pos, FluidState fluid) const {
    if (fluid.empty()) {
        return;
    }

    // Down first, and down excludes sideways. A column of falling water does
    // not wet the walls it passes: measured, the shaft holds level 8 and
    // nothing beside it.
    const BlockPos below = pos.below();
    if (level.shape().contains_y(below.y)) {
        const FluidState below_existing = fluid_at(level.block_at(below));
        const bool       mixes = below_existing.kind != FluidKind::None &&
                           below_existing.kind != fluid.kind;
        if (mixes) {
            // Lava landing on water makes stone there and stops; water landing
            // on lava hardens it and stops.
            if (try_mix(level, below, fluid.kind)) {
                // And then comes straight back: what was fluid underneath is
                // now a solid floor, so this position has to reconsider — it
                // can no longer fall and must spread sideways instead. Without
                // this, water poured onto a lava pool makes the one obsidian
                // block under the column and stops there, where vanilla goes on
                // to ring it with cobblestone.
                level.schedule_tick(pos, tick_name(fluid),
                                    tick_delay(fluid.kind, level.traits()),
                                    world::TickQueue::Fluid);
                wake_neighbours(level, below);
            }
            return;
        }
        if (can_flow_through(level, pos, below, Direction::Down, fluid.kind)) {
            flow_into(level, below, FluidState{fluid.kind, 0, true});
            return;
        }
    }

    // Sideways. A falling fluid feeds at full strength — measured: water
    // landing at the bottom of a shaft gives its neighbours level 1, exactly as
    // a source does.
    const u8  step   = drop_off(fluid.kind, level.traits());
    const u32 amount = fluid.falling ? step : static_cast<u32>(fluid.amount) + step;
    if (amount > FluidConstants::kMinAmount) {
        return;
    }

    std::array<bool, 4> directions{};
    spread_directions(level, pos, fluid, directions);
    for (usize i = 0; i < directions.size(); ++i) {
        if (!directions[i]) {
            continue;
        }
        flow_into(level, pos.offset(kHorizontal[i]),
                  FluidState{fluid.kind, static_cast<u8>(amount), false});
    }
}

void FluidRules::tick(world::LevelWriter& level, BlockPos pos) const {
    const registry::BlockStateId here  = level.block_at(pos);
    FluidState                   fluid = fluid_at(here);
    if (fluid.empty()) {
        return;
    }

    // A waterlogged block is a source that cannot be recomputed away — the
    // block, not the flow, is holding the water.
    const bool waterlogged = fluid.kind == FluidKind::Water && releases_water(here);

    if (!fluid.is_source() && !waterlogged) {
        const FluidState wanted = computed_state(level, pos);
        if (wanted.empty()) {
            level.set_block(pos, air_);
            // Emptying travels outwards the same way filling did: each cell
            // recomputes itself, so the ones that were leaning on this must be
            // woken. Without this a broken dam leaves the far end of the pool
            // standing for ever, held up by water that is no longer there.
            wake_neighbours(level, pos);
            return;
        }
        if (wanted != fluid) {
            fluid = wanted;
            level.set_block(pos, state_for(fluid));
            level.schedule_tick(pos, tick_name(fluid), tick_delay(fluid.kind, level.traits()),
                                world::TickQueue::Fluid);
            wake_neighbours(level, pos);
        }
    }

    spread(level, pos, fluid);
}

void FluidRules::wake_neighbours(world::LevelWriter& level, BlockPos pos) const {
    // The five a fluid can be leaning on: the four sides and what is above it.
    // Not below — a cell never depends on the one under it for how full it is.
    for (Direction direction : kHorizontal) {
        on_neighbour_changed(level, pos.offset(direction));
    }
    on_neighbour_changed(level, pos.above());
}

void FluidRules::on_neighbour_changed(world::LevelWriter& level, BlockPos pos) const {
    const FluidState fluid = fluid_at(level.block_at(pos));
    if (fluid.empty()) {
        return;
    }
    const std::string_view name = tick_name(fluid);
    if (level.has_scheduled_tick(pos, name, world::TickQueue::Fluid)) {
        return;
    }
    level.schedule_tick(pos, name, tick_delay(fluid.kind, level.traits()),
                        world::TickQueue::Fluid);
}

std::optional<registry::BlockStateId> FluidRules::waterlogged_variant(
    registry::BlockStateId state, bool waterlogged) const noexcept {
    if (blocks_ == nullptr || !blocks_->is_valid(state)) {
        return std::nullopt;
    }
    const registry::BlockId block = blocks_->block_of(state);
    if (waterlogged_stride_[block.value()] == 0) {
        return std::nullopt;
    }
    const auto property = blocks_->find_property(block, "waterlogged");
    if (!property) {
        return std::nullopt;
    }
    // The property's values are declared "true" then "false", so the index is
    // not the boolean. Looked up rather than assumed: the order is the pack's,
    // and a hard-coded 0/1 here drains every block it was meant to fill.
    for (u16 i = 0; i < property->values.size(); ++i) {
        if ((property->values[i] == "true") == waterlogged) {
            return blocks_->with_property(state, *property, i);
        }
    }
    return std::nullopt;
}

bool FluidRules::releases_water(registry::BlockStateId state) const noexcept {
    if (blocks_ == nullptr || !blocks_->is_valid(state)) {
        return false;
    }
    const registry::BlockId block = blocks_->block_of(state);
    if (waterlogged_stride_[block.value()] == 0) {
        return false;
    }
    const auto property = blocks_->find_property(block, "waterlogged");
    return property && blocks_->property_value(state, *property) == "true";
}

registry::BlockStateId FluidRules::state_after_break(registry::BlockStateId state) const noexcept {
    if (!releases_water(state)) {
        return air_;
    }
    return state_for(FluidState{FluidKind::Water, 0, false});
}

bool FluidRules::place_fluid(world::LevelWriter& level, BlockPos pos, FluidKind kind) const {
    if (kind == FluidKind::None || !level.shape().contains_y(pos.y)) {
        return false;
    }
    const registry::BlockStateId existing = level.block_at(pos);

    // Waterlogging: a bucket of water emptied into a slab, a stair, a fence or
    // a sign fills the block rather than replacing it. Only water — there is no
    // lavalogging, and asking for one is a caller's mistake rather than a
    // silent no-op.
    if (kind == FluidKind::Water) {
        if (const auto filled = waterlogged_variant(existing, true)) {
            if (releases_water(existing)) {
                return false;  // already full
            }
            level.set_block(pos, *filled);
            level.schedule_tick(pos, "minecraft:water", FluidConstants::kWaterDelay,
                                world::TickQueue::Fluid);
            return true;
        }
    }

    if (!can_hold(existing, kind)) {
        return false;
    }
    const FluidState source{kind, 0, false};
    level.set_block(pos, state_for(source));
    level.schedule_tick(pos, tick_name(source), tick_delay(kind, level.traits()),
                        world::TickQueue::Fluid);
    return true;
}

std::optional<FluidKind> FluidRules::pick_up_fluid(world::LevelWriter& level,
                                                   BlockPos            pos) const {
    const registry::BlockStateId existing = level.block_at(pos);

    if (releases_water(existing)) {
        if (const auto drained = waterlogged_variant(existing, false)) {
            level.set_block(pos, *drained);
            return FluidKind::Water;
        }
        return std::nullopt;
    }

    const FluidState fluid = fluid_at(existing);
    // Only a source fills a bucket. A flow is not enough fluid to hold.
    if (!fluid.is_source()) {
        return std::nullopt;
    }
    level.set_block(pos, air_);
    return fluid.kind;
}

Vec3d FluidRules::flow_vector(const world::LevelView& level, BlockPos pos) const {
    const FluidState fluid = fluid_at(level.block_at(pos));
    if (fluid.empty()) {
        return {};
    }

    Vec3d flow{};
    for (Direction direction : kHorizontal) {
        const BlockPos   neighbour_pos = pos.offset(direction);
        const FluidState neighbour     = fluid_at(level.block_at(neighbour_pos));

        f64 drop = 0.0;
        if (neighbour.kind == fluid.kind) {
            drop = static_cast<f64>(fluid.height()) - static_cast<f64>(neighbour.height());
        } else if (neighbour.empty()) {
            // An open edge pulls, but only where the fluid could actually go —
            // a wall is not a slope.
            if (!can_hold(level.block_at(neighbour_pos), fluid.kind)) {
                continue;
            }
            drop = static_cast<f64>(fluid.height());
        } else {
            continue;
        }
        if (drop == 0.0) {
            continue;
        }
        const Vec3i offset = direction_offset(direction);
        flow.x += static_cast<f64>(offset.x) * drop;
        flow.z += static_cast<f64>(offset.z) * drop;
    }
    return flow;
}

usize FluidRules::absorb(world::LevelWriter& level, BlockPos pos) const {
    // A breadth-first sweep, so the count is taken nearest first and the cap
    // bites at the edge rather than in an arbitrary corner.
    struct Entry {
        BlockPos pos;
        i32      depth;
    };
    // Bounded by the radius, so no allocation once warm: the reachable set of a
    // radius-7 taxicab ball is 1289 cells.
    std::vector<Entry> queue;
    queue.reserve(1400);
    std::vector<BlockPos> seen;
    seen.reserve(1400);

    queue.push_back(Entry{pos, 0});
    seen.push_back(pos);

    usize taken = 0;
    for (usize head = 0; head < queue.size(); ++head) {
        const Entry entry = queue[head];
        if (entry.depth > FluidConstants::kSpongeRadius) {
            continue;
        }
        for (u8 d = 0; d < kDirectionCount; ++d) {
            const auto     direction = static_cast<Direction>(d);
            const BlockPos next      = entry.pos.offset(direction);
            if (!level.shape().contains_y(next.y) || !level.is_loaded(next)) {
                continue;
            }
            if (std::find(seen.begin(), seen.end(), next) != seen.end()) {
                continue;
            }
            seen.push_back(next);

            const registry::BlockStateId state = level.block_at(next);
            const FluidState             fluid = fluid_at(state);
            if (fluid.kind != FluidKind::Water) {
                continue;
            }
            if (releases_water(state)) {
                if (const auto drained = waterlogged_variant(state, false)) {
                    level.set_block(next, *drained);
                }
            } else {
                level.set_block(next, air_);
            }
            ++taken;
            if (taken >= FluidConstants::kSpongeMax) {
                return taken;
            }
            queue.push_back(Entry{next, entry.depth + 1});
        }
    }
    return taken;
}

}  // namespace ov::gameplay
