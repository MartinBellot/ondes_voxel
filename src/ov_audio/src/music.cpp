#include "ov/audio/music.hpp"

#include "ov/audio/sound_engine.hpp"
#include "ov/base/log.hpp"

#include <algorithm>

namespace ov::audio {

MusicChoice default_music(bool creative) noexcept {
    MusicChoice choice;
    choice.event = creative ? std::string_view{"minecraft:music.creative"}
                            : std::string_view{"minecraft:music.game"};
    return choice;
}

MusicManager::MusicManager(i64 seed, i32 first_delay) noexcept
    : random_(seed), delay_(std::max(first_delay, 0)) {}

void MusicManager::tick(SoundEngine& engine, const MusicChoice& choice) {
    if (playing_) {
        if (!engine.is_playing(current_)) {
            // The track ended: a fresh silence, uniform over the choice's range.
            playing_       = false;
            const i32 span = std::max(choice.max_delay - choice.min_delay, 0) + 1;
            delay_         = choice.min_delay + random_.next_int(span);
            return;
        }
        if (choice.replace_current && choice.event != current_) {
            engine.stop(SoundCategory::Music, current_);
            playing_ = false;
            delay_   = 0;
        } else {
            return;
        }
    }
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
        // Most often: the music was not imported (ov-assetimport --music). Said
        // once per attempt, then a full silence before trying again, rather than
        // a warning every tick.
        OV_LOG_WARN("music {}: {}", choice.event, to_string(started.error()));
        delay_ = std::max(choice.min_delay, 1);
        return;
    }
    playing_ = true;
    current_ = std::string{choice.event};
}

}  // namespace ov::audio
