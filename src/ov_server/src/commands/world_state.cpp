#include "world_state.hpp"

#include "ov/protocol/play.hpp"

#include <algorithm>

namespace ov::server::cmd {

WorldState::WorldState(u64 seed) : random_{seed, seed ^ 0x9E3779B97F4A7C15ULL} {}

void WorldState::load(const world::LevelSettings& settings) {
    game_time               = settings.game_time;
    day_time                = settings.day_time;
    weather.clear_time      = settings.clear_weather_time;
    weather.rain_time       = settings.rain_time;
    weather.thunder_time    = settings.thunder_time;
    weather.raining         = settings.raining;
    weather.thundering      = settings.thundering;
    // A world saved while raining comes back raining, at full strength — the
    // levels are not stored, and vanilla starts them at the flag.
    weather.rain_level      = settings.raining ? 1.0F : 0.0F;
    weather.thunder_level   = settings.thundering ? 1.0F : 0.0F;
    difficulty              = static_cast<u8>(std::clamp<i32>(settings.difficulty, 0, 3));
    difficulty_locked       = settings.difficulty_locked;
    default_game_mode       = static_cast<u8>(std::clamp<i32>(settings.game_type, 0, 3));
    seed                    = settings.seed;
    rules.load(settings.game_rules);
}

void WorldState::store(world::LevelSettings& settings) const {
    settings.game_time          = game_time;
    settings.day_time           = day_time;
    settings.clear_weather_time = weather.clear_time;
    settings.rain_time          = weather.rain_time;
    settings.thunder_time       = weather.thunder_time;
    settings.raining            = weather.raining;
    settings.thundering         = weather.thundering;
    settings.difficulty         = static_cast<i8>(difficulty);
    settings.difficulty_locked  = difficulty_locked;
    settings.game_type          = default_game_mode;
    settings.game_rules         = rules.store();
}

i32 WorldState::random_between(i32 low, i32 high) {
    return low + random_.next_int(high - low + 1);
}

void WorldState::set_weather(i32 clear_time, i32 weather_time, bool raining, bool thundering) {
    weather.clear_time   = clear_time;
    weather.rain_time    = weather_time;
    weather.thunder_time = weather_time;
    weather.raining      = raining;
    weather.thundering   = thundering;
}

void WorldState::tick(std::vector<Broadcast>& out) {
    const bool was_raining = weather.is_raining();

    if (rules.flag("doWeatherCycle")) {
        if (weather.clear_time > 0) {
            --weather.clear_time;
            weather.thunder_time = weather.thundering ? 0 : 1;
            weather.rain_time    = weather.raining ? 0 : 1;
            weather.thundering   = false;
            weather.raining      = false;
        } else {
            if (weather.thunder_time > 0) {
                if (--weather.thunder_time == 0) {
                    weather.thundering = !weather.thundering;
                }
            } else {
                weather.thunder_time =
                    weather.thundering ? random_between(kThunderDurationMin, kThunderDurationMax)
                                       : random_between(kThunderDelayMin, kThunderDelayMax);
            }
            if (weather.rain_time > 0) {
                if (--weather.rain_time == 0) {
                    weather.raining = !weather.raining;
                }
            } else {
                weather.rain_time = weather.raining
                                        ? random_between(kRainDurationMin, kRainDurationMax)
                                        : random_between(kRainDelayMin, kRainDelayMax);
            }
        }
    }

    // The levels move every tick, cycle or not: the capture ran with
    // doWeatherCycle off and still watched them climb.
    const f32 previous_thunder = weather.thunder_level;
    weather.thunder_level =
        std::clamp(weather.thunder_level + (weather.thundering ? 0.01F : -0.01F), 0.0F, 1.0F);
    const f32 previous_rain = weather.rain_level;
    weather.rain_level =
        std::clamp(weather.rain_level + (weather.raining ? 0.01F : -0.01F), 0.0F, 1.0F);

    if (previous_rain != weather.rain_level) {
        out.push_back({net::clientbound::kGameEvent, net::encode_game_event(7, weather.rain_level)});
    }
    if (previous_thunder != weather.thunder_level) {
        out.push_back(
            {net::clientbound::kGameEvent, net::encode_game_event(8, weather.thunder_level)});
    }
    if (was_raining != weather.is_raining()) {
        out.push_back({net::clientbound::kGameEvent, net::encode_game_event(was_raining ? 2 : 1, 0.0F)});
        out.push_back({net::clientbound::kGameEvent, net::encode_game_event(7, weather.rain_level)});
        out.push_back(
            {net::clientbound::kGameEvent, net::encode_game_event(8, weather.thunder_level)});
    }

    ++game_time;
    if (rules.flag("doDaylightCycle")) {
        ++day_time;
    }
}

std::vector<u8> WorldState::update_time_payload() const {
    i64 shown = day_time;
    if (!rules.flag("doDaylightCycle")) {
        shown = day_time == 0 ? -1 : -day_time;
    }
    return net::encode_update_time(game_time, shown);
}

void WorldState::join_packets(std::vector<Broadcast>& out) const {
    if (weather.is_raining()) {
        out.push_back({net::clientbound::kGameEvent, net::encode_game_event(1, 0.0F)});
        out.push_back({net::clientbound::kGameEvent, net::encode_game_event(7, weather.rain_level)});
        out.push_back(
            {net::clientbound::kGameEvent, net::encode_game_event(8, weather.thunder_level)});
    }
}

}  // namespace ov::server::cmd
