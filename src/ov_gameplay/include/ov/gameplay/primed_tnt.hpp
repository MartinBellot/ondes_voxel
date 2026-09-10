// Primed TNT, creepers, and what an explosion does once it has been decided.
//
// explosion.hpp decides *which* blocks a charge takes and how hard it hits an
// entity. This is everything around that: the entity that carries a charge
// for eighty ticks, the rule that lights one from redstone, the creeper that
// counts to thirty, and the two phases in which a decided explosion is applied
// to a world.
//
// Measured on a real 1.20.1 server by scripts/measure_tnt_gravity.py
// (docs/provenance/tnt-et-gravite.md):
//
//   * **The prime.** A TNT lit by redstone is sent with a velocity of
//     (104, 1600, 120) in 1/8000ths: 0.2 up and 0.02 sideways, the side in a
//     random direction. Its Motion one tick later reads 0.1568000029206276 —
//     `(0.2F − 0.04) × 0.98`, the float 0.2 widened, not the double.
//
//   * **The flight.** 3950 samples of ten TNT in the air, each tagged with its
//     own `Fuse`, fit gravity 0.04 and drag 0.98 on every axis to 3e-9 on the
//     vertical and 1e-17 on the horizontal. On the ground the horizontal
//     shrinks a further ×0.7 and the vertical rests at exactly zero.
//
//   * **The fuse.** 80, re-read here as the largest `Fuse` seen: 79, one tick
//     after priming. A TNT that another explosion lights waits 10 to 29 ticks
//     — 384 of them read, 20 distinct values, exactly that range.
//
//   * **The creeper.** Its explosion drops what it breaks one time in three:
//     989 blocks broken over twelve shots, 333 items back, 0.337.
#pragma once

#include "ov/entity/logic.hpp"
#include "ov/entity/world.hpp"
#include "ov/gameplay/explosion.hpp"
#include "ov/gameplay/falling_block.hpp"
#include "ov/gameplay/loot.hpp"
#include "ov/gameplay/signal.hpp"
#include "ov/math/random.hpp"
#include "ov/math/vec.hpp"
#include "ov/world/level.hpp"

#include <optional>
#include <vector>

namespace ov::gameplay {

/// The vertical speed a TNT is primed with: the float `0.2F`, widened.
inline constexpr f64 kTntPrimeUp = static_cast<f64>(0.2F);

/// The horizontal speed a TNT is primed with. Its direction is drawn.
inline constexpr f64 kTntPrimeSpread = 0.02;

/// The velocity of a freshly primed TNT. One draw from `rng`, for the angle.
///
/// The magnitude and the vertical are measured; the direction's distribution
/// is not — one captured spawn cannot say whether it is uniform, and the wiki
/// says only "a random direction".
[[nodiscard]] Vec3d tnt_prime_velocity(math::LegacyRandomSource& rng);

/// Where a primed TNT's charge goes off: its feet raised by a sixteenth of its
/// height. Captured as y = -59.93874999880791 for a TNT standing at -60.
[[nodiscard]] Vec3d tnt_blast_centre(const entity::EntityState& state) noexcept;

/// Where entity logic reports a charge going off.
///
/// A queue, drained by the caller once the entity tick is over — an
/// explosion writes blocks and hurts other entities, and neither may happen
/// while the entity world is being iterated.
struct BlastEvents {
    struct Blast {
        Vec3d            centre{};
        f32              power{kTntPower};
        BlockInteraction interaction{BlockInteraction::Destroy};
        /// The wire id of what exploded.
        i32 source{0};
    };
    std::vector<Blast> blasts;
};

/// The logic of one `minecraft:tnt`.
class PrimedTntLogic final : public entity::IEntityLogic {
public:
    PrimedTntLogic(i32 fuse, BlastEvents& events, bool gravity = true,
                   BlockEntityMotion motion = {}) noexcept
        : fuse_{fuse}, events_{&events}, gravity_{gravity}, motion_{motion} {}

    /// Move, then count down, then — on the tick the count reaches zero —
    /// report the charge and remove the entity. Measured order: the Motion
    /// read beside a `Fuse` of 79 has had exactly one tick of motion.
    void tick(entity::EntityWorld& world, entity::EntityHandle self,
              const entity::TickContext& context) override;

    [[nodiscard]] std::string_view name() const noexcept override { return "tnt"; }

    [[nodiscard]] i32 fuse() const noexcept { return fuse_; }

private:
    i32               fuse_;
    BlastEvents*      events_;
    bool              gravity_;
    BlockEntityMotion motion_;
};

/// Does redstone light the TNT block at `pos`?
///
/// Vanilla's rule is "any neighbour signal", the same question a lamp asks.
/// Measured: a redstone block set beside a TNT primes it in the same tick.
[[nodiscard]] bool tnt_lit_by_signal(const Signals& signals, const RedstoneWorld& world,
                                     BlockPos pos, registry::BlockId tnt);

// ── Creeper ─────────────────────────────────────────────────────────────────

/// A creeper starts swelling at a target closer than this, in blocks.
///
/// From the wiki's Creeper article ("within 3 blocks"), not measured here: the
/// capture that tried to watch a creeper swell at a survival probe saw it turn
/// aggressive and never close the distance.
inline constexpr f64 kCreeperSwellStart = 3.0;

/// …and gives up once the target is farther than this. Same source.
inline constexpr f64 kCreeperSwellGiveUp = 7.0;

/// One creeper's countdown. The swell counts up by one a tick while swelling
/// and down by one while not; it explodes on reaching the fuse. `swell` is
/// what the fuse of 30 measured in explosions.md § 5 counts.
struct CreeperSwell {
    i32  swell{0};
    i32  direction{-1};
    i32  fuse{kCreeperFuseTicks};
    bool ignited{false};
    bool powered{false};
};

struct CreeperStep {
    /// `direction` changed this tick: the client needs index 16 again.
    bool direction_changed{false};
    /// The countdown reached the fuse: explode now.
    bool explode{false};
};

/// One tick of the countdown.
///
/// `target_distance` is the distance to the creeper's target, or nothing when
/// it has none; `sees_target` is the line of sight. Lit by flint and steel it
/// swells whatever the target does.
[[nodiscard]] CreeperStep tick_creeper(CreeperSwell& creeper, std::optional<f64> target_distance,
                                       bool sees_target) noexcept;

/// A creeper's charge: 3, or 6 when it is charged.
[[nodiscard]] constexpr f32 creeper_power(const CreeperSwell& creeper) noexcept {
    return creeper.powered ? kChargedCreeperPower : kCreeperPower;
}

/// A creeper's charge drops a broken block's loot one time in `power`.
/// Measured: 333 items for 989 blocks over twelve shots, 0.337.
inline constexpr BlockInteraction kCreeperInteraction = BlockInteraction::DestroyWithDecay;

// ── Applying an explosion ───────────────────────────────────────────────────

/// One decided explosion, and what it will do to the blocks.
///
/// Reused between explosions: the vectors keep their capacity, which is what
/// keeps a chain of fifty charges from allocating fifty times.
struct Detonation {
    /// The blocks the rays took, sorted.
    std::vector<BlockPos> blocks;
    /// The air cells they reached with energy left, sorted. Carried by the
    /// packet, destroyed by nothing.
    std::vector<BlockPos> air;

    struct Dropped {
        BlockPos pos{};
        Drop     drop{};
    };
    /// Filled by `destroy_detonation`.
    std::vector<Dropped> drops;

    struct Primed {
        BlockPos pos{};
        i32      fuse{0};
    };
    /// TNT blocks the explosion lit, with their chained fuses.
    std::vector<Primed> primed;

    void clear() noexcept {
        blocks.clear();
        air.clear();
        drops.clear();
        primed.clear();
    }
};

/// Phase one: which cells the rays take. Nothing is written.
///
/// Separate from phase two because vanilla hurts the entities **between** the
/// two: the exposure an entity is hurt through is computed while the wall it
/// is hiding behind still stands, even if that wall is about to go.
void collect_detonation(const world::LevelView& level, const Explosions& explosions,
                        const ExplosionSpec& spec, math::LegacyRandomSource& rng,
                        Detonation& out);

/// Phase two: roll the drops, light the TNT, then empty every cell.
///
/// Every state is read before any cell is written, so a two-block plant whose
/// other half is also in the crater still draws its own table. The drops come
/// from the block's own loot table with an empty hand — which is why a charge
/// in stone leaves cobblestone. `loot` may be null, and then nothing drops.
void destroy_detonation(world::LevelWriter& level, const Explosions& explosions,
                        const LootTables* loot, const ExplosionSpec& spec, registry::BlockId tnt,
                        math::LegacyRandomSource& rng, math::XoroshiroRandomSource& loot_rng,
                        Detonation& detonation);

}  // namespace ov::gameplay
