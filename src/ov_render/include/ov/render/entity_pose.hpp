// Walking: the angles a model's bones take from how far the thing has walked.
//
// Vanilla's limb swing is not a clock. It is a **distance**: the model asks how
// far the entity has walked, not how long it has lived, which is why a mob
// pushed sideways by a piston does not paddle its legs and why two mobs moving
// at different speeds never march in step. Every walk formula below is of the
// form `cos(distance × 38.17°) × amplitude × amount`.
//
// Where these numbers come from: Mojang publishes the vanilla animations as
// data for Bedrock Edition, in degrees — `math.cos(query.anim_time * 38.17) *
// 80.0` for a quadruped's legs, and so on. docs/provenance/rendu-entites.md
// records which file and which expression. Since the models are the Java
// client's own (entity_model.hpp), bones are named as Java names them
// (`right_hind_leg`, not Bedrock's `leg0`).
//
// ── The sign ────────────────────────────────────────────────────────────────
//
// Both Java's `xRot = cos(…) × 1.4` and Bedrock's published `cos(…) × 80` are
// written in the game's model space, y down. This project's model space is y
// up, and a rotation seen through that reflection keeps its angle about Y and
// changes sign about X and Z. The formulas below are written in the game's
// numbers and converted in one place (`game_rotation`), so that each reads as
// its source does. The first version of this file applied the published
// numbers unconverted: legs swing symmetrically so nobody saw it, but every
// head looked up when the game's looked down.
//
// No GPU and no allocation once warm: this fills a caller's vector of angles,
// so it is a unit test rather than a screenshot.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/vec.hpp"
#include "ov/render/entity_model.hpp"

#include <array>
#include <string_view>
#include <vector>

namespace ov::render {

/// Which set of formulas a species moves by.
///
/// Deliberately not derived from the model's bone names: a model with a bone
/// called `right_hind_leg` might be a cow or a wolf, and guessing would animate
/// one as the other. A type with no animation gets `Static` — drawn in its rest
/// pose, which is visibly a standing mob rather than a wrong one.
enum class EntityAnimation : u8 {
    Static,
    /// Two arms, two legs, arms swinging: player, skeleton, piglin.
    Humanoid,
    /// A humanoid whose arms are held out in front: zombie and its kin.
    ZombieArms,
    /// A humanoid whose limbs are clamped short: the enderman.
    Enderman,
    /// Arms folded, legs swinging at half the humanoid's amplitude.
    Villager,
    /// Four legs in two diagonal pairs at 1.4 rad: cow, pig, sheep, creeper,
    /// horse, fox, wolf, hoglin.
    Quadruped,
    /// The same pairs at 1 rad, the cat's and the ocelot's.
    Feline,
    /// Eight legs, each with a rest angle of its own.
    Spider,
    /// Two legs; beak and wattle follow the head.
    Chicken,
    /// A boat's two oars, at rest.
    Boat,
    /// Twelve rods orbiting a head, on the entity's age.
    Blaze,
    /// Nine tentacles waving, on the entity's age.
    Ghast,
    /// A glass cube spinning and bobbing over its base.
    EndCrystal,
    /// Rotations read from the entity's metadata, one per limb.
    ArmorStand,
};

/// An armour stand's pose when the server sends none of its six limbs, in the
/// metadata's order (head, body, left arm, right arm, left leg, right leg),
/// degrees: the defaults the wiki lists for the `Pose` tag — which the game's
/// own vertices for a stand summoned bare confirm (docs/provenance,
/// rendu-entites.md § 9).
inline constexpr std::array<std::array<f32, 3>, 6> kArmorStandRestPose{{
    {0.0F, 0.0F, 0.0F},
    {0.0F, 0.0F, 0.0F},
    {-10.0F, 0.0F, -10.0F},
    {-15.0F, 0.0F, 10.0F},
    {-1.0F, 0.0F, -1.0F},
    {1.0F, 0.0F, 1.0F},
}};

/// Everything the formulas read.
struct WalkState {
    /// Accumulated walk distance, in vanilla's units. See the header comment.
    f32 swing{0.0F};
    /// How much of the amplitude to apply, 0 standing still to 1 running.
    f32 amount{0.0F};
    /// Where the head points relative to the body, in degrees: yaw positive
    /// turning the way the entity's own yaw grows, pitch positive looking down.
    f32 head_yaw{0.0F};
    f32 head_pitch{0.0F};
    /// Ticks since the client first saw the entity, fractional between ticks.
    /// Drives what the game animates on time rather than on distance.
    f32 age_ticks{0.0F};
    /// A zombie that is attacking raises its arms higher.
    bool aggressive{false};
    /// An armour stand's six limb rotations, head/body/left arm/right arm/left
    /// leg/right leg, in degrees as the metadata carries them.
    std::array<Vec3f, 6> stand{};
    /// A baby: the head is drawn larger relative to the body. The factor and
    /// the offset come from the species (entity_look.hpp).
    f32   baby_head_scale{1.0F};
    Vec3f baby_head_offset{};
};

/// Accumulate one tick of walking.
///
/// `horizontal_distance` is how far the entity moved on the xz plane during the
/// tick. The two constants are the one pair in this file that no published
/// source states; they are named in the provenance document rather than
/// presented as measured.
void advance_walk(WalkState& state, f32 horizontal_distance) noexcept;

/// What an animation does to one bone.
struct BonePose {
    /// Degrees, **added** to the bone's rest rotation, in this project's axes.
    Vec3f rotation{};
    /// Model units, applied after the rotation.
    Vec3f offset{};
    /// Multiplied into the bone's rest scale.
    Vec3f scale{1.0F, 1.0F, 1.0F};
    /// Hide this bone and everything under it.
    bool hidden{false};
};

/// A rotation written in the game's numbers (Java's y-down model space, or
/// Bedrock's published animations, in degrees), in this project's axes.
[[nodiscard]] constexpr Vec3f game_rotation(f32 x, f32 y, f32 z) noexcept {
    return Vec3f{-x, y, -z};
}

/// Where a dragon has been: its height and heading over the last ticks,
/// newest first. The game draws its neck and tail following this history, so a
/// turning dragon's tail lags behind its head.
struct DragonHistory {
    static constexpr usize kTicks = 32;
    std::array<f32, kTicks> y{};
    std::array<f32, kTicks> yaw{};
    usize                   count{0};

    /// Record one tick. The oldest entry falls off the end.
    void push(f32 height, f32 heading) noexcept {
        for (usize index = kTicks - 1; index > 0; --index) {
            y[index]   = y[index - 1];
            yaw[index] = yaw[index - 1];
        }
        y[0]   = height;
        yaw[0] = heading;
        count  = count < kTicks ? count + 1 : kTicks;
    }

    /// The entry `ticks` ago, or the oldest known one before there are enough.
    [[nodiscard]] f32 y_at(usize ticks) const noexcept {
        return count == 0 ? 0.0F : y[ticks < count ? ticks : count - 1];
    }
    [[nodiscard]] f32 yaw_at(usize ticks) const noexcept {
        return count == 0 ? 0.0F : yaw[ticks < count ? ticks : count - 1];
    }
};

/// Fill `out` with one pose per bone of `model`.
///
/// `out` is resized rather than returned so that a frame drawing a hundred mobs
/// reuses one vector. A bone the animation says nothing about keeps its rest
/// pose.
void pose_model(const EntityModel& model, EntityAnimation animation, const WalkState& state,
                std::vector<BonePose>& out);

/// One placement of the dragon's `neck` bone: the game draws that one part
/// five times as the neck and twelve times as the tail.
struct DragonSegment {
    /// Where the bone's pivot goes, in this project's model space.
    Vec3f pivot{};
    /// Its rotation, degrees, this project's axes.
    Vec3f rotation{};
};

struct DragonPose {
    std::array<DragonSegment, 5>  neck{};
    DragonSegment                 head{};
    std::array<DragonSegment, 12> tail{};
    /// The body's bob with the wing beat, in model units upwards.
    f32 bob{0.0F};
};

/// The ender dragon: wings, legs and jaw from the wing beat; the head, the neck
/// and the tail following the flight history.
///
/// Every number is the one Mojang publishes for the dragon in
/// `ender_dragon.entity.json` (pre-animation scripts) and
/// `ender_dragon.animation.json` — the neck's 10-unit segments from
/// (0, −20, −12), their 25.79° phase and 8.6° amplitude, the tail's from
/// (0, −10, 60) turned 180°, and the history offsets (ticks 5 to 0 for the
/// neck, 11 to 23 for the tail). `flap` is the wing beat as a fraction of a
/// cycle. `body` receives the poses of the whole model with the `neck` bone
/// hidden; the segments are drawn as separate placements of it.
void pose_dragon(const EntityModel& model, f32 flap, const DragonHistory& history, bool sitting,
                 std::vector<BonePose>& body, DragonPose& segments);

}  // namespace ov::render
