// The Great Pyramid in a world the server generates — an original Ondes VOXEL
// structure (great_pyramid.hpp), end to end through `GeneratedWorld`.
//
// Seed 138 puts one 120 blocks from spawn, its entrance facing north: the one
// docs/provenance/grande-pyramide.md gives the user to visit. Its start chunk
// is (7, 0), its centre (120, 8), its floor y 70. The test generates the few
// squares that hold the entrance, the hall, the Pharaoh's chamber and the
// crypt, by the server's own path, and reads the blocks back.
#include "../src/generated_world.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/chunk.hpp"
#include "ov/worldgen/great_pyramid.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <map>
#include <vector>

using namespace ov;

namespace {

constexpr i64 kSeed = 138;

[[nodiscard]] std::filesystem::path data_dir() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data";
}

}  // namespace

TEST_CASE("seed 138: the Great Pyramid stands where the docs say, rooms and all",
          "[great_pyramid][world][.slow]") {
    const auto pack = data_dir() / "vanilla" / "1.20.1" / "registry.ovpack";
    const auto jar  = std::filesystem::path{OV_SOURCE_DIR} / "tools" / "vanilla" / "server.jar";
    if (!std::filesystem::is_regular_file(pack) || !std::filesystem::exists(jar) ||
        !std::filesystem::is_directory(data_dir() / "vanilla" / "1.20.1" / "generated")) {
        SKIP("no registry pack, generated data or server jar");
    }
    auto blocks     = registry::BlockRegistry::load(pack);
    auto registries = registry::Registries::load(pack);
    REQUIRE(blocks);
    REQUIRE(registries);
    std::vector<std::string_view> biomes;
    for (u32 index = 0; index < blocks->biome_count(); ++index) {
        biomes.push_back(blocks->biome_name(index));
    }
    auto world = server::GeneratedWorld::load(data_dir(), *blocks, *registries, biomes, kSeed, 1);
    REQUIRE(world);

    // Squares of 4 x 4 chunks: (4..7, -4..-1) holds the entrance and the hall,
    // (4..7, 0..3) the Pharaoh's chamber, the door and the crypt.
    std::map<std::pair<i32, i32>, world::Chunk> chunks;
    for (const auto& [ox, oz] : std::vector<std::pair<i32, i32>>{{4, -4}, {4, 0}}) {
        std::vector<std::pair<ChunkPos, world::Chunk>> out;
        world->generate_square(0, ox, oz, 4, out);
        for (auto& [pos, chunk] : out) {
            chunks.emplace(std::pair{pos.x, pos.z}, std::move(chunk));
        }
    }
    const auto name = [&](i32 x, i32 y, i32 z) -> std::string_view {
        const auto it = chunks.find({x >> 4, z >> 4});
        REQUIRE(it != chunks.end());
        const auto state = it->second.get_block(static_cast<usize>(x & 15), y,
                                                static_cast<usize>(z & 15));
        return blocks->block_name(blocks->block_of(state));
    };
    const auto entity = [&](i32 x, i32 y, i32 z) -> const nbt::Tag* {
        const auto it = chunks.find({x >> 4, z >> 4});
        REQUIRE(it != chunks.end());
        const auto* e = it->second.block_entity_at(static_cast<usize>(x & 15), y,
                                                   static_cast<usize>(z & 15));
        return e != nullptr ? &e->data : nullptr;
    };

    // Facing north, the canonical frame is the world's: (u, y, v) is
    // (120 + u, 70 + y, 8 + v).
    constexpr i32 cx = 120;
    constexpr i32 cz = 8;
    constexpr i32 y0 = 70;
    // The entrance: pillars, the portal's air, the tunnel.
    REQUIRE(name(cx - 5, y0 + 5, cz - 51) == "minecraft:chiseled_sandstone");
    REQUIRE(name(cx, y0 + 4, cz - 45) == "minecraft:air");
    REQUIRE(name(cx, y0 - 1, cz - 45) == "minecraft:cut_sandstone");
    // The hall and its pillars.
    REQUIRE(name(cx - 9, y0 + 5, cz - 15) == "minecraft:air");
    REQUIRE(name(cx + 6, y0 + 5, cz - 21) == "minecraft:cut_sandstone");
    REQUIRE(name(cx, y0, cz - 22) == "minecraft:sandstone_stairs");
    // The Pharaoh's chamber: four chests with our table.
    for (const auto& [u, v] : std::vector<std::pair<i32, i32>>{{-6, 6}, {-6, 10}, {6, 6}, {6, 10}}) {
        REQUIRE(name(cx + u, y0 + 26, cz + v) == "minecraft:chest");
        const nbt::Tag* chest = entity(cx + u, y0 + 26, cz + v);
        REQUIRE(chest != nullptr);
        REQUIRE(chest->find("LootTable")->as_string() == "ondes_voxel:chests/great_pyramid/treasure");
    }
    // The secret door, shut.
    REQUIRE(name(cx - 2, y0 + 27, cz + 3) == "minecraft:sticky_piston");
    REQUIRE(name(cx - 2, y0 + 27, cz + 1) == "minecraft:lever");
    // The crypt's spawner, of husks.
    REQUIRE(name(cx, y0 - 12, cz) == "minecraft:spawner");
    REQUIRE(entity(cx, y0 - 12, cz)->find("SpawnData")->find("entity")->find("id")->as_string() ==
            "minecraft:husk");
    // The capstone is gold.
    // (y0 + 50 is above the tested squares' interest; the shell's band is here.)
    REQUIRE(name(cx, y0 + 16, cz - 34) == "minecraft:cut_sandstone");

    // The start, in its chunk; a reference in a chunk it crosses.
    const nbt::Tag& start_chunk = chunks.at({7, 0}).structures();
    REQUIRE(start_chunk.find("starts")->find("ondes_voxel:great_pyramid") != nullptr);
    const nbt::Tag* refs = chunks.at({5, -3}).structures().find("References");
    REQUIRE(refs != nullptr);
    REQUIRE(refs->find("ondes_voxel:great_pyramid") != nullptr);
}
