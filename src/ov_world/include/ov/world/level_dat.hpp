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

#include <optional>
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
    // ── screens ──
    /// The overworld is generated from the seed (vanilla's "minecraft:noise"
    /// generator) rather than superflat. Read from and written to
    /// WorldGenSettings, so a world created with a seed is reopened with it —
    /// by this server and by vanilla.
    bool generated{false};

    // ── The world's clocks, weather and rules ───────────────────────────────
    //
    // Written by the command engine and read back on start, so that
    // `/time set`, `/weather` and `/gamerule` survive a restart the way they
    // do in vanilla. The defaults are the values this file always wrote.
    f32  spawn_angle{0.0F};
    i64  game_time{0};
    i64  day_time{1000};
    i32  clear_weather_time{0};
    i32  rain_time{0};
    i32  thunder_time{0};
    bool raining{false};
    bool thundering{false};
    /// 0 peaceful, 1 easy, 2 normal, 3 hard.
    i8   difficulty{2};
    bool difficulty_locked{false};
    /// `GameRules`, as vanilla stores it: every value a string. Empty is an
    /// empty compound, which vanilla reads as every rule at its default.
    std::vector<std::pair<std::string, std::string>> game_rules;

    // ── dragon ── `DragonFight`, as read, or as the server's dragon fight last
    // wrote it. Empty: the fixed compound of a world nobody took to the End.
    std::optional<nbt::Tag> dragon_fight;
};

/// Read what a level.dat says about itself into `into`, leaving fields it does
/// not carry as they were. `data` is the `Data` compound.
void read_level_settings(const nbt::Tag& data, LevelSettings& into);

/// Build the level.dat document for a superflat world.
[[nodiscard]] nbt::Document make_level_dat(const LevelSettings& settings);

/// Serialise and gzip it, ready to write to disk.
[[nodiscard]] std::vector<u8> encode_level_dat(const LevelSettings& settings);

}  // namespace ov::world
