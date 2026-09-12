#include "ov/render/entity_pose.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace ov::render {

namespace {

/// 0.6662 radians. Every walk cycle in the game turns on this one number.
constexpr f32 kSwingDegreesPerUnit = 38.17F;
/// Leg amplitude: 80 degrees, which is 1.4 radians.
constexpr f32 kLegSwingDegrees = 80.0F;
/// Arm amplitude: 57.3 degrees, which is 1 radian.
constexpr f32 kArmSwingDegrees = 57.3F;
/// The spider's, from its published walk animation.
constexpr f32 kSpiderSwingDegrees = 22.92F;

[[nodiscard]] f32 to_radians(f32 degrees) noexcept {
    return degrees * (std::numbers::pi_v<f32> / 180.0F);
}

[[nodiscard]] f32 swing_cos(f32 swing, f32 phase_degrees, f32 rate = 1.0F) noexcept {
    return std::cos(to_radians(swing * kSwingDegreesPerUnit * rate + phase_degrees));
}

[[nodiscard]] f32 swing_sin(f32 swing, f32 phase_degrees) noexcept {
    return std::sin(to_radians(swing * kSwingDegreesPerUnit + phase_degrees));
}

/// Set a bone's rotation, given in the game's numbers. A bone the model does
/// not have is skipped: a zombie has no jacket, a player has.
void rotate(std::vector<BonePose>& out, const EntityModel& model, std::string_view bone, f32 x,
            f32 y, f32 z) noexcept {
    const i32 index = model.bone(bone);
    if (index >= 0) {
        out[static_cast<usize>(index)].rotation = game_rotation(x, y, z);
    }
}

/// Move a bone so its pivot lands at `java_pivot`, given in the game's model
/// space (y down) — what the game's animations write into `ModelPart.x/y/z`.
void place(std::vector<BonePose>& out, const EntityModel& model, std::string_view bone,
           Vec3f java_pivot) noexcept {
    const i32 index = model.bone(bone);
    if (index < 0) {
        return;
    }
    const EntityBone& entry = model.bones[static_cast<usize>(index)];
    // Only a root's pivot is absolute in the game's space; these animations
    // only ever move roots (the blaze's rods), and a child moved this way
    // would be moved relative to where its parent put it.
    const Vec3f target{java_pivot.x, -java_pivot.y, java_pivot.z};
    out[static_cast<usize>(index)].offset = target - entry.pivot;
}

/// The head, which every species turns the same way: pitch about x, yaw about
/// y. `hat` is the humanoid's second head layer, a sibling rather than a child
/// in the game's models, so it is given the same angles.
void pose_head(std::vector<BonePose>& out, const EntityModel& model, const WalkState& state,
               std::string_view head, std::string_view hat = {}) noexcept {
    rotate(out, model, head, state.head_pitch, state.head_yaw, 0.0F);
    if (!hat.empty()) {
        rotate(out, model, hat, state.head_pitch, state.head_yaw, 0.0F);
    }
}

void baby_head(std::vector<BonePose>& out, const EntityModel& model, const WalkState& state,
               std::string_view head) noexcept {
    if (state.baby_head_scale == 1.0F) {
        return;
    }
    const i32 index = model.bone(head);
    if (index < 0) {
        return;
    }
    BonePose& pose = out[static_cast<usize>(index)];
    pose.scale     = Vec3f{state.baby_head_scale, state.baby_head_scale, state.baby_head_scale};
    pose.offset    = pose.offset + state.baby_head_offset;
}

/// Two arms and two legs, with the overlays the player and the piglin carry
/// as siblings of their limbs.
void pose_humanoid_limbs(std::vector<BonePose>& out, const EntityModel& model,
                         const WalkState& state, f32 arm_x_right, f32 arm_x_left,
                         f32 leg_limit = 0.0F) noexcept {
    f32 leg_right = swing_cos(state.swing, 0.0F) * kLegSwingDegrees * state.amount;
    f32 leg_left  = swing_cos(state.swing, 180.0F) * kLegSwingDegrees * state.amount;
    if (leg_limit > 0.0F) {
        leg_right = std::clamp(leg_right, -leg_limit, leg_limit);
        leg_left  = std::clamp(leg_left, -leg_limit, leg_limit);
    }
    for (const std::string_view bone : {std::string_view{"right_arm"}, std::string_view{"right_sleeve"}}) {
        rotate(out, model, bone, arm_x_right, 0.0F, 0.0F);
    }
    for (const std::string_view bone : {std::string_view{"left_arm"}, std::string_view{"left_sleeve"}}) {
        rotate(out, model, bone, arm_x_left, 0.0F, 0.0F);
    }
    for (const std::string_view bone : {std::string_view{"right_leg"}, std::string_view{"right_pants"}}) {
        rotate(out, model, bone, leg_right, 0.0F, 0.0F);
    }
    for (const std::string_view bone : {std::string_view{"left_leg"}, std::string_view{"left_pants"}}) {
        rotate(out, model, bone, leg_left, 0.0F, 0.0F);
    }
}

/// Four legs on the diagonal: right hind with left front, left hind with
/// right front, as the published `quadruped.walk` pairs them.
void pose_four_legs(std::vector<BonePose>& out, const EntityModel& model, const WalkState& state,
                    f32 amplitude) noexcept {
    const f32 leg = swing_cos(state.swing, 0.0F) * amplitude * state.amount;
    rotate(out, model, "right_hind_leg", leg, 0.0F, 0.0F);
    rotate(out, model, "left_front_leg", leg, 0.0F, 0.0F);
    rotate(out, model, "left_hind_leg", -leg, 0.0F, 0.0F);
    rotate(out, model, "right_front_leg", -leg, 0.0F, 0.0F);
}

}  // namespace

void advance_walk(WalkState& state, f32 horizontal_distance) noexcept {
    // Vanilla's walk animation is a first-order filter on the distance walked
    // in a tick, saturating at one. Neither constant is stated by a source this
    // project can cite; both are named in the provenance document.
    constexpr f32 kDistanceToSpeed = 4.0F;
    constexpr f32 kSmoothing       = 0.4F;

    const f32 target = std::min(horizontal_distance * kDistanceToSpeed, 1.0F);
    state.amount += (target - state.amount) * kSmoothing;
    state.swing += state.amount;
}

void pose_model(const EntityModel& model, EntityAnimation animation, const WalkState& state,
                std::vector<BonePose>& out) {
    out.assign(model.bones.size(), BonePose{});

    switch (animation) {
        case EntityAnimation::Static:
            break;

        case EntityAnimation::Humanoid:
        case EntityAnimation::Enderman: {
            pose_head(out, model, state, "head", "hat");
            // `humanoid.animation.json` → `move`: the right arm swings against
            // the left leg, a radian each way; legs 1.4.
            const f32 arm = swing_cos(state.swing, 0.0F) * kArmSwingDegrees * state.amount;
            if (animation == EntityAnimation::Enderman) {
                // `enderman.animation.json` → `arms_legs`: every limb clamped
                // to ±22.92°, which is 0.4 rad.
                constexpr f32 kLimit = 22.92F;
                pose_humanoid_limbs(out, model, state, std::clamp(-arm, -kLimit, kLimit),
                                    std::clamp(arm, -kLimit, kLimit), kLimit);
            } else {
                pose_humanoid_limbs(out, model, state, -arm, arm);
            }
            break;
        }

        case EntityAnimation::ZombieArms: {
            pose_head(out, model, state, "head", "hat");
            // Arms held out in front, splayed and breathing: a 2.865° bob at
            // 3.84° a tick, y ±5.73°, z ±(2.865° + 2.865° × cos) at 5.16° a
            // tick (`zombie.animation.json` → `attack_bare_hand`). The angle
            // itself is the game's vertices, not the published file's −90°:
            // a husk's and a zombie's hands sit 10° lower, at −80°
            // (docs/provenance/rendu-entites.md § 9); −120° when aggressive.
            const f32 age   = state.age_ticks;
            const f32 bob_x = std::sin(to_radians(age * 3.8388F)) * 2.865F;
            const f32 bob_z = std::cos(to_radians(age * 5.1566F)) * 2.865F + 2.865F;
            const f32 arms  = state.aggressive ? -120.0F : -80.0F;
            pose_humanoid_limbs(out, model, state, arms + bob_x, arms - bob_x);
            for (const std::string_view bone : {std::string_view{"right_arm"}, std::string_view{"right_sleeve"}}) {
                rotate(out, model, bone, arms + bob_x, -5.73F, bob_z);
            }
            for (const std::string_view bone : {std::string_view{"left_arm"}, std::string_view{"left_sleeve"}}) {
                rotate(out, model, bone, arms - bob_x, 5.73F, -bob_z);
            }
            break;
        }

        case EntityAnimation::Villager: {
            pose_head(out, model, state, "head");
            // `villager.animation.json` → `move`: ±40°, half the humanoid's
            // stride; the arms stay folded at their rest angle.
            const f32 leg = swing_cos(state.swing, 0.0F) * 40.0F * state.amount;
            rotate(out, model, "right_leg", leg, 0.0F, 0.0F);
            rotate(out, model, "left_leg", -leg, 0.0F, 0.0F);
            break;
        }

        case EntityAnimation::Quadruped:
            pose_head(out, model, state, "head");
            pose_four_legs(out, model, state, kLegSwingDegrees);
            break;

        case EntityAnimation::Feline:
            // `cat.animation.json` → `walk`: 57.3°, a radian.
            pose_head(out, model, state, "head");
            pose_four_legs(out, model, state, kArmSwingDegrees);
            // The tail's tip bent up and back, 99° standing and swinging 45°
            // with the stride: where the game's vertices put a standing cat's
            // tail (docs/provenance/rendu-entites.md § 9).
            rotate(out, model, "tail2",
                   99.0F + swing_cos(state.swing, 0.0F) * 45.0F * state.amount, 0.0F, 0.0F);
            break;

        case EntityAnimation::Boat:
            // The oars as a boat nobody rows holds them: tipped 37.5° down and
            // turned 37.86° towards the stern, each side the other's mirror —
            // where the game's vertices put them (§ 9).
            rotate(out, model, "left_paddle", -37.5F, 37.86F, 0.0F);
            rotate(out, model, "right_paddle", -37.5F, -37.86F, 0.0F);
            break;

        case EntityAnimation::Chicken: {
            // The beak and the wattle are siblings of the head, not children.
            pose_head(out, model, state, "head");
            rotate(out, model, "beak", state.head_pitch, state.head_yaw, 0.0F);
            rotate(out, model, "red_thing", state.head_pitch, state.head_yaw, 0.0F);
            const f32 leg = swing_cos(state.swing, 0.0F) * kLegSwingDegrees * state.amount;
            rotate(out, model, "right_leg", leg, 0.0F, 0.0F);
            rotate(out, model, "left_leg", -leg, 0.0F, 0.0F);
            // The wings flap from the bird's vertical speed, which no packet
            // this client reads carries. Left at rest, and said.
            break;
        }

        case EntityAnimation::Spider: {
            pose_head(out, model, state, "head");
            // `spider.animation.json` → `default_leg_pose` then `walk`, leg0 to
            // leg7 being the right hind leg to the left front one — the same
            // angles, in degrees, that the game's own spider sets.
            constexpr std::array<std::string_view, 8> kLegs{
                "right_hind_leg",         "left_hind_leg",         "right_middle_hind_leg",
                "left_middle_hind_leg",   "right_middle_front_leg", "left_middle_front_leg",
                "right_front_leg",        "left_front_leg"};
            constexpr std::array<f32, 8> kRestYaw{45.0F,  -45.0F, 22.5F,  -22.5F,
                                                  -22.5F, 22.5F,  -45.0F, 45.0F};
            constexpr std::array<f32, 8> kRestRoll{-45.0F, 45.0F,  -33.3F, 33.3F,
                                                   -33.3F, 33.3F,  -45.0F, 45.0F};
            for (usize leg = 0; leg < kLegs.size(); ++leg) {
                const f32 phase = 90.0F * static_cast<f32>(leg / 2);
                // The yaw beats at twice the stride; the roll at once.
                const f32 yaw =
                    std::abs(swing_cos(state.swing, phase, 2.0F) * kSpiderSwingDegrees) *
                    state.amount;
                const f32 roll =
                    std::abs(swing_sin(state.swing, phase) * kSpiderSwingDegrees) * state.amount;
                const f32 side = (leg % 2 == 0) ? 1.0F : -1.0F;
                rotate(out, model, kLegs[leg], 0.0F, kRestYaw[leg] - side * yaw,
                       kRestRoll[leg] + side * roll);
            }
            break;
        }

        case EntityAnimation::Blaze: {
            pose_head(out, model, state, "head");
            // `blaze.animation.json` → `move`, three rings of four rods. The
            // published file writes the orbit per second (−360°, 108°, −180°)
            // and the bob per tick; both are written here per tick. At age 0
            // these positions are exactly the ones the game's own model is
            // baked with (docs/provenance/rendu-entites.md, § blaze), which is
            // the check that the rings and the phases are the game's.
            const f32 age = state.age_ticks;
            std::array<char, 7> name{'p', 'a', 'r', 't', '0', '0', '\0'};
            for (u32 rod = 0; rod < 12; ++rod) {
                f32 degrees = 0.0F;
                f32 radius  = 0.0F;
                f32 height  = 0.0F;
                if (rod < 4) {
                    degrees = age * -18.0F + 90.0F * static_cast<f32>(rod);
                    radius  = 9.0F;
                    height  = -2.0F + std::cos((static_cast<f32>(rod) * 2.0F + age) * 0.25F);
                } else if (rod < 8) {
                    degrees = age * 5.4F + 45.0F + 90.0F * static_cast<f32>(rod - 4);
                    radius  = 7.0F;
                    height  = 2.0F + std::cos((static_cast<f32>(rod) * 2.0F + age) * 0.25F);
                } else {
                    degrees = age * -9.0F + 27.0F + 90.0F * static_cast<f32>(rod - 8);
                    radius  = 5.0F;
                    height  = 11.0F + std::cos((static_cast<f32>(rod) * 1.5F + age) * 0.5F);
                }
                const usize length = rod < 10 ? 5 : 6;
                if (rod < 10) {
                    name[4] = static_cast<char>('0' + rod);
                } else {
                    name[4] = '1';
                    name[5] = static_cast<char>('0' + (rod - 10));
                }
                place(out, model, std::string_view(name.data(), length),
                      Vec3f{std::cos(to_radians(degrees)) * radius, height,
                            std::sin(to_radians(degrees)) * radius});
            }
            break;
        }

        case EntityAnimation::Ghast: {
            // `ghast.animation.json` → `move`: 23° + 11.5° × sin(18° a tick +
            // 57° per tentacle), which is 0.4 + 0.2 × sin(0.3 t + i) in radians.
            std::array<char, 10> name{'t', 'e', 'n', 't', 'a', 'c', 'l', 'e', '0', '\0'};
            for (u32 tentacle = 0; tentacle < 9; ++tentacle) {
                name[8] = static_cast<char>('0' + tentacle);
                const f32 x =
                    23.0F + 11.5F * std::sin(to_radians(state.age_ticks * 18.0F +
                                                        57.3F * static_cast<f32>(tentacle)));
                rotate(out, model, std::string_view(name.data(), 9), x, 0.0F, 0.0F);
            }
            break;
        }

        case EntityAnimation::EndCrystal: {
            // `ender_crystal.animation.json` → `move`: the cubes spin 60° a
            // second about their tilted axis and bob 8 units at 230° a second.
            const f32 seconds = state.age_ticks / 20.0F;
            const f32 bob     = 16.0F + std::sin(to_radians(seconds * 230.0F)) * 8.0F;
            const f32 spin    = 14.5F - seconds * 60.0F;
            for (const std::string_view bone : {std::string_view{"glass"}, std::string_view{"cube"}}) {
                const i32 index = model.bone(bone);
                if (index < 0) {
                    continue;
                }
                BonePose& pose = out[static_cast<usize>(index)];
                pose.rotation  = game_rotation(bone == "glass" ? 39.2F : 219.2F, spin, -39.2F);
                pose.offset    = Vec3f{0.0F, bob, 0.0F};
                if (bone == "cube") {
                    pose.scale = Vec3f{0.875F, 0.875F, 0.875F};
                }
            }
            break;
        }

        case EntityAnimation::ArmorStand: {
            constexpr std::array<std::string_view, 6> kLimbs{"head",      "body",     "left_arm",
                                                             "right_arm", "left_leg", "right_leg"};
            for (usize limb = 0; limb < kLimbs.size(); ++limb) {
                const Vec3f& degrees = state.stand[limb];
                rotate(out, model, kLimbs[limb], degrees.x, degrees.y, degrees.z);
            }
            rotate(out, model, "hat", state.stand[0].x, state.stand[0].y, state.stand[0].z);
            break;
        }
    }

    baby_head(out, model, state, "head");
    // Parts of the head that the model hangs off the root rather than off the
    // head — the chicken's beak and wattle, the rabbit's ears and nose: the
    // game grows and moves them with the head. A part that is the head's child
    // (a piglin's ear, a villager's nose) already follows it.
    const i32 head = model.bone("head");
    for (const std::string_view part : {std::string_view{"beak"}, std::string_view{"red_thing"},
                                        std::string_view{"left_ear"}, std::string_view{"right_ear"},
                                        std::string_view{"nose"}}) {
        const i32 index = model.bone(part);
        if (index < 0 || index == head) {
            continue;
        }
        const i32 parent = model.bones[static_cast<usize>(index)].parent;
        if (parent < 0 || model.bones[static_cast<usize>(parent)].parent < 0) {
            baby_head(out, model, state, part);
        }
    }
}

namespace {

/// A heading difference, wrapped to (−180, 180].
[[nodiscard]] f32 wrap(f32 degrees) noexcept {
    f32 wrapped = std::fmod(degrees + 180.0F, 360.0F);
    if (wrapped < 0.0F) {
        wrapped += 360.0F;
    }
    return wrapped - 180.0F;
}

}  // namespace

void pose_dragon(const EntityModel& model, f32 flap, const DragonHistory& history, bool sitting,
                 std::vector<BonePose>& body, DragonPose& segments) {
    body.assign(model.bones.size(), BonePose{});

    constexpr f32 kRotationScale = 1.5F;
    constexpr f32 kSegment       = 10.0F;
    const f32     flap_degrees   = flap * 360.0F;

    // `rotation_factor_translate`: the body's bob with the beat.
    const f32 base   = std::sin(to_radians(flap_degrees - 57.3F)) + 1.0F;
    const f32 factor = (base * base + base * 2.0F) * 0.05F;
    segments.bob     = factor;

    // The heading the whole chain rolls about: half-way between five and ten
    // ticks ago (`piece_rotation`).
    const f32 pre_rotation   = wrap(history.yaw_at(5) - history.yaw_at(10));
    const f32 piece_rotation = wrap(history.yaw_at(5) + pre_rotation / 2.0F);

    // The neck: five segments, each ten units on from the last.
    Vec3f position{0.0F, -20.0F, -12.0F};
    for (usize index = 0; index < segments.neck.size(); ++index) {
        const usize frame = 5 - index;
        const f32   rise  = sitting ? static_cast<f32>(index)
                                    : history.y_at(frame) - history.y_at(6);
        const f32   rx =
            std::cos(to_radians(static_cast<f32>(index) * 25.79F + flap_degrees)) * 8.6F +
            rise * kRotationScale * 5.0F;
        const f32 ry = wrap(history.yaw_at(frame) - history.yaw_at(6)) * kRotationScale;
        const f32 rz = -wrap(history.yaw_at(frame) - piece_rotation) * kRotationScale;
        segments.neck[index] = DragonSegment{position, game_rotation(rx, ry, rz)};
        const f32 cx         = std::cos(to_radians(rx));
        position = position - Vec3f{std::sin(to_radians(ry)) * cx, std::sin(to_radians(rx)),
                                    std::cos(to_radians(ry)) * cx} *
                                  kSegment;
    }
    // The head, at the end of the neck.
    const f32 head_rx = sitting ? 6.0F * kRotationScale * 5.0F : 0.0F;
    const f32 head_ry = wrap(history.yaw_at(0) - history.yaw_at(6));
    const f32 head_rz = -wrap(history.yaw_at(0) - piece_rotation);
    segments.head     = DragonSegment{position, game_rotation(head_rx, head_ry, head_rz)};

    // The tail: twelve segments from behind the body, turned to point back.
    position   = Vec3f{0.0F, -10.0F, 60.0F};
    f32 wobble = std::sin(to_radians(flap_degrees)) * 2.86F;
    for (usize index = 0; index < segments.tail.size(); ++index) {
        if (index > 0) {
            wobble += std::sin(to_radians(static_cast<f32>(index) * 25.78F + flap_degrees)) * 2.86F;
        }
        const usize frame = 12 + index;
        const f32   rx    = wobble + (history.y_at(frame) - history.y_at(11)) * kRotationScale * 5.0F;
        const f32   ry =
            wrap(history.yaw_at(frame) - history.yaw_at(11)) * kRotationScale + 180.0F;
        const f32 rz = -wrap(history.yaw_at(frame) - piece_rotation) * kRotationScale;
        segments.tail[index] = DragonSegment{position, game_rotation(rx, ry, rz)};
        const f32 cx         = std::cos(to_radians(rx));
        position = position - Vec3f{std::sin(to_radians(ry)) * cx, std::sin(to_radians(rx)),
                                    std::cos(to_radians(ry)) * cx} *
                                  kSegment;
    }

    // The wings and the legs, `ender_dragon.animation.json` → `wings_limbs_movement`;
    // the jaw, `jaw_movement`. The right wing is the published `wing`, the left
    // `wing1`.
    const f32 beat = std::sin(to_radians(flap_degrees));
    // Each wing swept 14.32° (a quarter radian) towards the tail: the game's
    // wing bones sit 1.28 blocks further back than the unswept ones.
    rotate(body, model, "right_wing", 7.16F - std::cos(to_radians(flap_degrees)) * 11.46F, 14.32F,
           (beat + 0.125F) * 45.84F);
    rotate(body, model, "right_wing_tip", 0.0F, 0.0F,
           -(std::sin(to_radians(flap_degrees + 114.6F)) + 0.5F) * 43.0F);
    // The left wing mirrors the right: the game's two wing bones sit at
    // (±5.724, 3.853) — symmetric — where the published `wing1`, beating on
    // −flap, drew them 0.87 apart in height.
    rotate(body, model, "left_wing", 7.16F - std::cos(to_radians(flap_degrees)) * 11.46F, -14.32F,
           -(beat + 0.125F) * 45.84F);
    rotate(body, model, "left_wing_tip", 0.0F, 0.0F,
           (std::sin(to_radians(flap_degrees + 114.6F)) + 0.5F) * 43.0F);
    const f32 lift = factor * 5.7F;
    for (const std::string_view side : {std::string_view{"right"}, std::string_view{"left"}}) {
        std::array<char, 24> name{};
        const auto set = [&](std::string_view suffix, f32 degrees) {
            usize length = 0;
            for (const char c : side) {
                name[length++] = c;
            }
            for (const char c : suffix) {
                name[length++] = c;
            }
            rotate(body, model, std::string_view(name.data(), length), degrees, 0.0F, 0.0F);
        };
        set("_hind_leg", 57.3F + lift);
        set("_hind_leg_tip", 28.65F + lift);
        set("_hind_foot", 43.0F + lift);
        set("_front_leg", 74.5F + lift);
        set("_front_leg_tip", -28.65F - lift);
        set("_front_foot", 43.0F + lift);
    }
    rotate(body, model, "jaw", (beat + 1.0F) * 11.46F, 0.0F, 0.0F);

    // The neck bone is drawn by the segments; the head is moved to the end of
    // the neck and turned as it.
    const i32 neck = model.bone("neck");
    if (neck >= 0) {
        body[static_cast<usize>(neck)].hidden = true;
    }
    const i32 head = model.bone("head");
    if (head >= 0) {
        BonePose& pose = body[static_cast<usize>(head)];
        pose.offset    = segments.head.pivot - model.bones[static_cast<usize>(head)].pivot;
        pose.rotation  = segments.head.rotation;
    }
}

}  // namespace ov::render
