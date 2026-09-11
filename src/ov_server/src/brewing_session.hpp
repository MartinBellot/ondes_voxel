// The brewing stand, the potions, and what they do — as this server carries
// them out.
//
// The rules are in ov_gameplay (brewing.hpp): which mix makes what, the
// stand's clock and fuel, the splash law, the cloud. What is here is what
// knows about block entities, windows and players:
//
//   * **the stands**, a ticked block entity like the furnace: indexed from the
//     loaded chunks once a second and run every tick whether anybody is
//     looking or not. The block entity's NBT is the only copy of a stand —
//     `Items`, `BrewTime`, `Fuel`, vanilla's own names — so a hopper that
//     feeds one through `block_container` and a player who opens one see the
//     same thing, and a save written mid-brew reloads mid-brew;
//   * **the window**, `minecraft:brewing_stand`: five slots with three
//     admission rules, and the two bars the client draws from two numbers;
//   * **the potions**: a drink, a splash, a lingering cloud, a tipped arrow,
//     and the suspicious stew, each applied to a player through
//     `EffectSession::with_target`.
//
// Callbacks rather than a reference to the server, as for workbench.hpp and
// item_transport.hpp: this file does not know what a connection, a chunk map or
// a player list is. server.cpp touches it in short blocks marked `// ── brewing ──`.
#pragma once

#include "effect_session.hpp"
#include "entity_storage.hpp"  // ── persistence ──
#include "survival_session.hpp"

#include "ov/gameplay/brewing.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/math/vec.hpp"
#include "ov/protocol/play.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/chunk.hpp"

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ov::server {

// ── Potion items ────────────────────────────────────────────────────────────

/// What a potion item (or a tipped arrow) holds.
struct PotionContents {
    gameplay::Potion potion{gameplay::Potion::Empty};
    /// The potion's effects, then its `CustomPotionEffects`, as vanilla lists
    /// them.
    std::vector<gameplay::PotionEffect> effects;
    /// How many of `effects` are the potion's own; the rest are custom. An
    /// arrow cuts only the first ones to an eighth — measured.
    usize own{0};
    /// The `Potion` tag named something that is not one of the forty-three.
    /// Vanilla reads that as `minecraft:empty`; so does this, and says so.
    bool unknown_name{false};
};

[[nodiscard]] PotionContents potion_contents(const net::ItemStack& stack);

/// The item NBT of a plain potion, `{Potion:"minecraft:…"}`, as wire bytes.
[[nodiscard]] std::vector<u8> potion_tag(gameplay::Potion potion);

/// A suspicious stew's `Effects` (`EffectId`, `EffectDuration`), the 1.20.1
/// shape. An entry with no duration lasts `gameplay::kStewDefaultTicks`.
[[nodiscard]] std::vector<gameplay::PotionEffect> stew_effects(const net::ItemStack& stack);

/// What drinking did to the held stack.
struct DrinkOutcome {
    /// The item was a potion and it was drunk.
    bool drunk{false};
    /// The hand held more than one potion: one was drunk, and a glass bottle
    /// must go into the inventory rather than into the hand.
    bool give_bottle{false};
};

/// A potion's use has finished: apply it, and leave a glass bottle in the hand
/// (not in creative — wiki; checked by the `drink` campaign).
DrinkOutcome drink_potion(const registry::Registries& registries, net::ItemStack& held,
                          bool creative, EffectSession& effects, SurvivalSession& survival,
                          const EffectIo& io, const EffectBearer& bearer);

/// A suspicious stew has been eaten: its effects. Returns true when the item
/// was one (the bowl is the caller's, as with any food).
bool eat_stew(const net::ItemStack& held, EffectSession& effects, SurvivalSession& survival,
              const EffectIo& io, const EffectBearer& bearer);

// ── The stand and its window ────────────────────────────────────────────────

/// The window id this server gives a brewing stand's screen. The chest's is 1
/// and the workbench's 2: three screens that obey different rules, and one id
/// each so that a click can never land in the wrong one.
inline constexpr u8 kBrewingWindowId = 3;

/// One open brewing screen.
struct BrewingWindow {
    u8       window_id{kBrewingWindowId};
    BlockPos pos{};
    i32      state_id{1};
    /// What the client was last told: the two bars, and a signature of each of
    /// the five slots so that a change made by a hopper or by the stand itself
    /// is resent and nothing else is.
    std::array<i16, 2> sent_properties{-1, -1};
    std::array<u64, 5> sent_slots{};
};

/// What the stands reach outside themselves for.
struct StandHost {
    /// The chunk at chunk coordinates, or nullptr when it is not resident.
    std::function<world::Chunk*(i32, i32)> chunk;
    std::function<void(i32, i32)>          mark_dirty;
    /// Write `has_bottle_0..2` on the block, and tell everyone.
    std::function<void(BlockPos, std::array<bool, 3>)> set_bottles;
    /// Put a stack on the ground at a stand (dragon's breath's bottle, when the
    /// ingredient slot is still occupied).
    std::function<void(BlockPos, const net::ItemStack&)> drop;
};

using PacketSink = std::function<void(i32 id, std::span<const u8> payload)>;

// ── Thrown potions, clouds, arrows ──────────────────────────────────────────

struct PotionPlayer {
    i32   entity_id{0};
    Vec3d feet{};
    bool  alive{true};
};

/// The rule a splash, a cloud or an arrow runs against one player's effects.
using EffectRule = std::function<void(gameplay::ActiveEffects&, gameplay::EffectTarget&)>;

struct PotionHost {
    std::function<void(std::vector<PotionPlayer>&)> players;
    /// Run a rule against a player's effects. The server resolves the id and
    /// calls `EffectSession::with_target`, which sends what changed.
    std::function<void(i32 player, const EffectRule&)> affect;
    std::function<i32()> next_entity_id;
    PacketSink           broadcast;
};

/// What the passes did, for the log and the end-to-end check.
struct BrewingStats {
    usize stands{0};
    usize brewing{0};
    usize brewed{0};
    usize refuelled{0};
    usize splashes{0};
    usize clouds{0};
    usize applied{0};
};

class Brewing final : public LooseAdopter {
public:
    explicit Brewing(const registry::Registries& registries);

    // ── persistence: the lingering clouds, a LooseAdopter ───────────────────
    //
    // `minecraft:area_effect_cloud`: `Age`, `Duration`, `DurationOnUse`,
    // `WaitTime`, `ReapplicationDelay` (ints), `Radius`, `RadiusOnUse`,
    // `RadiusPerTick` (floats), `Particle`, `Potion` (strings), the custom
    // `Effects`, `Color` when it came with one; measured on the real server.

    /// Where a cloud read from disk gets its wire id and is announced. Not
    /// owned; must outlive the last read.
    void set_persistence_host(const PotionHost* host) noexcept { persistence_host_ = host; }

    [[nodiscard]] bool owns_type(std::string_view type) const noexcept override {
        return type == "minecraft:area_effect_cloud";
    }
    bool adopt_saved(const nbt::Tag& compound) override;
    void positions(std::vector<Vec3d>& out) const override;
    void save(std::vector<LooseEntity>& out) const override;
    void release(const std::function<bool(ChunkPos)>& leaving, std::vector<LooseEntity>& out,
                 std::vector<i32>& removed) override;
    [[nodiscard]] usize clouds() const noexcept { return clouds_.size(); }

    // ── Stands: the tick thread, `chunk_mutex` held ─────────────────────────

    /// Every brewing stand in the loaded chunks, one tick. The index is
    /// rebuilt once a second, as the furnaces' is.
    BrewingStats tick_stands(const StandHost& host, std::span<const ChunkPos> loaded, i64 now);

    // ── The window: the caller holds `chunk_mutex` ──────────────────────────

    /// Open the screen over the stand at `pos`. False, and nothing sent, when
    /// there is no stand there.
    bool open(const StandHost& host, BlockPos pos, const PacketSink& send,
              std::span<const net::ItemStack> inventory, std::optional<BrewingWindow>& out) const;

    /// One click, and the resend it owes.
    void click(const StandHost& host, BrewingWindow& window, const net::ContainerClick& click,
               std::span<net::ItemStack> inventory, net::ItemStack& carried,
               const PacketSink& send, const std::function<void(const net::ItemStack&)>& drop) const;

    /// Once a tick per open screen: the bars, and the slots when they moved.
    /// False when the stand is gone and the caller must close the screen.
    bool refresh(const StandHost& host, BrewingWindow& window,
                 std::span<const net::ItemStack> inventory, const net::ItemStack& carried,
                 const PacketSink& send) const;

    // ── Potions: the entity pass, `players_mutex` held ──────────────────────

    /// A splash or lingering potion broke at `at`. `direct` is what it hit, 0
    /// for a block.
    void potion_broke(const PotionHost& host, Vec3d at, const net::ItemStack& potion, i32 direct,
                      bool direct_is_player);

    /// An arrow landed a hit on `target`: its potion, or a spectral arrow's glow.
    void arrow_hit(const PotionHost& host, const net::ItemStack& arrow, i32 target,
                   bool target_is_player);

    /// Every cloud, one tick.
    void tick_clouds(const PotionHost& host);

    /// The packets that make every cloud appear, for a client arriving now.
    void cloud_packets(const PacketSink& send) const;

    [[nodiscard]] BrewingStats take_stats() noexcept;

    /// What this module recognises and does not carry out, for the log.
    [[nodiscard]] static std::span<const std::string_view> gaps() noexcept;

private:
    struct StandCache {
        std::string_view brewing{};
        u8               bottles{0xFF};
    };

    struct CloudEntity {
        i32                                 id{0};
        Vec3d                               at{};
        gameplay::Cloud                     cloud{};
        std::vector<gameplay::PotionEffect> effects;
        u32                                 color{gameplay::kWaterColor};
        /// Entity id → the age at which it may be affected again.
        std::unordered_map<i32, i32> victims;
        bool                         sent_waiting{true};
        /// ── persistence ── The potion it came from, how many of `effects`
        /// are that potion's own (the rest are custom), and the compound it
        /// was read with (End when none) with its UUID.
        gameplay::Potion potion{gameplay::Potion::Empty};
        usize            own{0};
        nbt::Tag         saved{};
        net::Uuid        uuid{};
    };

    [[nodiscard]] nbt::Tag cloud_nbt(const CloudEntity& cloud) const;
    void spawn_cloud(const PotionHost& host, Vec3d at, PotionContents contents);
    [[nodiscard]] std::string_view item_name(i32 id) const;
    [[nodiscard]] i32              item_id(std::string_view name) const;

    const registry::Registries*         registries_;
    std::optional<registry::RegistryId> item_registry_;
    i32                                 menu_{-1};
    i32                                 cloud_type_{-1};

    std::vector<BlockPos>                     stands_;
    std::unordered_map<u64, StandCache>       cache_;
    i64                                       indexed_at_{-1};
    std::vector<CloudEntity>                  clouds_;
    const PotionHost*                         persistence_host_{nullptr};  // ── persistence ──
    std::vector<PotionPlayer>                 players_;
    BrewingStats                              stats_{};
};

}  // namespace ov::server
