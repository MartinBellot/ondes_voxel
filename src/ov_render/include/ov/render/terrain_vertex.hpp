// The terrain vertex, packed into twelve bytes.
//
// The plan said eight, and eight is what this was until the fields were
// actually counted against a real atlas. The arithmetic:
//
//     position, 3 axes ....... 33 bits   1/64 of a block over 32 blocks
//     atlas u, v ............. 22 bits   half a texel on a 1024 atlas
//     sky + block light ....... 8 bits   vanilla's own 4 + 4
//     ambient occlusion ....... 2 bits   vanilla's four levels
//     facing .................. 3 bits   six directions, plus "unshaded"
//     tint channel ............ 2 bits   none, grass, foliage, water
//     ------------------------------------------------------------------
//     total .................. 70 bits
//
// Eight bytes is 64. The first version fitted by giving the texture
// coordinates eight bits each, which is enough for a sprite and nowhere near
// enough for an atlas: 256 steps across a 1024-texel atlas puts every vertex on
// a four-texel grid, and every block in the game would have sampled the wrong
// part of its own texture. The bug was invisible in the packing tests, which
// only ever asked whether a value survived the round trip, and appeared the
// moment a real atlas rect went through it.
//
// Twelve bytes leaves 26 bits spare, which is not waste — it is where the
// second uv for an overlay, or a normal for anything that is not axis-aligned,
// will go without another format change.
//
// Eight bytes is still reachable, and it is worth writing down how, so that the
// option is not lost: give each section a palette of the sprites it uses,
// store a six-bit palette index and a sprite-local uv instead of an atlas one,
// and move the tint into the palette entry where it belongs — a tint is a
// property of a material, not of a vertex. That trades a dependent read in the
// shader for four bytes a vertex, and whether it pays is a measurement nobody
// has taken. Taking it before it is needed is how a renderer ends up with a
// clever format and no picture.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/vec.hpp"

#include <array>

namespace ov::render {

/// Which biome-driven colour multiplies this vertex.
///
/// Vanilla multiplies the tint into a vertex colour on the CPU; three more
/// bytes for that would not have fitted, so the channel is named and the shader
/// looks the colour up. A real difference in mechanism, and correct as long as
/// the tint is constant across a quad — which for one block it is.
enum class TintChannel : u8 {
    None    = 0,
    Grass   = 1,
    Foliage = 2,
    Water   = 3,
};

/// A vertex before packing. Never stored in a buffer; it exists so the packing
/// rules have one place to be stated and tested.
struct TerrainVertexAttributes {
    /// Section-local position in blocks, within
    /// [kPositionMin, kPositionMax].
    Vec3f position{};
    /// Atlas coordinates, normalised to 0..1, v downward. Already resolved
    /// through the sprite's rect by the mesher.
    f32 u{};
    f32 v{};
    /// 0..15, as stored in a chunk's light arrays.
    u8 sky_light{15};
    u8 block_light{0};
    /// 0 (fully occluded corner) to 3 (open).
    u8 ao{3};
    /// The face this vertex belongs to, for directional shading.
    Direction facing{Direction::Up};
    /// Elements with `"shade": false` take no directional shading at all.
    bool        shade{true};
    TintChannel tint{TintChannel::None};
};

/// Position range, in blocks relative to the section origin.
///
/// A section is 16 blocks; the extra eight on each side cover an element that
/// overhangs, with room left over. The scale is a power of two, so every
/// coordinate a model can state — they are all multiples of 1/16 — survives the
/// round trip exactly.
inline constexpr f32 kPositionMin   = -8.0F;
inline constexpr f32 kPositionScale = 2048.0F;
inline constexpr f32 kPositionMax   = kPositionMin + 65535.0F / kPositionScale;

/// The `facing` code that means "no directional shading". It reuses the two
/// spare values of a three-bit direction field rather than costing a bit.
inline constexpr u8 kFacingUnshaded = 6;

/// Bit layout, low bit first. Word-aligned on purpose: no field straddles a
/// 32-bit word, so the vertex shader unpacks it with six bitfieldExtract calls
/// and no reassembly.
///
///     word 0    0-15   x        16 bits
///              16-31   y        16 bits
///     word 1    0-15   z        16 bits
///              16-31   u        16 bits   normalised atlas coordinate
///     word 2    0-15   v        16 bits
///              16-19   sky       4 bits
///              20-23   block     4 bits
///              24-25   ao        2 bits
///              26-28   facing    3 bits   0-5 a Direction, 6 unshaded
///              29-30   tint      2 bits
///                 31   spare     1 bit
struct TerrainVertex {
    std::array<u32, 3> words{};

    friend bool operator==(const TerrainVertex&, const TerrainVertex&) noexcept = default;
};

static_assert(sizeof(TerrainVertex) == 12, "the vertex format is part of the memory budget");

[[nodiscard]] TerrainVertex pack_vertex(const TerrainVertexAttributes& attributes) noexcept;

/// Round-trips everything except the precision the packing deliberately drops.
[[nodiscard]] TerrainVertexAttributes unpack_vertex(const TerrainVertex& vertex) noexcept;

}  // namespace ov::render
