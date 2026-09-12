// Subtitles: a line for each sound heard, in a black box above the bottom
// right corner, with an arrow toward a sound off to the side.
//
// What the wiki's Subtitles page documents for Java Edition: the black box
// "above the bottom right corner"; "'<' or '>' point in the direction the
// sound is coming from"; as a sound fades "the text also fades, becoming less
// white"; the option is "Show Subtitles" (`showSubtitles` in options.txt,
// written by the real client — src/ov_client/tests/data). The numbers the
// page does not give are ours and named where they are chosen: how long a
// line stays, how far to the side a sound must be for an arrow, the grey it
// fades to, the box's margins. None is measured against the vanilla client.
//
// Layer 15 and nothing but geometry until draw(): lines() is what a test reads.
#pragma once

#include "ov/audio/sound_engine.hpp"
#include "ov/base/types.hpp"
#include "ov/math/vec.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ov::client {

class Gui;

class SubtitleOverlay {
public:
    /// Seconds a line stays after its sound was last heard: ours, 3 s, times
    /// options.txt's `notificationDisplayTime` (1.0 by default).
    static constexpr f64 kDisplaySeconds = 3.0;
    /// A sound this far to the side of the head (|pan|) gets an arrow. Ours.
    static constexpr f32 kArrowPan = 0.5F;

    /// A sound started with this subtitle key. The same key heard again takes
    /// the existing line — refreshed, moved to the new position — rather than
    /// a second one.
    void heard(std::string_view key, Vec3d position, bool relative);

    /// The display clock, in seconds: the interface's, not the game's.
    void advance(f64 seconds) noexcept { now_ += seconds; }
    void set_display_time(f64 multiplier) noexcept { display_ = kDisplaySeconds * multiplier; }

    struct Line {
        std::string key;
        /// -1 left ('<'), 0 ahead or behind or relative, 1 right ('>').
        i32 arrow{0};
        /// 1 when just heard, down to 0 when it leaves.
        f32 freshness{1.0F};
    };
    /// The lines still up, newest at the bottom, as draw() lays them out.
    [[nodiscard]] std::vector<Line> lines(const audio::Listener& listener) const;

    /// Forget the lines whose time is up.
    void prune();

    /// Draw, translating each key. Nothing when there is no line.
    void draw(Gui& gui, const std::function<std::string(std::string_view)>& translate,
              const audio::Listener& listener) const;

    [[nodiscard]] usize size() const noexcept { return entries_.size(); }

private:
    struct Entry {
        std::string key;
        Vec3d       position{};
        bool        relative{false};
        f64         time{0.0};
    };
    std::vector<Entry> entries_;
    f64                now_{0.0};
    f64                display_{kDisplaySeconds};
};

}  // namespace ov::client
