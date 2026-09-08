// The surface height of each of a chunk's 256 columns.
//
// The client needs two of these in every chunk packet and will not render
// without them. The server needs them for rain, for snow, for mob spawning, for
// lightning and for the "can the sky see this block" question the light engine
// asks constantly. Computing that by scanning down a column would make each of
// those a 384-block walk.
//
// What is stored is **the first free space above the surface**, as an offset
// from the dimension's floor — not the surface block's own y. An empty column
// stores 0. Measured against 4577024 columns of a real world: the stored value
// is `top + 1 - min_y` in every one of them. The off-by-one in the other
// direction is the kind that makes rain fall a block into the ground.
//
// Two things the same measurement settled, and neither is guessable:
//
//   * The origin is the **dimension's** floor, not the lowest section the file
//     happens to list. Two chunks in that world list 25 sections starting at -5
//     rather than 24 starting at -4, because vanilla writes an extra section
//     below the world for lighting. Deriving the origin from the section list
//     shifts every column in those chunks by sixteen blocks.
//
//   * The packing is the palette rule again: 9 bits an entry, seven to a long,
//     and an entry never spans two longs. 37 longs, not 36.
#pragma once

#include "ov/base/types.hpp"
#include "ov/world/paletted_container.hpp"

#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ov::world {

/// Columns in a chunk: 16 x 16.
inline constexpr usize kColumnCount = 256;

/// The heightmaps vanilla keeps.
///
/// Only the first two are sent to the client; the rest are the server's own,
/// and the `_WG` pair exists only while a chunk is being generated. They are
/// named here because a chunk read from disk carries whichever ones it was
/// saved with, and silently dropping one would lose it on the next save.
enum class HeightmapType : u8 {
    /// Highest non-air block. Sent to the client.
    WorldSurface,
    /// Highest block that stops movement. Sent to the client.
    MotionBlocking,
    /// The same, ignoring leaves.
    MotionBlockingNoLeaves,
    /// Highest block that is solid or a fluid.
    OceanFloor,
    /// WorldSurface as it stood during generation.
    WorldSurfaceWG,
    /// OceanFloor as it stood during generation.
    OceanFloorWG,
};

/// The name the format uses, e.g. "WORLD_SURFACE".
[[nodiscard]] std::string_view to_string(HeightmapType type) noexcept;

/// The type a format name refers to, if any.
[[nodiscard]] std::optional<HeightmapType> heightmap_type_from(std::string_view name) noexcept;

/// The two the chunk packet carries.
[[nodiscard]] bool is_sent_to_client(HeightmapType type) noexcept;

/// Bits an entry needs to address a world `height` blocks tall.
///
/// A 384-block world stores values 0 to 384 inclusive — 385 possibilities — so
/// nine bits. Sizing for 384 rather than 385 would make the very top of the
/// world unrepresentable, which is invisible until someone builds there.
[[nodiscard]] constexpr u8 bits_for_height(u32 height) noexcept {
    u8 bits = 1;
    while ((u32{1} << bits) < height + 1) {
        ++bits;
    }
    return bits;
}

class Heightmap {
public:
    /// A heightmap for a dimension whose floor is `min_y` and which is
    /// `height` blocks tall. Every column starts empty.
    Heightmap(i32 min_y, u32 height);

    /// World y of the first free space above the column's surface.
    ///
    /// Equals `min_y` for a column holding nothing, which is what makes an
    /// empty column and a column full to the floor distinguishable.
    [[nodiscard]] i32 first_free(usize x, usize z) const noexcept;

    /// Record that the column's surface is the block at `block_y`.
    ///
    /// Clamped rather than rejected: a caller passing a y outside the world is
    /// a bug, but it is not worth losing a chunk over.
    void set_surface(usize x, usize z, i32 block_y) noexcept;

    /// Mark the column as holding nothing.
    void clear_column(usize x, usize z) noexcept;

    /// Raise the surface if a block was placed above the current one. Cheap,
    /// and the common case when a player builds.
    void raise_to(usize x, usize z, i32 block_y) noexcept;

    [[nodiscard]] u8 bits() const noexcept { return bits_; }

    [[nodiscard]] i32 min_y() const noexcept { return min_y_; }

    [[nodiscard]] u32 height() const noexcept { return height_; }

    /// The packed longs, exactly as they go to disk and on the wire.
    [[nodiscard]] std::span<const u64> data() const noexcept { return data_; }

    /// Adopt packed longs from a chunk file or a packet.
    ///
    /// Returns false unless the length matches this dimension's shape. Both
    /// sources are untrusted, and a short array would be read past on the
    /// first lookup.
    [[nodiscard]] bool load(std::span<const u64> longs);

private:
    [[nodiscard]] u16 raw(usize index) const noexcept;
    void              set_raw(usize index, u16 value) noexcept;

    i32              min_y_{0};
    u32              height_{0};
    u8               bits_{0};
    std::vector<u64> data_;
};

}  // namespace ov::world
