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

/// A chicken lies on its back: its body bone is turned a quarter turn about x
/// and stays there. Mojang's published animation for the older chicken
/// geometry says exactly `"rotation": [ "90.0 - this", 0.0, 0.0 ]`, and the
/// newer one bakes the same quarter turn into the cube instead. This build
/// keeps the un-baked geometry and applies the angle here, so that the model
/// file stays a pure list of boxes.
constexpr f32 kChickenBodyDegrees = 90.0F;

[[nodiscard]] f32 to_radians(f32 degrees) noexcept {
    return degrees * (std::numbers::pi_v<f32> / 180.0F);
}

/// cos of the swing, at a phase given in degrees of the *cycle*.
[[nodiscard]] f32 swing_cos(f32 swing, f32 phase_degrees, f32 rate = 1.0F) noexcept {
    return std::cos(to_radians(swing * kSwingDegreesPerUnit * rate + phase_degrees));
}

[[nodiscard]] f32 swing_sin(f32 swing, f32 phase_degrees) noexcept {
    return std::sin(to_radians(swing * kSwingDegreesPerUnit + phase_degrees));
}

void set_rotation(std::vector<BonePose>& out, const EntityModel& model, std::string_view bone,
                  Vec3f rotation) noexcept {
    const i32 index = model.bone(bone);
    if (index < 0) {
        return;
    }
    out[static_cast<usize>(index)].rotation = rotation;
}

/// The head, which every species turns the same way.
void pose_head(std::vector<BonePose>& out, const EntityModel& model, const WalkState& state,
               std::string_view bone) noexcept {
    set_rotation(out, model, bone, Vec3f{state.head_pitch, state.head_yaw, 0.0F});
}

struct Species {
    std::string_view type;
    std::string_view model;
    std::string_view texture;
    EntityAnimation  animation;
};

/// The nine models this build has. Everything else is refused by name.
///
/// The eight mobs are the eight the server can bring to life
/// (gameplay::mob_kinds); the ninth is the player, which every multiplayer
/// session needs and which no mob list contains.
constexpr std::array<Species, 9> kSpecies{{
    {"minecraft:player", "humanoid", "minecraft:entity/player/wide/steve",
     EntityAnimation::Humanoid},
    {"minecraft:zombie", "zombie", "minecraft:entity/zombie/zombie", EntityAnimation::Humanoid},
    {"minecraft:skeleton", "skeleton", "minecraft:entity/skeleton/skeleton",
     EntityAnimation::Humanoid},
    {"minecraft:creeper", "creeper", "minecraft:entity/creeper/creeper", EntityAnimation::Creeper},
    {"minecraft:spider", "spider", "minecraft:entity/spider/spider", EntityAnimation::Spider},
    {"minecraft:cow", "cow", "minecraft:entity/cow/cow", EntityAnimation::Quadruped},
    {"minecraft:pig", "pig", "minecraft:entity/pig/pig", EntityAnimation::Quadruped},
    {"minecraft:sheep", "sheep", "minecraft:entity/sheep/sheep", EntityAnimation::Quadruped},
    {"minecraft:chicken", "chicken", "minecraft:entity/chicken", EntityAnimation::Chicken},
}};

[[nodiscard]] const Species* species_of(std::string_view type_name) noexcept {
    for (const Species& species : kSpecies) {
        if (species.type == type_name) {
            return &species;
        }
    }
    return nullptr;
}

}  // namespace

EntityAnimation entity_animation(std::string_view type_name) noexcept {
    const Species* species = species_of(type_name);
    return species != nullptr ? species->animation : EntityAnimation::Static;
}

std::string_view entity_model_name(std::string_view type_name) noexcept {
    const Species* species = species_of(type_name);
    return species != nullptr ? species->model : std::string_view{};
}

std::string_view entity_texture_name(std::string_view type_name) noexcept {
    const Species* species = species_of(type_name);
    return species != nullptr ? species->texture : std::string_view{};
}

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

        case EntityAnimation::Humanoid: {
            pose_head(out, model, state, "head");
            // The head layer rides the head, so it needs no angle of its own —
            // it is a child bone and inherits one.
            const f32 arm = swing_cos(state.swing, 0.0F) * kArmSwingDegrees * state.amount;
            const f32 leg = swing_cos(state.swing, 0.0F) * kLegSwingDegrees * state.amount;
            set_rotation(out, model, "leftArm", Vec3f{arm, 0.0F, 0.0F});
            set_rotation(out, model, "rightArm", Vec3f{-arm, 0.0F, 0.0F});
            set_rotation(out, model, "leftLeg", Vec3f{-leg, 0.0F, 0.0F});
            set_rotation(out, model, "rightLeg", Vec3f{leg, 0.0F, 0.0F});
            break;
        }

        case EntityAnimation::Quadruped:
        case EntityAnimation::Creeper: {
            pose_head(out, model, state, "head");
            // Legs are numbered 0 right-front, 1 left-front, 2 right-hind,
            // 3 left-hind, and move on the diagonal: 0 and 3 together, 1 and 2
            // together. Same signs in both species; a creeper walks like a cow
            // that is about to explode.
            const f32 leg = swing_cos(state.swing, 0.0F) * kLegSwingDegrees * state.amount;
            set_rotation(out, model, "leg0", Vec3f{leg, 0.0F, 0.0F});
            set_rotation(out, model, "leg1", Vec3f{-leg, 0.0F, 0.0F});
            set_rotation(out, model, "leg2", Vec3f{-leg, 0.0F, 0.0F});
            set_rotation(out, model, "leg3", Vec3f{leg, 0.0F, 0.0F});
            break;
        }

        case EntityAnimation::Chicken: {
            pose_head(out, model, state, "head");
            set_rotation(out, model, "body", Vec3f{kChickenBodyDegrees, 0.0F, 0.0F});
            const f32 leg = swing_cos(state.swing, 0.0F) * kLegSwingDegrees * state.amount;
            set_rotation(out, model, "leg0", Vec3f{leg, 0.0F, 0.0F});
            set_rotation(out, model, "leg1", Vec3f{-leg, 0.0F, 0.0F});
            // The wings flap from the bird's vertical speed, which no packet in
            // this server carries. They are left at rest, and that is said in
            // the provenance document rather than faked from the clock.
            break;
        }

        case EntityAnimation::Spider: {
            pose_head(out, model, state, "head");
            // Rest pose, then the walk on top of it. Both sets of numbers are
            // Mojang's published ones, in degrees, unchanged.
            constexpr std::array<f32, 8> kRestYaw{45.0F,  -45.0F, 22.5F,  -22.5F,
                                                  -22.5F, 22.5F,  -45.0F, 45.0F};
            constexpr std::array<f32, 8> kRestRoll{-45.0F, 45.0F,  -33.3F, 33.3F,
                                                   -33.3F, 33.3F,  -45.0F, 45.0F};
            std::array<char, 5> name{'l', 'e', 'g', '0', '\0'};
            for (usize leg = 0; leg < 8; ++leg) {
                name[3]                = static_cast<char>('0' + leg);
                const std::string_view bone(name.data(), 4);
                const f32              phase = 90.0F * static_cast<f32>(leg / 2);
                // The leg's yaw beats at twice the stride; its roll at once.
                const f32 yaw =
                    std::abs(swing_cos(state.swing, phase, 2.0F) * kSpiderSwingDegrees) *
                    state.amount;
                const f32 roll =
                    std::abs(swing_sin(state.swing, phase) * kSpiderSwingDegrees) * state.amount;
                const f32 side = (leg % 2 == 0) ? 1.0F : -1.0F;
                set_rotation(out, model, bone,
                             Vec3f{0.0F, kRestYaw[leg] - side * yaw, kRestRoll[leg] + side * roll});
            }
            break;
        }
    }
}

}  // namespace ov::render
