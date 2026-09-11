// Fire: the block that spreads, the lava that lights it, and what burns.
//
// Fire is not a timer either. A fire block asks for a block tick when it is
// placed — 30 ticks plus a draw of ten — and every time the tick comes due it
// does five things, in this order: asks for the next one; checks it may still
// stand; lets the rain put it out; ages; and then rolls against each of its six
// neighbours to burn it and against every empty cell in a 3x3x6 box above it to
// set it alight. How likely each roll is comes from two numbers per block, the
// **ignite odds** ("encouragement") and the **burn odds** ("flammability"). A
// plank is 5 and 20: slow to catch from a distance, quick to be eaten once
// fire sits on it.
//
// ── Where the numbers come from ─────────────────────────────────────────────
//
// The per-block table and the formulas are the Minecraft Wiki's "Fire" article
// (docs/provenance/feu.md cites it). scripts/measure_fire.py measures them
// against a real 1.20.1 server — the tick delay, the age steps, the life of a
// fire on stone and on netherrack, the burn and ignite odds of a sample of
// blocks, rain, lava, burning entities — and docs/provenance/feu.md holds what
// each campaign gave. A number here is the wiki's until that document says it
// was measured.
//
// ── Layer 9 ─────────────────────────────────────────────────────────────────
//
// Every rule takes a `world::LevelWriter&`. What a fire needs that a level
// cannot answer — the weather, the difficulty, the biome's humidity, the
// gamerule, a TNT to prime — is gathered into `FireEnvironment`.
//
// The entity half (`EntityFire`) takes no level at all: the caller says what
// the entity is touching and gets back the damage it owes. A mob and a player
// and a client-side prediction all use the same rules.
#pragma once

#include "ov/base/types.hpp"
#include "ov/gameplay/damage.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/math/random.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/level.hpp"

#include <optional>
#include <span>
#include <string_view>

namespace ov::gameplay {

/// The generator the fire rules draw from. Vanilla's is the level's own and is
/// seeded from the clock: parity is statistical by nature, and ours is seeded
/// explicitly (CLAUDE.md principle 5).
using FireRandom = math::XoroshiroRandomSource;

/// A block's two numbers. Zero and zero for anything fire ignores.
struct FireOdds {
    /// How strongly this block pulls fire into an empty cell next to it.
    u8 ignite{0};
    /// How likely a fire next to it is to eat it.
    u8 burn{0};
};

/// Which `#infiniburn_*` tag a dimension reads: netherrack and magma everywhere,
/// bedrock too in the End.
enum class FireDimension : u8 { Overworld, Nether, End };

/// What a fire asks the world that a `LevelWriter` cannot answer.
class FireEnvironment {
public:
    FireEnvironment()                                  = default;
    FireEnvironment(const FireEnvironment&)            = delete;
    FireEnvironment& operator=(const FireEnvironment&) = delete;
    FireEnvironment(FireEnvironment&&)                 = delete;
    FireEnvironment& operator=(FireEnvironment&&)      = delete;
    virtual ~FireEnvironment()                         = default;

    /// The `doFireTick` gamerule. Off, a fire still reschedules itself and
    /// does nothing else — which is what freezes a burning forest in place.
    [[nodiscard]] virtual bool fire_tick() const = 0;

    /// Is it raining at all? (`Level.isRaining`: the rain level above 0.2.)
    [[nodiscard]] virtual bool is_raining() const = 0;

    /// Does rain fall on this very block: raining, open to the sky, and in a
    /// biome where it rains rather than snows.
    [[nodiscard]] virtual bool is_raining_at(BlockPos pos) const = 0;

    /// Is this position in `#minecraft:increased_fire_burnout` — the jungles,
    /// swamps, mushroom fields and the three high peaks?
    [[nodiscard]] virtual bool increased_burnout(BlockPos pos) const = 0;

    /// 0 peaceful .. 3 hard. Spreading adds seven per level.
    [[nodiscard]] virtual i32 difficulty() const = 0;

    [[nodiscard]] virtual FireDimension dimension() const { return FireDimension::Overworld; }

    /// A TNT block was just burnt away at `pos`. The caller primes it.
    virtual void prime_tnt(BlockPos pos) = 0;
};

/// What one fire tick did. Instrumentation, and what the tests count.
struct FireTickResult {
    bool extinguished{false};
    bool rained_out{false};
    bool aged{false};
    u8   burnt{0};
    u8   spread{0};
};

/// The fire rules, with every registry lookup resolved once.
///
/// Stateless between calls: everything a fire remembers is its `age`.
class FireRules {
public:
    FireRules(const registry::BlockRegistry& blocks, const registry::Registries& registries);
    ~FireRules();
    FireRules(const FireRules&)            = delete;
    FireRules& operator=(const FireRules&) = delete;
    FireRules(FireRules&&) noexcept;
    FireRules& operator=(FireRules&&) noexcept;

    /// The delay a fire asks for: `kTickBase + next_int(kTickSpread)`, 30..39.
    /// The wiki's "30 to 40 game ticks"; the `blocks` campaign reads the gaps
    /// between a fire's age changes to settle whether 40 itself occurs.
    static constexpr i32 kTickBase   = 30;
    static constexpr i32 kTickSpread = 10;
    [[nodiscard]] static i64 tick_delay(FireRandom& random) noexcept;

    // ── The table ───────────────────────────────────────────────────────────

    /// Both numbers, **ignoring** a waterlogged state's zero.
    [[nodiscard]] FireOdds odds(registry::BlockId block) const noexcept;
    /// Both numbers as the fire sees this state: zero when waterlogged.
    [[nodiscard]] FireOdds odds_of(registry::BlockStateId state) const noexcept;
    /// `canBurn`: ignite odds above zero, and not waterlogged.
    [[nodiscard]] bool can_burn(registry::BlockStateId state) const noexcept;
    /// Lava lights an empty cell next to this block (vanilla's `ignitedByLava`,
    /// a set of its own — not the same set as the table above).
    [[nodiscard]] bool ignited_by_lava(registry::BlockStateId state) const noexcept;

    [[nodiscard]] bool is_fire(registry::BlockStateId state) const noexcept;
    [[nodiscard]] bool is_soul_fire(registry::BlockStateId state) const noexcept;
    /// Either of the two: `#minecraft:fire`.
    [[nodiscard]] bool is_any_fire(registry::BlockStateId state) const noexcept {
        return is_fire(state) || is_soul_fire(state);
    }
    [[nodiscard]] bool is_lava(registry::BlockStateId state) const noexcept;
    [[nodiscard]] bool is_campfire(registry::BlockStateId state) const noexcept;
    [[nodiscard]] bool is_soul_campfire(registry::BlockStateId state) const noexcept;

    /// The fire's age, 0..15, or -1 when `state` is not fire.
    [[nodiscard]] i32 age_of(registry::BlockStateId state) const noexcept;

    // ── Placement ───────────────────────────────────────────────────────────

    /// `BaseFireBlock.getState`: soul fire over `#soul_fire_base_blocks`, else
    /// fire — the default state when the block below can hold it, otherwise
    /// one face flag per flammable neighbour. Age 0.
    [[nodiscard]] registry::BlockStateId state_for(const world::LevelView& level,
                                                   BlockPos               pos) const noexcept;

    /// The same, at a given age (`getStateWithAge`). Soul fire has no age.
    [[nodiscard]] registry::BlockStateId state_with_age(const world::LevelView& level,
                                                        BlockPos pos, i32 age) const noexcept;

    /// May a fire (or soul fire) stand here? Fire: a sturdy top face below, or
    /// a flammable neighbour. Soul fire: a soul fire base below.
    [[nodiscard]] bool survives(const world::LevelView& level, BlockPos pos,
                                registry::BlockStateId fire) const noexcept;

    /// `BaseFireBlock.canBePlacedAt` without the portal half: an empty cell
    /// where the fire `state_for` would give can stand. What a flint and
    /// steel, a fire charge and a lightning bolt ask.
    [[nodiscard]] std::optional<registry::BlockStateId> placement(const world::LevelView& level,
                                                                  BlockPos pos) const noexcept;

    /// Light a fire here if one may stand — the entry point for anything that
    /// sets fire to the world from outside the rules: lightning, a fire
    /// charge, a flaming arrow on a campfire's neighbour. True when a fire was
    /// written. The fire's first tick is scheduled by `neighbour_changed`,
    /// which every write reaches.
    bool ignite(world::LevelWriter& level, BlockPos pos) const;

    // ── Ticks ───────────────────────────────────────────────────────────────

    /// A block was written at or next to `pos`. For a fire at `pos`: gone if it
    /// can no longer stand, its face flags recomputed if it can (vanilla's
    /// `updateShape`), and its first tick asked for if it has none — which is
    /// `onPlace` for a fire any path wrote.
    void neighbour_changed(world::LevelWriter& level, BlockPos pos, FireRandom& random) const;

    /// The fire's own block tick. False when `pos` does not hold a fire block
    /// (vanilla drops a tick whose block has gone).
    bool scheduled_tick(world::LevelWriter& level, FireEnvironment& env, BlockPos pos,
                        FireRandom& random, FireTickResult* result = nullptr) const;

    /// Could a random tick on this state light anything? Lava, any level.
    [[nodiscard]] bool ticks_randomly(registry::BlockStateId state) const noexcept {
        return is_lava(state);
    }

    /// Lava's random tick: one to two steps upward looking for an empty cell
    /// with a lava-ignitable neighbour, or three looks at the blocks level with
    /// it for a lava-ignitable one with room above. Returns the fires lit. A
    /// fire is written only where it can stand: vanilla writes one anywhere and
    /// takes it back at once when it cannot (measured: a fire lit above the
    /// lava under a crafting table never shows).
    i32 lava_random_tick(world::LevelWriter& level, FireEnvironment& env, BlockPos pos,
                         FireRandom& random) const;

    /// How many times a lava block's random tick runs when the chunk's random
    /// tick picks it. **Two**, measured: 1.315 fires per pick under a plank
    /// roof where one tick gives 2/3, 0.430 under a roof one higher where one
    /// gives 49/243 (docs/provenance/feu.md § 5). The caller loops.
    static constexpr i32 kLavaTicksPerPick = 2;

    /// The blocks of the table this build could not find in the registry.
    /// Zero in 1.20.1; a table written for another version says so here.
    [[nodiscard]] usize unknown_names() const noexcept;

private:
    struct Impl;
    Impl* impl_{nullptr};
};

// ── Things on fire ──────────────────────────────────────────────────────────

/// An entity's fire, as vanilla keeps it: one counter, `Fire` in NBT.
///
/// Positive: burning, one tick less each tick. Zero or below: not burning, and
/// the depth below zero is the grace a fire block gives before it lights you —
/// one tick for a mob, twenty for a player (`immune_ticks`).
struct EntityFire {
    i32 remaining{-1};
    /// `getFireImmuneTicks`: 1, and 20 for a player.
    i32 immune_ticks{1};
    /// Blazes, striders, magma cubes…: never burn, and a burning counter falls
    /// by four a tick.
    bool fire_immune{false};

    [[nodiscard]] bool on_fire() const noexcept { return remaining > 0 && !fire_immune; }
};

/// What an entity is touching this tick. The caller knows the world.
struct FireContact {
    /// Its box meets a fire or soul fire block.
    bool in_fire{false};
    bool in_soul_fire{false};
    /// Its box meets lava.
    bool in_lava{false};
    /// In water, or rained on (at its feet or at the top of its box).
    bool wet{false};
    /// In water — which clears the counter outright, before the tick.
    bool in_water{false};
    /// Its box meets a lit campfire (1) or soul campfire (2).
    u8 campfire{0};
};

/// Damage owed by one tick, by type. Every field is zero on most ticks.
struct FireDamage {
    f32 on_fire{0.0F};
    f32 in_fire{0.0F};
    f32 lava{0.0F};
};

/// The numbers the entity rules are made of. Measured on cows of 200 health,
/// read every tick (docs/provenance/feu.md § 6): a point at Fire = 200, 180,
/// …; 1 and 2 per damage window in fire and soul fire; 4 in lava, the counter
/// held at 300; 1 and 2 on the two campfires, never lit. The 8 s of a fire
/// block is the wiki's: the bench's summoned cows started their counter at 0
/// and never crossed the zero that lights them.
struct EntityFireConstants {
    f32 on_fire_damage{1.0F};
    i32 on_fire_interval{20};
    f32 fire_damage{1.0F};
    f32 soul_fire_damage{2.0F};
    f32 lava_damage{4.0F};
    i32 fire_block_seconds{8};
    i32 lava_seconds{15};
    f32 campfire_damage{1.0F};
    f32 soul_campfire_damage{2.0F};
};

/// `setSecondsOnFire`: the counter becomes the larger of itself and
/// `seconds * 20`. Fire Protection is not applied (no armour enchantments on
/// this server's entities yet — named).
void set_on_fire(EntityFire& fire, i32 seconds) noexcept;

/// One tick of an entity's fire, in vanilla's order: water clears the counter;
/// the counter falls, charging `on_fire` every twentieth tick unless in lava;
/// lava lights it for fifteen seconds and hurts; then the blocks it touches —
/// a fire block adds one to the counter and lights it for eight seconds the
/// tick the counter reaches zero, and hurts; and last, the counter is reset to
/// `-immune_ticks` when nothing hot is touching and it is not burning, or when
/// it is wet.
///
/// Damage is what the caller should *attempt*: the invulnerability window and
/// fire resistance are the caller's (see `blocks_fire_damage`).
[[nodiscard]] FireDamage tick_entity_fire(EntityFire& fire, const FireContact& contact,
                                          const EntityFireConstants& constants = {}) noexcept;

/// Whether Fire Resistance stops a damage type. Every `#is_fire` type: in_fire,
/// on_fire, lava, hot_floor, fireball, unattributed_fireball.
[[nodiscard]] bool fire_resistance_blocks(DamageKind kind) noexcept;

/// `getLightLevelDependentMagicValue` in the overworld: `l/15 / (4 - 3 l/15)`.
[[nodiscard]] f32 light_magic_value(i32 light) noexcept;

/// Does a sun-sensitive mob catch fire this tick? (`isSunBurnTick`.)
///
/// `light` is the brighter of the dimmed sky and the block light at the eyes;
/// `sky_darken` decides "day" (< 4). One draw of `next_float`, and only when
/// the other conditions pass. Measured: 32 zombies at noon lit after 21.5
/// ticks on average (the rule: 25), 32 at darken 2 after 85.4 (68.5) —
/// docs/provenance/feu.md § 6, and the KS comparison in test_fire.cpp.
[[nodiscard]] bool sun_burns(i32 light, i32 sky_darken, bool sees_sky, bool wet,
                             FireRandom& random) noexcept;

// ── Campfires ───────────────────────────────────────────────────────────────

/// One of a campfire's four cooking slots.
struct CampfireSlot {
    i32 item{-1};      ///< item protocol id, -1 empty
    i32 progress{0};   ///< CookingTimes
    i32 total{0};      ///< CookingTotalTimes, from the recipe
};

/// One tick of a campfire's cooking. Lit: every occupied slot advances, and a
/// slot that reaches its total is returned (its index) to be dropped as the
/// recipe's result and emptied by the caller. Unlit: progress falls by two.
/// Returns a bit set of the slots finished this tick.
[[nodiscard]] u8 tick_campfire(std::span<CampfireSlot, 4> slots, bool lit) noexcept;

}  // namespace ov::gameplay
