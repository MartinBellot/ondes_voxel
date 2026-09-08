// The terrain vertex, packed into eight bytes.
//
// Eight bytes is a budget, not a preference. A render distance of 12 chunks is
// roughly 600 sections, and a section of ordinary terrain meshes to a few
// thousand quads; at 16 bytes a vertex the working set stops fitting in the
// arena and every camera movement becomes an upload spike. The upload spike,
// not the steady-state frame rate, is what the milestone's p99 target is
// about.
//
// So every field below had to earn its bits, and two of them are compromises
// that are written down rather than discovered later:
//
//   * position is quantised to 1/64 of a block. The range is deliberately
//     wider than a section, because the model format lets an element overhang
//     its own block by a whole block in each direction.
//   * texture coordinates are in sprite space at 1/8 of a sprite unit, which
//     is 1/128 of a sprite. The scale is a power of two on purpose: 0 and 16
//     are the two commonest coordinates in the whole corpus — every full-face
//     quad uses them — and a scale that could not represent 16 exactly would
//     shrink every ordinary block face by a fraction of a texel.
//
// What is NOT here is a vertex colour. Vanilla multiplies the biome tint into
// it on the CPU; three more bytes would not fit. Instead `tint` names which of
// three tint channels applies — grass, foliage, water — and the shader looks
// the colour up. That is a real difference from vanilla's mechanism, chosen
// for the byte budget, and it is only correct as long as a quad's tint is
// constant across the quad, which for a single block it is.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/vec.hpp"

namespace ov::render {

/// Which biome-driven colour multiplies this vertex.
enum class TintChannel : u8 {
    None    = 0,
    Grass   = 1,
    Foliage = 2,
    Water   = 3,
};

/// A vertex before packing. Never stored in a buffer; it exists so that the
/// packing rules have one place to be stated and tested.
struct TerrainVertexAttributes {
    /// Section-local position in blocks. Must lie within
    /// [kPositionMin, kPositionMax].
    Vec3f position{};
    /// Sprite space, 0..16.
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
/// overhangs, with room left over. The scale is exactly 64, so every multiple
/// of 1/64 of a block — which is every coordinate an unrotated model can
/// produce — survives the round trip untouched. The consequence is that the
/// top of the range is one step short of 24, which costs nothing that a wider
/// range would have bought.
inline constexpr f32 kPositionMin   = -8.0F;
inline constexpr f32 kPositionScale = 64.0F;
inline constexpr f32 kPositionMax   = kPositionMin + 2047.0F / kPositionScale;

/// Sprite coordinate range and precision. 1/8 of a sprite unit, which puts 0,
/// 16 and every half unit a model writes on an exact step.
inline constexpr f32 kTexCoordScale = 8.0F;
inline constexpr f32 kTexCoordMax   = 255.0F / kTexCoordScale;

/// The `facing` code that means "no directional shading". It reuses the two
/// spare values of a three-bit direction field rather than costing a bit of
/// its own.
inline constexpr u8 kFacingUnshaded = 6;

/// Bit layout of the packed vertex, low bit first:
///
///     0-10   x        11 bits
///    11-21   y        11 bits
///    22-32   z        11 bits
///    33-40   u         8 bits   1/8 sprite unit
///    41-48   v         8 bits
///    49-52   sky      4 bits
///    53-56   block    4 bits
///    57-58   ao       2 bits
///    59-61   facing   3 bits   0-5 a Direction, 6 unshaded
///    62-63   tint     2 bits
struct TerrainVertex {
    u64 packed{0};

    friend bool operator==(TerrainVertex, TerrainVertex) noexcept = default;
};

static_assert(sizeof(TerrainVertex) == 8, "the whole point of this type is that it is 8 bytes");

[[nodiscard]] TerrainVertex pack_vertex(const TerrainVertexAttributes& attributes) noexcept;

/// Round-trips everything except the precision the packing deliberately drops.
[[nodiscard]] TerrainVertexAttributes unpack_vertex(TerrainVertex vertex) noexcept;

}  // namespace ov::render
