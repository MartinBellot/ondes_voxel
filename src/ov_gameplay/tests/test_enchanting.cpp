// Enchantments, the table, the anvil and the grindstone.
//
// Two kinds of test. The first half pins rules the documentation states
// outright — the conflict groups, the wiki's own anvil examples, the EPF table.
// The second half replays scripts/measure_enchanting.py, which asked a real
// 1.20.1 server: every table offer it recorded must come out identical — same
// XpSeed, same bookshelves, same item, same ten numbers — and every enchantment
// a button actually applied must be the list ours draws. The measurement is
// regenerated locally and never committed; when it is missing the parity cases
// say so and skip.
#include "ov/gameplay/enchanting.hpp"

#include "ov/nbt/tag.hpp"

#include <catch2/catch_test_macros.hpp>
#include <simdjson.h>

#include <cmath>
#include <filesystem>
#include <map>
#include <string>

using namespace ov;
using namespace ov::gameplay;

namespace {

[[nodiscard]] EnchantmentList list_of(std::initializer_list<EnchantmentLevel> entries) {
    EnchantmentList out;
    for (const EnchantmentLevel& e : entries) {
        out.push(e);
    }
    return out;
}

[[nodiscard]] EnchantStack stack(std::string_view item, EnchantmentList enchantments = {},
                                 i32 damage = 0, i32 repair_cost = 0) {
    EnchantStack s;
    s.item         = item;
    s.enchantments = enchantments;
    s.damage       = damage;
    s.repair_cost  = repair_cost;
    return s;
}

[[nodiscard]] std::filesystem::path measured_file() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "normalized" /
           "enchanting.json";
}

}  // namespace

TEST_CASE("the thirty-nine are in registry order", "[enchanting]") {
    REQUIRE(enchantment_info(Enchantment::Protection).name == "minecraft:protection");
    REQUIRE(enchantment_info(Enchantment::Sharpness).name == "minecraft:sharpness");
    REQUIRE(network_id(Enchantment::Sharpness) == 13);
    REQUIRE(network_id(Enchantment::Unbreaking) == 22);
    REQUIRE(network_id(Enchantment::VanishingCurse) == 38);
    for (usize i = 0; i < kEnchantmentCount; ++i) {
        const auto e = static_cast<Enchantment>(i);
        REQUIRE(enchantment_from_name(enchantment_info(e).name) == e);
    }
    REQUIRE(enchantment_from_name("sweeping") == Enchantment::Sweeping);
    REQUIRE_FALSE(enchantment_from_name("minecraft:sweeping_edge").has_value());
}

TEST_CASE("conflicts are the wiki's groups and nothing else", "[enchanting]") {
    using E = Enchantment;
    CHECK_FALSE(compatible(E::Protection, E::BlastProtection));
    CHECK_FALSE(compatible(E::FireProtection, E::ProjectileProtection));
    CHECK(compatible(E::FeatherFalling, E::Protection));
    CHECK(compatible(E::FeatherFalling, E::BlastProtection));
    CHECK_FALSE(compatible(E::Sharpness, E::Smite));
    CHECK_FALSE(compatible(E::BaneOfArthropods, E::Smite));
    CHECK(compatible(E::Sharpness, E::Impaling));
    CHECK_FALSE(compatible(E::SilkTouch, E::Fortune));
    CHECK_FALSE(compatible(E::SilkTouch, E::Looting));
    CHECK_FALSE(compatible(E::LuckOfTheSea, E::SilkTouch));
    CHECK(compatible(E::Fortune, E::Looting));
    CHECK_FALSE(compatible(E::DepthStrider, E::FrostWalker));
    CHECK_FALSE(compatible(E::Mending, E::Infinity));
    CHECK_FALSE(compatible(E::Riptide, E::Loyalty));
    CHECK_FALSE(compatible(E::Channeling, E::Riptide));
    CHECK(compatible(E::Loyalty, E::Channeling));
    CHECK_FALSE(compatible(E::Multishot, E::Piercing));
    CHECK_FALSE(compatible(E::Efficiency, E::Efficiency));
    CHECK(compatible(E::Efficiency, E::Unbreaking));
}

TEST_CASE("level windows are the documented ones", "[enchanting]") {
    using E = Enchantment;
    CHECK(min_cost(E::Protection, 4) == 34);
    CHECK(max_cost(E::Protection, 4) == 45);
    CHECK(min_cost(E::Thorns, 3) == 50);
    CHECK(max_cost(E::Thorns, 3) == 80);
    CHECK(max_cost(E::Efficiency, 5) == 91);
    CHECK(min_cost(E::QuickCharge, 3) == 52);
    CHECK(max_cost(E::QuickCharge, 3) == 50);  // unreachable, as documented
    CHECK(min_cost(E::Loyalty, 2) == 19);
    CHECK(min_cost(E::Mending, 1) == 25);
}

TEST_CASE("the bookshelf ring and its gaps", "[enchanting]") {
    const auto offsets = bookshelf_offsets();
    REQUIRE(offsets.size() == 32);
    const BlockOffset corner = bookshelf_gap({2, 0, 2});
    CHECK((corner.dx == 1 && corner.dy == 0 && corner.dz == 1));
    const BlockOffset edge = bookshelf_gap({-2, 1, 1});
    CHECK((edge.dx == -1 && edge.dy == 1 && edge.dz == 0));
    std::array<ShelfProbe, 32> probes{};
    for (usize i = 0; i < 20; ++i) {
        probes[i] = {true, i % 5 != 0};
    }
    CHECK(count_bookshelves(probes) == 16);
}

TEST_CASE("what the table accepts", "[enchanting]") {
    CHECK(table_accepts("minecraft:book", 1, false));
    CHECK_FALSE(table_accepts("minecraft:book", 2, false));
    CHECK(table_accepts("minecraft:diamond_sword", 1, false));
    CHECK_FALSE(table_accepts("minecraft:diamond_sword", 1, true));
    CHECK_FALSE(table_accepts("minecraft:stick", 1, false));
    CHECK(enchantability("minecraft:golden_chestplate") == 25);
    CHECK(enchantability("minecraft:golden_sword") == 22);
    CHECK(enchantability("minecraft:shears") == 0);
    // An item the table takes but cannot enchant offers three zeros.
    const TableOffers shears = table_offers(1234, 15, "minecraft:shears");
    CHECK(shears.costs == std::array<i32, 3>{0, 0, 0});
}

TEST_CASE("the wiki's first enchant: seed 0, fifteen shelves, a diamond sword", "[enchanting]") {
    // "a diamond sword enchanted with 3 lapis lazuli and 15 bookshelves always
    // gets Unbreaking 3 and Looting 2" — the wiki's own statement about the
    // XpSeed every new player starts with. It runs the whole draw: the three
    // costs from the seed, the reseed at seed + 2, both modifiers, the weights
    // and the extra-enchantment loop.
    const TableOffers offers = table_offers(0, 15, "minecraft:diamond_sword");
    REQUIRE(offers.costs[2] >= 30);
    const EnchantmentList list = table_enchantments(0, 2, offers.costs[2], "minecraft:diamond_sword");
    CHECK(list.size() == 2);
    CHECK(list.level(Enchantment::Unbreaking) == 3);
    CHECK(list.level(Enchantment::Looting) == 2);
}

TEST_CASE("the wiki's own anvil examples", "[enchanting][anvil]") {
    using E = Enchantment;
    const auto cost = [](EnchantStack left, EnchantStack right) {
        AnvilRequest request;
        request.left  = left;
        request.right = right;
        return anvil_result(request);
    };
    const AnvilResult equal =
        cost(stack("minecraft:diamond_sword",
                   list_of({{E::Sharpness, 3}, {E::Knockback, 2}, {E::Looting, 3}})),
             stack("minecraft:diamond_sword", list_of({{E::Sharpness, 3}, {E::Looting, 3}})));
    CHECK(equal.valid);
    CHECK(equal.cost == 16);
    CHECK(equal.enchantments.level(E::Sharpness) == 4);
    CHECK(equal.repair_cost == 1);

    CHECK(cost(stack("minecraft:diamond_sword",
                     list_of({{E::Sharpness, 3}, {E::Knockback, 2}, {E::Looting, 1}})),
               stack("minecraft:diamond_sword", list_of({{E::Sharpness, 1}, {E::Looting, 3}})))
              .cost == 15);
    const AnvilResult conflict =
        cost(stack("minecraft:diamond_sword", list_of({{E::Sharpness, 2}, {E::Looting, 2}})),
             stack("minecraft:diamond_sword", list_of({{E::Smite, 5}, {E::Looting, 2}})));
    CHECK(conflict.cost == 13);
    CHECK(conflict.enchantments.level(E::Smite) == 0);
    CHECK(conflict.enchantments.level(E::Looting) == 3);

    CHECK(cost(stack("minecraft:diamond_sword", list_of({{E::Looting, 2}})),
               stack("minecraft:enchanted_book",
                     list_of({{E::Protection, 3}, {E::Sharpness, 1}, {E::Looting, 2}})))
              .cost == 7);
    // The order of two books matters: 2 one way, 12 the other.
    CHECK(cost(stack("minecraft:enchanted_book", list_of({{E::SoulSpeed, 3}})),
               stack("minecraft:enchanted_book", list_of({{E::Mending, 1}})))
              .cost == 2);
    CHECK(cost(stack("minecraft:enchanted_book", list_of({{E::Mending, 1}})),
               stack("minecraft:enchanted_book", list_of({{E::SoulSpeed, 3}})))
              .cost == 12);
}

TEST_CASE("anvil repair, rename and the ceiling", "[enchanting][anvil]") {
    AnvilRequest unit;
    unit.left            = stack("minecraft:diamond_pickaxe", {}, 1000);
    unit.right           = stack("minecraft:diamond");
    unit.right->count    = 2;
    const AnvilResult r2 = anvil_result(unit);
    CHECK(r2.valid);
    CHECK(r2.cost == 2);
    CHECK(r2.damage == 1000 - 2 * (1561 / 4));
    CHECK(r2.right_consumed == 2);

    AnvilRequest rename;
    rename.left       = stack("minecraft:stick");
    rename.rename     = "Bob";
    rename.hover_name = "Stick";
    const AnvilResult named = anvil_result(rename);
    CHECK(named.valid);
    CHECK(named.cost == 1);
    CHECK(named.name == NameChange::Set);
    CHECK(named.repair_cost == 0);  // a rename alone does not raise the penalty

    rename.left.repair_cost = 60;
    CHECK(anvil_result(rename).cost == 39);  // a rename alone is capped under the ceiling
    CHECK(anvil_result(rename).valid);

    AnvilRequest dear;
    dear.left              = stack("minecraft:diamond_sword", {}, 0, 31);
    dear.right             = stack("minecraft:enchanted_book",
                                   list_of({{Enchantment::Sharpness, 5}, {Enchantment::Looting, 3}}));
    const AnvilResult pricey = anvil_result(dear);
    CHECK(pricey.cost >= 40);
    CHECK_FALSE(pricey.valid);
    dear.creative = true;
    CHECK(anvil_result(dear).valid);
}

TEST_CASE("the grindstone keeps curses and refunds the rest", "[enchanting][grindstone]") {
    using E = Enchantment;
    const GrindstoneResult sword = grindstone_result(
        stack("minecraft:diamond_sword", list_of({{E::Sharpness, 5}}), 100), std::nullopt);
    CHECK(sword.valid);
    CHECK(sword.enchantments.empty());
    CHECK(sword.damage == 100);
    CHECK(sword.experience_base == 45);

    const GrindstoneResult cursed = grindstone_result(
        stack("minecraft:diamond_chestplate",
              list_of({{E::Protection, 4}, {E::BindingCurse, 1}}), 0, 7),
        std::nullopt);
    CHECK(cursed.enchantments.size() == 1);
    CHECK(cursed.repair_cost == 1);

    const GrindstoneResult book = grindstone_result(
        stack("minecraft:enchanted_book", list_of({{E::Unbreaking, 1}})), std::nullopt);
    CHECK(book.item == "minecraft:book");
    CHECK(book.experience_base == 5);

    CHECK_FALSE(grindstone_result(stack("minecraft:iron_sword"), std::nullopt).valid);
    const GrindstoneResult pair = grindstone_result(stack("minecraft:iron_pickaxe", {}, 200),
                                                    stack("minecraft:iron_pickaxe", {}, 150));
    CHECK(pair.damage == std::max(250 - (50 + 100 + 12), 0));

    math::LegacyRandomSource random{42};
    for (int i = 0; i < 200; ++i) {
        const i32 xp = grindstone_experience(45, random);
        CHECK((xp >= 23 && xp <= 45));
    }
}

TEST_CASE("EPF, Mending and the damage bonus", "[enchanting]") {
    using E = Enchantment;
    const std::array<EnchantmentList, 1> head{list_of({{E::Protection, 4}})};
    CHECK(total_epf(head, DamageKind::Generic) == 4);
    CHECK(after_protection(10.0F, 4) == 10.0F * (1.0F - 4.0F / 25.0F));
    const std::array<EnchantmentList, 1> boots{list_of({{E::FeatherFalling, 4}})};
    CHECK(total_epf(boots, DamageKind::Fall) == 12);
    CHECK(total_epf(boots, DamageKind::Generic) == 0);
    CHECK(after_protection(10.0F, 30) == 10.0F * (1.0F - 20.0F / 25.0F));

    CHECK(mend(10, 200).repaired == 20);
    CHECK(mend(10, 200).left == 0);
    CHECK(mend(5, 3).repaired == 3);
    CHECK(mend(5, 3).left == 4);

    CHECK(mob_group("minecraft:zombie") == MobGroup::Undead);
    CHECK(mob_group("minecraft:cave_spider") == MobGroup::Arthropod);
    CHECK(damage_bonus(list_of({{E::Smite, 5}}), MobGroup::Undead) == 12.5F);
    CHECK(damage_bonus(list_of({{E::Smite, 5}}), MobGroup::Default) == 0.0F);
    CHECK(damage_bonus(list_of({{E::Sharpness, 5}}), MobGroup::Default) == 3.0F);
}

TEST_CASE("the damage path applies the worn EPF after Resistance", "[enchanting]") {
    // What the server actually calls. A 10-point fall through Feather Falling
    // IV (EPF 12) must leave 5.2; the parity test above checks the formula,
    // this checks that apply_damage uses it.
    DamageMitigation mitigation;
    mitigation.protection[static_cast<usize>(DamageKind::Fall)] = 12;
    HealthState        state;
    const DamageConstants constants{};
    state.health         = 20.0F;
    const DamageResult hit = apply_damage(state, DamageKind::Fall, 10.0F, constants, mitigation);
    CHECK(hit.applied);
    CHECK(std::fabs(hit.dealt - 5.2F) < 1e-5F);
    CHECK(std::fabs(state.health - 14.8F) < 1e-5F);

    // A kind the EPF does not name is untouched.
    HealthState other;
    other.health = 20.0F;
    CHECK(apply_damage(other, DamageKind::Generic, 10.0F, constants, mitigation).dealt == 10.0F);
}

TEST_CASE("enchantments survive an NBT round trip", "[enchanting]") {
    nbt::Tag tag = nbt::Tag::make_compound();
    write_enchantments(tag, list_of({{Enchantment::Sharpness, 5}, {Enchantment::Unbreaking, 3}}),
                       false);
    const EnchantmentList back = read_enchantments(tag, false);
    REQUIRE(back.size() == 2);
    CHECK(back[0] == EnchantmentLevel{Enchantment::Sharpness, 5});
    CHECK(read_enchantments(tag, true).empty());
    write_enchantments(tag, {}, false);
    CHECK(tag.find("Enchantments") == nullptr);
}

// ── Parity with the real server ─────────────────────────────────────────────

TEST_CASE("every table offer the real server made", "[enchanting][parity]") {
    const auto path = measured_file();
    if (!std::filesystem::exists(path)) {
        WARN("no enchanting.json — run scripts/measure_enchanting.py table");
        return;
    }
    simdjson::dom::parser  parser;
    simdjson::dom::element document;
    REQUIRE(parser.load(path.string()).get(document) == simdjson::SUCCESS);
    simdjson::dom::object table;
    if (document["table"].get(table) != simdjson::SUCCESS ||
        table["offers"].error() != simdjson::SUCCESS) {
        WARN("enchanting.json has no table campaign");
        return;
    }
    usize offers = 0;
    usize same   = 0;
    for (simdjson::dom::element offer : table["offers"].get_array()) {
        const auto      seed    = static_cast<i32>(int64_t(offer["seed"]));
        const auto      shelves = static_cast<i32>(int64_t(offer["shelves"]));
        std::string     item{"minecraft:"};
        item += std::string_view(offer["item"]);
        std::array<i32, 10> props{};
        usize               k = 0;
        for (simdjson::dom::element v : offer["props"].get_array()) {
            props[k++] = static_cast<i32>(int64_t(v));
        }
        TableOffers mine{};
        if (table_accepts(item, 1, false)) {
            mine = table_offers(seed, shelves, item);
        }
        const std::array<i32, 10> ours{mine.costs[0],
                                       mine.costs[1],
                                       mine.costs[2],
                                       props[3],
                                       mine.clue_enchantment[0],
                                       mine.clue_enchantment[1],
                                       mine.clue_enchantment[2],
                                       mine.clue_level[0],
                                       mine.clue_level[1],
                                       mine.clue_level[2]};
        ++offers;
        // Property 3 is the seed with its low four bits cleared, and — like
        // every Container Property — travels as a short: bits 4..15 survive.
        const bool seed_ok = props[3] == static_cast<i16>(table_seed_property(seed));
        if (ours == props && seed_ok) {
            ++same;
        } else {
            UNSCOPED_INFO("seed " << seed << " shelves " << shelves << " " << item);
        }
    }
    INFO(same << " / " << offers << " offers identical");
    REQUIRE(offers > 0);
    CHECK(same == offers);
}

TEST_CASE("every enchantment a table button applied", "[enchanting][parity]") {
    const auto path = measured_file();
    if (!std::filesystem::exists(path)) {
        WARN("no enchanting.json");
        return;
    }
    simdjson::dom::parser  parser;
    simdjson::dom::element document;
    REQUIRE(parser.load(path.string()).get(document) == simdjson::SUCCESS);
    simdjson::dom::array enchants;
    if (document["table"]["enchants"].get(enchants) != simdjson::SUCCESS) {
        WARN("enchanting.json has no table campaign");
        return;
    }
    usize applied = 0;
    usize same    = 0;
    for (simdjson::dom::element record : enchants) {
        int64_t button = 0;
        if (record["button"].get(button) != simdjson::SUCCESS) {
            continue;
        }
        const auto  seed = static_cast<i32>(int64_t(record["seed"]));
        std::string item{"minecraft:"};
        item += std::string_view(record["item"]);
        const auto cost = static_cast<i32>(int64_t(record["props"].at(static_cast<usize>(button))));
        const EnchantmentList mine =
            table_enchantments(seed, static_cast<i32>(button), cost, item);

        EnchantmentList theirs;
        simdjson::dom::object tag;
        if (record["result"]["tag"].get(tag) == simdjson::SUCCESS) {
            simdjson::dom::array list;
            if (tag["Enchantments"].get(list) != simdjson::SUCCESS) {
                (void)tag["StoredEnchantments"].get(list);
            }
            for (simdjson::dom::element e : list) {
                const auto id = enchantment_from_name(std::string_view(e["id"]));
                REQUIRE(id.has_value());
                theirs.push({*id, static_cast<i32>(int64_t(e["lvl"]))});
            }
        }
        ++applied;
        // Levels and lapis: the button's index plus one, not the cost shown.
        const auto levels = int64_t(record["levels_before"]) - int64_t(record["levels_after"]);
        CHECK(levels == button + 1);
        CHECK(int64_t(record["lapis_left"]) == 64 - (button + 1));
        if (mine == theirs) {
            ++same;
        } else {
            UNSCOPED_INFO("seed " << seed << " slot " << button << " " << item);
        }
    }
    INFO(same << " / " << applied << " enchantments identical");
    REQUIRE(applied > 0);
    CHECK(same == applied);
}

TEST_CASE("Protection against the real server's /damage", "[enchanting][parity]") {
    const auto path = measured_file();
    if (!std::filesystem::exists(path)) {
        WARN("no enchanting.json");
        return;
    }
    simdjson::dom::parser  parser;
    simdjson::dom::element document;
    REQUIRE(parser.load(path.string()).get(document) == simdjson::SUCCESS);
    simdjson::dom::object cases;
    if (document["protection"].get(cases) != simdjson::SUCCESS ||
        cases["error"].error() == simdjson::SUCCESS) {
        WARN("enchanting.json has no protection campaign");
        return;
    }
    usize       total = 0;
    usize       same  = 0;
    std::string differing;
    for (auto [name, value] : cases) {
        const auto kind =
            damage_kind_from_name(std::string{"minecraft:"} + std::string{std::string_view(value["damage_type"])});
        REQUIRE(kind.has_value());
        EnchantmentList worn;
        for (simdjson::dom::element pair : value["enchantments"].get_array()) {
            const auto e = enchantment_from_name(std::string_view(pair.at(0)));
            REQUIRE(e.has_value());
            worn.push({*e, static_cast<i32>(int64_t(pair.at(1)))});
        }
        const std::array<EnchantmentList, 1> pieces{worn};
        const auto amount = static_cast<f32>(double(value["amount"]));
        const auto before = static_cast<f32>(double(value["health_before"]));
        const auto after  = static_cast<f32>(double(value["health_after"]));
        // A kind the body's own armour ignores still meets Resistance-free,
        // armour-free health here: the head has no armour points.
        const f32 dealt = after_protection(amount, total_epf(pieces, *kind));
        ++total;
        if (std::fabs((before - dealt) - after) < 1e-4F) {
            ++same;
        } else {
            differing += std::string{std::string_view(name)} + " ";
        }
    }
    INFO(same << " / " << total << " hits identical; differing: " << differing);
    CHECK(same == total);
}
