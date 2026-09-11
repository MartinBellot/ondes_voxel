// ── nether-2 ── The Nether's feature types.
//
// What is checked here is what a picture of the Nether does not show: that the
// nine types load instead of being refused, that the order of a Manhattan walk
// is the one the delta and the blobs depend on, and that a feature's blocks are
// a function of its seed alone. Whether the shapes are the game's is measured
// against a Nether the real server generated, by
// `tools/ov_netherparity --full` (docs/provenance/nether-2.md).

#include "ov/worldgen/feature.hpp"
#include "ov/worldgen/nether_feature.hpp"
#include "ov/worldgen/structure_pieces.hpp"

#include <optional>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <vector>
#include <map>
#include <set>
#include <string_view>
#include <tuple>

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

[[nodiscard]] bool have_data() {
    return std::filesystem::exists(pack_path()) && std::filesystem::is_directory(data_root());
}

/// A Nether cavern: netherrack floor at y <= 40 and roof at y >= 60, air
/// between, bedrock at the bottom.
class Cavern final : public FeatureLevel {
public:
    Cavern(registry::BlockStateId floor, registry::BlockStateId roof,
           registry::BlockStateId bedrock)
        : floor_(floor), roof_(roof), bedrock_(bedrock) {}

    [[nodiscard]] registry::BlockStateId block_at(i32 x, i32 y, i32 z) const override {
        if (const auto found = written_.find({x, y, z}); found != written_.end()) {
            return found->second;
        }
        if (y <= 0) return bedrock_;
        if (y <= 40) return floor_;
        if (y >= 60) return roof_;
        return registry::kAirState;
    }

    bool set_block(i32 x, i32 y, i32 z, registry::BlockStateId state) override {
        if (outside_build_height(y)) return false;
        written_[{x, y, z}] = state;
        return true;
    }

    [[nodiscard]] i32 height(world::HeightmapType, i32, i32) const override { return 128; }
    [[nodiscard]] std::string_view biome_at(i32, i32, i32) const override {
        return "minecraft:nether_wastes";
    }
    [[nodiscard]] i32 min_y() const override { return 0; }
    [[nodiscard]] i32 world_height() const override { return 128; }
    [[nodiscard]] i32 sea_level() const override { return 32; }

    [[nodiscard]] const std::map<std::tuple<i32, i32, i32>, registry::BlockStateId>& written()
        const {
        return written_;
    }

private:
    registry::BlockStateId                                         floor_;
    registry::BlockStateId                                         roof_;
    registry::BlockStateId                                         bedrock_;
    std::map<std::tuple<i32, i32, i32>, registry::BlockStateId>    written_;
};

class Everywhere final : public BiomeFeatures {
public:
    [[nodiscard]] bool lists(std::string_view, std::string_view) const override { return true; }
};

[[nodiscard]] std::filesystem::path server_jar() {
    return source_root() / "tools" / "vanilla" / "server.jar";
}

/// A Nether whose base column is solid up to `floor`, air above, one biome.
class FlatNether final : public StructureWorldSampler {
public:
    FlatNether(i32 floor, std::string_view biome, bool knows) : floor_(floor), biome_(biome), knows_(knows) {}
    [[nodiscard]] std::string_view biome_at(i32, i32, i32) const override { return biome_; }
    [[nodiscard]] i32 surface_height(i32, i32) const override { return 128; }
    [[nodiscard]] i32 ocean_floor_height(i32, i32) const override { return 128; }
    [[nodiscard]] std::optional<bool> base_solid(i32, i32 y, i32) const override {
        if (!knows_) {
            return std::nullopt;
        }
        return y <= floor_;
    }

private:
    i32              floor_;
    std::string_view biome_;
    bool             knows_;
};

}  // namespace

TEST_CASE("a Manhattan walk visits every position once, ring by ring, mirrors in z",
          "[worldgen][nether]") {
    const BlockPos centre{10, 20, 30};
    ManhattanWalk  walk{centre, 2, 1, 3};
    BlockPos       pos;
    std::set<std::tuple<i32, i32, i32>> seen;
    i32 last_distance = 0;
    std::vector<BlockPos> order;
    while (walk.next(pos)) {
        const i32 distance =
            std::abs(pos.x - centre.x) + std::abs(pos.y - centre.y) + std::abs(pos.z - centre.z);
        CHECK(distance >= last_distance);
        last_distance = distance;
        CHECK(std::abs(pos.x - centre.x) <= 2);
        CHECK(std::abs(pos.y - centre.y) <= 1);
        CHECK(std::abs(pos.z - centre.z) <= 3);
        CHECK(seen.insert({pos.x, pos.y, pos.z}).second);
        order.push_back(pos);
    }
    CHECK(seen.size() == static_cast<usize>(5 * 3 * 7));

    // The first ring, in its fixed order: the centre; then x = -1; then
    // x = 0 with y = -1, then (0, 0, +1) and its mirror; then y = +1; x = +1.
    REQUIRE(order.size() >= 8);
    CHECK(order[0] == centre);
    CHECK(order[1] == centre.offset(-1, 0, 0));
    CHECK(order[2] == centre.offset(0, -1, 0));
    CHECK(order[3] == centre.offset(0, 0, 1));
    CHECK(order[4] == centre.offset(0, 0, -1));
    CHECK(order[5] == centre.offset(0, 1, 0));
    CHECK(order[6] == centre.offset(1, 0, 0));
}

TEST_CASE("the Nether's nine feature types load", "[worldgen][nether][feature]") {
    if (!have_data()) {
        SUCCEED("no generated data; run tools/ov_datagen");
        return;
    }
    auto pack = registry::BlockRegistry::load(pack_path());
    REQUIRE(pack.has_value());
    auto registry = FeatureRegistry::load(data_root(), *pack);
    REQUIRE(registry.has_value());

    static constexpr std::array<std::string_view, 14> kConfigured{
        "minecraft:glowstone_extra",           "minecraft:crimson_fungus",
        "minecraft:warped_fungus",             "minecraft:crimson_forest_vegetation",
        "minecraft:warped_forest_vegetation",  "minecraft:nether_sprouts",
        "minecraft:weeping_vines",             "minecraft:twisting_vines",
        "minecraft:small_basalt_columns",      "minecraft:large_basalt_columns",
        "minecraft:basalt_pillar",             "minecraft:delta",
        "minecraft:basalt_blobs",              "minecraft:blackstone_blobs",
    };
    for (const std::string_view name : kConfigured) {
        INFO(name);
        CHECK(registry->configured(name) != nullptr);
    }
    static constexpr std::array<std::string_view, 15> kPlaced{
        "minecraft:glowstone",          "minecraft:glowstone_extra",
        "minecraft:crimson_fungi",      "minecraft:warped_fungi",
        "minecraft:crimson_forest_vegetation", "minecraft:warped_forest_vegetation",
        "minecraft:nether_sprouts",     "minecraft:weeping_vines",
        "minecraft:twisting_vines",     "minecraft:small_basalt_columns",
        "minecraft:large_basalt_columns", "minecraft:basalt_pillar",
        "minecraft:delta",              "minecraft:basalt_blobs",
        "minecraft:blackstone_blobs",
    };
    for (const std::string_view name : kPlaced) {
        INFO(name);
        CHECK(registry->placed(name) != nullptr);
    }
    // The planted fungus (bone meal's) is refused by name: it breaks blocks
    // with drops, which a FeatureLevel cannot do.
    CHECK(registry->configured("minecraft:crimson_fungus_planted") == nullptr);
}

TEST_CASE("a glowstone blob hangs from the roof, and its shape is its seed's",
          "[worldgen][nether][feature]") {
    if (!have_data()) {
        SUCCEED("no generated data; run tools/ov_datagen");
        return;
    }
    auto pack = registry::BlockRegistry::load(pack_path());
    REQUIRE(pack.has_value());
    auto registry = FeatureRegistry::load(data_root(), *pack);
    REQUIRE(registry.has_value());
    const auto* blob = registry->configured("minecraft:glowstone_extra");
    REQUIRE(blob != nullptr);
    const auto netherrack = pack->find_block("minecraft:netherrack");
    const auto bedrock    = pack->find_block("minecraft:bedrock");
    const auto glowstone  = pack->find_block("minecraft:glowstone");
    REQUIRE(netherrack.has_value());
    REQUIRE(bedrock.has_value());
    REQUIRE(glowstone.has_value());

    const Everywhere everywhere;
    FeatureContext   context;
    context.blocks = &*pack;
    context.biomes = &everywhere;

    const auto grow = [&](i64 seed, BlockPos at) {
        Cavern level{pack->default_state(*netherrack), pack->default_state(*netherrack),
                     pack->default_state(*bedrock)};
        FeatureRandom random{FeatureRandom::Kind::Legacy, seed};
        const bool    placed = blob->place(context, level, random, at);
        return std::pair{placed, level.written()};
    };

    // Not under the roof: nothing.
    CHECK_FALSE(grow(1, {0, 50, 0}).first);
    for (i64 seed = 1; seed <= 10; ++seed) {
        const auto [placed, blocks] = grow(seed, {0, 59, 0});
        REQUIRE(placed);
        CHECK(blocks == grow(seed, {0, 59, 0}).second);
        // Every block glowstone, all within reach of the origin, none above it.
        for (const auto& [where, state] : blocks) {
            const auto [x, y, z] = where;
            CHECK(pack->block_of(state) == *glowstone);
            CHECK(y <= 59);
            CHECK(y >= 59 - 11);
            CHECK(std::abs(x) <= 7);
            CHECK(std::abs(z) <= 7);
        }
        CHECK(blocks.size() > 1);
    }
}

TEST_CASE("a Nether fossil stands on the first floor under its height, in a fossil biome",
          "[worldgen][nether][structure]") {
    if (!have_data() || !std::filesystem::exists(server_jar())) {
        SKIP("vanilla data, registry pack or tools/vanilla/server.jar absent");
    }
    auto pack = registry::BlockRegistry::load(pack_path());
    REQUIRE(pack.has_value());
    auto tags = BlockTags::load(data_root(), *pack);
    REQUIRE(tags.has_value());
    std::string detail;
    auto builder = StructureBuilder::load(server_jar(), data_root(), *pack, *tags, &detail);
    REQUIRE(builder.has_value());

    StructureDefinition fossil;
    fossil.name = "minecraft:nether_fossil";
    fossil.kind = StructureKind::NetherFossil;

    // A floor at 60: every start that finds one sits on it, whatever its height.
    const FlatNether valley{60, "minecraft:soul_sand_valley", true};
    i32              placed = 0;
    for (i32 cx = 0; cx < 12; ++cx) {
        const auto start = builder->generate(fossil, 1234567890, cx, 3, &valley);
        if (!start) {
            // A height drawn under the floor finds solid all the way down.
            CHECK(start.error() == "nether fossil: no floor in the column");
            continue;
        }
        ++placed;
        REQUIRE(start->pieces.size() == 1);
        const StructurePiece& piece = start->pieces.front();
        CHECK(piece.kind == PieceKind::NetherFossil);
        CHECK(piece.origin.y == 60);
        CHECK(piece.origin.x >= cx * 16);
        CHECK(piece.origin.x < cx * 16 + 16);
        CHECK(piece.template_name.starts_with("minecraft:nether_fossils/fossil_"));
        // The same seed, the same start.
        const auto again = builder->generate(fossil, 1234567890, cx, 3, &valley);
        REQUIRE(again.has_value());
        CHECK(again->pieces.front().template_name == piece.template_name);
        CHECK(again->pieces.front().rotation == piece.rotation);
    }
    CHECK(placed > 0);

    // Outside the tag's biomes, refused; with no base column, refused by name.
    const FlatNether wastes{60, "minecraft:nether_wastes", true};
    const FlatNether blind{60, "minecraft:soul_sand_valley", false};
    for (i32 cx = 0; cx < 12; ++cx) {
        const auto there = builder->generate(fossil, 1234567890, cx, 3, &wastes);
        CHECK_FALSE(there.has_value());
        const auto unknown = builder->generate(fossil, 1234567890, cx, 3, &blind);
        CHECK_FALSE(unknown.has_value());
    }
}
