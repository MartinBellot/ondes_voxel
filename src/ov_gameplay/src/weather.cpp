#include "ov/gameplay/weather.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <numbers>

namespace ov::gameplay {

namespace {

constexpr registry::BlockId kNoBlock{0xFFFF};

/// The twelve edge gradients of a cube, then four repeated to fill sixteen.
/// Only the first twelve are reached by the 2-D sample (`% 12`).
constexpr std::array<std::array<i32, 3>, 16> kGradient{{{1, 1, 0},
                                                        {-1, 1, 0},
                                                        {1, -1, 0},
                                                        {-1, -1, 0},
                                                        {1, 0, 1},
                                                        {-1, 0, 1},
                                                        {1, 0, -1},
                                                        {-1, 0, -1},
                                                        {0, 1, 1},
                                                        {0, -1, 1},
                                                        {0, 1, -1},
                                                        {0, -1, -1},
                                                        {1, 1, 0},
                                                        {0, -1, 1},
                                                        {-1, 1, 0},
                                                        {0, -1, -1}}};

[[nodiscard]] i32 floor_i32(f64 value) noexcept {
    const auto truncated = static_cast<i32>(value);
    return value < static_cast<f64>(truncated) ? truncated - 1 : truncated;
}

[[nodiscard]] f64 corner(i32 gradient, f64 x, f64 y, f64 z, f64 falloff) noexcept {
    f64 t = falloff - x * x - y * y - z * z;
    if (t < 0.0) {
        return 0.0;
    }
    t *= t;
    const auto& g = kGradient[static_cast<usize>(gradient)];
    return t * t *
           (static_cast<f64>(g[0]) * x + static_cast<f64>(g[1]) * y + static_cast<f64>(g[2]) * z);
}

constexpr std::array<i32, 1> kSingleOctave{0};
constexpr std::array<i32, 3> kFrozenOctaves{-2, -1, 0};

[[nodiscard]] PerlinSimplexNoise make_noise(i64 seed, std::span<const i32> octaves) {
    math::LegacyRandomSource random{seed};
    return PerlinSimplexNoise{random, octaves};
}

[[nodiscard]] i32 parse_int(std::string_view text) noexcept {
    i32 value = -1;
    (void)std::from_chars(text.data(), text.data() + text.size(), value);
    return value;
}

}  // namespace

// ── SimplexNoise ────────────────────────────────────────────────────────────

SimplexNoise::SimplexNoise(math::LegacyRandomSource& random) noexcept {
    // Named, in order: three draws, each its own statement (briefing, trap 2).
    const f64 x = random.next_double();
    const f64 y = random.next_double();
    const f64 z = random.next_double();
    xo_ = x * 256.0;
    yo_ = y * 256.0;
    zo_ = z * 256.0;
    for (i32 i = 0; i < 256; ++i) {
        permutation_[static_cast<usize>(i)] = i;
    }
    for (i32 i = 0; i < 256; ++i) {
        const i32 j = random.next_int(256 - i);
        std::swap(permutation_[static_cast<usize>(i)], permutation_[static_cast<usize>(i + j)]);
    }
}

f64 SimplexNoise::value(f64 x, f64 y) const noexcept {
    // Skew to the simplex grid and back. F2 and G2 are exact expressions of
    // sqrt(3); IEEE sqrt is correctly rounded everywhere, so these agree with
    // any other implementation to the bit.
    const f64 f2 = 0.5 * (std::sqrt(3.0) - 1.0);
    const f64 g2 = (3.0 - std::sqrt(3.0)) / 6.0;

    const f64 skew = (x + y) * f2;
    const i32 i    = floor_i32(x + skew);
    const i32 j    = floor_i32(y + skew);
    const f64 back = static_cast<f64>(i + j) * g2;
    const f64 x0   = x - (static_cast<f64>(i) - back);
    const f64 y0   = y - (static_cast<f64>(j) - back);

    i32 i1 = 0;
    i32 j1 = 1;
    if (x0 > y0) {
        i1 = 1;
        j1 = 0;
    }
    const f64 x1 = x0 - static_cast<f64>(i1) + g2;
    const f64 y1 = y0 - static_cast<f64>(j1) + g2;
    const f64 x2 = x0 - 1.0 + 2.0 * g2;
    const f64 y2 = y0 - 1.0 + 2.0 * g2;

    const i32 ii = i & 255;
    const i32 jj = j & 255;
    const i32 g0 = p(ii + p(jj)) % 12;
    const i32 g1 = p(ii + i1 + p(jj + j1)) % 12;
    const i32 g3 = p(ii + 1 + p(jj + 1)) % 12;

    const f64 n0 = corner(g0, x0, y0, 0.0, 0.5);
    const f64 n1 = corner(g1, x1, y1, 0.0, 0.5);
    const f64 n2 = corner(g3, x2, y2, 0.0, 0.5);
    return 70.0 * (n0 + n1 + n2);
}

// ── PerlinSimplexNoise ──────────────────────────────────────────────────────

PerlinSimplexNoise::PerlinSimplexNoise(math::LegacyRandomSource& random,
                                       std::span<const i32> octaves) {
    if (octaves.empty()) {
        supported_ = false;
        return;
    }
    const auto [low_it, high_it] = std::minmax_element(octaves.begin(), octaves.end());
    const i32 first              = -*low_it;
    const i32 last               = *high_it;
    const i32 count              = first + last + 1;
    if (last > 0 || count <= 0) {
        // A positive octave is seeded from a 3-D sample this class does not
        // carry. Refused, and said.
        supported_ = false;
        return;
    }
    const auto has = [&](i32 octave) {
        return std::find(octaves.begin(), octaves.end(), octave) != octaves.end();
    };

    levels_.resize(static_cast<usize>(count));
    // The first noise is always drawn, whether octave 0 is wanted or not.
    SimplexNoise zeroth{random};
    const i32    base = last;
    if (base >= 0 && base < count && has(0)) {
        levels_[static_cast<usize>(base)] = zeroth;
    }
    for (i32 level = base + 1; level < count; ++level) {
        if (level >= 0 && has(base - level)) {
            levels_[static_cast<usize>(level)] = SimplexNoise{random};
        } else {
            // An octave that is not wanted still consumes what its noise
            // would have: 262 ints.
            for (i32 k = 0; k < 262; ++k) {
                (void)random.next_int();
            }
        }
    }
    input_factor_ = std::pow(2.0, static_cast<f64>(last));
    value_factor_ = 1.0 / (std::pow(2.0, static_cast<f64>(count)) - 1.0);
}

f64 PerlinSimplexNoise::value(f64 x, f64 y, bool use_offsets) const noexcept {
    f64 total     = 0.0;
    f64 amplitude = value_factor_;
    f64 frequency = input_factor_;
    for (const std::optional<SimplexNoise>& level : levels_) {
        if (level) {
            total += level->value(x * frequency + (use_offsets ? level->xo() : 0.0),
                                  y * frequency + (use_offsets ? level->yo() : 0.0)) *
                     amplitude;
        }
        frequency /= 2.0;
        amplitude *= 2.0;
    }
    return total;
}

// ── Climate ─────────────────────────────────────────────────────────────────

BiomeClimate climate_of(const registry::BiomeEffects& effects) noexcept {
    return BiomeClimate{static_cast<f32>(effects.temperature), effects.temperature_modifier == 1,
                        effects.has_precipitation};
}

std::string_view to_string(Precipitation precipitation) noexcept {
    switch (precipitation) {
    case Precipitation::None: return "none";
    case Precipitation::Rain: return "rain";
    case Precipitation::Snow: return "snow";
    }
    return "none";
}

ClimateNoise::ClimateNoise()
    : height_{make_noise(1234, kSingleOctave)},
      frozen_{make_noise(3456, kFrozenOctaves)},
      info_{make_noise(2345, kSingleOctave)} {}

f64 ClimateNoise::height_noise(f64 x, f64 z) const noexcept { return height_.value(x, z, false); }

f32 ClimateNoise::frozen_temperature(f32 temperature, BlockPos pos) const noexcept {
    const f64 patch = frozen_.value(static_cast<f64>(pos.x) * 0.05, static_cast<f64>(pos.z) * 0.05,
                                    false) *
                      7.0;
    const f64 detail = info_.value(static_cast<f64>(pos.x) * 0.2, static_cast<f64>(pos.z) * 0.2,
                                   false);
    if (patch + detail < 0.3) {
        const f64 gate = info_.value(static_cast<f64>(pos.x) * 0.09,
                                     static_cast<f64>(pos.z) * 0.09, false);
        if (gate < 0.8) {
            return 0.2F;
        }
    }
    return temperature;
}

f32 ClimateNoise::temperature_at(const BiomeClimate& biome, BlockPos pos,
                                 i32 sea_level) const noexcept {
    const f32 base = biome.frozen ? frozen_temperature(biome.temperature, pos) : biome.temperature;
    if (pos.y <= sea_level + 17) {
        return base;
    }
    // Float, in the order the expression reads: the x and z are divided as
    // floats before the sample, and the drop is a float product.
    const f32 sample_x = static_cast<f32>(pos.x) / 8.0F;
    const f32 sample_z = static_cast<f32>(pos.z) / 8.0F;
    const auto noise   = static_cast<f32>(
        height_.value(static_cast<f64>(sample_x), static_cast<f64>(sample_z), false) * 8.0);
    const f32 above = noise + static_cast<f32>(pos.y) - 80.0F;
    const f32 drop  = above * 0.05F / 40.0F;
    return base - drop;
}

Precipitation ClimateNoise::precipitation_at(const BiomeClimate& biome, BlockPos pos,
                                             i32 sea_level) const noexcept {
    if (!biome.has_precipitation) {
        return Precipitation::None;
    }
    return warm_enough_to_rain(temperature_at(biome, pos, sea_level)) ? Precipitation::Rain
                                                                      : Precipitation::Snow;
}

// ── The sky ─────────────────────────────────────────────────────────────────

f32 table_cos(f32 radians) noexcept {
    // The table holds sin(i * 2pi / 65536) as floats; cos is a quarter turn on.
    const auto index = static_cast<i32>(radians * 10430.378F + 16384.0F) & 65535;
    return static_cast<f32>(
        std::sin(static_cast<f64>(index) * std::numbers::pi * 2.0 / 65536.0));
}

u8 sky_darken(i64 day_time, f32 rain, f32 thunder) noexcept {
    // The time of day through the overworld's shaping curve, cast to float
    // before the division by three.
    const f64 fraction = static_cast<f64>(day_time) / 24000.0 - 0.25;
    const f64 d0       = fraction - std::floor(fraction);
    const f64 d1       = 0.5 - std::cos(d0 * std::numbers::pi) / 2.0;
    const f32 time     = static_cast<f32>(d0 * 2.0 + d1) / 3.0F;

    const f32 rain_level    = std::clamp(rain, 0.0F, 1.0F);
    const f32 thunder_level = std::clamp(thunder, 0.0F, 1.0F) * rain_level;

    const f64 wet    = 1.0 - static_cast<f64>(rain_level * 5.0F) / 16.0;
    const f64 storm  = 1.0 - static_cast<f64>(thunder_level * 5.0F) / 16.0;
    const f32 cosine = table_cos(time * (static_cast<f32>(std::numbers::pi) * 2.0F));
    const f64 light  = 0.5 + 2.0 * std::clamp(static_cast<f64>(cosine), -0.25, 0.25);
    const auto darken = static_cast<i32>((1.0 - light * wet * storm) * 11.0);
    return static_cast<u8>(std::clamp(darken, 0, 15));
}

// ── Precipitation on blocks ─────────────────────────────────────────────────

PrecipitationRules::PrecipitationRules(const registry::BlockRegistry& blocks,
                                       const registry::Registries&    registries)
    : blocks_{&blocks}, registries_{&registries} {
    const auto find = [&](std::string_view name) {
        const auto id = blocks.find_block(name);
        return id ? *id : kNoBlock;
    };
    water_                = find("minecraft:water");
    bubble_column_        = find("minecraft:bubble_column");
    kelp_                 = find("minecraft:kelp");
    kelp_plant_           = find("minecraft:kelp_plant");
    seagrass_             = find("minecraft:seagrass");
    tall_seagrass_        = find("minecraft:tall_seagrass");
    snow_                 = find("minecraft:snow");
    cauldron_             = find("minecraft:cauldron");
    water_cauldron_       = find("minecraft:water_cauldron");
    powder_snow_cauldron_ = find("minecraft:powder_snow_cauldron");
    const registry::BlockId ice = find("minecraft:ice");
    known_ = water_ != kNoBlock && snow_ != kNoBlock && ice != kNoBlock;
    if (ice != kNoBlock) {
        ice_ = blocks.default_state(ice);
    }

    const auto block_registry = registries.find("minecraft:block");
    wire_.assign(blocks.block_count(), -1);
    if (block_registry) {
        for (usize i = 0; i < blocks.block_count(); ++i) {
            const auto id = registries.protocol_id(
                *block_registry, blocks.block_name(registry::BlockId{static_cast<u16>(i)}));
            wire_[i] = id ? *id : -1;
        }
        cannot_survive_on_ =
            registries.find_tag(*block_registry, "minecraft:snow_layer_cannot_survive_on");
        can_survive_on_ = registries.find_tag(*block_registry, "minecraft:snow_layer_can_survive_on");
    }
}

bool PrecipitationRules::in_tag(const std::optional<registry::TagId>& tag,
                                registry::BlockId                     block) const noexcept {
    return tag && block.value() < wire_.size() && wire_[block.value()] >= 0 &&
           registries_->tag_contains(*tag, wire_[block.value()]);
}

i32 PrecipitationRules::level_of(registry::BlockStateId state) const noexcept {
    const auto property = blocks_->find_property(blocks_->block_of(state), "level");
    return property ? parse_int(blocks_->property_value(state, *property)) : -1;
}

bool PrecipitationRules::is_water(registry::BlockStateId state) const noexcept {
    const registry::BlockId block = blocks_->block_of(state);
    if (block == water_ || block == bubble_column_ || block == kelp_ || block == kelp_plant_ ||
        block == seagrass_ || block == tall_seagrass_) {
        return true;
    }
    const auto waterlogged = blocks_->find_property(block, "waterlogged");
    return waterlogged && blocks_->property_value(state, *waterlogged) == "true";
}

bool PrecipitationRules::should_freeze(const world::LevelView& level, BlockPos pos,
                                       f32 temperature, u8 block_light, bool at_edge) const {
    if (!known_ || warm_enough_to_rain(temperature) || !level.shape().contains_y(pos.y) ||
        block_light >= 10) {
        return false;
    }
    const registry::BlockStateId state = level.block_at(pos);
    // Source water in the water block itself: a waterlogged stair or a kelp
    // holds water and never becomes ice.
    if (blocks_->block_of(state) != water_ || level_of(state) != 0) {
        return false;
    }
    if (!at_edge) {
        return true;
    }
    const bool surrounded = is_water(level.block_at(pos.offset(Direction::West))) &&
                            is_water(level.block_at(pos.offset(Direction::East))) &&
                            is_water(level.block_at(pos.offset(Direction::North))) &&
                            is_water(level.block_at(pos.offset(Direction::South)));
    return !surrounded;
}

bool PrecipitationRules::snow_survives(const world::LevelView& level, BlockPos pos) const {
    const BlockPos               below_pos{pos.x, pos.y - 1, pos.z};
    const registry::BlockStateId below = level.block_at(below_pos);
    const registry::BlockId      block = blocks_->block_of(below);
    if (in_tag(cannot_survive_on_, block)) {
        return false;
    }
    if (in_tag(can_survive_on_, block)) {
        return true;
    }
    if (block == snow_) {
        const auto layers = blocks_->find_property(block, "layers");
        return layers && blocks_->property_value(below, *layers) == "8";
    }
    // "A full top face" is the collision shape's; the registry carries the
    // sturdy face, which is the same thing for every block a snow layer
    // meets on the ground. Named in the provenance file.
    return blocks_->face_is_sturdy(below, registry::BlockRegistry::Face::Up);
}

bool PrecipitationRules::should_snow(const world::LevelView& level, BlockPos pos, f32 temperature,
                                     u8 block_light) const {
    if (!known_ || warm_enough_to_rain(temperature) || !level.shape().contains_y(pos.y) ||
        block_light >= 10) {
        return false;
    }
    const registry::BlockStateId state = level.block_at(pos);
    const registry::BlockId      block = blocks_->block_of(state);
    if (!blocks_->is_air(block) && block != snow_) {
        return false;
    }
    return snow_survives(level, pos);
}

std::optional<registry::BlockStateId> PrecipitationRules::next_snow(registry::BlockStateId current,
                                                                    i32 accumulation_height) const {
    if (!known_ || accumulation_height <= 0) {
        return std::nullopt;
    }
    const registry::BlockId block = blocks_->block_of(current);
    if (block != snow_) {
        return blocks_->default_state(snow_);
    }
    const auto layers = blocks_->find_property(snow_, "layers");
    if (!layers) {
        return std::nullopt;
    }
    const i32 now = parse_int(blocks_->property_value(current, *layers));
    if (now < std::min(accumulation_height, 8)) {
        // Values are "1".."8" in order, so the next index is the next layer.
        const u16 index = blocks_->property_index(current, *layers);
        return blocks_->with_property(current, *layers, static_cast<u16>(index + 1));
    }
    return std::nullopt;
}

bool PrecipitationRules::takes_precipitation(registry::BlockStateId state) const noexcept {
    const registry::BlockId block = blocks_->block_of(state);
    return block == cauldron_ || block == water_cauldron_ || block == powder_snow_cauldron_;
}

std::optional<registry::BlockStateId> PrecipitationRules::precipitation_on(
    registry::BlockStateId state, Precipitation precipitation, f32 roll) const {
    if (precipitation == Precipitation::None) {
        return std::nullopt;
    }
    const f32  chance = precipitation == Precipitation::Rain ? 0.05F : 0.1F;
    const bool fills  = roll < chance;
    const registry::BlockId block = blocks_->block_of(state);
    if (block == cauldron_) {
        if (!fills) {
            return std::nullopt;
        }
        const registry::BlockId into =
            precipitation == Precipitation::Rain ? water_cauldron_ : powder_snow_cauldron_;
        return into == kNoBlock ? std::nullopt
                                : std::optional<registry::BlockStateId>{blocks_->default_state(into)};
    }
    const bool matches = (block == water_cauldron_ && precipitation == Precipitation::Rain) ||
                         (block == powder_snow_cauldron_ && precipitation == Precipitation::Snow);
    if (!fills || !matches) {
        return std::nullopt;
    }
    const auto property = blocks_->find_property(block, "level");
    if (!property || blocks_->property_value(state, *property) == "3") {
        return std::nullopt;
    }
    const u16 index = blocks_->property_index(state, *property);
    return blocks_->with_property(state, *property, static_cast<u16>(index + 1));
}

void PrecipitationRules::tick_column(world::LevelWriter& level, const PrecipitationColumn& column,
                                     const ClimateNoise& climate, bool raining,
                                     i32 accumulation_height, math::LegacyRandomSource& random,
                                     PrecipitationStats& stats) const {
    if (!known_) {
        return;
    }
    const BlockPos top   = column.top;
    const BlockPos below{top.x, top.y - 1, top.z};

    // Water freezes whether it rains or not: cold is enough.
    if (should_freeze(level, below,
                      climate.temperature_at(column.biome, below, column.sea_level),
                      column.light_below, true)) {
        level.set_block(below, ice_);
        ++stats.froze;
    }
    if (!raining) {
        return;
    }

    if (accumulation_height > 0 &&
        should_snow(level, top, climate.temperature_at(column.biome, top, column.sea_level),
                    column.light_top)) {
        const registry::BlockStateId current = level.block_at(top);
        if (const auto next = next_snow(current, accumulation_height)) {
            const bool first = blocks_->block_of(current) != snow_;
            level.set_block(top, *next);
            ++(first ? stats.snowed : stats.layered);
            // The block under a new layer learns it is snowy — the shape
            // update the game's write carries, done here because a writer
            // only writes.
            if (first) {
                const registry::BlockStateId ground = level.block_at(below);
                const auto snowy = blocks_->find_property(blocks_->block_of(ground), "snowy");
                if (snowy && blocks_->property_value(ground, *snowy) == "false") {
                    for (usize i = 0; i < snowy->values.size(); ++i) {
                        if (snowy->values[i] == "true") {
                            level.set_block(below,
                                            blocks_->with_property(ground, *snowy,
                                                                   static_cast<u16>(i)));
                            ++stats.snowy;
                        }
                    }
                }
            }
        }
    }

    const Precipitation falling = climate.precipitation_at(column.biome, below, column.sea_level);
    if (falling == Precipitation::None) {
        return;
    }
    const registry::BlockStateId state = level.block_at(below);
    if (!takes_precipitation(state)) {
        return;
    }
    const f32 roll = random.next_float();
    if (const auto filled = precipitation_on(state, falling, roll)) {
        level.set_block(below, *filled);
        ++stats.cauldrons;
    }
}

// ── Lightning ───────────────────────────────────────────────────────────────

AABB lightning_hit_box(const Vec3d& bolt) noexcept {
    return AABB{Vec3d{bolt.x - 3.0, bolt.y - 3.0, bolt.z - 3.0},
                      Vec3d{bolt.x + 3.0, bolt.y + 6.0 + 3.0, bolt.z + 3.0}};
}

AABB lightning_target_box(BlockPos top, i32 max_build_height) noexcept {
    return AABB{Vec3d{static_cast<f64>(top.x) - 3.0, static_cast<f64>(top.y) - 3.0,
                            static_cast<f64>(top.z) - 3.0},
                      Vec3d{static_cast<f64>(top.x) + 3.0,
                            static_cast<f64>(max_build_height) + 3.0,
                            static_cast<f64>(top.z) + 3.0}};
}

std::optional<BlockPos> nearest_rod(BlockPos from, std::span<const BlockPos> rods) noexcept {
    std::optional<BlockPos> best;
    i64                     best_distance = 0;
    constexpr i64           kReach =
        static_cast<i64>(kLightningRodRange) * static_cast<i64>(kLightningRodRange);
    for (const BlockPos& rod : rods) {
        const i64 dx       = static_cast<i64>(rod.x) - from.x;
        const i64 dy       = static_cast<i64>(rod.y) - from.y;
        const i64 dz       = static_cast<i64>(rod.z) - from.z;
        const i64 distance = dx * dx + dy * dy + dz * dz;
        if (distance > kReach) {
            continue;
        }
        if (!best || distance < best_distance) {
            best          = rod;
            best_distance = distance;
        }
    }
    return best;
}

LightningConversion lightning_conversion(std::string_view entity_type) noexcept {
    if (entity_type == "minecraft:pig") {
        return LightningConversion::ZombifiedPiglin;
    }
    if (entity_type == "minecraft:villager") {
        return LightningConversion::Witch;
    }
    if (entity_type == "minecraft:creeper") {
        return LightningConversion::ChargeCreeper;
    }
    if (entity_type == "minecraft:mooshroom") {
        return LightningConversion::FlipMooshroom;
    }
    if (entity_type == "minecraft:turtle") {
        return LightningConversion::Kill;
    }
    return LightningConversion::None;
}

BoltClock new_bolt(math::LegacyRandomSource& random) noexcept {
    (void)random.next_long();  // the drawing seed: the client's, never used here
    BoltClock clock;
    clock.life    = 2;
    clock.flashes = random.next_int(3) + 1;
    return clock;
}

BoltStep tick_bolt(BoltClock& clock, math::LegacyRandomSource& random) noexcept {
    BoltStep step;
    if (clock.life == 2) {
        step.strike = true;
    }
    --clock.life;
    if (clock.life < 0) {
        if (clock.flashes == 0) {
            step.remove = true;
            return step;
        }
        const i32 wait = random.next_int(10);
        if (clock.life < -wait) {
            --clock.flashes;
            clock.life = 1;
            (void)random.next_long();
            step.refire = true;
        }
    }
    step.lit = clock.life >= 0;
    return step;
}

}  // namespace ov::gameplay
