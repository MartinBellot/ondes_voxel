// blockstates/<block>.json: which model, or models, a block state draws.
//
// Two shapes exist and only two. `variants` maps a property string to one
// model; `multipart` lists model pieces, each with a condition, and a state
// draws every piece whose condition holds. A fence is multipart — post, then
// one side piece per connected neighbour — and a stair is variants, with all
// 80 combinations written out.
#pragma once

#include "ov/base/types.hpp"
#include "ov/render/asset_path.hpp"
#include "ov/render/model.hpp"

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ov::render {

/// One model reference with the rotation the blockstate applies to it.
struct ModelVariant {
    ResourceLocation model;
    /// Rotation about the X axis, in whole steps of 90 degrees.
    i32 x{0};
    /// Rotation about the Y axis, in whole steps of 90 degrees.
    i32 y{0};
    /// Keep the texture aligned to the world rather than rotating it with the
    /// model. What makes a rotated fence gate's planks still run horizontally.
    bool uvlock{false};
    /// Relative weight when a variant lists alternatives.
    i32 weight{1};

    friend bool operator==(const ModelVariant&, const ModelVariant&) = default;
};

/// One slot of the model list a state resolves to.
///
/// `alternatives` usually holds one entry. When a blockstate offers several
/// randomised models — grass, sand, stone all do — it holds them all, weights
/// included, and the choice is left to the caller. That is deliberate: vanilla
/// picks by hashing the block position, so the same block must pick the same
/// model every time it is re-meshed, and only the mesher knows the position.
struct VariantGroup {
    std::vector<ModelVariant> alternatives;
};

/// A single `name=value` requirement. `values` holds the alternatives of a
/// `north=side|up` clause, so one test is a membership check.
struct PropertyTest {
    std::string              name;
    std::vector<std::string> values;
};

/// A multipart `when`. A disjunction of conjunctions: `any_of` is the OR, each
/// inner vector is the AND. An empty `any_of` means the piece always applies.
struct MultipartCondition {
    std::vector<std::vector<PropertyTest>> any_of;

    [[nodiscard]] bool always() const noexcept { return any_of.empty(); }
};

/// One `{ when, apply }` entry of a multipart file.
struct MultipartCase {
    MultipartCondition condition;
    VariantGroup       group;
};

/// One `"facing=east,half=top": { ... }` entry of a variants file.
struct VariantCase {
    /// The requirements the key spells out. Empty for the `""` key, which
    /// matches every state — the shape a one-model block like stone uses.
    std::vector<PropertyTest> requirements;
    VariantGroup              group;
};

/// The state properties of a block, as name/value pairs.
using PropertyList = std::span<const std::pair<std::string_view, std::string_view>>;

/// A parsed blockstates/<block>.json.
class BlockStateFile {
public:
    [[nodiscard]] static std::expected<BlockStateFile, ModelError> parse(std::span<const u8> bytes);

    [[nodiscard]] bool is_multipart() const noexcept { return !multipart_.empty(); }

    /// Every model group this state draws.
    ///
    /// Variants yield at most one group; multipart yields one per matching
    /// case, in file order, which is the order vanilla draws them in.
    [[nodiscard]] std::vector<VariantGroup> select(PropertyList properties) const;

    [[nodiscard]] std::span<const VariantCase> variants() const noexcept { return variants_; }

    [[nodiscard]] std::span<const MultipartCase> multipart() const noexcept { return multipart_; }

private:
    std::vector<VariantCase>   variants_;
    std::vector<MultipartCase> multipart_;
};

/// Does a state satisfy a multipart condition? Exposed because it is the part
/// with the interesting edge cases, and it is worth testing on its own.
[[nodiscard]] bool condition_matches(const MultipartCondition& condition, PropertyList properties);

}  // namespace ov::render
