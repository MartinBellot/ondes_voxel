#include "ov/gameplay/wandering_trader.hpp"

#include "ov/gameplay/villager.hpp"

#include <algorithm>
#include <array>

namespace ov::gameplay {
namespace {

const MobKind kWanderingTrader{
    .type_name      = "minecraft:wandering_trader",
    .category       = MobCategory::Creature,
    .movement_speed = 0.7,
    .stroll         = 0.35,
    .panic          = 0.5,
    .opens_doors    = false,
};

using K = ListingKind;

/// Emeralds for `count` of `item`, `max` uses; xp 1, multiplier 0.05 on every
/// trader offer (measured).
constexpr Listing buy(std::string_view item, i32 count, i32 emeralds, i32 max) {
    Listing l;
    l.kind       = K::BuyForEmeralds;
    l.item       = item;
    l.count      = count;
    l.emeralds   = emeralds;
    l.max_uses   = max;
    l.xp         = 1;
    l.multiplier = 0.05F;
    return l;
}

// The 64 seen in 1000 draws of 200 traders. Pool order is ours (grouped as
// the wiki lists them); only the set, the prices and the uses are measured.
constexpr std::array<Listing, 64> kOrdinary{{
    buy("minecraft:sea_pickle", 1, 2, 5),        buy("minecraft:slime_ball", 1, 4, 5),
    buy("minecraft:glowstone", 1, 2, 5),         buy("minecraft:nautilus_shell", 1, 5, 5),
    buy("minecraft:fern", 1, 1, 12),             buy("minecraft:sugar_cane", 1, 1, 8),
    buy("minecraft:pumpkin", 1, 1, 4),           buy("minecraft:kelp", 1, 3, 12),
    buy("minecraft:cactus", 1, 3, 8),            buy("minecraft:dandelion", 1, 1, 12),
    buy("minecraft:poppy", 1, 1, 12),            buy("minecraft:blue_orchid", 1, 1, 8),
    buy("minecraft:allium", 1, 1, 12),           buy("minecraft:azure_bluet", 1, 1, 12),
    buy("minecraft:red_tulip", 1, 1, 12),        buy("minecraft:orange_tulip", 1, 1, 12),
    buy("minecraft:white_tulip", 1, 1, 12),      buy("minecraft:pink_tulip", 1, 1, 12),
    buy("minecraft:oxeye_daisy", 1, 1, 12),      buy("minecraft:cornflower", 1, 1, 12),
    buy("minecraft:lily_of_the_valley", 1, 1, 7), buy("minecraft:wheat_seeds", 1, 1, 12),
    buy("minecraft:beetroot_seeds", 1, 1, 12),   buy("minecraft:pumpkin_seeds", 1, 1, 12),
    buy("minecraft:melon_seeds", 1, 1, 12),      buy("minecraft:acacia_sapling", 1, 5, 8),
    buy("minecraft:birch_sapling", 1, 5, 8),     buy("minecraft:dark_oak_sapling", 1, 5, 8),
    buy("minecraft:jungle_sapling", 1, 5, 8),    buy("minecraft:oak_sapling", 1, 5, 8),
    buy("minecraft:spruce_sapling", 1, 5, 8),    buy("minecraft:cherry_sapling", 1, 5, 8),
    buy("minecraft:mangrove_propagule", 1, 5, 8), buy("minecraft:red_dye", 3, 1, 12),
    buy("minecraft:white_dye", 3, 1, 12),        buy("minecraft:blue_dye", 3, 1, 12),
    buy("minecraft:pink_dye", 3, 1, 12),         buy("minecraft:black_dye", 3, 1, 12),
    buy("minecraft:green_dye", 3, 1, 12),        buy("minecraft:light_gray_dye", 3, 1, 12),
    buy("minecraft:magenta_dye", 3, 1, 12),      buy("minecraft:yellow_dye", 3, 1, 12),
    buy("minecraft:gray_dye", 3, 1, 12),         buy("minecraft:purple_dye", 3, 1, 12),
    buy("minecraft:light_blue_dye", 3, 1, 12),   buy("minecraft:lime_dye", 3, 1, 12),
    buy("minecraft:orange_dye", 3, 1, 12),       buy("minecraft:brown_dye", 3, 1, 12),
    buy("minecraft:cyan_dye", 3, 1, 12),         buy("minecraft:brain_coral_block", 1, 3, 8),
    buy("minecraft:bubble_coral_block", 1, 3, 8), buy("minecraft:fire_coral_block", 1, 3, 8),
    buy("minecraft:horn_coral_block", 1, 3, 8),  buy("minecraft:tube_coral_block", 1, 3, 8),
    buy("minecraft:vine", 1, 1, 12),             buy("minecraft:brown_mushroom", 1, 1, 12),
    buy("minecraft:red_mushroom", 1, 1, 12),     buy("minecraft:lily_pad", 2, 1, 5),
    buy("minecraft:small_dripleaf", 2, 1, 5),    buy("minecraft:sand", 8, 1, 8),
    buy("minecraft:red_sand", 4, 1, 6),          buy("minecraft:pointed_dripstone", 2, 1, 5),
    buy("minecraft:rooted_dirt", 2, 1, 5),       buy("minecraft:moss_block", 2, 1, 5),
}};

// The 6 of the sixth offer (200 draws: 30, 24, 37, 30, 45, 34).
constexpr std::array<Listing, 6> kRare{{
    buy("minecraft:tropical_fish_bucket", 1, 5, 4), buy("minecraft:pufferfish_bucket", 1, 5, 4),
    buy("minecraft:packed_ice", 1, 3, 6),           buy("minecraft:blue_ice", 1, 6, 6),
    buy("minecraft:gunpowder", 1, 1, 8),            buy("minecraft:podzol", 3, 3, 6),
}};

void add(VillagerState& trader, std::span<const Listing> pool, usize picks,
         math::LegacyRandomSource& random) {
    std::array<usize, kTraderOrdinaryOffers> chosen{};
    usize                                    n = 0;
    while (n < std::min(picks, pool.size())) {
        const auto index = static_cast<usize>(random.next_int(static_cast<i32>(pool.size())));
        if (std::find(chosen.begin(), chosen.begin() + static_cast<std::ptrdiff_t>(n), index) ==
            chosen.begin() + static_cast<std::ptrdiff_t>(n)) {
            chosen[n++] = index;
        }
    }
    std::stable_sort(chosen.begin(), chosen.begin() + static_cast<std::ptrdiff_t>(n),
                     [](usize a, usize b) { return listed_before(a, b); });
    for (usize k = 0; k < n; ++k) {
        if (auto offer = make_offer(pool[chosen[k]], trader.type, random)) {
            trader.offers.push_back(*offer);
        }
    }
}

}  // namespace

const MobKind* wandering_trader_kind(std::string_view type_name) noexcept {
    return type_name == kWanderingTrader.type_name ? &kWanderingTrader : nullptr;
}

void install_wandering_trader_goals(GoalSelector& selector, const MobKind& kind, i32 look_type) {
    selector.add(0, std::make_unique<FloatGoal>());
    selector.add(1, std::make_unique<VillagerPanicGoal>(kind.speed(kind.panic)));
    selector.add(1, std::make_unique<TradeWithPlayerGoal>());
    selector.add(8, std::make_unique<RandomStrollGoal>(kind.speed(kind.stroll)));
    selector.add(9, std::make_unique<LookAtEntityGoal>(look_type, 8.0, 0.02F));
    selector.add(10, std::make_unique<RandomLookGoal>());
}

std::span<const Listing> wandering_trader_offers() noexcept { return kOrdinary; }
std::span<const Listing> wandering_trader_rare_offers() noexcept { return kRare; }

void init_wandering_trader(VillagerState& trader, math::LegacyRandomSource& random) {
    trader.active     = true;
    trader.wandering  = true;
    trader.typed      = true;
    trader.profession = Profession::None;
    trader.offers.clear();
    add(trader, kOrdinary, kTraderOrdinaryOffers, random);
    add(trader, kRare, 1, random);
    trader.offers_drawn = true;
}

bool tick_despawn(VillagerState& trader) noexcept {
    if (trader.despawn_delay <= 0 || trader.trading_player >= 0) {
        return false;
    }
    return --trader.despawn_delay == 0;
}

bool TraderSpawner::tick(math::LegacyRandomSource& random, bool allowed) noexcept {
    if (--tick_delay > 0) {
        return false;
    }
    tick_delay = 1200;
    spawn_delay -= 1200;
    if (spawn_delay > 0) {
        return false;
    }
    spawn_delay = 24000;
    if (!allowed) {
        return false;
    }
    const i32 chance = spawn_chance;
    spawn_chance     = std::clamp(spawn_chance + 25, 25, 75);
    return random.next_int(100) <= chance;
}

}  // namespace ov::gameplay
