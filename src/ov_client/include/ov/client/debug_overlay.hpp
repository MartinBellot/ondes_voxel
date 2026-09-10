// The F3 screen: what the game knows about where the player is, as lines.
//
// The lines, their order and their wording were read off the running 1.20.1
// client with F3 open (scripts/measure_screens.py asks its debug overlay for
// the two lists it draws). A line whose value this client does not have is
// **left out**, never drawn with a made-up number: an F3 screen that lies is
// worse than a short one. Which lines are left out is in
// docs/provenance/ecrans.md.
//
// The text is built here without a device, so the formats are testable; the
// drawing is vanilla's: each line on a grey box (`0x90505050`) one pixel
// wider than the text on each side, left column from x = 2, right column
// ending at width − 2, nine pixels a line from y = 2, text `0xE0E0E0`, no
// shadow.
#pragma once

#include "ov/base/types.hpp"
#include "ov/client/gui.hpp"

#include <optional>
#include <string>
#include <vector>

namespace ov::client {

struct DebugInfo {
    /// "1.20.1", then the brand.
    std::string version{"1.20.1"};
    i32         fps{0};
    /// Chunk sections drawn and resident, for the "C:" line.
    u32         sections_drawn{0};
    u32         sections_resident{0};
    /// Entities drawn and known, for the "E:" line.
    u32         entities_drawn{0};
    u32         entities_known{0};
    /// The integrated server's name, or empty on a remote server.
    std::string server_brand;
    f64         x{0.0};
    f64         y{0.0};
    f64         z{0.0};
    f32         yaw{0.0F};
    f32         pitch{0.0F};
    /// The biome under the player, as a registry name.
    std::string biome;
    /// Light at the feet, when the chunk carries it.
    std::optional<i32> sky_light;
    std::optional<i32> block_light;
    /// The block aimed at, as a registry name with its properties, and where.
    std::optional<std::string> target_block;
    i32                        target_x{0};
    i32                        target_y{0};
    i32                        target_z{0};
    /// Whole days and the time of day in ticks.
    i64 day_time{0};
    /// Memory: what this process uses, in bytes, and the machine's total.
    u64 memory_used{0};
    u64 memory_total{0};
    /// The graphics device.
    std::string gpu;
    u32         framebuffer_width{0};
    u32         framebuffer_height{0};
    std::string cpu;
};

/// Vanilla's cardinal name for a yaw, and the axis it faces: south is +Z.
[[nodiscard]] std::string facing_line(f32 yaw, f32 pitch);

/// The left and right columns, as vanilla orders them.
[[nodiscard]] std::vector<std::string> debug_left_lines(const DebugInfo& info);
[[nodiscard]] std::vector<std::string> debug_right_lines(const DebugInfo& info);

/// Draw both columns. Between gui.begin() and gui.flush().
void draw_debug_overlay(Gui& gui, const DebugInfo& info);

}  // namespace ov::client
