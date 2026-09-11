#include "ov/client/debug_overlay.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace ov::client {
namespace {

/// Vanilla's `Mth.wrapDegrees`: into [−180, 180).
[[nodiscard]] f32 wrap_degrees(f32 degrees) {
    f32 wrapped = std::fmod(degrees, 360.0F);
    if (wrapped >= 180.0F) {
        wrapped -= 360.0F;
    }
    if (wrapped < -180.0F) {
        wrapped += 360.0F;
    }
    return wrapped;
}

[[nodiscard]] i32 floor_int(f64 value) {
    return static_cast<i32>(std::floor(value));
}

}  // namespace

std::string facing_line(f32 yaw, f32 pitch) {
    // Direction from the yaw in quarter turns: 0 south (+Z), 1 west, 2 north,
    // 3 east — Minecraft's own convention, in which yaw 0 faces +Z.
    const i32 quarter = static_cast<i32>(std::floor(yaw / 90.0F + 0.5F)) & 3;
    static constexpr std::array<std::string_view, 4> kName{"south", "west", "north", "east"};
    static constexpr std::array<std::string_view, 4> kTowards{
        "Towards positive Z", "Towards negative X", "Towards negative Z", "Towards positive X"};
    return fmt::format("Facing: {} ({}) ({:.1f} / {:.1f})", kName[static_cast<usize>(quarter)],
                       kTowards[static_cast<usize>(quarter)], wrap_degrees(yaw),
                       wrap_degrees(pitch));
}

std::vector<std::string> debug_left_lines(const DebugInfo& info) {
    std::vector<std::string> lines;
    lines.push_back(fmt::format("Minecraft {} ({}/vanilla)", info.version, info.version));
    lines.push_back(fmt::format("{} fps", info.fps));
    if (!info.server_brand.empty()) {
        lines.push_back(info.server_brand);
    }
    lines.push_back(fmt::format("C: {}/{}", info.sections_drawn, info.sections_resident));
    lines.push_back(fmt::format("E: {}/{}", info.entities_drawn, info.entities_known));
    lines.push_back("minecraft:overworld");
    lines.emplace_back();
    const i32 bx = floor_int(info.x);
    const i32 by = floor_int(info.y);
    const i32 bz = floor_int(info.z);
    lines.push_back(fmt::format("XYZ: {:.3f} / {:.5f} / {:.3f}", info.x, info.y, info.z));
    lines.push_back(fmt::format("Block: {} {} {} [{} {} {}]", bx, by, bz, bx & 15, by & 15, bz & 15));
    const i32 cx = bx >> 4;
    const i32 cz = bz >> 4;
    lines.push_back(fmt::format("Chunk: {} {} {} [{} {} in r.{}.{}.mca]", cx, by >> 4, cz, cx & 31,
                                cz & 31, cx >> 5, cz >> 5));
    lines.push_back(facing_line(info.yaw, info.pitch));
    if (info.sky_light && info.block_light) {
        lines.push_back(fmt::format("Client Light: {} ({} sky, {} block)",
                                    std::max(*info.sky_light, *info.block_light), *info.sky_light,
                                    *info.block_light));
    }
    if (!info.biome.empty()) {
        lines.push_back("Biome: " + info.biome);
    }
    return lines;
}

std::vector<std::string> debug_right_lines(const DebugInfo& info) {
    std::vector<std::string> lines;
    const u64 mib = 1024ULL * 1024ULL;
    if (info.memory_total != 0) {
        lines.push_back(fmt::format("Mem: {}% {}/{}MB", info.memory_used * 100 / info.memory_total,
                                    info.memory_used / mib, info.memory_total / mib));
    }
    lines.emplace_back();
    if (!info.cpu.empty()) {
        lines.push_back("CPU: " + info.cpu);
    }
    lines.emplace_back();
    lines.push_back(fmt::format("Display: {}x{}", info.framebuffer_width, info.framebuffer_height));
    if (!info.gpu.empty()) {
        lines.push_back(info.gpu);
    }
    if (info.target_block) {
        lines.emplace_back();
        lines.push_back(fmt::format("§nTargeted Block: {}, {}, {}", info.target_x, info.target_y,
                                    info.target_z));
        lines.push_back(*info.target_block);
    }
    return lines;
}

void draw_debug_overlay(Gui& gui, const DebugInfo& info) {
    constexpr f32 kLine = 9.0F;
    constexpr u32 kBox  = 0x90505050U;
    constexpr u32 kInk  = 0xFFE0E0E0U;
    const auto    left  = debug_left_lines(info);
    const auto    right = debug_right_lines(info);
    // Boxes first, then text, so the batch is cut once rather than per line.
    for (usize i = 0; i < left.size(); ++i) {
        if (left[i].empty()) {
            continue;
        }
        const f32 y = 2.0F + kLine * static_cast<f32>(i);
        gui.fill(1.0F, y - 1.0F, gui.font().width(left[i]) + 2.0F, kLine, kBox);
    }
    for (usize i = 0; i < right.size(); ++i) {
        if (right[i].empty()) {
            continue;
        }
        const f32 y     = 2.0F + kLine * static_cast<f32>(i);
        const f32 width = gui.font().width(right[i]);
        gui.fill(gui.width() - 2.0F - width - 1.0F, y - 1.0F, width + 2.0F, kLine, kBox);
    }
    for (usize i = 0; i < left.size(); ++i) {
        gui.text(2.0F, 2.0F + kLine * static_cast<f32>(i), left[i], kInk, false);
    }
    for (usize i = 0; i < right.size(); ++i) {
        gui.text_right(gui.width() - 2.0F, 2.0F + kLine * static_cast<f32>(i), right[i], kInk,
                       false);
    }
}

}  // namespace ov::client
