// Bows, crossbows, tridents, snowballs, eggs, pearls and bottles, as this
// server carries them out; and the skeleton that shoots back.
//
// The rules are in ov_gameplay (projectile.hpp): how a projectile moves, what
// it hits, how hard. What is here is the part that knows about players and
// sockets: reading a draw off Use Item and Player Action, taking an arrow out
// of an inventory, spawning the entity and telling clients about it, hurting
// what it hit, and handing a stuck arrow back to whoever walks over it.
//
// Callbacks rather than a reference to the server, as for tnt_gravity.hpp:
// this file does not know what a `Player` or a chunk map is.
//
// ── Threads ─────────────────────────────────────────────────────────────────
//
// `on_use_item`, `on_release` and `cancel` run on the network thread, inside
// the packet handler, with the player's inventory in hand. They only decide:
// what was drawn, for how long, what it cost. The entity is spawned by the
// tick, from a queue behind its own lock — so the entity world and the random
// source a shot's spread is drawn from are only ever touched by one thread.
#pragma once

#include "mob_combat.hpp"

#include "ov/entity/world.hpp"
#include "ov/gameplay/collision.hpp"
#include "ov/gameplay/damage.hpp"
#include "ov/gameplay/projectile.hpp"
#include "ov/math/random.hpp"
#include "ov/protocol/play.hpp"
#include "ov/registry/registries.hpp"

#include <array>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ov::server {

/// Send one packet somewhere.
using ProjectileDeliver = std::function<void(i32 id, std::span<const u8> payload)>;

/// A connected player, as a projectile sees one.
struct ProjectilePlayer {
    i32   entity_id{0};
    Vec3d feet{};
    bool  creative{false};
    /// Dead, or waiting to respawn: nothing hits it and it picks nothing up.
    bool alive{true};
};

/// The player who pressed or released the button, on the network thread.
struct Shooter {
    i32   entity_id{0};
    Vec3d feet{};
    f32   yaw{0.0F};
    f32   pitch{0.0F};
    bool  creative{false};
    /// The player's 46 slots, as the protocol numbers them.
    std::span<net::ItemStack> inventory;
    /// The held slot, 36..44.
    usize held_slot{36};
    /// Tell this client about one of its slots.
    std::function<void(usize slot)> send_slot;
    /// To this client only.
    ProjectileDeliver send;
    /// The held item broke: every client sees it crack.
    std::function<void()> held_broke;
};

/// What the tick reaches outside the module for.
struct ProjectileHost {
    /// Every connected player. Fills a vector the module reuses.
    std::function<void(std::vector<ProjectilePlayer>&)> players;
    /// Hurt a player. True when it landed — false for a creative player or an
    /// open window, which makes an arrow bounce.
    std::function<bool(i32 player, f32 damage, gameplay::DamageKind kind)> hurt_player;
    /// Give a player a stack. Returns how many were taken.
    std::function<i8(i32 player, const net::ItemStack& stack)> give;
    /// Move a player (an ender pearl landed).
    std::function<void(i32 player, Vec3d to)> teleport;
    /// Spawn a mob of this type here (an egg hatched).
    std::function<void(std::string_view type, Vec3d at)> spawn_mob;
    /// Drop an experience orb here (a bottle broke).
    std::function<void(Vec3d at, i32 value)> spawn_orb;
    /// Put a stack on the ground (a mob an arrow killed).
    std::function<void(Vec3d at, const net::ItemStack& stack)> drop_item;
    // ── brewing ──
    /// A splash or lingering potion broke here. `target` is what it hit
    /// directly, 0 for a block.
    std::function<void(Vec3d at, const net::ItemStack& potion, i32 target, bool target_is_player)>
        potion_broke;
    /// An arrow's hit landed on `target`. `arrow` is what a pickup would give
    /// back — a tipped arrow carries its potion there.
    std::function<void(const net::ItemStack& arrow, i32 target, bool target_is_player)> arrow_hit;
    /// ── fire ── Set a mob on fire: a Flame arrow that landed. Empty when
    /// nothing on this server burns.
    std::function<void(i32 entity_id, i32 seconds)> set_on_fire;
};

/// What one tick did, for the log and the end-to-end check.
struct ProjectileStats {
    usize hits{0};
    usize stuck{0};
    usize broke{0};
    usize picked_up{0};
    usize expired{0};
};

class Projectiles {
public:
    /// `mob_combat` may be null: then a mob an arrow reaches is not hurt, and
    /// the arrow bounces off it.
    Projectiles(const registry::Registries& registries, const registry::BlockRegistry& blocks,
                MobCombat* mob_combat);

    // ── The network thread ──────────────────────────────────────────────────

    /// Use Item with something that shoots or is thrown in the main hand.
    /// Returns false when the held item is none of this module's, so the
    /// caller can hand the packet on (to eating, for instance).
    bool on_use_item(const Shooter& shooter, i64 tick);

    /// Player Action 5, "release use item". Returns true when a draw was in
    /// progress — a bow shot, a crossbow loaded, a trident thrown, or nothing
    /// because it was let go too soon.
    bool on_release(const Shooter& shooter, i64 tick);

    /// The player changed slots or left: any draw is dropped.
    void cancel(i32 player);

    // ── The tick thread ─────────────────────────────────────────────────────

    [[nodiscard]] bool has_pending() const;

    /// Spawn what the network thread asked for.
    void spawn_pending(entity::EntityWorld& world, const ProjectileDeliver& deliver);

    /// The boxes a projectile may hit this tick: every living entity of the
    /// world, and every player. Before the entity tick.
    void before_entity_tick(entity::EntityWorld& world, const ProjectileHost& host);

    /// Every skeleton with a player in sight draws and shoots. `difficulty`
    /// is 0 peaceful … 3 hard.
    void tick_skeletons(entity::EntityWorld& world, const gameplay::CollisionWorld& collisions,
                        i32 difficulty, const ProjectileDeliver& deliver);

    /// Everything the entity tick reported: hits, landings, pickups.
    ProjectileStats after_entity_tick(entity::EntityWorld& world, const ProjectileHost& host,
                                      const ProjectileDeliver& deliver);

    /// Is this entity type one this module spawns and speaks for?
    [[nodiscard]] bool owns(i32 type) const noexcept;

    /// The packets that make one of this module's entities appear.
    void spawn_packets(entity::EntityWorld& world, const entity::EntityState& state,
                       const ProjectileDeliver& deliver) const;

private:
    /// A shot the network thread decided and the tick will spawn.
    struct Shot {
        gameplay::ProjectileData data{};
        Vec3d                    start{};
        f32                      yaw{0.0F};
        f32                      pitch{0.0F};
        f32                      speed{0.0F};
        f32                      inaccuracy{0.0F};
        /// What a pickup gives back: the arrow, the trident with its NBT.
        net::ItemStack item{};
    };

    /// A draw in progress: which item, since when.
    struct Draw {
        std::string_view item;
        i64              started{0};
    };

    void spawn_shot(entity::EntityWorld& world, const Shot& shot, Vec3d velocity,
                    const ProjectileDeliver& deliver);
    void queue(Shot shot);
    void hit_mob(entity::EntityWorld& world, entity::EntityState& projectile,
                 gameplay::ProjectileData& data, const gameplay::ProjectileEvent& event,
                 const ProjectileHost& host, const ProjectileDeliver& deliver, bool& landed);
    [[nodiscard]] i32              item_id(std::string_view name) const;
    [[nodiscard]] std::string_view item_name(i32 id) const;
    [[nodiscard]] i32              type_of(gameplay::ProjectileKind kind) const noexcept;
    /// The first arrow in the inventory, in the game's order, or an empty
    /// optional. Offhand, then hotbar, then the rest.
    [[nodiscard]] std::optional<usize> find_ammo(std::span<const net::ItemStack> inventory) const;
    /// Wear the held item by one; break it at its maximum.
    void wear_held(const Shooter& shooter, std::string_view item) const;

    const registry::Registries*         registries_;
    const registry::BlockRegistry*      blocks_;
    MobCombat*                          mob_combat_;
    std::optional<registry::RegistryId> item_registry_;

    /// Wire type ids, by kind; -1 when the registry does not have one.
    std::array<i32, 8> types_{-1, -1, -1, -1, -1, -1, -1, -1};
    i32                skeleton_type_{-1};
    i32                stray_type_{-1};
    i32                blaze_type_{-1};

    gameplay::ProjectileWorld world_;
    gameplay::DamageConstants damage_constants_{};

    /// Shots and damage rolls, on the tick thread only. Fixed seeds, as the
    /// TNT's: two runs of the same server shoot the same arrows.
    math::LegacyRandomSource    random_{0x0A77'0000'5EED'0001LL};
    math::XoroshiroRandomSource loot_random_{0xA7'7055ULL, 0x5EEDULL};

    mutable std::mutex            mutex_;
    std::unordered_map<i32, Draw> draws_;
    std::vector<Shot>             pending_;
    std::vector<Shot>             spawning_;

    /// What a stuck arrow or trident gives back, by wire id.
    std::unordered_map<i32, net::ItemStack> pickup_items_;
    /// What a thrown potion holds, by wire id. ── brewing ──
    std::unordered_map<i32, net::ItemStack> potion_items_;

    /// Each skeleton's bow: ticks until the next arrow while it sees a player.
    std::unordered_map<i32, i32>  skeletons_;
    std::vector<ProjectilePlayer> players_;
    std::vector<i32>              doomed_;
};

}  // namespace ov::server
