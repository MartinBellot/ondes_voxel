// Walking: the angles a model's bones take from how far the thing has walked.
//
// Vanilla's limb swing is not a clock. It is a **distance**: the model asks how
// far the entity has walked, not how long it has lived, which is why a mob
// pushed sideways by a piston does not paddle its legs and why two mobs moving
// at different speeds never march in step. Every formula below is of the form
// `cos(distance * 38.17°) * amplitude * amount`.
//
// The 38.17° is exact and worth recognising: it is 0.6662 radians, the constant
// that appears in every one of these. A full stride is therefore
// 360 / 38.17 = 9.43 units of accumulated swing.
//
// Where these numbers come from: Mojang publishes the vanilla animations as
// data for Bedrock Edition, and the ones for these species are stated there in
// degrees — `math.cos(query.anim_time * 38.17) * 80.0` for a quadruped's legs,
// and so on. docs/provenance/rendu-entites.md records which file, which
// expression, and the two places where that source and Java Edition may
// disagree. Nothing here was eyeballed against a screenshot.
//
// No GPU and no allocation: this fills a caller's vector of angles, so it is a
// unit test rather than a screenshot.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/vec.hpp"
#include "ov/render/entity_model.hpp"

#include <string_view>
#include <vector>

namespace ov::render {

/// Which set of formulas a species walks by.
///
/// Deliberately not derived from the model's bone names: a model with a bone
/// called `leg0` might be a chicken or a cow, and guessing would animate one as
/// the other. A type this build has no animation for gets `Static` — drawn in
/// its rest pose, which is visibly a standing mob rather than a wrong one.
enum class EntityAnimation : u8 {
    Static,
    /// Two arms, two legs: player, zombie, skeleton.
    Humanoid,
    /// Four legs in two pairs: cow, pig, sheep.
    Quadruped,
    /// Four legs, and the diagonal pairing of a quadruped.
    Creeper,
    /// Eight legs, each with a rest angle of its own.
    Spider,
    /// Two legs, a body lying on its back, and wings.
    Chicken,
};

/// The animation for a `minecraft:`-qualified entity type name, or Static.
[[nodiscard]] EntityAnimation entity_animation(std::string_view type_name) noexcept;

/// The model name for a `minecraft:`-qualified entity type, or empty.
///
/// One place, so that adding a species is one line and a missing one is a
/// lookup that fails loudly instead of a model chosen by string surgery.
[[nodiscard]] std::string_view entity_model_name(std::string_view type_name) noexcept;

/// The texture, as a resource location without the `textures/` prefix or the
/// `.png` suffix, or empty.
[[nodiscard]] std::string_view entity_texture_name(std::string_view type_name) noexcept;

/// Everything the formulas read.
struct WalkState {
    /// Accumulated walk distance, in vanilla's units. See the header comment.
    f32 swing{0.0F};
    /// How much of the amplitude to apply, 0 standing still to 1 running.
    f32 amount{0.0F};
    /// Where the head points relative to the body, in degrees.
    f32 head_yaw{0.0F};
    f32 head_pitch{0.0F};
};

/// Accumulate one tick of walking.
///
/// `horizontal_distance` is how far the entity moved on the xz plane during the
/// tick. The two constants are the one pair in this file that no published
/// source states; they are named in the provenance document rather than
/// presented as measured.
void advance_walk(WalkState& state, f32 horizontal_distance) noexcept;

/// The rotation of one bone, in degrees, applied about its own pivot in
/// x-then-y-then-z order.
struct BonePose {
    Vec3f rotation{};
    /// Extra translation in model units, applied after the rotation. Only the
    /// chicken uses it today; it exists because a bone that both rotates and
    /// slides is one call rather than two conventions.
    Vec3f offset{};
};

/// Fill `out` with one pose per bone of `model`.
///
/// `out` is resized rather than returned so that a frame drawing a hundred mobs
/// reuses one vector. A bone the animation says nothing about keeps its rest
/// pose, which is all zeroes.
void pose_model(const EntityModel& model, EntityAnimation animation, const WalkState& state,
                std::vector<BonePose>& out);

}  // namespace ov::render
