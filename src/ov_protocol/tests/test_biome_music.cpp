// Biome music and the dimension, read back out of the packets our server
// sends: Login (play) with the codec built from the data generator, and
// Respawn. The codec is generated locally and never committed, so the first
// test skips — saying so — where it has not been built.
#include "ov/io/byte_writer.hpp"
#include "ov/io/file.hpp"
#include "ov/protocol/biome_music.hpp"
#include "ov/protocol/play.hpp"
#include "ov/protocol/types.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>

using namespace ov;

TEST_CASE("Login (play): the codec's biome music and the dimension name", "[protocol][music]") {
    const auto path = std::filesystem::path{OV_SOURCE_DIR} / "data/vanilla/1.20.1/registry_codec.nbt";
    const auto codec = io::read_file(path);
    if (!codec) {
        SKIP("no data/vanilla/1.20.1/registry_codec.nbt (tools/ov_datagen)");
    }
    net::LoginPlay login;
    login.entity_id      = 7;
    login.dimension_type = "minecraft:the_nether";
    login.dimension_name = "minecraft:the_nether";
    login.registry_codec = *codec;
    const auto body      = net::encode_login_play(login);

    const auto world = net::read_login_world(body);
    REQUIRE(world.has_value());
    CHECK(world->dimension == "minecraft:the_nether");
    // 31 biomes of 1.20.1 name their music in the data generator's output.
    CHECK(world->music.size() == 31);
    const auto find = [&](std::string_view biome) {
        return std::ranges::find_if(world->music,
                                    [&](const net::BiomeMusic& m) { return m.biome == biome; });
    };
    const auto forest = find("minecraft:birch_forest");
    REQUIRE(forest != world->music.end());
    CHECK(forest->sound == "minecraft:music.overworld.forest");
    CHECK(forest->min_delay == 12000);
    CHECK(forest->max_delay == 24000);
    CHECK_FALSE(forest->replace_current);
    const auto warped = find("minecraft:warped_forest");
    REQUIRE(warped != world->music.end());
    CHECK(warped->sound == "minecraft:music.nether.warped_forest");
    // Plains has no music of its own: the game's plays there.
    CHECK(find("minecraft:plains") == world->music.end());
}

TEST_CASE("Respawn: the dimension name is the second field", "[protocol][music]") {
    io::ByteWriter writer;
    net::write_string(writer, "minecraft:the_end");
    net::write_string(writer, "minecraft:the_end");
    writer.write_i64(0);  // what follows is not read
    const auto name = net::read_respawn_dimension(writer.data());
    REQUIRE(name.has_value());
    CHECK(*name == "minecraft:the_end");
    CHECK_FALSE(net::read_respawn_dimension(std::span<const u8>{}).has_value());
}
