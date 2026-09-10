#include "ov/world/level_dat.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ov;
using namespace ov::world;

// A world created with a seed from the client's Create World screen must be
// reopened as that world: level.dat has to say that the overworld is
// generated ("minecraft:noise") and carry the seed, and reading it back has
// to see both. A superflat world must stay superflat.

namespace {

[[nodiscard]] LevelSettings round_trip(const LevelSettings& written) {
    const nbt::Document document = make_level_dat(written);
    const nbt::Tag*     data     = document.root.find("Data");
    REQUIRE(data != nullptr);
    LevelSettings read;
    read_level_settings(*data, read);
    return read;
}

[[nodiscard]] std::string_view overworld_generator(const LevelSettings& settings) {
    static nbt::Document document;
    document = make_level_dat(settings);
    const nbt::Tag* generator = document.root.find("Data")
                                    ->find("WorldGenSettings")
                                    ->find("dimensions")
                                    ->find("minecraft:overworld")
                                    ->find("generator");
    REQUIRE(generator != nullptr);
    return generator->find("type")->as_string();
}

}  // namespace

TEST_CASE("a seeded world declares the noise generator and reads back seeded", "[level_dat]") {
    LevelSettings seeded;
    seeded.name      = "Oracle World";
    seeded.seed      = 1234567890;
    seeded.generated = true;
    seeded.game_type = 0;
    CHECK(overworld_generator(seeded) == "minecraft:noise");
    const LevelSettings read = round_trip(seeded);
    CHECK(read.generated);
    CHECK(read.seed == 1234567890);
    CHECK(read.name == "Oracle World");
    CHECK(read.game_type == 0);
}

TEST_CASE("a superflat world stays superflat", "[level_dat]") {
    LevelSettings flat;
    flat.layers = {{"minecraft:bedrock", 1}, {"minecraft:dirt", 2}, {"minecraft:grass_block", 1}};
    CHECK(overworld_generator(flat) == "minecraft:flat");
    CHECK_FALSE(round_trip(flat).generated);
}
