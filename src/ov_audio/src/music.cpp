#include "ov/audio/music.hpp"

#include "ov/audio/sound_engine.hpp"
#include "ov/base/log.hpp"

#include <algorithm>

namespace ov::audio {

namespace {

/// The wiki's "10 to 20 minutes after currently playing tracks are finished".
constexpr i32 kGameMin = 12000;
constexpr i32 kGameMax = 24000;

[[nodiscard]] MusicChoice make(std::string_view event, i32 min_delay, i32 max_delay,
                               bool replace) noexcept {
    MusicChoice choice;
    choice.event           = event;
    choice.min_delay       = min_delay;
    choice.max_delay       = max_delay;
    choice.replace_current = replace;
    return choice;
}

}  // namespace

MusicChoice default_music(bool creative) noexcept {
    return make(creative ? std::string_view{"minecraft:music.creative"}
                         : std::string_view{"minecraft:music.game"},
                kGameMin, kGameMax, false);
}

MusicChoice situational_music(const MusicSituation& situation) noexcept {
    if (situation.credits) {
        // "The song 'Alpha' always plays instantly during the credits."
        return make("minecraft:music.credits", 0, 0, true);
    }
    if (situation.menu) {
        // "Menu music plays after 1 to 30 seconds": 20 to 600 ticks. It
        // replaces what the game was playing — the game is gone.
        return make("minecraft:music.menu", 20, 600, true);
    }
    if (situation.dimension == "minecraft:the_end") {
        if (situation.boss_music) {
            // "'Boss' always plays instantly while the player is in the End if
            // the ender dragon is undefeated or resummoned."
            return make("minecraft:music.dragon", 0, 0, true);
        }
        return make("minecraft:music.end", kGameMin, kGameMax, false);
    }
    if (situation.underwater && situation.underwater_biome) {
        return make("minecraft:music.under_water", kGameMin, kGameMax, false);
    }
    if (situation.creative) {
        return default_music(true);
    }
    if (situation.biome) {
        return *situation.biome;
    }
    return default_music(false);
}

MusicManager::MusicManager(i64 seed, i32 first_delay) noexcept
    : random_(seed), delay_(std::max(first_delay, 0)) {}

void MusicManager::draw_silence(const MusicChoice& choice) {
    const i32 span = std::max(choice.max_delay - choice.min_delay, 0) + 1;
    delay_         = std::max(choice.min_delay, 0) + random_.next_int(span);
}

void MusicManager::stop(SoundEngine& engine, const MusicChoice& choice) {
    if (playing_) {
        engine.stop(SoundCategory::Music, current_);
    }
    playing_          = false;
    current_replaces_ = false;
    draw_silence(choice);
}

void MusicManager::tick(SoundEngine& engine, const MusicChoice& choice) {
    if (playing_) {
        if (!engine.is_playing(current_)) {
            // The track ended: a fresh silence, uniform over the choice's range.
            playing_          = false;
            current_replaces_ = false;
            draw_silence(choice);
            return;
        }
        if (choice.event == current_) {
            return;
        }
        if (choice.replace_current) {
            engine.stop(SoundCategory::Music, current_);
            playing_ = false;
            delay_   = 0;
        } else if (current_replaces_) {
            // The menu's track, the dragon's, the credits': theirs alone.
            engine.stop(SoundCategory::Music, current_);
            playing_          = false;
            current_replaces_ = false;
            draw_silence(choice);
            return;
        } else {
            return;
        }
    }
    delay_ = std::min(delay_, std::max(choice.max_delay, 0));
    if (delay_ > 0) {
        --delay_;
        return;
    }
    PlayRequest request;
    request.event    = choice.event;
    request.category = SoundCategory::Music;
    request.relative = true;
    request.seed     = static_cast<i64>(random_.next_int());
    if (const auto started = engine.play(request); !started) {
        // Most often: the music was not imported (ov-assetimport --music), or
        // the event is empty (warped forest). Said once per attempt, then a
        // full silence before trying again, rather than a warning every tick.
        OV_LOG_WARN("music {}: {}", choice.event, to_string(started.error()));
        delay_ = std::max(choice.max_delay, 1);
        return;
    }
    playing_          = true;
    current_replaces_ = choice.replace_current;
    current_          = std::string{choice.event};
}

}  // namespace ov::audio
