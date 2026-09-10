// The 45 gamerules of 1.20.1: names, types and defaults.
//
// The names and the bool/int split are the data generator's command report
// (`reports/commands.json`, the children of `gamerule`); the defaults were
// read from a fresh vanilla server by `scripts/measure_gamerules.py`, one
// `gamerule <name>` query each. Nothing here came from memory.
//
// Values are stored in level.dat the way vanilla stores them — every one a
// string, "true" or "3" — and a rule this server does not know is kept and
// written back untouched, so opening a world never loses a setting.
#pragma once

#include "ov/base/types.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ov::server::cmd {

struct GameRuleSpec {
    std::string_view name;
    bool             integer{false};
    i32              default_value{0};
};

/// Alphabetical, which is the order vanilla's own `gamerule` tree lists them
/// in (the Commands packet shows it).
inline constexpr std::array<GameRuleSpec, 45> kGameRules{{
    {"announceAdvancements", false, 1},
    {"blockExplosionDropDecay", false, 1},
    {"commandBlockOutput", false, 1},
    {"commandModificationBlockLimit", true, 32768},
    {"disableElytraMovementCheck", false, 0},
    {"disableRaids", false, 0},
    {"doDaylightCycle", false, 1},
    {"doEntityDrops", false, 1},
    {"doFireTick", false, 1},
    {"doImmediateRespawn", false, 0},
    {"doInsomnia", false, 1},
    {"doLimitedCrafting", false, 0},
    {"doMobLoot", false, 1},
    {"doMobSpawning", false, 1},
    {"doPatrolSpawning", false, 1},
    {"doTileDrops", false, 1},
    {"doTraderSpawning", false, 1},
    {"doVinesSpread", false, 1},
    {"doWardenSpawning", false, 1},
    {"doWeatherCycle", false, 1},
    {"drowningDamage", false, 1},
    {"fallDamage", false, 1},
    {"fireDamage", false, 1},
    {"forgiveDeadPlayers", false, 1},
    {"freezeDamage", false, 1},
    {"globalSoundEvents", false, 1},
    {"keepInventory", false, 0},
    {"lavaSourceConversion", false, 0},
    {"logAdminCommands", false, 1},
    {"maxCommandChainLength", true, 65536},
    {"maxEntityCramming", true, 24},
    {"mobExplosionDropDecay", false, 1},
    {"mobGriefing", false, 1},
    {"naturalRegeneration", false, 1},
    {"playersSleepingPercentage", true, 100},
    {"randomTickSpeed", true, 3},
    {"reducedDebugInfo", false, 0},
    {"sendCommandFeedback", false, 1},
    {"showDeathMessages", false, 1},
    {"snowAccumulationHeight", true, 1},
    {"spawnRadius", true, 10},
    {"spectatorsGenerateChunks", false, 1},
    {"tntExplosionDropDecay", false, 0},
    {"universalAnger", false, 0},
    {"waterSourceConversion", false, 1},
}};

class GameRules {
public:
    GameRules();

    [[nodiscard]] static std::optional<usize> index_of(std::string_view name) noexcept;

    [[nodiscard]] i32  value(usize index) const noexcept { return values_[index]; }
    [[nodiscard]] bool flag(std::string_view name) const noexcept;
    [[nodiscard]] i32  number(std::string_view name) const noexcept;
    void               set(usize index, i32 value) noexcept { values_[index] = value; }

    /// "true", "false" or the decimal number: how vanilla prints and stores it.
    [[nodiscard]] std::string text(usize index) const;

    /// Read level.dat's `GameRules`: a compound of strings. Unknown names are
    /// kept for writing back; unparsable values keep the default.
    void load(const std::vector<std::pair<std::string, std::string>>& stored);

    /// Everything to write: every known rule, then the unknown ones as found.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> store() const;

private:
    std::array<i32, kGameRules.size()>                values_{};
    std::vector<std::pair<std::string, std::string>> unknown_;
};

}  // namespace ov::server::cmd
