#define OV_LOG_CATEGORY "server"

#include "world_ticks.hpp"

#include "ov/base/log.hpp"

#include <utility>

namespace ov::server {

// ── The level ───────────────────────────────────────────────────────────────

ServerLevel::ServerLevel(const registry::BlockRegistry& blocks, LevelHooks hooks)
    : blocks_{&blocks}, hooks_{std::move(hooks)} {
    // A puddle settling touches a few hundred positions and a wire a few
    // thousand. Reserved once so the drain never grows the vector under the
    // tick.
    changed_.reserve(4096);
}

registry::BlockStateId ServerLevel::block_at(BlockPos pos) const {
    if (!shape_.contains_y(pos.y) || !hooks_.block_at) {
        return registry::kAirState;
    }
    return hooks_.block_at(pos);
}

bool ServerLevel::is_loaded(BlockPos pos) const {
    return hooks_.is_loaded && shape_.contains_y(pos.y) && hooks_.is_loaded(pos);
}

void ServerLevel::set_block(BlockPos pos, registry::BlockStateId state) {
    if (!shape_.contains_y(pos.y) || !hooks_.set_block) {
        return;
    }
    hooks_.set_block(pos, state);
    changed_.push_back(pos);
    ++writes_;
}

void ServerLevel::schedule_tick(BlockPos pos, std::string_view what, i64 delay,
                                world::TickQueue queue, world::TickPriority priority) {
    this->queue(queue).schedule(pos, what, delay, now_, priority);
}

bool ServerLevel::has_scheduled_tick(BlockPos pos, std::string_view what,
                                     world::TickQueue queue) const {
    return this->queue(queue).is_scheduled(pos, what);
}

i32 ServerLevel::container_signal(BlockPos pos) const {
    return hooks_.container_signal ? hooks_.container_signal(pos) : -1;
}

world::BlockTickScheduler& ServerLevel::queue(world::TickQueue which) noexcept {
    return which == world::TickQueue::Fluid ? fluid_ticks_ : block_ticks_;
}

const world::BlockTickScheduler& ServerLevel::queue(world::TickQueue which) const noexcept {
    return which == world::TickQueue::Fluid ? fluid_ticks_ : block_ticks_;
}

// ── The driver ──────────────────────────────────────────────────────────────

WorldTicks::WorldTicks(const registry::BlockRegistry& blocks,
                       const registry::Registries&    registries)
    : fluid_{blocks}, redstone_{blocks, registries} {
    due_.reserve(1024);
    wave_.reserve(4096);
}

usize WorldTicks::settle(ServerLevel& level, usize& waves) {
    usize notified = 0;

    while (!level.changed().empty() && waves < kMaxWaves) {
        ++waves;

        // Moved out rather than iterated in place: notifying writes, and a
        // write during this wave must land in the next one. Iterating
        // `level.changed()` directly would both invalidate the span and let one
        // wave chase its own tail for as long as the circuit kept moving.
        wave_.assign(level.changed().begin(), level.changed().end());
        level.clear_changed();

        for (const BlockPos pos : wave_) {
            // The position itself first, then its six neighbours. Vanilla
            // notifies only the six; the seventh is what makes a fluid that was
            // *replaced* — a source broken into air — recompute rather than
            // wait for someone else to poke it.
            fluid_.on_neighbour_changed(level, pos);
            ++notified;

            for (u8 i = 0; i < kDirectionCount; ++i) {
                const BlockPos neighbour = pos.offset(static_cast<Direction>(i));
                if (!level.shape().contains_y(neighbour.y)) {
                    continue;
                }
                fluid_.on_neighbour_changed(level, neighbour);
                (void)redstone_.neighbour_changed(level, neighbour, pos);
                ++notified;
            }
        }
    }

    // A settle that hit the ceiling is a circuit that is still moving, not an
    // error: the queues still hold whatever it scheduled and the next tick
    // picks it up. Said out loud, because a silent truncation here looks
    // exactly like a rule that stopped working.
    if (waves >= kMaxWaves && !level.changed().empty()) {
        OV_LOG_DEBUG("block update wave ceiling reached ({} waves), {} writes deferred", waves,
                     level.changed().size());
        level.clear_changed();
    }

    return notified;
}

usize WorldTicks::notify(ServerLevel& level, BlockPos pos) {
    // Seeded the same way a write is, so a player's edit and a rule's edit go
    // down exactly one path. The alternative — a second, shorter notification
    // for players — is how the two drift apart.
    level.clear_changed();
    level.mark_changed(pos);
    usize waves = 0;
    return settle(level, waves);
}

WorldTickStats WorldTicks::run(ServerLevel& level, i64 now) {
    WorldTickStats stats;
    level.set_game_time(now);
    level.clear_changed();

    // Fluids first, then blocks. Vanilla drains the two queues in that order
    // and the difference is observable: a water flow that arrives on the same
    // tick as a redstone update reaches the wire before the wire recomputes,
    // rather than one tick later.
    level.queue(world::TickQueue::Fluid).collect_due(now, due_);
    for (const world::ScheduledTick& tick : due_) {
        fluid_.tick(level, tick.pos);
        ++stats.fluid_ticks;
    }

    level.queue(world::TickQueue::Block).collect_due(now, due_);
    for (const world::ScheduledTick& tick : due_) {
        if (!level.blocks().find_block(tick.what).has_value()) {
            // A tick naming a block this version does not have. Refused and
            // named rather than dropped: it means something scheduled with a
            // name that is not in the registry, and nothing about the world
            // would otherwise say where it came from.
            OV_LOG_WARN("block tick names an unknown block '{}' at {},{},{}", tick.what,
                        tick.pos.x, tick.pos.y, tick.pos.z);
            ++stats.refused;
            continue;
        }
        (void)redstone_.scheduled_tick(level, tick.pos, tick.what);
        ++stats.block_ticks;
    }

    // The torch history is the one thing redstone keeps between ticks, and it
    // has to be trimmed once a tick or a world full of clocks grows it without
    // bound.
    redstone_.torch_history().expire(now);

    stats.notifications = settle(level, stats.waves);
    return stats;
}

}  // namespace ov::server
