// Turning baked models into packed vertices.
//
// Emission is per face, from the model. It is NOT a greedy mesher, and the
// reasoning is written out in docs/PROVENANCE.md: greedy fusion assumes blocks
// are cubes and lighting is flat across a face, and vanilla is neither. Fusing
// two faces destroys the vertices between them, and those vertices are exactly
// where the per-vertex ambient occlusion and smooth light live.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/vec.hpp"
#include "ov/render/atlas.hpp"
#include "ov/render/baked_model.hpp"
#include "ov/render/terrain_vertex.hpp"

#include <vector>

namespace ov::render {

/// Vanilla's four terrain layers, each with its own pipeline state.
///
/// The split is not cosmetic. Cutout discards fragments and so cannot write
/// depth early; translucent must be drawn last, back to front, and sorted again
/// whenever the camera moves far enough. Drawing everything in one pass gets
/// glass right and leaves leaves wrong, or the reverse.
enum class RenderLayer : u8 {
    Solid,
    Cutout,
    CutoutMipped,
    Translucent,
    Count,
};

[[nodiscard]] std::string_view to_string(RenderLayer layer) noexcept;

/// What the mesher needs to know about the blocks around the one it is
/// emitting.
///
/// An interface rather than a chunk, so that the mesher can be tested against a
/// handful of blocks stated inline, and so that ov_render does not have to know
/// how a world stores them.
class NeighbourhoodView {
public:
    NeighbourhoodView()                                    = default;
    NeighbourhoodView(const NeighbourhoodView&)            = default;
    NeighbourhoodView(NeighbourhoodView&&)                 = default;
    NeighbourhoodView& operator=(const NeighbourhoodView&) = default;
    NeighbourhoodView& operator=(NeighbourhoodView&&)      = default;
    virtual ~NeighbourhoodView();

    /// Does the block at `position` present a full face on the side facing
    /// `towards`, so that a quad declaring `cullface = towards` is hidden?
    ///
    /// The registry answers this exactly, from the per-state collision shapes
    /// and their precomputed sturdy-face mask. Guessing it from "is the block
    /// opaque" is how a slab ends up hiding the ground under it.
    [[nodiscard]] virtual bool occludes(Vec3i position, Direction towards) const = 0;

    /// Does the block at `position` cast ambient occlusion onto its
    /// neighbours? A full cube does; a torch does not.
    [[nodiscard]] virtual bool casts_ambient_occlusion(Vec3i position) const = 0;

    [[nodiscard]] virtual u8 sky_light(Vec3i position) const   = 0;
    [[nodiscard]] virtual u8 block_light(Vec3i position) const = 0;

    /// Which fluid fills this position, or 0 for none.
    ///
    /// Fluids are the one thing vanilla does not draw from a model — water.json
    /// declares a particle texture and no geometry at all — so the renderer
    /// synthesises their boxes, and two touching boxes of the same fluid must
    /// not draw the face between them or an ocean becomes a grid of cubes.
    /// Opacity cannot answer it: water is transparent, so the ordinary
    /// occlusion test correctly refuses to hide anything behind it.
    [[nodiscard]] virtual u16 fluid_at(Vec3i position) const;
};

/// Vertices per render layer, ready to upload. Four vertices per quad, wound
/// outward; the indices come from the shared static index buffer.
struct MeshBuffers {
    std::vector<TerrainVertex> layers[static_cast<usize>(RenderLayer::Count)];

    [[nodiscard]] std::vector<TerrainVertex>& operator[](RenderLayer layer) {
        return layers[static_cast<usize>(layer)];
    }

    [[nodiscard]] const std::vector<TerrainVertex>& operator[](RenderLayer layer) const {
        return layers[static_cast<usize>(layer)];
    }

    [[nodiscard]] usize total_vertices() const noexcept;

    void clear();
};

/// How one block's model should be emitted.
struct BlockRenderInfo {
    RenderLayer layer{RenderLayer::Solid};
    /// Which biome colour multiplies the quads that declare a tint index.
    TintChannel tint{TintChannel::None};
    /// Non-zero when this block is a fluid, identifying which one.
    u16 fluid{0};
};

/// Emit one block's baked model at `block_position`, in section-local
/// coordinates.
///
/// Quads whose cullface points at an occluding neighbour are dropped, which is
/// the single biggest reduction in a solid world — the inside of a hill emits
/// nothing at all.
void emit_block(const BakedModel& model, Vec3i block_position, const BlockRenderInfo& info,
                const TextureAtlas& atlas, const NeighbourhoodView& view, MeshBuffers& out);

/// The shared static index buffer: `0,1,2, 0,2,3` per quad, repeated.
///
/// One buffer for the whole terrain, bound once. Every section draws from it
/// with its own vertexOffset, which removes every byte of per-section index
/// memory — at four bytes an index and six indices a quad, that is 24 bytes per
/// quad that never has to be allocated, uploaded or evicted.
[[nodiscard]] std::vector<u32> build_shared_quad_indices(u32 quad_count);

}  // namespace ov::render
