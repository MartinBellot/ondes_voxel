#include "ov/gameplay/trading.hpp"

#include <algorithm>
#include <cmath>

namespace ov::gameplay {
namespace {

// ── The pools ───────────────────────────────────────────────────────────────
//
// One row per pool entry, in pool order. The order is observable: it decides
// which of a level's two offers is listed first (see `listed_before`).
//
// Every row was checked against the samples of scripts/measure_villagers.py
// `offers` — item, count, price, uses, experience and multiplier — and each
// pool against the set of rows the samples contain, by
// scripts/check_trades.py. Where the wiki and the samples disagreed, the
// samples won; villageois.md lists those places.

using K = ListingKind;

constexpr Listing sell(std::string_view item, i32 count, i32 max_uses, i32 xp) {
    Listing l;
    l.kind       = K::SellForEmerald;
    l.item       = item;
    l.count      = count;
    l.emeralds   = 1;
    l.max_uses   = max_uses;
    l.xp         = xp;
    l.multiplier = 0.05F;
    return l;
}

constexpr Listing buy(std::string_view item, i32 emeralds, i32 count, i32 max_uses, i32 xp,
                      f32 multiplier = 0.05F) {
    Listing l;
    l.kind       = K::BuyForEmeralds;
    l.item       = item;
    l.count      = count;
    l.emeralds   = emeralds;
    l.max_uses   = max_uses;
    l.xp         = xp;
    l.multiplier = multiplier;
    return l;
}

constexpr Listing exchange(std::string_view input, i32 input_count, i32 emeralds,
                           std::string_view item, i32 count, i32 max_uses, i32 xp) {
    Listing l;
    l.kind        = K::Exchange;
    l.input       = input;
    l.input_count = input_count;
    l.emeralds    = emeralds;
    l.item        = item;
    l.count       = count;
    l.max_uses    = max_uses;
    l.xp          = xp;
    l.multiplier  = 0.05F;
    return l;
}

constexpr Listing enchanted(std::string_view item, i32 base, i32 max_uses, i32 xp,
                            f32 multiplier = 0.2F) {
    Listing l;
    l.kind       = K::EnchantedItem;
    l.item       = item;
    l.emeralds   = base;
    l.max_uses   = max_uses;
    l.xp         = xp;
    l.multiplier = multiplier;
    return l;
}

constexpr Listing book(i32 xp) {
    Listing l;
    l.kind       = K::EnchantedBook;
    l.item       = "minecraft:enchanted_book";
    l.max_uses   = 12;
    l.xp         = xp;
    l.multiplier = 0.2F;
    return l;
}

constexpr Listing stew(i32 effect, i32 duration) {
    Listing l;
    l.kind       = K::SuspiciousStew;
    l.item       = "minecraft:suspicious_stew";
    l.emeralds   = 1;
    l.max_uses   = 12;
    l.xp         = 15;
    l.multiplier = 0.05F;
    l.effect     = effect;
    l.duration   = duration;
    return l;
}

constexpr Listing dyed(std::string_view item, i32 emeralds, i32 xp) {
    Listing l;
    l.kind       = K::DyedArmor;
    l.item       = item;
    l.emeralds   = emeralds;
    l.max_uses   = 12;
    l.xp         = xp;
    l.multiplier = 0.2F;
    return l;
}

constexpr Listing map_for(i32 emeralds, i32 xp) {
    Listing l;
    l.kind       = K::TreasureMap;
    l.item       = "minecraft:filled_map";
    l.emeralds   = emeralds;
    l.max_uses   = 12;
    l.xp         = xp;
    l.multiplier = 0.2F;
    return l;
}

constexpr Listing tipped() {
    Listing l;
    l.kind        = K::TippedArrow;
    l.item        = "minecraft:tipped_arrow";
    l.input       = "minecraft:arrow";
    l.input_count = 5;
    l.emeralds    = 2;
    l.count       = 5;
    l.max_uses    = 12;
    l.xp          = 30;
    return l;
}

constexpr Listing boat_by_type() {
    Listing l;
    l.kind     = K::ByType;
    l.count    = 1;
    l.max_uses = 12;
    l.xp       = 30;
    // desert, jungle, plains, savanna, snow, swamp, taiga — measured, twelve
    // master fishermen of each type.
    l.by_type = {"minecraft:jungle_boat", "minecraft:jungle_boat", "minecraft:oak_boat",
                 "minecraft:acacia_boat", "minecraft:spruce_boat", "minecraft:dark_oak_boat",
                 "minecraft:spruce_boat"};
    return l;
}

// ── Armorer ──
constexpr std::array kArmorer1{
    sell("minecraft:coal", 15, 16, 2),
    buy("minecraft:iron_leggings", 7, 1, 12, 1, 0.2F),
    buy("minecraft:iron_boots", 4, 1, 12, 1, 0.2F),
    buy("minecraft:iron_helmet", 5, 1, 12, 1, 0.2F),
    buy("minecraft:iron_chestplate", 9, 1, 12, 1, 0.2F),
};
constexpr std::array kArmorer2{
    sell("minecraft:iron_ingot", 4, 12, 10),
    buy("minecraft:bell", 36, 1, 12, 5, 0.2F),
    buy("minecraft:chainmail_boots", 1, 1, 12, 5, 0.2F),
    buy("minecraft:chainmail_leggings", 3, 1, 12, 5, 0.2F),
};
constexpr std::array kArmorer3{
    sell("minecraft:lava_bucket", 1, 12, 20),
    sell("minecraft:diamond", 1, 12, 20),
    buy("minecraft:chainmail_helmet", 1, 1, 12, 10, 0.2F),
    buy("minecraft:chainmail_chestplate", 4, 1, 12, 10, 0.2F),
    buy("minecraft:shield", 5, 1, 12, 10, 0.2F),
};
constexpr std::array kArmorer4{
    enchanted("minecraft:diamond_leggings", 14, 3, 15),
    enchanted("minecraft:diamond_boots", 8, 3, 15),
};
constexpr std::array kArmorer5{
    enchanted("minecraft:diamond_helmet", 8, 3, 30),
    enchanted("minecraft:diamond_chestplate", 16, 3, 30),
};

// ── Butcher ──
constexpr std::array kButcher1{
    sell("minecraft:chicken", 14, 16, 2),
    sell("minecraft:porkchop", 7, 16, 2),
    sell("minecraft:rabbit", 4, 16, 2),
    buy("minecraft:rabbit_stew", 1, 1, 12, 1),
};
constexpr std::array kButcher2{
    sell("minecraft:coal", 15, 16, 2),
    buy("minecraft:cooked_porkchop", 1, 5, 16, 5),
    buy("minecraft:cooked_chicken", 1, 8, 16, 5),
};
constexpr std::array kButcher3{
    sell("minecraft:mutton", 7, 16, 20),
    sell("minecraft:beef", 10, 16, 20),
};
constexpr std::array kButcher4{sell("minecraft:dried_kelp_block", 10, 12, 30)};
constexpr std::array kButcher5{sell("minecraft:sweet_berries", 10, 12, 30)};

// ── Cartographer ──
constexpr std::array kCartographer1{
    sell("minecraft:paper", 24, 16, 2),
    buy("minecraft:map", 7, 1, 12, 1),
};
constexpr std::array kCartographer2{
    sell("minecraft:glass_pane", 11, 16, 10),
    map_for(13, 5),
};
constexpr std::array kCartographer3{
    sell("minecraft:compass", 1, 12, 20),
    map_for(14, 10),
};
constexpr std::array kCartographer4{
    buy("minecraft:item_frame", 7, 1, 12, 15),
    buy("minecraft:white_banner", 3, 1, 12, 15),
    buy("minecraft:blue_banner", 3, 1, 12, 15),
    buy("minecraft:light_blue_banner", 3, 1, 12, 15),
    buy("minecraft:red_banner", 3, 1, 12, 15),
    buy("minecraft:pink_banner", 3, 1, 12, 15),
    buy("minecraft:green_banner", 3, 1, 12, 15),
    buy("minecraft:lime_banner", 3, 1, 12, 15),
    buy("minecraft:gray_banner", 3, 1, 12, 15),
    buy("minecraft:black_banner", 3, 1, 12, 15),
    buy("minecraft:purple_banner", 3, 1, 12, 15),
    buy("minecraft:magenta_banner", 3, 1, 12, 15),
    buy("minecraft:cyan_banner", 3, 1, 12, 15),
    buy("minecraft:brown_banner", 3, 1, 12, 15),
    buy("minecraft:yellow_banner", 3, 1, 12, 15),
    buy("minecraft:orange_banner", 3, 1, 12, 15),
    buy("minecraft:light_gray_banner", 3, 1, 12, 15),
};
constexpr std::array kCartographer5{buy("minecraft:globe_banner_pattern", 8, 1, 12, 30)};

// ── Cleric ──
constexpr std::array kCleric1{
    sell("minecraft:rotten_flesh", 32, 16, 2),
    buy("minecraft:redstone", 1, 2, 12, 1),
};
constexpr std::array kCleric2{
    sell("minecraft:gold_ingot", 3, 12, 10),
    buy("minecraft:lapis_lazuli", 1, 1, 12, 5),
};
constexpr std::array kCleric3{
    sell("minecraft:rabbit_foot", 2, 12, 20),
    buy("minecraft:glowstone", 4, 1, 12, 10),
};
constexpr std::array kCleric4{
    sell("minecraft:scute", 4, 12, 30),
    sell("minecraft:glass_bottle", 9, 12, 30),
    buy("minecraft:ender_pearl", 5, 1, 12, 15),
};
constexpr std::array kCleric5{
    sell("minecraft:nether_wart", 22, 12, 30),
    buy("minecraft:experience_bottle", 3, 1, 12, 30),
};

// ── Farmer ──
constexpr std::array kFarmer1{
    sell("minecraft:wheat", 20, 16, 2),
    sell("minecraft:potato", 26, 16, 2),
    sell("minecraft:carrot", 22, 16, 2),
    sell("minecraft:beetroot", 15, 16, 2),
    buy("minecraft:bread", 1, 6, 16, 1),
};
constexpr std::array kFarmer2{
    sell("minecraft:pumpkin", 6, 12, 10),
    buy("minecraft:pumpkin_pie", 1, 4, 12, 5),
    buy("minecraft:apple", 1, 4, 16, 5),
};
constexpr std::array kFarmer3{
    buy("minecraft:cookie", 3, 18, 12, 10),
    sell("minecraft:melon", 4, 12, 20),
};
constexpr std::array kFarmer4{
    buy("minecraft:cake", 1, 1, 12, 15),
    stew(16, 100),  // night vision
    stew(8, 160),   // jump boost
    stew(18, 140),  // weakness
    stew(15, 120),  // blindness
    stew(19, 280),  // poison
    stew(23, 7),    // saturation
};
constexpr std::array kFarmer5{
    buy("minecraft:golden_carrot", 3, 3, 12, 30),
    buy("minecraft:glistering_melon_slice", 4, 3, 12, 30),
};

// ── Fisherman ──
constexpr std::array kFisherman1{
    sell("minecraft:string", 20, 16, 2),
    sell("minecraft:coal", 10, 16, 2),
    exchange("minecraft:cod", 6, 1, "minecraft:cooked_cod", 6, 16, 1),
    buy("minecraft:cod_bucket", 3, 1, 16, 1),
};
constexpr std::array kFisherman2{
    sell("minecraft:cod", 15, 16, 10),
    exchange("minecraft:salmon", 6, 1, "minecraft:cooked_salmon", 6, 16, 5),
    buy("minecraft:campfire", 2, 1, 12, 5),
};
constexpr std::array kFisherman3{
    sell("minecraft:salmon", 13, 16, 20),
    enchanted("minecraft:fishing_rod", 3, 3, 10),
};
constexpr std::array kFisherman4{sell("minecraft:tropical_fish", 6, 12, 30)};
constexpr std::array kFisherman5{
    sell("minecraft:pufferfish", 4, 12, 30),
    boat_by_type(),
};

// ── Fletcher ──
constexpr std::array kFletcher1{
    sell("minecraft:stick", 32, 16, 2),
    buy("minecraft:arrow", 1, 16, 12, 1),
    exchange("minecraft:gravel", 10, 1, "minecraft:flint", 10, 12, 1),
};
constexpr std::array kFletcher2{
    sell("minecraft:flint", 26, 12, 10),
    buy("minecraft:bow", 2, 1, 12, 5),
};
constexpr std::array kFletcher3{
    sell("minecraft:string", 14, 16, 20),
    buy("minecraft:crossbow", 3, 1, 12, 10),
};
// Measured: the fletcher's enchanted bow and crossbow have multiplier 0.05,
// not the 0.2 every other enchanted item has.
constexpr std::array kFletcher4{
    sell("minecraft:feather", 24, 16, 30),
    enchanted("minecraft:bow", 2, 3, 15, 0.05F),
};
constexpr std::array kFletcher5{
    sell("minecraft:tripwire_hook", 8, 12, 30),
    enchanted("minecraft:crossbow", 3, 3, 15, 0.05F),
    tipped(),
};

// ── Leatherworker ──
constexpr std::array kLeatherworker1{
    sell("minecraft:leather", 6, 16, 2),
    dyed("minecraft:leather_leggings", 3, 1),
    dyed("minecraft:leather_chestplate", 7, 1),
};
constexpr std::array kLeatherworker2{
    sell("minecraft:flint", 26, 12, 10),
    dyed("minecraft:leather_helmet", 5, 5),
    dyed("minecraft:leather_boots", 4, 5),
};
// Measured: the journeyman's chestplate gives 1 experience, as the novice's.
constexpr std::array kLeatherworker3{
    sell("minecraft:rabbit_hide", 9, 12, 20),
    dyed("minecraft:leather_chestplate", 7, 1),
};
constexpr std::array kLeatherworker4{
    sell("minecraft:scute", 4, 12, 30),
    dyed("minecraft:leather_horse_armor", 6, 15),
};
constexpr std::array kLeatherworker5{
    buy("minecraft:saddle", 6, 1, 12, 30, 0.2F),
    dyed("minecraft:leather_helmet", 5, 30),
};

// ── Librarian ──
constexpr std::array kLibrarian1{
    sell("minecraft:paper", 24, 16, 2),
    book(1),
    buy("minecraft:bookshelf", 9, 1, 12, 1),
};
constexpr std::array kLibrarian2{
    sell("minecraft:book", 4, 12, 10),
    book(5),
    buy("minecraft:lantern", 1, 1, 12, 5),
};
constexpr std::array kLibrarian3{
    sell("minecraft:ink_sac", 5, 12, 20),
    book(10),
    buy("minecraft:glass", 1, 4, 12, 10),
};
constexpr std::array kLibrarian4{
    sell("minecraft:writable_book", 2, 12, 30),
    book(15),
    buy("minecraft:clock", 5, 1, 12, 15),
    buy("minecraft:compass", 4, 1, 12, 15),
};
constexpr std::array kLibrarian5{buy("minecraft:name_tag", 20, 1, 12, 30)};

// ── Mason ──
constexpr std::array kMason1{
    sell("minecraft:clay_ball", 10, 16, 2),
    buy("minecraft:brick", 1, 10, 16, 1),
};
constexpr std::array kMason2{
    sell("minecraft:stone", 20, 16, 10),
    buy("minecraft:chiseled_stone_bricks", 1, 4, 16, 5),
};
constexpr std::array kMason3{
    sell("minecraft:granite", 16, 16, 20),
    sell("minecraft:andesite", 16, 16, 20),
    sell("minecraft:diorite", 16, 16, 20),
    buy("minecraft:dripstone_block", 1, 4, 16, 10),
    buy("minecraft:polished_andesite", 1, 4, 16, 10),
    buy("minecraft:polished_diorite", 1, 4, 16, 10),
    buy("minecraft:polished_granite", 1, 4, 16, 10),
};
constexpr std::array kMason4{
    sell("minecraft:quartz", 12, 12, 30),
    buy("minecraft:orange_terracotta", 1, 1, 12, 15),
    buy("minecraft:white_terracotta", 1, 1, 12, 15),
    buy("minecraft:blue_terracotta", 1, 1, 12, 15),
    buy("minecraft:light_blue_terracotta", 1, 1, 12, 15),
    buy("minecraft:gray_terracotta", 1, 1, 12, 15),
    buy("minecraft:light_gray_terracotta", 1, 1, 12, 15),
    buy("minecraft:black_terracotta", 1, 1, 12, 15),
    buy("minecraft:red_terracotta", 1, 1, 12, 15),
    buy("minecraft:pink_terracotta", 1, 1, 12, 15),
    buy("minecraft:magenta_terracotta", 1, 1, 12, 15),
    buy("minecraft:lime_terracotta", 1, 1, 12, 15),
    buy("minecraft:green_terracotta", 1, 1, 12, 15),
    buy("minecraft:cyan_terracotta", 1, 1, 12, 15),
    buy("minecraft:purple_terracotta", 1, 1, 12, 15),
    buy("minecraft:yellow_terracotta", 1, 1, 12, 15),
    buy("minecraft:brown_terracotta", 1, 1, 12, 15),
    buy("minecraft:orange_glazed_terracotta", 1, 1, 12, 15),
    buy("minecraft:white_glazed_terracotta", 1, 1, 12, 15),
    buy("minecraft:blue_glazed_terracotta", 1, 1, 12, 15),
    buy("minecraft:light_blue_glazed_terracotta", 1, 1, 12, 15),
    buy("minecraft:gray_glazed_terracotta", 1, 1, 12, 15),
    buy("minecraft:light_gray_glazed_terracotta", 1, 1, 12, 15),
    buy("minecraft:black_glazed_terracotta", 1, 1, 12, 15),
    buy("minecraft:red_glazed_terracotta", 1, 1, 12, 15),
    buy("minecraft:pink_glazed_terracotta", 1, 1, 12, 15),
    buy("minecraft:magenta_glazed_terracotta", 1, 1, 12, 15),
    buy("minecraft:lime_glazed_terracotta", 1, 1, 12, 15),
    buy("minecraft:green_glazed_terracotta", 1, 1, 12, 15),
    buy("minecraft:cyan_glazed_terracotta", 1, 1, 12, 15),
    buy("minecraft:purple_glazed_terracotta", 1, 1, 12, 15),
    buy("minecraft:yellow_glazed_terracotta", 1, 1, 12, 15),
    buy("minecraft:brown_glazed_terracotta", 1, 1, 12, 15),
};
constexpr std::array kMason5{
    buy("minecraft:quartz_pillar", 1, 1, 12, 30),
    buy("minecraft:quartz_block", 1, 1, 12, 30),
};

// ── Shepherd ──
constexpr std::array kShepherd1{
    sell("minecraft:white_wool", 18, 16, 2),
    sell("minecraft:brown_wool", 18, 16, 2),
    sell("minecraft:black_wool", 18, 16, 2),
    sell("minecraft:gray_wool", 18, 16, 2),
    buy("minecraft:shears", 2, 1, 12, 1),
};
constexpr std::array kShepherd2{
    sell("minecraft:white_dye", 12, 16, 10),
    sell("minecraft:gray_dye", 12, 16, 10),
    sell("minecraft:black_dye", 12, 16, 10),
    sell("minecraft:light_blue_dye", 12, 16, 10),
    sell("minecraft:lime_dye", 12, 16, 10),
    buy("minecraft:white_wool", 1, 1, 16, 5),
    buy("minecraft:orange_wool", 1, 1, 16, 5),
    buy("minecraft:magenta_wool", 1, 1, 16, 5),
    buy("minecraft:light_blue_wool", 1, 1, 16, 5),
    buy("minecraft:yellow_wool", 1, 1, 16, 5),
    buy("minecraft:lime_wool", 1, 1, 16, 5),
    buy("minecraft:pink_wool", 1, 1, 16, 5),
    buy("minecraft:gray_wool", 1, 1, 16, 5),
    buy("minecraft:light_gray_wool", 1, 1, 16, 5),
    buy("minecraft:cyan_wool", 1, 1, 16, 5),
    buy("minecraft:purple_wool", 1, 1, 16, 5),
    buy("minecraft:blue_wool", 1, 1, 16, 5),
    buy("minecraft:brown_wool", 1, 1, 16, 5),
    buy("minecraft:green_wool", 1, 1, 16, 5),
    buy("minecraft:red_wool", 1, 1, 16, 5),
    buy("minecraft:black_wool", 1, 1, 16, 5),
    buy("minecraft:white_carpet", 1, 4, 16, 5),
    buy("minecraft:orange_carpet", 1, 4, 16, 5),
    buy("minecraft:magenta_carpet", 1, 4, 16, 5),
    buy("minecraft:light_blue_carpet", 1, 4, 16, 5),
    buy("minecraft:yellow_carpet", 1, 4, 16, 5),
    buy("minecraft:lime_carpet", 1, 4, 16, 5),
    buy("minecraft:pink_carpet", 1, 4, 16, 5),
    buy("minecraft:gray_carpet", 1, 4, 16, 5),
    buy("minecraft:light_gray_carpet", 1, 4, 16, 5),
    buy("minecraft:cyan_carpet", 1, 4, 16, 5),
    buy("minecraft:purple_carpet", 1, 4, 16, 5),
    buy("minecraft:blue_carpet", 1, 4, 16, 5),
    buy("minecraft:brown_carpet", 1, 4, 16, 5),
    buy("minecraft:green_carpet", 1, 4, 16, 5),
    buy("minecraft:red_carpet", 1, 4, 16, 5),
    buy("minecraft:black_carpet", 1, 4, 16, 5),
};
constexpr std::array kShepherd3{
    sell("minecraft:yellow_dye", 12, 16, 20),
    sell("minecraft:light_gray_dye", 12, 16, 20),
    sell("minecraft:orange_dye", 12, 16, 20),
    sell("minecraft:red_dye", 12, 16, 20),
    sell("minecraft:pink_dye", 12, 16, 20),
    buy("minecraft:white_bed", 3, 1, 12, 10),
    buy("minecraft:yellow_bed", 3, 1, 12, 10),
    buy("minecraft:red_bed", 3, 1, 12, 10),
    buy("minecraft:black_bed", 3, 1, 12, 10),
    buy("minecraft:blue_bed", 3, 1, 12, 10),
    buy("minecraft:brown_bed", 3, 1, 12, 10),
    buy("minecraft:cyan_bed", 3, 1, 12, 10),
    buy("minecraft:gray_bed", 3, 1, 12, 10),
    buy("minecraft:green_bed", 3, 1, 12, 10),
    buy("minecraft:light_blue_bed", 3, 1, 12, 10),
    buy("minecraft:light_gray_bed", 3, 1, 12, 10),
    buy("minecraft:lime_bed", 3, 1, 12, 10),
    buy("minecraft:magenta_bed", 3, 1, 12, 10),
    buy("minecraft:orange_bed", 3, 1, 12, 10),
    buy("minecraft:pink_bed", 3, 1, 12, 10),
    buy("minecraft:purple_bed", 3, 1, 12, 10),
};
constexpr std::array kShepherd4{
    sell("minecraft:brown_dye", 12, 16, 30),
    sell("minecraft:purple_dye", 12, 16, 30),
    sell("minecraft:blue_dye", 12, 16, 30),
    sell("minecraft:green_dye", 12, 16, 30),
    sell("minecraft:magenta_dye", 12, 16, 30),
    sell("minecraft:cyan_dye", 12, 16, 30),
    buy("minecraft:white_banner", 3, 1, 12, 15),
    buy("minecraft:blue_banner", 3, 1, 12, 15),
    buy("minecraft:light_blue_banner", 3, 1, 12, 15),
    buy("minecraft:red_banner", 3, 1, 12, 15),
    buy("minecraft:pink_banner", 3, 1, 12, 15),
    buy("minecraft:green_banner", 3, 1, 12, 15),
    buy("minecraft:lime_banner", 3, 1, 12, 15),
    buy("minecraft:gray_banner", 3, 1, 12, 15),
    buy("minecraft:black_banner", 3, 1, 12, 15),
    buy("minecraft:purple_banner", 3, 1, 12, 15),
    buy("minecraft:magenta_banner", 3, 1, 12, 15),
    buy("minecraft:cyan_banner", 3, 1, 12, 15),
    buy("minecraft:brown_banner", 3, 1, 12, 15),
    buy("minecraft:yellow_banner", 3, 1, 12, 15),
    buy("minecraft:orange_banner", 3, 1, 12, 15),
    buy("minecraft:light_gray_banner", 3, 1, 12, 15),
};
constexpr std::array kShepherd5{buy("minecraft:painting", 2, 3, 12, 30)};

// ── Toolsmith ──
constexpr std::array kToolsmith1{
    sell("minecraft:coal", 15, 16, 2),
    buy("minecraft:stone_axe", 1, 1, 12, 1, 0.2F),
    buy("minecraft:stone_shovel", 1, 1, 12, 1, 0.2F),
    buy("minecraft:stone_pickaxe", 1, 1, 12, 1, 0.2F),
    buy("minecraft:stone_hoe", 1, 1, 12, 1, 0.2F),
};
constexpr std::array kToolsmith2{
    sell("minecraft:iron_ingot", 4, 12, 10),
    buy("minecraft:bell", 36, 1, 12, 5, 0.2F),
};
constexpr std::array kToolsmith3{
    sell("minecraft:flint", 30, 12, 20),
    enchanted("minecraft:iron_axe", 1, 3, 10),
    enchanted("minecraft:iron_shovel", 2, 3, 10),
    enchanted("minecraft:iron_pickaxe", 3, 3, 10),
    buy("minecraft:diamond_hoe", 4, 1, 3, 10, 0.2F),
};
constexpr std::array kToolsmith4{
    sell("minecraft:diamond", 1, 12, 30),
    enchanted("minecraft:diamond_axe", 12, 3, 15),
    enchanted("minecraft:diamond_shovel", 5, 3, 15),
};
constexpr std::array kToolsmith5{enchanted("minecraft:diamond_pickaxe", 13, 3, 30)};

// ── Weaponsmith ──
// Measured: the novice's enchanted iron sword has multiplier 0.05.
constexpr std::array kWeaponsmith1{
    sell("minecraft:coal", 15, 16, 2),
    buy("minecraft:iron_axe", 3, 1, 12, 1, 0.2F),
    enchanted("minecraft:iron_sword", 2, 3, 1, 0.05F),
};
constexpr std::array kWeaponsmith2{
    sell("minecraft:iron_ingot", 4, 12, 10),
    buy("minecraft:bell", 36, 1, 12, 5, 0.2F),
};
constexpr std::array kWeaponsmith3{sell("minecraft:flint", 24, 12, 20)};
constexpr std::array kWeaponsmith4{
    sell("minecraft:diamond", 1, 12, 30),
    enchanted("minecraft:diamond_axe", 12, 3, 15),
};
constexpr std::array kWeaponsmith5{enchanted("minecraft:diamond_sword", 8, 3, 30)};

struct Pools {
    std::array<std::span<const Listing>, 5> levels;
};

// Indexed by Profession; none and nitwit have nothing.
const std::array<Pools, kProfessionCount> kPools{{
    {},  // none
    {{kArmorer1, kArmorer2, kArmorer3, kArmorer4, kArmorer5}},
    {{kButcher1, kButcher2, kButcher3, kButcher4, kButcher5}},
    {{kCartographer1, kCartographer2, kCartographer3, kCartographer4, kCartographer5}},
    {{kCleric1, kCleric2, kCleric3, kCleric4, kCleric5}},
    {{kFarmer1, kFarmer2, kFarmer3, kFarmer4, kFarmer5}},
    {{kFisherman1, kFisherman2, kFisherman3, kFisherman4, kFisherman5}},
    {{kFletcher1, kFletcher2, kFletcher3, kFletcher4, kFletcher5}},
    {{kLeatherworker1, kLeatherworker2, kLeatherworker3, kLeatherworker4, kLeatherworker5}},
    {{kLibrarian1, kLibrarian2, kLibrarian3, kLibrarian4, kLibrarian5}},
    {{kMason1, kMason2, kMason3, kMason4, kMason5}},
    {},  // nitwit
    {{kShepherd1, kShepherd2, kShepherd3, kShepherd4, kShepherd5}},
    {{kToolsmith1, kToolsmith2, kToolsmith3, kToolsmith4, kToolsmith5}},
    {{kWeaponsmith1, kWeaponsmith2, kWeaponsmith3, kWeaponsmith4, kWeaponsmith5}},
}};

/// The sixteen dye colours, by DyeColor id (white .. black). The wiki's
/// "Dye" table; each single-dye leather colour sampled from a villager is one
/// of these exactly.
constexpr std::array<i32, 16> kDyeColours{
    0xF9FFFE, 0xF9801D, 0xC74EBD, 0x3AB3DA, 0xFED83D, 0x80C71F, 0xF38BAA, 0x474F52,
    0x9D9D97, 0x169C9C, 0x8932B8, 0x3C44AA, 0x835432, 0x5E7C16, 0xB02E26, 0x1D1D21,
};

/// Brewable potions with an effect, in `minecraft:potion` registry order: the
/// registry minus empty, water, mundane, thick, awkward (no effect) and luck
/// (not brewable). Thirteen of them came up on twenty-six sampled arrows, all
/// on this list.
constexpr std::array<std::string_view, 37> kTippedPotions{
    "minecraft:night_vision",         "minecraft:long_night_vision",
    "minecraft:invisibility",         "minecraft:long_invisibility",
    "minecraft:leaping",              "minecraft:long_leaping",
    "minecraft:strong_leaping",       "minecraft:fire_resistance",
    "minecraft:long_fire_resistance", "minecraft:swiftness",
    "minecraft:long_swiftness",       "minecraft:strong_swiftness",
    "minecraft:slowness",             "minecraft:long_slowness",
    "minecraft:strong_slowness",      "minecraft:turtle_master",
    "minecraft:long_turtle_master",   "minecraft:strong_turtle_master",
    "minecraft:water_breathing",      "minecraft:long_water_breathing",
    "minecraft:healing",              "minecraft:strong_healing",
    "minecraft:harming",              "minecraft:strong_harming",
    "minecraft:poison",               "minecraft:long_poison",
    "minecraft:strong_poison",        "minecraft:regeneration",
    "minecraft:long_regeneration",    "minecraft:strong_regeneration",
    "minecraft:strength",             "minecraft:long_strength",
    "minecraft:strong_strength",      "minecraft:weakness",
    "minecraft:long_weakness",        "minecraft:slow_falling",
    "minecraft:long_slow_falling",
};

constexpr std::string_view kEmerald = "minecraft:emerald";

[[nodiscard]] TradeItem stack(std::string_view item, i32 count) {
    TradeItem out;
    out.item  = item;
    out.count = count;
    return out;
}

[[nodiscard]] MerchantOffer base_offer(const Listing& l) {
    MerchantOffer offer;
    offer.max_uses         = l.max_uses;
    offer.xp               = l.xp;
    offer.price_multiplier = l.multiplier;
    return offer;
}

/// `Mth.nextInt(random, lo, hi)`: no draw at all when the range is one value.
[[nodiscard]] i32 next_int_between(math::LegacyRandomSource& random, i32 lo, i32 hi) {
    return lo >= hi ? lo : lo + random.next_int(hi - lo + 1);
}

/// Leather armour takes a dye; leather horse armour does not (measured).
[[nodiscard]] bool dyeable_armour(std::string_view item) noexcept {
    return item == "minecraft:leather_helmet" || item == "minecraft:leather_chestplate" ||
           item == "minecraft:leather_leggings" || item == "minecraft:leather_boots";
}

}  // namespace

std::span<const Listing> trade_listings(Profession profession, i32 level) noexcept {
    const auto index = static_cast<usize>(profession);
    if (index >= kPools.size() || level < 1 || level > kMaxLevel) {
        return {};
    }
    return kPools[index].levels[static_cast<usize>(level - 1)];
}

bool tradeable(Enchantment enchantment) noexcept {
    return enchantment != Enchantment::SoulSpeed && enchantment != Enchantment::SwiftSneak;
}

std::span<const std::string_view> tradeable_potions() noexcept { return kTippedPotions; }

std::string_view listing_kind_name(ListingKind kind) noexcept {
    switch (kind) {
        case K::SellForEmerald: return "sell_for_emerald";
        case K::BuyForEmeralds: return "buy_for_emeralds";
        case K::Exchange: return "exchange";
        case K::EnchantedItem: return "enchanted_item";
        case K::EnchantedBook: return "enchanted_book";
        case K::SuspiciousStew: return "suspicious_stew";
        case K::ByType: return "by_villager_type";
        case K::DyedArmor: return "dyed_armor";
        case K::TippedArrow: return "tipped_arrow";
        case K::TreasureMap: return "treasure_map";
    }
    return "?";
}

i32 mix_dye_colours(std::span<const u8> dyes) noexcept {
    if (dyes.empty()) {
        return -1;
    }
    i32 total_r = 0;
    i32 total_g = 0;
    i32 total_b = 0;
    i32 total_max = 0;
    i32 n = 0;
    for (const u8 dye : dyes) {
        const i32 colour = kDyeColours[dye & 15U];
        // Through the dye's float colour and back, truncated, as the game
        // stores and reads it.
        const auto channel = [colour](i32 shift) {
            const f32 unit = static_cast<f32>((colour >> shift) & 255) / 255.0F;
            return static_cast<i32>(unit * 255.0F);
        };
        const i32 r = channel(16);
        const i32 g = channel(8);
        const i32 b = channel(0);
        total_r += r;
        total_g += g;
        total_b += b;
        total_max += std::max(r, std::max(g, b));
        ++n;
    }
    const i32 avg_r  = total_r / n;
    const i32 avg_g  = total_g / n;
    const i32 avg_b  = total_b / n;
    const f32 bright = static_cast<f32>(total_max) / static_cast<f32>(n);
    const f32 top    = static_cast<f32>(std::max(avg_r, std::max(avg_g, avg_b)));
    const i32 r      = static_cast<i32>(static_cast<f32>(avg_r) * bright / top);
    const i32 g      = static_cast<i32>(static_cast<f32>(avg_g) * bright / top);
    const i32 b      = static_cast<i32>(static_cast<f32>(avg_b) * bright / top);
    return (r << 16) + (g << 8) + b;
}

BookDraw draw_enchanted_book(math::LegacyRandomSource& random) {
    // The 37 in registry order; the draw is an index into that list.
    std::array<Enchantment, kEnchantmentCount> candidates{};
    usize                                       n = 0;
    for (usize i = 0; i < kEnchantmentCount; ++i) {
        const auto e = static_cast<Enchantment>(i);
        if (tradeable(e)) {
            candidates[n++] = e;
        }
    }
    BookDraw out;
    out.enchantment             = candidates[static_cast<usize>(random.next_int(static_cast<i32>(n)))];
    const EnchantmentInfo& info = enchantment_info(out.enchantment);
    out.level                   = next_int_between(random, 1, info.max_level);
    const i32 spread            = random.next_int(5 + out.level * 10);
    i32       price             = 2 + spread + 3 * out.level;
    if (info.treasure) {
        price *= 2;
    }
    out.price = std::min(price, 64);
    return out;
}

std::optional<MerchantOffer> make_offer(const Listing& l, VillagerType type,
                                        math::LegacyRandomSource& random) {
    MerchantOffer offer = base_offer(l);
    switch (l.kind) {
        case K::SellForEmerald:
            offer.cost_a = stack(l.item, l.count);
            offer.result = stack(kEmerald, 1);
            return offer;
        case K::BuyForEmeralds:
            offer.cost_a = stack(kEmerald, l.emeralds);
            offer.result = stack(l.item, l.count);
            return offer;
        case K::Exchange:
            offer.cost_a = stack(kEmerald, l.emeralds);
            offer.cost_b = stack(l.input, l.input_count);
            offer.result = stack(l.item, l.count);
            return offer;
        case K::EnchantedItem: {
            const i32 level           = 5 + random.next_int(15);
            offer.result              = stack(l.item, 1);
            offer.result.enchantments = select_enchantments(random, l.item, level, false);
            offer.cost_a              = stack(kEmerald, std::min(l.emeralds + level, 64));
            return offer;
        }
        case K::EnchantedBook: {
            const BookDraw drawn = draw_enchanted_book(random);
            offer.cost_a         = stack(kEmerald, drawn.price);
            offer.cost_b         = stack("minecraft:book", 1);
            offer.result         = stack(l.item, 1);
            offer.result.stored  = true;
            offer.result.enchantments.set(drawn.enchantment, drawn.level);
            return offer;
        }
        case K::SuspiciousStew:
            offer.cost_a               = stack(kEmerald, l.emeralds);
            offer.result               = stack(l.item, 1);
            offer.result.stew_effect   = l.effect;
            offer.result.stew_duration = l.duration;
            return offer;
        case K::ByType:
            offer.cost_a = stack(l.by_type[static_cast<usize>(type)], l.count);
            offer.result = stack(kEmerald, 1);
            return offer;
        case K::DyedArmor: {
            offer.cost_a = stack(kEmerald, l.emeralds);
            offer.result = stack(l.item, 1);
            if (dyeable_armour(l.item)) {
                std::array<u8, 3> dyes{};
                usize             n = 0;
                dyes[n++]           = static_cast<u8>(random.next_int(16));
                if (random.next_float() > 0.7F) {
                    dyes[n++] = static_cast<u8>(random.next_int(16));
                }
                if (random.next_float() > 0.8F) {
                    dyes[n++] = static_cast<u8>(random.next_int(16));
                }
                offer.result.dye_colour = mix_dye_colours(std::span{dyes}.first(n));
            }
            return offer;
        }
        case K::TippedArrow: {
            offer.cost_a        = stack(kEmerald, l.emeralds);
            offer.cost_b        = stack(l.input, l.input_count);
            offer.result        = stack(l.item, l.count);
            offer.result.potion = kTippedPotions[static_cast<usize>(
                random.next_int(static_cast<i32>(kTippedPotions.size())))];
            return offer;
        }
        case K::TreasureMap:
            // Refused, and named: villageois.md.
            return std::nullopt;
    }
    return std::nullopt;
}

void draw_level_offers(VillagerState& villager, math::LegacyRandomSource& random) {
    const std::span<const Listing> pool = trade_listings(villager.profession, villager.level);
    if (pool.empty()) {
        return;
    }
    std::array<usize, kOffersPerLevel> picked{};
    usize                              count = 0;
    if (pool.size() > kOffersPerLevel) {
        while (count < kOffersPerLevel) {
            const auto index = static_cast<usize>(random.next_int(static_cast<i32>(pool.size())));
            if (std::find(picked.begin(), picked.begin() + static_cast<std::ptrdiff_t>(count),
                          index) == picked.begin() + static_cast<std::ptrdiff_t>(count)) {
                picked[count++] = index;
            }
        }
        // A HashSet<Integer> of two: by bucket (index mod 16), and within one
        // bucket in the order they were drawn.
        if (listed_before(picked[1], picked[0])) {
            std::swap(picked[0], picked[1]);
        }
    } else {
        for (usize i = 0; i < pool.size(); ++i) {
            picked[count++] = i;
        }
    }
    for (usize k = 0; k < count; ++k) {
        if (auto offer = make_offer(pool[picked[k]], villager.type, random)) {
            villager.offers.push_back(std::move(*offer));
        }
    }
}

void ensure_offers(VillagerState& villager) {
    if (villager.offers_drawn) {
        return;
    }
    villager.offers.clear();
    draw_level_offers(villager, villager.random);
    villager.offers_drawn = true;
}

i32 cost_a_count(const MerchantOffer& offer, i32 max_stack) noexcept {
    const i32 base = offer.cost_a.count;
    // Float, as the game computes it: (float)(base * demand) * multiplier.
    const f32 share = static_cast<f32>(base * offer.demand) * offer.price_multiplier;
    const i32 extra = std::max(0, static_cast<i32>(std::floor(share)));
    return std::clamp(base + extra + offer.special_price, 1, std::max(1, max_stack));
}

bool satisfied_by(const MerchantOffer& offer, i32 max_stack_a, std::string_view a_item,
                  i32 a_count, std::string_view b_item, i32 b_count) noexcept {
    if (a_item != offer.cost_a.item || a_count < cost_a_count(offer, max_stack_a)) {
        return false;
    }
    if (offer.cost_b.empty()) {
        // No second cost: the second slot must be empty. Anything there is
        // not "nothing", and the game's check refuses it.
        return b_count <= 0 || b_item.empty();
    }
    return b_item == offer.cost_b.item && b_count >= offer.cost_b.count;
}

TradeOutcome record_trade(VillagerState& villager, MerchantOffer& offer,
                          math::LegacyRandomSource& random) {
    TradeOutcome out;
    ++offer.uses;
    i32 orb = kTradeOrbBase + random.next_int(kTradeOrbSpread);
    villager.xp += offer.xp;
    const i32 need = next_level_xp(villager.level);
    if (need > 0 && villager.xp >= need) {
        villager.update_timer     = kLevelUpDelay;
        villager.level_up_pending = true;
        out.level_up_armed        = true;
        orb += kLevelUpOrbBonus;
    }
    out.orb = offer.reward_exp ? orb : 0;
    return out;
}

bool tick_level_up(VillagerState& villager) {
    if (villager.trading_player >= 0 || villager.update_timer <= 0) {
        return false;
    }
    --villager.update_timer;
    if (villager.update_timer > 0 || !villager.level_up_pending) {
        return false;
    }
    villager.level_up_pending = false;
    if (villager.level >= kMaxLevel) {
        return false;
    }
    ensure_offers(villager);
    ++villager.level;
    draw_level_offers(villager, villager.random);
    ++villager.revision;
    return true;
}

void note_day(VillagerState& villager, i64 day_time) {
    const i64 day = day_time / 24000;
    if (villager.restock_day >= 0 && day != villager.restock_day) {
        villager.restocks_today = 0;
    }
    villager.restock_day = day;
}

bool needs_restock(const VillagerState& villager) noexcept {
    return std::ranges::any_of(villager.offers,
                               [](const MerchantOffer& offer) { return offer.uses > 0; });
}

bool allowed_to_restock(const VillagerState& villager, i64 game_time) noexcept {
    return villager.restocks_today == 0 ||
           (villager.restocks_today < kRestocksPerDay &&
            game_time > villager.last_restock + kRestockMinInterval);
}

void restock(VillagerState& villager, i64 game_time) {
    for (MerchantOffer& offer : villager.offers) {
        offer.demand = offer.demand + offer.uses - (offer.max_uses - offer.uses);
        offer.uses   = 0;
    }
    villager.last_restock = game_time;
    ++villager.restocks_today;
}

}  // namespace ov::gameplay
