// Weather as the rules see it: the climate a position has, what rain and snow
// do to the block they land on, how dark the sky is under a storm, and the
// clock a lightning bolt runs on.
//
// Everything here is a rule over a `LevelView` / `LevelWriter` and explicit
// numbers. *Which* column a tick picks, whether it is raining, which bolts
// exist and who is standing where are the server's business
// (ov_server/src/weather_session.hpp); the client asks the same climate
// question to decide between rain and snow, which is why this is layer 9 and
// not the server's.
//
// ── Where the numbers come from ────────────────────────────────────────────
//
// The Minecraft Wiki (articles Rain, Snow, Ice, Cauldron, Lightning, Biome)
// for the rules and their thresholds; the simplex noise is Gustavson's public
// "Simplex noise demystified" construction, seeded the way the game seeds its
// other noises from `java.util.Random`. Every constant that has an oracle was
// measured against the 1.20.1 server — see docs/provenance/meteo-sommeil.md,
// which also names the ones that have none.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/aabb.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/math/random.hpp"
#include "ov/math/vec.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/level.hpp"

#include <array>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ov::gameplay {

// ── Noise ───────────────────────────────────────────────────────────────────

/// Two-dimensional simplex noise over a shuffled 256-entry table.
///
/// The table and the three offsets are drawn from `java.util.Random` in the
/// order the game draws them for every noise of this family: three doubles,
/// then a Fisher–Yates pass of 256 bounded ints.
class SimplexNoise {
public:
    explicit SimplexNoise(math::LegacyRandomSource& random) noexcept;

    /// The 2-D sample. The offsets are *not* applied here; the octave stack
    /// adds them when it is asked to.
    [[nodiscard]] f64 value(f64 x, f64 y) const noexcept;

    [[nodiscard]] f64 xo() const noexcept { return xo_; }
    [[nodiscard]] f64 yo() const noexcept { return yo_; }
    [[nodiscard]] f64 zo() const noexcept { return zo_; }

private:
    [[nodiscard]] i32 p(i32 i) const noexcept {
        return permutation_[static_cast<usize>(i & 255)];
    }

    std::array<i32, 256> permutation_{};
    f64                  xo_{0.0};
    f64                  yo_{0.0};
    f64                  zo_{0.0};
};

/// A stack of simplex octaves, the lowest frequency loudest.
///
/// Only octaves at or below zero are supported: a positive octave is seeded
/// from a 3-D sample of the first one, which nothing in this project needs —
/// the three climate noises are {0}, {-2,-1,0} and {0}. Asked for one, the
/// stack says so through `supported()` rather than returning a plausible
/// number nobody measured.
class PerlinSimplexNoise {
public:
    PerlinSimplexNoise(math::LegacyRandomSource& random, std::span<const i32> octaves);

    [[nodiscard]] f64 value(f64 x, f64 y, bool use_offsets) const noexcept;

    [[nodiscard]] bool supported() const noexcept { return supported_; }

private:
    std::vector<std::optional<SimplexNoise>> levels_;
    f64                                      input_factor_{1.0};
    f64                                      value_factor_{1.0};
    bool                                     supported_{true};
};

// ── Climate ─────────────────────────────────────────────────────────────────

/// What a biome says about its weather, from its datapack entry.
struct BiomeClimate {
    f32  temperature{0.8F};
    /// `temperature_modifier: frozen` — frozen oceans and frozen rivers, whose
    /// temperature is lifted to 0.2 in noisy patches so they are not solid ice.
    bool frozen{false};
    bool has_precipitation{true};
};

/// The climate of a biome as the registry pack carries it.
[[nodiscard]] BiomeClimate climate_of(const registry::BiomeEffects& effects) noexcept;

enum class Precipitation : u8 { None, Rain, Snow };

[[nodiscard]] std::string_view to_string(Precipitation precipitation) noexcept;

/// Below this, precipitation is snow and water can freeze.
inline constexpr f32 kRainTemperature = 0.15F;

/// The overworld's sea level. The temperature starts falling 17 blocks above it.
inline constexpr i32 kOverworldSeaLevel = 63;

[[nodiscard]] constexpr bool warm_enough_to_rain(f32 temperature) noexcept {
    return temperature >= kRainTemperature;
}

/// The three fixed-seed noises the climate reads: the height noise (seed
/// 1234), the frozen-patch noise (3456) and the biome information noise
/// (2345). Built once; a value, so the server and the client each own one.
class ClimateNoise {
public:
    ClimateNoise();

    /// The temperature at a position: the biome's, lifted by the frozen
    /// patches, then lowered by 0.05 per 40 blocks above sea level + 17 with a
    /// noise of amplitude 8 blocks on the height.
    [[nodiscard]] f32 temperature_at(const BiomeClimate& biome, BlockPos pos,
                                     i32 sea_level = kOverworldSeaLevel) const noexcept;

    /// None when the biome has no precipitation, otherwise rain or snow by the
    /// temperature at this position.
    [[nodiscard]] Precipitation precipitation_at(const BiomeClimate& biome, BlockPos pos,
                                                 i32 sea_level = kOverworldSeaLevel) const noexcept;

    /// The frozen modifier alone: 0.2 inside a patch, the input outside.
    [[nodiscard]] f32 frozen_temperature(f32 temperature, BlockPos pos) const noexcept;

    /// The height noise, raw. Exposed for the tests that pin the seeding.
    [[nodiscard]] f64 height_noise(f64 x, f64 z) const noexcept;

private:
    PerlinSimplexNoise height_;
    PerlinSimplexNoise frozen_;
    PerlinSimplexNoise info_;
};

// ── The sky ─────────────────────────────────────────────────────────────────

/// `Mth.cos`: a lookup into 65536 floats, not libm. See the briefing's first
/// trap; the day/night edge at which a bed works sits on this function.
[[nodiscard]] f32 table_cos(f32 radians) noexcept;

/// The sky darkening, 0..11, with the weather: rain takes five sixteenths of
/// the light and thunder five sixteenths of what is left. `thunder` is the
/// world's thunder level; it is multiplied by the rain level here, as the game
/// does, because thunder without rain is not dark.
///
/// Four or more is night as far as a bed is concerned.
[[nodiscard]] u8 sky_darken(i64 day_time, f32 rain, f32 thunder) noexcept;

// ── Precipitation on blocks ─────────────────────────────────────────────────

/// One column a precipitation tick picked.
struct PrecipitationColumn {
    /// The first free block above MOTION_BLOCKING — where snow would lie.
    BlockPos top{};
    /// The biome at `top`. Both the freeze below and the snow on top read the
    /// biome of `top`, each at its own height.
    BiomeClimate biome{};
    /// Stored block light at `top` and at the block under it.
    u8  light_top{0};
    u8  light_below{0};
    i32 sea_level{kOverworldSeaLevel};
};

struct PrecipitationStats {
    usize froze{0};
    usize snowed{0};
    usize layered{0};
    usize cauldrons{0};
    /// Snow laid on a block with a `snowy` flag, which was set.
    usize snowy{0};
};

/// Water that freezes, snow that lies, cauldrons that fill.
class PrecipitationRules {
public:
    PrecipitationRules(const registry::BlockRegistry& blocks, const registry::Registries& registries);

    /// Source water, block light under 10, cold — and, when `at_edge`, at
    /// least one horizontal neighbour that is not water. The chunk tick asks
    /// with `at_edge`; world generation's freeze pass asks without.
    [[nodiscard]] bool should_freeze(const world::LevelView& level, BlockPos pos, f32 temperature,
                                     u8 block_light, bool at_edge) const;

    /// Air or a snow layer, block light under 10, cold, and a layer of snow
    /// would survive here.
    [[nodiscard]] bool should_snow(const world::LevelView& level, BlockPos pos, f32 temperature,
                                   u8 block_light) const;

    /// Would a snow layer stand at `pos`? Not on ice, packed ice or a barrier;
    /// always on honey, soul sand and mud; otherwise on a full top face or on
    /// a full snow block (eight layers).
    [[nodiscard]] bool snow_survives(const world::LevelView& level, BlockPos pos) const;

    /// Any water: the block, a waterlogged state, kelp, seagrass, a bubble
    /// column.
    [[nodiscard]] bool is_water(registry::BlockStateId state) const noexcept;

    /// The state one more snow tick writes at a position holding `current`:
    /// a first layer on air, one more on a layer under
    /// min(`accumulation_height`, 8), nothing otherwise.
    [[nodiscard]] std::optional<registry::BlockStateId> next_snow(registry::BlockStateId current,
                                                                  i32 accumulation_height) const;

    /// Does precipitation draw a roll on this block? The three cauldrons that
    /// can fill do — before looking at their level, which is why a full one
    /// still consumes the draw.
    [[nodiscard]] bool takes_precipitation(registry::BlockStateId state) const noexcept;

    /// A cauldron under precipitation, given the float drawn for it: rain
    /// fills water one level in twenty, snow powder snow one in ten, up to 3.
    [[nodiscard]] std::optional<registry::BlockStateId> precipitation_on(
        registry::BlockStateId state, Precipitation precipitation, f32 roll) const;

    /// One precipitation tick of a column: the freeze below `top` whatever
    /// the weather, then — while it rains — the snow on `top` and the
    /// precipitation on the block below it.
    void tick_column(world::LevelWriter& level, const PrecipitationColumn& column,
                     const ClimateNoise& climate, bool raining, i32 accumulation_height,
                     math::LegacyRandomSource& random, PrecipitationStats& stats) const;

    [[nodiscard]] registry::BlockStateId ice() const noexcept { return ice_; }

private:
    [[nodiscard]] bool in_tag(const std::optional<registry::TagId>& tag,
                              registry::BlockId                     block) const noexcept;
    [[nodiscard]] i32  level_of(registry::BlockStateId state) const noexcept;

    const registry::BlockRegistry* blocks_;
    const registry::Registries*    registries_;
    std::vector<i32>               wire_;

    registry::BlockId water_{}, bubble_column_{}, kelp_{}, kelp_plant_{}, seagrass_{},
        tall_seagrass_{}, snow_{}, cauldron_{}, water_cauldron_{}, powder_snow_cauldron_{};
    registry::BlockStateId ice_{};
    std::optional<registry::TagId> cannot_survive_on_;
    std::optional<registry::TagId> can_survive_on_;
    bool                           known_{false};
};

// ── Lightning ───────────────────────────────────────────────────────────────

/// A chunk under a thunderstorm is struck one tick in this many.
inline constexpr i32 kThunderChance = 100000;
/// How far a lightning rod reaches for a bolt, in blocks (squared distance).
inline constexpr i32 kLightningRodRange = 128;
/// What a bolt does to whatever it touches.
inline constexpr f32 kLightningDamage      = 5.0F;
inline constexpr i32 kLightningFireSeconds = 8;
/// How long a struck lightning rod stays powered, in ticks.
inline constexpr i32 kLightningRodPowerTicks = 8;

/// The box a bolt hurts: three blocks around it, and nine up.
[[nodiscard]] AABB lightning_hit_box(const Vec3d& bolt) noexcept;

/// The column a strike looks for living targets in: three blocks around the
/// picked surface block, from three below it to three above the build limit.
[[nodiscard]] AABB lightning_target_box(BlockPos top, i32 max_build_height) noexcept;

/// The nearest rod within range of `from`, by squared distance, a rod being
/// usable only when it is the highest block of its column. Ties go to the
/// first in `rods` — the order the caller found them in.
[[nodiscard]] std::optional<BlockPos> nearest_rod(BlockPos from, std::span<const BlockPos> rods) noexcept;

/// What a bolt turns an entity into.
enum class LightningConversion : u8 {
    /// The ordinary strike: set on fire, five damage.
    None,
    /// A pig becomes a zombified piglin with a golden sword (not on Peaceful).
    ZombifiedPiglin,
    /// A villager becomes a witch (not on Peaceful).
    Witch,
    /// A creeper takes the strike and becomes charged.
    ChargeCreeper,
    /// A mooshroom swaps red and brown, once per bolt, and takes no damage.
    FlipMooshroom,
    /// A turtle is killed outright.
    Kill,
};

[[nodiscard]] LightningConversion lightning_conversion(std::string_view entity_type) noexcept;

/// A bolt's own clock. It flashes one to three times, a first strike and
/// then up to two more a random handful of ticks apart, and hurts what stands
/// in its box on every tick it is lit.
struct BoltClock {
    i32 life{2};
    i32 flashes{1};
};

/// A new bolt's clock: its seed is drawn and thrown away (it only drives the
/// client's drawing), then the flash count.
[[nodiscard]] BoltClock new_bolt(math::LegacyRandomSource& random) noexcept;

struct BoltStep {
    /// The first tick: fire, the lightning rod, the thunder.
    bool strike{false};
    /// A later flash: fire again.
    bool refire{false};
    /// Lit this tick: whatever is in the box is struck.
    bool lit{false};
    /// Gone.
    bool remove{false};
};

[[nodiscard]] BoltStep tick_bolt(BoltClock& clock, math::LegacyRandomSource& random) noexcept;

}  // namespace ov::gameplay
