// Gossip: what a villager has heard about whom, and the reputation it adds up
// to. Plain data and rules, held by a villager (villager_state.hpp).
//
// ── The model ───────────────────────────────────────────────────────────────
//
// A villager keeps, per target (a player's or an entity's UUID), a value for
// each of five gossip types. A type has a weight — reputation is the sum of
// value × weight over the types — a maximum, what a day of forgetting takes
// off, and what it loses when it is passed to another villager:
//
//   type             weight  max  per day  per transfer
//   major_negative     -5    100    10          10
//   minor_negative     -1    200    20          20
//   minor_positive      1    200     1           5
//   major_positive      5    100     0         100     (never forgotten, never passed on)
//   trading             1     25     2          20
//
// The table is the Minecraft Wiki's ("Villager", Gossiping). Its weights are
// what the price campaign measures — the special price of every offer under
// each type — and its daily losses what the decay campaign measures; see
// docs/provenance/cerveaux.md for the numbers that were confirmed.
//
// ── Allocation ──────────────────────────────────────────────────────────────
//
// Gossip changes on events — a trade, a hit, a cure, a meeting — and once a
// day; never on an ordinary tick. The entries live in a vector reserved at
// creation, so a village's usual handful of players costs no allocation.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/random.hpp"
#include "ov/protocol/types.hpp"

#include <array>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ov::gameplay::brain {

enum class GossipType : u8 { MajorNegative, MinorNegative, MinorPositive, MajorPositive, Trading };
inline constexpr usize kGossipTypeCount = 5;

struct GossipTypeInfo {
    std::string_view name;
    i32              weight{0};
    i32              max{0};
    i32              decay_per_day{0};
    i32              decay_per_transfer{0};
};
[[nodiscard]] const GossipTypeInfo& gossip_info(GossipType type) noexcept;
[[nodiscard]] std::optional<GossipType> gossip_type_from_name(std::string_view name) noexcept;

/// What happened, as the villagers who saw it tell it (vanilla's
/// `ReputationEventType`), and the gossip each adds.
enum class ReputationEvent : u8 {
    /// A zombie villager cured by this player: major_positive 20 and
    /// minor_positive 25, to the villager it became.
    ZombieVillagerCured,
    /// A trade: trading 2.
    Trade,
    /// A hit: minor_negative 25.
    VillagerHurt,
    /// A villager killed, told to the villagers who saw it: major_negative 25.
    VillagerKilled,
    /// An iron golem killed: major_negative 25? Not measured — named.
    GolemKilled,
};

struct GossipEntry {
    net::Uuid  target{};
    GossipType type{GossipType::Trading};
    i32        value{0};
};

/// Days between two losses: `LastGossipDecay` + this is when the next is due.
inline constexpr i64 kGossipDecayInterval = 24000;

class Gossips {
public:
    Gossips() { entries_.reserve(16); }

    /// Add `amount` to a gossip. Past the type's maximum the value stays at the
    /// larger of the maximum and what it was (a value read over the maximum is
    /// not cut down by a small addition).
    void add(const net::Uuid& target, GossipType type, i32 amount);
    /// The event's gossip, added.
    void add_event(const net::Uuid& target, ReputationEvent event);

    /// Sum of value × weight for one target.
    [[nodiscard]] i32 reputation(const net::Uuid& target) const noexcept;
    /// The value of one gossip, 0 when there is none.
    [[nodiscard]] i32 value(const net::Uuid& target, GossipType type) const noexcept;

    /// One day of forgetting: every value loses its type's daily loss and is
    /// dropped once it falls under `kGossipKeepAtLeast`.
    void decay();

    /// What a villager hears from another: up to `picks` of the other's
    /// gossips, drawn with a chance proportional to |value × weight|, each
    /// arriving less the type's transfer loss; kept when still at least
    /// `kGossipKeepAtLeast`, and never lowering what this one already knew.
    void transfer_from(const Gossips& other, math::LegacyRandomSource& random, i32 picks = 10);

    /// Every entry, in insertion order. Read-only: `add` keeps the invariants.
    [[nodiscard]] std::span<const GossipEntry> entries() const noexcept { return entries_; }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
    void clear() noexcept { entries_.clear(); }
    /// An entry read from disk, as it was (no clamping: vanilla does not).
    void put(const GossipEntry& entry);

private:
    [[nodiscard]] GossipEntry*       find(const net::Uuid& target, GossipType type) noexcept;
    [[nodiscard]] const GossipEntry* find(const net::Uuid& target, GossipType type) const noexcept;

    std::vector<GossipEntry> entries_;
};

/// A decayed or passed-on value under this is forgotten. Measured by the decay
/// campaign's edge cell (docs/provenance/cerveaux.md).
inline constexpr i32 kGossipKeepAtLeast = 2;

/// When `LastGossipDecay` is due, forget a day. `last_decay` 0 means "never":
/// it becomes the current time and nothing is forgotten — measured, a villager
/// summoned with `LastGossipDecay:0` read back the current game time and all
/// its gossip. True when a day was forgotten.
bool maybe_decay(Gossips& gossips, i64& last_decay, i64 game_time);

// ── Prices ──────────────────────────────────────────────────────────────────

/// The special price reputation gives an offer: `-floor(reputation ×
/// multiplier)`, the product in float as the game computes it.
[[nodiscard]] i32 reputation_price_diff(i32 reputation, f32 multiplier) noexcept;

/// The special price Hero of the Village gives an offer whose base cost is
/// `base_count`: `-max(1, floor((0.3 + 0.0625 × amplifier) × base))`, in
/// double.
[[nodiscard]] i32 hero_price_diff(i32 amplifier, i32 base_count) noexcept;

}  // namespace ov::gameplay::brain
