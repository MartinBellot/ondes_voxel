// Husbandry, as this server carries it out: a right-click on an animal, the
// births, eggs and grazing the entity tick reports, and what clients are told.
//
// The rules are in ov_gameplay (breeding.hpp): what food is, how much a calf
// grows, how many wool a shearing gives. What is here is the part that knows
// about inventories and sockets.
//
// Callbacks rather than a reference to the server, as for projectiles.hpp and
// tnt_gravity.hpp: this file does not know what a `Player` or a chunk map is.
//
// ── Threads ─────────────────────────────────────────────────────────────────
//
// `queue_interact` runs on the network thread, inside the packet handler, and
// only records the click. Everything else runs on the tick thread, inside the
// entity block, with the players' lock held — so an animal, the player's
// inventory and the random source are only ever touched by one thread.
#pragma once

#include "ov/entity/world.hpp"
#include "ov/gameplay/animal.hpp"
#include "ov/gameplay/mob_logic.hpp"
#include "ov/math/random.hpp"
#include "ov/protocol/entity.hpp"
#include "ov/protocol/interaction.hpp"
#include "ov/protocol/play.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/level.hpp"

#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ov::server {

/// Send one packet to every client.
using HusbandryDeliver = std::function<void(i32 id, std::span<const u8> payload)>;

/// One player's hand, as a right-click on an animal sees it.
struct HusbandryHand {
    i32   entity_id{0};
    Vec3d eyes{};
    bool  creative{false};
    /// The player's 46 slots, as the protocol numbers them.
    std::span<net::ItemStack> inventory;
    /// The slot of the hand that clicked: 36..44, or 45 for the off hand.
    usize slot{36};
    /// Tell this client about one of its slots.
    std::function<void(usize slot)> send_slot;
    /// Put a stack in the inventory; returns how many were taken.
    std::function<i8(const net::ItemStack& stack)> give;
    /// The held tool took `amount` wear, and broke if it reached its maximum.
    std::function<void(i32 amount)> wear;
};

/// What the module reaches outside itself for.
struct HusbandryHost {
    /// Find a connected player and lend the hand that clicked. False when the
    /// player is gone.
    std::function<bool(i32 player, net::Hand hand, const std::function<void(HusbandryHand&)>&)>
        with_hand;
    /// Every player an animal may be tempted by. Fills a vector the module
    /// reuses: position, and the registry names of what is in each hand.
    std::function<void(std::vector<gameplay::Tempter>&)> tempters;
    /// A new mob of this type, with its behaviour, not yet announced.
    std::function<entity::EntityHandle(std::string_view type, Vec3d at)> create_mob;
    /// Tell every client about a mob created by `create_mob`.
    std::function<void(const entity::EntityState& state)> announce;
    std::function<void(Vec3d at, const net::ItemStack& stack)> drop_item;
    std::function<void(Vec3d at, i32 value)>                    spawn_orb;
};

/// What one tick did, for the log and the end-to-end check.
struct HusbandryStats {
    usize fed{0};
    usize births{0};
    usize grown{0};
    usize eggs{0};
    usize shorn{0};
    usize milked{0};
    usize grazed{0};
    usize dyed{0};
    usize saddled{0};
};

class Husbandry {
public:
    Husbandry(const registry::Registries& registries, const registry::BlockRegistry& blocks);

    // ── The network thread ──────────────────────────────────────────────────

    /// Interact (type 0) on an entity. Recorded; carried out by the tick.
    void queue_interact(i32 player, i32 entity, net::Hand hand);

    // ── The tick thread ─────────────────────────────────────────────────────

    [[nodiscard]] bool has_pending() const;

    /// Carry out the clicks, and point the entity tick at this tick's
    /// tempters and at the event sink. Before `EntityWorld::tick`.
    HusbandryStats before_entity_tick(entity::EntityWorld& world, gameplay::MobContext& context,
                                      const HusbandryHost& host, const HusbandryDeliver& deliver);

    /// Births, grown calves, eggs and grazing. After `EntityWorld::tick`.
    /// `level` is where eaten grass is written; null leaves the grass.
    HusbandryStats after_entity_tick(entity::EntityWorld& world, world::LevelWriter* level,
                                     const HusbandryHost& host, const HusbandryDeliver& deliver);

    /// The husbandry fields of a spawn's metadata: baby, fleece, saddle.
    /// Only what differs from the default, as the real server sends a spawn.
    void spawn_metadata(entity::EntityWorld& world, const entity::EntityState& state,
                        net::MetadataWriter& fields) const;

    /// Make a freshly created mob a newborn. For an egg that hatched.
    void make_baby(entity::EntityWorld& world, entity::EntityHandle handle) const;

private:
    struct Click {
        i32       player{0};
        i32       entity{0};
        net::Hand hand{net::Hand::Main};
    };

    void interact(entity::EntityWorld& world, const Click& click, const HusbandryHost& host,
                  const HusbandryDeliver& deliver, HusbandryStats& stats);
    void send_metadata(const entity::EntityState& state, const gameplay::AnimalState& animal,
                       const HusbandryDeliver& deliver) const;
    [[nodiscard]] i32              item_id(std::string_view name) const;
    [[nodiscard]] std::string_view item_name(i32 id) const;
    [[nodiscard]] std::string_view type_name(i32 type) const;

    const registry::Registries*    registries_;
    const registry::BlockRegistry* blocks_;
    std::optional<registry::RegistryId> item_registry_;
    std::optional<registry::RegistryId> entity_registry_;

    /// Wool, calves' colours and orbs, on the tick thread only. A fixed seed,
    /// as the projectiles': two runs of the same server shear the same wool.
    math::LegacyRandomSource random_{0x5EED'0000'F10C'0001LL};

    mutable std::mutex mutex_;
    std::vector<Click> pending_;
    std::vector<Click> working_;

    std::vector<gameplay::Tempter>    tempters_;
    std::vector<gameplay::AnimalEvent> events_;
};

}  // namespace ov::server
