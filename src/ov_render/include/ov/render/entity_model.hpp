// The model of a thing that moves: a tree of bones, each carrying boxes.
//
// An entity model is not a block model and does not read like one. A block
// model is a JSON file in the resource pack with faces that name sprites in an
// atlas; an entity model is a hierarchy of boxes cut from **one** texture whose
// layout is the unfolded net of those boxes. Nothing in `assets/` describes it:
// in Java Edition the geometry lives in code, which is why this project cannot
// read it from the game it targets and had to establish it another way. See
// docs/provenance/rendu-entites.md for where the numbers come from and for the
// oracle that checks them.
//
// Coordinates, stated once because every sign error here is a mob rendered
// inside-out:
//
//   • **Model units are 1/16 of a block.** A cube of size 8 is half a block.
//   • **+Y is up, and y = 0 is the ground the entity stands on.** A bone pivot
//     of 24 is a block and a half above the feet.
//   • **−Z is the direction the entity faces**, +X is its **left**. The head of
//     a cow sits at z = −14 for exactly this reason.
//   • A cube is a half-open box `[origin, origin + size]`; `inflate` grows it
//     by that many units on all six sides **without moving its texture**, which
//     is how the same 8×8×8 net draws both a head and the hat over it.
//
// The transform from model space to the world is a **reflection**, not a
// rotation — see entity_mesh.hpp, which is where that is paid for.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/vec.hpp"

#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ov::render {

/// The most bones a model may have.
///
/// Not a guess: the largest model this build carries is the spider at eleven,
/// and the largest in vanilla is well under this. The bound exists so that
/// posing and meshing can use a stack array and allocate nothing per frame; a
/// model that exceeds it is refused by name rather than truncated.
inline constexpr usize kMaxBones = 64;

/// One box, and where its net sits on the texture.
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
    /// This is what lets one arm's texture serve both arms.
    bool mirror{false};
};

/// One bone: a pivot, a parent, and the boxes rigidly attached to it.
struct EntityBone {
    std::string name;
    /// The point this bone rotates about, in model units, in the *model's*
    /// space — not the parent's. A child's pivot is absolute, like its cubes.
    Vec3f pivot{};
    /// Index into EntityModel::bones, or -1 for a root. Always smaller than
    /// this bone's own index: load() refuses a model whose bones are not in
    /// parent-before-child order, so posing is one forward pass.
    i32 parent{-1};
    /// False for a bone that exists only to be a hinge or an attachment point.
    /// Its cubes, if any, are not drawn — the `hat` layer of a zombie is the
    /// example, and drawing it would put a second head inside the first.
    bool render{true};
    std::vector<EntityCube> cubes;
};

/// A whole model: the bones, and the size of the texture their nets are cut
/// from. The texture itself is named per entity type, not here, because one
/// model serves many skins.
struct EntityModel {
    std::string             name;
    f32                     texture_width{64.0F};
    f32                     texture_height{64.0F};
    std::vector<EntityBone> bones;

    /// Index of a bone by name, or -1. Linear: a model has a dozen bones and a
    /// map would be slower and one more allocation.
    [[nodiscard]] i32 bone(std::string_view bone_name) const noexcept;

    /// The box the model occupies with every bone in its rest pose, in blocks,
    /// relative to the entity's feet. This is what is compared against the
    /// type's measured collision box — see docs/provenance/rendu-entites.md.
    void rest_bounds(Vec3f& min, Vec3f& max) const noexcept;
};

enum class EntityModelError : u8 {
    /// The file is not JSON, or not this file's shape.
    Malformed,
    /// A bone names a parent that has not been declared yet, or at all.
    BadHierarchy,
    /// The pack format version is one this build does not know.
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
    /// The one format version this build reads.
    static constexpr i32 kFormat = 1;

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

private:
    std::vector<EntityModel> models_;
    std::vector<std::string> refused_;
    std::string              source_;
};

}  // namespace ov::render
