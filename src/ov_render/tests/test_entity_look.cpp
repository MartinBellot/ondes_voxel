// Which layers and which textures an entity is drawn with, from the numbers the
// server sent — asserted without a GPU and without a server.
//
// The metadata indices are vanilla's (entity_look.hpp says where they were
// read); the textures are file names of the 1.20.1 pack. A wrong index here is
// not an error at run time, it is a lime sheep drawn white or a librarian
// drawn as an unemployed plains villager, so each case pins one choice.
#include "ov/registry/registries.hpp"
#include "ov/render/entity_look.hpp"
#include "ov/render/entity_model.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <optional>

using namespace ov;
using namespace ov::render;

namespace {

/// The registry pack is generated locally and never committed; a test that
/// needs registry names skips without it, like the chunk mesher's.
[[nodiscard]] std::optional<registry::Registries> try_load_registries() {
    for (const auto* candidate :
         {"data/vanilla/1.20.1/registry.ovpack", "../data/vanilla/1.20.1/registry.ovpack",
          "../../data/vanilla/1.20.1/registry.ovpack",
          "../../../data/vanilla/1.20.1/registry.ovpack"}) {
        if (!std::filesystem::exists(candidate)) {
            continue;
        }
        auto loaded = registry::Registries::load(candidate);
        if (loaded) {
            return std::move(*loaded);
        }
    }
    return std::nullopt;
}

void set_integer(EntityTraits& traits, u8 index, i64 value) {
    traits.slots[index].present = true;
    traits.slots[index].integer = value;
}

[[nodiscard]] std::vector<std::string> textures_of(const EntityLook& look) {
    std::vector<std::string> out;
    for (const LookLayer& layer : look.layers) {
        out.push_back(layer.texture);
    }
    return out;
}

}  // namespace

TEST_CASE("a sheep's fleece takes its dye, and a sheared sheep has none", "[entity][look]") {
    EntityTraits traits;
    EntityLook   look = resolve_look("minecraft:sheep", traits, LookContext{});
    REQUIRE(look.layers.size() == 2);
    CHECK(look.layers[0].model == "sheep");
    CHECK(look.layers[1].model == "sheep#fur");
    // White wool is not the dye's white: the game tints a fleece 0.902 grey.
    CHECK(look.layers[1].tint == 0xFFE6E6E6U);

    set_integer(traits, meta::kSheepFleece, 5);  // lime
    look = resolve_look("minecraft:sheep", traits, LookContext{});
    REQUIRE(look.layers.size() == 2);
    CHECK(look.layers[1].tint == 0xFF609517U);

    set_integer(traits, meta::kSheepFleece, 0x10 | 5);  // sheared lime
    look = resolve_look("minecraft:sheep", traits, LookContext{});
    CHECK(look.layers.size() == 1);
}

TEST_CASE("a baby is half its parent with a bigger head", "[entity][look]") {
    EntityTraits traits;
    EntityLook   adult = resolve_look("minecraft:cow", traits, LookContext{});
    CHECK(adult.scale == Catch::Approx(1.0F));
    CHECK(adult.origin.y == Catch::Approx(24.016F));
    CHECK(adult.animation == EntityAnimation::Quadruped);

    set_integer(traits, meta::kBaby, 1);
    EntityLook baby = resolve_look("minecraft:cow", traits, LookContext{});
    CHECK(baby.baby);
    CHECK(baby.scale == Catch::Approx(0.5F));
    CHECK(baby.baby_head_scale == Catch::Approx(2.0F));

    // A piglin keeps its baby flag one index later than every other mob.
    EntityTraits piglin;
    set_integer(piglin, meta::kBaby, 1);  // 16 is immunity, not babyhood
    CHECK_FALSE(resolve_look("minecraft:piglin", piglin, LookContext{}).baby);
    set_integer(piglin, meta::kPiglinBaby, 1);
    CHECK(resolve_look("minecraft:piglin", piglin, LookContext{}).baby);

    // Index 16 is not a baby flag everywhere: a swelling creeper, a size-2
    // slime and a perched dragon are none of them babies.
    EntityTraits sixteen;
    set_integer(sixteen, meta::kBaby, 1);
    CHECK_FALSE(resolve_look("minecraft:creeper", sixteen, LookContext{}).baby);
    CHECK_FALSE(resolve_look("minecraft:ender_dragon", sixteen, LookContext{}).baby);
    CHECK(resolve_look("minecraft:zombie", sixteen, LookContext{}).baby);
}

TEST_CASE("the measured renderer scales and the parts each renderer hides", "[entity][look]") {
    const EntityTraits none;
    CHECK(resolve_look("minecraft:villager", none, LookContext{}).scale == Catch::Approx(0.9375F));
    CHECK(resolve_look("minecraft:witch", none, LookContext{}).scale == Catch::Approx(0.9375F));
    CHECK(resolve_look("minecraft:rabbit", none, LookContext{}).scale == Catch::Approx(0.6F));
    CHECK(resolve_look("minecraft:cat", none, LookContext{}).scale == Catch::Approx(0.8F));
    CHECK(resolve_look("minecraft:horse", none, LookContext{}).scale == Catch::Approx(1.1F));
    const EntityLook crystal = resolve_look("minecraft:end_crystal", none, LookContext{});
    CHECK(crystal.scale == Catch::Approx(2.0F));
    CHECK(crystal.lift == Catch::Approx(-0.5F));

    const auto hidden = [](const EntityLook& look, std::string_view bone) {
        return std::find(look.hidden.begin(), look.hidden.end(), bone) != look.hidden.end();
    };
    // One ear short on a zombified piglin, never on a piglin.
    const EntityLook zombified = resolve_look("minecraft:zombified_piglin", none, LookContext{});
    CHECK(hidden(zombified, "right_ear"));
    CHECK_FALSE(hidden(zombified, "left_ear"));
    CHECK_FALSE(hidden(resolve_look("minecraft:piglin", none, LookContext{}), "right_ear"));
    // The boat's water patch is a depth mask, and its oars are posed.
    const EntityLook boat = resolve_look("minecraft:boat", none, LookContext{});
    CHECK(hidden(boat, "water_patch"));
    CHECK(boat.animation == EntityAnimation::Boat);
    // The stand's bare hat is hidden on its own layer only: a helmet keeps its hat.
    EntityTraits helmet;
    helmet.equipment[5] = "minecraft:iron_helmet";
    const EntityLook stand = resolve_look("minecraft:armor_stand", helmet, LookContext{});
    REQUIRE(stand.layers.size() == 2);
    CHECK(stand.layers[0].hide_bare_hat);
    CHECK_FALSE(stand.layers[1].hide_bare_hat);
    CHECK_FALSE(hidden(stand, "hat"));
}

TEST_CASE("a baby's head is sized and placed per species, as measured", "[entity][look]") {
    EntityTraits baby;
    set_integer(baby, meta::kBaby, 1);
    const EntityLook pig = resolve_look("minecraft:pig", baby, LookContext{});
    CHECK(pig.baby_head_scale == Catch::Approx(2.0F));
    CHECK(pig.baby_head_offset.y == Catch::Approx(4.0F));
    CHECK(resolve_look("minecraft:cow", baby, LookContext{}).baby_head_offset.y ==
          Catch::Approx(0.0F));
    CHECK(resolve_look("minecraft:cat", baby, LookContext{}).baby_head_scale == Catch::Approx(1.5F));
    const EntityLook rabbit = resolve_look("minecraft:rabbit", baby, LookContext{});
    CHECK(rabbit.scale == Catch::Approx(0.4F));
    // A baby villager is its parent halved, head and all.
    const EntityLook villager = resolve_look("minecraft:villager", baby, LookContext{});
    CHECK(villager.scale == Catch::Approx(0.9375F * 0.5F));
    CHECK(villager.baby_head_scale == Catch::Approx(1.0F));
}

TEST_CASE("a renderer's own scale is the game's", "[entity][look]") {
    // Ratios of the box the running client emitted to the model's own box.
    const EntityTraits none;
    CHECK(resolve_look("minecraft:cave_spider", none, LookContext{}).scale == Catch::Approx(0.7F));
    CHECK(resolve_look("minecraft:wither_skeleton", none, LookContext{}).scale ==
          Catch::Approx(1.2F));
    CHECK(resolve_look("minecraft:ghast", none, LookContext{}).scale == Catch::Approx(4.5F));
    CHECK(resolve_look("minecraft:husk", none, LookContext{}).scale == Catch::Approx(1.0625F));

    // A slime grows with its size; size 1 (the smallest, `Size:0b`) is one unit.
    EntityTraits slime;
    set_integer(slime, meta::kSlimeSize, 1);
    const f32 small = resolve_look("minecraft:slime", slime, LookContext{}).scale;
    set_integer(slime, meta::kSlimeSize, 4);
    CHECK(resolve_look("minecraft:slime", slime, LookContext{}).scale ==
          Catch::Approx(4.0F * small));
    CHECK_FALSE(resolve_look("minecraft:slime", slime, LookContext{}).baby);
}

TEST_CASE("glowing eyes, a charged creeper and a slime's jelly take their own pass",
          "[entity][look]") {
    EntityTraits traits;
    const EntityLook enderman = resolve_look("minecraft:enderman", traits, LookContext{});
    REQUIRE(enderman.layers.size() == 2);
    CHECK(enderman.layers[1].pass == LayerPass::Eyes);
    CHECK(enderman.layers[1].texture == "minecraft:entity/enderman/enderman_eyes");

    const EntityLook spider = resolve_look("minecraft:cave_spider", traits, LookContext{});
    REQUIRE(spider.layers.size() == 2);
    CHECK(spider.layers[0].texture == "minecraft:entity/spider/cave_spider");
    CHECK(spider.layers[1].pass == LayerPass::Eyes);

    CHECK(resolve_look("minecraft:creeper", traits, LookContext{}).layers.size() == 1);
    set_integer(traits, meta::kCreeperPowered, 1);
    const EntityLook charged = resolve_look("minecraft:creeper", traits, LookContext{});
    REQUIRE(charged.layers.size() == 2);
    CHECK(charged.layers[1].model == "creeper#armor");
    CHECK(charged.layers[1].pass == LayerPass::Energy);

    const EntityLook slime = resolve_look("minecraft:slime", EntityTraits{}, LookContext{});
    REQUIRE(slime.layers.size() == 2);
    CHECK(slime.layers[1].model == "slime#outer");
    CHECK(slime.layers[1].pass == LayerPass::Translucent);
}

TEST_CASE("a villager is its skin, its biome, its profession and its badge", "[entity][look]") {
    const auto registries = try_load_registries();
    if (!registries) {
        SKIP("registry.ovpack is absent; generate it with tools/ov_datagen/ovpack.py");
    }
    const auto types       = registries->find("minecraft:villager_type");
    const auto professions = registries->find("minecraft:villager_profession");
    REQUIRE(types.has_value());
    REQUIRE(professions.has_value());
    const auto plains    = registries->protocol_id(*types, "minecraft:plains");
    const auto librarian = registries->protocol_id(*professions, "minecraft:librarian");
    const auto nitwit    = registries->protocol_id(*professions, "minecraft:nitwit");
    REQUIRE(plains.has_value());
    REQUIRE(librarian.has_value());
    REQUIRE(nitwit.has_value());

    EntityTraits traits;
    traits.slots[meta::kVillagerData].present  = true;
    traits.slots[meta::kVillagerData].villager = {*plains, *librarian, 3};
    const LookContext context{&*registries, nullptr};

    const EntityLook look = resolve_look("minecraft:villager", traits, context);
    CHECK(textures_of(look) ==
          std::vector<std::string>{"minecraft:entity/villager/villager",
                                   "minecraft:entity/villager/type/plains",
                                   "minecraft:entity/villager/profession/librarian",
                                   "minecraft:entity/villager/profession_level/gold"});

    // A nitwit wears its clothes but never a badge; a baby wears neither.
    traits.slots[meta::kVillagerData].villager = {*plains, *nitwit, 1};
    CHECK(resolve_look("minecraft:villager", traits, context).layers.size() == 3);
    set_integer(traits, meta::kBaby, 1);
    CHECK(resolve_look("minecraft:villager", traits, context).layers.size() == 2);
}

TEST_CASE("a cat is its registry variant; a rabbit, a horse and a boat are their numbers",
          "[entity][look]") {
    const auto registries = try_load_registries();
    if (registries) {
        const auto cats = registries->find("minecraft:cat_variant");
        REQUIRE(cats.has_value());
        const auto jellie = registries->protocol_id(*cats, "minecraft:jellie");
        REQUIRE(jellie.has_value());
        EntityTraits cat;
        set_integer(cat, meta::kCatVariant, *jellie);
        const EntityLook look = resolve_look("minecraft:cat", cat, LookContext{&*registries, nullptr});
        REQUIRE_FALSE(look.layers.empty());
        CHECK(look.layers[0].texture == "minecraft:entity/cat/jellie");
    }

    EntityTraits rabbit;
    set_integer(rabbit, meta::kRabbitType, 99);
    CHECK(resolve_look("minecraft:rabbit", rabbit, LookContext{}).layers[0].texture ==
          "minecraft:entity/rabbit/caerbannog");
    rabbit.name = "Toast";
    CHECK(resolve_look("minecraft:rabbit", rabbit, LookContext{}).layers[0].texture ==
          "minecraft:entity/rabbit/toast");

    EntityTraits horse;
    set_integer(horse, meta::kHorseVariant, 3 | (2 << 8));  // brown, white field
    const EntityLook horse_look = resolve_look("minecraft:horse", horse, LookContext{});
    CHECK(textures_of(horse_look) ==
          std::vector<std::string>{"minecraft:entity/horse/horse_brown",
                                   "minecraft:entity/horse/horse_markings_whitefield"});

    EntityTraits boat;
    set_integer(boat, meta::kBoatType, 5);
    const EntityLook boat_look = resolve_look("minecraft:boat", boat, LookContext{});
    REQUIRE(boat_look.layers.size() == 1);
    CHECK(boat_look.layers[0].model == "boat/cherry");
    CHECK(boat_look.layers[0].texture == "minecraft:entity/boat/cherry");
    CHECK(boat_look.lift == Catch::Approx(0.375F));
    CHECK(boat_look.origin.y == Catch::Approx(0.0F));
    CHECK_FALSE(boat_look.living);
}

TEST_CASE("armour is drawn on the parts one piece covers", "[entity][look]") {
    EntityTraits traits;
    traits.equipment[5] = "minecraft:iron_helmet";
    traits.equipment[3] = "minecraft:leather_leggings";
    const EntityLook look = resolve_look("minecraft:zombie", traits, LookContext{});
    REQUIRE(look.layers.size() == 4);
    CHECK(look.layers[1].model == "zombie#outer_armor");
    CHECK(look.layers[1].texture == "minecraft:models/armor/iron_layer_1");
    CHECK(look.layers[1].armor == ArmorPart::Head);
    CHECK(look.layers[2].model == "zombie#inner_armor");
    CHECK(look.layers[2].texture == "minecraft:models/armor/leather_layer_2");
    CHECK(look.layers[2].tint == 0xFFA06540U);
    CHECK(look.layers[3].texture == "minecraft:models/armor/leather_layer_2_overlay");

    // A helmet layer hides everything but the head and the hat.
    constexpr std::string_view kArmorModel = R"({"format": 2, "models": {"a": {"texture_width": 64,
        "texture_height": 32, "bones": [
          {"name": "head", "parent": "", "pivot": [0, 0, 0]},
          {"name": "hat", "parent": "", "pivot": [0, 0, 0]},
          {"name": "body", "parent": "", "pivot": [0, 0, 0]},
          {"name": "right_arm", "parent": "", "pivot": [0, 0, 0]},
          {"name": "left_leg", "parent": "", "pivot": [0, 0, 0]}]}}})";
    const auto set = EntityModelSet::parse(
        std::span<const u8>(reinterpret_cast<const u8*>(kArmorModel.data()), kArmorModel.size()));
    REQUIRE(set.has_value());
    const EntityModel*    model = set->find("a");
    REQUIRE(model != nullptr);
    std::vector<BonePose> poses(model->bones.size());
    apply_layer_visibility(*model, look.layers[1], poses);
    CHECK_FALSE(poses[0].hidden);
    CHECK_FALSE(poses[1].hidden);
    CHECK(poses[2].hidden);
    CHECK(poses[3].hidden);
    CHECK(poses[4].hidden);
}

TEST_CASE("the parts the game hides by state are hidden", "[entity][look]") {
    // An adult horse draws its adult legs and not the baby ones, and no saddle
    // it does not wear; the player model's cloak and ears belong to layers of
    // their own. All of these are bones of the game's own models (the dump
    // carries them visible), so drawing them would double a horse's legs.
    EntityTraits     traits;
    const EntityLook horse = resolve_look("minecraft:horse", traits, LookContext{});
    const auto hides = [](const EntityLook& look, std::string_view bone) {
        return std::find(look.hidden.begin(), look.hidden.end(), bone) != look.hidden.end();
    };
    CHECK(hides(horse, "right_hind_baby_leg"));
    CHECK_FALSE(hides(horse, "right_hind_leg"));
    CHECK(hides(horse, "saddle"));

    set_integer(traits, meta::kTameFlags, 0x04);  // saddled
    set_integer(traits, meta::kBaby, 1);
    const EntityLook foal = resolve_look("minecraft:horse", traits, LookContext{});
    CHECK(hides(foal, "right_hind_leg"));
    CHECK_FALSE(hides(foal, "right_hind_baby_leg"));
    CHECK_FALSE(hides(foal, "saddle"));

    const EntityLook player = resolve_look("minecraft:player", EntityTraits{}, LookContext{});
    CHECK(hides(player, "cloak"));
    CHECK(hides(player, "ear"));
}

TEST_CASE("every entity kind is named, and nothing unknown is drawn as something else",
          "[entity][look]") {
    CHECK(entity_kind("minecraft:zombie") == EntityKind::Model);
    CHECK(entity_kind("minecraft:ender_dragon") == EntityKind::Model);
    CHECK(entity_kind("minecraft:end_crystal") == EntityKind::Model);
    CHECK(entity_kind("minecraft:chest_minecart") == EntityKind::Model);
    CHECK(entity_kind("minecraft:item") == EntityKind::DroppedItem);
    CHECK(entity_kind("minecraft:experience_orb") == EntityKind::ExperienceOrb);
    CHECK(entity_kind("minecraft:tnt") == EntityKind::Block);
    CHECK(entity_kind("minecraft:falling_block") == EntityKind::Block);
    CHECK(entity_kind("minecraft:snowball") == EntityKind::ThrownItem);
    CHECK(entity_kind("minecraft:dragon_fireball") == EntityKind::Billboard);
    CHECK(entity_kind("minecraft:area_effect_cloud") == EntityKind::Invisible);
    CHECK(entity_kind("minecraft:lightning_bolt") == EntityKind::Refused);
    CHECK_FALSE(refusal_reason("minecraft:lightning_bolt").empty());
    CHECK(entity_kind("minecraft:not_a_thing") == EntityKind::Unknown);
}

TEST_CASE("the atlas list holds every variant once", "[entity][look]") {
    std::vector<std::string> textures;
    list_look_textures(LookContext{}, textures);
    const auto has = [&](std::string_view name) {
        return std::find(textures.begin(), textures.end(), name) != textures.end();
    };
    CHECK(has("minecraft:entity/villager/profession/librarian"));
    CHECK(has("minecraft:entity/zombie_villager/profession_level/diamond"));
    CHECK(has("minecraft:entity/cat/british_shorthair"));
    CHECK(has("minecraft:entity/boat/bamboo"));
    CHECK(has("minecraft:models/armor/gold_layer_2"));
    CHECK(has("minecraft:models/armor/leather_layer_1_overlay"));
    std::vector<std::string> sorted = textures;
    std::sort(sorted.begin(), sorted.end());
    CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
}

TEST_CASE("the dye table is the game's", "[entity][look]") {
    CHECK(dye_colour(14) == 0xB02E26U);
    CHECK(sheep_fleece_colour(0) == 0xE6E6E6U);
    CHECK(sheep_fleece_colour(15) == 0x161619U);
    // Out of range clamps rather than reading past the table.
    CHECK(dye_colour(99) == dye_colour(15));
}
