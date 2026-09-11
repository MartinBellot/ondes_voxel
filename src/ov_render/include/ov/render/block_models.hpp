// From a block state id to the quads that draw it.
//
// This is the join between the registry, which knows what a state *is*, and the
// model pipeline, which knows what it looks like. It is the last piece of
// scaffolding the demo scene was standing in for: with this, anything the
// registry can name can be drawn, and the renderer stops needing a hand-written
// palette.
//
// Resolution is lazy. A region of the world touches a few hundred of the 24135
// states, and baking all of them costs about forty megabytes of quads to answer
// questions nobody asked.
#pragma once

#include "ov/base/types.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/render/asset_source.hpp"
#include "ov/render/atlas.hpp"
#include "ov/render/baked_model.hpp"
#include "ov/render/mesher.hpp"
#include "ov/render/position_random.hpp"

#include <memory>
#include <string>
#include <vector>

namespace ov::render {

/// Everything needed to draw one block state.
struct BlockRender {
    /// The quads, with every matching multipart piece already concatenated.
    BakedModel  model;
    RenderLayer layer{RenderLayer::Solid};
    TintChannel tint{TintChannel::None};
    /// Non-zero when this state is a fluid, identifying which one. Fluids have
    /// no model in the assets and their geometry is synthesised here.
    u16 fluid{0};
    /// False for air, and for a state whose model could not be resolved.
    bool drawable{false};
    // ── render-parity ── a blockstate that lists weighted alternatives —
    // stone plain and mirrored, grass turned four ways — keeps every one of
    // them, baked, with its weight; `model` is the first. The mesher picks by
    // the block's position (position_random.hpp), as the game does, so that a
    // field of grass is turned the way the game turns it and not all one way.
    std::vector<BakedModel> alternatives;
    std::vector<i32>        weights;
    /// Plants nudged off their block's centre per position
    /// (position_random.hpp). From a table of measured blocks; None for the
    /// rest, which is right for every full block and wrong, visibly, for any
    /// plant the table does not name yet.
    OffsetType offset{OffsetType::None};
    f32        max_offset{0.25F};
    f32        max_vertical_offset{0.2F};
};

class BlockModelCache {
public:
    BlockModelCache(const AssetSource& source, const registry::BlockRegistry& blocks);
    BlockModelCache(const BlockModelCache&)            = delete;
    BlockModelCache& operator=(const BlockModelCache&) = delete;
    BlockModelCache(BlockModelCache&&) noexcept;
    BlockModelCache& operator=(BlockModelCache&&) noexcept;
    ~BlockModelCache();

    /// Resolve a state, baking it the first time it is asked for. The reference
    /// stays valid for the life of the cache.
    [[nodiscard]] const BlockRender& resolve(registry::BlockStateId state);

    /// Every distinct sprite named by everything resolved so far, for the
    /// atlas builder. The atlas has to exist before meshing, so the order is:
    /// resolve the states a world uses, stitch, classify, mesh.
    [[nodiscard]] const std::vector<std::string>& sprites() const noexcept;

    /// Decide each resolved state's render layer from its sprites' alpha.
    ///
    /// Vanilla keeps this in a hand-written table in Java. Reading it out of
    /// the pixels instead is data-driven and agrees with vanilla on everything
    /// that matters: fully opaque is solid, hard-edged alpha is cutout, and
    /// partial alpha is translucent. See docs/PROVENANCE.md for the one case it
    /// cannot recover — the split between `cutout` and `cutout_mipped`, which
    /// is a rendering choice and not a property of the texture.
    void classify_layers(const TextureAtlas& atlas);

    [[nodiscard]] usize resolved_count() const noexcept;

    /// States that resolved to no geometry at all, worth reporting: a block
    /// that silently draws nothing looks exactly like a bug in the mesher.
    [[nodiscard]] usize missing_count() const noexcept;

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

/// Which biome colour a block's tinted faces take.
///
/// Vanilla hardcodes this per block in Java, so there is nothing to read it
/// from; the table is written out and its shape — grass, foliage, water — is
/// what the vertex's two tint bits encode.
[[nodiscard]] TintChannel tint_channel_for(std::string_view block_name) noexcept;

}  // namespace ov::render
