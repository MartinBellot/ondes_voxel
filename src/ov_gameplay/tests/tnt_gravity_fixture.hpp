// A small world for the falling-block and TNT tests, and a driver that runs it
// the way the server does: block ticks, then notifications, then entities,
// then what the entities asked for.
//
// Deliberately the same order as src/ov_server/src/tnt_gravity.cpp and the
// server's tick, so that a stagger or a landing that comes out right here comes
// out right for the same reason there.
#pragma once

#include "ov/entity/world.hpp"
#include "ov/gameplay/falling_block.hpp"
#include "ov/gameplay/mob_logic.hpp"
#include "ov/gameplay/primed_tnt.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/block_ticks.hpp"

#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <tuple>
#include <vector>

namespace ov::test {

[[nodiscard]] inline std::filesystem::path pack_path() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack";
}

[[nodiscard]] inline const registry::BlockRegistry* pack_blocks() {
    static const auto loaded = registry::BlockRegistry::load(pack_path());
    return loaded ? &*loaded : nullptr;
}

[[nodiscard]] inline const registry::Registries* pack_registries() {
    static const auto loaded = registry::Registries::load(pack_path());
    return loaded ? &*loaded : nullptr;
}

[[nodiscard]] inline registry::BlockStateId state_of(std::string_view name) {
    const auto block = pack_blocks()->find_block(name);
    return block ? pack_blocks()->default_state(*block) : registry::kAirState;
}

/// A world of explicit cells over a solid floor.
class TestLevel final : public gameplay::RedstoneWorld {
public:
    explicit TestLevel(const registry::BlockRegistry& blocks, i32 floor_y = -61,
                       registry::BlockStateId floor = registry::BlockStateId{0})
        : blocks_{&blocks}, floor_y_{floor_y}, floor_{floor} {
        if (floor_.value() == 0) {
            floor_ = state_of("minecraft:stone");
        }
    }

    [[nodiscard]] registry::BlockStateId block_at(BlockPos pos) const override {
        if (const auto it = cells_.find(key(pos)); it != cells_.end()) {
            return it->second;
        }
        return pos.y <= floor_y_ ? floor_ : registry::kAirState;
    }
    [[nodiscard]] bool              is_loaded(BlockPos) const override { return true; }
    [[nodiscard]] world::WorldShape shape() const override {
        return world::WorldShape::overworld();
    }
    [[nodiscard]] world::DimensionTraits traits() const override { return {}; }
    [[nodiscard]] const registry::BlockRegistry& blocks() const override { return *blocks_; }

    void set_block(BlockPos pos, registry::BlockStateId state) override {
        cells_[key(pos)] = state;
        changed_.push_back(pos);
    }
    void schedule_tick(BlockPos pos, std::string_view what, i64 delay, world::TickQueue,
                       world::TickPriority priority) override {
        queue_.schedule(pos, what, delay, now_, priority);
    }
    [[nodiscard]] bool has_scheduled_tick(BlockPos pos, std::string_view what,
                                          world::TickQueue) const override {
        return queue_.is_scheduled(pos, what);
    }
    [[nodiscard]] i64 game_time() const override { return now_; }
    [[nodiscard]] i32 container_signal(BlockPos) const override { return -1; }

    /// Put a block without it counting as a change: the world as it was built.
    void place(BlockPos pos, registry::BlockStateId state) { cells_[key(pos)] = state; }

    void                            set_now(i64 now) { now_ = now; }
    world::BlockTickScheduler&      queue() { return queue_; }
    std::vector<BlockPos>&          changed() { return changed_; }

    static registry::BlockStateId look_up(void* context, i32 x, i32 y, i32 z) {
        return static_cast<const TestLevel*>(context)->block_at(BlockPos{x, y, z});
    }

private:
    using Key = std::tuple<i32, i32, i32>;
    [[nodiscard]] static Key key(BlockPos pos) { return {pos.x, pos.y, pos.z}; }

    const registry::BlockRegistry*    blocks_;
    i32                               floor_y_;
    registry::BlockStateId            floor_;
    std::map<Key, registry::BlockStateId> cells_;
    std::vector<BlockPos>             changed_;
    world::BlockTickScheduler         queue_;
    i64                               now_{0};
};

/// Blocks fall, entities fly, landings land — one server tick at a time.
struct Simulation {
    TestLevel&                       level;
    const gameplay::FallingBlocks&   rules;
    entity::EntityWorld&             world;
    gameplay::FallingEvents          falling;
    gameplay::BlastEvents            blasts;
    gameplay::CollisionWorld         collisions;
    gameplay::MobContext             mob;
    i64                              now{0};

    struct Birth {
        i64      tick;
        BlockPos pos;
    };
    std::vector<Birth>                      births;
    std::vector<gameplay::Landing>          landings;
    std::vector<gameplay::FallingEvents::Landed> landed_log;

    Simulation(TestLevel& level_, const gameplay::FallingBlocks& rules_,
               entity::EntityWorld& world_)
        : level{level_},
          rules{rules_},
          world{world_},
          collisions{level_.blocks(), &TestLevel::look_up, &level_},
          mob{&collisions, &level_, false} {}

    /// Tell every changed cell and its neighbours, until nothing changes.
    void settle() {
        for (int wave = 0; wave < 64 && !level.changed().empty(); ++wave) {
            std::vector<BlockPos> seeds;
            seeds.swap(level.changed());
            for (const BlockPos pos : seeds) {
                (void)rules.neighbour_changed(level, pos);
                for (u8 i = 0; i < kDirectionCount; ++i) {
                    (void)rules.neighbour_changed(level, pos.offset(static_cast<Direction>(i)));
                }
            }
        }
    }

    void step() {
        level.set_now(now);
        std::vector<world::ScheduledTick> due;
        level.queue().collect_due(now, due);
        for (const world::ScheduledTick& tick : due) {
            if (const auto start = rules.tick(level, tick.pos, tick.what)) {
                const auto handle =
                    world.spawn("minecraft:falling_block",
                                Vec3d{static_cast<f64>(start->pos.x) + 0.5,
                                      static_cast<f64>(start->pos.y),
                                      static_cast<f64>(start->pos.z) + 0.5},
                                net::Uuid{});
                if (handle) {
                    world.set_logic(*handle, std::make_unique<gameplay::FallingBlockLogic>(
                                                 rules, start->state, start->pos, falling));
                }
                births.push_back(Birth{now, start->pos});
            }
        }
        settle();
        world.tick(entity::TickContext{now, &mob});
        for (const auto& landed : falling.landed) {
            landed_log.push_back(landed);
            landings.push_back(landed.expired
                                   ? gameplay::Landing::Dropped
                                   : rules.land(level, landed.cell, landed.state, landed.in_water));
        }
        falling.landed.clear();
        settle();
        ++now;
    }
};

}  // namespace ov::test
