// Weather and sleep rules, against a map-backed level.
//
// The thresholds asserted here are the game's, as docs/provenance/
// meteo-sommeil.md records them: the day/night edges a bed works at, the
// edge-only freeze, the snow layers capped by the gamerule, the cauldron
// chances, the 128-block rod, the bolt that flashes one to three times.
#include "ov/gameplay/sleep.hpp"
#include "ov/gameplay/weather.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace ov;
using namespace ov::gameplay;

namespace {

struct Packs {
    std::optional<registry::BlockRegistry> blocks;
    std::optional<registry::Registries>    registries;
};

[[nodiscard]] const Packs& packs() {
    static const Packs state = [] {
        const auto path = std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" /
                          "registry.ovpack";
        Packs out;
        auto  blocks = registry::BlockRegistry::load(path);
        auto  regs   = registry::Registries::load(path);
        if (blocks && regs) {
            out.blocks     = std::move(*blocks);
            out.registries = std::move(*regs);
        }
        return out;
    }();
    return state;
}

[[nodiscard]] bool have_packs() { return packs().blocks.has_value() && packs().registries.has_value(); }
[[nodiscard]] const registry::BlockRegistry& blocks() { return *packs().blocks; }

[[nodiscard]] registry::BlockStateId state_of(
    std::string_view name,
    std::initializer_list<std::pair<std::string_view, std::string_view>> props = {}) {
    const auto id = blocks().find_block(name);
    REQUIRE(id.has_value());
    registry::BlockStateId state = blocks().default_state(*id);
    for (const auto& [key, value] : props) {
        const auto property = blocks().find_property(*id, key);
        REQUIRE(property.has_value());
        bool found = false;
        for (usize i = 0; i < property->values.size(); ++i) {
            if (property->values[i] == value) {
                state = blocks().with_property(state, *property, static_cast<u16>(i));
                found = true;
            }
        }
        REQUIRE(found);
    }
    return state;
}

[[nodiscard]] std::string name_at(registry::BlockStateId state) {
    return std::string{blocks().block_name(blocks().block_of(state))};
}

[[nodiscard]] std::string value_at(registry::BlockStateId state, std::string_view property) {
    const auto p = blocks().find_property(blocks().block_of(state), property);
    return p ? std::string{blocks().property_value(state, *p)} : std::string{};
}

class MapLevel final : public world::LevelWriter {
public:
    [[nodiscard]] registry::BlockStateId block_at(BlockPos pos) const override {
        const auto found = cells_.find(key(pos));
        return found == cells_.end() ? registry::kAirState : found->second;
    }
    [[nodiscard]] bool                   is_loaded(BlockPos) const override { return true; }
    [[nodiscard]] world::WorldShape      shape() const override { return world::WorldShape::overworld(); }
    [[nodiscard]] world::DimensionTraits traits() const override { return {}; }
    [[nodiscard]] const registry::BlockRegistry& blocks() const override { return ::blocks(); }

    void set_block(BlockPos pos, registry::BlockStateId state) override {
        cells_[key(pos)] = state;
        ++writes;
    }
    void schedule_tick(BlockPos, std::string_view, i64, world::TickQueue,
                       world::TickPriority) override {}
    [[nodiscard]] bool has_scheduled_tick(BlockPos, std::string_view,
                                          world::TickQueue) const override {
        return false;
    }
    [[nodiscard]] i64 game_time() const override { return 0; }

    /// A flat floor of `ground` at y = -61 over a square, air above.
    void floor(registry::BlockStateId ground, i32 half = 12) {
        for (i32 x = -half; x <= half; ++x) {
            for (i32 z = -half; z <= half; ++z) {
                cells_[key({x, -61, z})] = ground;
            }
        }
    }

    usize writes{0};

private:
    [[nodiscard]] static std::tuple<i32, i32, i32> key(BlockPos p) { return {p.x, p.y, p.z}; }
    std::map<std::tuple<i32, i32, i32>, registry::BlockStateId> cells_;
};

constexpr BiomeClimate kSnowy{0.0F, false, true};
constexpr BiomeClimate kPlains{0.8F, false, true};
constexpr BiomeClimate kDesert{2.0F, false, false};

}  // namespace

// ── Noise and climate ───────────────────────────────────────────────────────

TEST_CASE("the climate noises are deterministic, bounded and not constant") {
    const ClimateNoise a;
    const ClimateNoise b;
    f64 low = 1e9;
    f64 high = -1e9;
    for (i32 i = 0; i < 2000; ++i) {
        const f64 x = static_cast<f64>(i) * 0.731 - 400.0;
        const f64 z = static_cast<f64>(i) * -1.113 + 90.0;
        const f64 v = a.height_noise(x, z);
        CHECK(v == b.height_noise(x, z));
        low  = std::min(low, v);
        high = std::max(high, v);
    }
    // Simplex noise is bounded by one in magnitude, and a sample this size
    // reaches well into both halves.
    CHECK(low >= -1.0);
    CHECK(high <= 1.0);
    CHECK(low < -0.5);
    CHECK(high > 0.5);
}

TEST_CASE("a positive octave is refused, not guessed") {
    math::LegacyRandomSource random{7};
    const std::array<i32, 2> octaves{0, 1};
    const PerlinSimplexNoise noise{random, octaves};
    CHECK_FALSE(noise.supported());
}

TEST_CASE("temperature only falls above sea level plus seventeen") {
    const ClimateNoise climate;
    CHECK(climate.temperature_at(kPlains, {10, 80, 10}) == 0.8F);
    CHECK(climate.temperature_at(kPlains, {10, -40, 10}) == 0.8F);
    // Well above, it has fallen by about 0.00125 a block, give or take the
    // eight-block noise on the height.
    const f32 high = climate.temperature_at(kPlains, {10, 300, 10});
    CHECK(high < 0.8F);
    CHECK(high == Catch::Approx(0.8 - 220.0 * 0.05 / 40.0).margin(8.0 * 0.05 / 40.0 + 1e-6));
    // Taiga at 0.25: rain at sea level, snow on a mountain.
    const BiomeClimate taiga{0.25F, false, true};
    CHECK(climate.precipitation_at(taiga, {0, 64, 0}) == Precipitation::Rain);
    CHECK(climate.precipitation_at(taiga, {0, 250, 0}) == Precipitation::Snow);
    CHECK(climate.precipitation_at(kDesert, {0, 64, 0}) == Precipitation::None);
}

TEST_CASE("a frozen biome is lifted to 0.2 in patches, and only there") {
    const ClimateNoise climate;
    const BiomeClimate frozen_ocean{0.0F, true, true};
    usize lifted = 0;
    usize total  = 0;
    for (i32 x = -256; x < 256; x += 4) {
        for (i32 z = -256; z < 256; z += 4) {
            const f32 t = climate.temperature_at(frozen_ocean, {x, 62, z});
            CHECK((t == 0.0F || t == 0.2F));
            lifted += t == 0.2F ? 1 : 0;
            ++total;
        }
    }
    // Patches, not all and not none. How much is lifted is the reference
    // world's to say (test_climate_oracle, docs/provenance/meteo-sommeil.md).
    CHECK(lifted > total / 50);
    CHECK(lifted < total - total / 50);
}

// ── The sky ─────────────────────────────────────────────────────────────────

TEST_CASE("the sky darkening reproduces the day and the storm") {
    CHECK(sky_darken(0, 0.0F, 0.0F) == 0);
    CHECK(sky_darken(6000, 0.0F, 0.0F) == 0);
    CHECK(sky_darken(18000, 0.0F, 0.0F) == 11);
    // Thunder without rain is not dark: the thunder level is multiplied by it.
    CHECK(sky_darken(6000, 0.0F, 1.0F) == 0);
    // A noon storm reads internal light 10: darkening 5.
    CHECK(sky_darken(6000, 1.0F, 1.0F) == 5);
    // Rain alone at noon: 3.
    CHECK(sky_darken(6000, 1.0F, 0.0F) == 3);
}

TEST_CASE("the bed's day and night edges, clear, rain and thunder") {
    // The edges documented for 1.20.1, and measured by measure_weather.py sleep.
    CHECK(is_day(sky_darken(12541, 0.0F, 0.0F)));
    CHECK_FALSE(is_day(sky_darken(12542, 0.0F, 0.0F)));
    CHECK_FALSE(is_day(sky_darken(23459, 0.0F, 0.0F)));
    CHECK(is_day(sky_darken(23460, 0.0F, 0.0F)));
    CHECK(is_day(sky_darken(12009, 1.0F, 0.0F)));
    CHECK_FALSE(is_day(sky_darken(12010, 1.0F, 0.0F)));
    CHECK_FALSE(is_day(sky_darken(23991, 1.0F, 0.0F)));
    CHECK(is_day(sky_darken(23992, 1.0F, 0.0F)));
    CHECK(is_day(sky_darken(6000, 1.0F, 0.0F)));
    CHECK_FALSE(is_day(sky_darken(6000, 1.0F, 1.0F)));
    CHECK_FALSE(is_day(sky_darken(1000, 1.0F, 1.0F)));
}

// ── Precipitation ───────────────────────────────────────────────────────────

TEST_CASE("water freezes from the edge in, and only when cold and dark") {
    if (!have_packs()) {
        SKIP("registry.ovpack is not built");
    }
    const PrecipitationRules rules{blocks(), *packs().registries};
    MapLevel level;
    level.floor(state_of("minecraft:grass_block"));
    const auto water = state_of("minecraft:water");
    for (i32 x = 0; x < 5; ++x) {
        for (i32 z = 0; z < 5; ++z) {
            level.set_block({x, -61, z}, water);
        }
    }
    CHECK(rules.should_freeze(level, {0, -61, 0}, 0.0F, 0, true));   // corner
    CHECK(rules.should_freeze(level, {2, -61, 0}, 0.0F, 0, true));   // edge
    CHECK_FALSE(rules.should_freeze(level, {2, -61, 2}, 0.0F, 0, true));  // middle
    CHECK(rules.should_freeze(level, {2, -61, 2}, 0.0F, 0, false));  // world generation's pass
    CHECK_FALSE(rules.should_freeze(level, {0, -61, 0}, 0.15F, 0, true));  // warm enough to rain
    CHECK(rules.should_freeze(level, {0, -61, 0}, 0.149F, 9, true));
    CHECK_FALSE(rules.should_freeze(level, {0, -61, 0}, 0.0F, 10, true));  // a torch nearby
    // Once the edge is ice, the ring inside it is an edge.
    level.set_block({2, -61, 1}, rules.ice());
    CHECK(rules.should_freeze(level, {2, -61, 2}, 0.0F, 0, true));
    // Flowing water and a waterlogged block never freeze.
    level.set_block({7, -61, 7}, state_of("minecraft:water", {{"level", "3"}}));
    CHECK_FALSE(rules.should_freeze(level, {7, -61, 7}, 0.0F, 0, false));
    level.set_block({8, -61, 8}, state_of("minecraft:oak_stairs", {{"waterlogged", "true"}}));
    CHECK_FALSE(rules.should_freeze(level, {8, -61, 8}, 0.0F, 0, false));
    CHECK(rules.is_water(level.block_at({8, -61, 8})));
}

TEST_CASE("ponds freeze from the edge in, as fast as the real server froze them") {
    if (!have_packs()) {
        SKIP("registry.ovpack is not built");
    }
    // measure_weather.py `precip`: 36 ponds of 5x5 in snowy plains, read after
    // about 9000 ticks (8961 counted, plus the seconds between filling them and
    // the first gametime). Vanilla: edge 511/576, inner 239/288, centre 25/36.
    const PrecipitationRules rules{blocks(), *packs().registries};
    const auto               water = state_of("minecraft:water");
    const auto               grass = state_of("minecraft:grass_block");
    constexpr i32            kTicks   = 9000;
    constexpr f64            kPerTick = 1.0 / 16.0 / 256.0;
    constexpr i32            kPonds   = 3600;

    const auto ring_of = [](i32 dx, i32 dz) {
        if (dx == 0 || dx == 4 || dz == 0 || dz == 4) return 0;
        return dx == 2 && dz == 2 ? 2 : 1;
    };
    const auto simulate = [&](bool at_edge, std::array<f64, 3>& fraction) {
        math::LegacyRandomSource random{2024};
        std::array<usize, 3> frozen{};
        std::array<usize, 3> cells{};
        for (i32 pond = 0; pond < kPonds; ++pond) {
            MapLevel level;
            level.floor(grass, 6);
            // Every cell's hits, as a time-ordered list: geometric gaps.
            std::vector<std::tuple<i32, i32, i32>> hits;
            for (i32 dx = 0; dx < 5; ++dx) {
                for (i32 dz = 0; dz < 5; ++dz) {
                    level.set_block({dx, -61, dz}, water);
                    i32 t = 0;
                    while (true) {
                        const f64 u   = random.next_double();
                        const auto gap = static_cast<i32>(std::floor(std::log(1.0 - u) / std::log(1.0 - kPerTick))) + 1;
                        t += gap;
                        if (t > kTicks) break;
                        hits.emplace_back(t, dx, dz);
                    }
                }
            }
            std::ranges::sort(hits);
            for (const auto& [t, dx, dz] : hits) {
                if (rules.should_freeze(level, {dx, -61, dz}, 0.0F, 0, at_edge)) {
                    level.set_block({dx, -61, dz}, rules.ice());
                }
            }
            for (i32 dx = 0; dx < 5; ++dx) {
                for (i32 dz = 0; dz < 5; ++dz) {
                    const i32 ring = ring_of(dx, dz);
                    ++cells[static_cast<usize>(ring)];
                    frozen[static_cast<usize>(ring)] += level.block_at({dx, -61, dz}) == rules.ice() ? 1 : 0;
                }
            }
        }
        for (usize i = 0; i < 3; ++i) {
            fraction[i] = static_cast<f64>(frozen[i]) / static_cast<f64>(cells[i]);
        }
    };
    std::array<f64, 3> model{};
    std::array<f64, 3> witness{};
    simulate(true, model);
    simulate(false, witness);
    const std::array<f64, 3> observed{511.0, 239.0, 25.0};
    const std::array<f64, 3> totals{576.0, 288.0, 36.0};
    const std::array<const char*, 3> names{"edge", "inner", "centre"};
    f64 chi_model   = 0.0;
    f64 chi_witness = 0.0;
    for (usize i = 0; i < 3; ++i) {
        const auto z = [&](f64 p) {
            return (observed[i] - totals[i] * p) / std::sqrt(totals[i] * p * (1.0 - p));
        };
        WARN(names[i] << ": vanilla " << observed[i] << "/" << totals[i] << ", edge rule "
                      << model[i] * totals[i] << " (z " << z(model[i]) << "), no edge rule "
                      << witness[i] * totals[i] << " (z " << z(witness[i]) << ")");
        chi_model += z(model[i]) * z(model[i]);
        chi_witness += z(witness[i]) * z(witness[i]);
    }
    WARN("chi2, three rings: edge rule " << chi_model << ", no edge rule " << chi_witness);
    CHECK(chi_model < 11.3);  // p > 0.01 on three degrees of freedom
    CHECK(chi_witness > chi_model);
}

TEST_CASE("snow lies on full faces, never on ice, and stacks to the gamerule") {
    if (!have_packs()) {
        SKIP("registry.ovpack is not built");
    }
    const PrecipitationRules rules{blocks(), *packs().registries};
    MapLevel level;
    level.floor(state_of("minecraft:grass_block"));
    CHECK(rules.should_snow(level, {0, -60, 0}, 0.0F, 0));
    CHECK_FALSE(rules.should_snow(level, {0, -60, 0}, 0.2F, 0));
    CHECK_FALSE(rules.should_snow(level, {0, -60, 0}, 0.0F, 10));
    level.set_block({1, -61, 1}, state_of("minecraft:ice"));
    CHECK_FALSE(rules.should_snow(level, {1, -60, 1}, 0.0F, 0));
    level.set_block({2, -61, 2}, state_of("minecraft:soul_sand"));
    CHECK(rules.should_snow(level, {2, -60, 2}, 0.0F, 0));
    level.set_block({3, -61, 3}, state_of("minecraft:oak_slab"));  // bottom slab: no full top
    CHECK_FALSE(rules.should_snow(level, {3, -60, 3}, 0.0F, 0));
    level.set_block({4, -60, 4}, state_of("minecraft:oak_leaves"));
    CHECK_FALSE(rules.should_snow(level, {4, -60, 4}, 0.0F, 0));  // occupied by a block

    const auto air = registry::kAirState;
    const auto one = rules.next_snow(air, 1);
    REQUIRE(one.has_value());
    CHECK(value_at(*one, "layers") == "1");
    CHECK_FALSE(rules.next_snow(*one, 1).has_value());
    const auto two = rules.next_snow(*one, 3);
    REQUIRE(two.has_value());
    CHECK(value_at(*two, "layers") == "2");
    const auto three = rules.next_snow(*two, 3);
    REQUIRE(three.has_value());
    CHECK_FALSE(rules.next_snow(*three, 3).has_value());
    // The gamerule goes up to 128, the block stops at eight.
    auto layers = *one;
    for (i32 i = 0; i < 20; ++i) {
        if (const auto next = rules.next_snow(layers, 128)) {
            layers = *next;
        }
    }
    CHECK(value_at(layers, "layers") == "8");
    CHECK_FALSE(rules.next_snow(air, 0).has_value());
}

TEST_CASE("cauldrons fill one roll in twenty under rain, one in ten under snow") {
    if (!have_packs()) {
        SKIP("registry.ovpack is not built");
    }
    const PrecipitationRules rules{blocks(), *packs().registries};
    const auto empty = state_of("minecraft:cauldron");
    CHECK(rules.takes_precipitation(empty));
    CHECK_FALSE(rules.takes_precipitation(state_of("minecraft:lava_cauldron")));
    const auto rained = rules.precipitation_on(empty, Precipitation::Rain, 0.049F);
    REQUIRE(rained.has_value());
    CHECK(name_at(*rained) == "minecraft:water_cauldron");
    CHECK(value_at(*rained, "level") == "1");
    CHECK_FALSE(rules.precipitation_on(empty, Precipitation::Rain, 0.05F).has_value());
    const auto snowed = rules.precipitation_on(empty, Precipitation::Snow, 0.099F);
    REQUIRE(snowed.has_value());
    CHECK(name_at(*snowed) == "minecraft:powder_snow_cauldron");
    CHECK_FALSE(rules.precipitation_on(empty, Precipitation::Snow, 0.1F).has_value());
    // A water cauldron takes rain to three, and never snow.
    const auto two = rules.precipitation_on(*rained, Precipitation::Rain, 0.0F);
    REQUIRE(two.has_value());
    CHECK(value_at(*two, "level") == "2");
    CHECK_FALSE(rules.precipitation_on(*rained, Precipitation::Snow, 0.0F).has_value());
    const auto full = state_of("minecraft:water_cauldron", {{"level", "3"}});
    CHECK_FALSE(rules.precipitation_on(full, Precipitation::Rain, 0.0F).has_value());
}

TEST_CASE("one precipitation tick: freeze whatever the weather, snow and cauldrons only in it") {
    if (!have_packs()) {
        SKIP("registry.ovpack is not built");
    }
    const PrecipitationRules rules{blocks(), *packs().registries};
    const ClimateNoise       climate;
    MapLevel                 level;
    level.floor(state_of("minecraft:grass_block", {{"snowy", "false"}}));
    level.set_block({0, -61, 0}, state_of("minecraft:water"));
    math::LegacyRandomSource random{1};
    PrecipitationStats stats;
    PrecipitationColumn column;
    column.top   = {0, -60, 0};
    column.biome = kSnowy;
    rules.tick_column(level, column, climate, false, 1, random, stats);
    CHECK(stats.froze == 1);
    CHECK(name_at(level.block_at({0, -61, 0})) == "minecraft:ice");

    column.top = {3, -60, 3};
    rules.tick_column(level, column, climate, false, 1, random, stats);
    CHECK(stats.snowed == 0);
    rules.tick_column(level, column, climate, true, 1, random, stats);
    CHECK(stats.snowed == 1);
    CHECK(name_at(level.block_at({3, -60, 3})) == "minecraft:snow");
    CHECK(value_at(level.block_at({3, -61, 3}), "snowy") == "true");

    // Plains rain: no snow, and a cauldron eventually fills.
    column.biome = kPlains;
    column.top   = {5, -59, 5};
    level.set_block({5, -60, 5}, state_of("minecraft:cauldron"));
    for (i32 i = 0; i < 400 && stats.cauldrons == 0; ++i) {
        rules.tick_column(level, column, climate, true, 1, random, stats);
    }
    CHECK(stats.cauldrons == 1);
    CHECK(name_at(level.block_at({5, -60, 5})) == "minecraft:water_cauldron");
}

// ── Lightning ───────────────────────────────────────────────────────────────

TEST_CASE("a rod draws a bolt within 128 blocks, the nearest first") {
    const std::array<BlockPos, 3> rods{BlockPos{100, 70, 0}, BlockPos{-50, 64, 0},
                                       BlockPos{0, 64, 129}};
    const auto near = nearest_rod({0, 64, 0}, rods);
    REQUIRE(near.has_value());
    CHECK(*near == BlockPos{-50, 64, 0});
    CHECK_FALSE(nearest_rod({300, 64, 300}, rods).has_value());
    // Exactly 128 away counts; 129 does not.
    const std::array<BlockPos, 1> edge{BlockPos{128, 64, 0}};
    CHECK(nearest_rod({0, 64, 0}, edge).has_value());
    const std::array<BlockPos, 1> beyond{BlockPos{129, 64, 0}};
    CHECK_FALSE(nearest_rod({0, 64, 0}, beyond).has_value());
}

TEST_CASE("a bolt hurts three blocks around and nine up, and flashes one to three times") {
    const AABB box = lightning_hit_box({0.5, -60.0, 0.5});
    CHECK(box.min.x == -2.5);
    CHECK(box.max.y == -51.0);
    CHECK(lightning_conversion("minecraft:pig") == LightningConversion::ZombifiedPiglin);
    CHECK(lightning_conversion("minecraft:creeper") == LightningConversion::ChargeCreeper);
    CHECK(lightning_conversion("minecraft:cow") == LightningConversion::None);

    math::LegacyRandomSource random{42};
    std::array<i32, 4> flashes{};
    for (i32 n = 0; n < 3000; ++n) {
        BoltClock clock = new_bolt(random);
        REQUIRE(clock.flashes >= 1);
        REQUIRE(clock.flashes <= 3);
        ++flashes[static_cast<usize>(clock.flashes)];
        i32  strikes = 0;
        i32  ticks   = 0;
        bool gone    = false;
        while (!gone && ticks < 100) {
            const BoltStep step = tick_bolt(clock, random);
            strikes += step.strike ? 1 : 0;
            gone = step.remove;
            ++ticks;
        }
        REQUIRE(gone);
        REQUIRE(strikes == 1);
    }
    CHECK(flashes[1] > 800);
    CHECK(flashes[3] > 800);
}

// ── Sleep ───────────────────────────────────────────────────────────────────

TEST_CASE("the verdict follows the game's order") {
    SleepCheck check;
    CHECK(judge_sleep(check).problem == BedProblem::None);
    CHECK(judge_sleep(check).sets_spawn);
    check.bed_works = false;
    CHECK(judge_sleep(check).explodes);
    check = {};
    check.is_day = true;
    const SleepVerdict day = judge_sleep(check);
    CHECK(day.problem == BedProblem::NotPossibleNow);
    CHECK(day.sets_spawn);  // "respawn point set", then "only at night"
    check.in_range = false;
    CHECK(judge_sleep(check).problem == BedProblem::TooFarAway);
    CHECK_FALSE(judge_sleep(check).sets_spawn);
    check = {};
    check.monsters_near = true;
    CHECK(judge_sleep(check).problem == BedProblem::NotSafe);
    check.creative = true;
    CHECK(judge_sleep(check).problem == BedProblem::None);
    check = {};
    check.occupied = true;
    CHECK(judge_sleep(check).problem == BedProblem::Occupied);
    CHECK(bed_message_key(BedProblem::NotSafe) == "block.minecraft.bed.not_safe");
    CHECK(bed_message_key(BedProblem::None).empty());
}

TEST_CASE("the night skip lands on the next morning, and needs enough sleepers") {
    CHECK(wake_day_time(18000) == 24000);
    CHECK(wake_day_time(12542) == 24000);
    CHECK(wake_day_time(24000 + 13000) == 48000);
    CHECK(sleepers_needed(1, 100) == 1);
    CHECK(sleepers_needed(2, 100) == 2);
    CHECK(sleepers_needed(2, 50) == 1);
    CHECK(sleepers_needed(3, 50) == 2);
    CHECK(sleepers_needed(0, 100) == 1);
    CHECK(sleepers_needed(5, 0) == 1);
    CHECK(prevents_rest("minecraft:zombie"));
    CHECK(prevents_rest("minecraft:enderman"));
    CHECK_FALSE(prevents_rest("minecraft:zombified_piglin"));
    CHECK_FALSE(prevents_rest("minecraft:slime"));
}

TEST_CASE("a bed: either half, the range, what obstructs it, where one stands up") {
    if (!have_packs()) {
        SKIP("registry.ovpack is not built");
    }
    const BedRules rules{blocks()};
    MapLevel       level;
    level.floor(state_of("minecraft:grass_block"));
    level.set_block({5, -60, 0}, state_of("minecraft:red_bed", {{"part", "head"}, {"facing", "east"}}));
    level.set_block({4, -60, 0}, state_of("minecraft:red_bed", {{"part", "foot"}, {"facing", "east"}}));
    const auto from_foot = rules.bed_at(level, {4, -60, 0});
    const auto from_head = rules.bed_at(level, {5, -60, 0});
    REQUIRE(from_foot.has_value());
    REQUIRE(from_head.has_value());
    CHECK(from_foot->head == BlockPos{5, -60, 0});
    CHECK(from_head->foot == BlockPos{4, -60, 0});
    CHECK_FALSE(from_foot->occupied);

    CHECK(BedRules::in_range({4.5, -60.0, -2.5}, *from_foot));
    CHECK_FALSE(BedRules::in_range({4.5, -60.0, -2.6}, *from_foot));
    CHECK(BedRules::in_range({8.5, -60.0, 0.5}, *from_foot));
    CHECK_FALSE(BedRules::in_range({8.6, -60.0, 0.5}, *from_foot));
    CHECK(BedRules::in_range({4.5, -58.0, -1.5}, *from_foot));
    CHECK_FALSE(BedRules::in_range({4.5, -57.0, -1.5}, *from_foot));

    CHECK_FALSE(rules.obstructed(level, *from_foot));
    level.set_block({4, -59, 0}, state_of("minecraft:stone"));
    CHECK(rules.obstructed(level, *from_foot));
    level.set_block({4, -59, 0}, state_of("minecraft:glass"));
    CHECK_FALSE(rules.obstructed(level, *from_foot));
    level.set_block({4, -59, 0}, state_of("minecraft:oak_slab"));
    CHECK_FALSE(rules.obstructed(level, *from_foot));
    level.set_block({4, -59, 0}, registry::kAirState);

    const AABB box = BedRules::monster_box(*from_foot);
    CHECK(box.intersects(AABB::from_entity({13.7, -60.0, 0.5}, 0.6, 1.95)));
    CHECK_FALSE(box.intersects(AABB::from_entity({13.9, -60.0, 0.5}, 0.6, 1.95)));
    CHECK(box.intersects(AABB::from_entity({5.5, -55.1, 0.5}, 0.6, 1.95)));
    CHECK_FALSE(box.intersects(AABB::from_entity({5.5, -54.9, 0.5}, 0.6, 1.95)));

    const Vec3d lying = BedRules::sleeping_position(*from_foot);
    CHECK(lying.y == -60.0 + 0.6875);
    const auto occupied = rules.with_occupied(level.block_at({5, -60, 0}), true);
    CHECK(value_at(occupied, "occupied") == "true");

    // Facing south (yaw 0) on a bed that points east: up on the north side,
    // beside the head.
    const auto stand = rules.stand_up_position(level, *from_foot, 0.0F);
    REQUIRE(stand.has_value());
    CHECK(stand->x == 5.5);
    CHECK(stand->z == -0.5);
    CHECK(stand->y == -60.0);
    const auto other = rules.stand_up_position(level, *from_foot, 180.0F);
    REQUIRE(other.has_value());
    CHECK(other->z == 1.5);

    // A half on its own is not a bed.
    level.set_block({5, -60, 0}, registry::kAirState);
    CHECK_FALSE(rules.bed_at(level, {4, -60, 0}).has_value());
}
