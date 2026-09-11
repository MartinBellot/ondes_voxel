// ── tame ── Taming, owners and riding, as this server carries them out: a
// right-click with a bone, a saddle, an empty hand on a horse; a horse steered
// by its rider's client; what clients are told; what goes into entities/.
//
// The rules are in ov_gameplay (tame.hpp): what tames, at what odds, how a
// foal inherits, when a wolf follows and whom it attacks. What is here is the
// part that knows about inventories, UUIDs, sockets and NBT.
//
// Callbacks rather than a reference to the server, as for husbandry.hpp: this
// file does not know what a `Player` or a chunk map is.
//
// ── Threads ─────────────────────────────────────────────────────────────────
//
// The `queue_*` calls run on the network thread and only record. Everything
// else runs on the tick thread, inside the entity block, with the players'
// lock held — an animal, a player's inventory and the random source are only
// ever touched by one thread.
#pragma once

#include "husbandry.hpp"

#include "ov/entity/world.hpp"
#include "ov/gameplay/mob_attack.hpp"
#include "ov/gameplay/mob_logic.hpp"
#include "ov/gameplay/tame_state.hpp"
#include "ov/math/random.hpp"
#include "ov/nbt/tag.hpp"
#include "ov/protocol/entity.hpp"
#include "ov/protocol/play.hpp"
#include "ov/registry/registries.hpp"

#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ov::server {

using TamingDeliver = std::function<void(i32 id, std::span<const u8> payload)>;

/// A player as the taming module needs one.
struct TamingPlayer {
    i32       network_id{0};
    net::Uuid uuid{};
    Vec3d     feet{};
    bool      on_ground{true};
};

/// What the module reaches outside itself for.
struct TamingHost {
    /// Lend the hand that clicked (husbandry's own lending). False when the
    /// player is gone.
    std::function<bool(i32 player, net::Hand hand, const std::function<void(HusbandryHand&)>&)>
        with_hand;
    /// Every connected, living player in the overworld.
    std::function<void(std::vector<TamingPlayer>&)> players;
    /// A rider rides: the server's idea of where the player is follows the
    /// animal (no packet). False when the player is gone.
    std::function<bool(i32 player, Vec3d seat)> carry_rider;
    /// A rider got off: put the player down there, with a teleport.
    std::function<void(i32 player, Vec3d at)> set_down;
};

/// What one tick did, for the log and the end-to-end check.
struct TamingStats {
    usize tamed{0};
    usize refused{0};
    usize mounted{0};
    usize thrown{0};
    usize teleported{0};
    usize steered{0};
};

class Taming {
public:
    explicit Taming(const registry::Registries& registries);

    // ── The network thread ──────────────────────────────────────────────────

    /// Interact (type 0) on an entity.
    void queue_interact(i32 player, i32 entity, net::Hand hand, bool sneaking);
    /// Move Vehicle (serverbound 0x18): where the rider's client put its mount.
    void queue_vehicle_move(i32 player, Vec3d at, f32 yaw, f32 pitch);
    /// Player Input (0x1F): flag 0x02 is "get off".
    void queue_input(i32 player, u8 flags);

    // ── The tick thread ─────────────────────────────────────────────────────

    /// A player's hit landed on a mob: a wild wolf's pack turns on them, a
    /// tame animal stands up, and the player's own wolves join in.
    void on_player_hit(entity::EntityWorld& world, i32 player, i32 target, i64 tick);

    /// Clicks, dismounts, the owners; points the entity tick at them.
    TamingStats before_entity_tick(entity::EntityWorld& world, gameplay::MobContext& context,
                                   const TamingHost& host, const TamingDeliver& deliver, i64 tick);

    /// Decisions (tamed, thrown, teleported), what hurt an owner, the riders'
    /// steering, the riders carried, and every changed metadata. After the
    /// entity tick and before the swings are resolved.
    TamingStats after_entity_tick(entity::EntityWorld& world,
                                  std::span<const gameplay::MobAttack> attacks,
                                  const TamingHost& host, const TamingDeliver& deliver, i64 tick);

    /// The tame fields of a spawn's metadata, only what differs from the
    /// default, as a real server sends a spawn.
    void spawn_metadata(entity::EntityWorld& world, const entity::EntityState& state,
                        net::MetadataWriter& fields) const;
    /// A horse's own drawn attributes over its type's base values.
    void attributes(entity::EntityWorld& world, const entity::EntityState& state,
                    std::vector<net::AttributeValue>& values) const;
    /// After a spawn: the armour a horse wears, and who rides it.
    void spawn_extra(entity::EntityWorld& world, const entity::EntityState& state,
                     const TamingDeliver& deliver) const;

    /// A player left: they get off.
    void forget_player(entity::EntityWorld& world, i32 player, const TamingHost& host,
                       const TamingDeliver& deliver);

    /// A birth husbandry has just finished: a pup has its parent's owner, a
    /// foal its parents' stats.
    void on_birth(entity::EntityWorld& world, entity::EntityHandle mother,
                  entity::EntityHandle father, entity::EntityHandle child);

    // ── entities/ ── the storage's `write_extra` and `read_extra`
    void write_nbt(entity::EntityWorld& world, const entity::EntityState& state,
                   nbt::Tag& out) const;
    void read_nbt(entity::EntityWorld& world, entity::EntityState& state,
                  const nbt::Tag& compound) const;

    /// The animal a player rides, by wire id, or 0.
    [[nodiscard]] i32 vehicle_of(i32 player) const;

    /// The eleven cat variants, in `minecraft:cat_variant` order (the wire's).
    [[nodiscard]] static std::span<const std::string_view> cat_variants() noexcept;

private:
    struct Click {
        i32       player{0};
        i32       entity{0};
        net::Hand hand{net::Hand::Main};
        bool      sneaking{false};
    };
    struct Move {
        Vec3d at{};
        f32   yaw{0.0F};
        f32   pitch{0.0F};
    };
    struct Track {
        entity::EntityHandle hurt_by{entity::kNoEntity};
        i64                  hurt_by_tick{-1};
        entity::EntityHandle attacked{entity::kNoEntity};
        i64                  attacked_tick{-1};
    };

    void interact(entity::EntityWorld& world, const Click& click, const TamingHost& host,
                  const TamingDeliver& deliver, TamingStats& stats);
    void mount(entity::EntityWorld& world, entity::EntityHandle handle, i32 player,
               const TamingDeliver& deliver);
    /// `why` goes to the debug log: got off, gone, thrown, left, carry.
    void dismount(entity::EntityWorld& world, i32 player, std::string_view why,
                  const TamingHost& host, const TamingDeliver& deliver);
    void send_metadata(const entity::EntityState& state, const gameplay::MobBrain& brain,
                       const TamingDeliver& deliver) const;
    void send_equipment(const entity::EntityState& state, const gameplay::TameState& tame,
                        const TamingDeliver& deliver) const;
    [[nodiscard]] std::string_view type_name(i32 type) const;
    [[nodiscard]] i32              item_id(std::string_view name) const;
    [[nodiscard]] std::string_view item_name(i32 id) const;
    [[nodiscard]] i32              type_id(std::string_view name) const;

    const registry::Registries*         registries_;
    std::optional<registry::RegistryId> items_;
    std::optional<registry::RegistryId> types_;
    std::optional<registry::RegistryId> attributes_;

    /// Taming tries, tantrums, anger: on the tick thread only, fixed seed as
    /// husbandry's, so two runs of the same server tame the same wolves.
    math::LegacyRandomSource random_{0x5EED'0000'7A3E'0001LL};

    mutable std::mutex         mutex_;
    std::vector<Click>         pending_;
    std::vector<Click>         working_;
    std::unordered_map<i32, Move> moves_;
    std::vector<i32>           getting_off_;
    std::vector<i32>           off_now_;

    gameplay::TameWorld              world_;
    std::vector<gameplay::Owner>     owners_;
    std::vector<gameplay::TameEvent> events_;
    std::vector<TamingPlayer>        players_;
    std::unordered_map<i32, Track>   tracks_;
    /// Rider → animal, by wire ids.
    std::unordered_map<i32, i32> riding_;
    /// The tick being run, for the log.
    i64 now_{0};
    /// The goals a ridden animal is running, for the debug log (reused: no
    /// allocation once it has grown).
    std::vector<std::string_view> running_;
    std::string                   running_names_;
};

}  // namespace ov::server
