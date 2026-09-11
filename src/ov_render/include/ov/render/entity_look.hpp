// What an entity looks like: which layers of which models, with which
// textures, chosen from what the server said about it.
//
// The network hands the client a type and a list of `(index, value)` pairs.
// Which index is a sheep's fleece and which a pig's saddle depends on the type,
// and which texture a villager wears depends on three numbers inside one of
// them. This is that table, as one pure function from those numbers to a list
// of layers — so `test_entity_look.cpp` can assert that a sheared lime sheep
// draws no wool and a level-3 plains librarian draws four textures, without a
// GPU and without a server.
//
// Where the indices come from: PrismarineJS/minecraft-data (MIT) lists, for
// 1.20, every entity's metadata keys in index order. Every index this project
// had already measured on the real server (sheep fleece 17, villager data 18,
// minecart display block 11, slime size 16, creeper powered 17, baby 16) is at
// the place that list puts it; docs/provenance/rendu-entites.md keeps the table.
// Where the *layers* come from: the game's own renderer table, read from the
// running client (scripts/entity_model_oracle.java, "renderers").
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/vec.hpp"
#include "ov/render/asset_source.hpp"
#include "ov/render/entity_pose.hpp"

#include <array>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ov::registry {
class Registries;
}

namespace ov::render {

/// The metadata indices the look reads. Each is the one a vanilla 1.20.1
/// server writes; see the header comment for the source.
namespace meta {
/// Byte on every entity. 0x01 on fire, 0x02 crouching, 0x20 invisible.
inline constexpr u8 kSharedFlags = 0;
inline constexpr u8 kSharedOnFire = 0x01;
inline constexpr u8 kSharedInvisible = 0x20;
/// Optional chat component, and a boolean.
inline constexpr u8 kCustomName        = 2;
inline constexpr u8 kCustomNameVisible = 3;
/// Mob flags: 0x04 aggressive.
inline constexpr u8 kMobFlags      = 15;
inline constexpr u8 kMobAggressive = 0x04;
inline constexpr u8 kMobLeftHanded = 0x02;
/// Every ageable mob and every zombie: a baby.
inline constexpr u8 kBaby = 16;
/// Piglins put their baby flag one later: 16 is immunity to zombification.
inline constexpr u8 kPiglinBaby = 17;
inline constexpr u8 kSheepFleece    = 17;
inline constexpr u8 kPigSaddle      = 17;
inline constexpr u8 kCreeperPowered = 17;
inline constexpr u8 kSlimeSize      = 16;
inline constexpr u8 kVillagerData   = 18;
inline constexpr u8 kZombieVillagerData = 20;
/// Wolf and cat: tame flags (0x04 tamed), then per species.
inline constexpr u8 kTameFlags   = 17;
inline constexpr u8 kTamed       = 0x04;
inline constexpr u8 kWolfCollar  = 20;
inline constexpr u8 kWolfAnger   = 21;
inline constexpr u8 kCatVariant  = 19;
inline constexpr u8 kCatCollar   = 22;
inline constexpr u8 kFoxType     = 17;
inline constexpr u8 kRabbitType  = 17;
inline constexpr u8 kHorseVariant = 18;
inline constexpr u8 kStriderCold   = 18;
inline constexpr u8 kStriderSaddle = 19;
inline constexpr u8 kGhastCharging = 16;
inline constexpr u8 kArmorStandFlags = 15;
inline constexpr u8 kArmorStandSmall = 0x01;
inline constexpr u8 kArmorStandArms  = 0x04;
inline constexpr u8 kArmorStandNoBasePlate = 0x08;
/// Head, body, left arm, right arm, left leg, right leg: Rotations, degrees.
inline constexpr u8 kArmorStandPoses = 16;
inline constexpr u8 kMinecartDisplayBlock  = 11;
inline constexpr u8 kMinecartDisplayOffset = 12;
inline constexpr u8 kMinecartCustomDisplay = 13;
inline constexpr u8 kBoatType = 11;
inline constexpr u8 kEndCrystalBeam = 8;
inline constexpr u8 kEndCrystalBottom = 9;
inline constexpr u8 kDragonPhase = 16;
inline constexpr u8 kItemFrameItem     = 8;
inline constexpr u8 kItemFrameRotation = 9;
inline constexpr u8 kTntFuse = 8;
inline constexpr u8 kCloudRadius = 8;
inline constexpr u8 kCloudColour = 9;
}  // namespace meta

/// One metadata field as the look reads it: the last value the server sent
/// for that index, whatever its wire type.
struct MetaSlot {
    bool               present{false};
    i64                integer{0};
    f32                real{0.0F};
    std::array<i32, 3> villager{};
    std::array<f32, 3> rotation{};
    /// A block position as the wire packs it: 26 bits of x, 26 of z, 12 of y.
    /// An end crystal's beam target.
    i64 position{0};
};

inline constexpr usize kMetaSlots = 24;

/// What the client knows about one entity beyond its type.
struct EntityTraits {
    std::array<MetaSlot, kMetaSlots> slots{};
    /// The six equipment slots as item names (`minecraft:iron_helmet`), main
    /// hand, off hand, feet, legs, chest, head. Empty for nothing.
    std::array<std::string, 6> equipment;
    /// The custom name, flattened to plain text. Empty for none.
    std::string name;

    [[nodiscard]] bool has(u8 index) const noexcept {
        return index < kMetaSlots && slots[index].present;
    }
    [[nodiscard]] i64 integer(u8 index, i64 fallback = 0) const noexcept {
        return has(index) ? slots[index].integer : fallback;
    }
    [[nodiscard]] bool flag(u8 index, u8 bit) const noexcept {
        return (static_cast<u64>(integer(index)) & bit) != 0;
    }
};

/// How a layer is blended.
enum class LayerPass : u8 {
    /// Opaque or cut out, depth written: almost every model.
    Cutout,
    /// Blended, depth tested: a slime's outer jelly, a horse's markings.
    Translucent,
    /// Added, unlit: an enderman's or a spider's eyes.
    Eyes,
    /// Added, unlit, scrolling on a repeating texture: a charged creeper.
    Energy,
};

/// Which of a humanoid's bones an armour layer draws, as the game's armour
/// layer shows only the parts one piece covers.
enum class ArmorPart : u8 { All, Head, Chest, Legs, Feet };

/// One model drawn with one texture.
struct LookLayer {
    /// A model name in the EntityModelSet: the game's layer name without the
    /// namespace, and without `#main` (`sheep#fur`, `boat/oak`).
    std::string model;
    /// A resource location without `textures/` and `.png`.
    std::string texture;
    LayerPass   pass{LayerPass::Cutout};
    /// 0xAARRGGBB, multiplied into the texture.
    u32 tint{0xFFFFFFFFU};
    /// A villager's type layer under a profession that wears its own hat.
    bool hide_hat{false};
    ArmorPart armor{ArmorPart::All};
    /// This layer draws no `hat` cube (an armour stand's bare model), while a
    /// helmet layer over it still draws its own.
    bool hide_bare_hat{false};
};

/// What kind of thing an entity type is drawn as.
enum class EntityKind : u8 {
    /// No look this build knows. Reported, never drawn as something else.
    Unknown,
    /// Layers of models from the EntityModelSet.
    Model,
    /// A stack on the ground: the item's own model.
    DroppedItem,
    /// A sprite from `entity/experience_orb.png`, sized by its value.
    ExperienceOrb,
    /// A block model: primed TNT, a falling block.
    Block,
    /// A frame block model with the item it holds.
    ItemFrame,
    /// An item's sprite, facing the camera: snowballs, eggs, pearls, potions.
    ThrownItem,
    /// A sprite from its own texture, facing the camera: the dragon's fireball.
    Billboard,
    /// Drawn by the game from code, not from a model this build can read.
    Refused,
    /// Nothing is drawn: the game draws this one as particles only.
    Invisible,
};

[[nodiscard]] EntityKind entity_kind(std::string_view type_name) noexcept;

/// Why a Refused kind is not drawn, for the one log line that says so.
[[nodiscard]] std::string_view refusal_reason(std::string_view type_name) noexcept;

/// Everything needed to draw a Model entity.
struct EntityLook {
    EntityAnimation        animation{EntityAnimation::Static};
    std::vector<LookLayer> layers;
    /// Uniform scale about the model origin, the renderer's own and the
    /// baby's together.
    f32 scale{1.0F};
    /// EntityPlacement::origin — 24.016 units up for every living entity.
    Vec3f origin{0.0F, 24.016F, 0.0F};
    f32   lift{0.0F};
    f32   model_yaw{0.0F};
    /// A baby's head, relative to its body (entity_pose.hpp).
    f32   baby_head_scale{1.0F};
    Vec3f baby_head_offset{};
    bool  baby{false};
    /// "Dinnerbone" and "Grumm" are drawn upside down.
    bool upside_down{false};
    /// Whether the dying topple and the red flash apply: living entities only.
    bool living{true};
    /// Bones the game hides for this entity in its current state: a horse's
    /// baby legs on an adult and its saddle when it has none, the player
    /// model's cloak and ears (drawn by layers of their own), an armour stand's
    /// base plate and arms by its flags, a crystal's base.
    std::vector<std::string> hidden;
};

enum class HatKind : u8 { None, Partial, Full };

/// The `villager` section of each type and profession texture's `.mcmeta`,
/// read once from the resource pack stack. What it decides is documented with
/// resource packs: a profession texture whose hat is `full` hides the hat of
/// the type layer drawn under it, a `partial` one hides it only when the type
/// declares a `full` hat of its own.
class VillagerHats {
public:
    /// Read every villager and zombie villager type and profession texture's
    /// metadata that `source` has.
    static VillagerHats load(const AssetSource& source);

    void set(std::string texture, HatKind hat) { hats_[std::move(texture)] = hat; }

    [[nodiscard]] HatKind of(const std::string& texture) const noexcept;

private:
    std::unordered_map<std::string, HatKind> hats_;
};

/// What resolve_look needs beyond the entity: the registries, to turn a cat
/// variant or a villager profession id into its name, and the villager hats.
struct LookContext {
    const registry::Registries* registries{nullptr};
    const VillagerHats*         hats{nullptr};
};

/// The look of a Model entity. An empty `layers` means this type is not a
/// Model this build draws; entity_kind() says what it is instead.
[[nodiscard]] EntityLook resolve_look(std::string_view type_name, const EntityTraits& traits,
                                      const LookContext& context);

/// Every texture resolve_look can name, for the atlas: built once at startup,
/// so the atlas holds every variant before the first villager arrives.
void list_look_textures(const LookContext& context, std::vector<std::string>& out);

/// The dye colours, 0xRRGGBB, by the dye's id. `sheep` is what a fleece is
/// tinted with, which is not the dye itself (docs/provenance/rendu-entites.md).
[[nodiscard]] u32 dye_colour(i64 dye) noexcept;
[[nodiscard]] u32 sheep_fleece_colour(i64 dye) noexcept;

/// Hide the bones an armour layer does not cover, and a villager's hat.
void apply_layer_visibility(const EntityModel& model, const LookLayer& layer,
                            std::vector<BonePose>& poses);

/// Hide the bones the look says the game hides (EntityLook::hidden).
void apply_look_visibility(const EntityModel& model, const EntityLook& look,
                           std::vector<BonePose>& poses);

}  // namespace ov::render
