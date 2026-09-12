// Background music: which track, and how long the silence between two.
//
// Vanilla's music is not a playlist. A track plays, then nothing for a long
// random while, then another. What is chosen depends on the situation — the
// menu, the game mode, the dimension, the biome, the water, a boss — and a
// biome says so in its own data: the registry codec's biome entries carry a
// `music` object with `sound`, `min_delay`, `max_delay` and
// `replace_current_music`, in ticks. MusicChoice is that object.
//
// The situations and their order are the wiki's Music page (see
// docs/provenance/son.md, "Musique"): the menu music "after 1 to 30 seconds",
// the credits' and the dragon's "instantly", the rest "10 to 20 minutes after
// currently playing tracks are finished", underwater music "underwater in an
// ocean or river biome", the Nether per biome. Where the page leaves an order
// open — creative in the Nether — the choice is named in situational_music.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/random.hpp"

#include <optional>
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
/// `minecraft:music.creative` in creative. 12000..24000 ticks: the wiki's "10
/// to 20 minutes", and the bounds of every biome that names its own music in
/// the data generator's output.
[[nodiscard]] MusicChoice default_music(bool creative) noexcept;

/// What the client knows that decides the music.
struct MusicSituation {
    /// No world: the title screen and the menus in front of it.
    bool menu{false};
    /// The end poem and credits roll.
    bool credits{false};
    /// `minecraft:overworld`, `minecraft:the_nether`, `minecraft:the_end`.
    std::string_view dimension{"minecraft:overworld"};
    /// A boss bar with the "play boss music" flag (0x02) is up.
    bool boss_music{false};
    /// Creative: the player may build instantly and fly.
    bool creative{false};
    /// The player's eyes are in water.
    bool underwater{false};
    /// The biome at the player is in `#minecraft:plays_underwater_music`
    /// (oceans and rivers).
    bool underwater_biome{false};
    /// The `music` field of the biome at the player, when it has one.
    std::optional<MusicChoice> biome;
};

/// The music for a situation, in the wiki's order: credits, menu, the End
/// (the dragon while a boss bar asks for it), underwater, creative, the
/// biome's own, the game's.
///
/// Creative before the biome is our reading: the wiki says creative plays
/// "the Creative music (as well as regular)" and the Nether "a Nether biome
/// track", and does not say which wins in the Nether in creative.
[[nodiscard]] MusicChoice situational_music(const MusicSituation& situation) noexcept;

class MusicManager {
public:
    /// `first_delay` is the silence before the first track, in ticks. Ours: the
    /// value vanilla starts with is not documented. The first situation's
    /// maximum caps it, so the menu still speaks within its 30 seconds.
    explicit MusicManager(i64 seed, i32 first_delay = 100) noexcept;

    /// One client tick (20 Hz). Starts a track through the engine when the
    /// silence runs out; draws a new silence when a track has ended.
    ///
    /// When the situation changes:
    ///  * a choice that replaces (the menu, the dragon, the credits) stops the
    ///    track playing and starts at once;
    ///  * a track that was started by such a choice stops when its situation
    ///    ends — the wiki's menu music that "stops playing when the player
    ///    enters the loading world screen";
    ///  * a silence longer than the new choice's maximum is cut to it, which is
    ///    what makes the menu speak within its 30 seconds after a game.
    void tick(SoundEngine& engine, const MusicChoice& choice);

    /// Stop the track now and draw a fresh silence for `choice`.
    void stop(SoundEngine& engine, const MusicChoice& choice);

    [[nodiscard]] i32              ticks_until_next() const noexcept { return delay_; }
    [[nodiscard]] bool             playing() const noexcept { return playing_; }
    [[nodiscard]] std::string_view current() const noexcept { return current_; }

private:
    void draw_silence(const MusicChoice& choice);

    math::LegacyRandomSource random_;
    i32                      delay_;
    bool                     playing_{false};
    /// The track was started by a choice that replaces.
    bool                     current_replaces_{false};
    std::string              current_;
};

}  // namespace ov::audio
