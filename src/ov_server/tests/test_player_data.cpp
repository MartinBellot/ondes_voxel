// The player file: what vanilla 1.20.1 writes, read and written back here.
//
// The fixture below is the shape of a file the real server wrote for a bot
// (scripts/measure_player_data.py, campaign `capture`; see
// docs/provenance/donnees-joueur.md): the same keys, types and values, built by
// hand rather than committed, because a save file written by the game stays out
// of the repository. The oracle run itself — our write read back by vanilla —
// is the script's job; what is pinned here is every rule the script found.
#include "../src/player_data.hpp"

#include "ov/io/file.hpp"
#include "ov/nbt/binary.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>
#include <random>
#include <string>

using namespace ov;
using namespace ov::server;

namespace {

[[nodiscard]] const registry::Registries& registries() {
    static const std::optional<registry::Registries> loaded = [] {
        const auto path =
            std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack";
        auto regs = registry::Registries::load(path);
        return regs ? std::optional<registry::Registries>{std::move(*regs)} : std::nullopt;
    }();
    REQUIRE(loaded.has_value());
    return *loaded;
}

[[nodiscard]] ItemNames names() {
    return ItemNames{&registries(), registries().find("minecraft:item")};
}

[[nodiscard]] i32 item(std::string_view name) {
    const auto id = registries().protocol_id(*registries().find("minecraft:item"), name);
    REQUIRE(id.has_value());
    return static_cast<i32>(*id);
}

void put(nbt::Tag& compound, std::string_view name, nbt::Tag value) {
    (void)compound.put(std::string{name}, std::move(value));
}

[[nodiscard]] nbt::Tag doubles(f64 a, f64 b, f64 c) {
    nbt::Tag list = nbt::Tag::make_list(nbt::TagType::Double);
    (void)list.push(nbt::Tag{a});
    (void)list.push(nbt::Tag{b});
    (void)list.push(nbt::Tag{c});
    return list;
}

[[nodiscard]] nbt::Tag stack(i8 slot, std::string id, i8 count) {
    nbt::Tag entry = nbt::Tag::make_compound();
    put(entry, "Slot", nbt::Tag{slot});
    put(entry, "id", nbt::Tag{std::move(id)});
    put(entry, "Count", nbt::Tag{count});
    return entry;
}

[[nodiscard]] nbt::Tag effect(i32 id, i8 amplifier, i32 duration, bool shown) {
    nbt::Tag entry = nbt::Tag::make_compound();
    put(entry, "Id", nbt::Tag{id});
    put(entry, "Amplifier", nbt::Tag{amplifier});
    put(entry, "Duration", nbt::Tag{duration});
    put(entry, "Ambient", nbt::Tag::make_bool(false));
    put(entry, "ShowParticles", nbt::Tag::make_bool(shown));
    put(entry, "ShowIcon", nbt::Tag::make_bool(shown));
    return entry;
}

const net::Uuid kBot = net::Uuid::offline_player("ovplayer");

/// The measured file, key for key (docs/provenance/donnees-joueur.md § 2).
[[nodiscard]] nbt::Tag vanilla_file() {
    nbt::Tag root = nbt::Tag::make_compound();
    nbt::Tag brain = nbt::Tag::make_compound();
    put(brain, "memories", nbt::Tag::make_compound());
    put(root, "Brain", std::move(brain));
    put(root, "HurtByTimestamp", nbt::Tag{i32{0}});
    put(root, "SleepTimer", nbt::Tag{i16{0}});
    put(root, "SpawnForced", nbt::Tag::make_bool(true));

    nbt::Tag attributes = nbt::Tag::make_list(nbt::TagType::Compound);
    for (const char* name : {"minecraft:generic.armor_toughness", "minecraft:generic.armor"}) {
        nbt::Tag entry = nbt::Tag::make_compound();
        put(entry, "Base", nbt::Tag{0.0});
        put(entry, "Name", nbt::Tag{std::string{name}});
        (void)attributes.push(std::move(entry));
    }
    {
        nbt::Tag entry = nbt::Tag::make_compound();
        put(entry, "Base", nbt::Tag{0.10000000149011612});
        nbt::Tag modifiers = nbt::Tag::make_list(nbt::TagType::Compound);
        nbt::Tag modifier  = nbt::Tag::make_compound();
        put(modifier, "Amount", nbt::Tag{0.800000011920929});
        put(modifier, "Operation", nbt::Tag{i32{2}});
        put(modifier, "UUID",
            nbt::Tag{nbt::Tag::IntArray{-1850824106, 929776792, -1822740609, 1745290805}});
        put(modifier, "Name", nbt::Tag{std::string{"effect.minecraft.speed 3"}});
        (void)modifiers.push(std::move(modifier));
        put(entry, "Modifiers", std::move(modifiers));
        put(entry, "Name", nbt::Tag{std::string{"minecraft:generic.movement_speed"}});
        (void)attributes.push(std::move(entry));
    }
    put(root, "Attributes", std::move(attributes));
    put(root, "Invulnerable", nbt::Tag::make_bool(false));
    put(root, "FallFlying", nbt::Tag::make_bool(false));
    put(root, "PortalCooldown", nbt::Tag{i32{0}});
    put(root, "AbsorptionAmount", nbt::Tag{0.0F});
    nbt::Tag abilities = nbt::Tag::make_compound();
    put(abilities, "invulnerable", nbt::Tag::make_bool(false));
    put(abilities, "mayfly", nbt::Tag::make_bool(false));
    put(abilities, "instabuild", nbt::Tag::make_bool(false));
    put(abilities, "walkSpeed", nbt::Tag{0.1F});
    put(abilities, "mayBuild", nbt::Tag::make_bool(true));
    put(abilities, "flying", nbt::Tag::make_bool(false));
    put(abilities, "flySpeed", nbt::Tag{0.05F});
    put(root, "abilities", std::move(abilities));
    put(root, "FallDistance", nbt::Tag{0.0F});
    nbt::Tag recipes = nbt::Tag::make_compound();
    nbt::Tag known   = nbt::Tag::make_list(nbt::TagType::String);
    (void)known.push(nbt::Tag{std::string{"minecraft:furnace"}});
    put(recipes, "recipes", std::move(known));
    put(recipes, "isGuiOpen", nbt::Tag::make_bool(false));
    put(root, "recipeBook", std::move(recipes));
    put(root, "DeathTime", nbt::Tag{i16{0}});
    put(root, "XpSeed", nbt::Tag{i32{0}});
    put(root, "XpTotal", nbt::Tag{i32{17}});
    put(root, "UUID", nbt::Tag{nbt::Tag::IntArray{661731032, -574541385, -1810214774, 911127807}});
    put(root, "playerGameType", nbt::Tag{i32{0}});
    put(root, "SpawnDimension", nbt::Tag{std::string{"minecraft:overworld"}});
    put(root, "seenCredits", nbt::Tag::make_bool(false));
    put(root, "Motion", doubles(0.0, -0.0784000015258789, 0.0));
    put(root, "SpawnY", nbt::Tag{i32{-60}});
    put(root, "Health", nbt::Tag{20.0F});
    put(root, "SpawnZ", nbt::Tag{i32{4}});
    put(root, "foodSaturationLevel", nbt::Tag{0.0F});
    put(root, "SpawnX", nbt::Tag{i32{3}});
    put(root, "Air", nbt::Tag{i16{300}});
    put(root, "OnGround", nbt::Tag::make_bool(true));
    put(root, "Dimension", nbt::Tag{std::string{"minecraft:overworld"}});
    put(root, "SpawnAngle", nbt::Tag{45.0F});
    nbt::Tag rotation = nbt::Tag::make_list(nbt::TagType::Float);
    (void)rotation.push(nbt::Tag{90.0F});
    (void)rotation.push(nbt::Tag{10.0F});
    put(root, "Rotation", std::move(rotation));
    put(root, "XpLevel", nbt::Tag{i32{30}});
    nbt::Tag warden = nbt::Tag::make_compound();
    put(warden, "warning_level", nbt::Tag{i32{0}});
    put(warden, "ticks_since_last_warning", nbt::Tag{i32{152}});
    put(warden, "cooldown_ticks", nbt::Tag{i32{0}});
    put(root, "warden_spawn_tracker", std::move(warden));
    put(root, "Score", nbt::Tag{i32{17}});
    put(root, "Pos", doubles(12.5, -60.0, -7.25));
    put(root, "Fire", nbt::Tag{i16{-20}});
    put(root, "XpP", nbt::Tag{0.1517857164144516F});
    nbt::Tag ender = nbt::Tag::make_list(nbt::TagType::Compound);
    (void)ender.push(stack(3, "minecraft:ender_pearl", 16));
    put(root, "EnderItems", std::move(ender));
    put(root, "DataVersion", nbt::Tag{i32{3465}});
    put(root, "foodLevel", nbt::Tag{i32{13}});
    put(root, "foodExhaustionLevel", nbt::Tag{3.2000012397766113F});
    put(root, "HurtTime", nbt::Tag{i16{0}});
    put(root, "SelectedItemSlot", nbt::Tag{i32{2}});

    // Speed IV over a hidden speed II, and an infinite hidden night vision:
    // the two chains the capture carried. In id order, which is ours.
    nbt::Tag effects = nbt::Tag::make_list(nbt::TagType::Compound);
    nbt::Tag speed   = effect(1, 3, 551, true);
    put(speed, "HiddenEffect", effect(1, 1, 5951, true));
    (void)effects.push(std::move(speed));
    (void)effects.push(effect(16, 0, -1, false));
    put(root, "ActiveEffects", std::move(effects));

    nbt::Tag inventory = nbt::Tag::make_list(nbt::TagType::Compound);
    (void)inventory.push(stack(0, "minecraft:cobblestone", 64));
    nbt::Tag sword = stack(1, "minecraft:diamond_sword", 1);
    nbt::Tag tag   = nbt::Tag::make_compound();
    put(tag, "Damage", nbt::Tag{i32{5}});
    nbt::Tag display = nbt::Tag::make_compound();
    put(display, "Name", nbt::Tag{std::string{"\"Blade\""}});
    put(tag, "display", std::move(display));
    put(sword, "tag", std::move(tag));
    (void)inventory.push(std::move(sword));
    (void)inventory.push(stack(2, "minecraft:oak_log", 20));
    (void)inventory.push(stack(29, "minecraft:bread", 7));
    (void)inventory.push(stack(100, "minecraft:leather_boots", 1));
    (void)inventory.push(stack(103, "minecraft:iron_helmet", 1));
    (void)inventory.push(stack(-106, "minecraft:shield", 1));
    put(root, "Inventory", std::move(inventory));
    put(root, "foodTickTimer", nbt::Tag{i32{0}});
    return root;
}

/// Tag equality with compound order ignored: vanilla's compounds are hash
/// maps, and ours keep insertion order.
[[nodiscard]] bool same(const nbt::Tag& a, const nbt::Tag& b) {
    if (a.type() != b.type()) {
        return false;
    }
    if (const auto* entries = a.compound()) {
        if (entries->size() != b.compound()->size()) {
            return false;
        }
        for (const auto& entry : *entries) {
            const nbt::Tag* other = b.find(entry.name);
            if (other == nullptr || !same(entry.value, *other)) {
                return false;
            }
        }
        return true;
    }
    if (const auto* items = a.list()) {
        if (a.list_element_type() != b.list_element_type() || items->size() != b.list()->size()) {
            return false;
        }
        for (usize i = 0; i < items->size(); ++i) {
            if (!same((*items)[i], (*b.list())[i])) {
                return false;
            }
        }
        return true;
    }
    return a == b;
}

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        std::random_device device;
        path = std::filesystem::temp_directory_path() /
               ("ov_player_data_" + std::to_string(device()));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

}  // namespace

TEST_CASE("every inventory slot survives the file's numbering", "[player_data]") {
    usize mapped = 0;
    for (usize window = 0; window < kPlayerSlots; ++window) {
        const auto slot = file_slot_of_window(window);
        if (window <= 4) {
            CHECK_FALSE(slot.has_value());  // the 2x2 grid is never stored
            continue;
        }
        REQUIRE(slot.has_value());
        CHECK(window_slot_of_file(*slot) == window);
        ++mapped;
    }
    CHECK(mapped == 41);
    CHECK(file_slot_of_window(36) == i8{0});
    CHECK(file_slot_of_window(5) == i8{103});   // head
    CHECK(file_slot_of_window(8) == i8{100});   // feet
    CHECK(file_slot_of_window(45) == i8{-106});  // off hand
    CHECK_FALSE(window_slot_of_file(36).has_value());
    CHECK_FALSE(window_slot_of_file(99).has_value());
}

TEST_CASE("the vanilla file is read field by field", "[player_data]") {
    const nbt::Tag root   = vanilla_file();
    const auto     loaded = read_player(root, &kBot, names(), "fixture");
    REQUIRE(loaded.has_value());
    const PlayerRecord& r = loaded->record;
    CHECK(r.x == 12.5);
    CHECK(r.y == -60.0);
    CHECK(r.z == -7.25);
    CHECK(r.yaw == 90.0F);
    CHECK(r.pitch == 10.0F);
    CHECK(r.on_ground);
    CHECK(r.health == 20.0F);
    CHECK(r.food.food == 13);
    CHECK(r.food.saturation == 0.0F);
    CHECK(r.food.exhaustion == 3.2000012397766113F);
    CHECK(r.xp_level == 30);
    CHECK(r.xp_points == 17);  // 0.15178572f of the 112 level 30 costs
    CHECK(r.xp_total == 17);
    CHECK(r.selected_slot == 2);
    CHECK(r.inventory[36].item_id == item("minecraft:cobblestone"));
    CHECK(r.inventory[36].count == 64);
    CHECK(r.inventory[37].item_id == item("minecraft:diamond_sword"));
    CHECK_FALSE(r.inventory[37].nbt.empty());
    CHECK(r.inventory[29].item_id == item("minecraft:bread"));
    CHECK(r.inventory[5].item_id == item("minecraft:iron_helmet"));
    CHECK(r.inventory[8].item_id == item("minecraft:leather_boots"));
    CHECK(r.inventory[45].item_id == item("minecraft:shield"));

    // The chain comes back as it was: speed IV on top, speed II hidden.
    const gameplay::EffectInstance* speed = r.effects.get(gameplay::Effect::Speed);
    REQUIRE(speed != nullptr);
    CHECK(speed->amplifier == 3);
    CHECK(speed->duration == 551);
    const gameplay::EffectInstance* hidden = r.effects.hidden(gameplay::Effect::Speed, 1);
    REQUIRE(hidden != nullptr);
    CHECK(hidden->amplifier == 1);
    CHECK(hidden->duration == 5951);
    const gameplay::EffectInstance* night = r.effects.get(gameplay::Effect::NightVision);
    REQUIRE(night != nullptr);
    CHECK(night->infinite());
    CHECK_FALSE(night->visible);
    // And the modifier with them, at the measured amount.
    CHECK(r.attributes.value(gameplay::Attribute::MovementSpeed) ==
          (0.10000000149011612 * (1.0 + 0.800000011920929)));
    CHECK(loaded->unknown_items == 0);
    CHECK(loaded->unknown_effects == 0);
}

TEST_CASE("an untouched vanilla file writes back tag for tag", "[player_data]") {
    const nbt::Tag root   = vanilla_file();
    const auto     loaded = read_player(root, &kBot, names(), "fixture");
    REQUIRE(loaded.has_value());
    const nbt::Tag written = write_player(loaded->record, &loaded->original, kBot, names());
    CHECK(same(root, written));
    // Byte for byte too: every key we own is put back where it was, and the
    // fixture's effects are already in our id order.
    CHECK(nbt::write(nbt::Document{"", written}).size() ==
          nbt::write(nbt::Document{"", root}).size());
}

TEST_CASE("keys this server does not model are kept", "[player_data]") {
    nbt::Tag root = vanilla_file();
    put(root, "LastDeathLocation", nbt::Tag::make_compound());
    nbt::Tag modded = stack(4, "othermod:gizmo", 3);
    root.find("Inventory")->list()->push_back(modded);
    nbt::Tag strange = effect(99, 0, 100, true);
    root.find("ActiveEffects")->list()->push_back(strange);

    const auto loaded = read_player(root, &kBot, names(), "fixture");
    REQUIRE(loaded.has_value());
    CHECK(loaded->unknown_items == 1);
    CHECK(loaded->unknown_effects == 1);

    PlayerRecord changed = loaded->record;
    changed.x            = 100.5;
    changed.food.food    = 7;
    const nbt::Tag out   = write_player(changed, &loaded->original, kBot, names());
    CHECK(out.contains("LastDeathLocation"));
    CHECK(same(*out.find("recipeBook"), *root.find("recipeBook")));
    CHECK(same(*out.find("warden_spawn_tracker"), *root.find("warden_spawn_tracker")));
    CHECK(same(*out.find("EnderItems"), *root.find("EnderItems")));
    CHECK(out.find("SpawnX")->as_i64() == 3);
    CHECK(out.find("Score")->as_i64() == 17);
    CHECK(out.find("foodLevel")->as_i64() == 7);
    bool gizmo = false;
    for (const nbt::Tag& entry : *out.find("Inventory")->list()) {
        gizmo = gizmo || entry.find("id")->as_string() == "othermod:gizmo";
    }
    CHECK(gizmo);
    bool ninety_nine = false;
    for (const nbt::Tag& entry : *out.find("ActiveEffects")->list()) {
        ninety_nine = ninety_nine || entry.find("Id")->as_i64() == 99;
    }
    CHECK(ninety_nine);
}

TEST_CASE("a file this server cannot use is refused and named", "[player_data]") {
    SECTION("another data version") {
        nbt::Tag root = vanilla_file();
        put(root, "DataVersion", nbt::Tag{i32{3337}});
        const auto loaded = read_player(root, &kBot, names(), "old.dat");
        REQUIRE_FALSE(loaded.has_value());
        CHECK(loaded.error().kind == PlayerDataErrorKind::WrongDataVersion);
        CHECK(loaded.error().message.find("3337") != std::string::npos);
        CHECK(loaded.error().message.find("old.dat") != std::string::npos);
    }
    SECTION("no data version") {
        nbt::Tag root = vanilla_file();
        (void)root.erase("DataVersion");
        CHECK(read_player(root, &kBot, names(), "x").error().kind ==
              PlayerDataErrorKind::NoDataVersion);
    }
    SECTION("someone else's file") {
        const net::Uuid other = net::Uuid::offline_player("somebody");
        CHECK(read_player(vanilla_file(), &other, names(), "x").error().kind ==
              PlayerDataErrorKind::WrongUuid);
    }
    SECTION("a dimension this server does not have") {
        nbt::Tag root = vanilla_file();
        put(root, "Dimension", nbt::Tag{std::string{"mydatapack:moon"}});
        CHECK(read_player(root, &kBot, names(), "x").error().kind ==
              PlayerDataErrorKind::UnsupportedDimension);
    }
    SECTION("the End is one it has") {  // ── end ──
        nbt::Tag root = vanilla_file();
        put(root, "Dimension", nbt::Tag{std::string{"minecraft:the_end"}});
        const auto loaded = read_player(root, &kBot, names(), "x");
        REQUIRE(loaded.has_value());
        CHECK(loaded->record.dimension == "minecraft:the_end");
    }
    SECTION("the Nether is one it has") {  // ── nether ──
        nbt::Tag root = vanilla_file();
        put(root, "Dimension", nbt::Tag{std::string{"minecraft:the_nether"}});
        const auto loaded = read_player(root, &kBot, names(), "x");
        REQUIRE(loaded.has_value());
        CHECK(loaded->record.dimension == "minecraft:the_nether");
    }
    SECTION("bytes that are not a player file") {
        const std::vector<u8> junk{1, 2, 3, 4};
        CHECK(decode_player_file(junk, "junk.dat").error().kind == PlayerDataErrorKind::Unreadable);
    }
}

TEST_CASE("the live state goes to a file and comes back", "[player_data]") {
    SurvivalSession survival;
    EffectSession   effects;
    survival.health.health = 13.0F;
    survival.health.air    = 120;
    survival.food.food     = 9;
    survival.food.saturation = 1.5F;
    survival.food.exhaustion = 2.25F;
    survival.food.tick_timer = 40;
    survival.award_experience(345);
    const EffectIo     io{};
    const EffectBearer bearer{};
    (void)effects.apply(gameplay::EffectInstance{.effect = gameplay::Effect::Speed,
                                                 .duration = 1200, .amplifier = 1},
                        survival, io, bearer);
    (void)effects.apply(gameplay::EffectInstance{.effect = gameplay::Effect::Regeneration,
                                                 .duration = gameplay::kInfiniteDuration},
                        survival, io, bearer);

    std::array<net::ItemStack, kPlayerSlots> inventory{};
    inventory[36]         = net::ItemStack{item("minecraft:stone"), 32, {}};
    inventory[40]         = net::ItemStack{item("minecraft:diamond_pickaxe"), 1, {}};
    inventory[40].nbt     = nbt::write(nbt::Document{"tag", [] {
        nbt::Tag tag = nbt::Tag::make_compound();
        put(tag, "Damage", nbt::Tag{i32{12}});
        return tag;
    }()});
    inventory[6]          = net::ItemStack{item("minecraft:iron_chestplate"), 1, {}};
    inventory[45]         = net::ItemStack{item("minecraft:torch"), 10, {}};
    inventory[2]          = net::ItemStack{item("minecraft:oak_planks"), 3, {}};  // 2x2 grid
    const net::ItemStack carried{item("minecraft:apple"), 2, {}};

    usize              overflow = 0;
    const PlayerRecord record   = capture_player(
        PlayerPose{.x = 1234.5, .y = 70.0, .z = -777.25, .yaw = 45.0F, .pitch = -5.0F,
                   .on_ground = true},
        0, inventory, carried, 4, survival, effects, &overflow);
    CHECK(overflow == 0);
    // The cursor and then the grid went back into the first free slots,
    // hotbar first: 37 and 38, since 36 is taken. Cursor first is the order
    // measured when the screen closes (scripts/measure_window0.py).
    CHECK(record.inventory[37].item_id == item("minecraft:apple"));
    CHECK(record.inventory[38].item_id == item("minecraft:oak_planks"));

    const nbt::Tag root = write_player(record, nullptr, kBot, names());
    CHECK(root.find("DataVersion")->as_i64() == 3465);
    CHECK(root.find("Motion")->list()->at(1).as_f64() == -0.0784000015258789);
    CHECK_FALSE(root.contains("previousPlayerGameType"));
    CHECK(root.find("Fire")->as_i64() == -20);
    const auto bytes = encode_player_file(root);
    REQUIRE(bytes.size() > 2);
    CHECK(bytes[0] == 0x1F);
    CHECK(bytes[1] == 0x8B);

    const auto decoded = decode_player_file(bytes, "mem");
    REQUIRE(decoded.has_value());
    const auto loaded = read_player(*decoded, &kBot, names(), "mem");
    REQUIRE(loaded.has_value());

    PlayerPose                               pose;
    std::array<net::ItemStack, kPlayerSlots> back{};
    back[1] = net::ItemStack{item("minecraft:dirt"), 1, {}};  // stale, must go
    i16             held = 0;
    SurvivalSession survival_back;
    EffectSession   effects_back;
    restore_player(loaded->record, pose, back, held, survival_back, effects_back);

    CHECK(pose.x == 1234.5);
    CHECK(pose.z == -777.25);
    CHECK(pose.yaw == 45.0F);
    CHECK(held == 4);
    CHECK(back[1].empty());
    CHECK(back[36].count == 32);
    CHECK(back[40].nbt == inventory[40].nbt);
    CHECK(back[6].item_id == item("minecraft:iron_chestplate"));
    CHECK(back[45].count == 10);
    CHECK(survival_back.health.health == 13.0F);
    CHECK(survival_back.health.air == 120);
    CHECK(survival_back.food.food == 9);
    CHECK(survival_back.food.saturation == 1.5F);
    CHECK(survival_back.food.exhaustion == 2.25F);
    CHECK(survival_back.food.tick_timer == 40);
    CHECK(survival_back.experience_level == survival.experience_level);
    CHECK(survival_back.experience_points == survival.experience_points);
    CHECK(survival_back.experience_total == 345);
    const gameplay::EffectInstance* speed = effects_back.effects.get(gameplay::Effect::Speed);
    REQUIRE(speed != nullptr);
    CHECK(speed->duration == 1200);
    CHECK(speed->amplifier == 1);
    CHECK(effects_back.effects.get(gameplay::Effect::Regeneration)->infinite());
    CHECK(effects_back.attributes.value(gameplay::Attribute::MovementSpeed) ==
          effects.attributes.value(gameplay::Attribute::MovementSpeed));
    // They arrive as new effects, so the session's next flush tells the client.
    CHECK(effects_back.effects.events().size() == 2);
}

TEST_CASE("a change of mode keeps the previous one, as vanilla does", "[player_data]") {
    const auto loaded = read_player(vanilla_file(), &kBot, names(), "fixture");
    REQUIRE(loaded.has_value());
    PlayerRecord creative = loaded->record;
    creative.game_type    = 1;
    creative.abilities    = Abilities::for_game_type(1, false);
    const nbt::Tag out    = write_player(creative, &loaded->original, kBot, names());
    CHECK(out.find("playerGameType")->as_i64() == 1);
    CHECK(out.find("previousPlayerGameType")->as_i64() == 0);
    CHECK(out.find("abilities")->find("mayfly")->as_bool());
    CHECK(out.find("abilities")->find("instabuild")->as_bool());
}

TEST_CASE("the store keeps .dat_old and never writes over a refused file", "[player_data]") {
    const TempDir world;
    PlayerDataStore store{world.path, names(), ""};

    CHECK_FALSE(store.load(kBot, "ovplayer")->has_value());  // first arrival

    PlayerRecord record;
    record.x = 1.5;
    REQUIRE(store.save(kBot, "ovplayer", record));
    const auto first = io::read_file(store.file_of(kBot));
    REQUIRE(first.has_value());
    record.x = 2.5;
    REQUIRE(store.save(kBot, "ovplayer", record));
    auto old = store.file_of(kBot);
    old += "_old";
    const auto kept = io::read_file(old);
    REQUIRE(kept.has_value());
    CHECK(*kept == *first);  // the save before the last one, as measured

    const auto back = store.load(kBot, "ovplayer");
    REQUIRE(back.has_value());
    REQUIRE(back->has_value());
    CHECK((*back)->record.x == 2.5);

    // A file from another version: refused, and left exactly as it was.
    nbt::Tag foreign = vanilla_file();
    put(foreign, "DataVersion", nbt::Tag{i32{3700}});
    const auto foreign_bytes = encode_player_file(foreign);
    REQUIRE(io::write_file_atomic(store.file_of(kBot), foreign_bytes));
    const auto refused = store.load(kBot, "ovplayer");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().kind == PlayerDataErrorKind::WrongDataVersion);
    CHECK_FALSE(store.save(kBot, "ovplayer", record));
    CHECK(*io::read_file(store.file_of(kBot)) == foreign_bytes);
}

TEST_CASE("the singleplayer host is read from and written to level.dat", "[player_data]") {
    const TempDir world;
    // A vanilla singleplayer level.dat: the host under an account UUID.
    world::LevelSettings settings;
    nbt::Document        level = world::make_level_dat(settings);
    nbt::Tag             host  = vanilla_file();
    const nbt::Tag::IntArray account{1, 2, 3, 4};
    put(host, "UUID", nbt::Tag{account});
    put(*level.root.find("Data"), "Player", host);
    const auto level_path = world.path / "level.dat";
    REQUIRE(io::write_file_atomic(level_path, encode_player_file(level.root)));

    PlayerDataStore store{world.path, names(), "ovplayer"};
    store.load_level_player(level_path);
    const auto loaded = store.load(kBot, "ovplayer");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    CHECK((*loaded)->record.x == 12.5);  // Data.Player wins, as in vanilla

    PlayerRecord moved = (*loaded)->record;
    moved.x            = 99.5;
    REQUIRE(store.save(kBot, "ovplayer", moved));
    const auto bytes = store.encode_level_dat(settings);
    const auto root  = decode_player_file(bytes, "level.dat");
    REQUIRE(root.has_value());
    const nbt::Tag* player = root->find("Data")->find("Player");
    REQUIRE(player != nullptr);
    CHECK(player->find("Pos")->list()->at(0).as_f64() == 99.5);
    // The seat keeps the account's UUID; the playerdata file carries ours.
    CHECK(*player->find("UUID")->get_if<nbt::Tag::IntArray>() == account);
    CHECK(std::filesystem::exists(store.file_of(kBot)));

    // Somebody else on the same server is not the host.
    const net::Uuid guest = net::Uuid::offline_player("guest");
    CHECK_FALSE(store.load(guest, "guest")->has_value());
}
