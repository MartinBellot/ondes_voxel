// The catalogue: what the lab world contains, and where.
//
// One function per plot, one line per plot in the table at the bottom of
// plots.cpp. Adding a bench for a new system should be twenty lines and one
// table entry — if it is ever more than that, this file has gone wrong.
#pragma once

#include "canvas.hpp"

#include <string_view>
#include <vector>

namespace ov::lab {

/// Where a plot sits and what it is for. Plot origins are 32 blocks apart, so
/// every plot has a 16-block road around its 16x16 floor.
struct Plot {
    /// Origin in world coordinates: the plot's north-west corner.
    i32 x{0};
    i32 z{0};
    /// Shown on the plot's sign and in `--list`.
    std::string_view zone;
    std::string_view name;
    /// What the plot is a bench for — one line, printed by `--list`.
    std::string_view purpose;
    void (*build)(Canvas&, i32 x, i32 z){nullptr};
};

[[nodiscard]] std::vector<Plot> catalogue();

/// Build every plot, plus the plaza and the ground under all of it.
void build_world(Canvas& canvas);

}  // namespace ov::lab
