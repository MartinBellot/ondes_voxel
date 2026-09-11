// Where mobs come from, and where they go.
//
// A generated world with nothing in it is not a world; this is the rule that
// fills it. Vanilla's natural spawner is four nested questions and every one of
// them can be got subtly wrong in a way that looks fine:
//
//   1. how many of a category may exist at all, given how much world is ticked;
//   2. which chunk, and which position inside it;
//   3. is that position legal — floor, headroom, light, biome, distance;
//   4. and then, separately, which of the mobs already alive should stop being.
//
// ── What is measured ────────────────────────────────────────────────────────
//
// scripts/measure_mobs.py runs a real 1.20.1 server and reads two things off
// it: the light level at which monsters stop appearing, in rooms that differ in
// exactly one variable, and the number of each category the server settles at
// around one player. Both numbers are in docs/provenance/mobs.md with what they
// were measured against.
//
// What is *not* measured is the ordering of the checks and the exact per-chunk
// pack sizes. Those are ours, and stated as ours.
//
// ── Light, and why it is not on LevelView ───────────────────────────────────
//
// `world::LevelView` answers blocks. It does not answer light, and adding light
// to it would put a light engine into every replica that implements it. So the
// spawner takes a light source of its own: a two-method interface the caller
// implements over whatever it actually has. That also makes every rule below
// testable against a hand-written light field, which the unit tests do.
#pragma once

#include "ov/base/types.hpp"
#include "ov/entity/entity.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/math/random.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/level.hpp"

#include <span>
#include <string_view>
#include <vector>

namespace ov::gameplay {

/// The categories the game spawns by, in the order it lists them.
///
/// Each has its own cap, its own light rule and its own despawn distance, which
/// is why this is one enum and not a boolean called `hostile`.
enum class MobCategory : u8 {
    Monster,
    Creature,
    Ambient,
    Axolotls,
    UndergroundWaterCreature,
    WaterCreature,
    WaterAmbient,
    /// Items, projectiles, boats: never naturally spawned, never despawned by
    /// distance. Present so that a caller asking the category of an arrow gets
    /// an answer rather than a plausible `Monster`.
    Misc,
};

[[nodiscard]] std::string_view to_string(MobCategory category) noexcept;

/// The rules that follow from a category.
struct CategoryRules {
    /// How many of this category one *ticked chunk's worth* of world may hold.
    ///
    /// Vanilla's cap is expressed per category over the whole spawnable area
    /// and then compared against the number of chunks eligible to spawn: the
    /// effective limit is `cap * eligible_chunks / 289`, 289 being the chunks
    /// a 17x17 spawn square holds. The shape is reproduced here; the number is
    /// measured. See docs/provenance/mobs.md.
    i32 cap{0};

    /// Beyond this many blocks from the nearest player, remove immediately.
    i32 despawn_distance{128};

    /// Within this many blocks, never remove and reset the idle timer.
    i32 no_despawn_distance{32};

    /// Does this category spawn in water rather than on a floor?
    bool aquatic{false};

    /// The highest light level at which one of these may appear. -1 means the
    /// category does not test light at all.
    ///
    /// **Measured.** See docs/provenance/mobs.md for the rooms this came out of
    /// and for what the sky-light half of the answer turned out to be.
    i32 max_spawn_light{-1};
};

[[nodiscard]] CategoryRules rules_for(MobCategory category) noexcept;

/// The category of one entity type, by registry name.
///
/// A table rather than a derivation: nothing measured about an entity type says
/// which category it spawns in. A type that is not in the table is `Misc`, and
/// `Misc` never spawns naturally — which is the safe direction, since the
/// failure is a missing mob rather than a chicken in a cave at midnight.
[[nodiscard]] MobCategory category_of(std::string_view type_name) noexcept;

/// How the spawner reads light.
///
/// Two levels, because the rule needs both and they are not interchangeable:
/// block light is absolute, sky light is what the sky *would* give at full day
/// and has to be dimmed by the time of day before it means anything.
class LightSource {
public:
    LightSource()                              = default;
    LightSource(const LightSource&)            = delete;
    LightSource& operator=(const LightSource&) = delete;
    LightSource(LightSource&&)                 = delete;
    LightSource& operator=(LightSource&&)      = delete;
    virtual ~LightSource()                     = default;

    [[nodiscard]] virtual u8 block_light(BlockPos pos) const = 0;

    /// The stored sky light, 0..15, before the time of day is applied.
    [[nodiscard]] virtual u8 sky_light(BlockPos pos) const = 0;

    /// How much the current time of day takes off the sky light: 0 at noon,
    /// 11 at midnight. Supplied by the caller because a level does not know
    /// what time it is.
    [[nodiscard]] virtual u8 sky_darken() const = 0;

    /// The number the spawn rule actually compares.
    [[nodiscard]] u8 effective_light(BlockPos pos) const {
        const u8 sky = sky_light(pos);
        const u8 dimmed = sky > sky_darken() ? static_cast<u8>(sky - sky_darken()) : 0;
        const u8 block = block_light(pos);
        return block > dimmed ? block : dimmed;
    }
};

// ── mobs-2 ──
/// Which biome a position is in, as the index the chunk stores (the order of
/// the registry codec's `minecraft:worldgen/biome`). Supplied by the caller
/// for the same reason light is: a LevelView answers blocks.
class BiomeLookup {
public:
    BiomeLookup()                              = default;
    BiomeLookup(const BiomeLookup&)            = delete;
    BiomeLookup& operator=(const BiomeLookup&) = delete;
    BiomeLookup(BiomeLookup&&)                 = delete;
    BiomeLookup& operator=(BiomeLookup&&)      = delete;
    virtual ~BiomeLookup()                     = default;

    [[nodiscard]] virtual u16 biome_at(BlockPos pos) const = 0;
};
// ── end mobs-2 ──

/// One mob the spawner decided to create.
///
/// Returned rather than created: `ov_gameplay` may not know how a caller
/// numbers its entities or where it keeps them. The caller spawns these into
/// its own entity world.
struct SpawnRequest {
    std::string_view type_name;
    Vec3d            position{};
    MobCategory      category{MobCategory::Monster};
    /// Mobs of a pack share a spawn attempt and appear next to one another.
    /// Kept so a caller can group them, and so a test can assert that a pack is
    /// a pack rather than four unrelated cows.
    i32 pack{0};
};

/// What a spawn attempt needs to know about the world around it.
struct SpawnEnvironment {
    world::LevelView*  level{nullptr};
    const LightSource* light{nullptr};

    /// Where the players are. Their positions decide the cap's denominator,
    /// the minimum distance, and every despawn.
    std::span<const Vec3d> players;

    /// The chunks that are ticked, from the caller's ChunkMap. `ov_gameplay`
    /// may not name a ChunkMap — it is the server's — so the list arrives as
    /// positions.
    std::span<const ChunkPos> ticking_chunks;

    /// How many entities of each category are already alive.
    std::span<const i32> live_per_category;

    /// The registries, for turning a type name into a size and a hitbox.
    const registry::Registries* registries{nullptr};

    /// The world age, in ticks.
    ///
    /// One category is gated on it. Passive mobs do not attempt to spawn every
    /// tick like everything else: the game runs their pass **once every 400
    /// ticks**, which is why a plain sat by a river fills with cows over
    /// minutes rather than seconds. Without the gate the spawner offers a
    /// passive attempt four hundred times too often, the category cap fills in
    /// the first second of a world and nothing ever spawns again.
    ///
    /// -1 means "the caller keeps no clock", and it is **not** treated as tick
    /// zero — which would make the gate fire on every call. The spawner then
    /// counts its own calls instead, which is exactly as good a clock as long
    /// as `spawn_tick` means what it says.
    i64 game_time{-1};

    // ── mobs-2 ──
    /// The biome of a position. Null: the category-wide lists of
    /// `set_entries` are used everywhere, as before this existed.
    const BiomeLookup* biomes{nullptr};
    /// For slime chunks.
    i64 world_seed{0};
    /// For the moon phase a swamp slime is drawn against. -1: no moon, so no
    /// swamp slime — refused rather than assumed full.
    i64 day_time{-1};

    // ── nether-2 ──
    /// The Nether's rules (the_nether dimension type): no category-wide light
    /// gate — each type asks its own predicate, most of them none at all — and a
    /// monster's darkness is a sky draw and a constant light level of 7, with no
    /// block-light limit. See docs/provenance/nether-2.md § 3.
    bool nether{false};
    /// The highest y an attempt is drawn at: vanilla draws between the floor of
    /// the world and one above the column's WORLD_SURFACE, which in the Nether is
    /// the bedrock roof (128), not the top of the level (256). The default keeps
    /// the overworld's draw over the whole height, named in mobs-2.md § 8.
    i32 spawn_top{2147483647};
};

/// Which mob types a category may put in a biome, and how many at a time.
///
/// The real game reads this from the biome's `spawners` in the datapack. This
/// project has not parsed that yet, and rather than inventing a plausible
/// distribution the spawner takes the list from the caller — so a test supplies
/// one explicitly, and a future datapack reader supplies the real one, and
/// neither is silently substituted for the other.
struct SpawnerEntry {
    std::string_view type_name;
    /// Relative weight within its category.
    i32 weight{1};
    i32 min_group{1};
    i32 max_group{4};
};

/// The natural spawner.
///
/// One per level, on the tick thread. Holds its scratch for the life of the
/// object: an attempt runs many times a second and must not allocate.
class NaturalSpawner {
public:
    explicit NaturalSpawner(u64 seed = 0);

    /// The entries a category may draw from. Replaced wholesale rather than
    /// merged, so a caller cannot half-configure one.
    void set_entries(MobCategory category, std::span<const SpawnerEntry> entries);

    /// The effective cap for a category, given how much world is ticked.
    ///
    /// `cap * eligible_chunks / 289`. Public because it is the number a server
    /// operator actually wants to see, and because a test can then assert the
    /// scaling without running a spawn pass.
    [[nodiscard]] static i32 effective_cap(MobCategory category, usize eligible_chunks) noexcept;

    /// Run one tick's worth of spawning. Appends to `out`.
    ///
    /// Vanilla runs this once per tick over every eligible chunk, with the
    /// category caps checked once at the start. So does this.
    void spawn_tick(const SpawnEnvironment& environment, std::vector<SpawnRequest>& out);

    /// Is this a position a mob of this category may stand at?
    ///
    /// Split out from the tick because it is what the tests are written
    /// against: a rule that can only be exercised by running a whole spawn pass
    /// is a rule nobody checks.
    [[nodiscard]] bool can_spawn_at(const SpawnEnvironment& environment, MobCategory category,
                                    BlockPos pos, f32 width, f32 height) const;

    /// Everything `can_spawn_at` asks that does **not** depend on the mob.
    ///
    /// Loaded, inside the world, far enough from every player, dark enough, and
    /// — for a passive — on grass under sky. None of it needs a hitbox, so all
    /// of it can be answered before a type has been drawn.
    ///
    /// That order matters twice. It saves the draw on the vast majority of
    /// positions, which are underground or under a player's feet; and the draw
    /// was the only hot caller `registry::Registries::protocol_id` had. Asking
    /// the cheap half first is why a name → id hash index is not needed to make
    /// the spawn pass fast.
    [[nodiscard]] bool position_plausible(const SpawnEnvironment& environment,
                                          MobCategory category, BlockPos pos) const;

    /// How often the passive pass runs, in ticks. Vanilla's `gameTime % 400`.
    static constexpr i64 kPassiveSpawnInterval = 400;

    /// The distance below which no mob is ever placed, whatever else is true.
    static constexpr f64 kMinimumPlayerDistance = 24.0;

    /// How many positions one chunk is offered per tick.
    static constexpr i32 kAttemptsPerChunk = 3;

    // ── mobs-2 ──
    /// One biome's list for a category, from its `spawners`. A biome with a
    /// list is drawn from wherever a position lies in it — the game draws the
    /// type from the biome **of the position**, not of the player.
    void set_biome_entries(u16 biome, MobCategory category, std::span<const SpawnerEntry> entries);
    /// Whether the biome is in `#minecraft:allows_surface_slime_spawns`.
    void set_surface_slimes(u16 biome, bool allowed);
    /// The list a category draws from at a position: the biome's when biomes
    /// are known, the category-wide one otherwise.
    [[nodiscard]] const std::vector<SpawnerEntry>& entries_at(const SpawnEnvironment& environment,
                                                              MobCategory category,
                                                              BlockPos    pos) const noexcept;
    /// `can_spawn_at`, and then the type's own predicate (spawn_rules.hpp):
    /// its floor, the sky, the slime rules, the darkness draw. Draws from the
    /// spawner's own source, which is why it is not const.
    [[nodiscard]] bool can_spawn_type_at(const SpawnEnvironment& environment,
                                         MobCategory category, std::string_view type_name,
                                         BlockPos pos, f32 width, f32 height);
    // ── end mobs-2 ──

private:
    [[nodiscard]] const std::vector<SpawnerEntry>& entries(MobCategory category) const noexcept;

    // ── mobs-2 ──
    struct BiomeLists {
        std::vector<SpawnerEntry> lists[8];
        bool                      surface_slimes{false};
    };
    [[nodiscard]] bool category_has_entries(MobCategory category) const noexcept;
    [[nodiscard]] bool floor_in_tag(const SpawnEnvironment& environment, BlockPos floor,
                                    std::string_view tag) const;
    std::vector<BiomeLists> biomes_;
    // ── end mobs-2 ──

    math::LegacyRandomSource   random_;
    std::vector<SpawnerEntry>  entries_[8];
    i32                        next_pack_{1};
    /// Calls to `spawn_tick`, used as a clock when the caller supplies none.
    i64 ticks_{0};
};

/// What should happen to a mob that already exists.
enum class DespawnDecision : u8 {
    /// Leave it alone.
    Keep,
    /// Remove it now: too far from every player.
    Immediate,
    /// Remove it because the random check fired at an intermediate distance.
    Random,
};

/// Decide the fate of one mob.
///
/// `persistence_required` is the NBT flag: a mob that has been named, leashed,
/// or spawned by a command is never removed by distance. `idle_ticks` is how
/// long it has gone without a player nearby.
///
/// The `Random` branch is what keeps a world from filling up: between the two
/// distances a mob is removed with a small probability per tick, so a player
/// walking away leaves a thinning crowd behind rather than a wall of mobs that
/// vanishes all at once when they cross an exact radius.
[[nodiscard]] DespawnDecision decide_despawn(MobCategory category, f64 distance_to_nearest_player,
                                             bool persistence_required, i32 idle_ticks,
                                             math::LegacyRandomSource& random);

/// One in this many ticks, a mob at an intermediate distance is removed.
///
/// Ours, not measured: what *was* measured is that a vanilla mob between the
/// two distances does go away on its own, and that a persistent one does not.
/// The rate is chosen so that the half-life is about half a minute, which is
/// what the caps campaign's plateau implies but does not pin down.
inline constexpr i32 kRandomDespawnOdds = 800;

/// How long a mob must have gone without a player nearby before the random
/// branch may fire at all.
inline constexpr i32 kIdleTicksBeforeDespawn = 600;

}  // namespace ov::gameplay
