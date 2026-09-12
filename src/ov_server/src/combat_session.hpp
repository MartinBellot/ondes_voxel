// One player's melee and interaction state, and the packets it owes.
//
// The same shape as survival_session.{hpp,cpp} and for the same reason: so that
// server.cpp does not grow another system inside its packet switch. Everything
// about the four verbs — hitting, using, eating and wearing a tool out — lives
// here, and server.cpp would touch it in four short places.
//
// **Nothing calls this yet.** It is written, compiled and tested on its own; the
// call sites it needs are listed at the bottom of this header so that whoever
// wires it in has the list rather than the search.
//
// It is not in ov_gameplay because it sends packets, and layer 9 must never
// learn what a socket is. The rules are all up there; what is here is the
// wiring, plus the state that has to survive between two packets.
#pragma once

#include "ov/base/types.hpp"
#include "ov/gameplay/combat.hpp"
#include "ov/gameplay/durability.hpp"
#include "ov/gameplay/item_use.hpp"
#include "ov/math/random.hpp"
#include "ov/math/vec.hpp"
#include "ov/protocol/interaction.hpp"
#include "ov/world/level.hpp"

#include <functional>
#include <optional>
#include <span>
#include <string_view>

namespace ov::server {

/// How this session reaches the world and the wire.
///
/// Four sinks and three questions, all injected. A session that reached for a
/// `ServerLevel` would be untestable and would drag layer 12 into every test
/// that wanted to swing a sword.
struct CombatIo {
    /// To this player's own client.
    std::function<void(i32 packet_id, std::span<const u8> payload)> send;

    /// To every client that can see this player, including this one — the
    /// attacker has to see their own swing.
    std::function<void(i32 packet_id, std::span<const u8> payload)> broadcast;

    /// ── breaking ── To every client that can see this player *except* its
    /// own. A Swing Arm reaches the others only: on a real 1.20.1 server the
    /// swinger received no Entity Animation (docs/provenance/cassage-bloc.md).
    /// Unset, on_swing falls back to `broadcast`.
    std::function<void(i32 packet_id, std::span<const u8> payload)> broadcast_others;

    /// Hurt an entity. Returns false when there is no such entity, which is not
    /// an error: a client can name one that died on the tick it swung.
    std::function<bool(i32 entity_id, f32 damage, bool critical)> hurt_entity;

    /// Where an entity is, for the knockback direction. Nothing when it is gone.
    std::function<std::optional<Vec3d>(i32 entity_id)> entity_position;

    /// Whether an entity is standing on the ground, and its knockback
    /// resistance. The vertical term of a knockback depends on the first and
    /// the whole of it on the second.
    std::function<bool(i32 entity_id)> entity_on_ground;
    std::function<f32(i32 entity_id)>  entity_knockback_resistance;

    /// Push an entity. The session computes the velocity; only the caller can
    /// store it.
    std::function<void(i32 entity_id, Vec3d velocity)> set_entity_velocity;

    /// Every other living entity inside the sweep box around the attacker,
    /// excluding the one that was hit.
    std::function<void(Vec3d centre, f64 radius, i32 exclude,
                       const std::function<void(i32)>& visit)>
        entities_near;

    /// Charge the attacker's hunger. Hunger lives in SurvivalSession, which
    /// this one must not depend on, so it is a call rather than a reference.
    std::function<void(f32 exhaustion)> exhaust;

    /// The item in the player's main hand, by registry name, and its `Damage`.
    /// Empty name for an empty hand.
    std::function<std::string_view()> held_item;
    std::function<i32()>              held_damage;

    /// Write the held item's damage back, or destroy the stack.
    std::function<void(i32 damage)> set_held_damage;
    std::function<void()>           break_held_item;

    /// Take one from the held stack — bone meal, a bucket emptied, a cake.
    std::function<void()> consume_one_held;

    /// The enchantment levels on the held item. Read once per swing rather than
    /// stored, because the player can change hands between two of them.
    std::function<gameplay::Weapon()> held_weapon;

    /// ── enchanting ── Smite, Bane and Impaling against one target.
    std::function<f32(i32 entity_id)> target_bonus;

    /// ── fire ── Set an entity on fire for this many seconds: Fire Aspect.
    /// Empty when nothing on this server burns.
    std::function<void(i32 entity_id, i32 seconds)> set_on_fire;
};

/// What the server must know about the player doing the hitting.
struct CombatPlayer {
    i32  entity_id{0};
    f64  x{0.0};
    f64  y{0.0};
    f64  z{0.0};
    f32  yaw{0.0F};
    bool on_ground{true};
    bool sprinting{false};
    bool sneaking{false};

    /// Fall distance, in blocks, from the survival session that owns it.
    f32 fall_distance{0.0F};

    /// The eyes are in water, the player is on a ladder, blind, or riding.
    /// All four disqualify a critical and the server is the only thing that
    /// knows any of them.
    bool in_water{false};
    bool on_climbable{false};
    bool blind{false};
    bool riding{false};

    /// 0 survival, 1 creative, 2 adventure, 3 spectator. A creative player's
    /// tools do not wear out, which is a rule rather than an optimisation.
    u8 game_mode{0};

    /// The player's hunger, for `begin_use` — a full player cannot eat bread.
    i32 food{20};
    i32 max_food{20};

    /// ── effects ── Strength and Weakness amplifiers, -1 for none. Filled
    /// from the effect session; `resolve_attack` already knew what to do with
    /// them and nothing had ever set them.
    i8 strength{-1};
    i8 weakness{-1};
};

/// What one swing or one use asked the server to do beyond the packets.
/// ── pvp ── Hurt Animation's angle: where a blow came from, relative to where
/// the victim faces, in degrees. `dx`, `dz` run from the victim to the
/// attacker. The degrees are the float-rounded conversion, 57.2957763671875
/// per radian: the real server sent 179.99998 (0x4333FFFF), not 180, for the
/// capture's blow from straight along -x on a victim facing 0.
[[nodiscard]] f32 hurt_direction(f64 dx, f64 dz, f32 victim_yaw) noexcept;

struct CombatOutcome {
    /// How the interaction ended, for the caller that has a chain to continue.
    ///
    /// `hit` is not enough for that: Pass, Fail and Consume are all "did not
    /// hit" and only the first of them may fall through to the placement path.
    /// A caller that treated a Fail as a Pass would place a block *through* a
    /// locked iron door, which is the exact case the four-valued result exists
    /// for. Always Pass for the packets that are not Use Item On.
    gameplay::UseResult result{gameplay::UseResult::Pass};

    bool             hit{false};
    bool             critical{false};
    bool             swept{false};
    f32              damage{0.0F};
    /// ── sound ── how charged the swing was (0..1), and whether it was a
    /// sprinting knockback: the two things the attacker's sound depends on
    /// that the three fields above do not say.
    f32  strength_scale{0.0F};
    bool sprint_knockback{false};
    /// A primed TNT the caller must spawn.
    bool     spawn_primed_tnt{false};
    BlockPos tnt_position{};
    /// A screen the caller must open, and where.
    gameplay::ScreenKind screen{gameplay::ScreenKind::None};
    BlockPos             screen_position{};
    /// Something recognised and not carried out, named for the log.
    std::string_view unsupported{};
};

/// Everything one player carries between two packets.
class CombatSession {
public:
    gameplay::CombatConstants constants{};

    /// The gauge. Ticked once per tick, reset by every attack.
    gameplay::AttackerState attacker{};

    /// An eat or a drink in progress.
    gameplay::UseInProgress use{};

    /// Distance moved on the last tick, in blocks. The sweep needs a standing
    /// attacker and the server is the only thing that can measure that.
    f64 last_step{0.0};

    /// One tick. Advances the gauge and any use in progress.
    ///
    /// Returns the item whose use finished on this tick, or empty. Applying its
    /// effect — nutrition, a potion, milk clearing effects — is the caller's,
    /// because it reaches into three systems this one must not depend on.
    [[nodiscard]] std::string_view tick(const CombatPlayer& player, f64 step);

    /// Serverbound Interact. Attacks and right-clicks on an entity both arrive
    /// here.
    [[nodiscard]] CombatOutcome on_interact(const net::Interact& packet,
                                            const CombatPlayer& player, const CombatIo& io);

    /// Serverbound Use Item On: the block first, then the item.
    [[nodiscard]] CombatOutcome on_use_item_on(const net::UseItemOn& packet,
                                               const CombatPlayer& player, const CombatIo& io,
                                               world::LevelWriter& level,
                                               const gameplay::ItemUse& rules);

    /// Serverbound Use Item: the hand acts on nothing in particular. Eating
    /// starts here.
    [[nodiscard]] CombatOutcome on_use_item(const net::UseItem& packet,
                                            const CombatPlayer& player, const CombatIo& io);

    /// The player let go of the button, or switched slots. Abandons any use.
    void on_release(const CombatIo& io);

    /// Serverbound Swing Arm. Pure animation, and the whole handler: a server
    /// that took this for an attack would let a client hit fifty times a
    /// second.
    void on_swing(net::Hand hand, const CombatPlayer& player, const CombatIo& io);

private:
    /// Wear the held item by one action, and destroy it if that finishes it.
    void wear_held(const CombatIo& io, const CombatPlayer& player, gameplay::ToolAction action,
                   i32 times);

    /// Everything a landed hit owes: knockback, sweep, particles, exhaustion.
    void deliver(const gameplay::AttackOutcome& outcome, i32 target, const CombatPlayer& player,
                 const CombatIo& io);

    /// The RNG the durability draws come from. Explicit and per-session, so two
    /// players wearing tools out do not share a stream — CLAUDE.md principle 5.
    math::XoroshiroRandomSource random_{0x00C0'FFEE'0000'0001LL};
};

// ── Wiring, for whoever connects this ───────────────────────────────────────
//
// Nothing in ov_server calls any of the above yet. Five lines in server.cpp
// would, and they are listed here rather than left to be rediscovered:
//
//   1. `#include "combat_session.hpp"` beside the survival_session include.
//   2. A `CombatSession combat;` member on the player record, beside
//      `SurvivalSession survival;`.
//   3. Three cases in the serverbound switch:
//        net::serverbound::kInteract     -> parse_interact     -> on_interact
//        net::serverbound::kUseItem      -> parse_use_item     -> on_use_item
//        net::serverbound::kSwingArm     -> parse_swing_arm    -> on_swing
//      and, inside the existing kUseItemOn case, `on_use_item_on` **before**
//      the block-placement path, taking its result when it is not Pass.
//   4. One line in the per-player block of the tick:
//        `const auto finished = who.combat.tick(view, step);`
//      followed by applying `finished` through the food system.
//   5. `combat.on_release(io)` wherever Set Held Item and Player Action's
//      "release use item" (status 5) are already handled.
//
// The CombatIo sinks map onto things server.cpp already has: `hurt_entity` onto
// whatever the mob damage path is, `entities_near` onto the entity world's
// spatial query, `held_item` onto the player's inventory.

}  // namespace ov::server
