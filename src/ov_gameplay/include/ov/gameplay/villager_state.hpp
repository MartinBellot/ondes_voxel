// What a villager carries that a cow does not: a type, a profession, a level,
// experience, a job site, a bed, and the offers it trades.
//
// Plain structs in a header of their own, for the reason animal.hpp is one:
// `MobBrain` (goals.hpp) holds a VillagerState, and the rules that act on it
// (villager.hpp, trading.hpp) need the goals — a header each way would be a
// cycle. What gives these fields a meaning is measured, and lives in those two
// headers with its provenance; see docs/provenance/villageois.md.
#pragma once

#include "ov/base/types.hpp"
#include "ov/gameplay/brain/gossip.hpp"  // ── brains ──
#include "ov/gameplay/enchanting.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/math/random.hpp"
#include "ov/math/vec.hpp"

#include <optional>
#include <string_view>
#include <vector>

namespace ov::gameplay {

/// `minecraft:villager_type`, in registry order — which is the network id the
/// client hard-codes and VillagerData carries. Checked against registries.json
/// by test_villager.cpp.
enum class VillagerType : u8 { Desert, Jungle, Plains, Savanna, Snow, Swamp, Taiga };
inline constexpr usize kVillagerTypeCount = 7;

/// `minecraft:villager_profession`, in registry order, likewise.
enum class Profession : u8 {
    None,
    Armorer,
    Butcher,
    Cartographer,
    Cleric,
    Farmer,
    Fisherman,
    Fletcher,
    Leatherworker,
    Librarian,
    Mason,
    Nitwit,
    Shepherd,
    Toolsmith,
    Weaponsmith,
};
inline constexpr usize kProfessionCount = 15;

/// One side of a trade, as the rules see it: an item by registry name, a count,
/// and the few tags a trade can put on what it sells. The names point into
/// static tables (trading.cpp) and outlive every offer.
struct TradeItem {
    std::string_view item;
    i32              count{0};
    /// `Enchantments`, or `StoredEnchantments` when `stored` (a book).
    EnchantmentList enchantments{};
    bool            stored{false};
    /// The suspicious stew's one effect (network id, 1-based) and duration.
    i32 stew_effect{0};
    i32 stew_duration{0};
    /// Leather armour's `display.color`; -1 for none.
    i32 dye_colour{-1};
    /// A tipped arrow's `Potion`, a registry name; empty for none.
    std::string_view potion;

    [[nodiscard]] bool empty() const noexcept { return item.empty() || count <= 0; }
};

/// One offer, with vanilla's field names in mind (`MerchantOffer`).
struct MerchantOffer {
    TradeItem cost_a;
    TradeItem cost_b;
    TradeItem result;
    i32       uses{0};
    i32       max_uses{0};
    /// The villager's experience per trade (`xp`).
    i32 xp{0};
    /// Reputation's share of the price (`specialPrice`). Always 0 here: gossip
    /// is not modelled — named in villageois.md.
    i32 special_price{0};
    i32 demand{0};
    f32 price_multiplier{0.05F};
    /// The trade pays the player an orb (`rewardExp`). True for every villager
    /// offer vanilla draws.
    bool reward_exp{true};

    [[nodiscard]] bool out_of_stock() const noexcept { return uses >= max_uses; }
};

/// A job site or a bed the villager has chosen and not yet reached, or reached.
struct VillagerClaims {
    std::optional<BlockPos> potential_job_site;
    std::optional<BlockPos> job_site;
    std::optional<BlockPos> home;
    /// ── brains ── the bell: a meeting point is shared (32 tickets), never
    /// "claimed" against another villager.
    std::optional<BlockPos> meeting_point;
};

/// The incremental scan for free job blocks and beds. A full sphere of 48 is
/// four hundred thousand blocks: it is walked a slab at a time over many
/// ticks, never in one.
struct VillagerScan {
    bool     active{false};
    BlockPos origin{};
    i32      layer{0};
    i32      row{0};
    std::optional<BlockPos> best_job;
    i64                     best_job_distance{0};
    std::optional<BlockPos> best_bed;
    i64                     best_bed_distance{0};
    std::optional<BlockPos> best_bell;  // ── brains ──
    i64                     best_bell_distance{0};
    /// No new scan before this tick.
    i64 next_scan_tick{0};
};

/// The villager half of a mob. `active` false on every other mob.
struct VillagerState {
    bool         active{false};
    VillagerType type{VillagerType::Plains};
    Profession   profession{Profession::None};
    i32          level{1};
    /// `Xp`: the villager's experience, not the player's.
    i32 xp{0};

    std::vector<MerchantOffer> offers;
    /// Offers are drawn lazily, the first time anything asks — measured: a
    /// summoned villager with a profession and no `Offers` has a fresh draw the
    /// first time it is saved. A change of profession undraws them.
    bool offers_drawn{false};

    VillagerClaims claims;
    VillagerScan   scan;

    /// Restocking (`RestocksToday`, `LastRestock`), and the day it counts in.
    i32 restocks_today{0};
    i64 last_restock{-1};
    i64 restock_day{-1};
    i64 next_work_tick{0};

    /// A trade that crossed a threshold arms a level-up, carried out once the
    /// screen has been closed for `update_timer` ticks.
    i32  update_timer{0};
    bool level_up_pending{false};

    /// The player trading, by entity id, and where their eyes are. -1: nobody.
    i32   trading_player{-1};
    Vec3d trading_with{};

    /// Ticks of fright left after being hurt.
    i32 hurt_ticks{0};
    /// Asleep in `claims.home`.
    bool sleeping{false};
    /// Ticks the head keeps shaking (the "unhappy counter").
    i32 unhappy{0};

    /// Bumped on every change a client must hear about: type, profession,
    /// level, sleep. The server compares it with what it last sent.
    u32 revision{0};

    /// The villager's own generator, for its offers. Per villager for the
    /// reason every mob has its own: a shared one makes the world depend on
    /// the order villagers were ticked in.
    math::LegacyRandomSource random{0};

    // ── brains ── gossip, its day, the villager's pockets
    brain::Gossips gossips;
    /// `LastGossipDecay`: 0 until the first check sets it (measured).
    i64 last_gossip_decay{0};
    /// The last time this villager gossiped: at most once in 1200 ticks.
    /// Not saved (vanilla does not either).
    i64 last_gossip_time{-1};
    /// `Inventory`: eight slots, an item by registry name (static strings:
    /// only what a villager picks up or harvests) and a count.
    struct Slot {
        std::string_view item;
        i32              count{0};
    };
    std::array<Slot, 8> inventory{};
    /// `FoodLevel`: food already eaten towards breeding.
    i32 food_level{0};
    /// A wandering trader: trades, but has no VillagerData, no level, no job.
    bool wandering{false};
    /// The type was decided (read from disk, kept through a cure, drawn at a
    /// birth). A villager without one takes its biome's (measured, 53 biomes).
    bool typed{false};
    /// `DespawnDelay` of a wandering trader: ticks left, 0 for never.
    i32 despawn_delay{0};
};

}  // namespace ov::gameplay
