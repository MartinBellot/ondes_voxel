// Villagers, as this server carries them out: a right-click that opens the
// trading screen or shakes a head, the screen itself (Select Trade, clicks on
// the payment and result slots, closing), the Merchant Offers packet, and the
// metadata that tells clients a villager's type, profession, level and sleep.
//
// The rules are in ov_gameplay (villager.hpp, trading.hpp). What is here is
// the part that knows about inventories, registries and sockets. Callbacks
// rather than a reference to the server, as for husbandry.hpp.
//
// ── Threads ─────────────────────────────────────────────────────────────────
//
// The `queue_*` calls run on the network thread and only record what the
// client sent. Everything else runs on the tick thread, in the entity block,
// with the players' lock held — so a villager's offers, the player's
// inventory and the screen are only ever touched by one thread. The set of
// open merchant windows is the one thing both threads read, under `mutex_`.
//
// ── Provenance ──────────────────────────────────────────────────────────────
//
// Every packet id and metadata index below was captured from a real 1.20.1
// server by scripts/measure_villagers.py; docs/provenance/villageois.md.
#pragma once

#include "ov/entity/world.hpp"
#include "ov/gameplay/mob_logic.hpp"
#include "ov/gameplay/villager.hpp"
#include "ov/protocol/entity.hpp"
#include "ov/protocol/interaction.hpp"
#include "ov/protocol/play.hpp"
#include "ov/registry/registries.hpp"

#include <array>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ov::server {

/// Serverbound Select Trade, protocol 763. The archive's number; confirmed by
/// the real server answering it (the payment slot filled from the inventory).
inline constexpr i32 kSelectTrade = 0x26;
/// Clientbound Merchant Offers. Identified by its content in a capture — the
/// archive has been wrong about ids before (the Explosion packet).
inline constexpr i32 kMerchantOffers = 0x2A;
/// The window id this server gives a trading screen. The chest is 1, the
/// workbench 2, the enchanting screens 3.
inline constexpr u8 kMerchantWindow = 4;
/// Slots of the merchant menu: two payments, the result, then the player's 36.
inline constexpr i16 kMerchantSlots = 39;

/// Metadata indices, each measured one NBT field at a time against a baseline
/// of the same type.
namespace villager_metadata {
/// VarInt: the head shake, 40 on a refused click and counting down.
inline constexpr u8 kUnhappy = 17;
/// VillagerData (type 18): the villager's type, profession, level. Sent on
/// every spawn, even at plains / none / 1.
inline constexpr u8 kData = 18;
/// Pose (type 20) and the sleeping position (optional block pos, type 11):
/// `SleepingX/Y/Z` moves both, the pose to 2.
inline constexpr u8  kPose          = 6;
inline constexpr u8  kSleepingPos   = 14;
inline constexpr i32 kPoseStanding  = 0;
inline constexpr i32 kPoseSleeping  = 2;
/// Zombie villager: converting (boolean) and its VillagerData.
inline constexpr u8 kZombieConverting = 19;
inline constexpr u8 kZombieData       = 20;
}  // namespace villager_metadata

/// Send one packet to every client.
using VillagerDeliver = std::function<void(i32 id, std::span<const u8> payload)>;

/// One player, as a trading screen sees them.
struct MerchantPlayer {
    i32   entity_id{0};
    Vec3d eyes{};
    /// The player's 46 slots, as the protocol numbers them.
    std::span<net::ItemStack> inventory;
    net::ItemStack*           carried{nullptr};
    /// A packet to this player only.
    std::function<void(i32 id, std::span<const u8> payload)> send;
    std::function<void(const net::ItemStack&)>               drop;
    // ── brains ── who they are to a villager's gossip, and Hero of the
    // Village's amplifier (-1: none), for the prices
    net::Uuid uuid{};
    i32       hero_amplifier{-1};
};

/// ── brains ── The special price of every offer for this player: reputation
/// (`-floor(rep × multiplier)`) plus Hero of the Village (`-max(1,
/// floor((0.3 + 0.0625 a) × base))`). Measured: 90 offers of 18 villagers,
/// every one equal (docs/provenance/cerveaux.md).
void apply_special_prices(gameplay::VillagerState& v, const net::Uuid& player, i32 hero_amplifier);

struct VillagerHost {
    /// Lend a connected player. False when the player is gone.
    std::function<bool(i32 player, const std::function<void(MerchantPlayer&)>&)> with_player;
    std::function<void(Vec3d at, i32 value)>                                     spawn_orb;
};

struct VillagerStats {
    usize opened{0};
    usize refused{0};
    usize trades{0};
    usize metadata{0};
};

/// The stack a trade side becomes, NBT included: `StoredEnchantments` on a
/// book, `Enchantments` on a tool, a stew's `Effects`, a dye's
/// `display.color`, an arrow's `Potion`, and `Damage:0` on anything that wears
/// out — every such stack a real villager sells carries it.
[[nodiscard]] net::ItemStack trade_stack(const registry::Registries& registries,
                                         registry::RegistryId item_registry,
                                         const gameplay::TradeItem& item);

/// The body of Merchant Offers. Field order captured byte for byte from the
/// real server: VarInt window, VarInt count, per offer (slot, slot, slot,
/// bool disabled, int uses, int max, int xp, int special, float multiplier,
/// int demand), then VarInt level, VarInt experience, bool regular, bool
/// restock. The first cost goes out at its base count: the client adds the
/// demand itself — measured, 17 emeralds on the wire for an offer at demand 4.
[[nodiscard]] std::vector<u8> encode_merchant_offers(const registry::Registries& registries,
                                                     registry::RegistryId item_registry,
                                                     u8 window, const gameplay::VillagerState& v);

/// Select Trade's one field: the offer's index, a VarInt.
[[nodiscard]] std::optional<i32> parse_select_trade(std::span<const u8> payload);

/// The screen title: the villager's name, as the real server sends it — a
/// translation with the entity's hover event and its UUID as insertion.
[[nodiscard]] std::string merchant_title(const gameplay::VillagerState& v, const net::Uuid& uuid);

class Villagers {
public:
    Villagers(const registry::Registries& registries, const registry::BlockRegistry& blocks);

    // ── The network thread ──────────────────────────────────────────────────

    void queue_interact(i32 player, i32 entity, net::Hand hand, bool sneaking);
    /// True when the player has a trading screen open: the packet is ours.
    bool queue_select(i32 player, i32 index);
    bool queue_click(i32 player, const net::ContainerClick& click);
    bool queue_close(i32 player, u8 window);

    // ── The tick thread ─────────────────────────────────────────────────────

    /// Carry out what the clients asked, and point the entity tick at this
    /// tick's time and hostile types. Before `EntityWorld::tick`.
    VillagerStats before_entity_tick(entity::EntityWorld& world, gameplay::MobContext& context,
                                     const VillagerHost& host, const VillagerDeliver& deliver,
                                     i64 day_time, i64 game_time);

    /// Tell clients what changed on a villager this tick. After the tick.
    VillagerStats after_entity_tick(entity::EntityWorld& world, const VillagerDeliver& deliver);

    /// A villager's fields in a spawn's metadata.
    void spawn_metadata(entity::EntityWorld& world, const entity::EntityState& state,
                        net::MetadataWriter& fields) const;

    /// ── brains ── What the entity tick hands every villager: the time, the
    /// hostiles, and (set by the villager-life module) the event sink.
    [[nodiscard]] gameplay::VillagerWorld& world() noexcept { return world_; }

private:
    enum class ActionKind : u8 { Interact, Select, Click, Close };
    struct Action {
        ActionKind          kind{ActionKind::Interact};
        i32                 player{0};
        i32                 entity{0};
        net::Hand           hand{net::Hand::Main};
        bool                sneaking{false};
        i32                 index{0};
        u8                  window{0};
        net::ContainerClick click{};
    };
    struct Window {
        i32                           player{0};
        i32                           villager{0};
        std::array<net::ItemStack, 3> slots{};
        i32                           hint{0};
        i32                           state_id{1};
        /// The offer the result slot shows, or -1.
        i32 active{-1};
        net::Uuid player_uuid{};  // ── brains ── a trade is gossip about them
    };

    void interact(entity::EntityWorld& world, const Action& a, const VillagerHost& host,
                  const VillagerDeliver& deliver, VillagerStats& stats);
    void select(gameplay::VillagerState& v, Window& w, i32 index, MerchantPlayer& p);
    void click(entity::EntityWorld& world, gameplay::VillagerState& v, Window& w,
               const net::ContainerClick& click, MerchantPlayer& p, const VillagerHost& host,
               VillagerStats& stats);
    void close(entity::EntityWorld& world, Window& w, MerchantPlayer* p);
    /// The result slot, from what the payment slots hold.
    void refresh(gameplay::VillagerState& v, Window& w) const;
    /// Take the result: pay, count the trade, pay the orb.
    bool take(entity::EntityWorld& world, gameplay::VillagerState& v, Window& w,
              const VillagerHost& host, VillagerStats& stats);
    void send_contents(Window& w, MerchantPlayer& p) const;
    void send_data(const entity::EntityState& state, const gameplay::VillagerState& v,
                   const VillagerDeliver& deliver) const;

    [[nodiscard]] bool pays(const gameplay::MerchantOffer& offer, const net::ItemStack& a,
                            const net::ItemStack& b) const;
    [[nodiscard]] i32  cost_a(const gameplay::MerchantOffer& offer) const;
    [[nodiscard]] gameplay::VillagerState* villager_of(entity::EntityWorld& world,
                                                       i32 network_id) const;
    [[nodiscard]] std::string_view item_name(i32 id) const;
    [[nodiscard]] i32              item_id(std::string_view name) const;
    [[nodiscard]] i32              stack_limit(i32 item) const;

    const registry::Registries*    registries_;
    const registry::BlockRegistry* blocks_;
    registry::RegistryId           item_registry_{};
    registry::RegistryId           menu_registry_{};
    i32                            merchant_menu_{-1};

    std::vector<gameplay::HostileSight> hostiles_;
    gameplay::VillagerWorld              world_{};

    mutable std::mutex          mutex_;
    std::vector<Action>         pending_;
    std::vector<Action>         working_;
    std::unordered_map<i32, u8> open_;  // player -> window, under mutex_

    std::vector<Window>          windows_;
    std::unordered_map<i32, u32> sent_revision_;
    /// Each villager's health last tick: a drop is a hit, and a hit makes it
    /// run. Watched here because nothing on the server tells a mob it was hurt.
    std::unordered_map<i32, f32> last_health_;
};

}  // namespace ov::server
