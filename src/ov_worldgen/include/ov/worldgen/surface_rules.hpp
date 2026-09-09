// The surface rules, as the datapack describes them.
//
// The noise stage fills the world with stone. Nothing in it puts grass on a
// hill, sand on a beach, gravel on a sea bed, snow on a peak, sandstone under a
// desert or bedrock at the bottom — all of that is one more expression written
// in JSON, `surface_rule` in the noise settings file, and it is *interpreted*
// rather than reimplemented, for the same reason the density graph is: a
// hand-written approximation would be wrong at the first datapack and could
// never be seed-exact. See src/ov_worldgen/src/density.cpp, which this follows.
//
// The shape of the language is small. A rule is a sequence, a condition
// guarding another rule, a block, or the badlands clay bands; a condition asks
// about the biome, a noise, the height, the water above, the steepness, the
// depth of stone under the surface, or a random gradient. The whole overworld
// is 724 conditions over 523 blocks.
//
// A type this file does not implement is refused and named. That rule has
// already earned its keep elsewhere in this project: a silently ignored
// condition is a world that looks plausible and is wrong, and nothing reports
// it.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/random.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/worldgen/noise.hpp"

#include <array>
#include <expected>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace ov::worldgen {

enum class SurfaceError : u8 {
    /// The settings file, or something it names, is not there.
    Missing,
    /// The file is not JSON, or not the shape a surface rule has.
    Malformed,
    /// A rule or condition type this interpreter does not implement. Named
    /// loudly rather than treated as "never matches".
    Unsupported,
    /// A `result_state` naming a block, or a property value, the registry does
    /// not have.
    UnknownBlock,
};

[[nodiscard]] std::string_view to_string(SurfaceError error) noexcept;

/// "no water anywhere above this position". Vanilla's sentinel is
/// Integer.MIN_VALUE and the `water` condition compares against it directly, so
/// the value itself is part of the specification rather than a convenience.
inline constexpr i32 kNoWater = std::numeric_limits<i32>::min();

/// "the terrain never rose this high". Same reasoning: `preliminary_surface`
/// returning Integer.MAX_VALUE is what makes `above_preliminary_surface` false
/// everywhere in a column of pure air.
inline constexpr i32 kNoSurface = std::numeric_limits<i32>::max();

/// What a rule can ask about a place that is not simply its coordinates.
///
/// Kept abstract on purpose. The rules have to run over two different worlds:
/// ours, where the biomes and the heightmap come from our own chunk, and the
/// game's, where a parity harness hands over what the real server wrote. An
/// interpreter that reached for a ChunkGenerator could only ever be checked
/// against itself.
class SurfaceQueries {
public:
    SurfaceQueries()                                 = default;
    SurfaceQueries(const SurfaceQueries&)            = delete;
    SurfaceQueries& operator=(const SurfaceQueries&) = delete;
    virtual ~SurfaceQueries();

    /// The biome at a block position, as a resource location.
    [[nodiscard]] virtual std::string_view biome_at(i32 x, i32 y, i32 z) const = 0;

    /// The biome's temperature at a position — what `temperature` tests
    /// against 0.15.
    [[nodiscard]] virtual f64 temperature_at(i32 x, i32 y, i32 z) const = 0;

    /// The height of the highest non-air block in a column, as WORLD_SURFACE_WG
    /// holds it during generation. `steep` reads it for the two neighbouring
    /// columns, clamped inside the chunk.
    [[nodiscard]] virtual i32 surface_height(i32 x, i32 z) const = 0;

    /// The coarse height the terrain reached before the surface stage, from
    /// `initial_density_without_jaggedness`. kNoSurface where the column never
    /// became solid.
    [[nodiscard]] virtual i32 preliminary_surface(i32 x, i32 z) const = 0;
};

/// Everything the rules read about one position.
///
/// A plain aggregate rather than an object with accessors: it is rebuilt for
/// every block of every column — about a hundred thousand times per chunk — and
/// the column walk fills it in place.
struct SurfaceContext {
    const SurfaceQueries* queries{nullptr};

    i32 x{0};
    i32 y{0};
    i32 z{0};

    /// How thick the soft layer is here, from the surface noise. Per column,
    /// not per block. Zero or less is a "hole" — the rules use it to leave
    /// bare stone showing through grass.
    i32 surface_depth{0};

    /// The raw secondary surface noise for this column, in [-1, 1]. Mapped
    /// onto `secondary_depth_range` by the `stone_depth` condition; kept raw
    /// because the mapping's endpoints are part of that condition rather than
    /// of the column.
    f64 secondary_depth{0.0};

    /// Blocks of stone from the top of this run down to here, inclusive. Reset
    /// by air and by fluid.
    i32 stone_depth_above{0};
    /// The same downwards: from here to the bottom of the run.
    i32 stone_depth_below{0};

    /// The first air above the water column standing over this position, or
    /// kNoWater when nothing here is under water.
    i32 water_height{kNoWater};

    i32 min_y{-64};
    i32 height{384};
};

/// A rule: what block, if any, this position becomes.
class SurfaceRule {
public:
    SurfaceRule()                              = default;
    SurfaceRule(const SurfaceRule&)            = delete;
    SurfaceRule& operator=(const SurfaceRule&) = delete;
    virtual ~SurfaceRule();

    /// Nothing is not air. It means no rule claimed this position and it keeps
    /// the stone the noise put there.
    [[nodiscard]] virtual std::optional<registry::BlockStateId> apply(
        const SurfaceContext& at) const = 0;
};

class SurfaceCondition {
public:
    SurfaceCondition()                                   = default;
    SurfaceCondition(const SurfaceCondition&)            = delete;
    SurfaceCondition& operator=(const SurfaceCondition&) = delete;
    virtual ~SurfaceCondition();

    [[nodiscard]] virtual bool test(const SurfaceContext& at) const = 0;
};

using SurfaceRuleRef = std::shared_ptr<const SurfaceRule>;

/// Where the interpreter gets the seeded things a rule needs.
///
/// The noises, the random factories and the clay bands all depend on the world
/// seed, and none of them belongs to the rule tree — SurfaceSystem owns them
/// and hands them over through this. It is also what keeps the parser free of
/// any opinion about how a noise is seeded.
class SurfaceResources {
public:
    SurfaceResources()                                   = default;
    SurfaceResources(const SurfaceResources&)            = delete;
    SurfaceResources& operator=(const SurfaceResources&) = delete;
    virtual ~SurfaceResources();

    /// A named normal noise, seeded from the world seed. Null when the
    /// datapack names one that has no file.
    [[nodiscard]] virtual const NormalNoise* noise(std::string_view name) = 0;

    /// A positional generator factory under a name, for `vertical_gradient`.
    [[nodiscard]] virtual math::XoroshiroPositionalFactory random_factory(
        std::string_view name) = 0;

    /// Resolve a `result_state` to a block state id.
    [[nodiscard]] virtual std::optional<registry::BlockStateId> block_state(
        std::string_view                                               name,
        std::span<const std::pair<std::string_view, std::string_view>> properties) = 0;

    /// The badlands clay band at a position: 192 bands of coloured terracotta,
    /// shifted vertically by a noise.
    [[nodiscard]] virtual registry::BlockStateId clay_band(i32 x, i32 y, i32 z) const = 0;
};

/// Build a rule tree from the `surface_rule` member of a noise settings file.
///
/// The path is the settings file itself — `worldgen/noise_settings/
/// overworld.json` — because that is where the rule lives; nothing else about
/// the file is read here.
[[nodiscard]] std::expected<SurfaceRuleRef, SurfaceError> load_surface_rule(
    const std::filesystem::path& settings_file, SurfaceResources& resources);

/// Resolve one of the datapack's three ways of naming a height.
///
/// `absolute` is a y; `above_bottom` counts up from the bottom of the world and
/// `below_top` down from the top. The bedrock floor is written with the first
/// and the deepslate transition with the second, so both are load-bearing.
[[nodiscard]] i32 resolve_anchor_absolute(i32 value) noexcept;
[[nodiscard]] i32 resolve_anchor_above_bottom(i32 value, i32 min_y) noexcept;
[[nodiscard]] i32 resolve_anchor_below_top(i32 value, i32 min_y, i32 height) noexcept;

/// The 192 clay bands of the badlands, in the order the generator draws them.
///
/// Exposed because the order of the draws is the whole of it: the same five
/// colours in a different order is a different mountain, and the only way to
/// check the order is to compare the table against one the game produced.
/// `terracotta` and the five coloured variants are passed in rather than looked
/// up, so the function stays free of the registry.
struct ClayBandColours {
    registry::BlockStateId terracotta{};
    registry::BlockStateId orange{};
    registry::BlockStateId yellow{};
    registry::BlockStateId brown{};
    registry::BlockStateId red{};
    registry::BlockStateId white{};
    registry::BlockStateId light_gray{};
};

inline constexpr usize kClayBandCount = 192;

[[nodiscard]] std::array<registry::BlockStateId, kClayBandCount> generate_clay_bands(
    math::XoroshiroRandomSource& random, const ClayBandColours& colours);

}  // namespace ov::worldgen
