// ── nether-2 ── What the Nether's mobs do that no other mob does.
//
// Rules only, as the rest of this module: numbers, clocks and draws, with no
// knowledge of players, sockets or chunk maps. The server's session
// (src/ov_server/src/nether_mobs.*) owns the entities and calls these.
//
// Sources: the Minecraft Wiki pages "Piglin" (bartering, the six-second
// admiration), "Blaze" (the three-shot volley), "Ghast" (the charge and the
// fireball), "Fireball"/"Small Fireball" (a projectile pushed by a constant
// acceleration and slowed by 0.95 a tick) — and, for every number the pages do
// not give, the real 1.20.1 server (scripts/measure_nether_mobs.py,
// docs/provenance/nether-2.md § 3). No game code and no third-party code.
#pragma once

#include "ov/base/types.hpp"
#include "ov/entity/logic.hpp"
#include "ov/entity/world.hpp"
#include "ov/math/random.hpp"
#include "ov/math/vec.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace ov::gameplay {

// ── Bartering ───────────────────────────────────────────────────────────────

/// One line of `loot_tables/gameplay/piglin_bartering.json`: an item, its
/// weight, its count range, and the one function it may carry.
struct BarterEntry {
    std::string item;
    i32         weight{1};
    i32         min_count{1};
    i32         max_count{1};
    /// `set_potion`: the potion's name, empty for none.
    std::string potion;
    /// `enchant_randomly` restricted to Soul Speed.
    bool soul_speed{false};
};

/// What one gold ingot bought.
struct BarterDrop {
    std::string_view item;
    i32              count{1};
    std::string_view potion;
    /// 1..3 when the item carries Soul Speed (a book becomes an enchanted
    /// book); 0 otherwise.
    i32 soul_speed_level{0};
};

/// The table, drawn once per ingot: one weighted entry, then its count, then
/// its enchantment level — in that order, each a draw of its own.
class BarterTable {
public:
    BarterTable() = default;
    explicit BarterTable(std::vector<BarterEntry> entries);

    [[nodiscard]] bool  empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] i32   total_weight() const noexcept { return total_; }
    [[nodiscard]] const std::vector<BarterEntry>& entries() const noexcept { return entries_; }

    [[nodiscard]] BarterDrop draw(math::LegacyRandomSource& random) const;

private:
    std::vector<BarterEntry> entries_;
    i32                      total_{0};
};

/// How long a piglin admires a gold ingot before it pays: six seconds
/// (minecraft.wiki "Piglin"; measured, nether-2.md § 3.2).
inline constexpr i32 kAdmireTicks = 120;

// ── The blaze's volley ──────────────────────────────────────────────────────

/// The blaze's attack clock (minecraft.wiki "Blaze": it charges, then fires
/// three small fireballs in quick succession, then rests). Ticked once per
/// game tick while the blaze has a target it can see within its follow range.
struct BlazeVolley {
    i32 step{0};
    i32 time{0};
    /// Set while charging and shooting: the blaze's metadata flag.
    bool charged{false};

    /// One tick. True when a small fireball goes out this tick.
    bool tick(bool engaged) noexcept;
};

inline constexpr i32 kBlazeChargeTicks  = 60;
inline constexpr i32 kBlazeShotSpacing  = 6;
inline constexpr i32 kBlazeRestTicks    = 100;
inline constexpr i32 kBlazeShotsPerVolley = 3;

// ── The ghast's charge ──────────────────────────────────────────────────────

/// The ghast's charge: it builds while the ghast sees its target within 64
/// blocks, shrieks at 10, fires at 20 and starts again from -40.
struct GhastCharge {
    i32 charge{0};

    /// One tick. True when a fireball goes out this tick.
    bool tick(bool engaged) noexcept;
    /// The metadata flag "attacking": the open-mouthed face.
    [[nodiscard]] bool attacking() const noexcept { return charge > 10; }
};

inline constexpr f64 kGhastSightRange = 64.0;

// ── Fireballs ───────────────────────────────────────────────────────────────

/// A hurting projectile's acceleration: the direction it was aimed in,
/// normalised, times 0.1. Zero for a zero direction.
[[nodiscard]] Vec3d fireball_power(Vec3d direction) noexcept;

/// One tick of flight: moved by its velocity, then the velocity gains the
/// acceleration and loses 5 % (20 % in water).
void step_fireball(Vec3d& position, Vec3d& velocity, Vec3d power, bool in_water) noexcept;

inline constexpr f32 kSmallFireballDamage = 5.0F;
inline constexpr i32 kSmallFireballBurnSeconds = 5;
inline constexpr f32 kLargeFireballDamage = 6.0F;
inline constexpr f32 kGhastExplosionPower = 1.0F;

// ── The ghast's flight ──────────────────────────────────────────────────────

/// A ghast floats: a destination drawn up to 16 blocks away on each axis when
/// it has none, has reached it or is more than 60 away, and a push of 0.1
/// towards it every 2 to 6 ticks, under the air's 0.91 drag and no gravity.
/// Collisions are the ordinary box's (`step_entity`); `canReach` — the game's
/// sweep of the whole route before each push — is approximated by the
/// destination's box being free, named in nether-2.md.
class GhastFlight final : public entity::IEntityLogic {
public:
    explicit GhastFlight(i64 seed) noexcept : random_{seed} {}

    void tick(entity::EntityWorld& world, entity::EntityHandle self,
              const entity::TickContext& context) override;

    [[nodiscard]] std::string_view name() const noexcept override { return "minecraft:ghast"; }

    /// Look at a target (a fireball is about to be aimed): the body turns.
    void face(Vec3d at) noexcept {
        look_       = at;
        has_look_   = true;
    }

private:
    math::LegacyRandomSource random_;
    Vec3d                    wanted_{};
    bool                     has_wanted_{false};
    i32                      float_duration_{0};
    Vec3d                    look_{};
    bool                     has_look_{false};
};

// ── The strider ─────────────────────────────────────────────────────────────

/// A strider out of lava is cold: it shivers (its metadata flag) and walks
/// slower — 0.66 of its warm pace, measured (0.02880 against 0.06610 b/t).
[[nodiscard]] constexpr bool strider_cold(bool in_lava) noexcept { return !in_lava; }

struct MobKind;
/// The strider's kind while cold (mob_species.cpp).
[[nodiscard]] const MobKind& strider_cold_kind() noexcept;

}  // namespace ov::gameplay
