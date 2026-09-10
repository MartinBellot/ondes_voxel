// The world's clock, weather, difficulty and rules — the state `/time`,
// `/weather`, `/difficulty` and `/gamerule` change.
//
// Owned by the tick thread and by nothing else: commands run on the tick
// thread, so there is one writer and no lock, which is the project's rule for
// world state.
//
// The weather is vanilla's, as its own packets show it (the capture holds a
// minute of them):
//
//   * the rain and thunder *levels* move by 0.01 per tick in float arithmetic
//     toward 1 or 0, whatever the gamerules say, and each change is sent as
//     Game Event 7 / 8 — `0.009999999776`, `0.019999999553`, … exactly;
//   * "raining" as the client knows it is `rain level > 0.2`, and crossing it
//     sends Game Event 1 (begin) or 2 (end) followed by both levels again;
//   * the *timers* — how long until it rains, how long it lasts — only run
//     with doWeatherCycle, and draw their durations from the ranges the
//     Minecraft Wiki's Weather article gives: rain 12000–24000 ticks, thunder
//     3600–15600, and 12000–180000 between each. Vanilla's own random stream
//     cannot be reproduced (it is the level's), so the draw here is ours,
//     seeded and deterministic.
#pragma once

#include "game_rules.hpp"

#include "ov/base/types.hpp"
#include "ov/math/random.hpp"
#include "ov/world/level_dat.hpp"

#include <vector>

namespace ov::server::cmd {

/// A packet for every connected player.
struct Broadcast {
    i32             id{0};
    std::vector<u8> payload;
};

struct Weather {
    i32  clear_time{0};
    i32  rain_time{0};
    i32  thunder_time{0};
    bool raining{false};
    bool thundering{false};
    f32  rain_level{0.0F};
    f32  thunder_level{0.0F};

    /// What the client calls raining.
    [[nodiscard]] bool is_raining() const noexcept { return rain_level > 0.2F; }
};

/// Vanilla's weather durations, inclusive, in ticks.
inline constexpr i32 kRainDelayMin      = 12000;
inline constexpr i32 kRainDelayMax      = 180000;
inline constexpr i32 kRainDurationMin   = 12000;
inline constexpr i32 kRainDurationMax   = 24000;
inline constexpr i32 kThunderDelayMin   = 12000;
inline constexpr i32 kThunderDelayMax   = 180000;
inline constexpr i32 kThunderDurationMin = 3600;
inline constexpr i32 kThunderDurationMax = 15600;

class WorldState {
public:
    explicit WorldState(u64 seed = 0x4F56574541544852ULL);

    void load(const world::LevelSettings& settings);
    void store(world::LevelSettings& settings) const;

    /// One tick: weather, then the clock. Packets go into `out`.
    void tick(std::vector<Broadcast>& out);

    /// Update Time as vanilla sends it: the world's age, and the time of day —
    /// negated when doDaylightCycle is off, and -1 for a frozen 0, because the
    /// client cannot tell -0 from 0.
    [[nodiscard]] std::vector<u8> update_time_payload() const;

    /// vanilla's setWeatherParameters.
    void set_weather(i32 clear_time, i32 weather_time, bool raining, bool thundering);

    /// What a joining player is told about the weather.
    void join_packets(std::vector<Broadcast>& out) const;

    /// Uniform in [low, high], for the weather command's random durations.
    [[nodiscard]] i32 random_between(i32 low, i32 high);

    i64       game_time{0};
    i64       day_time{0};
    Weather   weather;
    u8        difficulty{2};
    bool      difficulty_locked{false};
    u8        default_game_mode{1};
    i64       seed{0};
    GameRules rules;

private:
    math::XoroshiroRandomSource random_;
};

}  // namespace ov::server::cmd
