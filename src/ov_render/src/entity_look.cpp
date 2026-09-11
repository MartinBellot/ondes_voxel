#include "ov/render/entity_look.hpp"

#include "json.hpp"
#include "ov/base/resource_location.hpp"
#include "ov/registry/registries.hpp"

#include <algorithm>

namespace ov::render {

namespace {

constexpr std::string_view kNs = "minecraft:";

/// The model origin of a living entity: the game draws its model 1.501 blocks
/// above the feet, which is 24.016 model units.
constexpr f32 kLivingOrigin = 24.016F;

/// The dye colours and the fleece tints, as the running client computed them
/// (scripts/entity_model_oracle.java, "dye"). A fleece is the dye's colour
/// times 0.75, except white, which is a flat 0.902.
constexpr std::array<u32, 16> kDye{0xF9FFFE, 0xF9801D, 0xC74EBD, 0x3AB3DA, 0xFED83D, 0x80C71F,
                                   0xF38BAA, 0x474F52, 0x9D9D97, 0x169C9C, 0x8932B8, 0x3C44AA,
                                   0x835432, 0x5E7C16, 0xB02E26, 0x1D1D21};
constexpr std::array<u32, 16> kFleece{0xE6E6E6, 0xBB6016, 0x953B8E, 0x2C86A3, 0xBEA22E, 0x609517,
                                      0xB66880, 0x353B3E, 0x767671, 0x117575, 0x67268A, 0x2D3380,
                                      0x623F26, 0x475D11, 0x84231D, 0x161619};

[[nodiscard]] std::string tex(std::string_view path) {
    std::string out(kNs);
    out += "entity/";
    out += path;
    return out;
}

[[nodiscard]] std::string_view strip_ns(std::string_view name) noexcept {
    return name.starts_with(kNs) ? name.substr(kNs.size()) : name;
}

/// A registry entry's name, without the namespace, or empty.
[[nodiscard]] std::string_view registry_name(const LookContext& context, std::string_view registry,
                                             i64 id) {
    if (context.registries == nullptr || id < 0) {
        return {};
    }
    const auto found = context.registries->find(registry);
    if (!found) {
        return {};
    }
    return strip_ns(
        context.registries->entry_of(*found, static_cast<registry::ProtocolId>(id)));
}

constexpr std::array<std::string_view, 7> kVillagerTypes{"desert", "jungle", "plains", "savanna",
                                                         "snow",   "swamp",  "taiga"};
constexpr std::array<std::string_view, 15> kProfessions{
    "armorer",   "butcher", "cartographer", "cleric",   "farmer",
    "fisherman", "fletcher", "leatherworker", "librarian", "mason",
    "nitwit",    "none",    "shepherd",      "toolsmith", "weaponsmith"};
constexpr std::array<std::string_view, 5> kLevels{"stone", "iron", "gold", "emerald", "diamond"};
constexpr std::array<std::string_view, 11> kCatVariants{
    "all_black", "black", "british_shorthair", "calico", "jellie", "persian",
    "ragdoll",   "red",   "siamese",           "tabby",  "white"};
constexpr std::array<std::string_view, 7> kHorseColours{"white", "creamy", "chestnut", "brown",
                                                        "black", "gray",   "darkbrown"};
constexpr std::array<std::string_view, 5> kHorseMarkings{"", "white", "whitefield", "whitedots",
                                                         "blackdots"};
/// Rabbit types 0..5; 99 is the killer bunny.
constexpr std::array<std::string_view, 6> kRabbitTypes{"brown", "white", "black",
                                                       "white_splotched", "gold", "salt"};
/// Boat types in their metadata order.
constexpr std::array<std::string_view, 9> kBoatTypes{"oak",    "spruce",   "birch",
                                                     "jungle", "acacia",   "cherry",
                                                     "dark_oak", "mangrove", "bamboo"};
constexpr std::array<std::string_view, 7> kArmorMaterials{"leather", "chainmail", "iron", "golden",
                                                          "diamond", "netherite", "turtle"};

/// The villager (or zombie villager) layers on top of the base skin.
void villager_layers(EntityLook& look, const EntityTraits& traits, const LookContext& context,
                     std::string_view model, std::string_view folder, u8 index) {
    const MetaSlot& data = traits.slots[index];
    const std::string_view type =
        data.present ? registry_name(context, "minecraft:villager_type", data.villager[0])
                     : std::string_view{"plains"};
    const std::string_view profession =
        data.present ? registry_name(context, "minecraft:villager_profession", data.villager[1])
                     : std::string_view{"none"};
    const i32 level = data.present ? data.villager[2] : 1;

    const std::string type_texture = tex(std::string(folder) + "/type/" + std::string(type));
    const std::string profession_texture =
        tex(std::string(folder) + "/profession/" + std::string(profession));

    HatKind type_hat       = HatKind::None;
    HatKind profession_hat = HatKind::None;
    if (context.hats != nullptr) {
        type_hat       = context.hats->of(type_texture);
        profession_hat = context.hats->of(profession_texture);
    }
    const bool hide_type_hat =
        profession_hat == HatKind::Full ||
        (profession_hat == HatKind::Partial && type_hat == HatKind::Full);

    if (!type.empty()) {
        look.layers.push_back(LookLayer{std::string(model), type_texture, LayerPass::Cutout,
                                        0xFFFFFFFFU, hide_type_hat, ArmorPart::All});
    }
    if (look.baby || profession.empty() || profession == "none") {
        return;
    }
    look.layers.push_back(LookLayer{std::string(model), profession_texture, LayerPass::Cutout,
                                    0xFFFFFFFFU, false, ArmorPart::All});
    if (profession == "nitwit") {
        return;
    }
    const usize level_index = static_cast<usize>(std::clamp(level, 1, 5) - 1);
    look.layers.push_back(LookLayer{
        std::string(model),
        tex(std::string(folder) + "/profession_level/" + std::string(kLevels[level_index])),
        LayerPass::Cutout, 0xFFFFFFFFU, false, ArmorPart::All});
}

/// The armour a humanoid wears, as the game's armour layer draws it: the
/// outer model for the helmet, the chestplate and the boots, the inner one for
/// the leggings.
void armor_layers(EntityLook& look, const EntityTraits& traits, std::string_view mob) {
    struct Slot {
        usize            equipment;
        std::string_view piece;
        ArmorPart        part;
        bool             inner;
    };
    constexpr std::array<Slot, 4> kSlots{{{5, "_helmet", ArmorPart::Head, false},
                                          {4, "_chestplate", ArmorPart::Chest, false},
                                          {3, "_leggings", ArmorPart::Legs, true},
                                          {2, "_boots", ArmorPart::Feet, false}}};
    for (const Slot& slot : kSlots) {
        const std::string_view item = strip_ns(traits.equipment[slot.equipment]);
        if (!item.ends_with(slot.piece)) {
            continue;
        }
        const std::string_view material = item.substr(0, item.size() - slot.piece.size());
        if (std::find(kArmorMaterials.begin(), kArmorMaterials.end(), material) ==
            kArmorMaterials.end()) {
            continue;
        }
        // The file is named after the material as the armour model knows it:
        // golden armour is `gold_layer_1`.
        const std::string_view file = material == "golden" ? std::string_view{"gold"} : material;
        std::string texture(kNs);
        texture += "models/armor/";
        texture += file;
        texture += slot.inner ? "_layer_2" : "_layer_1";
        // Undyed leather is the game's default leather colour.
        const u32 tint = material == "leather" ? 0xFFA06540U : 0xFFFFFFFFU;
        look.layers.push_back(LookLayer{std::string(mob) + (slot.inner ? "#inner_armor" : "#outer_armor"),
                                        texture, LayerPass::Cutout, tint, false, slot.part});
        if (material == "leather") {
            // The undyed overlay on top of the dyed leather.
            std::string overlay = texture + "_overlay";
            look.layers.push_back(LookLayer{std::string(mob) + (slot.inner ? "#inner_armor" : "#outer_armor"),
                                            overlay, LayerPass::Cutout, 0xFFFFFFFFU, false, slot.part});
        }
    }
}

void add(EntityLook& look, std::string_view model, std::string texture,
         LayerPass pass = LayerPass::Cutout, u32 tint = 0xFFFFFFFFU) {
    look.layers.push_back(
        LookLayer{std::string(model), std::move(texture), pass, tint, false, ArmorPart::All});
}

/// A humanoid mob: skin, then armour.
void humanoid(EntityLook& look, const EntityTraits& traits, std::string_view model,
              std::string texture, EntityAnimation animation) {
    look.animation = animation;
    add(look, model, std::move(texture));
    armor_layers(look, traits, model);
}

/// The baby of a quadruped: half the size, the head its parent's full size.
/// The head stays where half the adult model puts it: the published Bedrock
/// offset (0, 4, 4) put a baby cow's head 2 pixels too high and 2 too far back
/// against the game's own vertices, and (0, 0, 0) matches them exactly
/// (docs/provenance/rendu-entites.md § 9).
void quadruped_baby(EntityLook& look) {
    look.scale *= 0.5F;
    look.baby_head_scale  = 2.0F;
    look.baby_head_offset = Vec3f{};
}

void humanoid_baby(EntityLook& look) {
    look.scale *= 0.5F;
    look.baby_head_scale = 1.5F;
}

}  // namespace

EntityKind entity_kind(std::string_view type_name) noexcept {
    const std::string_view name = strip_ns(type_name);
    if (name == "item") {
        return EntityKind::DroppedItem;
    }
    if (name == "experience_orb") {
        return EntityKind::ExperienceOrb;
    }
    if (name == "tnt" || name == "falling_block") {
        return EntityKind::Block;
    }
    if (name == "item_frame" || name == "glow_item_frame") {
        return EntityKind::ItemFrame;
    }
    if (name == "snowball" || name == "egg" || name == "ender_pearl" || name == "potion" ||
        name == "experience_bottle" || name == "fireball" || name == "small_fireball" ||
        name == "eye_of_ender") {
        return EntityKind::ThrownItem;
    }
    if (name == "dragon_fireball") {
        return EntityKind::Billboard;
    }
    if (name == "area_effect_cloud" || name == "marker" || name == "interaction") {
        return EntityKind::Invisible;
    }
    if (name == "lightning_bolt" || name == "arrow" || name == "spectral_arrow" ||
        name == "trident" || name == "fishing_bobber" || name == "leash_knot" ||
        name == "painting" || name == "evoker_fangs" || name == "shulker_bullet" ||
        name == "wither_skull" || name == "llama_spit" || name == "firework_rocket") {
        return EntityKind::Refused;
    }
    const EntityLook look = resolve_look(type_name, EntityTraits{}, LookContext{});
    return look.layers.empty() ? EntityKind::Unknown : EntityKind::Model;
}

std::string_view refusal_reason(std::string_view type_name) noexcept {
    const std::string_view name = strip_ns(type_name);
    if (name == "lightning_bolt") {
        return "a lightning bolt is a random branching drawn by code, from a seed the "
               "protocol does not carry";
    }
    if (name == "arrow" || name == "spectral_arrow" || name == "trident") {
        return "drawn by code from its texture, not from a model the client dump contains";
    }
    return "drawn by code, not from a model the client dump contains";
}

EntityLook resolve_look(std::string_view type_name, const EntityTraits& traits,
                        const LookContext& context) {
    EntityLook             look;
    const std::string_view name = strip_ns(type_name);
    look.origin                 = Vec3f{0.0F, kLivingOrigin, 0.0F};
    look.upside_down            = traits.name == "Dinnerbone" || traits.name == "Grumm";
    // Index 16 is a baby only on the ageable mobs and the zombies. On a slime
    // it is the size, on a creeper the swell, on a dragon the phase: reading it
    // as a baby everywhere drew every size-2 slime at a quarter of its size.
    static constexpr std::array<std::string_view, 25> kBabyAt16{
        "zombie",  "husk",  "drowned", "zombie_villager", "zombified_piglin", "villager",
        "cow",     "mooshroom", "pig", "sheep",   "chicken", "wolf", "cat",  "ocelot", "fox",
        "rabbit",  "horse", "donkey", "mule",     "llama",   "hoglin", "zoglin", "strider",
        "goat",    "polar_bear"};
    look.baby = std::find(kBabyAt16.begin(), kBabyAt16.end(), name) != kBabyAt16.end() &&
                traits.integer(meta::kBaby) != 0;

    // ── Humanoids ────────────────────────────────────────────────────────────
    if (name == "player") {
        humanoid(look, traits, "player", tex("player/wide/steve"), EntityAnimation::Humanoid);
    } else if (name == "zombie") {
        humanoid(look, traits, "zombie", tex("zombie/zombie"), EntityAnimation::ZombieArms);
    } else if (name == "husk") {
        humanoid(look, traits, "husk", tex("zombie/husk"), EntityAnimation::ZombieArms);
    } else if (name == "drowned") {
        humanoid(look, traits, "drowned", tex("zombie/drowned"), EntityAnimation::ZombieArms);
        add(look, "drowned#outer", tex("zombie/drowned_outer_layer"));
    } else if (name == "zombified_piglin") {
        humanoid(look, traits, "zombified_piglin", tex("piglin/zombified_piglin"),
                 EntityAnimation::ZombieArms);
    } else if (name == "zombie_villager") {
        look.animation = EntityAnimation::ZombieArms;
        add(look, "zombie_villager", tex("zombie_villager/zombie_villager"));
        villager_layers(look, traits, context, "zombie_villager", "zombie_villager",
                        meta::kZombieVillagerData);
        armor_layers(look, traits, "zombie_villager");
    } else if (name == "skeleton") {
        humanoid(look, traits, "skeleton", tex("skeleton/skeleton"), EntityAnimation::Humanoid);
    } else if (name == "stray") {
        humanoid(look, traits, "stray", tex("skeleton/stray"), EntityAnimation::Humanoid);
        add(look, "stray#outer", tex("skeleton/stray_overlay"));
    } else if (name == "wither_skeleton") {
        humanoid(look, traits, "wither_skeleton", tex("skeleton/wither_skeleton"),
                 EntityAnimation::Humanoid);
    } else if (name == "piglin" || name == "piglin_brute") {
        look.baby = name == "piglin" && traits.integer(meta::kPiglinBaby) != 0;
        humanoid(look, traits, name, tex("piglin/" + std::string(name)),
                 EntityAnimation::Humanoid);
    } else if (name == "enderman") {
        look.animation = EntityAnimation::Enderman;
        add(look, "enderman", tex("enderman/enderman"));
        add(look, "enderman", tex("enderman/enderman_eyes"), LayerPass::Eyes);
    } else if (name == "witch") {
        look.animation = EntityAnimation::Villager;
        add(look, "witch", tex("witch"));
    } else if (name == "villager") {
        look.animation = EntityAnimation::Villager;
        add(look, "villager", tex("villager/villager"));
        villager_layers(look, traits, context, "villager", "villager", meta::kVillagerData);
    } else if (name == "wandering_trader") {
        look.animation = EntityAnimation::Villager;
        add(look, "wandering_trader", tex("wandering_trader"));
    } else if (name == "iron_golem") {
        look.animation = EntityAnimation::Humanoid;
        add(look, "iron_golem", tex("iron_golem/iron_golem"));
    }
    // ── Monsters ─────────────────────────────────────────────────────────────
    else if (name == "creeper") {
        look.animation = EntityAnimation::Quadruped;
        add(look, "creeper", tex("creeper/creeper"));
        if (traits.integer(meta::kCreeperPowered) != 0) {
            add(look, "creeper#armor", tex("creeper/creeper_armor"), LayerPass::Energy);
        }
    } else if (name == "spider" || name == "cave_spider") {
        look.animation = EntityAnimation::Spider;
        add(look, name, tex("spider/" + std::string(name)));
        add(look, name, tex("spider_eyes"), LayerPass::Eyes);
    } else if (name == "slime") {
        add(look, "slime", tex("slime/slime"));
        add(look, "slime#outer", tex("slime/slime"), LayerPass::Translucent);
    } else if (name == "magma_cube") {
        add(look, "magma_cube", tex("slime/magmacube"));
    } else if (name == "blaze") {
        look.animation = EntityAnimation::Blaze;
        add(look, "blaze", tex("blaze"));
    } else if (name == "ghast") {
        look.animation = EntityAnimation::Ghast;
        add(look, "ghast", tex(traits.integer(meta::kGhastCharging) != 0 ? "ghast/ghast_shooting"
                                                                          : "ghast/ghast"));
    } else if (name == "hoglin" || name == "zoglin") {
        look.animation = EntityAnimation::Quadruped;
        add(look, name, tex("hoglin/" + std::string(name)));
    } else if (name == "strider") {
        add(look, "strider", tex(traits.integer(meta::kStriderCold) != 0 ? "strider/strider_cold"
                                                                          : "strider/strider"));
        if (traits.integer(meta::kStriderSaddle) != 0) {
            add(look, "strider#saddle", tex("strider/strider_saddle"));
        }
    } else if (name == "ender_dragon") {
        // Posed from the flap and the flight history (entity_pose.hpp,
        // pose_dragon); drawn as one model per neck and tail segment.
        add(look, "ender_dragon", tex("enderdragon/dragon"));
        add(look, "ender_dragon", tex("enderdragon/dragon_eyes"), LayerPass::Eyes);
    }
    // ── Animals ──────────────────────────────────────────────────────────────
    else if (name == "cow" || name == "mooshroom") {
        look.animation = EntityAnimation::Quadruped;
        add(look, name == "cow" ? "cow" : "mooshroom",
            tex(name == "cow" ? "cow/cow" : "cow/red_mooshroom"));
    } else if (name == "pig") {
        look.animation = EntityAnimation::Quadruped;
        add(look, "pig", tex("pig/pig"));
        if (traits.integer(meta::kPigSaddle) != 0) {
            add(look, "pig#saddle", tex("pig/pig_saddle"));
        }
    } else if (name == "sheep") {
        look.animation = EntityAnimation::Quadruped;
        add(look, "sheep", tex("sheep/sheep"));
        const i64 fleece = traits.integer(meta::kSheepFleece);
        // 0x10 is sheared; the low four bits are the dye.
        if ((fleece & 0x10) == 0) {
            add(look, "sheep#fur", tex("sheep/sheep_fur"), LayerPass::Cutout,
                0xFF000000U | sheep_fleece_colour(fleece & 0x0F));
        }
    } else if (name == "chicken") {
        look.animation = EntityAnimation::Chicken;
        add(look, "chicken", tex("chicken"));
    } else if (name == "wolf") {
        look.animation     = EntityAnimation::Quadruped;
        const bool tame    = traits.flag(meta::kTameFlags, meta::kTamed);
        const bool angry   = traits.integer(meta::kWolfAnger) > 0;
        add(look, "wolf", tex(tame ? "wolf/wolf_tame" : angry ? "wolf/wolf_angry" : "wolf/wolf"));
        if (tame) {
            add(look, "wolf", tex("wolf/wolf_collar"), LayerPass::Cutout,
                0xFF000000U | dye_colour(traits.integer(meta::kWolfCollar, 14)));
        }
    } else if (name == "cat") {
        look.animation = EntityAnimation::Feline;
        std::string_view variant =
            traits.has(meta::kCatVariant)
                ? registry_name(context, "minecraft:cat_variant", traits.integer(meta::kCatVariant))
                : std::string_view{"tabby"};
        if (variant.empty()) {
            variant = "tabby";
        }
        add(look, "cat", tex("cat/" + std::string(variant)));
        if (traits.flag(meta::kTameFlags, meta::kTamed)) {
            add(look, "cat#collar", tex("cat/cat_collar"), LayerPass::Cutout,
                0xFF000000U | dye_colour(traits.integer(meta::kCatCollar, 14)));
        }
    } else if (name == "ocelot") {
        look.animation = EntityAnimation::Feline;
        add(look, "ocelot", tex("cat/ocelot"));
    } else if (name == "fox") {
        look.animation = EntityAnimation::Quadruped;
        add(look, "fox", tex(traits.integer(meta::kFoxType) == 1 ? "fox/snow_fox" : "fox/fox"));
    } else if (name == "rabbit") {
        const i64 type = traits.integer(meta::kRabbitType);
        std::string skin;
        if (traits.name == "Toast") {
            skin = "toast";
        } else if (type == 99) {
            skin = "caerbannog";
        } else {
            skin = std::string(kRabbitTypes[static_cast<usize>(std::clamp<i64>(type, 0, 5))]);
        }
        add(look, "rabbit", tex("rabbit/" + skin));
    } else if (name == "horse") {
        look.animation      = EntityAnimation::Quadruped;
        const i64 variant   = traits.integer(meta::kHorseVariant);
        const auto colour   = static_cast<usize>(std::clamp<i64>(variant & 0xFF, 0, 6));
        const auto markings = static_cast<usize>(std::clamp<i64>((variant >> 8) & 0xFF, 0, 4));
        add(look, "horse", tex("horse/horse_" + std::string(kHorseColours[colour])));
        if (markings != 0) {
            add(look, "horse",
                tex("horse/horse_markings_" + std::string(kHorseMarkings[markings])),
                LayerPass::Translucent);
        }
    }
    // ── Things that are not alive ────────────────────────────────────────────
    else if (name == "armor_stand") {
        look.animation = EntityAnimation::ArmorStand;
        look.living    = false;
        add(look, "armor_stand", tex("armorstand/wood"));
        // The `hat` cube is the humanoid model's second head layer: the game
        // draws none on the bare stand (ten cubes, not eleven), but a helmet
        // over it keeps its own.
        look.layers.back().hide_bare_hat = true;
        armor_layers(look, traits, "armor_stand");
        if (traits.flag(meta::kArmorStandFlags, meta::kArmorStandSmall)) {
            look.scale = 0.5F;
        }
    } else if (name == "minecart" || name == "chest_minecart" || name == "furnace_minecart" ||
               name == "tnt_minecart" || name == "hopper_minecart" ||
               name == "spawner_minecart" || name == "command_block_minecart") {
        look.living = false;
        look.origin = Vec3f{};
        look.lift   = 0.375F;
        add(look, name, tex("minecart"));
    } else if (name == "boat" || name == "chest_boat") {
        look.animation   = EntityAnimation::Boat;
        look.living      = false;
        look.origin      = Vec3f{};
        look.lift        = 0.375F;
        look.model_yaw   = 90.0F;
        const auto type  = static_cast<usize>(std::clamp<i64>(traits.integer(meta::kBoatType), 0, 8));
        const std::string wood(kBoatTypes[type]);
        add(look, std::string(name) + "/" + wood, tex(std::string(name) + "/" + wood));
        // The water patch is drawn by the game only into the depth buffer, to
        // keep water out of the hull; drawn here it would be a tenth cube.
        look.hidden.emplace_back("water_patch");
    } else if (name == "end_crystal") {
        look.animation = EntityAnimation::EndCrystal;
        look.living    = false;
        look.origin    = Vec3f{};
        add(look, "end_crystal", tex("end_crystal/end_crystal"));
    }

    // ── The renderer's own scale, measured ─────────────────────────────────
    // Each is the ratio of the game's emitted box to this model's at scale 1,
    // from the vertices the running client emitted for the same entity
    // (docs/provenance/rendu-entites.md, § 9): 0.7000, 1.2002, 1.0628, 4.5000,
    // and 0.999 per unit of a slime's size.
    if (name == "cave_spider") {
        look.scale *= 0.7F;
    } else if (name == "wither_skeleton") {
        look.scale *= 1.2F;
    } else if (name == "husk") {
        look.scale *= 1.0625F;
    } else if (name == "ghast") {
        look.scale *= 4.5F;
    } else if (name == "slime" || name == "magma_cube") {
        look.scale *= 0.999F * static_cast<f32>(std::max<i64>(1, traits.integer(meta::kSlimeSize, 1)));
    } else if (name == "villager" || name == "witch" || name == "wandering_trader") {
        look.scale *= 0.9375F;  // 2.023 / 2.158 and 0.469 / 0.500, every axis
    } else if (name == "rabbit") {
        look.scale *= 0.6F;  // 0.638 / 1.063, 0.150 / 0.250, 0.352 / 0.587
    } else if (name == "cat") {
        look.scale *= 0.8F;  // 0.125 / 0.156 across, 0.601 / 0.751 up
    } else if (name == "horse") {
        look.scale *= 1.1F;  // 2.322 / 2.111, 0.347 / 0.316, 1.620 / 1.473
    } else if (name == "end_crystal") {
        // Twice the model, its base's bottom a block under the feet: the
        // game's base spans y −1.0..−0.5 and ±0.75 across.
        look.scale *= 2.0F;
        look.lift = -0.5F;
    }

    // ── The parts the game shows or hides by state ──────────────────────────
    if (name == "player" || name == "piglin" || name == "piglin_brute" ||
        name == "zombified_piglin") {
        // The cloak and the ears are drawn by layers of their own, not with
        // the body; the player model carries them all the same.
        look.hidden = {"cloak", "ear"};
        if (name == "zombified_piglin") {
            // One ear short: the game's vertices carry fifteen cubes, the
            // model's sixteen less the one at its right ear.
            look.hidden.emplace_back("right_ear");
        }
    } else if (name == "horse") {
        // Adult legs or baby legs, never both; the saddle only when saddled
        // (horse flags, 0x04), the reins only when ridden — which the look
        // does not know, so never.
        const bool saddled = traits.flag(meta::kTameFlags, 0x04);
        for (const std::string_view leg : {"right_hind", "left_hind", "right_front", "left_front"}) {
            look.hidden.push_back(std::string(leg) + (look.baby ? "_leg" : "_baby_leg"));
        }
        look.hidden.emplace_back("left_saddle_line");
        look.hidden.emplace_back("right_saddle_line");
        if (!saddled) {
            for (const std::string_view part : {"saddle", "head_saddle", "mouth_saddle_wrap",
                                                "left_saddle_mouth", "right_saddle_mouth"}) {
                look.hidden.emplace_back(part);
            }
        }
    } else if (name == "armor_stand") {
        if (traits.flag(meta::kArmorStandFlags, meta::kArmorStandNoBasePlate)) {
            look.hidden.emplace_back("base_plate");
        }
        if (!traits.flag(meta::kArmorStandFlags, meta::kArmorStandArms)) {
            look.hidden.emplace_back("left_arm");
            look.hidden.emplace_back("right_arm");
        }
    } else if (name == "end_crystal") {
        if (traits.has(meta::kEndCrystalBottom) && traits.integer(meta::kEndCrystalBottom) == 0) {
            look.hidden.emplace_back("base");
        }
    }

    if (look.baby && look.living) {
        if (look.animation == EntityAnimation::Quadruped ||
            look.animation == EntityAnimation::Feline || look.animation == EntityAnimation::Chicken) {
            quadruped_baby(look);
            // Each species' head offset, model units: the game's head cubes
            // against ours at (0, 0, 0), world blocks × 32 (docs § 3).
            if (name == "pig") {
                look.baby_head_offset = Vec3f{0.0F, 4.0F, 2.0F};
            } else if (name == "sheep") {
                look.baby_head_offset = Vec3f{0.0F, 2.0F, 0.0F};
            } else if (name == "wolf") {
                look.baby_head_offset = Vec3f{-1.0F, 0.5F, -3.0F};
            } else if (name == "chicken") {
                look.baby_head_offset = Vec3f{0.0F, -1.0F, 0.0F};
            } else if (name == "horse") {
                // The whole foal stands 0.138 higher in the game: two pixels
                // at the horse's 1.1.
                look.lift += 0.1375F;
            } else if (name == "cat" || name == "ocelot") {
                // The head at 0.75 of its parent's, not its parent's size: the
                // game's head cubes are 0.75 of the adult's on every axis.
                look.baby_head_scale  = 1.5F;
                look.baby_head_offset = Vec3f{0.0F, 1.5F, 1.5F};
            } else if (name == "fox") {
                look.baby_head_scale  = 1.5F;
                look.baby_head_offset = Vec3f{-0.5F, 3.75F, 3.5F};
            } else if (name == "hoglin" || name == "zoglin") {
                // 0.789 of the parent's head (1.5 / 1.9), measured.
                look.baby_head_scale  = 2.0F * 1.5F / 1.9F;
                look.baby_head_offset = Vec3f{0.0F, 10.2F, 2.5F};
            }
        } else if (name == "rabbit") {
            // A baby rabbit's body is 0.4 of the model and its head 0.567 —
            // not half of the adult's 0.6: the game's body cube is 1.333 of
            // what half the adult gave, its head 1.26.
            look.scale            = 0.4F;
            look.baby_head_scale  = 0.56666666F / 0.4F;
            look.baby_head_offset = Vec3f{0.0F, -1.8F, 2.4F};
        } else if (look.animation == EntityAnimation::Villager) {
            // Half its parent on every axis, head included: the game's box for
            // a baby villager is exactly half the adult's.
            look.scale *= 0.5F;
        } else {
            humanoid_baby(look);
        }
    }
    return look;
}

void list_look_textures(const LookContext& context, std::vector<std::string>& out) {
    (void)context;
    const auto push = [&](std::string texture) {
        if (std::find(out.begin(), out.end(), texture) == out.end()) {
            out.push_back(std::move(texture));
        }
    };
    for (const std::string_view path :
         {"player/wide/steve", "zombie/zombie", "zombie/husk", "zombie/drowned",
          "zombie/drowned_outer_layer", "piglin/zombified_piglin", "piglin/piglin",
          "piglin/piglin_brute", "zombie_villager/zombie_villager", "skeleton/skeleton",
          "skeleton/stray", "skeleton/stray_overlay", "skeleton/wither_skeleton",
          "enderman/enderman", "enderman/enderman_eyes", "witch", "villager/villager",
          "wandering_trader", "iron_golem/iron_golem", "creeper/creeper", "spider/spider",
          "spider/cave_spider", "spider_eyes", "slime/slime", "slime/magmacube", "blaze",
          "ghast/ghast", "ghast/ghast_shooting", "hoglin/hoglin", "hoglin/zoglin",
          "strider/strider", "strider/strider_cold", "strider/strider_saddle",
          "enderdragon/dragon", "enderdragon/dragon_eyes", "enderdragon/dragon_fireball",
          "cow/cow", "cow/red_mooshroom", "pig/pig", "pig/pig_saddle", "sheep/sheep",
          "sheep/sheep_fur", "chicken", "wolf/wolf", "wolf/wolf_tame", "wolf/wolf_angry",
          "wolf/wolf_collar", "cat/cat_collar", "cat/ocelot", "fox/fox", "fox/snow_fox",
          "rabbit/toast", "rabbit/caerbannog", "armorstand/wood", "minecart",
          "end_crystal/end_crystal", "end_crystal/end_crystal_beam", "experience_orb"}) {
        push(tex(path));
    }
    for (const std::string_view folder : {"villager", "zombie_villager"}) {
        for (const std::string_view type : kVillagerTypes) {
            push(tex(std::string(folder) + "/type/" + std::string(type)));
        }
        for (const std::string_view profession : kProfessions) {
            if (profession != "none") {
                push(tex(std::string(folder) + "/profession/" + std::string(profession)));
            }
        }
        for (const std::string_view level : kLevels) {
            push(tex(std::string(folder) + "/profession_level/" + std::string(level)));
        }
    }
    for (const std::string_view variant : kCatVariants) {
        push(tex("cat/" + std::string(variant)));
    }
    for (const std::string_view type : kRabbitTypes) {
        push(tex("rabbit/" + std::string(type)));
    }
    for (const std::string_view colour : kHorseColours) {
        push(tex("horse/horse_" + std::string(colour)));
    }
    for (usize marking = 1; marking < kHorseMarkings.size(); ++marking) {
        push(tex("horse/horse_markings_" + std::string(kHorseMarkings[marking])));
    }
    for (const std::string_view wood : kBoatTypes) {
        push(tex("boat/" + std::string(wood)));
        push(tex("chest_boat/" + std::string(wood)));
    }
    for (const std::string_view material : kArmorMaterials) {
        const std::string_view file = material == "golden" ? std::string_view{"gold"} : material;
        for (const std::string_view layer : {"_layer_1", "_layer_2"}) {
            std::string texture(kNs);
            texture += "models/armor/";
            texture += file;
            texture += layer;
            push(texture);
            if (material == "leather") {
                push(texture + "_overlay");
            }
        }
    }
}

u32 dye_colour(i64 dye) noexcept {
    return kDye[static_cast<usize>(std::clamp<i64>(dye, 0, 15))];
}

u32 sheep_fleece_colour(i64 dye) noexcept {
    return kFleece[static_cast<usize>(std::clamp<i64>(dye, 0, 15))];
}

VillagerHats VillagerHats::load(const AssetSource& source) {
    VillagerHats hats;
    std::vector<std::string> textures;
    for (const std::string_view folder : {"villager", "zombie_villager"}) {
        for (const std::string_view type : kVillagerTypes) {
            textures.push_back(std::string(folder) + "/type/" + std::string(type));
        }
        for (const std::string_view profession : kProfessions) {
            textures.push_back(std::string(folder) + "/profession/" + std::string(profession));
        }
    }
    for (const std::string& path : textures) {
        const std::string file = "assets/minecraft/textures/entity/" + path + ".png.mcmeta";
        const auto        bytes = source.read(file);
        if (!bytes) {
            continue;
        }
        auto document = json::Document::parse(*bytes);
        if (!document) {
            continue;
        }
        const std::string_view hat = document->root()["villager"]["hat"].as_string();
        HatKind                kind = HatKind::None;
        if (hat == "full") {
            kind = HatKind::Full;
        } else if (hat == "partial") {
            kind = HatKind::Partial;
        }
        hats.set(tex(path), kind);
    }
    return hats;
}

HatKind VillagerHats::of(const std::string& texture) const noexcept {
    const auto found = hats_.find(texture);
    return found == hats_.end() ? HatKind::None : found->second;
}

void apply_look_visibility(const EntityModel& model, const EntityLook& look,
                           std::vector<BonePose>& poses) {
    for (const std::string& bone : look.hidden) {
        const i32 index = model.bone(bone);
        if (index >= 0 && static_cast<usize>(index) < poses.size()) {
            poses[static_cast<usize>(index)].hidden = true;
        }
    }
}

void apply_layer_visibility(const EntityModel& model, const LookLayer& layer,
                            std::vector<BonePose>& poses) {
    const auto hide = [&](std::string_view bone) {
        const i32 index = model.bone(bone);
        if (index >= 0 && static_cast<usize>(index) < poses.size()) {
            poses[static_cast<usize>(index)].hidden = true;
        }
    };
    if (layer.hide_hat) {
        // The whole head, not only the hat: the game's vertices for a
        // librarian's biome layer carry no head, nose, hat or rim — seven
        // cubes of eleven (docs/provenance/rendu-entites.md § 9).
        hide("head");
        hide("nose");
        hide("hat");
        hide("hat_rim");
    }
    if (layer.hide_bare_hat) {
        hide("hat");
    }
    if (layer.armor == ArmorPart::All) {
        return;
    }
    // The game's armour layer shows, per piece: the helmet on the head and
    // hat, the chestplate on the body and both arms, the leggings on the body
    // and both legs (inner model), the boots on both legs.
    constexpr std::array<std::string_view, 7> kBones{"head",     "hat",       "body",    "right_arm",
                                                     "left_arm", "right_leg", "left_leg"};
    for (const std::string_view bone : kBones) {
        bool shown = false;
        switch (layer.armor) {
            case ArmorPart::Head:
                shown = bone == "head" || bone == "hat";
                break;
            case ArmorPart::Chest:
                shown = bone == "body" || bone == "right_arm" || bone == "left_arm";
                break;
            case ArmorPart::Legs:
                shown = bone == "body" || bone == "right_leg" || bone == "left_leg";
                break;
            case ArmorPart::Feet:
                shown = bone == "right_leg" || bone == "left_leg";
                break;
            case ArmorPart::All:
                shown = true;
                break;
        }
        if (!shown) {
            hide(bone);
        }
    }
}

}  // namespace ov::render
