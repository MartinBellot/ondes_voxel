#define OV_LOG_CATEGORY "worldgen"

// The caves. For now: `geode`.
//
// The other cave types — dripstone, the lush caves' patches and roots, glow
// lichen, sculk — are still left unclaimed here and so refused by name in
// feature.cpp.

#include "overworld_feature.hpp"

#include "ov/base/log.hpp"
#include "ov/worldgen/noise.hpp"

#include <array>
#include <cmath>
#include <utility>

namespace ov::worldgen {

namespace {

constexpr std::array<std::array<i32, 3>, 6> kDirections{{
    {0, -1, 0},  // down
    {0, 1, 0},   // up
    {0, 0, -1},  // north
    {0, 0, 1},   // south
    {-1, 0, 0},  // west
    {1, 0, 0},   // east
}};

constexpr std::array<std::string_view, 6> kFacings{"down", "up", "north", "south", "west", "east"};

/// The geode's shell noise: `NormalNoise.create(new WorldgenRandom(new
/// LegacyRandomSource(worldSeed)), -4, 1.0)`.
///
/// One octave at 1/16, twice — the two stacks each take their positional
/// factory from one `nextLong` of the *legacy* generator, and the octave is
/// seeded by the Java string hash of its name. noise.cpp builds its stacks on
/// Xoroshiro only, so this small one is built here rather than widening a file
/// the nether work also uses.
class GeodeNoise {
public:
    explicit GeodeNoise(i64 world_seed) {
        math::LegacyRandomSource random{world_seed};
        first_  = make_octave(random);
        second_ = make_octave(random);
    }

    [[nodiscard]] f64 value(f64 x, f64 y, f64 z) const noexcept {
        const f64 a = sample(*first_, x, y, z);
        const f64 b = sample(*second_, x * NormalNoise::kInputFactor, y * NormalNoise::kInputFactor,
                             z * NormalNoise::kInputFactor);
        // One non-zero octave: the deviation is 0.1 × (1 + 1/1), and the
        // factor is (1/6) / 0.2.
        constexpr f64 kFactor = (1.0 / 6.0) / (0.1 * (1.0 + 1.0 / 1.0));
        return (a + b) * kFactor;
    }

private:
    [[nodiscard]] static std::unique_ptr<ImprovedNoise> make_octave(math::LegacyRandomSource& random) {
        const math::LegacyPositionalFactory factory{static_cast<u64>(random.next_long())};
        auto                                source = factory.from_hash_of("octave_-4");
        return std::make_unique<ImprovedNoise>(source);
    }

    /// `PerlinNoise.getValue` for a single octave at `firstOctave = -4`:
    /// input factor 2⁻⁴, value factor 2⁰ / (2¹ − 1) = 1, amplitude 1.
    [[nodiscard]] static f64 sample(const ImprovedNoise& octave, f64 x, f64 y, f64 z) noexcept {
        constexpr f64 kInput = 1.0 / 16.0;
        return octave.noise(PerlinNoise::wrap(x * kInput), PerlinNoise::wrap(y * kInput),
                            PerlinNoise::wrap(z * kInput));
    }

    std::unique_ptr<ImprovedNoise> first_;
    std::unique_ptr<ImprovedNoise> second_;
};

struct GeodeConfig {
    StateProviderRef filling;
    StateProviderRef inner_layer;
    StateProviderRef alternate_inner_layer;
    StateProviderRef middle_layer;
    StateProviderRef outer_layer;
    std::vector<registry::BlockStateId> inner_placements;
    std::vector<u16> cannot_replace;
    std::vector<u16> invalid_blocks;

    f64 filling_layer{1.7};
    f64 inner_layer_size{2.2};
    f64 middle_layer_size{3.2};
    f64 outer_layer_size{4.2};

    f64 generate_crack_chance{1.0};
    f64 base_crack_size{2.0};
    i32 crack_point_offset{2};

    bool placements_require_layer0_alternate{true};
    f64  use_potential_placements_chance{0.35};
    f64  use_alternate_layer0_chance{0.0};

    IntProviderRef outer_wall_distance;
    i32            outer_wall_max{6};
    IntProviderRef distribution_points;
    IntProviderRef point_offset;
    i32            min_gen_offset{-16};
    i32            max_gen_offset{16};
    f64            noise_multiplier{0.05};
    i32            invalid_blocks_threshold{1};

    registry::BlockId water{0};
};

/// `geode`: an amethyst geode.
///
/// A handful of distribution points around the origin; each block of a
/// 33-wide cube sums `1 / sqrt(distance² + offset)` over the points, plus a
/// little noise, and the sum picks the layer — air in the middle, then
/// amethyst (budding, now and then), calcite, smooth basalt. A crack is a
/// second set of three points that carves air through the shell.
///
/// Not reproduced: the fluid ticks the game schedules next to the carved
/// crack. Worldgen fluid ticks are not simulated anywhere in this generator;
/// they move no block at the moment of placement.
class GeodeFeature final : public Feature {
public:
    explicit GeodeFeature(GeodeConfig config) : config_(std::move(config)) {}

    [[nodiscard]] std::string_view type_name() const override { return "geode"; }

    bool place(const FeatureContext& context, FeatureLevel& level, FeatureRandom& random,
               BlockPos origin) const override {
        const auto& blocks = *context.blocks;
        const auto& c      = config_;

        const i32 points = c.distribution_points->sample(random);
        const GeodeNoise noise{context.level_seed};

        const f64 d = static_cast<f64>(points) / static_cast<f64>(c.outer_wall_max);
        const f64 filling = 1.0 / std::sqrt(c.filling_layer);
        const f64 inner   = 1.0 / std::sqrt(c.inner_layer_size + d);
        const f64 middle  = 1.0 / std::sqrt(c.middle_layer_size + d);
        const f64 outer   = 1.0 / std::sqrt(c.outer_layer_size + d);
        const f64 crack_draw = random.next_double();
        const f64 crack_size =
            1.0 / std::sqrt(c.base_crack_size + crack_draw / 2.0 + (points > 3 ? d : 0.0));
        const f32  crack_roll = random.next_float();
        const bool cracked    = static_cast<f64>(crack_roll) < c.generate_crack_chance;

        std::vector<std::pair<BlockPos, i32>> centres;
        i32 invalid = 0;
        for (i32 n = 0; n < points; ++n) {
            const i32 ox = c.outer_wall_distance->sample(random);
            const i32 oy = c.outer_wall_distance->sample(random);
            const i32 oz = c.outer_wall_distance->sample(random);
            const BlockPos point = origin.offset(ox, oy, oz);
            const auto block = blocks.block_of(level.block_at(point.x, point.y, point.z));
            if (blocks.is_air(block) || holds(c.invalid_blocks, block)) {
                if (++invalid > c.invalid_blocks_threshold) {
                    return false;
                }
            }
            const i32 offset = c.point_offset->sample(random);
            centres.emplace_back(point, offset);
        }

        std::vector<BlockPos> crack_points;
        if (cracked) {
            const i32 side  = random.next_int(4);
            const i32 reach = points * 2 + 1;
            const i32 cx    = side == 0 || side == 2 ? reach : 0;
            const i32 cz    = side == 1 || side == 2 ? reach : 0;
            crack_points.push_back(origin.offset(cx, 7, cz));
            crack_points.push_back(origin.offset(cx, 5, cz));
            crack_points.push_back(origin.offset(cx, 1, cz));
        }

        const auto set_safe = [&](BlockPos at, registry::BlockStateId state) {
            const auto here = blocks.block_of(level.block_at(at.x, at.y, at.z));
            if (!holds(c.cannot_replace, here)) {
                (void)level.set_block(at.x, at.y, at.z, state);
            }
        };

        std::vector<BlockPos> potential;
        // `BlockPos.betweenClosed`: x fastest, then y, then z.
        for (i32 z = origin.z + c.min_gen_offset; z <= origin.z + c.max_gen_offset; ++z) {
            for (i32 y = origin.y + c.min_gen_offset; y <= origin.y + c.max_gen_offset; ++y) {
                for (i32 x = origin.x + c.min_gen_offset; x <= origin.x + c.max_gen_offset; ++x) {
                    const BlockPos at{x, y, z};
                    const f64 wobble = noise.value(static_cast<f64>(x), static_cast<f64>(y),
                                                   static_cast<f64>(z)) *
                                       c.noise_multiplier;
                    f64 shell = 0.0;
                    for (const auto& [point, offset] : centres) {
                        shell += 1.0 / std::sqrt(dist_sqr(at, point) + static_cast<f64>(offset)) +
                                 wobble;
                    }
                    f64 split = 0.0;
                    for (const BlockPos& point : crack_points) {
                        split += 1.0 / std::sqrt(dist_sqr(at, point) +
                                                 static_cast<f64>(c.crack_point_offset)) +
                                 wobble;
                    }
                    if (shell < outer) {
                        continue;
                    }
                    if (cracked && split >= crack_size && shell < filling) {
                        set_safe(at, blocks.default_state(*blocks.find_block("minecraft:air")));
                        continue;
                    }
                    if (shell >= filling) {
                        set_safe(at, c.filling->state(level, random, at));
                        continue;
                    }
                    if (shell >= inner) {
                        const f32  roll      = random.next_float();
                        const bool alternate = static_cast<f64>(roll) < c.use_alternate_layer0_chance;
                        set_safe(at, alternate ? c.alternate_inner_layer->state(level, random, at)
                                               : c.inner_layer->state(level, random, at));
                        if (!c.placements_require_layer0_alternate || alternate) {
                            const f32 chance = random.next_float();
                            if (static_cast<f64>(chance) < c.use_potential_placements_chance) {
                                potential.push_back(at);
                            }
                        }
                        continue;
                    }
                    if (shell >= middle) {
                        set_safe(at, c.middle_layer->state(level, random, at));
                        continue;
                    }
                    set_safe(at, c.outer_layer->state(level, random, at));
                }
            }
        }

        // A bud on the first face of each chosen block that opens onto air or
        // a water source, facing out of it, in the order of `Direction`.
        for (const BlockPos& at : potential) {
            auto state = c.inner_placements[static_cast<usize>(
                random.next_int(static_cast<i32>(c.inner_placements.size())))];
            for (usize face = 0; face < kDirections.size(); ++face) {
                state = with_property_value(blocks, state, "facing", kFacings[face]);
                const BlockPos next = at.offset(kDirections[face][0], kDirections[face][1],
                                                kDirections[face][2]);
                const auto next_state = level.block_at(next.x, next.y, next.z);
                const bool source     = is_water_source(blocks, next_state);
                state = with_property_value(blocks, state, "waterlogged", source ? "true" : "false");
                const auto next_block = blocks.block_of(next_state);
                const bool open = blocks.is_air(next_block) ||
                                  (next_block == c.water && source);
                if (!open) {
                    continue;
                }
                set_safe(next, state);
                break;
            }
        }
        return true;
    }

private:
    [[nodiscard]] static f64 dist_sqr(BlockPos a, BlockPos b) noexcept {
        const f64 dx = static_cast<f64>(a.x) - static_cast<f64>(b.x);
        const f64 dy = static_cast<f64>(a.y) - static_cast<f64>(b.y);
        const f64 dz = static_cast<f64>(a.z) - static_cast<f64>(b.z);
        return dx * dx + dy * dy + dz * dz;
    }

    /// A water source: still water at level 0, or anything waterlogged.
    [[nodiscard]] static bool is_water_source(const registry::BlockRegistry& blocks,
                                              registry::BlockStateId           state) {
        const auto block = blocks.block_of(state);
        if (blocks.block_name(block) == "minecraft:water") {
            const auto level = blocks.find_property(block, "level");
            return !level || blocks.property_value(state, *level) == "0";
        }
        const auto waterlogged = blocks.find_property(block, "waterlogged");
        return waterlogged && blocks.property_value(state, *waterlogged) == "true";
    }

    GeodeConfig config_;
};

[[nodiscard]] std::expected<IntProviderRef, FeatureError> int_field(Json node, std::string_view key) {
    auto field = node.at_key(key);
    if (field.error() != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    return parse_int_provider(field.value());
}

[[nodiscard]] std::expected<StateProviderRef, FeatureError> provider_field(
    Json node, std::string_view key, const registry::BlockRegistry& blocks, const BlockTags& tags) {
    auto field = node.at_key(key);
    if (field.error() != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    return parse_state_provider(field.value(), blocks, tags);
}

[[nodiscard]] f64 number(Json node, std::string_view key, f64 fallback) {
    f64 value = fallback;
    if (node.at_key(key).get(value) == simdjson::SUCCESS) {
        return value;
    }
    i64 whole = 0;
    if (node.at_key(key).get(whole) == simdjson::SUCCESS) {
        return static_cast<f64>(whole);
    }
    return fallback;
}

[[nodiscard]] ClaimedFeature parse_geode(Json config, const registry::BlockRegistry& blocks,
                                         const BlockTags& tags) {
    GeodeConfig c;
    auto block_settings = config.at_key("blocks");
    auto layers         = config.at_key("layers");
    auto crack          = config.at_key("crack");
    if (block_settings.error() != simdjson::SUCCESS || layers.error() != simdjson::SUCCESS ||
        crack.error() != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    const Json b = block_settings.value();

    auto filling = provider_field(b, "filling_provider", blocks, tags);
    auto inner   = provider_field(b, "inner_layer_provider", blocks, tags);
    auto alt     = provider_field(b, "alternate_inner_layer_provider", blocks, tags);
    auto middle  = provider_field(b, "middle_layer_provider", blocks, tags);
    auto outer   = provider_field(b, "outer_layer_provider", blocks, tags);
    if (!filling || !inner || !alt || !middle || !outer) {
        return std::unexpected(FeatureError::Malformed);
    }
    c.filling               = *filling;
    c.inner_layer           = *inner;
    c.alternate_inner_layer = *alt;
    c.middle_layer          = *middle;
    c.outer_layer           = *outer;

    simdjson::dom::array placements;
    if (b.at_key("inner_placements").get(placements) != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    for (auto entry : placements) {
        auto state = parse_block_state(entry, blocks);
        if (!state) return std::unexpected(state.error());
        c.inner_placements.push_back(*state);
    }
    if (c.inner_placements.empty()) {
        return std::unexpected(FeatureError::Malformed);
    }

    std::string_view cannot;
    std::string_view invalid;
    if (b.at_key("cannot_replace").get(cannot) != simdjson::SUCCESS ||
        b.at_key("invalid_blocks").get(invalid) != simdjson::SUCCESS || !cannot.starts_with('#') ||
        !invalid.starts_with('#')) {
        // A plain list is legal in the format and vanilla never writes one.
        OV_LOG_ERROR("worldgen: a geode's block sets are not tags, which this reader expects");
        return std::unexpected(FeatureError::Unsupported);
    }
    auto cannot_set  = tag_members(blocks, tags, cannot.substr(1));
    auto invalid_set = tag_members(blocks, tags, invalid.substr(1));
    if (!cannot_set) return std::unexpected(cannot_set.error());
    if (!invalid_set) return std::unexpected(invalid_set.error());
    c.cannot_replace = std::move(*cannot_set);
    c.invalid_blocks = std::move(*invalid_set);

    c.filling_layer     = number(layers.value(), "filling", 1.7);
    c.inner_layer_size  = number(layers.value(), "inner_layer", 2.2);
    c.middle_layer_size = number(layers.value(), "middle_layer", 3.2);
    c.outer_layer_size  = number(layers.value(), "outer_layer", 4.2);

    c.generate_crack_chance = number(crack.value(), "generate_crack_chance", 1.0);
    c.base_crack_size       = number(crack.value(), "base_crack_size", 2.0);
    c.crack_point_offset    = static_cast<i32>(number(crack.value(), "crack_point_offset", 2));

    bool require = true;
    (void)config.at_key("placements_require_layer0_alternate").get(require);
    c.placements_require_layer0_alternate = require;
    c.use_potential_placements_chance = number(config, "use_potential_placements_chance", 0.35);
    c.use_alternate_layer0_chance     = number(config, "use_alternate_layer0_chance", 0.0);
    c.min_gen_offset   = static_cast<i32>(number(config, "min_gen_offset", -16));
    c.max_gen_offset   = static_cast<i32>(number(config, "max_gen_offset", 16));
    c.noise_multiplier = number(config, "noise_multiplier", 0.05);
    c.invalid_blocks_threshold = static_cast<i32>(number(config, "invalid_blocks_threshold", 1));

    auto wall   = int_field(config, "outer_wall_distance");
    auto points = int_field(config, "distribution_points");
    auto offset = int_field(config, "point_offset");
    if (!wall || !points || !offset) {
        return std::unexpected(FeatureError::Malformed);
    }
    c.outer_wall_distance = *wall;
    c.distribution_points = *points;
    c.point_offset        = *offset;
    // `outerWallDistance.getMaxValue()` enters the layer sizes, so the bound is
    // needed as a number.
    i64 wall_max = 0;
    if (config.at_key("outer_wall_distance").at_key("value").at_key("max_inclusive").get(wall_max) !=
        simdjson::SUCCESS) {
        if (config.at_key("outer_wall_distance").get(wall_max) != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
    }
    c.outer_wall_max = static_cast<i32>(wall_max);

    auto water = named_block(blocks, "minecraft:water");
    if (!water) return std::unexpected(water.error());
    c.water = *water;
    if (!blocks.find_block("minecraft:air")) {
        return std::unexpected(FeatureError::Missing);
    }

    return std::static_pointer_cast<const Feature>(std::make_shared<const GeodeFeature>(std::move(c)));
}

}  // namespace

ClaimedFeature parse_cave_feature(std::string_view kind, Json config,
                                  const registry::BlockRegistry& blocks, const BlockTags& tags,
                                  const FeatureResolver& resolve) {
    if (kind == "geode") {
        return parse_geode(config, blocks, tags);
    }
    if (auto claimed = parse_lush_feature(kind, config, blocks, tags, resolve)) {
        return claimed;
    }
    if (auto claimed = parse_root_system_feature(kind, config, blocks, tags, resolve)) {
        return claimed;
    }
    if (auto claimed = parse_dripstone_feature(kind, config, blocks, tags)) {
        return claimed;
    }
    return std::nullopt;
}

}  // namespace ov::worldgen
