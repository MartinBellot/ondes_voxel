#include "combat_session.hpp"

#include "ov/protocol/entity.hpp"
#include "ov/protocol/play.hpp"

namespace ov::server {
namespace {

/// Entity Animation codes this session sends.
///
/// 0 is the arm swing every client draws for itself and needs to be told about
/// for everybody else; 4 is the critical-hit particle burst and 5 the magic
/// one. They are one byte apart and mean entirely different things.
constexpr u8 kAnimationSwingMain = 0;
constexpr u8 kAnimationSwingOff  = 3;
constexpr u8 kAnimationCritical  = 4;

/// The sweep box: the attacker's own bounding box grown by one block in x and
/// z. Measured: two sheep three blocks either side of the bot never swept, two
/// at nine tenths of a block always did.
constexpr f64 kSweepRadius = 1.0;

}  // namespace

std::string_view CombatSession::tick(const CombatPlayer& player, f64 step) {
    gameplay::tick_attack_strength(attacker);
    attacker.sprinting                  = player.sprinting;
    attacker.on_ground                  = player.on_ground;
    attacker.fall_distance              = player.fall_distance;
    attacker.in_water                   = player.in_water;
    attacker.on_climbable               = player.on_climbable;
    attacker.blind                      = player.blind;
    attacker.riding                     = player.riding;
    // A walking player covers about 0.215 blocks a tick, a sprinting one about
    // 0.28. The sweep wants a *standing* attacker, so the threshold sits below
    // a walk rather than between a walk and a sprint.
    attacker.moving_faster_than_walking = step > 0.2;
    last_step                           = step;

    if (!use.active()) {
        return {};
    }
    const std::string_view item = use.item;
    if (gameplay::tick_use(use)) {
        return item;
    }
    return {};
}

void CombatSession::wear_held(const CombatIo& io, const CombatPlayer& player,
                              gameplay::ToolAction action, i32 times) {
    if (player.game_mode == 1 || times <= 0 || !io.held_item) {
        return;
    }
    const std::string_view item = io.held_item();
    if (item.empty()) {
        return;
    }
    const gameplay::Weapon weapon = io.held_weapon ? io.held_weapon() : gameplay::Weapon{item};
    i32 damage = io.held_damage ? io.held_damage() : 0;
    for (i32 i = 0; i < times; ++i) {
        const gameplay::WearResult result =
            gameplay::wear(item, damage, action, weapon.unbreaking, random_);
        damage = result.damage;
        if (result.broke) {
            if (io.break_held_item) {
                io.break_held_item();
            }
            return;
        }
    }
    if (io.set_held_damage) {
        io.set_held_damage(damage);
    }
}

void CombatSession::deliver(const gameplay::AttackOutcome& outcome, i32 target,
                            const CombatPlayer& player, const CombatIo& io) {
    // ── Knockback ───────────────────────────────────────────────────────────
    //
    // Two impulses in two directions, in the order the server applies them: the
    // hit's own along the line from attacker to victim, then Knockback's and
    // the sprint's along the attacker's *facing*. The second halves whatever
    // the first left, which is why the order is not a detail.
    if (io.entity_position && io.set_entity_velocity) {
        const auto where = io.entity_position(target);
        if (where) {
            const f32 resistance =
                io.entity_knockback_resistance ? io.entity_knockback_resistance(target) : 0.0F;
            const bool grounded = io.entity_on_ground ? io.entity_on_ground(target) : true;

            Vec3d velocity{0.0, 0.0, 0.0};
            // From the attacker to the victim. `apply_knockback` subtracts, so
            // the vector handed in points the other way — as vanilla's own call
            // does, with the attacker's position minus the victim's.
            const f64 dx = player.x - where->x;
            const f64 dz = player.z - where->z;
            velocity = gameplay::apply_knockback(velocity, grounded, outcome.knockback_strength,
                                                 dx, dz, resistance, constants);
            if (outcome.directed_knockback > 0.0F) {
                const Vec3d facing = gameplay::knockback_direction(player.yaw);
                // The facing goes in as it comes out. `apply_knockback`
                // subtracts, and a yaw of 0 gives (0, -1), which pushes the
                // victim toward +z — where the player is looking. Negating it
                // would push them straight through the attacker.
                velocity = gameplay::apply_knockback(velocity, grounded,
                                                     outcome.directed_knockback, facing.x,
                                                     facing.z, resistance, constants);
            }
            io.set_entity_velocity(target, velocity);
        }
    }

    // ── The sweep ───────────────────────────────────────────────────────────
    if (outcome.sweeping && io.entities_near && io.hurt_entity) {
        const Vec3d centre{player.x, player.y, player.z};
        io.entities_near(centre, kSweepRadius, target, [&](i32 other) {
            (void)io.hurt_entity(other, outcome.sweep_damage, false);
            if (io.entity_position && io.set_entity_velocity) {
                const auto where = io.entity_position(other);
                if (where) {
                    const bool grounded =
                        io.entity_on_ground ? io.entity_on_ground(other) : true;
                    const Vec3d facing = gameplay::knockback_direction(player.yaw);
                    const Vec3d pushed = gameplay::apply_knockback(
                        Vec3d{0.0, 0.0, 0.0}, grounded, constants.sweep_knockback, facing.x,
                        facing.z, 0.0F, constants);
                    io.set_entity_velocity(other, pushed);
                }
            }
        });
    }

    // ── What the clients see ────────────────────────────────────────────────
    if (outcome.critical && io.broadcast) {
        const auto packet = net::encode_entity_animation(target, kAnimationCritical);
        io.broadcast(net::clientbound::kEntityAnimation, packet);
    }

    if (io.exhaust) {
        io.exhaust(outcome.exhaustion);
    }
}

CombatOutcome CombatSession::on_interact(const net::Interact& packet, const CombatPlayer& player,
                                         const CombatIo& io) {
    CombatOutcome out;

    if (packet.kind != net::InteractKind::Attack) {
        // Right-clicking an entity: shearing a sheep, saddling a pig, naming a
        // mob. None of them is implemented here, and each is named rather than
        // treated as a miss.
        out.unsupported = "interact with entity";
        return out;
    }

    const gameplay::Weapon weapon =
        io.held_weapon ? io.held_weapon()
                       : gameplay::Weapon{io.held_item ? io.held_item() : std::string_view{}};

    const gameplay::AttackOutcome resolved = gameplay::resolve_attack(weapon, attacker, constants);
    // The gauge resets on every attack that is *sent*, including one that
    // reaches nothing. A server that only reset it on a hit would let a client
    // keep a full charge by swinging at air between blows.
    attacker.strength_ticker = 0;

    if (!io.hurt_entity || !io.hurt_entity(packet.entity_id, resolved.damage, resolved.critical)) {
        // The target is gone. The swing still cost the gauge, which is the
        // point above, and nothing else.
        return out;
    }

    out.hit      = true;
    out.critical = resolved.critical;
    out.swept    = resolved.sweeping;
    out.damage   = resolved.damage;

    deliver(resolved, packet.entity_id, player, io);
    wear_held(io, player, gameplay::ToolAction::Attack, 1);

    if (resolved.sprint_knockback) {
        // A sprinting hit stops the sprint and slows the attacker. Only the
        // caller can store the attacker's velocity, so what leaves here is the
        // velocity packet; the sprint flag is the caller's to clear.
        if (io.set_entity_velocity) {
            io.set_entity_velocity(player.entity_id,
                                   gameplay::attacker_after_sprint_hit(
                                       Vec3d{0.0, 0.0, 0.0}, constants));
        }
    }
    return out;
}

CombatOutcome CombatSession::on_use_item_on(const net::UseItemOn& packet,
                                            const CombatPlayer& player, const CombatIo& io,
                                            world::LevelWriter&      level,
                                            const gameplay::ItemUse& rules) {
    CombatOutcome out;

    gameplay::UseContext context;
    context.position =
        BlockPos{packet.position.x, packet.position.y, packet.position.z};
    context.face     = packet.face;
    context.cursor_x = packet.cursor_x;
    context.cursor_y = packet.cursor_y;
    context.cursor_z = packet.cursor_z;
    context.item     = io.held_item ? io.held_item() : std::string_view{};
    context.sneaking = player.sneaking;

    const gameplay::UseOutcome result = rules.use_on(level, context);
    out.unsupported                   = result.unsupported;

    if (result.result == gameplay::UseResult::Pass) {
        return out;
    }

    if (result.item_damage > 0) {
        wear_held(io, player, gameplay::ToolAction::UseOnBlock, result.item_damage);
    }
    if (result.consume_one && player.game_mode != 1 && io.consume_one_held) {
        io.consume_one_held();
    }
    out.spawn_primed_tnt = result.spawn_primed_tnt;
    out.tnt_position     = result.tnt_position;
    out.screen           = result.screen;
    out.screen_position  = result.screen_position;
    out.hit              = result.result == gameplay::UseResult::Success;

    if (result.result == gameplay::UseResult::Success && io.broadcast) {
        const auto swing = net::encode_entity_animation(player.entity_id, kAnimationSwingMain);
        io.broadcast(net::clientbound::kEntityAnimation, swing);
    }
    // Whatever happened, the client predicted something and is waiting to be
    // told whether it was right. Without this it rolls the change back after a
    // moment, which looks exactly like the server ignoring the player.
    if (io.send) {
        const auto ack = net::encode_acknowledge_dig(packet.sequence);
        io.send(net::clientbound::kAcknowledgeDig, ack);
    }
    return out;
}

CombatOutcome CombatSession::on_use_item(const net::UseItem& packet, const CombatPlayer& player,
                                         const CombatIo& io) {
    CombatOutcome out;
    const std::string_view item = io.held_item ? io.held_item() : std::string_view{};

    if (gameplay::begin_use(use, item, player.food, player.max_food)) {
        out.hit = true;
    } else if (!item.empty()) {
        out.unsupported = item;
    }
    if (io.send) {
        const auto ack = net::encode_acknowledge_dig(packet.sequence);
        io.send(net::clientbound::kAcknowledgeDig, ack);
    }
    return out;
}

void CombatSession::on_release(const CombatIo&) {
    // Letting go at tick 31 has eaten nothing. That is the rule, and it is why
    // this is a cancel rather than a completion.
    gameplay::cancel_use(use);
}

void CombatSession::on_swing(net::Hand hand, const CombatPlayer& player, const CombatIo& io) {
    if (!io.broadcast) {
        return;
    }
    const auto packet = net::encode_entity_animation(
        player.entity_id, hand == net::Hand::Main ? kAnimationSwingMain : kAnimationSwingOff);
    io.broadcast(net::clientbound::kEntityAnimation, packet);
}

}  // namespace ov::server
