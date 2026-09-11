// What the server writes into a finished chunk about the structures around it
// (docs/provenance/structures.md § 20.3): the starts of its own start chunk,
// and a reference from every chunk a start's box crosses — and only those.
#include "../src/world_structures.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/chunk.hpp"
#include "ov/worldgen/biome_source.hpp"
#include "ov/worldgen/chunk_generator.hpp"
#include "ov/worldgen/density.hpp"
#include "ov/worldgen/structure_nbt.hpp"
#include "ov/worldgen/structure_pieces.hpp"
#include "ov/worldgen/structure_stage.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <vector>

using namespace ov;

namespace {

[[nodiscard]] std::filesystem::path data_dir() { return std::filesystem::path{OV_SOURCE_DIR} / "data"; }

[[nodiscard]] std::filesystem::path generated() {
    return data_dir() / "vanilla" / "1.20.1" / "generated";
}

[[nodiscard]] std::filesystem::path pack() {
    return data_dir() / "vanilla" / "1.20.1" / "registry.ovpack";
}

[[nodiscard]] std::vector<i64> references(const world::Chunk& chunk, std::string_view name) {
    const nbt::Tag* refs = chunk.structures().find("References");
    const nbt::Tag* list = refs != nullptr ? refs->find(name) : nullptr;
    const auto*     longs = list != nullptr ? list->get_if<nbt::Tag::LongArray>() : nullptr;
    return longs != nullptr ? *longs : std::vector<i64>{};
}

}  // namespace

TEST_CASE("a chunk records its starts and the starts crossing it", "[structures][server]") {
    const auto jar = server::WorldStructures::default_jar(data_dir());
    if (!std::filesystem::is_regular_file(pack()) ||
        !std::filesystem::is_directory(generated() / "data" / "minecraft" / "worldgen") ||
        !std::filesystem::exists(jar)) {
        SKIP("no registry pack, generated data or server jar");
    }
    auto blocks     = registry::BlockRegistry::load(pack());
    auto registries = registry::Registries::load(pack());
    REQUIRE(blocks);
    REQUIRE(registries);
    constexpr i64 kSeed  = 1234567890;
    const auto    data   = generated() / "data" / "minecraft";
    auto          router = worldgen::NoiseRouter::load(data, "overworld", kSeed);
    auto          source = worldgen::BiomeSource::load(generated(), "overworld");
    REQUIRE(router);
    REQUIRE(source);
    const worldgen::ChunkGenerator generator{*router, *source, *blocks};

    auto structures = server::WorldStructures::load(data, jar, *blocks, source->biomes());
    REQUIRE(structures);
    auto stack = structures->make_stage(generator, *blocks, *registries, kSeed);
    REQUIRE(stack);

    // A ship whose box crosses from chunk (9, 5) into (10, 5), put in by hand
    // so that nothing here depends on terrain.
    worldgen::StructureStart start;
    start.structure = "minecraft:shipwreck";
    start.chunk_x   = 9;
    start.chunk_z   = 5;
    worldgen::StructurePiece piece;
    piece.kind             = worldgen::PieceKind::Shipwreck;
    piece.template_name    = "minecraft:shipwreck/sideways_full";
    piece.origin           = {150, 60, 80};
    piece.generated_origin = {150, 90, 80};
    piece.box              = {150, 60, 80, 170, 68, 88};
    piece.height_settled   = true;
    start.pieces.push_back(piece);
    start.box = piece.box;
    stack->stage->add_start(start);

    const auto air = world::AirStates::from(*blocks);
    world::Chunk home{ChunkPos{9, 5}, world::WorldShape::overworld(), air, &*blocks};
    world::Chunk crossed{ChunkPos{10, 5}, world::WorldShape::overworld(), air, &*blocks};
    world::Chunk beside{ChunkPos{9, 7}, world::WorldShape::overworld(), air, &*blocks};
    stack->record(home);
    stack->record(crossed);
    stack->record(beside);

    const nbt::Tag* starts = home.structures().find("starts");
    REQUIRE(starts != nullptr);
    const nbt::Tag* ship = starts->find("minecraft:shipwreck");
    REQUIRE(ship != nullptr);
    CHECK(ship->find("ChunkX")->as_i64() == 9);
    CHECK(ship->find("Children")->list()->size() == 1);

    const i64 packed = worldgen::packed_chunk_pos(9, 5);
    CHECK(references(home, "minecraft:shipwreck") == std::vector<i64>{packed});
    CHECK(references(crossed, "minecraft:shipwreck") == std::vector<i64>{packed});
    // The crossed chunk is not the start chunk: a reference, no start.
    const nbt::Tag* crossed_starts = crossed.structures().find("starts");
    REQUIRE(crossed_starts != nullptr);
    CHECK(crossed_starts->find("minecraft:shipwreck") == nullptr);
    // Two chunks away in z the box does not reach.
    CHECK(references(beside, "minecraft:shipwreck").empty());
}

TEST_CASE("a start with terrain adaptation is referenced 12 blocks wider", "[structures][server]") {
    const auto jar = server::WorldStructures::default_jar(data_dir());
    if (!std::filesystem::is_regular_file(pack()) || !std::filesystem::exists(jar)) {
        SKIP("no registry pack or server jar");
    }
    auto blocks     = registry::BlockRegistry::load(pack());
    auto registries = registry::Registries::load(pack());
    REQUIRE(blocks);
    REQUIRE(registries);
    constexpr i64 kSeed  = 987654321;
    const auto    data   = generated() / "data" / "minecraft";
    auto          router = worldgen::NoiseRouter::load(data, "overworld", kSeed);
    auto          source = worldgen::BiomeSource::load(generated(), "overworld");
    REQUIRE(router);
    REQUIRE(source);
    const worldgen::ChunkGenerator generator{*router, *source, *blocks};
    auto structures = server::WorldStructures::load(data, jar, *blocks, source->biomes());
    REQUIRE(structures);
    auto stack = structures->make_stage(generator, *blocks, *registries, kSeed);
    REQUIRE(stack);
    REQUIRE(stack->placer != nullptr);

    // Read from the pack: the fossil adapts the terrain, the ship does not.
    REQUIRE(stack->placer->find("minecraft:nether_fossil") != nullptr);
    CHECK(stack->placer->find("minecraft:nether_fossil")->terrain_adaptation);
    CHECK_FALSE(stack->placer->find("minecraft:shipwreck")->terrain_adaptation);

    // A fossil four blocks wide, well inside chunk (9, 5): the chunk east of
    // it is 5 blocks away — inside the margin — and the one after, 21.
    worldgen::StructureStart start;
    start.structure = "minecraft:nether_fossil";
    start.chunk_x   = 9;
    start.chunk_z   = 5;
    worldgen::StructurePiece piece;
    piece.kind             = worldgen::PieceKind::NetherFossil;
    piece.template_name    = "minecraft:nether_fossils/fossil_3";
    piece.origin           = {150, 50, 84};
    piece.generated_origin = piece.origin;
    piece.box              = {150, 50, 84, 154, 53, 86};
    piece.height_settled   = true;
    start.pieces.push_back(piece);
    start.box = piece.box;
    stack->stage->add_start(start);

    const auto   air = world::AirStates::from(*blocks);
    world::Chunk near{ChunkPos{10, 5}, world::WorldShape::overworld(), air, &*blocks};
    world::Chunk far{ChunkPos{11, 5}, world::WorldShape::overworld(), air, &*blocks};
    stack->record(near);
    stack->record(far);
    CHECK(references(near, "minecraft:nether_fossil") ==
          std::vector<i64>{worldgen::packed_chunk_pos(9, 5)});
    CHECK(references(far, "minecraft:nether_fossil").empty());
}

TEST_CASE("the server refuses the portals and the treasure by name", "[structures][server]") {
    const auto jar = server::WorldStructures::default_jar(data_dir());
    if (!std::filesystem::is_regular_file(pack()) || !std::filesystem::exists(jar)) {
        SKIP("no registry pack or server jar");
    }
    auto blocks     = registry::BlockRegistry::load(pack());
    auto registries = registry::Registries::load(pack());
    REQUIRE(blocks);
    REQUIRE(registries);
    constexpr i64 kSeed  = 1234567890;
    const auto    data   = generated() / "data" / "minecraft";
    auto          router = worldgen::NoiseRouter::load(data, "overworld", kSeed);
    auto          source = worldgen::BiomeSource::load(generated(), "overworld");
    REQUIRE(router);
    REQUIRE(source);
    const worldgen::ChunkGenerator generator{*router, *source, *blocks};
    auto structures = server::WorldStructures::load(data, jar, *blocks, source->biomes());
    REQUIRE(structures);
    auto stack = structures->make_stage(generator, *blocks, *registries, kSeed);
    REQUIRE(stack);

    // The game's buried treasure of reference-1234567890 starts in chunk
    // (4571, 3937): the placer says yes, the server refuses it and says why.
    const auto& starts = stack->stage->starts_at(4571, 3937);
    for (const auto& start : starts) {
        CHECK(start.structure != "minecraft:buried_treasure");
    }
    bool named = false;
    for (const auto& [reason, count] : stack->stage->stats().refused) {
        if (reason.starts_with("minecraft:buried_treasure: buried treasure:")) {
            named = count > 0;
        }
    }
    CHECK(named);
}
