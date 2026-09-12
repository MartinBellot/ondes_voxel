// ── worldgen-3 ── Icebergs, blue ice, fossils, and the two surface passes.
//
// What is checked here is what no measurement shows directly: that the types
// load, that a shape depends on its seed and on nothing else, that each
// feature refuses where the game's refuses, and that the passes only ever
// touch their own biome. Whether the shapes are the game's is measured against
// probe worlds of the real server (tools/ov_features --probe --control,
// tools/ov_surfparity), not asserted here.

#include "ov/worldgen/biome_zoom.hpp"
#include "ov/worldgen/feature.hpp"
#include "ov/worldgen/structure_template.hpp"
#include "ov/worldgen/surface_system.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <string_view>
#include <tuple>
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
[[nodiscard]] std::filesystem::path jar_path() {
    return source_root() / "tools" / "vanilla" / "server.jar";
}
[[nodiscard]] bool have_data() {
    return std::filesystem::exists(pack_path()) && std::filesystem::is_directory(data_root());
}

/// A sea: stone up to `floor`, water up to 62, air above. Writes are kept.
class SeaLevel final : public FeatureLevel {
public:
    SeaLevel(registry::BlockStateId stone, registry::BlockStateId water, i32 floor)
        : stone_(stone), water_(water), floor_(floor) {}

    [[nodiscard]] registry::BlockStateId block_at(i32 x, i32 y, i32 z) const override {
        if (const auto found = written_.find(key(x, y, z)); found != written_.end()) {
            return found->second;
        }
        if (y <= floor_) return stone_;
        return y <= 62 ? water_ : registry::kAirState;
    }
    bool set_block(i32 x, i32 y, i32 z, registry::BlockStateId state) override {
        if (outside_build_height(y)) return false;
        written_[key(x, y, z)] = state;
        return true;
    }
    [[nodiscard]] i32 height(world::HeightmapType type, i32, i32) const override {
        return type == world::HeightmapType::OceanFloorWG ? floor_ + 1 : 63;
    }
    [[nodiscard]] std::string_view biome_at(i32, i32, i32) const override {
        return "minecraft:frozen_ocean";
    }
    [[nodiscard]] i32 min_y() const override { return -64; }
    [[nodiscard]] i32 world_height() const override { return 384; }
    [[nodiscard]] i32 sea_level() const override { return 63; }

    [[nodiscard]] const std::map<i64, registry::BlockStateId>& written() const { return written_; }

private:
    [[nodiscard]] static i64 key(i32 x, i32 y, i32 z) {
        return (static_cast<i64>(x) << 40) | ((static_cast<i64>(z) & 0xFFFFF) << 20) |
               static_cast<i64>(y + 64);
    }

    registry::BlockStateId                stone_;
    registry::BlockStateId                water_;
    i32                                   floor_;
    std::map<i64, registry::BlockStateId> written_;
};

[[nodiscard]] registry::BlockStateId state(const registry::BlockRegistry& blocks,
                                           std::string_view name) {
    const auto block = blocks.find_block(name);
    REQUIRE(block.has_value());
    return blocks.default_state(*block);
}

[[nodiscard]] usize count_of(const SeaLevel& level, registry::BlockStateId wanted) {
    usize count = 0;
    for (const auto& [where, written] : level.written()) {
        count += written == wanted ? 1 : 0;
    }
    return count;
}

/// A column the surface stage is told about, with a chosen biome.
class ColumnQueries final : public SurfaceQueries {
public:
    explicit ColumnQueries(std::string_view biome, f64 temperature)
        : biome_(biome), temperature_(temperature) {}
    [[nodiscard]] std::string_view biome_at(i32, i32, i32) const override { return biome_; }
    [[nodiscard]] f64 temperature_at(i32, i32, i32) const override { return temperature_; }
    [[nodiscard]] i32 surface_height(i32, i32) const override { return 62; }
    [[nodiscard]] i32 preliminary_surface(i32, i32) const override { return 40; }

private:
    std::string_view biome_;
    f64              temperature_;
};

}  // namespace

TEST_CASE("SHA-256 gives the standard's own digests", "[worldgen][biome][worldgen3]") {
    // FIPS 180-4, appendix B: "abc", and the empty message.
    const std::array<u8, 3> abc{'a', 'b', 'c'};
    const auto              digest = sha256(abc);
    const std::array<u8, 32> expected{0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
                                      0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
                                      0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
                                      0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    CHECK(digest == expected);
    const auto empty = sha256({});
    CHECK(empty[0] == 0xe3);
    CHECK(empty[1] == 0xb0);
    CHECK(empty[31] == 0x55);
    // 56 bytes: the padding spills into a second block.
    const std::string_view two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    const auto             long_digest =
        sha256(std::span<const u8>(reinterpret_cast<const u8*>(two.data()), two.size()));
    CHECK(long_digest[0] == 0x24);
    CHECK(long_digest[1] == 0x8d);
    CHECK(long_digest[31] == 0xc1);
}

TEST_CASE("the biome zoom reads the block's cell or a neighbour towards its corner",
          "[worldgen][biome][worldgen3]") {
    const i64 seed = obfuscate_biome_seed(1234567890);
    CHECK(seed == obfuscate_biome_seed(1234567890));
    CHECK(seed != obfuscate_biome_seed(1234567891));
    usize moved = 0;
    for (i32 x = -40; x < 40; ++x) {
        for (i32 z = -40; z < 40; ++z) {
            const BiomeCell cell = fuzzy_biome_cell(seed, x, 70, z);
            const i32       own_x = x >> 2;
            const i32       own_z = z >> 2;
            // Towards the nearest corner: the block's own cell, or the one it
            // leans to after the half-cell shift.
            CHECK(cell.x >= ((x - 2) >> 2));
            CHECK(cell.x <= ((x - 2) >> 2) + 1);
            CHECK(cell.z >= ((z - 2) >> 2));
            CHECK(cell.z <= ((z - 2) >> 2) + 1);
            moved += (cell.x != own_x || cell.z != own_z) ? 1 : 0;
            CHECK(cell == fuzzy_biome_cell(seed, x, 70, z));
        }
    }
    // Not the identity, and not a wholesale shift either.
    CHECK(moved > 0);
    CHECK(moved < 80 * 80);
}

TEST_CASE("icebergs, blue ice and fossils load", "[worldgen][feature][worldgen3]") {
    if (!have_data()) {
        SUCCEED("no generated data; run tools/ov_datagen");
        return;
    }
    auto pack = registry::BlockRegistry::load(pack_path());
    REQUIRE(pack.has_value());
    auto registry = FeatureRegistry::load(data_root(), *pack);
    REQUIRE(registry.has_value());
    for (const std::string_view name :
         {"minecraft:iceberg_packed", "minecraft:iceberg_blue", "minecraft:blue_ice",
          "minecraft:fossil_coal", "minecraft:fossil_diamonds", "minecraft:sculk_vein"}) {
        INFO(name);
        CHECK(registry->configured(name) != nullptr);
    }
    for (const std::string_view name : {"minecraft:iceberg_packed", "minecraft:iceberg_blue",
                                        "minecraft:blue_ice", "minecraft:fossil_upper",
                                        "minecraft:fossil_lower", "minecraft:sculk_vein"}) {
        INFO(name);
        CHECK(registry->placed(name) != nullptr);
    }
}

TEST_CASE("an iceberg floats on the sea, and its shape is its seed's",
          "[worldgen][feature][worldgen3]") {
    if (!have_data()) {
        SUCCEED("no generated data; run tools/ov_datagen");
        return;
    }
    auto pack = registry::BlockRegistry::load(pack_path());
    REQUIRE(pack.has_value());
    auto registry = FeatureRegistry::load(data_root(), *pack);
    REQUIRE(registry.has_value());
    const Feature* iceberg = registry->configured("minecraft:iceberg_packed");
    REQUIRE(iceberg != nullptr);

    const auto stone  = state(*pack, "minecraft:stone");
    const auto water  = state(*pack, "minecraft:water");
    const auto packed = state(*pack, "minecraft:packed_ice");

    FeatureContext context;
    context.blocks = &*pack;
    const auto run = [&](i64 seed) {
        auto          level = std::make_unique<SeaLevel>(stone, water, 30);
        FeatureRandom random{configured_feature_random(), seed};
        // The pipeline's height is ignored: a berg sits on the sea.
        (void)iceberg->place(context, *level, random, {8, -64, 8});
        return level;
    };
    usize differing_seeds = 0;
    for (i64 seed = 1; seed <= 12; ++seed) {
        const auto a = run(seed);
        const auto b = run(seed);
        CHECK(a->written() == b->written());
        CHECK(count_of(*a, packed) > 0);
        // Nothing is written below the root's deepest reach or far above the sea.
        for (const auto& [where, written] : a->written()) {
            const auto y = static_cast<i32>(where & 0xFFFFF) - 64;
            CHECK(y > 62 - 18);
            CHECK(y < 63 + 60);
        }
        differing_seeds += a->written() != run(seed + 1000)->written() ? 1 : 0;
    }
    CHECK(differing_seeds == 12);
}

TEST_CASE("blue ice only grows against packed ice, under the sea", "[worldgen][feature][worldgen3]") {
    if (!have_data()) {
        SUCCEED("no generated data; run tools/ov_datagen");
        return;
    }
    auto pack = registry::BlockRegistry::load(pack_path());
    REQUIRE(pack.has_value());
    auto registry = FeatureRegistry::load(data_root(), *pack);
    REQUIRE(registry.has_value());
    const Feature* blue = registry->configured("minecraft:blue_ice");
    REQUIRE(blue != nullptr);

    const auto stone  = state(*pack, "minecraft:stone");
    const auto water  = state(*pack, "minecraft:water");
    const auto packed = state(*pack, "minecraft:packed_ice");
    const auto blue_s = state(*pack, "minecraft:blue_ice");

    FeatureContext context;
    context.blocks = &*pack;
    {
        SeaLevel      level{stone, water, 30};
        FeatureRandom random{configured_feature_random(), 7};
        CHECK_FALSE(blue->place(context, level, random, {0, 45, 0}));
        CHECK(level.written().empty());
    }
    {
        SeaLevel level{stone, water, 30};
        for (i32 y = 31; y <= 70; ++y) {
            (void)level.set_block(1, y, 0, packed);
        }
        FeatureRandom random{configured_feature_random(), 7};
        CHECK(blue->place(context, level, random, {0, 45, 0}));
        CHECK(level.block_at(0, 45, 0) == blue_s);
        // Above the sea it refuses whatever is next to it.
        FeatureRandom again{configured_feature_random(), 7};
        CHECK_FALSE(blue->place(context, level, again, {0, 63, 0}));
    }
}

TEST_CASE("a fossil is buried under the floor, from the jar's templates, or not at all",
          "[worldgen][feature][worldgen3]") {
    if (!have_data()) {
        SUCCEED("no generated data; run tools/ov_datagen");
        return;
    }
    auto pack = registry::BlockRegistry::load(pack_path());
    REQUIRE(pack.has_value());
    auto registry = FeatureRegistry::load(data_root(), *pack);
    REQUIRE(registry.has_value());
    const Feature* fossil = registry->configured("minecraft:fossil_coal");
    REQUIRE(fossil != nullptr);

    const auto stone = state(*pack, "minecraft:stone");
    const auto water = state(*pack, "minecraft:water");
    const auto bone  = state(*pack, "minecraft:bone_block");

    // Without templates: nothing, rather than a guess at a skull.
    FeatureContext bare;
    bare.blocks = &*pack;
    {
        SeaLevel      level{stone, water, 50};
        FeatureRandom random{configured_feature_random(), 3};
        CHECK_FALSE(fossil->place(bare, level, random, {8, 50, 8}));
        CHECK(level.written().empty());
    }

    if (!std::filesystem::exists(jar_path())) {
        SUCCEED("no server jar; the templated half is not checked");
        return;
    }
    static constexpr std::array<std::string_view, 1> kFamilies{"fossil/"};
    auto templates = TemplateLibrary::open(jar_path(), *pack, kFamilies);
    REQUIRE(templates.has_value());
    FeatureContext context;
    context.blocks    = &*pack;
    context.templates = &*templates;
    usize placed = 0;
    for (i64 seed = 1; seed <= 8; ++seed) {
        SeaLevel      a{stone, water, 50};
        SeaLevel      b{stone, water, 50};
        FeatureRandom ra{configured_feature_random(), seed};
        FeatureRandom rb{configured_feature_random(), seed};
        const bool    wrote = fossil->place(context, a, ra, {8, 50, 8});
        (void)fossil->place(context, b, rb, {8, 50, 8});
        CHECK(a.written() == b.written());
        if (!wrote) {
            continue;
        }
        ++placed;
        CHECK(count_of(a, bone) > 0);
        // Fifteen to twenty-four under the ocean floor at 51, never above it.
        for (const auto& [where, written] : a.written()) {
            const auto y = static_cast<i32>(where & 0xFFFFF) - 64;
            CHECK(y <= 51 - 15 + 16);
            CHECK(y >= 51 - 24);
        }
    }
    CHECK(placed > 0);
}

TEST_CASE("the iceberg pass stacks ice over frozen oceans and nowhere else",
          "[worldgen][surface][worldgen3]") {
    if (!have_data()) {
        SUCCEED("no generated data; run tools/ov_datagen");
        return;
    }
    auto pack = registry::BlockRegistry::load(pack_path());
    REQUIRE(pack.has_value());
    auto surface = SurfaceSystem::load(data_root(), "overworld", 1234567890, *pack);
    REQUIRE(surface.has_value());

    const auto stone  = state(*pack, "minecraft:stone");
    const auto water  = state(*pack, "minecraft:water");
    const auto packed = state(*pack, "minecraft:packed_ice");
    const auto snow   = state(*pack, "minecraft:snow_block");

    const auto ice_in = [&](std::string_view biome, i32 x, i32 z) {
        std::vector<registry::BlockStateId> column(384, registry::kAirState);
        for (i32 y = -64; y <= 62; ++y) {
            column[static_cast<usize>(y + 64)] = y <= 40 ? stone : water;
        }
        const ColumnQueries queries{biome, 0.0};
        surface->build_column(column, x, z, queries, *pack);
        usize ice = 0;
        for (const auto cell : column) {
            ice += (cell == packed || cell == snow) ? 1 : 0;
        }
        return ice;
    };
    usize frozen = 0;
    usize plain  = 0;
    for (i32 x = 0; x < 256; x += 3) {
        for (i32 z = 0; z < 256; z += 5) {
            frozen += ice_in("minecraft:frozen_ocean", x, z);
            plain += ice_in("minecraft:ocean", x, z);
            CHECK(ice_in("minecraft:frozen_ocean", x, z) == ice_in("minecraft:frozen_ocean", x, z));
        }
    }
    CHECK(frozen > 0);
    CHECK(plain == 0);
}

namespace {

/// One kind of column everywhere: stone, then `top_block` at `top`, air above,
/// in one biome.
class ColumnLevel final : public FeatureLevel {
public:
    ColumnLevel(registry::BlockStateId stone, registry::BlockStateId top_block, i32 top,
                std::string_view biome)
        : stone_(stone), top_block_(top_block), top_(top), biome_(biome) {}

    [[nodiscard]] registry::BlockStateId block_at(i32 x, i32 y, i32 z) const override {
        if (const auto found = written_.find({x, y, z}); found != written_.end()) {
            return found->second;
        }
        if (y > top_) return registry::kAirState;
        return y == top_ ? top_block_ : stone_;
    }
    bool set_block(i32 x, i32 y, i32 z, registry::BlockStateId state) override {
        written_[{x, y, z}] = state;
        return true;
    }
    [[nodiscard]] i32 height(world::HeightmapType, i32, i32) const override { return top_ + 1; }
    [[nodiscard]] std::string_view biome_at(i32, i32, i32) const override { return biome_; }
    [[nodiscard]] i32 min_y() const override { return -64; }
    [[nodiscard]] i32 world_height() const override { return 384; }
    [[nodiscard]] i32 sea_level() const override { return 63; }

    std::map<std::tuple<i32, i32, i32>, registry::BlockStateId> written_;

private:
    registry::BlockStateId stone_;
    registry::BlockStateId top_block_;
    i32                    top_;
    std::string_view       biome_;
};

}  // namespace

TEST_CASE("the top layer freezes where the biome is cold at that height, and only there",
          "[worldgen][feature][worldgen3]") {
    if (!have_data()) {
        SUCCEED("no generated data; run tools/ov_datagen");
        return;
    }
    auto pack = registry::BlockRegistry::load(pack_path());
    REQUIRE(pack.has_value());
    auto registry = FeatureRegistry::load(data_root(), *pack);
    REQUIRE(registry.has_value());
    const Feature* freeze = registry->configured("minecraft:freeze_top_layer");
    REQUIRE(freeze != nullptr);
    CHECK(registry->placed("minecraft:freeze_top_layer") != nullptr);

    const auto stone = state(*pack, "minecraft:stone");
    const auto grass = state(*pack, "minecraft:grass_block");
    const auto water = state(*pack, "minecraft:water");
    const auto snow  = state(*pack, "minecraft:snow");
    const auto ice   = state(*pack, "minecraft:ice");
    const auto grass_block = pack->find_block("minecraft:grass_block");
    REQUIRE(grass_block.has_value());
    const std::array<std::pair<std::string_view, std::string_view>, 1> snowy_on{
        std::pair{std::string_view("snowy"), std::string_view("true")}};
    const auto snowy_grass = pack->state_for(*grass_block, snowy_on);
    REQUIRE(snowy_grass.has_value());

    FeatureContext context;
    context.blocks = &*pack;
    FeatureRandom random{configured_feature_random(), 0};
    const auto    run = [&](registry::BlockStateId top_block, i32 top, std::string_view biome) {
        auto level = std::make_unique<ColumnLevel>(stone, top_block, top, biome);
        (void)freeze->place(context, *level, random, {32, -64, 48});
        return level;
    };
    const auto count = [](const ColumnLevel& level, registry::BlockStateId wanted) {
        usize n = 0;
        for (const auto& [where, written] : level.written_) {
            n += written == wanted ? 1 : 0;
        }
        return n;
    };

    const auto snowy = run(grass, 70, "minecraft:snowy_plains");
    CHECK(count(*snowy, snow) == 256);
    CHECK(count(*snowy, *snowy_grass) == 256);
    CHECK(snowy->block_at(32, 71, 48) == snow);
    // Only the chunk's own columns.
    CHECK(snowy->block_at(31, 71, 48) == registry::kAirState);

    const auto warm = run(grass, 70, "minecraft:plains");
    CHECK(warm->written_.empty());

    // The river freezes, and no snow is laid on the ice made in the same pass.
    const auto river = run(water, 62, "minecraft:frozen_river");
    CHECK(count(*river, ice) == 256);
    CHECK(count(*river, snow) == 0);

    // A taiga (0.25) is too warm to snow at 70 and cold enough at 250: the
    // cooling above y = 80 is what puts snow on its mountains.
    CHECK(run(grass, 70, "minecraft:taiga")->written_.empty());
    CHECK(count(*run(grass, 250, "minecraft:taiga"), snow) == 256);
}
