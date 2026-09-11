// Structure templates and the pieces built from them.
//
// The measurement is `tools/ov_structblocks` against the reference worlds; these
// tests freeze what it established, so that a refactor that turns a ship the
// wrong way fails here rather than in a screenshot. Every pinned value below is
// the game's own — read from the `structures.starts` a reference world stored —
// never a value our code produced and we then copied.
//
// The templates themselves are Mojang's data and are never in the repository:
// the tests that need them read `tools/vanilla/server.jar` and skip when it is
// absent, saying so.

#include "ov/registry/block_states.hpp"
#include "ov/world/chunk.hpp"
#include "ov/worldgen/placement.hpp"
#include "ov/worldgen/structure.hpp"
#include "ov/worldgen/structure_pieces.hpp"
#include "ov/worldgen/structure_set.hpp"
#include "ov/worldgen/structure_stage.hpp"
#include "ov/worldgen/structure_template.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>

using namespace ov;
using namespace ov::worldgen;

namespace {

[[nodiscard]] std::filesystem::path data_root() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "generated" /
           "data" / "minecraft";
}

[[nodiscard]] std::filesystem::path registry_pack() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack";
}

[[nodiscard]] std::filesystem::path server_jar() {
    return std::filesystem::path{OV_SOURCE_DIR} / "tools" / "vanilla" / "server.jar";
}

[[nodiscard]] bool have_registry() {
    return std::filesystem::is_regular_file(registry_pack());
}

[[nodiscard]] bool have_everything() {
    return have_registry() && std::filesystem::is_directory(data_root() / "worldgen") &&
           std::filesystem::exists(server_jar());
}

constexpr i64 kSeed = 1234567890;

/// A level made of a map: every read of an unwritten block is air, unless a
/// `ground` below which everything is stone was asked for.
class MapLevel final : public StructureLevel {
public:
    explicit MapLevel(registry::BlockStateId ground_state = registry::kAirState, i32 ground = -64)
        : ground_state_(ground_state), ground_(ground) {}

    [[nodiscard]] registry::BlockStateId block_at(i32 x, i32 y, i32 z) const override {
        if (const auto it = blocks_.find({x, y, z}); it != blocks_.end()) {
            return it->second;
        }
        return y < ground_ ? ground_state_ : registry::kAirState;
    }

    bool set_block(i32 x, i32 y, i32 z, registry::BlockStateId state) override {
        blocks_[{x, y, z}] = state;
        return true;
    }

    [[nodiscard]] i32 height(world::HeightmapType /*type*/, i32 /*x*/, i32 /*z*/) const override {
        return ground_;
    }

    [[nodiscard]] std::string_view biome_at(i32, i32, i32) const override {
        return "minecraft:plains";
    }

    [[nodiscard]] i32 min_y() const override { return -64; }

    [[nodiscard]] i32 world_height() const override { return 384; }

    [[nodiscard]] i32 sea_level() const override { return 63; }

    void set_block_entity(i32 x, i32 y, i32 z, nbt::Tag data) override {
        entities_[{x, y, z}] = std::move(data);
    }

    [[nodiscard]] const nbt::Tag* block_entity(i32 x, i32 y, i32 z) const override {
        const auto it = entities_.find({x, y, z});
        return it == entities_.end() ? nullptr : &it->second;
    }

    [[nodiscard]] const std::map<std::tuple<i32, i32, i32>, registry::BlockStateId>& blocks()
        const {
        return blocks_;
    }

    [[nodiscard]] const std::map<std::tuple<i32, i32, i32>, nbt::Tag>& entities() const {
        return entities_;
    }

private:
    registry::BlockStateId                                      ground_state_;
    i32                                                         ground_;
    std::map<std::tuple<i32, i32, i32>, registry::BlockStateId> blocks_;
    std::map<std::tuple<i32, i32, i32>, nbt::Tag>               entities_;
};

[[nodiscard]] registry::BlockStateId parse_state(
    const registry::BlockRegistry& blocks, std::string_view name,
    std::initializer_list<std::pair<std::string_view, std::string_view>> values) {
    auto state = blocks.default_state(*blocks.find_block(name));
    for (const auto& [key, value] : values) {
        state = with_value(blocks, state, key, value);
    }
    return state;
}

[[nodiscard]] std::string fmt_position(const std::tuple<i32, i32, i32>& position) {
    return "(" + std::to_string(std::get<0>(position)) + "," +
           std::to_string(std::get<1>(position)) + "," + std::to_string(std::get<2>(position)) +
           ")";
}

[[nodiscard]] std::string_view value_of(const registry::BlockRegistry& blocks,
                                        registry::BlockStateId state, std::string_view key) {
    const auto view = blocks.find_property(blocks.block_of(state), key);
    return view ? blocks.property_value(state, *view) : std::string_view{};
}

}  // namespace

TEST_CASE("a rotation turns about the pivot and leaves it where it is", "[structure][template]") {
    const BlockPos pivot{4, 0, 15};
    for (const Rotation rotation : {Rotation::None, Rotation::Clockwise90, Rotation::Clockwise180,
                                    Rotation::CounterClockwise90}) {
        CHECK(transform(pivot, Mirror::None, rotation, pivot) == pivot);
    }
    // Four quarter turns are the identity; two are a half turn.
    BlockPos p{3, 7, -2};
    BlockPos q = p;
    for (int turn = 0; turn < 4; ++turn) {
        q = transform(q, Mirror::None, Rotation::Clockwise90, pivot);
    }
    CHECK(q == p);
    CHECK(transform(transform(p, Mirror::None, Rotation::Clockwise90, pivot), Mirror::None,
                    Rotation::Clockwise90,
                    pivot) == transform(p, Mirror::None, Rotation::Clockwise180, pivot));
    // A mirror negates one axis before the turn.
    CHECK(transform({2, 0, 3}, Mirror::LeftRight, Rotation::None, {}) == BlockPos{2, 0, -3});
    CHECK(transform({2, 0, 3}, Mirror::FrontBack, Rotation::None, {}) == BlockPos{-2, 0, 3});
}

TEST_CASE("the box of a turned ship is the one the game stored", "[structure][template]") {
    // Shipwreck `upsidedown_backhalf`, 9 x 9 x 16, turned COUNTERCLOCKWISE_90
    // about (4, 0, 15), origin (-104960, 43, 7072): the game stored the box
    // [-104971, 43, 7083, -104956, 51, 7091] in chunk (-6560, 442) of
    // run/reference-1234567890.
    StructureTemplate tpl;
    tpl.size              = {9, 9, 16};
    const BoundingBox box = tpl.bounding_box({-104960, 43, 7072}, Mirror::None,
                                             Rotation::CounterClockwise90, {4, 0, 15});
    CHECK(box.min_x == -104971);
    CHECK(box.min_y == 43);
    CHECK(box.min_z == 7083);
    CHECK(box.max_x == -104956);
    CHECK(box.max_y == 51);
    CHECK(box.max_z == 7091);

    // A ruined portal, mirrored FRONT_BACK and turned, about the middle of its
    // footprint: portal_4 (8 x 9 x 9) at (73136, 64, 63040), stored box
    // [73136, 64, 63048, 73144, 72, 63055].
    StructureTemplate portal;
    portal.size                  = {8, 9, 9};
    const BoundingBox portal_box = portal.bounding_box({73136, 64, 63040}, Mirror::FrontBack,
                                                       Rotation::CounterClockwise90, {4, 0, 4});
    CHECK(portal_box.min_x == 73136);
    CHECK(portal_box.min_z == 63048);
    CHECK(portal_box.max_x == 73144);
    CHECK(portal_box.max_z == 63055);
}

TEST_CASE("the position seed wraps the x product at 32 bits", "[structure][template]") {
    // The x term is an int product: 700 * 3129871 overflows an int and must
    // wrap before it meets the long terms, so the seed of x = 700 is not the
    // seed computed in 64-bit arithmetic throughout.
    const i64 wrapped = position_seed(700, 64, 0);
    u64       naive   = static_cast<u64>(700LL * 3129871LL) ^ static_cast<u64>(64LL);
    naive             = naive * naive * 42317861ULL + naive * 11ULL;
    CHECK(wrapped != static_cast<i64>(naive) >> 16);
    // Deterministic, and position-keyed.
    CHECK(position_seed(1, 2, 3) == position_seed(1, 2, 3));
    CHECK(position_seed(1, 2, 3) != position_seed(3, 2, 1));
}

TEST_CASE("states turn and mirror as their blocks do", "[structure][template]") {
    if (!have_registry()) {
        SKIP("registry pack absent; run tools/ov_datagen/ovpack.py");
    }
    auto blocks = registry::BlockRegistry::load(registry_pack());
    REQUIRE(blocks.has_value());

    const auto stairs = parse_state(*blocks, "minecraft:oak_stairs",
                                    {{"facing", "north"}, {"shape", "outer_left"}});
    const auto turned = transform_state(*blocks, stairs, Mirror::None, Rotation::Clockwise90);
    CHECK(value_of(*blocks, turned, "facing") == "east");
    CHECK(value_of(*blocks, turned, "shape") == "outer_left");
    const auto mirrored = transform_state(*blocks, stairs, Mirror::LeftRight, Rotation::None);
    CHECK(value_of(*blocks, mirrored, "facing") == "south");
    CHECK(value_of(*blocks, mirrored, "shape") == "outer_right");

    const auto log = parse_state(*blocks, "minecraft:oak_log", {{"axis", "x"}});
    CHECK(value_of(*blocks, transform_state(*blocks, log, Mirror::None, Rotation::Clockwise90),
                   "axis") == "z");
    CHECK(value_of(*blocks, transform_state(*blocks, log, Mirror::None, Rotation::Clockwise180),
                   "axis") == "x");

    const auto sign = parse_state(*blocks, "minecraft:oak_sign", {{"rotation", "0"}});
    CHECK(value_of(*blocks, transform_state(*blocks, sign, Mirror::None, Rotation::Clockwise90),
                   "rotation") == "4");
    CHECK(value_of(*blocks, transform_state(*blocks, sign, Mirror::LeftRight, Rotation::None),
                   "rotation") == "8");

    const auto rail = parse_state(*blocks, "minecraft:rail", {{"shape", "south_east"}});
    CHECK(value_of(*blocks, transform_state(*blocks, rail, Mirror::None, Rotation::Clockwise90),
                   "shape") == "south_west");
    const auto straight = parse_state(*blocks, "minecraft:rail", {{"shape", "north_south"}});
    CHECK(value_of(*blocks,
                   transform_state(*blocks, straight, Mirror::None, Rotation::CounterClockwise90),
                   "shape") == "east_west");

    const auto fence        = parse_state(*blocks, "minecraft:oak_fence", {{"north", "true"}});
    const auto fence_turned = transform_state(*blocks, fence, Mirror::None, Rotation::Clockwise90);
    CHECK(value_of(*blocks, fence_turned, "north") == "false");
    CHECK(value_of(*blocks, fence_turned, "east") == "true");

    // A slab's `type` is not a chest's: top stays top under a mirror.
    const auto slab = parse_state(*blocks, "minecraft:oak_slab", {{"type", "top"}});
    CHECK(value_of(*blocks, transform_state(*blocks, slab, Mirror::FrontBack, Rotation::None),
                   "type") == "top");
    const auto chest = parse_state(*blocks, "minecraft:chest", {{"type", "left"}});
    CHECK(value_of(*blocks, transform_state(*blocks, chest, Mirror::FrontBack, Rotation::None),
                   "type") == "right");
}

TEST_CASE("stairs and fences take their shape from their neighbours", "[structure][template]") {
    if (!have_registry()) {
        SKIP("registry pack absent; run tools/ov_datagen/ovpack.py");
    }
    auto blocks = registry::BlockRegistry::load(registry_pack());
    REQUIRE(blocks.has_value());

    MapLevel level;
    // A stair facing north with, behind it (to the north), a stair facing
    // east: an outer corner, turning to the right.
    const auto north =
        parse_state(*blocks, "minecraft:oak_stairs", {{"facing", "north"}, {"shape", "straight"}});
    const auto east =
        parse_state(*blocks, "minecraft:oak_stairs", {{"facing", "east"}, {"shape", "straight"}});
    level.set_block(0, 0, 0, north);
    level.set_block(0, 0, -1, east);
    CHECK(value_of(*blocks, stair_shape(*blocks, level, {0, 0, 0}, north), "shape") ==
          "outer_right");
    // Alone, it is straight whatever it was saved as.
    MapLevel   alone;
    const auto saved = with_value(*blocks, north, "shape", "outer_right");
    CHECK(value_of(*blocks, stair_shape(*blocks, alone, {0, 0, 0}, saved), "shape") == "straight");

    // A fence joins another fence and a stone block, not the air.
    MapLevel   yard;
    const auto fence = blocks->default_state(*blocks->find_block("minecraft:oak_fence"));
    yard.set_block(0, 0, 0, fence);
    yard.set_block(1, 0, 0, fence);
    yard.set_block(0, 0, -1, blocks->default_state(*blocks->find_block("minecraft:stone")));
    const auto joined = fence_connections(*blocks, yard, {0, 0, 0}, fence);
    CHECK(value_of(*blocks, joined, "east") == "true");
    CHECK(value_of(*blocks, joined, "north") == "true");
    CHECK(value_of(*blocks, joined, "south") == "false");
    CHECK(value_of(*blocks, joined, "west") == "false");
}

TEST_CASE("pieces from the seed are the game's pieces", "[structure][pieces]") {
    if (!have_everything()) {
        SKIP("vanilla data, registry pack or tools/vanilla/server.jar absent");
    }
    auto blocks = registry::BlockRegistry::load(registry_pack());
    REQUIRE(blocks.has_value());
    auto tags = BlockTags::load(data_root(), *blocks);
    REQUIRE(tags.has_value());
    auto sets = StructureSetRegistry::load(data_root());
    REQUIRE(sets.has_value());
    auto placer = StructurePlacer::load(data_root(), *sets);
    REQUIRE(placer.has_value());
    std::string detail;
    auto builder = StructureBuilder::load(server_jar(), data_root(), *blocks, *tags, &detail);
    REQUIRE(builder.has_value());

    const auto generate = [&](std::string_view name, i32 x, i32 z) {
        const StructureDefinition* definition = placer->find(name);
        REQUIRE(definition != nullptr);
        auto start = builder->generate(*definition, kSeed, x, z, nullptr);
        REQUIRE(start.has_value());
        return *start;
    };

    // Igloo, chunk (-90, 23) of run/struct-locate-1234567890: turned 180°, a
    // basement four segments deep.
    const auto igloo = generate("minecraft:igloo", -90, 23);
    REQUIRE(igloo.pieces.size() == 6);
    CHECK(igloo.pieces.front().template_name == "minecraft:igloo/bottom");
    CHECK(igloo.pieces.front().origin == BlockPos{-1440, 72, 366});
    CHECK(igloo.pieces[1].origin == BlockPos{-1438, 87, 372});
    CHECK(igloo.pieces[4].origin == BlockPos{-1438, 78, 372});
    CHECK(igloo.pieces.back().template_name == "minecraft:igloo/top");
    CHECK(igloo.pieces.back().origin == BlockPos{-1440, 90, 368});
    for (const auto& piece : igloo.pieces) {
        CHECK(piece.rotation == Rotation::Clockwise180);
    }

    // Shipwreck, chunk (-6560, 442) of run/reference-1234567890.
    const auto ship = generate("minecraft:shipwreck", -6560, 442);
    REQUIRE(ship.pieces.size() == 1);
    CHECK(ship.pieces.front().template_name == "minecraft:shipwreck/upsidedown_backhalf");
    CHECK(ship.pieces.front().rotation == Rotation::CounterClockwise90);

    // Ruined portal, chunk (4571, 3940).
    const auto portal = generate("minecraft:ruined_portal", 4571, 3940);
    REQUIRE(portal.pieces.size() == 1);
    CHECK(portal.pieces.front().template_name == "minecraft:ruined_portal/portal_4");
    CHECK(portal.pieces.front().rotation == Rotation::CounterClockwise90);
    CHECK(portal.pieces.front().mirror == Mirror::FrontBack);
    CHECK(portal.pieces.front().portal.placement == "on_land_surface");
    CHECK_FALSE(portal.pieces.front().portal.air_pocket);

    // Large cold ocean ruin, chunk (3249, -4250): three layers, and the
    // cluster around it is named as missing rather than silently absent.
    const auto ruin = generate("minecraft:ocean_ruin_cold", 3249, -4250);
    REQUIRE(ruin.pieces.size() == 3);
    CHECK(ruin.pieces[0].template_name == "minecraft:underwater_ruin/big_brick_8");
    CHECK(ruin.pieces[1].template_name == "minecraft:underwater_ruin/big_cracked_8");
    CHECK(ruin.pieces[2].template_name == "minecraft:underwater_ruin/big_mossy_8");
    CHECK(ruin.pieces[0].rotation == Rotation::Clockwise180);
    CHECK(ruin.pieces[0].integrity == 0.9F);
    CHECK_FALSE(ruin.incomplete.empty());

    // The loot-seed index: a structure's rank by name within its step.
    CHECK(structure_step_index(*placer, "minecraft:igloo") == 3);
    CHECK(structure_step_index(*placer, "minecraft:shipwreck") == 17);
    CHECK(structure_step_index(*placer, "minecraft:buried_treasure") == 0);

    // What is not built is refused by name.
    const StructureDefinition* pyramid = placer->find("minecraft:desert_pyramid");
    REQUIRE(pyramid != nullptr);
    const auto refused = builder->generate(*pyramid, kSeed, 0, 0, nullptr);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().find("desert_pyramid") != std::string::npos);
}

TEST_CASE("a piece written chunk by chunk is the piece written at once", "[structure][stage]") {
    if (!have_everything()) {
        SKIP("vanilla data, registry pack or tools/vanilla/server.jar absent");
    }
    auto blocks = registry::BlockRegistry::load(registry_pack());
    REQUIRE(blocks.has_value());
    auto tags = BlockTags::load(data_root(), *blocks);
    REQUIRE(tags.has_value());
    auto sets = StructureSetRegistry::load(data_root());
    REQUIRE(sets.has_value());
    auto placer = StructurePlacer::load(data_root(), *sets);
    REQUIRE(placer.has_value());
    auto builder = StructureBuilder::load(server_jar(), data_root(), *blocks, *tags);
    REQUIRE(builder.has_value());

    // The game's own ship of chunk (-6560, 442), height already settled at 43:
    // its box crosses four chunks.
    StructureStart start;
    start.structure = "minecraft:shipwreck";
    start.chunk_x   = -6560;
    start.chunk_z   = 442;
    StructurePiece piece;
    piece.kind                   = PieceKind::Shipwreck;
    piece.template_name          = "minecraft:shipwreck/upsidedown_backhalf";
    piece.origin                 = {-104960, 43, 7072};
    piece.rotation               = Rotation::CounterClockwise90;
    piece.pivot                  = {4, 0, 15};
    piece.height_settled         = true;
    const StructureTemplate* tpl = builder->templates().find(piece.template_name);
    REQUIRE(tpl != nullptr);
    piece.box = tpl->bounding_box(piece.origin, piece.mirror, piece.rotation, piece.pivot);
    start.pieces.push_back(piece);
    start.box = piece.box;

    // All at once, into a map.
    MapLevel      whole;
    FeatureRandom unused{FeatureRandom::Kind::Xoroshiro, 0};
    (void)builder->place(whole, piece, BoundingBox{-2000000, -64, -2000000, 2000000, 319, 2000000},
                         unused);

    // Chunk by chunk through the stage, in two different orders.
    const auto run = [&](bool reversed) {
        std::map<std::pair<i32, i32>, std::unique_ptr<world::Chunk>> chunks;
        const auto                                                   chunk = [&](i32 x, i32 z) {
            auto& slot = chunks[{x, z}];
            if (!slot) {
                slot =
                    std::make_unique<world::Chunk>(ChunkPos{x, z}, world::WorldShape::overworld(),
                                                   world::AirStates::from(*blocks), &*blocks);
            }
            return slot.get();
        };
        StructureStage stage{*placer, *builder, nullptr, *blocks, nullptr, kSeed};
        stage.add_start(start);
        std::vector<std::pair<i32, i32>> order{
            {-6561, 442}, {-6560, 442}, {-6561, 443}, {-6560, 443}};
        if (reversed) {
            std::reverse(order.begin(), order.end());
        }
        for (const auto& [cx, cz] : order) {
            std::array<world::Chunk*, 9> around{};
            for (i32 dz = -1; dz <= 1; ++dz) {
                for (i32 dx = -1; dx <= 1; ++dx) {
                    around[static_cast<usize>((dz + 1) * 3 + (dx + 1))] = chunk(cx + dx, cz + dz);
                }
            }
            stage.place(around, cx, cz, 63);
        }
        std::map<std::tuple<i32, i32, i32>, registry::BlockStateId> out;
        for (const auto& [position, owned] : chunks) {
            for (i32 y = 40; y < 56; ++y) {
                for (usize z = 0; z < 16; ++z) {
                    for (usize x = 0; x < 16; ++x) {
                        const auto state = owned->get_block(x, y, z);
                        if (state != registry::kAirState) {
                            out[{position.first * 16 + static_cast<i32>(x), y,
                                 position.second * 16 + static_cast<i32>(z)}] = state;
                        }
                    }
                }
            }
        }
        return out;
    };
    const auto forward  = run(false);
    const auto backward = run(true);

    // The order of the chunks does not matter …
    CHECK(forward == backward);
    // … and every block the whole piece wrote is there, the same, in some
    // chunk: nothing was lost at a border.
    usize       same  = 0;
    usize       solid = 0;
    std::string differences;
    for (const auto& [position, state] : whole.blocks()) {
        if (state == registry::kAirState) {
            continue;
        }
        ++solid;
        const auto it = forward.find(position);
        if (it != forward.end() && it->second == state) {
            ++same;
        } else {
            differences +=
                fmt_position(position) + " whole " +
                std::string{blocks->block_name(blocks->block_of(state))} +
                (it == forward.end()
                     ? std::string{" chunked <none>"}
                     : " chunked " +
                           std::string{blocks->block_name(blocks->block_of(it->second))}) +
                "; ";
        }
    }
    INFO(differences);
    CHECK(solid > 300);
    CHECK(same == solid);
}
