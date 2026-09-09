// An item, as it appears in a sixteen-by-sixteen cell.
//
// There are two kinds of item icon in Minecraft and they have almost nothing in
// common. A sword is a *flat* picture: its model's chain ends at
// `builtin/generated` and its `layer0` texture is drawn as a square. A block is
// a *model*: `item/stone` is nothing but `"parent": "minecraft:block/stone"`,
// and what is drawn in the slot is the block itself, turned by the `gui`
// display transform its chain inherits from `block/block` — thirty degrees
// about X, two hundred and twenty-five about Y, five eighths of the size.
//
// So this module resolves an item name to one of those two things, and for the
// second it does the projection: model space to the cell's own pixels, with
// back faces dropped and what is left sorted so that the near ones are drawn
// last. There is no depth buffer in the interface — an item is a handful of
// quads and a painter's sort is cheaper than a second attachment — and the
// cost of that choice is named in docs/provenance/interface.md.
//
// Nothing here touches a GPU. That is what lets the geometry be a unit test
// instead of a screenshot: a cube in a GUI cell has three visible faces, the
// top one is a rhombus centred on the cell, and every one of those is a claim a
// test can check.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/vec.hpp"
#include "ov/render/asset_source.hpp"
#include "ov/render/atlas.hpp"

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ov::render {

/// A point in the cell's own pixels. ov_math has no two-component vector and
/// this is the only place that wants one, so it is spelled out rather than
/// added to a header every module includes.
struct ItemPoint {
    f32 x{0.0F};
    f32 y{0.0F};
};

/// One quad of a three-dimensional item, already flattened into the cell.
struct ItemQuad {
    /// The four corners, in GUI pixels, with (0, 0) at the cell's top-left and
    /// y downward — the interface's own axes, not the model's.
    std::array<ItemPoint, 4> position{};
    /// The matching texture coordinates in the atlas, 0..1.
    std::array<ItemPoint, 4> uv{};
    /// View depth of the quad's centre. Larger is nearer the viewer, so drawing
    /// in ascending order puts the near faces on top.
    f32 depth{0.0F};
    /// Vanilla's flat per-face multiplier: 1 for the top, 0.8 north and south,
    /// 0.6 east and west, 0.5 for the bottom.
    ///
    /// ⚠️ This is the *terrain's* directional shading, not the two-light rig
    /// vanilla sets up for items in a GUI. It produces a cube that reads
    /// correctly and it is not the same numbers; the difference is stated
    /// rather than hidden. See docs/provenance/interface.md.
    f32 shade{1.0F};
    /// Which hardcoded tint the face takes, or -1. Grass and foliage in a
    /// hotbar are tinted, and an untinted grass block icon is grey.
    i32 tint_index{-1};
};

/// What one item looks like.
struct ItemMesh {
    /// True when the model is a flat icon built from `layers`.
    bool flat{false};
    /// The atlas rects of a flat icon, back to front. Usually one; a potion or
    /// a spawn egg has two. Baked rather than named, because the caller draws
    /// them and has no business resolving a sprite.
    std::vector<SpriteUv> layers;
    /// The quads of a solid icon, already culled and sorted back to front.
    std::vector<ItemQuad> quads;
    /// False when nothing could be resolved: no model file, or a model with no
    /// geometry and no layers.
    bool drawable{false};
};

/// The size of a GUI item cell, in GUI pixels. Vanilla's, and the unit every
/// position in an ItemQuad is expressed in.
inline constexpr f32 kItemCellSize = 16.0F;

/// Resolves item names to meshes, in two phases.
///
/// The atlas cannot be stitched until every sprite is known, and a mesh's
/// texture coordinates cannot be computed until the atlas exists. So: `resolve`
/// every item the client may draw, hand `sprites()` to the AtlasBuilder, then
/// `bake` against the finished atlas. The same shape as BlockModelCache, for
/// the same reason.
class ItemModelCache {
public:
    explicit ItemModelCache(const AssetSource& source);
    ItemModelCache(const ItemModelCache&)            = delete;
    ItemModelCache& operator=(const ItemModelCache&) = delete;
    ItemModelCache(ItemModelCache&&) noexcept;
    ItemModelCache& operator=(ItemModelCache&&) noexcept;
    ~ItemModelCache();

    /// Read `models/item/<name>.json` and remember the sprites it needs.
    /// `name` may be `minecraft:stone` or `stone`.
    void resolve(std::string_view name);

    /// Every sprite named by everything resolved so far, sorted and unique.
    [[nodiscard]] const std::vector<std::string>& sprites() const noexcept;

    /// Turn every resolved item into a mesh against a finished atlas.
    void bake(const TextureAtlas& atlas);

    /// Null when the item was never resolved. Valid until the cache dies.
    [[nodiscard]] const ItemMesh* mesh(std::string_view name) const noexcept;

    [[nodiscard]] usize resolved_count() const noexcept;
    /// Items whose model file was missing or held no geometry at all.
    [[nodiscard]] usize missing_count() const noexcept;

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

}  // namespace ov::render
