// The model of a thing that moves: a tree of bones, each carrying geometry.
//
// An entity model is not a block model and does not read like one. A block
// model is a JSON file in the resource pack with faces that name sprites in an
// atlas; an entity model is a hierarchy of parts cut from **one** texture whose
// layout is the unfolded net of those parts. Nothing in `assets/` describes it:
// in Java Edition the geometry is built by the client at run time. See
// docs/provenance/rendu-entites.md for where the numbers come from — since the
// second format, the baked ModelPart trees of the user's own 1.20.1 client,
// read while it runs — and for the oracles that check them.
//
// Coordinates, stated once because every sign error here is a mob rendered
// inside-out:
//
//   • **Model units are 1/16 of a block.**
//   • **+Y is up.** Format 1 (Bedrock) puts the feet at y = 0. Format 2 keeps
//     Java's own origin, which is *not* the feet: a living entity's model is
//     drawn 1.501 blocks above them and a minecart's 0.375 blocks. That offset
//     belongs to the renderer, not the layer, so it travels in
//     EntityPlacement::origin (entity_look.cpp says which species takes which).
//   • **−Z is the direction the entity faces**, +X is its **left**.
//   • A cube is a half-open box `[origin, origin + size]`; `inflate` grows it
//     by that many units on all six sides **without moving its texture**.
//   • A quad (format 2) is four corners already in the model's space, wound
//     **clockwise seen from outside**, because the model-to-world map is a
//     reflection that turns it the right way round — see entity_mesh.hpp.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/vec.hpp"

#include <array>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ov::render {

/// The most bones a model may have.
///
/// Not a guess: the largest model the game has is the horse at twenty-four
/// parts. The bound exists so that posing and meshing can use a stack array and
/// allocate nothing per frame; a model that exceeds it is refused by name rather
/// than truncated.
inline constexpr usize kMaxBones = 64;

/// One box, and where its net sits on the texture. Format 1 only.
struct EntityCube {
    /// Minimum corner, in model units.
    Vec3f origin{};
    /// Extent from `origin`. Always positive.
    Vec3f size{};
    /// Top-left of the box's net on the texture, in texels.
    f32 uv_u{0.0F};
    f32 uv_v{0.0F};
    /// Grown by this many units on every side; the net is unchanged.
    f32 inflate{0.0F};
    /// Left and right faces swapped, and the whole net read right to left.
    bool mirror{false};
};

/// One face exactly as the game draws it. Format 2.
///
/// The game's own polygons, not a box to unfold: whatever the game did with
/// mirroring, inflation or a zero-thickness plane is already in the corners and
/// the texture coordinates, so there is no rule here to get wrong.
struct EntityQuad {
    /// Corners in model units, in the model's space, before any bone moves.
    std::array<Vec3f, 4> position{};
    /// Texture coordinates in texels of the model's sheet.
    std::array<f32, 4> u{};
    std::array<f32, 4> v{};
    /// The outward normal the game gives the face.
    Vec3f normal{};
};

/// One bone: a pivot, a parent, and the geometry rigidly attached to it.
struct EntityBone {
    std::string name;
    /// The point this bone rotates about, in model units, in the *model's*
    /// space before any rotation — not the parent's. A child's pivot is
    /// absolute, like its geometry.
    Vec3f pivot{};
    /// Index into EntityModel::bones, or -1 for a root. Always smaller than
    /// this bone's own index: parse() refuses a model whose bones are not in
    /// parent-before-child order, so posing is one forward pass.
    i32 parent{-1};
    /// False for a bone whose own geometry is not drawn (a hinge, an attachment
    /// point, the game's `skipDraw`). Its children still are.
    bool render{true};
    /// False hides the bone **and everything under it**, as the game's
    /// `visible` does. A player model's cloak and ear are the example.
    bool visible{true};
    /// The rotation the bone has at rest, in degrees in this project's axes,
    /// composed Z·Y·X about the pivot. A cow's body lies at x = −90: its box is
    /// modelled standing and turned onto its legs by this.
    Vec3f rest_rotation{};
    /// Scale about the pivot, applied after the rotation.
    Vec3f rest_scale{1.0F, 1.0F, 1.0F};
    std::vector<EntityCube> cubes;
    std::vector<EntityQuad> quads;
};

/// A whole model: the bones, and the size of the texture their nets are cut
/// from. The texture itself is chosen per entity, not here, because one model
/// serves many skins.
struct EntityModel {
    std::string             name;
    f32                     texture_width{64.0F};
    f32                     texture_height{64.0F};
    std::vector<EntityBone> bones;

    /// Index of a bone by name, or -1. Linear: a model has a dozen bones and a
    /// map would be slower and one more allocation.
    [[nodiscard]] i32 bone(std::string_view bone_name) const noexcept;

    /// The box the model's geometry occupies with every bone at rest — rest
    /// rotations included — in blocks, in the model's own space (no renderer
    /// origin). entity_bounds() is the placed, posed counterpart.
    void rest_bounds(Vec3f& min, Vec3f& max) const noexcept;
};

enum class EntityModelError : u8 {
    /// The file is not JSON, or not this file's shape.
    Malformed,
    /// A bone names a parent that has not been declared yet, or at all.
    BadHierarchy,
    /// The format version is one this build does not know.
    UnknownFormat,
    /// The file was not where it was said to be.
    NotFound,
    /// More bones than kMaxBones.
    TooManyBones,
};

[[nodiscard]] std::string_view to_string(EntityModelError error) noexcept;

/// Every model this build knows how to draw, read from one file.
///
/// The file is **generated, never committed**: it is derived from Mojang data
/// and lives under `data/vanilla/1.20.1/`, which .gitignore excludes, exactly
/// like the data generator's output. `scripts/measure_entity_models.py` writes
/// it. A build without it draws no entities and says so once, rather than
/// inventing a shape.
class EntityModelSet {
public:
    /// The newest format this build reads: the Java client's own geometry.
    static constexpr i32 kFormat = 2;
    /// The Bedrock-derived format of the first entity work, still read so an
    /// old generated file keeps drawing what it can.
    static constexpr i32 kLegacyFormat = 1;

    [[nodiscard]] static std::expected<EntityModelSet, EntityModelError> parse(
        std::span<const u8> bytes);

    /// Read the generated file from the filesystem.
    ///
    /// Not through an AssetSource: this is not a resource pack asset. It is
    /// derived data that lives beside the registry pack, and pretending it were
    /// a pack file would put it on a search path a resource pack could shadow.
    [[nodiscard]] static std::expected<EntityModelSet, EntityModelError> load(
        const std::filesystem::path& path);

    [[nodiscard]] const EntityModel* find(std::string_view name) const noexcept;

    [[nodiscard]] std::span<const EntityModel> models() const noexcept { return models_; }

    /// What the generator refused to convert, and why, carried through so the
    /// client can say "no model for X" instead of drawing nothing silently.
    [[nodiscard]] std::span<const std::string> refused() const noexcept { return refused_; }

    /// Where the geometry came from, as the generator recorded it. Printed by
    /// the client at startup so a screenshot can be traced to a source.
    [[nodiscard]] std::string_view source() const noexcept { return source_; }

    /// The file's format: kFormat, or kLegacyFormat for a Bedrock-era file.
    [[nodiscard]] i32 format() const noexcept { return format_; }

private:
    std::vector<EntityModel> models_;
    std::vector<std::string> refused_;
    std::string              source_;
    i32                      format_{kFormat};
};

}  // namespace ov::render
