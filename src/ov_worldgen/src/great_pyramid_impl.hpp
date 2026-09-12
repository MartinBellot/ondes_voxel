// Private to the three great_pyramid*.cpp files: the resolved blocks and the
// room plan in canonical coordinates.
//
// Canonical frame: `u` across the entrance face, `v` into the pyramid from it
// (the entrance is on the v = -50 face), `y` up from the floor (y = 0 is the
// first free block above the plinth; the plinth top is y = -1).
#pragma once

#include "ov/worldgen/great_pyramid.hpp"

#include <array>

namespace ov::worldgen::pyramid {

inline constexpr i32 kHalf        = 50;  // the base is 2 * 50 + 1 = 101 wide
inline constexpr i32 kTop         = 50;  // 51 layers, y = 0..50
inline constexpr i32 kPlinthHalf  = 52;  // 105 x 105
inline constexpr i32 kFeather     = 6;   // sand feathered this far past the plinth
inline constexpr i32 kMaxCauseway = 14;
inline constexpr i32 kFillDepth   = 48;  // the fill under the plinth stops here
inline constexpr i32 kClearAbove  = 16;  // terrain above the plinth cleared up to this y

/// A canonical box, inclusive.
struct CBox {
    i32 u0, y0, v0, u1, y1, v1;
};

// ── The rooms, canonical. docs/provenance/grande-pyramide.md § 2 draws them. ──
inline constexpr CBox kTunnel{-3, 0, -50, 3, 8, -31};
inline constexpr CBox kHall{-15, 0, -30, 15, 11, 0};
inline constexpr CBox kQueenPassage{2, 13, -11, 7, 15, -9};
inline constexpr CBox kQueen{8, 13, -14, 16, 17, -6};
inline constexpr CBox kPharaoh{-7, 26, 4, 7, 34, 12};
inline constexpr CBox kCryptInside{-10, -12, -10, 10, -6, 10};
inline constexpr CBox kCryptShell{-12, -14, -12, 12, -4, 12};
inline constexpr i32  kMazeOuter = 30;  // the labyrinth slab spans |u|, |v| <= 30
inline constexpr i32  kMazeCore  = 18;  // ... outside the core |u|, |v| < 18
inline constexpr i32  kMazeFloor = 13;  // corridors are y 14..16

/// The grand gallery: stairs at |u| <= 1 from v = -22 (y 0) to v = 2 (y 24).
inline constexpr i32 kGalleryFirst = -22;
inline constexpr i32 kGalleryLast  = 2;
[[nodiscard]] constexpr i32 gallery_step(i32 v) noexcept { return v + 22; }

/// The labyrinth stair from the hall: u = -17, stairs from v = -28 (y 0) to
/// v = -15 (y 13).
inline constexpr i32 kMazeStairU = -17;
[[nodiscard]] constexpr i32 maze_stair_step(i32 v) noexcept { return v + 28; }

/// The crypt stair: under the hatch at (14, -1, -3), stairs (14, -5 - i, -3 + i).
inline constexpr i32 kCryptStairU = 14;
inline constexpr i32 kHatchV      = -3;
inline constexpr i32 kCryptSteps  = 7;

/// The hall's TNT pit: a plate at (0, 0, -26) and nine TNT under the floor.
inline constexpr i32 kHallPitV = -26;

/// The crypt's sand pit, where the suspicious sand hides.
inline constexpr CBox kSandPit{-3, -13, 6, 3, -13, 9};
inline constexpr i32  kSuspiciousCount = 6;

/// Every block the pyramid is made of, resolved once. Facing-dependent states
/// are indexed by *world* facing: 0 north, 1 east, 2 south, 3 west.
struct Palette {
    registry::BlockStateId air{}, sandstone{}, smooth{}, cut{}, chiseled{}, sand{};
    registry::BlockStateId orange{}, blue{}, gold{};
    registry::BlockStateId lantern{}, lantern_hanging{}, soul_lantern{}, soul_lantern_hanging{};
    registry::BlockStateId blackstone_bricks{}, quartz{}, chiseled_quartz{};
    registry::BlockStateId spawner{}, tnt{}, plate{}, suspicious_sand{};
    registry::BlockStateId tripwire_ns{}, tripwire_ew{};
    registry::BlockStateId trapdoor{};
    std::array<registry::BlockStateId, 4> stairs{}, chest{}, dispenser{}, hook{};
    std::array<registry::BlockStateId, 4> sticky_extended{}, sticky_head{}, lever_on{};
};

}  // namespace ov::worldgen::pyramid

namespace ov::worldgen {

struct GreatPyramid::Impl {
    const registry::BlockRegistry* blocks{nullptr};
    pyramid::Palette               palette;
    /// Per block id: does the fill under the plinth stop at it? The ground —
    /// stone, sand, dirt, terracotta; not water, plants, leaves or cactus.
    std::vector<bool> opaque;
};

}  // namespace ov::worldgen
