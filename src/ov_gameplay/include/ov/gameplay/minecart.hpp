// Minecarts: the seven carts, how one rolls along a rail, and what it does
// when it leaves one.
//
// The physics is one pure step, `step_minecart`, over an `EntityState` and the
// cart's own body. It is checked tick by tick against trajectories recorded on
// a real 1.20.1 server by scripts/measure_rails.py `carts` — fourteen lanes,
// every sample labelled by a witness TNT's fuse read in the same tick (see
// docs/provenance/rails-wagonnets.md for the figures).
//
// On a rail, one tick is:
//
//   1. gravity, 0.04, as for any entity (it never shows on a rail: step 6
//      throws the vertical away);
//   2. a slope pushes 1/128 downhill;
//   3. the velocity is turned onto the rail, keeping its horizontal speed
//      (capped at 2) and its sense of travel;
//   4. an unpowered powered rail halves it, or stops it under 0.03;
//   5. the cart is put back on the rail's centre line and moved by the
//      velocity — times 0.75 with a rider — each axis capped at 0.4;
//   6. the velocity is multiplied by 0.997 with a rider, 0.96 without, and
//      its vertical is dropped;
//   7. a change of height converts to speed, 0.05 per block;
//   8. entering a new cell turns the velocity towards it;
//   9. a powered rail adds 0.06 in the direction of travel, or launches a
//      cart at rest away from a conductor at either end with 0.02.
//
// Off a rail: each axis capped at 0.4, halved on the ground, moved, and
// multiplied by 0.95 while in the air.
//
// Layer 9: the step reads a `LevelView` and a `CollisionWorld`; riding,
// pushing other entities, and the blocks a cart touches are the caller's.
#pragma once

#include "ov/base/types.hpp"
#include "ov/entity/logic.hpp"
#include "ov/entity/world.hpp"
#include "ov/gameplay/collision.hpp"
#include "ov/gameplay/primed_tnt.hpp"
#include "ov/gameplay/rails.hpp"
#include "ov/math/random.hpp"
#include "ov/math/vec.hpp"
#include "ov/world/level.hpp"

#include <array>
#include <optional>
#include <string_view>
#include <vector>

namespace ov::gameplay {

/// The seven carts.
enum class MinecartKind : u8 {
    Rideable,
    Chest,
    Furnace,
    Tnt,
    Hopper,
    Spawner,
    CommandBlock,
};

inline constexpr std::array<std::string_view, 7> kMinecartTypes{
    "minecraft:minecart",         "minecraft:chest_minecart",  "minecraft:furnace_minecart",
    "minecraft:tnt_minecart",     "minecraft:hopper_minecart", "minecraft:spawner_minecart",
    "minecraft:command_block_minecart",
};

[[nodiscard]] std::optional<MinecartKind> minecart_kind(std::string_view type_name) noexcept;
[[nodiscard]] constexpr std::string_view  minecart_type(MinecartKind kind) noexcept {
    return kMinecartTypes[static_cast<usize>(kind)];
}

/// The item a player places a cart with. A spawner cart and a command block
/// cart have none in survival.
[[nodiscard]] std::optional<MinecartKind> minecart_for_item(std::string_view item) noexcept;
[[nodiscard]] std::string_view            minecart_item(MinecartKind kind) noexcept;

/// The block a cart carries, for rendering and for its drop: chest, furnace,
/// tnt, hopper, spawner, command block. Empty for a plain cart.
[[nodiscard]] std::string_view minecart_block(MinecartKind kind) noexcept;

/// How many slots a container cart holds: 27, 5, or 0.
[[nodiscard]] constexpr usize minecart_slots(MinecartKind kind) noexcept {
    return kind == MinecartKind::Chest ? 27U : (kind == MinecartKind::Hopper ? 5U : 0U);
}

// ── Constants ───────────────────────────────────────────────────────────────

inline constexpr f64 kMinecartGravity      = 0.04;
inline constexpr f64 kMinecartSlope        = 0.0078125;
inline constexpr f64 kMinecartMaxSpeed     = 0.4;
inline constexpr f64 kMinecartRiderFactor  = 0.75;
inline constexpr f64 kMinecartDragEmpty    = 0.96;
inline constexpr f64 kMinecartDragRidden   = 0.997;
inline constexpr f64 kMinecartBoost        = 0.06;
inline constexpr f64 kMinecartLaunch       = 0.02;
inline constexpr f64 kMinecartBrakeStop    = 0.03;
inline constexpr f64 kMinecartAirDrag      = 0.95;
inline constexpr f64 kMinecartHeightToSpeed = 0.05;
/// A cart sits this far above the bottom of its rail's cell.
inline constexpr f64 kMinecartRailHeight   = 0.0625;

/// A furnace cart burns one unit a tick; one coal is 3600 (the wiki).
inline constexpr i32 kFurnaceFuelPerCoal = 3600;
/// A furnace cart's speed cap on a rail: half a plain cart's (the wiki).
inline constexpr f64 kFurnaceMaxSpeed    = 0.2;
/// A TNT cart primed by an activator rail explodes this many ticks later.
inline constexpr i32 kTntMinecartFuse    = 80;

/// A cart breaks once its accumulated damage passes this. Ten per point of a
/// hit, decaying one a tick (the wiki's Minecart article).
inline constexpr f32 kMinecartBreakDamage = 40.0F;

/// Everything a cart carries beyond `EntityState`.
struct MinecartBody {
    MinecartKind kind{MinecartKind::Rideable};

    /// Did the yaw flip half a turn to follow the travel? Vanilla keeps a cart
    /// facing the way it rolls rather than spinning at every reversal.
    bool flipped{false};

    /// A rider is on it. Changes the drag and the step size.
    bool ridden{false};
    /// The rider's walking impulse this tick, horizontal, blocks per tick.
    /// Zero when the rider is not a player or pushes nothing.
    Vec3d rider_impulse{};

    /// Furnace: ticks of fuel left, and the push direction.
    i32 fuel{0};
    f64 push_x{0.0};
    f64 push_z{0.0};

    /// TNT: ticks to go once primed, -1 when not.
    i32 fuse{-1};

    /// Hopper: collects while true; a powered activator rail turns it off.
    bool enabled{true};

    /// Shaking, as the client draws it: time, sense, and the damage it keeps.
    i32 hurt_time{0};
    i32 hurt_dir{1};
    f32 damage{0.0F};

    /// The block a cart displays in place of its own, as a state id, and its
    /// height in sixteenths. `custom_display` false means "the kind's own".
    bool custom_display{false};
    i32  display_state{0};
    i32  display_offset{6};
};

/// What one tick found under the cart, for the caller to act on.
struct MinecartContact {
    bool      on_rail{false};
    BlockPos  rail{};
    RailKind  kind{RailKind::None};
    bool      powered{false};
};

/// Advance a cart by one tick. Returns what it rolled over.
MinecartContact step_minecart(entity::EntityState& state, MinecartBody& body, const Rails& rails,
                              const world::LevelView& level, const CollisionWorld& collisions);

/// Where on the rail at the position's cell (or the one under it) a cart would
/// sit, or nothing when there is no rail there.
[[nodiscard]] std::optional<Vec3d> rail_point(const Rails& rails, const world::LevelView& level,
                                              Vec3d position);

/// The explosion a TNT cart makes: 4, plus up to 1.5 × its speed (at most 5)
/// drawn at random. The wiki's TNT minecart article; not measured here.
[[nodiscard]] f32 tnt_minecart_power(f64 horizontal_speed_sqr, math::LegacyRandomSource& rng);

/// Where entity logic reports what the caller must finish: the rails a cart
/// rolled over (detector and activator rails), and TNT carts going off.
struct MinecartEvents {
    struct Contact {
        i32             network_id{0};
        MinecartContact contact{};
    };
    std::vector<Contact> contacts;
    /// TNT carts whose fuse ran out, as charges for primed_tnt's drain.
    BlastEvents* blasts{nullptr};
};

/// The logic of any of the seven carts.
class MinecartLogic final : public entity::IEntityLogic {
public:
    MinecartLogic(const Rails& rails, MinecartEvents& events, MinecartBody body) noexcept
        : rails_{&rails}, events_{&events}, body_{body} {}

    void tick(entity::EntityWorld& world, entity::EntityHandle self,
              const entity::TickContext& context) override;

    [[nodiscard]] std::string_view name() const noexcept override { return "minecart"; }

    [[nodiscard]] MinecartBody&       body() noexcept { return body_; }
    [[nodiscard]] const MinecartBody& body() const noexcept { return body_; }

private:
    const Rails*    rails_;
    MinecartEvents* events_;
    MinecartBody    body_;
};

}  // namespace ov::gameplay
