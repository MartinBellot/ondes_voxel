#include "ov/gameplay/primed_tnt.hpp"

#include "ov/gameplay/mob_logic.hpp"

#include <cmath>
#include <numbers>

namespace ov::gameplay {

Vec3d tnt_prime_velocity(math::LegacyRandomSource& rng) {
    const f64 angle = rng.next_double() * 2.0 * std::numbers::pi;
    return Vec3d{-std::sin(angle) * kTntPrimeSpread, kTntPrimeUp,
                 -std::cos(angle) * kTntPrimeSpread};
}

Vec3d tnt_blast_centre(const entity::EntityState& state) noexcept {
    // The height is the table's float, widened: 0.98F × 0.0625 is
    // 0.06125000119, and that last digit is in the captured packet.
    return Vec3d{state.position.x,
                 state.position.y + static_cast<f64>(state.height) * 0.0625,
                 state.position.z};
}

void PrimedTntLogic::tick(entity::EntityWorld& world, entity::EntityHandle self,
                          const entity::TickContext& context) {
    const MobContext* mob = mob_context(context);
    entity::EntityState* state = world.mutable_state(self);
    if (state == nullptr || state->removed) {
        return;
    }
    if (mob != nullptr && mob->world != nullptr) {
        *state = step_block_entity(*state, motion_, *mob->world, gravity_);
    }
    // The count goes on even without a world to move through: a fuse that
    // stopped because the caller forgot the collision world would be a TNT
    // that never goes off, and say nothing about why.
    --fuse_;
    if (fuse_ <= 0) {
        events_->blasts.push_back(BlastEvents::Blast{tnt_blast_centre(*state), kTntPower,
                                                     BlockInteraction::Destroy,
                                                     state->network_id});
        state->removed = true;
    }
}

bool tnt_lit_by_signal(const Signals& signals, const RedstoneWorld& world, BlockPos pos,
                       registry::BlockId tnt) {
    const registry::BlockStateId state = world.block_at(pos);
    if (world.blocks().block_of(state) != tnt) {
        return false;
    }
    return signals.has_neighbour_signal(world, pos);
}

CreeperStep tick_creeper(CreeperSwell& creeper, std::optional<f64> target_distance,
                         bool sees_target) noexcept {
    const i32 before = creeper.direction;
    if (creeper.ignited) {
        creeper.direction = 1;
    } else if (!target_distance) {
        creeper.direction = -1;
    } else {
        // Swelling starts close and continues while the target stays inside
        // the wider radius and in sight; the two radii are the hysteresis
        // that lets a player back off a step without defusing it.
        const bool engaged = creeper.direction > 0 || *target_distance < kCreeperSwellStart;
        creeper.direction =
            engaged && sees_target && *target_distance <= kCreeperSwellGiveUp ? 1 : -1;
    }

    creeper.swell += creeper.direction;
    if (creeper.swell < 0) {
        creeper.swell = 0;
    }
    CreeperStep step;
    step.direction_changed = creeper.direction != before;
    if (creeper.swell >= creeper.fuse) {
        creeper.swell = creeper.fuse;
        step.explode  = true;
    }
    return step;
}

void collect_detonation(const world::LevelView& level, const Explosions& explosions,
                        const ExplosionSpec& spec, math::LegacyRandomSource& rng,
                        Detonation& out) {
    out.clear();
    explosions.collect_cells(level, spec, rng, out.blocks, out.air);
}

void destroy_detonation(world::LevelWriter& level, const Explosions& explosions,
                        const LootTables* loot, const ExplosionSpec& spec, registry::BlockId tnt,
                        math::LegacyRandomSource& rng, math::XoroshiroRandomSource& loot_rng,
                        Detonation& detonation) {
    detonation.drops.clear();
    detonation.primed.clear();
    const registry::BlockRegistry& blocks = level.blocks();

    std::vector<Drop> drawn;
    for (const BlockPos pos : detonation.blocks) {
        const registry::BlockStateId state = level.block_at(pos);
        if (blocks.block_of(state) == tnt) {
            // A TNT block is lit, not broken: it gives no item and goes off a
            // short, random while later.
            detonation.primed.push_back(
                Detonation::Primed{pos, explosions.chained_fuse(kTntFuseTicks, rng)});
            continue;
        }
        if (loot == nullptr || !explosions.can_drop_from_explosion(state) ||
            !explosions.rolls_drop(spec, rng)) {
            continue;
        }
        drawn.clear();
        loot->drops(state, Held{}, loot_rng, drawn,
                    Neighbours{.above = level.block_at(pos.offset(Direction::Up)),
                               .below = level.block_at(pos.offset(Direction::Down))});
        for (const Drop& drop : drawn) {
            detonation.drops.push_back(Detonation::Dropped{pos, drop});
        }
    }

    // Only now, with every table drawn against the world as it was.
    for (const BlockPos pos : detonation.blocks) {
        level.set_block(pos, registry::kAirState);
    }
}

}  // namespace ov::gameplay
