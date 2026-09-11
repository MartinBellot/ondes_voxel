// Trading: what a villager offers at each level, what an offer costs, what a
// trade does to the villager, and how it restocks.
//
// Rules only, on VillagerState (villager_state.hpp). Slots, stacks with NBT
// and packets are the server's (ov_server/src/merchant_session.{hpp,cpp}).
//
// ── Provenance ──────────────────────────────────────────────────────────────
//
// The data generator does not report trades: in vanilla they are code. The
// structure of the tables — one pool per profession and level, two offers
// drawn from it, the listing kinds — is the Minecraft Wiki's "Trading" page.
// The contents of every pool, and every number below, were then **measured**
// on a real 1.20.1 server by scripts/measure_villagers.py: a villager draws
// its offers the first time anything asks for them, and saving the entity
// asks, so `data get entity … Offers` on freshly summoned villagers samples
// the real pools (4298 offers from 2319 villagers). scripts/check_trades.py
// confronts the pools below with those samples, row by row;
// docs/provenance/villageois.md has the counts and where the wiki was wrong.
#pragma once

#include "ov/base/types.hpp"
#include "ov/gameplay/villager_state.hpp"
#include "ov/math/random.hpp"

#include <array>
#include <optional>
#include <span>
#include <string_view>

namespace ov::gameplay {

// ── Levels ──────────────────────────────────────────────────────────────────

/// Villager experience needed to *reach* each level, 1-based: novice 0,
/// apprentice 10, journeyman 70, expert 150, master 250. Measured on the
/// real server one point either side of each threshold (9 stays, 10 rises).
inline constexpr std::array<i32, 6> kLevelThresholds{0, 0, 10, 70, 150, 250};
inline constexpr i32                kMaxLevel = 5;

/// The experience a villager at `level` needs for the next one; 0 at master.
[[nodiscard]] constexpr i32 next_level_xp(i32 level) noexcept {
    return level >= 1 && level < kMaxLevel ? kLevelThresholds[static_cast<usize>(level + 1)] : 0;
}

/// Ticks between the trade that crossed a threshold and the level up, counted
/// only while no one is trading. Measured: the level read 1 at 1.87 s after
/// the screen closed and 2 at 2.13 s — 40 ticks is inside the bracket.
inline constexpr i32 kLevelUpDelay = 40;
/// Offers drawn from a level's pool.
inline constexpr usize kOffersPerLevel = 2;
/// A trade's orb is 3 + next_int(4); 5 more when the trade arms a level up.
/// Measured: 3, 4, 5, 6 on plain trades; 8, 9, 10, 11 on the ones that rose.
inline constexpr i32 kTradeOrbBase    = 3;
inline constexpr i32 kTradeOrbSpread  = 4;
inline constexpr i32 kLevelUpOrbBonus = 5;

// ── Restocking ──────────────────────────────────────────────────────────────

/// Restocks per day, and the ticks that must pass between two. Measured: the
/// second restock came 2437 ticks after the first, and no third in 3883 more.
inline constexpr i32 kRestocksPerDay     = 2;
inline constexpr i64 kRestockMinInterval = 2400;

// ── Listings ────────────────────────────────────────────────────────────────

/// The kinds of pool entry vanilla has. Named after what the player does.
enum class ListingKind : u8 {
    /// The player sells `count` of `item` for one emerald.
    SellForEmerald,
    /// The player buys `count` of `item` for `emeralds`.
    BuyForEmeralds,
    /// The player gives `emeralds` and `input_count` of `input` for `count`
    /// of `item` (cod and salmon cooked, gravel to flint).
    Exchange,
    /// An enchanted tool or armour: `emeralds` plus the enchanting level,
    /// capped at 64; enchanted at 5 + next_int(15) levels, no treasure.
    EnchantedItem,
    /// An enchanted book for emeralds and a book.
    EnchantedBook,
    /// A suspicious stew with one fixed effect.
    SuspiciousStew,
    /// Something that depends on the villager's type (the fisherman's boat).
    ByType,
    /// Leather armour, dyed with one to three random dyes. Horse armour is
    /// sold plain: measured, none of its samples carried a colour.
    DyedArmor,
    /// A tipped arrow of a random brewable potion, for emeralds and arrows.
    TippedArrow,
    /// An explorer map. **Refused**: it needs a structure search this
    /// server does not have. Vanilla skips it too when no structure is found
    /// (the sampling world had none: cartographers of levels 2 and 3 had one
    /// offer, never two).
    TreasureMap,
};

struct Listing {
    ListingKind      kind{ListingKind::SellForEmerald};
    std::string_view item;
    i32              count{1};
    i32              emeralds{1};
    i32              max_uses{12};
    i32              xp{1};
    f32              multiplier{0.05F};
    std::string_view input;
    i32              input_count{0};
    /// SuspiciousStew: the effect's network id and duration in ticks.
    i32 effect{0};
    i32 duration{0};
    /// ByType: the item per villager type, in VillagerType order.
    std::array<std::string_view, kVillagerTypeCount> by_type{};
};

/// The pool for a profession at a level (1..5). Empty for none and nitwit.
[[nodiscard]] std::span<const Listing> trade_listings(Profession profession, i32 level) noexcept;

/// One offer from one listing, or nothing when the listing is a refused kind
/// (vanilla also skips a listing that yields nothing — a map with no
/// structure to point at).
[[nodiscard]] std::optional<MerchantOffer> make_offer(const Listing& listing, VillagerType type,
                                                      math::LegacyRandomSource& random);

/// The order two picked pool indices are listed in. Not pool order: vanilla
/// keeps the picks in a `HashSet<Integer>`, which iterates by bucket — the
/// index modulo 16 — so in a pool of more than sixteen entries index 23 comes
/// before index 10. Measured on the mason's and shepherd's big pools, where
/// pool order alone put 9 to 16 villagers of 30 out of order.
[[nodiscard]] constexpr bool listed_before(usize a, usize b) noexcept {
    return (a & 15U) < (b & 15U);
}

/// Add this level's offers: two distinct pool entries at random (or the whole
/// pool when it has two or fewer), in the order above.
void draw_level_offers(VillagerState& villager, math::LegacyRandomSource& random);

/// Draw the offers if they have not been drawn yet: the current level's pool
/// only — measured, a villager summoned at level 3 with no offers has two
/// offers, both from the level-3 pool.
void ensure_offers(VillagerState& villager);

/// The enchanted book's pick: one of the 37 tradeable enchantments with equal
/// chance, a level uniform in its range, and the price
/// `2 + next_int(5 + 10·level) + 3·level`, doubled for a treasure, capped at 64.
/// Measured: 248 books, all 37 enchantments, every price inside its range.
struct BookDraw {
    Enchantment enchantment{Enchantment::Protection};
    i32         level{1};
    i32         price{0};
};
[[nodiscard]] BookDraw draw_enchanted_book(math::LegacyRandomSource& random);

/// The enchantments a trade may put on a book: all but Soul Speed and Swift
/// Sneak, in registry order.
[[nodiscard]] bool tradeable(Enchantment enchantment) noexcept;

/// The colour one to three dyes (DyeColor ids, 0 white .. 15 black) give
/// leather armour: the average of their colours, brightened back to the
/// average of their brightest channels — the wiki's "Dye" rule, in float as
/// the game computes it. Explains all 65 colours sampled, out of 936 the rule
/// can reach at all.
[[nodiscard]] i32 mix_dye_colours(std::span<const u8> dyes) noexcept;

/// The 37 potions a tipped arrow may carry: those with an effect that can be
/// brewed, in registry order.
[[nodiscard]] std::span<const std::string_view> tradeable_potions() noexcept;

/// What the listing kinds are called, for logs.
[[nodiscard]] std::string_view listing_kind_name(ListingKind kind) noexcept;

// ── Prices ──────────────────────────────────────────────────────────────────

/// The first cost's count as the trade asks it: the base, plus the demand's
/// share `max(0, floor(base * demand * multiplier))` computed in float as the
/// game does, plus the special price, clamped to [1, max_stack]. Measured on
/// four offers, n - 1 emeralds refused and n accepted each time, including
/// 9 × 0.05 × 11 = 4.95 → 13.
[[nodiscard]] i32 cost_a_count(const MerchantOffer& offer, i32 max_stack) noexcept;

/// Do these two stacks pay for the offer? Item for item, count at least the
/// cost's; with no second cost, the second slot must be empty. Tags on the
/// payment are ignored: no vanilla cost has one.
[[nodiscard]] bool satisfied_by(const MerchantOffer& offer, i32 max_stack_a,
                                std::string_view a_item, i32 a_count, std::string_view b_item,
                                i32 b_count) noexcept;

// ── Trading ─────────────────────────────────────────────────────────────────

struct TradeOutcome {
    /// The experience orb the player is paid; 0 when the offer pays none.
    i32  orb{0};
    bool level_up_armed{false};
};

/// A trade went through: one more use, the villager's experience, the orb.
TradeOutcome record_trade(VillagerState& villager, MerchantOffer& offer,
                          math::LegacyRandomSource& random);

/// One tick of the level-up timer. True on the tick the level went up (and the
/// new level's offers were added). Waits while someone is trading.
bool tick_level_up(VillagerState& villager);

/// A new day resets the count of restocks.
void note_day(VillagerState& villager, i64 day_time);

/// Some offer has been used since the last restock.
[[nodiscard]] bool needs_restock(const VillagerState& villager) noexcept;

/// The count allows it: none yet today, or fewer than two and 2400 ticks since
/// the last.
[[nodiscard]] bool allowed_to_restock(const VillagerState& villager, i64 game_time) noexcept;

/// Restock every offer: its demand moves by what was bought against what was
/// not (`demand + uses - (max_uses - uses)`), then its uses go to 0. Measured:
/// uses 0 / 5 / 12 of 12 from demand 0 gave -12 / -2 / 12.
void restock(VillagerState& villager, i64 game_time);

}  // namespace ov::gameplay
