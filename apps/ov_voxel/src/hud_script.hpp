// ── hud ── The HUD scenes, played by our client.
//
// scripts/hud_scenes.txt is the one list of scenes both clients play against
// the same vanilla server (scripts/measure_hud.py): the real 1.20.1 client
// through scripts/hud_oracle.java, ours through `ov_voxel --hud-script`. This
// reads it and hands the steps out one at a time, on the wall clock at 20
// ticks a second — the rate the real client's GUI ticks at, which is what its
// `wait` counts.
//
// Nothing here acts: main.cpp does each step through the paths a player's
// hand takes (a command through the chat's submit, Tab through the tab list's
// key, F3 through the menus'), so a scene shows what a player would see.
#pragma once

#include "ov/base/types.hpp"

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ov::demo {

struct HudStep {
    enum class Kind : u8 { Command, Wait, Shot, Key, Look, TabFoot, Use };
    Kind        kind{Kind::Wait};
    /// Command: the command without its slash. Shot: the name. Key: "tab" or
    /// "f3". TabFoot: the header's JSON.
    std::string text;
    /// Key: "press", "release" or "tap". TabFoot: the footer's JSON.
    std::string second;
    /// Wait: ticks.
    i32 ticks{0};
    /// Look: degrees.
    f32 yaw{0.0F};
    f32 pitch{0.0F};
};

/// Parse the scene lines. An unknown verb is an error naming its line, not a
/// silently skipped scene.
[[nodiscard]] std::expected<std::vector<HudStep>, std::string> parse_hud_scenes(std::string_view text);

class HudScript {
public:
    [[nodiscard]] static std::expected<HudScript, std::string> load(const std::string& path);

    explicit HudScript(std::vector<HudStep> steps) : steps_(std::move(steps)) {}

    /// The next step due at `now` (seconds, any origin), or nothing while a
    /// wait runs. A shot is held back 120 ms, as the oracle holds its own,
    /// so the frame it captures follows the last change by at least one.
    [[nodiscard]] std::optional<HudStep> next(f64 now);

    [[nodiscard]] bool done() const noexcept { return index_ >= steps_.size(); }
    [[nodiscard]] usize size() const noexcept { return steps_.size(); }

private:
    std::vector<HudStep> steps_;
    usize                index_{0};
    std::optional<f64>   ready_at_;
};

}  // namespace ov::demo
