// The chunk packet, encoded and read back.
//
// This is the one packet whose reader and writer can disagree silently. Every
// other packet is a handful of fixed fields; this one is a length-prefixed
// blob of paletted containers whose widths decide their own format, followed
// by block entities, followed by four bitsets and two variable-length light
// arrays. A single miscounted byte shifts everything after it, and the symptom
// is a world that looks almost right.
//
// So the check is a round trip over real chunks off a real save: encode what
// the server would send, parse it the way a client would, and compare block by
// block, biome by biome and nibble by nibble.

#include "ov/nbt/region.hpp"
#include "ov/protocol/play.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/world/chunk_storage.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>

using namespace ov;

namespace {

/// The save and the pack are generated locally and never committed, so a fresh
/// clone skips rather than fails.
struct Fixture {
    registry::BlockRegistry registry;
    std::filesystem::path   region;
};

[[nodiscard]] std::optional<Fixture> fixture() {
    const std::filesystem::path pack("data/vanilla/1.20.1/registry.ovpack");
    const std::filesystem::path region("run/world/region");
    if (!std::filesystem::exists(pack) || !std::filesystem::is_directory(region)) {
        return std::nullopt;
    }
    auto registry = registry::BlockRegistry::load(pack);
    if (!registry) {
        return std::nullopt;
    }
    return Fixture{std::move(*registry), region};
}

}  // namespace

TEST_CASE("a chunk survives the wire unchanged", "[protocol][chunk]") {
    auto env = fixture();
    if (!env) {
        SKIP("run/world or registry.ovpack is absent; generate them first");
    }

    // Any region file will do; the first one on disk is a real one either way.
    std::filesystem::path chosen;
    for (const auto& entry : std::filesystem::directory_iterator(env->region)) {
        if (entry.path().extension() == ".mca") {
            chosen = entry.path();
            break;
        }
    }
    if (chosen.empty()) {
        SKIP("no region file in run/world/region");
    }

    auto region = nbt::RegionFile::open(chosen);
    REQUIRE(region.has_value());

    world::ChunkCodecContext context;
    context.blocks = &env->registry;
    context.air    = world::AirStates::from(env->registry);

    std::vector<std::string_view> biome_names(env->registry.biome_count());
    for (u32 index = 0; index < env->registry.biome_count(); ++index) {
        biome_names[index] = env->registry.biome_name(index);
    }
    context.biome_names = biome_names;

    const auto shape = world::WorldShape::overworld();

    usize checked = 0;
    usize blocks_compared = 0;
    for (i32 index = 0; index < 1024 && checked < 24; ++index) {
        const i32  local_x = index % 32;
        const i32  local_z = index / 32;
        if (!region->has_chunk(static_cast<u32>(local_x), static_cast<u32>(local_z))) {
            continue;
        }
        const auto document =
            region->read_chunk(static_cast<u32>(local_x), static_cast<u32>(local_z));
        if (!document) {
            continue;
        }
        auto original = world::from_nbt(*document, context);
        if (!original) {
            continue;
        }

        const auto  encoded = net::encode_chunk_data(*original);
        const auto  decoded = net::parse_chunk_data(encoded, shape, context.air, &env->registry);
        REQUIRE(decoded.has_value());

        CHECK(decoded->position().x == original->position().x);
        CHECK(decoded->position().z == original->position().z);

        // Blocks, biomes and light, cell by cell. Comparing the encoded bytes
        // instead would only prove the encoder is deterministic.
        bool blocks_match = true;
        bool biomes_match = true;
        bool light_match  = true;
        for (i32 y = shape.min_y; y <= shape.max_y() && blocks_match; ++y) {
            for (usize z = 0; z < 16; ++z) {
                for (usize x = 0; x < 16; ++x) {
                    if (decoded->get_block(x, y, z) != original->get_block(x, y, z)) {
                        blocks_match = false;
                    }
                    if (decoded->get_biome(x, y, z) != original->get_biome(x, y, z)) {
                        biomes_match = false;
                    }
                    ++blocks_compared;
                }
            }
        }
        CAPTURE(original->position().x, original->position().z);
        CHECK(blocks_match);
        CHECK(biomes_match);

        // Light lives on the sections, and it is the field most likely to
        // survive a wrong reader looking right: a world lit uniformly and a
        // world lit correctly are hard to tell apart by eye.
        for (usize section = 0; section < shape.section_count() && light_match; ++section) {
            const i32 origin = shape.min_y + static_cast<i32>(section) * 16;
            const world::ChunkSection* before = original->section_for_y(origin);
            const world::ChunkSection* after  = decoded->section_for_y(origin);
            REQUIRE(before != nullptr);
            REQUIRE(after != nullptr);
            for (usize cell = 0; cell < 4096; ++cell) {
                if (before->sky_light().get(cell) != after->sky_light().get(cell) ||
                    before->block_light().get(cell) != after->block_light().get(cell)) {
                    light_match = false;
                    break;
                }
            }
        }
        CHECK(light_match);

        // The heightmaps are recomputed by the reader rather than trusted, so
        // they are worth checking separately: they must agree with the ones
        // the save carried.
        const auto after_map  = decoded->heightmap(world::HeightmapType::MotionBlocking).data();
        const auto before_map = original->heightmap(world::HeightmapType::MotionBlocking).data();
        CHECK(std::ranges::equal(after_map, before_map));

        ++checked;
    }

    REQUIRE(checked > 0);
    INFO(checked << " chunks, " << blocks_compared << " cells");
}

TEST_CASE("a Nether chunk keeps its shape through the disk and the wire",
          "[protocol][chunk][nether]") {
    // A chunk read back from disk was always built overworld-shaped: a saved
    // Nether chunk came back with 24 sections instead of 16, was sent that way
    // to a client that expects 16, and was written back with yPos -4.
    using namespace ov::world;
    const std::filesystem::path pack("data/vanilla/1.20.1/registry.ovpack");
    if (!std::filesystem::exists(pack)) {
        SKIP("registry.ovpack is absent; generate it first");
    }
    auto blocks = registry::BlockRegistry::load(pack);
    REQUIRE(blocks.has_value());
    const auto netherrack = blocks->find_block("minecraft:netherrack");
    REQUIRE(netherrack.has_value());
    const registry::BlockStateId state = blocks->default_state(*netherrack);

    const WorldShape nether = WorldShape::nether();
    const AirStates  air    = AirStates::from(*blocks);
    Chunk            chunk{ChunkPos{3, -2}, nether, air, &*blocks};
    chunk.set_block(0, 0, 0, state);      // the floor of the Nether
    chunk.set_block(5, 100, 7, state);
    chunk.set_block(15, 255, 15, state);  // and its ceiling

    ChunkCodecContext context;
    context.blocks = &*blocks;
    context.air    = air;
    std::vector<std::string_view> biome_names(blocks->biome_count());
    for (u32 index = 0; index < blocks->biome_count(); ++index) {
        biome_names[index] = blocks->biome_name(index);
    }
    context.biome_names = biome_names;
    context.shape       = nether;

    const nbt::Document document = to_nbt(chunk, context);
    const auto          back     = from_nbt(document, context);
    REQUIRE(back.has_value());
    CHECK(back->shape().min_y == 0);
    CHECK(back->shape().section_count() == 16);
    CHECK(back->get_block(0, 0, 0) == state);
    CHECK(back->get_block(5, 100, 7) == state);
    CHECK(back->get_block(15, 255, 15) == state);

    // And a client in the Nether reads what the server then sends.
    const auto encoded = net::encode_chunk_data(*back);
    const auto decoded = net::parse_chunk_data(encoded, nether, air, &*blocks);
    REQUIRE(decoded.has_value());
    CHECK(decoded->get_block(5, 100, 7) == state);
}

TEST_CASE("a truncated chunk packet is refused, not read past", "[protocol][chunk]") {
    // The payload comes off a socket. Every prefix of a valid packet must be
    // rejected rather than read as if the missing half were zeroes.
    const auto shape = world::WorldShape::overworld();
    world::AirStates air;

    // A short buffer cannot even carry the coordinates.
    const std::array<u8, 4> stub{0, 0, 0, 0};
    CHECK_FALSE(net::parse_chunk_data(stub, shape, air, nullptr).has_value());
    CHECK_FALSE(net::parse_chunk_data({}, shape, air, nullptr).has_value());
}
