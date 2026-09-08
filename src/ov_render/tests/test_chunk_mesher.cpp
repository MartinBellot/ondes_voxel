#include "ov/render/chunk_mesher.hpp"
#include "ov/world/chunk_section.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

using namespace ov;
using namespace ov::render;

namespace {

/// The registry pack is generated locally and never committed, so every test
/// that needs real block states has to be able to skip.
[[nodiscard]] std::optional<registry::BlockRegistry> try_load_registry() {
    for (const auto* candidate :
         {"data/vanilla/1.20.1/registry.ovpack", "../data/vanilla/1.20.1/registry.ovpack",
          "../../data/vanilla/1.20.1/registry.ovpack",
          "../../../data/vanilla/1.20.1/registry.ovpack"}) {
        if (!std::filesystem::exists(candidate)) {
            continue;
        }
        auto loaded = registry::BlockRegistry::load(candidate);
        if (loaded) {
            return std::move(*loaded);
        }
    }
    return std::nullopt;
}

[[nodiscard]] registry::BlockStateId default_state(const registry::BlockRegistry& blocks,
                                                   std::string_view               name) {
    const auto block = blocks.find_block(name);
    REQUIRE(block.has_value());
    return blocks.default_state(*block);
}

}  // namespace

TEST_CASE("the face enumerations agree between render and registry", "[chunk-mesher]") {
    // A silent disagreement here hides the ground under every slab, and both
    // enumerations claim to be the protocol's numbering — so it is worth
    // asserting rather than trusting.
    STATIC_REQUIRE(static_cast<u8>(Direction::Down) ==
                   static_cast<u8>(registry::BlockRegistry::Face::Down));
    STATIC_REQUIRE(static_cast<u8>(Direction::Up) ==
                   static_cast<u8>(registry::BlockRegistry::Face::Up));
    STATIC_REQUIRE(static_cast<u8>(Direction::North) ==
                   static_cast<u8>(registry::BlockRegistry::Face::North));
    STATIC_REQUIRE(static_cast<u8>(Direction::South) ==
                   static_cast<u8>(registry::BlockRegistry::Face::South));
    STATIC_REQUIRE(static_cast<u8>(Direction::West) ==
                   static_cast<u8>(registry::BlockRegistry::Face::West));
    STATIC_REQUIRE(static_cast<u8>(Direction::East) ==
                   static_cast<u8>(registry::BlockRegistry::Face::East));
}

TEST_CASE("occlusion asks the collision shape, not whether a block is solid", "[chunk-mesher]") {
    auto blocks = try_load_registry();
    if (!blocks) {
        SKIP("no registry pack");
    }

    // This is the whole reason the demo scene's bool was not good enough. A
    // bottom slab presents a full square downwards and nothing upwards, so the
    // ground under it stays hidden and the air above it does not.
    const auto slab = [&] {
        const auto block = blocks->find_block("minecraft:stone_slab");
        REQUIRE(block.has_value());
        const auto type = blocks->find_property(*block, "type");
        REQUIRE(type.has_value());
        auto state = blocks->default_state(*block);
        for (u16 index = 0; index < type->values.size(); ++index) {
            if (type->values[index] == "bottom") {
                state = blocks->with_property(state, *type, index);
            }
        }
        return state;
    }();

    CHECK(blocks->face_is_sturdy(slab, registry::BlockRegistry::Face::Down));
    CHECK_FALSE(blocks->face_is_sturdy(slab, registry::BlockRegistry::Face::Up));

    // A full cube is sturdy on all six.
    const auto stone = default_state(*blocks, "minecraft:stone");
    for (u8 face = 0; face < kDirectionCount; ++face) {
        CHECK(blocks->face_is_sturdy(stone, static_cast<registry::BlockRegistry::Face>(face)));
    }

    // Glass fills its cube, so it IS sturdy — this expectation was wrong the
    // first time it was written, and being wrong is what found the bug.
    // Sturdiness alone would hide everything behind a window, so occlusion also
    // asks whether the block stops light.
    const auto glass = default_state(*blocks, "minecraft:glass");
    CHECK(blocks->face_is_sturdy(glass, registry::BlockRegistry::Face::Up));
    CHECK_FALSE(blocks->blocks_sky_light(blocks->block_of(glass)));
    CHECK(blocks->blocks_sky_light(blocks->block_of(stone)));
}

TEST_CASE("you can see through glass", "[chunk-mesher]") {
    auto blocks = try_load_registry();
    if (!blocks) {
        SKIP("no registry pack");
    }

    const auto shape  = world::WorldShape::overworld();
    auto       centre = std::make_unique<world::Chunk>(ChunkPos{0, 0}, shape,
                                                       world::AirStates::from(*blocks), &*blocks);

    const auto glass = default_state(*blocks, "minecraft:glass");
    const auto stone = default_state(*blocks, "minecraft:stone");
    centre->set_block(1, 64, 1, glass);
    centre->set_block(2, 64, 1, stone);

    ChunkNeighbours around{};
    around[4] = centre.get();
    const ChunkSectionView view(*blocks, around, 0, 64, 0);

    // The stone's west face looks into the glass. It must survive, or the world
    // behind every window disappears.
    CHECK_FALSE(view.occludes(Vec3i{1, 0, 1}, Direction::East));
    // Stone against stone still occludes.
    CHECK(view.occludes(Vec3i{2, 0, 1}, Direction::West));
}

TEST_CASE("a section view reads through to the right chunk", "[chunk-mesher]") {
    auto blocks = try_load_registry();
    if (!blocks) {
        SKIP("no registry pack");
    }

    const auto shape  = world::WorldShape::overworld();
    auto       centre = std::make_unique<world::Chunk>(ChunkPos{0, 0}, shape,
                                                       world::AirStates::from(*blocks), &*blocks);
    auto       east   = std::make_unique<world::Chunk>(ChunkPos{1, 0}, shape,
                                                       world::AirStates::from(*blocks), &*blocks);

    const auto stone = default_state(*blocks, "minecraft:stone");
    centre->set_block(15, 64, 8, stone);
    east->set_block(0, 64, 8, stone);

    ChunkNeighbours around{};
    around[4] = centre.get();
    around[5] = east.get();

    // Section origin at (0, 64, 0), so local x 15 is the centre chunk's edge and
    // local x 16 is the first column of the chunk to the east.
    const ChunkSectionView view(*blocks, around, 0, 64, 0);

    CHECK(view.state_at(Vec3i{15, 0, 8}) == stone);
    CHECK(view.state_at(Vec3i{16, 0, 8}) == stone);
    CHECK(view.state_at(Vec3i{14, 0, 8}) == registry::kAirState);

    // Two chunks east is outside the neighbourhood: air, not solid. Treating
    // the unloaded edge as solid walls the world in with invisible surfaces.
    CHECK(view.state_at(Vec3i{40, 0, 8}) == registry::kAirState);
    CHECK_FALSE(view.occludes(Vec3i{40, 0, 8}, Direction::West));
}

TEST_CASE("an unloaded neighbour is open sky, not darkness", "[chunk-mesher]") {
    auto blocks = try_load_registry();
    if (!blocks) {
        SKIP("no registry pack");
    }

    const auto shape  = world::WorldShape::overworld();
    auto       centre = std::make_unique<world::Chunk>(ChunkPos{0, 0}, shape,
                                                       world::AirStates::from(*blocks), &*blocks);
    ChunkNeighbours around{};
    around[4] = centre.get();

    const ChunkSectionView view(*blocks, around, 0, 64, 0);

    // Off the edge of what is loaded: full daylight, so the boundary is bright
    // rather than a black wall.
    CHECK(view.sky_light(Vec3i{100, 0, 0}) == 15);
    CHECK(view.block_light(Vec3i{100, 0, 0}) == 0);
}

TEST_CASE("meshing an empty section produces nothing", "[chunk-mesher]") {
    auto blocks = try_load_registry();
    if (!blocks) {
        SKIP("no registry pack");
    }

    const auto shape  = world::WorldShape::overworld();
    auto       centre = std::make_unique<world::Chunk>(ChunkPos{0, 0}, shape,
                                                       world::AirStates::from(*blocks), &*blocks);
    ChunkNeighbours around{};
    around[4] = centre.get();

    MemoryAssetSource source;
    BlockModelCache   models(source, *blocks);
    AtlasBuilder      builder(source);
    auto              atlas = builder.build();
    REQUIRE(atlas.has_value());

    const ChunkSectionView view(*blocks, around, 0, 64, 0);
    MeshBuffers            mesh;
    const auto             stats = mesh_section(view, models, *atlas, mesh);

    CHECK(stats.blocks_visited == 4096);
    CHECK(stats.blocks_drawn == 0);
    CHECK(mesh.total_vertices() == 0);
}
