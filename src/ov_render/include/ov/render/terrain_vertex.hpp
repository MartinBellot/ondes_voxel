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
// Sixteen bytes now, and the fourth word is the biome tint, as eight bits a
// channel. It went from two bits to twenty-four for a reason that only a
// correct biome table makes visible: a tint is not one of four colours, it is
// the average of the twenty-five biome cells around the block, sampled out of
// a colormap texture, and a grass block in a plains and one four blocks into a
// forest differ by a few units in each channel. Two bits named a channel and
// the shader looked up a constant; that is exactly as wrong as it sounds, and
// it is what made every meadow, jungle and taiga in the world the same green.
//
// The cost is measured and real: four more bytes a vertex is 81 MiB more in
// the arena at a radius of 12. The alternatives were considered and rejected —
// packing the colour into the one spare bit and the two the tint channel gave
// back does not fit in three bits, and there is no precision left to take from
// the uv without reintroducing the atlas bug below.
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

/// Which biome-driven colour a block's tinted faces take.
///
/// Still named rather than resolved at this level: the channel is a property of
/// the *block* — grass takes grass, leaves take foliage — while the colour it
/// resolves to is a property of the *place*. The mesher, which knows both,
/// turns one into the other and bakes the result into the vertex, the way
/// vanilla does.
enum class TintChannel : u8 {
    None    = 0,
    Grass   = 1,
    Foliage = 2,
    Water   = 3,
    /// Spruce and birch leaves take a fixed colour and ignore the biome
    /// entirely — a spruce in a jungle is the same dark green as one in a
    /// taiga. Channels rather than special cases in the mesher, because that is
    /// exactly what they are: a rule about the block, resolved by whoever knows
    /// the colours.
    EvergreenFoliage = 4,
    BirchFoliage     = 5,
    /// ── implicit water ── a lily pad in a level: one constant, whatever the
    /// biome (the inventory item takes another).
    LilyPad = 6,
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
    /// Light in quarter levels, 0..60: smooth lighting sums four levels, and
    /// the sum is kept rather than rounded to a level so that light falls off
    /// across a face in quarter steps, as the game's does. Flat lighting is
    /// four times the stored level.
    u8 sky_quarters{60};
    u8 block_quarters{0};
    /// Smooth-lighting brightness of this corner, 0..1: the mean of four shade
    /// brightnesses (1.0, 0.8, 0.6, 0.4 at a full face's corners), interpolated
    /// for a vertex inside the face. Eight bits.
    f32 occlusion{1.0F};
    /// The face this vertex belongs to, for directional shading.
    Direction facing{Direction::Up};
    /// The biome colour that multiplies this vertex, 0xRRGGBB. White for the
    /// quads that declare no tint index, which is most of them.
    u32 tint_colour{0xFFFFFF};
    /// Elements with `"shade": false` take no directional shading at all.
    bool        shade{true};
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

/// The largest light a vertex can carry: four samples at level 15.
inline constexpr u8 kMaxLightQuarters = 60;

/// Bit layout, low bit first. Word-aligned on purpose: no field straddles a
/// 32-bit word, so the vertex shader unpacks it with six bitfieldExtract calls
/// and no reassembly.
///
///     word 0    0-15   x        16 bits
///              16-31   y        16 bits
///     word 1    0-15   z        16 bits
///              16-31   u        16 bits   normalised atlas coordinate
///     word 2    0-15   v        16 bits
///              16-21   sky       6 bits   quarter levels, 0..60
///              22-27   block     6 bits   quarter levels, 0..60
///              28-30   facing    3 bits   0-5 a Direction, 6 unshaded
///              31      spare     1 bit
///     word 3    0-7    red       8 bits   the baked biome tint
///               8-15   green     8 bits
///              16-23   blue      8 bits
///              24-31   occlusion 8 bits   smooth-lighting brightness, 0..1
///
/// Two bits of light and six of occlusion more than the first layout, from
/// the spare byte and two spare bits: the first layout rounded the four-level
/// sum to a whole level and the brightness to one of four fixed steps, where
/// the game keeps quarter levels and averages shade brightnesses — and the
/// fixed steps were not even the game's four values
/// (docs/provenance/rendu-parite.md).
struct TerrainVertex {
    std::array<u32, 4> words{};

    friend bool operator==(const TerrainVertex&, const TerrainVertex&) noexcept = default;
};

static_assert(sizeof(TerrainVertex) == 16, "the vertex format is part of the memory budget");

[[nodiscard]] TerrainVertex pack_vertex(const TerrainVertexAttributes& attributes) noexcept;

/// Round-trips everything except the precision the packing deliberately drops.
[[nodiscard]] TerrainVertexAttributes unpack_vertex(const TerrainVertex& vertex) noexcept;

}  // namespace ov::render
