// level.dat — the file that makes a directory of regions into a world.
//
// Without it Minecraft sees no world at all, whatever region files are present:
// the save list is built from level.dat, and a folder without one simply does
// not appear. It is also where the *generator* is declared, which matters more
// than it looks — a world whose regions were made by a flat generator but whose
// level.dat says "noise" will grow normal terrain the moment the player walks
// past what was already saved.
//
// gzip, unlike chunks inside a region, which are zlib. Nothing announces this;
// a level.dat written with the wrong container reads as a corrupt file.
#pragma once

#include "ov/nbt/binary.hpp"
#include "ov/world/chunk.hpp"

#include <string>
#include <vector>

namespace ov::world {

/// One layer of a superflat preset, bottom upwards.
struct FlatLayer {
    std::string block;
    i32         height{1};
};

/// What a world needs to describe itself.
struct LevelSettings {
    std::string name{"Ondes VOXEL"};
    i64         seed{0};
    i32         spawn_x{0};
    i32         spawn_y{-60};
    i32         spawn_z{0};
    /// 0 survival, 1 creative, 2 adventure, 3 spectator.
    i32                    game_type{1};
    std::string            biome{"minecraft:plains"};
    std::vector<FlatLayer> layers;
};

/// Build the level.dat document for a superflat world.
[[nodiscard]] nbt::Document make_level_dat(const LevelSettings& settings);

/// Serialise and gzip it, ready to write to disk.
[[nodiscard]] std::vector<u8> encode_level_dat(const LevelSettings& settings);

}  // namespace ov::world
