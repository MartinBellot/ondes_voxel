// Background music: which track, and how long the silence between two.
//
// Vanilla's music is not a playlist. A track plays, then nothing for a long
// random while, then another. What is chosen depends on the situation — the
// game mode, the dimension, the biome — and a biome says so in its own data:
// the data generator's biome JSONs carry a `music` object with `sound`,
// `min_delay`, `max_delay` and `replace_current_music`, in ticks. MusicChoice
// is that object.
//
// The two situations this client can tell apart today are the overworld in
// survival and in creative (see default_music). Biome and dimension tracks
// need the biome's music settings on the client, which Registry Data carries
// and ov_netclient does not yet decode — named in docs/provenance/son.md,
// not approximated.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/random.hpp"

#include <string>
#include <string_view>

namespace ov::audio {

class SoundEngine;

/// The shape of a biome's `music` field.
struct MusicChoice {
    std::string_view event;
    i32              min_delay{12000};
    i32              max_delay{24000};
    bool             replace_current{false};
};

/// The overworld's situational music: `minecraft:music.game`, or
/// `minecraft:music.creative` in creative. The delays are the 12000..24000
/// ticks every biome that names its own music uses in the data generator's
/// output; that the situational default shares them is an inference, named as
/// one in son.md.
[[nodiscard]] MusicChoice default_music(bool creative) noexcept;

class MusicManager {
public:
    /// `first_delay` is the silence before the first track, in ticks. Ours: the
    /// value vanilla starts with is not documented.
    explicit MusicManager(i64 seed, i32 first_delay = 100) noexcept;

    /// One client tick (20 Hz). Starts a track through the engine when the
    /// silence runs out; draws a new silence when a track has ended.
    void tick(SoundEngine& engine, const MusicChoice& choice);

    [[nodiscard]] i32              ticks_until_next() const noexcept { return delay_; }
    [[nodiscard]] bool             playing() const noexcept { return playing_; }
    [[nodiscard]] std::string_view current() const noexcept { return current_; }

private:
    math::LegacyRandomSource random_;
    i32                      delay_;
    bool                     playing_{false};
    std::string              current_;
};

}  // namespace ov::audio
