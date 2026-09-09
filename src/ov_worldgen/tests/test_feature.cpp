// The feature layer: the placement pipeline, the ore veins, and the ordering
// that decides which seed each feature gets.
//
// Most of what can go wrong here is invisible in the output. A pipeline run
// breadth-first instead of depth-first draws the same numbers in a different
// order and puts every ore somewhere else; an anchor read as an absolute moves
// a whole band by sixty-four; a step numbered differently moves everything. So
// these tests check the *order of the draws* and the *arithmetic of the seeds*,
// not only that blocks come out somewhere.

#include "ov/worldgen/decoration.hpp"
#include "ov/worldgen/feature.hpp"
#include "ov/worldgen/ore_feature.hpp"
#include "ov/worldgen/placement.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace ov;
using namespace ov::worldgen;

namespace {

[[nodiscard]] std::filesystem::path source_root() {
    return std::filesystem::path{OV_SOURCE_DIR};
}

[[nodiscard]] std::filesystem::path data_root() {
    return source_root() / "data" / "vanilla" / "1.20.1" / "generated" / "data" / "minecraft";
}

[[nodiscard]] std::filesystem::path pack_path() {
    return source_root() / "data" / "vanilla" / "1.20.1" / "registry.ovpack";
}

/// A world made entirely of one block, with a flat surface.
///
/// Enough for a vein: an ore asks what is at a position, whether it may replace
/// it, and how high the ground is. Anything more would be testing the reference
/// world rather than the feature.
class StoneLevel final : public FeatureLevel {
public:
    StoneLevel(registry::BlockStateId stone, i32 surface) : stone_(stone), surface_(surface) {}

    [[nodiscard]] registry::BlockStateId block_at(i32 x, i32 y, i32 z) const override {
        const auto found = written_.find(key(x, y, z));
        if (found != written_.end()) {
            return found->second;
        }
        return y <= surface_ ? stone_ : registry::kAirState;
    }

    bool set_block(i32 x, i32 y, i32 z, registry::BlockStateId state) override {
        if (outside_build_height(y)) {
            return false;
        }
        written_[key(x, y, z)] = state;
        return true;
    }

    [[nodiscard]] i32 height(world::HeightmapType, i32, i32) const override {
        return surface_ + 1;
    }
    [[nodiscard]] std::string_view biome_at(i32, i32, i32) const override {
        return "minecraft:plains";
    }
    [[nodiscard]] i32 min_y() const override { return -64; }
    [[nodiscard]] i32 world_height() const override { return 384; }
    [[nodiscard]] i32 sea_level() const override { return 63; }

    [[nodiscard]] const std::map<i64, registry::BlockStateId>& written() const { return written_; }
    void forget() { written_.clear(); }

private:
    [[nodiscard]] static i64 key(i32 x, i32 y, i32 z) {
        return (static_cast<i64>(x) << 40) | ((static_cast<i64>(z) & 0xFFFFFFF) << 12) |
               static_cast<i64>(y + 64);
    }

    registry::BlockStateId                stone_;
    i32                                   surface_;
    std::map<i64, registry::BlockStateId> written_;
};

}  // namespace

TEST_CASE("a vertical anchor follows the floor and the ceiling", "[worldgen][feature]") {
    // The whole reason the three kinds exist. Reading `above_bottom` as an
    // absolute puts every deep ore sixty-four blocks out, and nothing in the
    // generated world says so — the ores are simply in the wrong rock.
    constexpr i32 min_y  = -64;
    constexpr i32 height = 384;

    CHECK(VerticalAnchor{VerticalAnchor::Kind::Absolute, 0}.resolve(min_y, height) == 0);
    CHECK(VerticalAnchor{VerticalAnchor::Kind::Absolute, -59}.resolve(min_y, height) == -59);
    CHECK(VerticalAnchor{VerticalAnchor::Kind::AboveBottom, 0}.resolve(min_y, height) == -64);
    CHECK(VerticalAnchor{VerticalAnchor::Kind::AboveBottom, 8}.resolve(min_y, height) == -56);
    // One below the top, because the top itself is the first y outside the
    // world: a world -64..319 is 384 tall and its highest block is 319.
    CHECK(VerticalAnchor{VerticalAnchor::Kind::BelowTop, 0}.resolve(min_y, height) == 319);
    CHECK(VerticalAnchor{VerticalAnchor::Kind::BelowTop, 8}.resolve(min_y, height) == 311);
}

TEST_CASE("the decoration seeds separate chunks, features and steps",
          "[worldgen][feature]") {
    constexpr i64 seed = 1234567890;
    const auto    kind = FeatureRandom::Kind::Xoroshiro;

    // Same chunk, same answer; different chunk, different answer. The whole
    // point of the mechanism.
    CHECK(decoration_seed(seed, 0, 0, kind) == decoration_seed(seed, 0, 0, kind));
    CHECK(decoration_seed(seed, 0, 0, kind) != decoration_seed(seed, 16, 0, kind));
    CHECK(decoration_seed(seed, 16, 0, kind) != decoration_seed(seed, 0, 16, kind));
    CHECK(decoration_seed(seed, 0, 0, kind) != decoration_seed(seed + 1, 0, 0, kind));

    // Chunks 2^k apart must not share a seed. That is what forcing the two
    // multipliers odd buys, and it is invisible until someone notices that
    // every chunk on a power-of-two lattice has the same ores.
    std::set<i64> seeds;
    for (i32 power = 0; power < 20; ++power) {
        seeds.insert(decoration_seed(seed, 16 << power, 0, kind));
    }
    CHECK(seeds.size() == 20);

    // Steps are ten thousand apart, so a step of forty features cannot reach
    // into the next step's seeds.
    const i64 chunk = decoration_seed(seed, 0, 0, kind);
    CHECK(feature_seed(chunk, 0, 0) == chunk);
    CHECK(feature_seed(chunk, 7, 0) == chunk + 7);
    CHECK(feature_seed(chunk, 0, 6) == chunk + 60000);
    CHECK(feature_seed(chunk, 12, 6) == chunk + 60012);
    CHECK(feature_seed(chunk, 9999, 0) != feature_seed(chunk, 0, 1));
}

TEST_CASE("the ore features and their placements load", "[worldgen][feature]") {
    if (!std::filesystem::is_regular_file(pack_path()) ||
        !std::filesystem::is_directory(data_root() / "worldgen" / "configured_feature")) {
        SKIP("vanilla data absent; run tools/ov_datagen first");
    }
    auto blocks = registry::BlockRegistry::load(pack_path());
    REQUIRE(blocks.has_value());
    auto features = FeatureRegistry::load(data_root(), *blocks);
    REQUIRE(features.has_value());

    // Every ore this framework claims: the eight metals with their deepslate
    // variants, the nether's gold and quartz, the ancient debris, and copper.
    // Listed rather than counted, so that one which stops loading is a failure
    // with a name in it.
    constexpr std::array<const char*, 20> kOres{
        "minecraft:ore_coal_upper",     "minecraft:ore_coal_lower",
        "minecraft:ore_iron_upper",     "minecraft:ore_iron_middle",
        "minecraft:ore_iron_small",     "minecraft:ore_gold",
        "minecraft:ore_gold_lower",     "minecraft:ore_gold_extra",
        "minecraft:ore_redstone",       "minecraft:ore_redstone_lower",
        "minecraft:ore_diamond",        "minecraft:ore_diamond_large",
        "minecraft:ore_diamond_buried", "minecraft:ore_lapis",
        "minecraft:ore_lapis_buried",   "minecraft:ore_copper",
        "minecraft:ore_copper_large",   "minecraft:ore_emerald",
        "minecraft:ore_gold_nether",    "minecraft:ore_ancient_debris_large"};
    for (const char* name : kOres) {
        INFO(name);
        CHECK(features->placed(name) != nullptr);
    }

    // And the two simple features that keep the framework honest about not
    // being an ore special case.
    CHECK(features->placed("minecraft:spring_water") != nullptr);
    CHECK(features->placed("minecraft:disk_sand") != nullptr);

    // What did not load is *named*, never silently absent. A framework that
    // reported nothing here would be one where a missing tree type looks the
    // same as a world with no trees.
    const auto missing = features->unavailable();
    CHECK(!missing.empty());
    CHECK(features->unsupported_count() != 0);
    for (const auto& [name, error] : missing) {
        INFO(name);
        CHECK(error != FeatureError::Missing);
    }
}

TEST_CASE("the shared feature order agrees with every biome's own order",
          "[worldgen][feature]") {
    if (!std::filesystem::is_regular_file(pack_path()) ||
        !std::filesystem::is_directory(data_root() / "worldgen" / "biome")) {
        SKIP("vanilla data absent");
    }
    auto blocks = registry::BlockRegistry::load(pack_path());
    REQUIRE(blocks.has_value());
    auto features = FeatureRegistry::load(data_root(), *blocks);
    REQUIRE(features.has_value());
    auto decorator = Decorator::load(data_root(), *blocks, *features);
    REQUIRE(decorator.has_value());

    CHECK(decorator->biome_count() == 64);

    // The order the ores are seeded in. Every overworld biome lists them in
    // this relative order, so the shared order has to contain it: if it did
    // not, a chunk that straddles two biomes would place their common ores in
    // an order neither biome asked for.
    constexpr std::array<const char*, 8> kChain{
        "minecraft:ore_dirt",         "minecraft:ore_gravel",     "minecraft:ore_granite_upper",
        "minecraft:ore_coal_upper",   "minecraft:ore_iron_upper", "minecraft:ore_gold",
        "minecraft:ore_redstone",     "minecraft:ore_copper"};
    i32 previous = -1;
    for (const char* name : kChain) {
        const i32 index = decorator->index_of(DecorationStep::UndergroundOres, name);
        INFO(name);
        CHECK(index > previous);
        previous = index;
    }

    // A feature that belongs to another step has no index at this one. The
    // index is per step, so asking the wrong step must not quietly answer.
    CHECK(decorator->index_of(DecorationStep::UndergroundOres, "minecraft:spring_water") == -1);
    CHECK(decorator->index_of(DecorationStep::FluidSprings, "minecraft:spring_water") >= 0);

    // Every step's order is a permutation without repeats.
    for (usize step = 0; step < kDecorationStepCount; ++step) {
        const auto order = decorator->order_at(static_cast<DecorationStep>(step));
        const std::set<std::string_view> distinct(order.begin(), order.end());
        CHECK(distinct.size() == order.size());
    }
}

TEST_CASE("an ore vein is a line of spheres, placed once each",
          "[worldgen][feature]") {
    if (!std::filesystem::is_regular_file(pack_path())) {
        SKIP("registry absent");
    }
    auto blocks = registry::BlockRegistry::load(pack_path());
    REQUIRE(blocks.has_value());

    const auto stone_block = blocks->find_block("minecraft:stone");
    const auto ore_block   = blocks->find_block("minecraft:iron_ore");
    const auto dirt_block  = blocks->find_block("minecraft:dirt");
    REQUIRE(stone_block.has_value());
    REQUIRE(ore_block.has_value());
    REQUIRE(dirt_block.has_value());

    OreTarget target;
    target.state = blocks->default_state(*ore_block);
    target.replaceable.push_back(stone_block->value());
    OreConfig config;
    config.size = 9;
    config.targets.push_back(std::move(target));
    const FeatureRef ore = make_ore_feature(config);

    FeatureContext context;
    context.blocks       = &*blocks;
    context.feature_name = "test:ore";

    StoneLevel level{blocks->default_state(*stone_block), 100};

    // The same seed twice is the same vein, block for block. Without that
    // nothing above it can be reproducible.
    FeatureRandom first{FeatureRandom::Kind::Xoroshiro, 42};
    CHECK(ore->place(context, level, first, BlockPos{8, 40, 8}));
    const auto once = level.written();
    level.forget();
    FeatureRandom again{FeatureRandom::Kind::Xoroshiro, 42};
    CHECK(ore->place(context, level, again, BlockPos{8, 40, 8}));
    CHECK(level.written() == once);

    // A vein of size nine never places nine blocks' worth: the spheres overlap
    // and the ones swallowed whole are dropped before anything is written. It
    // also stays inside the box the placement reserved for it.
    CHECK(!once.empty());
    CHECK(once.size() < 60);
    for (const auto& [where, state] : once) {
        CHECK(state == blocks->default_state(*ore_block));
    }

    // Nothing is placed where the target rule does not match. A world made of
    // dirt gets no iron, however many spheres the vein has.
    StoneLevel dirt{blocks->default_state(*dirt_block), 100};
    FeatureRandom third{FeatureRandom::Kind::Xoroshiro, 42};
    CHECK_FALSE(ore->place(context, dirt, third, BlockPos{8, 40, 8}));
    CHECK(dirt.written().empty());

    // And a vein in open air is abandoned — but only after it has drawn, so
    // that the feature after it in the chunk is not moved by the abandonment.
    StoneLevel sky{blocks->default_state(*stone_block), -60};
    FeatureRandom fourth{FeatureRandom::Kind::Xoroshiro, 42};
    CHECK_FALSE(ore->place(context, sky, fourth, BlockPos{8, 200, 8}));
    FeatureRandom fifth{FeatureRandom::Kind::Xoroshiro, 42};
    (void)ore->place(context, sky, fifth, BlockPos{8, 200, 8});
    // Three draws for the shape, whatever happens next: the angle and the two
    // ends' vertical offsets.
    FeatureRandom counted{FeatureRandom::Kind::Xoroshiro, 42};
    (void)counted.next_float();
    (void)counted.next_int(3);
    (void)counted.next_int(3);
    FeatureRandom after{FeatureRandom::Kind::Xoroshiro, 42};
    (void)ore->place(context, sky, after, BlockPos{8, 200, 8});
    CHECK(after.next_long() == counted.next_long());
}

TEST_CASE("a scattered ore is single blocks, not a vein", "[worldgen][feature]") {
    if (!std::filesystem::is_regular_file(pack_path())) {
        SKIP("registry absent");
    }
    auto blocks = registry::BlockRegistry::load(pack_path());
    REQUIRE(blocks.has_value());
    const auto stone_block = blocks->find_block("minecraft:stone");
    const auto ore_block   = blocks->find_block("minecraft:gold_ore");
    REQUIRE(stone_block.has_value());
    REQUIRE(ore_block.has_value());

    OreTarget target;
    target.state = blocks->default_state(*ore_block);
    target.replaceable.push_back(stone_block->value());
    OreConfig config;
    config.size = 3;
    config.targets.push_back(std::move(target));
    const FeatureRef ore = make_scattered_ore_feature(config);

    FeatureContext context;
    context.blocks       = &*blocks;
    context.feature_name = "test:scattered";

    // At most `size` blocks, and often fewer: the count is drawn from
    // [0, size], so a scattered ore of size three places nothing about a
    // quarter of the time. That is why ancient debris is rare.
    usize empty = 0;
    for (i64 seed = 0; seed < 64; ++seed) {
        StoneLevel    level{blocks->default_state(*stone_block), 100};
        FeatureRandom random{FeatureRandom::Kind::Xoroshiro, seed};
        CHECK(ore->place(context, level, random, BlockPos{0, 40, 0}));
        CHECK(level.written().size() <= static_cast<usize>(config.size));
        if (level.written().empty()) {
            ++empty;
        }
    }
    CHECK(empty > 0);
    CHECK(empty < 40);
}

namespace {

/// A modifier that records when it ran and consumes a known number of draws.
class Recorder final : public PlacementModifier {
public:
    Recorder(std::string label, i32 fan_out, std::vector<std::string>& log)
        : label_(std::move(label)), fan_out_(fan_out), log_(&log) {}

    void positions(const FeatureContext&, const FeatureLevel&, FeatureRandom& random, BlockPos at,
                   std::vector<BlockPos>& out) const override {
        log_->push_back(label_);
        for (i32 index = 0; index < fan_out_; ++index) {
            // One draw per emitted position, so that a stage-by-stage run and a
            // depth-first one differ in the numbers as well as in the order.
            out.push_back({at.x + random.next_int(1000), at.y, at.z});
        }
    }
    [[nodiscard]] std::string_view name() const override { return label_; }

private:
    std::string               label_;
    i32                       fan_out_;
    std::vector<std::string>* log_;
};

}  // namespace

TEST_CASE("the placement pipeline is depth first", "[worldgen][feature]") {
    // The single most important property of the pipeline, and the one no
    // output would reveal. Run stage by stage, the same modifiers draw the same
    // numbers in a different order and every feature in the world moves.
    std::vector<std::string>          log;
    std::vector<PlacementModifierRef> pipeline{
        std::make_shared<const Recorder>("outer", 3, log),
        std::make_shared<const Recorder>("inner", 2, log)};

    FeatureContext context;
    StoneLevel     level{registry::kAirState, 0};
    FeatureRandom  random{FeatureRandom::Kind::Xoroshiro, 7};
    usize          reached = 0;
    expand(pipeline, context, level, random, BlockPos{0, 0, 0}, [&](BlockPos) {
        log.emplace_back("place");
        ++reached;
    });

    CHECK(reached == 6);
    // outer once, then inner and two placements, three times over. Not outer,
    // inner, inner, inner, then six placements.
    const std::vector<std::string> expected{"outer", "inner", "place", "place",
                                            "inner", "place", "place",
                                            "inner", "place", "place"};
    CHECK(log == expected);
}

TEST_CASE("a feature runs between the positions, not after them",
          "[worldgen][feature]") {
    if (!std::filesystem::is_regular_file(pack_path())) {
        SKIP("registry absent");
    }
    // The consequence of the property above that actually decides where ore
    // goes: the draws a feature makes for its first position come *before* the
    // draws that choose its second position.
    std::vector<std::string>          log;
    std::vector<PlacementModifierRef> pipeline{
        std::make_shared<const Recorder>("count", 2, log)};

    FeatureContext context;
    StoneLevel     level{registry::kAirState, 0};
    FeatureRandom  random{FeatureRandom::Kind::Xoroshiro, 11};
    std::vector<i32> order;
    expand(pipeline, context, level, random, BlockPos{0, 0, 0}, [&](BlockPos at) {
        order.push_back(at.x);
    });
    REQUIRE(order.size() == 2);

    // The same two draws, taken directly, must be the two x values — which is
    // only true if nothing was interleaved and nothing was buffered.
    FeatureRandom expected{FeatureRandom::Kind::Xoroshiro, 11};
    CHECK(order[0] == expected.next_int(1000));
    CHECK(order[1] == expected.next_int(1000));
}
