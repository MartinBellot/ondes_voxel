// The `structures` compound of a chunk, against what the game wrote in the
// reference worlds (docs/provenance/structures.md § 20).
#include "ov/registry/block_states.hpp"
#include "ov/world/chunk.hpp"
#include "ov/world/chunk_storage.hpp"
#include "ov/worldgen/placement.hpp"
#include "ov/worldgen/structure.hpp"
#include "ov/worldgen/structure_nbt.hpp"
#include "ov/worldgen/structure_pieces.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

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

[[nodiscard]] std::vector<i32> int_array(const nbt::Tag& tag, std::string_view key) {
    const nbt::Tag* found = tag.find(key);
    const auto*     array = found != nullptr ? found->get_if<nbt::Tag::IntArray>() : nullptr;
    return array != nullptr ? *array : std::vector<i32>{};
}

/// The beached ship of `reference-1234567890`, chunk (9, 5), as the game
/// stored it once the chunk was full.
[[nodiscard]] StructurePiece beached_ship() {
    StructurePiece piece;
    piece.kind             = PieceKind::Shipwreck;
    piece.template_name    = "minecraft:shipwreck/rightsideup_backhalf_degraded";
    piece.origin           = {144, 58, 80};
    piece.generated_origin = {144, 90, 80};
    piece.rotation         = Rotation::None;
    piece.box              = {144, 58, 80, 152, 66, 95};
    piece.beached          = true;
    piece.height_settled   = true;
    return piece;
}

}  // namespace

TEST_CASE("chunk positions are packed as the game packs them", "[structures][nbt]") {
    // `References` of reference-1234567890, pointing at the mineshaft whose
    // start chunk is (-7, -10).
    STATIC_CHECK(packed_chunk_pos(-7, -10) == -38654705671);
    STATIC_CHECK(packed_chunk_pos(9, 5) == 21474836489);
    STATIC_CHECK(packed_chunk_pos(0, 0) == 0);
}

TEST_CASE("a piece carries the game's names and tag types", "[structures][nbt]") {
    const nbt::Tag ship = piece_to_nbt(beached_ship());
    CHECK(ship.find("id")->as_string() == "minecraft:shipwreck");
    CHECK(int_array(ship, "BB") == std::vector<i32>{144, 58, 80, 152, 66, 95});
    REQUIRE(ship.find("isBeached") != nullptr);
    CHECK(ship.find("isBeached")->get_if<i8>() != nullptr);
    CHECK(ship.find("TPY")->get_if<i32>() != nullptr);
    CHECK(ship.find("TPY")->as_i64() == 58);
    CHECK(ship.find("GD")->as_i64() == 0);
    CHECK(ship.find("O")->as_i64() == 2);
    CHECK(ship.find("Rot")->as_string() == "NONE");
    CHECK(ship.find("Mirror") == nullptr);

    // The igloo keeps its generated template height (bottom `TPY` 54, box 35).
    StructurePiece igloo   = beached_ship();
    igloo.kind             = PieceKind::Igloo;
    igloo.template_name    = "minecraft:igloo/bottom";
    igloo.origin           = {61040, 35, 33134};
    igloo.generated_origin = {61040, 54, 33134};
    CHECK(piece_to_nbt(igloo).find("TPY")->as_i64() == 54);
    CHECK(piece_to_nbt(igloo).find("isBeached") == nullptr);

    StructurePiece portal          = beached_ship();
    portal.kind                    = PieceKind::RuinedPortal;
    portal.mirror                  = Mirror::FrontBack;
    portal.portal.placement        = "on_land_surface";
    portal.portal.mossiness        = 0.2F;
    const nbt::Tag portal_tag      = piece_to_nbt(portal);
    CHECK(portal_tag.find("Rotation")->as_string() == "NONE");
    CHECK(portal_tag.find("Rot") == nullptr);
    CHECK(portal_tag.find("Mirror")->as_string() == "FRONT_BACK");
    REQUIRE(portal_tag.find("Properties") != nullptr);
    CHECK(portal_tag.find("Properties")->find("mossiness")->get_if<f32>() != nullptr);
    CHECK(portal_tag.find("Properties")->find("air_pocket")->get_if<i8>() != nullptr);

    StructurePiece treasure = beached_ship();
    treasure.kind           = PieceKind::BuriedTreasure;
    const nbt::Tag chest    = piece_to_nbt(treasure);
    CHECK(chest.find("id")->as_string() == "minecraft:btp");
    CHECK(chest.find("O")->as_i64() == -1);
    CHECK(chest.find("TPX") == nullptr);
}

TEST_CASE("a chunk's structures: starts by name, references once each", "[structures][nbt]") {
    StructureStart start;
    start.structure = "minecraft:shipwreck_beached";
    start.chunk_x   = 9;
    start.chunk_z   = 5;
    start.pieces.push_back(beached_ship());
    StructureStart empty;
    empty.structure = "minecraft:igloo";

    const std::vector<StructureStart>     starts{start, empty};
    const std::vector<StructureReference> references{
        {"minecraft:shipwreck_beached", 9, 5}, {"minecraft:shipwreck_beached", 9, 5}};
    const nbt::Tag structures = chunk_structures_to_nbt(starts, references);

    const nbt::Tag* stored = structures.find("starts");
    REQUIRE(stored != nullptr);
    CHECK(stored->find("minecraft:igloo") == nullptr);
    const nbt::Tag* ship = stored->find("minecraft:shipwreck_beached");
    REQUIRE(ship != nullptr);
    CHECK(ship->find("id")->as_string() == "minecraft:shipwreck_beached");
    CHECK(ship->find("ChunkX")->as_i64() == 9);
    CHECK(ship->find("ChunkZ")->as_i64() == 5);
    CHECK(ship->find("references")->as_i64() == 0);
    REQUIRE(ship->find("Children")->list() != nullptr);
    CHECK(ship->find("Children")->list()->size() == 1);

    const nbt::Tag* refs = structures.find("References")->find("minecraft:shipwreck_beached");
    REQUIRE(refs != nullptr);
    const auto* longs = refs->get_if<nbt::Tag::LongArray>();
    REQUIRE(longs != nullptr);
    CHECK(*longs == std::vector<i64>{21474836489});
}

TEST_CASE("the builder reads back every piece it writes", "[structures][nbt]") {
    if (!std::filesystem::is_regular_file(registry_pack()) ||
        !std::filesystem::is_directory(data_root() / "worldgen") ||
        !std::filesystem::exists(server_jar())) {
        SKIP("no registry pack, generated data or server jar");
    }
    auto blocks = registry::BlockRegistry::load(registry_pack());
    REQUIRE(blocks);
    auto tags = BlockTags::load(data_root(), *blocks);
    REQUIRE(tags);
    auto builder = StructureBuilder::load(server_jar(), data_root(), *blocks, *tags);
    REQUIRE(builder);

    struct Case {
        std::string_view name;
        StructureKind    kind;
        i32              chunk_x;
        i32              chunk_z;
    };
    for (const Case& c : {Case{"minecraft:igloo", StructureKind::Igloo, 3815, 2071},
                          Case{"minecraft:shipwreck_beached", StructureKind::Shipwreck, 9, 5},
                          Case{"minecraft:ocean_ruin_cold", StructureKind::OceanRuin, 3249, -4250},
                          Case{"minecraft:buried_treasure", StructureKind::BuriedTreasure, 4571,
                               3937}}) {
        StructureDefinition definition;
        definition.name = std::string{c.name};
        definition.kind = c.kind;
        auto start      = builder->generate(definition, 1234567890, c.chunk_x, c.chunk_z, nullptr);
        REQUIRE(start);
        REQUIRE(!start->pieces.empty());
        for (const StructurePiece& piece : start->pieces) {
            auto back = builder->piece_from_nbt(piece_to_nbt(piece));
            REQUIRE(back);
            CHECK(back->kind == piece.kind);
            CHECK(back->template_name == piece.template_name);
            CHECK(back->origin == piece.origin);
            CHECK(back->rotation == piece.rotation);
            CHECK(back->mirror == piece.mirror);
            CHECK(back->box.min_y == piece.box.min_y);
            CHECK(back->box.max_x == piece.box.max_x);
            CHECK(back->integrity == piece.integrity);
            CHECK(back->beached == piece.beached);
            CHECK(back->large == piece.large);
            CHECK(back->warm == piece.warm);
        }
    }
}

TEST_CASE("a chunk keeps its structures through the disk", "[structures][nbt]") {
    if (!std::filesystem::is_regular_file(registry_pack())) {
        SKIP("no registry pack");
    }
    auto blocks = registry::BlockRegistry::load(registry_pack());
    REQUIRE(blocks);
    const auto   air = world::AirStates::from(*blocks);
    world::Chunk chunk{ChunkPos{9, 5}, world::WorldShape::overworld(), air, &*blocks};

    StructureStart start;
    start.structure = "minecraft:shipwreck_beached";
    start.chunk_x   = 9;
    start.chunk_z   = 5;
    start.pieces.push_back(beached_ship());
    const std::vector<StructureStart>     starts{start};
    const std::vector<StructureReference> references{{"minecraft:shipwreck_beached", 9, 5}};
    chunk.set_structures(chunk_structures_to_nbt(starts, references));

    std::vector<std::string_view> names;
    for (u32 index = 0; index < blocks->biome_count(); ++index) {
        names.push_back(blocks->biome_name(index));
    }
    world::ChunkCodecContext context;
    context.blocks      = &*blocks;
    context.biome_names = names;
    context.air         = air;

    const nbt::Document document = world::to_nbt(chunk, context);
    REQUIRE(document.root.find("structures") != nullptr);
    CHECK(*document.root.find("structures") == chunk.structures());
    const auto back = world::from_nbt(document, context);
    REQUIRE(back);
    CHECK(back->structures() == chunk.structures());

    // A chunk with none still writes both lists, as vanilla does.
    const world::Chunk bare{ChunkPos{0, 0}, world::WorldShape::overworld(), air, &*blocks};
    const nbt::Document bare_document = world::to_nbt(bare, context);
    const nbt::Tag*     structures    = bare_document.root.find("structures");
    REQUIRE(structures != nullptr);
    CHECK(structures->find("starts") != nullptr);
    CHECK(structures->find("References") != nullptr);
}
