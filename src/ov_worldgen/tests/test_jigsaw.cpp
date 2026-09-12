// The jigsaw assembler against pieces the game stored (docs/provenance/jigsaw.md).
//
// The bastion is the test case because it needs no terrain: rigid pieces, a
// constant start height, no projection — the whole start follows from the seed
// and the pools. The numbers are the ones `reference-nether-987654321` stores
// for the bastion whose start chunk is (54, 440).
#include "ov/registry/block_states.hpp"
#include "ov/worldgen/jigsaw.hpp"
#include "ov/worldgen/placement.hpp"
#include "ov/worldgen/structure.hpp"
#include "ov/worldgen/structure_nbt.hpp"
#include "ov/worldgen/structure_pieces.hpp"
#include "ov/worldgen/structure_set.hpp"
#include "ov/worldgen/structure_stage.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>
#include <string>

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

struct Loaded {
    std::optional<registry::BlockRegistry> blocks;
    std::optional<BlockTags>               tags;
    std::optional<StructureBuilder>        builder;
};

/// Loaded once for the file: the templates are read out of the jar.
[[nodiscard]] Loaded& loaded() {
    static Loaded state = [] {
        Loaded out;
        auto   blocks = registry::BlockRegistry::load(registry_pack());
        if (!blocks) {
            return out;
        }
        out.blocks.emplace(std::move(*blocks));
        auto tags = BlockTags::load(data_root(), *out.blocks);
        if (!tags) {
            return out;
        }
        out.tags.emplace(std::move(*tags));
        auto builder = StructureBuilder::load(server_jar(), data_root(), *out.blocks, *out.tags);
        if (builder) {
            out.builder.emplace(std::move(*builder));
        }
        return out;
    }();
    return state;
}

[[nodiscard]] bool available() {
    return std::filesystem::exists(server_jar()) && std::filesystem::exists(registry_pack()) &&
           std::filesystem::is_directory(data_root() / "worldgen" / "template_pool");
}

}  // namespace

TEST_CASE("a direction turns with its structure", "[jigsaw]") {
    CHECK(rotate(Direction::North, Rotation::Clockwise90) == Direction::East);
    CHECK(rotate(Direction::West, Rotation::Clockwise90) == Direction::North);
    CHECK(rotate(Direction::North, Rotation::Clockwise180) == Direction::South);
    CHECK(rotate(Direction::North, Rotation::CounterClockwise90) == Direction::West);
    CHECK(rotate(Direction::Up, Rotation::Clockwise90) == Direction::Up);
    CHECK(rotate(Direction::Down, Rotation::CounterClockwise90) == Direction::Down);
}

TEST_CASE("the pools and the jigsaw structures of 1.20.1 load", "[jigsaw]") {
    if (!available()) {
        SKIP("generated data or server jar missing");
    }
    REQUIRE(loaded().builder.has_value());
    const JigsawLibrary* library = loaded().builder->jigsaw();
    REQUIRE(library != nullptr);
    CHECK(library->pool_count() == 141);
    // One template a pool names and the jar lacks; the game places it empty.
    REQUIRE(library->missing_templates().size() == 1);
    CHECK(library->missing_templates().front() ==
          "minecraft:ancient_city/walls/intact_horizontal_wall_stairs_5");

    const JigsawConfig* plains = library->config("minecraft:village_plains");
    REQUIRE(plains != nullptr);
    CHECK(plains->start_pool == "minecraft:village/plains/town_centers");
    CHECK(plains->size == 6);
    CHECK(plains->max_distance == 80);
    CHECK(plains->expansion_hack);
    CHECK(plains->project_to == world::HeightmapType::WorldSurfaceWG);
    const JigsawConfig* city = library->config("minecraft:ancient_city");
    REQUIRE(city != nullptr);
    CHECK(city->start_jigsaw_name == "minecraft:city_anchor");
    CHECK(city->start_height == -27);
    CHECK_FALSE(city->project_to.has_value());
    CHECK(library->config("minecraft:igloo") == nullptr);

    // 1.20.1's jigsaw blocks carry name, target, pool, final state and joint;
    // the selection and placement priorities came later and are absent.
    const TemplatePool* starts = library->pool("minecraft:bastion/starts");
    REQUIRE(starts != nullptr);
    CHECK(starts->weighted.size() == 4);
    CHECK(library->pool("minecraft:empty") != nullptr);
}

TEST_CASE("a bastion grows from the seed into the game's pieces", "[jigsaw]") {
    if (!available()) {
        SKIP("generated data or server jar missing");
    }
    REQUIRE(loaded().builder.has_value());
    const JigsawLibrary& library = *loaded().builder->jigsaw();
    const JigsawConfig*  config  = library.config("minecraft:bastion_remnant");
    REQUIRE(config != nullptr);

    auto start = library.assemble(*config, 987654321, 54, 440, nullptr);
    REQUIRE(start.has_value());
    REQUIRE(start->pieces.size() == 53);

    const StructurePiece& first = start->pieces.front();
    REQUIRE(first.element != nullptr);
    CHECK(first.element->location == "minecraft:bastion/bridge/starting_pieces/entrance_base");
    CHECK(first.origin == BlockPos{864, 32, 7040});  // start height 33, lowered by its delta
    CHECK(first.rotation == Rotation::None);
    CHECK(first.box.min_x == 864);
    CHECK(first.box.max_y == 63);
    CHECK(first.box.max_z == 7071);
    REQUIRE(first.junctions.size() >= 2);
    CHECK(first.junctions[0].source_x == 866);
    CHECK(first.junctions[0].source_ground_y == 33);
    CHECK(first.junctions[0].source_z == 7059);
    CHECK(first.junctions[0].delta_y == 25);

    const StructurePiece& second = start->pieces[1];
    CHECK(second.element->location == "minecraft:bastion/mobs/sword_piglin");
    CHECK(second.origin == BlockPos{866, 57, 7059});
    CHECK(second.rotation == Rotation::CounterClockwise90);
    CHECK(second.ground_level_delta == -24);

    const StructurePiece& last = start->pieces.back();
    CHECK(last.element->location == "minecraft:bastion/mobs/melee_piglin");
    CHECK(last.origin == BlockPos{873, 73, 7078});
    CHECK(last.ground_level_delta == -40);

    // The witness: one seed off, and the same chunk grows something else.
    auto other = library.assemble(*config, 987654322, 54, 440, nullptr);
    REQUIRE(other.has_value());
    const bool differs = other->pieces.size() != start->pieces.size() ||
                         other->pieces.front().element != first.element ||
                         other->pieces.front().rotation != first.rotation;
    CHECK(differs);
}

TEST_CASE("the ancient city's anchor is its named start jigsaw", "[jigsaw]") {
    if (!available()) {
        SKIP("generated data or server jar missing");
    }
    REQUIRE(loaded().builder.has_value());
    const JigsawLibrary& library = *loaded().builder->jigsaw();
    const JigsawConfig*  config  = library.config("minecraft:ancient_city");
    REQUIRE(config != nullptr);
    // struct-locate-1234567890 stores this city with 86 pieces; the start
    // piece at (-1172, -52, 1341), turned counter-clockwise.
    const auto point = library.start_point(*config, 1234567890, -72, 83, nullptr);
    REQUIRE(point.has_value());
    CHECK(point->position == BlockPos{-1172, -52, 1341});
    CHECK(point->rotation == Rotation::CounterClockwise90);
    CHECK(point->box.min_z == 1324);
    CHECK(point->box.max_x == -1132);
    // The biome is read at the start height, where the anchor jigsaw sits.
    CHECK(point->anchor.y == -27);
    CHECK(point->anchor.x == (-1172 + -1132) / 2);

    auto start = library.assemble(*config, 1234567890, -72, 83, nullptr);
    REQUIRE(start.has_value());
    CHECK(start->pieces.size() == 86);
}

namespace {

/// A Nether with no noise: every column in the Nether wastes, a flat floor.
/// Enough for a bastion, which reads neither heights nor terrain.
class FlatNether final : public StructureWorldSampler {
public:
    [[nodiscard]] std::string_view biome_at(i32, i32, i32) const override {
        return "minecraft:nether_wastes";
    }
    [[nodiscard]] i32 surface_height(i32, i32) const override { return 64; }
    [[nodiscard]] i32 ocean_floor_height(i32, i32) const override { return 64; }
};

}  // namespace

TEST_CASE("the stage scans each set within its own reach and keeps what cannot change",
          "[jigsaw][stage]") {
    if (!available()) {
        SKIP("generated data or server jar missing");
    }
    REQUIRE(loaded().builder.has_value());
    auto sets = StructureSetRegistry::load(data_root());
    REQUIRE(sets.has_value());
    auto placer = StructurePlacer::load(data_root(), *sets);
    REQUIRE(placer.has_value());
    placer->set_jigsaw(loaded().builder->jigsaw());
    const JigsawConfig* bastion = loaded().builder->jigsaw()->config("minecraft:bastion_remnant");
    REQUIRE(bastion != nullptr);
    CHECK(bastion->reach_chunks == 9);
    CHECK(loaded().builder->jigsaw()->config("minecraft:ancient_city")->reach_chunks == 12);
    CHECK(loaded().builder->jigsaw()->config("minecraft:village_plains")->reach_chunks == 7);

    FlatNether     nether;
    StructureStage stage{*placer, *loaded().builder, &nether, *loaded().blocks, nullptr, 987654321};

    // The first bastion the grid and the set's draw give, searched outward.
    const StructureStart* found = nullptr;
    for (i32 z = 0; z < 96 && found == nullptr; ++z) {
        for (i32 x = 0; x < 96 && found == nullptr; ++x) {
            for (const StructureStart& start : stage.starts_at(x, z)) {
                if (start.structure == "minecraft:bastion_remnant") {
                    found = &start;
                }
            }
        }
    }
    REQUIRE(found != nullptr);
    const i32 start_x = found->chunk_x;
    const i32 start_z = found->chunk_z;

    const auto holds = [&](i32 chunk_x, i32 chunk_z) -> const StructureStart* {
        for (StructureStart* start : stage.starts_reaching(chunk_x, chunk_z)) {
            if (start->structure == "minecraft:bastion_remnant" && start->chunk_x == start_x &&
                start->chunk_z == start_z) {
                return start;
            }
        }
        return nullptr;
    };
    // Nine chunks away it is still asked; ten, never — the set's reach.
    const StructureStart* near = holds(start_x + 9, start_z);
    REQUIRE(near != nullptr);
    CHECK(holds(start_x + 10, start_z) == nullptr);
    CHECK(holds(start_x, start_z - 9) == near);

    // A jigsaw start is settled when it is grown: `clear` keeps it, the very
    // object, and the next square does not grow it again.
    const usize pieces = near->pieces.size();
    stage.clear();
    const StructureStart* again = holds(start_x + 9, start_z);
    CHECK(again == near);
    REQUIRE(again != nullptr);
    CHECK(again->pieces.size() == pieces);
}

TEST_CASE("a jigsaw piece is stored as the game stores it, and read back", "[jigsaw][nbt]") {
    if (!available()) {
        SKIP("generated data or server jar missing");
    }
    REQUIRE(loaded().builder.has_value());
    const JigsawLibrary& library = *loaded().builder->jigsaw();
    auto start = library.assemble(*library.config("minecraft:bastion_remnant"), 987654321, 54, 440,
                                  nullptr);
    REQUIRE(start.has_value());
    for (const StructurePiece& piece : start->pieces) {
        const nbt::Tag stored = piece_to_nbt(piece);
        CHECK(stored.find("id")->as_string() == "minecraft:jigsaw");
        CHECK(stored.find("O")->get_if<i32>() != nullptr);
        CHECK(stored.find("O")->as_i64() == -1);
        CHECK(stored.find("rotation")->get_if<std::string>() != nullptr);
        CHECK(stored.find("junctions")->list() != nullptr);
        auto back = jigsaw_piece_from_nbt(library, stored);
        REQUIRE(back.has_value());
        CHECK(back->element == piece.element);
        CHECK(back->origin == piece.origin);
        CHECK(back->rotation == piece.rotation);
        CHECK(back->ground_level_delta == piece.ground_level_delta);
        CHECK(back->junctions.size() == piece.junctions.size());
    }
    // The inline processor list is an empty list of TAG_End, as the game has it.
    const nbt::Tag first = piece_to_nbt(start->pieces.front());
    const nbt::Tag* processors = first.find("pool_element")->find("processors");
    REQUIRE(processors != nullptr);
}
