// Rails and minecarts, as this server carries them out.
//
// The rules are in ov_gameplay (rails.hpp, minecart.hpp) and know nothing about
// players, sockets or files. What is here is the part that does: the rail
// engine the world-tick drain calls, a placed rail shaped among its
// neighbours, carts spawned from an item, their packets, riding, breaking,
// the detector rail a cart presses, the activator rail that throws a rider
// off, and a cart's compound in `entities/r.x.z.mca`.
//
// The files themselves are not this session's: `EntityStorage` is their one
// reader and writer (entity_storage.hpp). The session is an `EntityAdopter`:
// the storage hands it each cart it reads from a chunk, and asks it for each
// live cart's compound when it writes one.
//
// Callbacks rather than a reference to the server, as for tnt_gravity.hpp:
// this file must not know what a `Player` or a chunk map is.
//
// ── Threads ─────────────────────────────────────────────────────────────────
//
// The `request_*` calls come from the network thread and only append to a list
// behind their own lock. Everything else runs on the tick thread, which is the
// only one that touches the entity world and writes the level.
#pragma once

#include "entity_storage.hpp"
#include "world_ticks.hpp"

#include "ov/entity/world.hpp"
#include "ov/gameplay/minecart.hpp"
#include "ov/gameplay/primed_tnt.hpp"
#include "ov/gameplay/rails.hpp"
#include "ov/gameplay/signal.hpp"
#include "ov/math/vec.hpp"
#include "ov/nbt/tag.hpp"
#include "ov/registry/registries.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ov::server {

/// Send one packet somewhere.
using RailsDeliver = std::function<void(i32 id, std::span<const u8> payload)>;

/// What the session reaches outside itself for.
struct RailsHost {
    /// Every connected player gets this.
    RailsDeliver broadcast;
    /// Put an item on the ground, by registry name.
    std::function<void(Vec3d, std::string_view item, i32 count)> drop_item;
    /// A rider rides: the server's idea of where that player stands follows
    /// the cart, so the chunks around them stay loaded. No packet. False when
    /// that player is gone, and the cart is then free again.
    std::function<bool(i32 player_id, Vec3d seat)> carry_rider;
    /// A rider got off: put the player down there, with a teleport.
    std::function<void(i32 player_id, Vec3d at)> set_down;
    /// Take one of the held item (a furnace cart fed coal). Survival only.
    std::function<void(i32 player_id)> consume_held;
};

/// A right-click or a hit on a cart, asked for by the network thread.
struct CartTouch {
    i32  player_id{0};
    i32  cart_id{0};
    bool attack{false};
    bool creative{false};
    bool sneaking{false};
    /// The player's facing, for a furnace cart's push.
    Vec3d player_feet{};
    /// What the player holds, by registry name.
    std::string held;
};

/// A rider's controls, from Player Input.
struct RiderInput {
    i32 player_id{0};
    f32 sideways{0.0F};
    f32 forward{0.0F};
    u8  flags{0};
    f32 yaw{0.0F};
};

/// What one tick did, for the log and for the report.
struct RailsStats {
    usize carts{0};
    usize placed{0};
    usize broken{0};
    usize detector_changes{0};
    usize shaped{0};
};

class RailsSession final : public BlockRuleExtension, public EntityAdopter {
public:
    RailsSession(const registry::BlockRegistry& blocks, const registry::Registries& registries);

    [[nodiscard]] const gameplay::Rails& rails() const noexcept { return rails_; }

    /// The queue a TNT cart's charge joins (tnt_gravity's own). Not owned.
    void set_blasts(gameplay::BlastEvents* blasts) noexcept { events_.blasts = blasts; }

    // ── BlockRuleExtension: called by the world-tick drain ──────────────────

    void neighbour_changed(ServerLevel& level, BlockPos pos) override;
    bool scheduled_tick(ServerLevel& level, BlockPos pos, std::string_view what) override;

    // ── Network thread ──────────────────────────────────────────────────────

    /// A player placed a rail at `pos`: shape it on the next tick.
    void request_shape(BlockPos pos, bool facing_east_west);

    /// A player used a cart item on the rail at `rail`. False when the block
    /// is not a rail — the item then does nothing, as vanilla's.
    [[nodiscard]] bool request_cart(gameplay::MinecartKind kind, BlockPos rail,
                                    registry::BlockStateId rail_state, f32 yaw);

    void request_touch(CartTouch touch);
    void request_input(RiderInput input);

    /// Is this entity type one of the seven carts? (Any thread: the table is
    /// set once, in the constructor.)
    [[nodiscard]] bool owns(i32 type) const noexcept override;

    /// Is anything waiting for the tick?
    [[nodiscard]] bool has_pending() const;

    /// The cart a player rides, or -1.
    [[nodiscard]] i32 vehicle_of(i32 player_id) const;

    // ── Tick thread ─────────────────────────────────────────────────────────

    /// Give a cart spawned elsewhere — `/summon`, a load — its behaviour.
    void adopt(entity::EntityWorld& world, entity::EntityHandle handle,
               const nbt::Tag* saved = nullptr);

    /// The packets that make a cart appear, for a player joining later.
    void spawn_packets(entity::EntityWorld& world, const entity::EntityState& state,
                       const RailsDeliver& deliver) const;

    /// Shapes, placements, touches and inputs; riders' controls onto carts.
    void before_entity_tick(entity::EntityWorld& world, ServerLevel& level, WorldTicks& ticks,
                            const RailsHost& host);

    /// Detector and activator rails, riders carried, fuel metadata.
    RailsStats after_entity_tick(entity::EntityWorld& world, ServerLevel& level, WorldTicks& ticks,
                                 const RailsHost& host);

    /// A comparator behind a detector rail: the container cart on it, or 0.
    /// -1 when `pos` is not a detector rail.
    [[nodiscard]] i32 comparator_signal(const world::LevelView& level, BlockPos pos) const;

    /// A player left: they get off whatever they were riding.
    void forget_player(entity::EntityWorld& world, i32 player_id, const RailsHost& host);

    // ── Persistence: EntityAdopter, called by EntityStorage ─────────────────

    /// A cart read from a chunk: spawned where it was, with everything it was
    /// saved with kept for its next save.
    std::optional<entity::EntityHandle> adopt_saved(entity::EntityWorld& world,
                                                    const nbt::Tag&      compound) override;

    /// A live cart's compound (`cart_nbt`). Nullopt for one being removed.
    [[nodiscard]] std::optional<nbt::Tag> save_entity(entity::EntityWorld& world,
                                                      entity::EntityHandle handle) const override;

    /// A cart leaves the world with its chunk: forgotten, and its rider with it.
    void release(entity::EntityWorld& world, entity::EntityHandle handle) override;

    /// The NBT vanilla writes for one cart: id, Pos, Motion, Rotation, UUID,
    /// and each kind's own — Fuel and PushX/Z, TNTFuse, Enabled, Items,
    /// CustomDisplayTile/DisplayState/DisplayOffset.
    [[nodiscard]] nbt::Tag cart_nbt(const entity::EntityState& state,
                                    const gameplay::MinecartBody& body) const;

    /// A rider's walking push, per unit of input. Fitted on one capture (a
    /// player facing east, forward for two seconds, read 0.0657 blocks per
    /// tick); the rule behind it is not understood. See the provenance note.
    static constexpr f64 kRiderPush = 0.0274;

    /// A detector rail rechecks this often while pressed. The measured
    /// switch-off came 8 ticks after the cart went, inside one period.
    static constexpr i64 kDetectorPeriod = 20;

private:
    struct Cart {
        entity::EntityHandle   handle{entity::kNoEntity};
        gameplay::MinecartKind kind{gameplay::MinecartKind::Rideable};
        i32                    rider{-1};
        /// Everything read from the save, for the keys this server does not
        /// model — a spawner's SpawnData, a command block's Command — and the
        /// Items of a container cart.
        nbt::Tag saved;
        bool     fuel_sent{false};
    };

    [[nodiscard]] gameplay::MinecartLogic* logic_of(entity::EntityWorld& world,
                                                   entity::EntityHandle handle) const;
    void spawn_cart(entity::EntityWorld& world, gameplay::MinecartKind kind, Vec3d at, f32 yaw,
                    const RailsHost& host);
    void touch(entity::EntityWorld& world, const CartTouch& touch, const RailsHost& host);
    void mount(entity::EntityWorld& world, Cart& cart, i32 cart_id, i32 player_id,
               const RailsHost& host);
    void dismount(entity::EntityWorld& world, Cart& cart, i32 cart_id, const RailsHost& host);
    void destroy(entity::EntityWorld& world, Cart& cart, entity::EntityState& state,
                 gameplay::MinecartLogic& logic, bool drops, const RailsHost& host);
    void send_shake(const entity::EntityState& state, const gameplay::MinecartBody& body,
                    const RailsHost& host) const;
    [[nodiscard]] i32 container_reading(const Cart& cart, gameplay::MinecartKind kind) const;

    const registry::BlockRegistry* blocks_;
    const registry::Registries*    registries_;
    gameplay::Signals              signals_;
    gameplay::Rails                rails_;
    gameplay::MinecartEvents       events_;

    /// Rails that lost their support during the drain, dropped once the
    /// entity pass holds the players: an item on the ground is published to
    /// every player, and the drain does not hold them.
    struct RailDrop {
        BlockPos         pos{};
        std::string_view item;
    };
    std::vector<RailDrop> rail_drops_;

    std::array<i32, 7> types_{};  // protocol id per MinecartKind
    std::optional<registry::RegistryId> entity_types_;

    std::unordered_map<i32, Cart> carts_;          // by wire id
    std::unordered_map<i32, i32>  vehicle_of_;     // player → cart
    /// Detector rails pressed this tick, and the cart on each.
    std::map<std::tuple<i32, i32, i32>, i32> pressed_;

    mutable std::mutex pending_mutex_;
    struct PendingShape {
        BlockPos pos{};
        bool     east_west{false};
    };
    struct PendingCart {
        gameplay::MinecartKind kind{gameplay::MinecartKind::Rideable};
        Vec3d                  at{};
        f32                    yaw{0.0F};
    };
    std::vector<PendingShape> pending_shapes_;
    std::vector<PendingCart>  pending_carts_;
    std::vector<CartTouch>    pending_touches_;
    std::vector<RiderInput>   pending_inputs_;
    /// Swapped out under the lock, so the work happens outside it.
    std::vector<PendingShape> shapes_now_;
    std::vector<PendingCart>  carts_now_;
    std::vector<CartTouch>    touches_now_;
    std::vector<RiderInput>   inputs_now_;
    /// Each rider's latest controls.
    std::unordered_map<i32, RiderInput> controls_;

    i32 next_uuid_salt_{0};
};

}  // namespace ov::server
