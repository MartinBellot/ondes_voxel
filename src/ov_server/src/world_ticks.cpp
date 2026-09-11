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
            if (extension_ != nullptr) {  // ── tnt and gravity ──
                extension_->neighbour_changed(level, pos);
            }
            if (portals_ != nullptr) {  // ── nether ──
                (void)portals_->on_block_changed(level, pos);
            }
            if (fire_extension_ != nullptr) {  // ── fire ──
                fire_extension_->neighbour_changed(level, pos);
            }
            ++notified;
            if (plants_ != nullptr) {  // ── agriculture ──
                // The written block itself: a leaf placed next to a log learns
                // its distance, as the game's placement computes it.
                plants_->neighbour_changed(level, *plant_env_, pos, pos);
            }

            for (u8 i = 0; i < kDirectionCount; ++i) {
                const BlockPos neighbour = pos.offset(static_cast<Direction>(i));
                if (!level.shape().contains_y(neighbour.y)) {
                    continue;
                }
                fluid_.on_neighbour_changed(level, neighbour);
                (void)redstone_.neighbour_changed(level, neighbour, pos);
                if (extension_ != nullptr) {  // ── tnt and gravity ──
                    extension_->neighbour_changed(level, neighbour);
                }
                if (fire_extension_ != nullptr) {  // ── fire ──
                    fire_extension_->neighbour_changed(level, neighbour);
                }
                if (plants_ != nullptr) {  // ── agriculture ──
                    plants_->neighbour_changed(level, *plant_env_, neighbour, pos);
                }
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
    // Seeded the same way a write is — through `changed`, into `settle` — so a
    // player's edit and a rule's edit go down one path. What differs is the
    // *width* of the seed, and that difference is a stated stand-in rather than
    // an accident.
    //
    // A wave notifies a position and its six neighbours. That is exactly what
    // vanilla's `Level.updateNeighborsAt` does, and it is **not enough for a
    // lever**: a floor lever powers the block underneath it, and the wire it is
    // meant to light is diagonal from the lever — one ring too far. Vanilla
    // reaches it because `LeverBlock` notifies twice, once around itself and
    // once around the block it is attached to; `ButtonBlock` and
    // `PressurePlateBlock` do the same.
    //
    // `ov_gameplay` has no per-block hook for that. `Redstone::neighbour_changed`
    // is the whole vocabulary, and adding an `updateNeighbours` override per
    // block is a layer-9 change this file may not make. So the seed for an edge
    // change is the position **and its six neighbours**, which notifies the
    // support ring among others — a superset of vanilla's rule.
    //
    // Over-notifying is safe here and that is not a hope: every rule recomputes
    // its state from the world rather than integrating a delta, so a position
    // told twice about nothing writes nothing and produces no second wave.
    // Under-notifying is not safe, and it is what left the lab's lever lighting
    // nothing at all.
    //
    // Measured on the lab's `dust` plot: with the one-ring seed, pulling the
    // lever moved the lever and **zero** of the fourteen wires. With this one,
    // 14 of 14.
    level.clear_changed();
    level.mark_changed(pos);
    for (u8 i = 0; i < kDirectionCount; ++i) {
        const BlockPos neighbour = pos.offset(static_cast<Direction>(i));
        if (level.shape().contains_y(neighbour.y)) {
            level.mark_changed(neighbour);
        }
    }
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
        // ── agriculture ── leaves, cactus, sugar cane and farmland ask for
        // their own ticks; the plant rules answer them before redstone sees
        // them.
        if (const auto block = level.blocks().find_block(tick.what);
            block && plants_ != nullptr && plants_->scheduled_tick(level, *plant_env_, tick.pos, *block)) {
            ++stats.block_ticks;
            continue;
        }
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
        // ── tnt and gravity: a falling block's tick is not redstone's ──
        if (extension_ != nullptr && extension_->scheduled_tick(level, tick.pos, tick.what)) {
            ++stats.block_ticks;
            continue;
        }
        // ── fire: a fire's own tick ──
        if (fire_extension_ != nullptr &&
            fire_extension_->scheduled_tick(level, tick.pos, tick.what)) {
            ++stats.block_ticks;
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
