// Trading rules against what a real 1.20.1 server did (measure_villagers.py).
// The whole pool table is confronted with 4298 sampled offers by
// scripts/check_trades.py; what is pinned here is the rest — the numbers the
// samples cannot show one at a time, and the rows the wiki had wrong.
#include "ov/gameplay/trading.hpp"
#include "ov/gameplay/villager.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <set>

using namespace ov;
using namespace ov::gameplay;

namespace {

[[nodiscard]] MerchantOffer offer_of(std::string_view buy, i32 count, i32 xp, f32 multiplier,
                                     i32 demand, i32 max_uses = 12) {
    MerchantOffer o;
    o.cost_a.item          = buy;
    o.cost_a.count         = count;
    o.result.item          = "minecraft:emerald";
    o.result.count         = 1;
    o.xp                   = xp;
    o.price_multiplier     = multiplier;
    o.demand               = demand;
    o.max_uses             = max_uses;
    return o;
}

}  // namespace

TEST_CASE("level thresholds: 10, 70, 150, 250, bracketed one point each side",
          "[trading][parity]") {
    // A trade worth 2 from Xp t-3 stayed; from t-2 it rose. Master never rose.
    CHECK(next_level_xp(1) == 10);
    CHECK(next_level_xp(2) == 70);
    CHECK(next_level_xp(3) == 150);
    CHECK(next_level_xp(4) == 250);
    CHECK(next_level_xp(5) == 0);
    for (const auto& [level, threshold] : std::array<std::pair<i32, i32>, 4>{
             {{1, 10}, {2, 70}, {3, 150}, {4, 250}}}) {
        CAPTURE(level);
        for (const i32 start : {threshold - 3, threshold - 2}) {
            VillagerState v;
            v.profession    = Profession::Librarian;
            v.level         = level;
            v.xp            = start;
            v.offers_drawn  = true;
            MerchantOffer o = offer_of("minecraft:paper", 24, 2, 0.05F, 0, 16);
            math::LegacyRandomSource random{7};
            const TradeOutcome out = record_trade(v, o, random);
            CHECK(out.level_up_armed == (start + 2 >= threshold));
            // Measured orbs: 3..6 on a plain trade, 8..11 on one that rose.
            if (out.level_up_armed) {
                CHECK(out.orb >= 8);
                CHECK(out.orb <= 11);
            } else {
                CHECK(out.orb >= 3);
                CHECK(out.orb <= 6);
            }
        }
    }
}

TEST_CASE("the price a demand makes: n - 1 refused, n accepted, four offers",
          "[trading][parity]") {
    // (base, multiplier, demand, price the server asked).
    struct Case {
        i32 base;
        f32 multiplier;
        i32 demand;
        i32 price;
    };
    constexpr std::array<Case, 4> kMeasured{{
        {10, 0.2F, 5, 20}, {10, 0.05F, 3, 11}, {10, 0.2F, -4, 10}, {9, 0.05F, 11, 13}}};
    for (const Case& c : kMeasured) {
        CAPTURE(c.base, c.multiplier, c.demand);
        const MerchantOffer o = offer_of("minecraft:emerald", c.base, 1, c.multiplier, c.demand);
        CHECK(cost_a_count(o, 64) == c.price);
        CHECK_FALSE(satisfied_by(o, 64, "minecraft:emerald", c.price - 1, {}, 0));
        CHECK(satisfied_by(o, 64, "minecraft:emerald", c.price, {}, 0));
    }
}

TEST_CASE("a single-cost offer refuses anything in the second slot", "[trading]") {
    const MerchantOffer o = offer_of("minecraft:paper", 24, 2, 0.05F, 0);
    CHECK(satisfied_by(o, 64, "minecraft:paper", 24, {}, 0));
    CHECK_FALSE(satisfied_by(o, 64, "minecraft:paper", 24, "minecraft:stone", 1));
    CHECK_FALSE(satisfied_by(o, 64, "minecraft:stone", 24, {}, 0));
}

TEST_CASE("restocking: demand from what was bought, twice a day", "[trading][parity]") {
    VillagerState v;
    v.offers = {offer_of("minecraft:paper", 24, 2, 0.05F, 0), offer_of("minecraft:paper", 24, 2, 0.05F, 0),
                offer_of("minecraft:paper", 24, 2, 0.05F, 0)};
    v.offers[0].uses = 0;
    v.offers[1].uses = 5;
    v.offers[2].uses = 12;
    CHECK(needs_restock(v));
    CHECK(allowed_to_restock(v, 100));
    restock(v, 100);
    // Measured: -12, -2, 12.
    CHECK(v.offers[0].demand == -12);
    CHECK(v.offers[1].demand == -2);
    CHECK(v.offers[2].demand == 12);
    CHECK_FALSE(needs_restock(v));

    v.offers[0].uses = 12;
    v.offers[1].uses = 12;
    v.offers[2].uses = 0;
    CHECK_FALSE(allowed_to_restock(v, 100 + 2400));  // not *more* than 2400 yet
    CHECK(allowed_to_restock(v, 100 + 2401));
    restock(v, 2600);
    // Measured: 0, 10, 0.
    CHECK(v.offers[0].demand == 0);
    CHECK(v.offers[1].demand == 10);
    CHECK(v.offers[2].demand == 0);
    // Measured: no third restock the same day.
    v.offers[0].uses = 1;
    CHECK_FALSE(allowed_to_restock(v, 2600 + 10000));
    note_day(v, 6000);
    note_day(v, 24000 + 6000);
    CHECK(allowed_to_restock(v, 2600 + 10000));
}

TEST_CASE("a level up waits for the screen to close, then 40 ticks", "[trading]") {
    VillagerState v;
    v.active     = true;
    v.profession = Profession::Librarian;
    v.random.set_seed(11);
    ensure_offers(v);
    REQUIRE(v.offers.size() == 2);
    v.xp = 8;
    math::LegacyRandomSource random{3};
    REQUIRE(record_trade(v, v.offers[0], random).level_up_armed);
    v.trading_player = 1;
    for (i32 i = 0; i < 100; ++i) {
        CHECK_FALSE(tick_level_up(v));
    }
    v.trading_player = -1;
    for (i32 i = 0; i < kLevelUpDelay - 1; ++i) {
        CHECK_FALSE(tick_level_up(v));
    }
    CHECK(tick_level_up(v));
    CHECK(v.level == 2);
    // Measured: one offer before, three after — two from the new level.
    CHECK(v.offers.size() == 4);
}

TEST_CASE("every drawn offer comes from its level's pool, in listed order", "[trading]") {
    for (usize p = 1; p < kProfessionCount; ++p) {
        const auto profession = static_cast<Profession>(p);
        for (i32 level = 1; level <= kMaxLevel; ++level) {
            const auto pool = trade_listings(profession, level);
            if (profession == Profession::Nitwit) {
                CHECK(pool.empty());
                continue;
            }
            REQUIRE_FALSE(pool.empty());
            for (i64 seed = 0; seed < 64; ++seed) {
                VillagerState v;
                v.profession = profession;
                v.level      = level;
                v.random.set_seed(seed);
                ensure_offers(v);
                CHECK(v.offers.size() <= kOffersPerLevel);
                for (const MerchantOffer& o : v.offers) {
                    CHECK_FALSE(o.result.empty());
                    CHECK_FALSE(o.cost_a.empty());
                    CHECK(o.cost_a.count <= 64);
                }
            }
        }
    }
}

TEST_CASE("the HashSet order: by index modulo 16", "[trading][parity]") {
    CHECK(listed_before(0, 1));
    CHECK(listed_before(23, 10));   // bucket 7 before bucket 10
    CHECK_FALSE(listed_before(10, 23));
    CHECK_FALSE(listed_before(3, 19));  // one bucket: draw order decides
    CHECK_FALSE(listed_before(19, 3));
}

TEST_CASE("rows the samples corrected", "[trading][parity]") {
    // The armourer's novice pool, in the order its villagers list it.
    const auto armorer = trade_listings(Profession::Armorer, 1);
    REQUIRE(armorer.size() == 5);
    CHECK(armorer[1].item == "minecraft:iron_leggings");
    CHECK(armorer[2].item == "minecraft:iron_boots");
    CHECK(armorer[3].item == "minecraft:iron_helmet");
    CHECK(armorer[4].item == "minecraft:iron_chestplate");
    // The fletcher's enchanted bow and crossbow, the weaponsmith's sword: 0.05.
    CHECK(trade_listings(Profession::Fletcher, 4)[1].multiplier == 0.05F);
    CHECK(trade_listings(Profession::Fletcher, 5)[1].multiplier == 0.05F);
    CHECK(trade_listings(Profession::Weaponsmith, 1)[2].multiplier == 0.05F);
    // The journeyman leatherworker's chestplate: 1 experience.
    CHECK(trade_listings(Profession::Leatherworker, 3)[1].xp == 1);
    // One pool entry at a single-entry level, measured one offer each time.
    CHECK(trade_listings(Profession::Butcher, 4).size() == 1);
    CHECK(trade_listings(Profession::Librarian, 5).size() == 1);
    CHECK(trade_listings(Profession::Weaponsmith, 3).size() == 1);
}

TEST_CASE("a master fisherman buys the boat of his type", "[trading][parity]") {
    // Measured, twelve master fishermen of each of the seven types.
    constexpr std::array<std::string_view, kVillagerTypeCount> kBoats{
        "minecraft:jungle_boat", "minecraft:jungle_boat", "minecraft:oak_boat",
        "minecraft:acacia_boat", "minecraft:spruce_boat", "minecraft:dark_oak_boat",
        "minecraft:spruce_boat"};
    const Listing& boat = trade_listings(Profession::Fisherman, 5)[1];
    math::LegacyRandomSource random{1};
    for (usize t = 0; t < kVillagerTypeCount; ++t) {
        const auto offer = make_offer(boat, static_cast<VillagerType>(t), random);
        REQUIRE(offer);
        CHECK(offer->cost_a.item == kBoats[t]);
        CHECK(offer->result.item == "minecraft:emerald");
    }
}

TEST_CASE("enchanted books: 37 enchantments, prices in their ranges", "[trading][parity]") {
    math::LegacyRandomSource random{2024};
    std::set<u8>             seen;
    for (i32 i = 0; i < 20000; ++i) {
        const BookDraw draw = draw_enchanted_book(random);
        seen.insert(static_cast<u8>(draw.enchantment));
        CHECK(tradeable(draw.enchantment));
        const EnchantmentInfo& info = enchantment_info(draw.enchantment);
        CHECK(draw.level >= 1);
        CHECK(draw.level <= info.max_level);
        i32 lo = 2 + 3 * draw.level;
        i32 hi = 6 + 13 * draw.level;
        if (info.treasure) {
            lo *= 2;
            hi *= 2;
        }
        CHECK(draw.price >= std::min(lo, 64));
        CHECK(draw.price <= std::min(hi, 64));
    }
    // Measured: 248 books from the real server showed all 37, and never Soul
    // Speed or Swift Sneak.
    CHECK(seen.size() == 37);
    CHECK_FALSE(seen.contains(static_cast<u8>(Enchantment::SoulSpeed)));
    CHECK_FALSE(seen.contains(static_cast<u8>(Enchantment::SwiftSneak)));
}

TEST_CASE("leather colours: the dyes, and three mixes the real server sold",
          "[trading][parity]") {
    constexpr std::array<i32, 16> kDyes{
        0xF9FFFE, 0xF9801D, 0xC74EBD, 0x3AB3DA, 0xFED83D, 0x80C71F, 0xF38BAA, 0x474F52,
        0x9D9D97, 0x169C9C, 0x8932B8, 0x3C44AA, 0x835432, 0x5E7C16, 0xB02E26, 0x1D1D21};
    for (u8 d = 0; d < 16; ++d) {
        const std::array<u8, 1> one{d};
        CHECK(mix_dye_colours(one) == kDyes[d]);
    }
    // Colours sampled from leatherworkers, and the dyes that make them.
    CHECK(mix_dye_colours(std::array<u8, 2>{0, 2}) == 14919903);
    CHECK(mix_dye_colours(std::array<u8, 2>{1, 3}) == 15329723);
    CHECK(mix_dye_colours(std::array<u8, 2>{1, 4}) == 16493613);
}

TEST_CASE("tipped arrows carry a brewable potion, dyed armour a colour", "[trading]") {
    math::LegacyRandomSource random{5};
    const Listing& arrow = trade_listings(Profession::Fletcher, 5)[2];
    const Listing& horse = trade_listings(Profession::Leatherworker, 4)[1];
    const Listing& pants = trade_listings(Profession::Leatherworker, 1)[1];
    for (i32 i = 0; i < 200; ++i) {
        const auto a = make_offer(arrow, VillagerType::Plains, random);
        REQUIRE(a);
        CHECK_FALSE(a->result.potion.empty());
        CHECK(a->cost_b.item == "minecraft:arrow");
        CHECK(a->cost_b.count == 5);
        const auto p = make_offer(pants, VillagerType::Plains, random);
        REQUIRE(p);
        CHECK(p->result.dye_colour >= 0);
        const auto h = make_offer(horse, VillagerType::Plains, random);
        REQUIRE(h);
        CHECK(h->result.dye_colour == -1);  // measured: horse armour is sold plain
    }
    CHECK(tradeable_potions().size() == 37);
}

TEST_CASE("explorer maps are refused, and named", "[trading]") {
    math::LegacyRandomSource random{9};
    const Listing& map = trade_listings(Profession::Cartographer, 2)[1];
    CHECK(map.kind == ListingKind::TreasureMap);
    CHECK_FALSE(make_offer(map, VillagerType::Plains, random).has_value());
    CHECK(listing_kind_name(map.kind) == "treasure_map");
}
