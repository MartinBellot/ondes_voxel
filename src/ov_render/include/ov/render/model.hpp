// Block models: the JSON on disk, resolved into something a mesher can bake.
//
// Vanilla's terrain is model-driven, not shape-driven. Every block state
// resolves through blockstates/<block>.json to a model reference, and that
// model resolves through a chain of `parent` files to a list of cuboid
// elements, each with up to six textured faces. A stair is not a special case
// in the renderer: it is three boxes in a JSON file.
//
// That is why this module exists before any Vulkan does. The whole pipeline —
// parent inheritance, texture variables, element rotation, uvlock — is pure
// data transformation, testable to the last quad without a GPU, and it is
// where the fidelity of the result is actually decided.
//
// See docs/PROVENANCE.md § « Modèles de blocs » for what is documented, what
// was measured against the 1.20.1 assets, and what is still open.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/vec.hpp"
#include "ov/render/asset_path.hpp"
#include "ov/render/asset_source.hpp"

#include <array>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ov::render {

enum class Axis : u8 { X, Y, Z };

[[nodiscard]] std::string_view axis_name(Axis axis) noexcept;

[[nodiscard]] std::optional<Axis> axis_from_name(std::string_view name) noexcept;

enum class ModelError {
    /// No such file in the pack stack.
    NotFound,
    /// The file is not JSON, or not shaped like a model.
    Malformed,
    /// The parent chain is longer than kMaxParentDepth, or loops.
    ParentChainTooLong,
};

[[nodiscard]] std::string_view to_string(ModelError error) noexcept;

/// The sprite a face falls back to when its texture variable never resolves.
///
/// Vanilla shows its checkerboard rather than dropping the face, and so do we:
/// a missing texture that is visible gets fixed, a missing texture that makes
/// the block invisible gets reported as "the block does not render".
inline constexpr std::string_view kMissingSprite = "minecraft:missingno";

/// One textured face of one element.
struct FaceDefinition {
    /// Resolved sprite name, e.g. `minecraft:block/oak_planks`. Texture
    /// variables (`#side`) are already chased; this never starts with '#'.
    std::string sprite;

    /// Texture region, `[x1, y1, x2, y2]` in 0..16 sprite units, v downward.
    /// Absent means "derive it from the element's own bounds" — see bake().
    std::optional<std::array<f32, 4>> uv;

    /// The neighbour that, if it has a full face here, hides this one.
    std::optional<Direction> cullface;

    /// Texture rotation in degrees clockwise: 0, 90, 180 or 270.
    i32 rotation{0};

    /// Which hardcoded tint applies, or -1 for none. Grass, foliage, water and
    /// redstone are the ones that use it; the colour comes from the biome.
    i32 tint_index{-1};
};

/// The optional per-element rotation. Not the blockstate's x/y: this one is
/// inside the model, arbitrary within its 22.5° steps, and is what makes a
/// cross model's two quads meet at 45°.
struct ElementRotation {
    Vec3f origin{8.0F, 8.0F, 8.0F};
    Axis  axis{Axis::Y};
    /// -45, -22.5, 0, 22.5 or 45 degrees.
    f32 angle{0.0F};
    /// Scale the face out so a rotated element still meets its neighbours.
    bool rescale{false};
};

/// One cuboid. Coordinates are in 0..16 units within the block, and the format
/// permits -16..32 so that a model may overhang its own block.
struct Element {
    Vec3f                                                      from{};
    Vec3f                                                      to{};
    std::optional<ElementRotation>                             rotation;
    bool                                                       shade{true};
    std::array<std::optional<FaceDefinition>, kDirectionCount> faces{};
};

/// One entry of a model's `display` block: where the model sits in a given
/// context — the hand, the head, the ground, a GUI cell.
///
/// Only `gui` is read today, and it is what makes a block appear in a hotbar
/// slot as a three-quarter cube rather than as a flat square: `block/block`
/// declares `rotation: [30, 225, 0]` and `scale: 0.625`, and every block model
/// in the game inherits it.
///
/// Translation is in blocks, already divided by the sixteen the file writes it
/// in. Rotation is in degrees, applied X then Y then Z.
struct DisplayTransform {
    Vec3f rotation{};
    Vec3f translation{};
    Vec3f scale{1.0F, 1.0F, 1.0F};
};

/// A model after its parent chain has been walked and every `#variable` has
/// been resolved. Nothing here refers to another file.
struct Model {
    std::vector<Element> elements;
    /// Whether smooth lighting applies. Declared by the *root* of the parent
    /// chain, not by the leaf — see PROVENANCE.
    bool ambient_occlusion{true};
    /// The sprite break particles take, or empty when the model declares none.
    std::string particle_sprite;

    /// `display.gui`, merged down the chain. Absent when nothing declared it,
    /// which for an item model means the identity — a flat icon.
    std::optional<DisplayTransform> gui_display;

    /// `gui_light: front`. The item is lit flat rather than as a solid, which
    /// is what every generated icon asks for.
    bool gui_light_front{false};

    /// The chain ended at `builtin/generated`: this model has no elements of
    /// its own and is drawn from `layers` instead.
    bool generated{false};

    /// `layer0`, `layer1`, … resolved to sprite names, in order. Only a
    /// generated model has them; a block model's textures are on its faces.
    std::vector<std::string> layers;
};

/// Loads models and resolves them. Caches by resource location, because a
/// world of oak planks resolves `block/cube_all` a few thousand times.
class ModelLoader {
public:
    /// The chain `block/oak_stairs` -> `block/stairs` -> `block/block` is three
    /// long; sixteen is beyond anything vanilla or a pack does, and turns a
    /// cyclic parent into an error instead of a hang.
    static constexpr u32 kMaxParentDepth = 16;

    explicit ModelLoader(const AssetSource& source);
    ModelLoader(const ModelLoader&)            = delete;
    ModelLoader& operator=(const ModelLoader&) = delete;
    ModelLoader(ModelLoader&&) noexcept;
    ModelLoader& operator=(ModelLoader&&) noexcept;
    ~ModelLoader();

    /// Resolved model, or why it could not be. The pointer is owned by the
    /// loader and stays valid until it is destroyed.
    [[nodiscard]] std::expected<const Model*, ModelError> load(const ResourceLocation& location);

    [[nodiscard]] usize cached_models() const noexcept;

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

}  // namespace ov::render
