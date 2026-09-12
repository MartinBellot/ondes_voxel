#include "survival_session.hpp"

#include "ov/protocol/entity.hpp"
#include "ov/protocol/play.hpp"
#include "ov/protocol/survival.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace ov::server {
namespace {

/// Below this, the void takes over. The overworld's floor is at -64, and
/// vanilla starts hurting sixty-four blocks below the build limit.
constexpr f64 kVoidMargin = 64.0;

/// The void's hit, every ten ticks.
constexpr f32 kVoidDamage       = 4.0F;
constexpr i32 kVoidIntervalTick = 10;

[[nodiscard]] bool takes_damage(u8 game_mode) noexcept {
    // 1 is creative, 3 is spectator. Both are immune, and a spectator is also
    // immune to hunger — which is why this gate is here rather than inside each
    // damage source.
    return game_mode != 1 && game_mode != 3;
}

}  // namespace

void SurvivalSession::award_experience(i32 amount) {
    if (amount <= 0) {
        return;
    }
    experience_total += amount;
    experience_points += amount;
    while (true) {
        const i32 cost = gameplay::experience_to_next_level(experience_level, curve);
        if (cost <= 0 || experience_points < cost) {
            break;
        }
        experience_points -= cost;
        ++experience_level;
    }
}

void SurvivalSession::spend_experience(i32 amount) {
    if (amount <= 0) {
        return;
    }
    const i32 held = gameplay::total_experience_for_level(experience_level, curve) +
                     experience_points;
    const i32 left = std::max(0, held - amount);
    const gameplay::LevelProgress progress = gameplay::level_for_total(left, curve);
    experience_level  = progress.level;
    experience_points = progress.points_into_level;
    experience_total  = std::max(0, experience_total - amount);
}

void SurvivalSession::send_health_if_changed(const SurvivalIo& io) {
    if (!io.send) {
        return;
    }
    // A tolerance, not equality: health is a float and regeneration moves it by
    // a sixth of a point at a time. Comparing exactly would send a packet every
    // tick of a heal, and comparing too loosely would leave the last sliver of
    // a heart undrawn.
    const bool changed = needs_first_update ||
                         std::abs(health.health - broadcast_health) > 0.0001F ||
                         food.food != broadcast_food;
    if (!changed) {
        return;
    }
    broadcast_health = health.health;
    broadcast_food   = food.food;
    io.send(net::clientbound::kSetHealth,
            net::encode_set_health(health.health, food.food, food.saturation));
}

void SurvivalSession::send_experience_if_changed(const SurvivalIo& io) {
    if (!io.send) {
        return;
    }
    if (!needs_first_update && experience_level == broadcast_level &&
        experience_points == broadcast_points) {
        return;
    }
    broadcast_level  = experience_level;
    broadcast_points = experience_points;
    const i32 cost   = gameplay::experience_to_next_level(experience_level, curve);
    const f32 bar =
        cost > 0 ? static_cast<f32>(experience_points) / static_cast<f32>(cost) : 0.0F;
    io.send(net::clientbound::kSetExperience,
            net::encode_set_experience(bar, experience_level, experience_total));
}

void SurvivalSession::send_state(const SurvivalIo& io) {
    if (!io.send) {
        return;
    }
    broadcast_health = health.health;
    broadcast_food   = food.food;
    broadcast_level  = experience_level;
    broadcast_points = experience_points;
    io.send(net::clientbound::kSetHealth,
            net::encode_set_health(health.health, food.food, food.saturation));
    const i32 cost = gameplay::experience_to_next_level(experience_level, curve);
    const f32 bar =
        cost > 0 ? static_cast<f32>(experience_points) / static_cast<f32>(cost) : 0.0F;
    io.send(net::clientbound::kSetExperience,
            net::encode_set_experience(bar, experience_level, experience_total));
}

void SurvivalSession::note_movement(f64 y, bool on_ground, f64 horizontal_distance) {
    if (!last_y_valid) {
        last_y       = y;
        last_y_valid = true;
        return;
    }
    const f32 fall =
        gameplay::accumulate_fall(health, y - last_y, on_ground, constants_damage);
    last_y = y;
    if (fall > 0.0F) {
        pending_fall_damage += fall;
    }
    // Sprinting is the only movement that costs anything in 1.20.1: walking was
    // measured at zero exhaustion over twenty-six blocks. Charged per block
    // actually covered, not per tick, so a laggy client is not fed for free.
    if (sprinting && on_ground && horizontal_distance > 0.0) {
        gameplay::add_exhaustion(
            food, static_cast<f32>(horizontal_distance) * constants_food.sprint_per_block,
            constants_food);
    }
}

gameplay::DamageResult SurvivalSession::hurt(gameplay::DamageKind kind, f32 amount,
                                             const SurvivalIo& io, i32 entity_id,
                                             const gameplay::DamageConstants* window,
                                             std::optional<i32> source) {
    const gameplay::DamageResult result = gameplay::apply_damage(
        health, kind, amount, window != nullptr ? *window : constants_damage, mitigation);
    if (!result.applied) {
        return result;
    }

    last_kill_ = kind;

    // The damage type's own exhaustion, from the datapack's declaration. Zero
    // for a fall, a tenth for a cactus — being hurt makes you hungry, and it is
    // the damage type that says how much.
    gameplay::add_exhaustion(food, gameplay::damage_type(kind).exhaustion, constants_food);

    // Damage Event and nothing else, for the hits measured here: two
    // environmental and mob hits of different types produced exactly one packet
    // each, and the client derives the flinch from it. A player's blow is the
    // exception — the real server then also sends the victim a Hurt Animation
    // (the scoreboard wave's two-probe capture) — and server.cpp's PvP path
    // sends it; this function does not.
    const std::vector<u8> payload =
        net::encode_damage_event(entity_id, damage_type_id(kind), source, source);
    if (io.send) {
        io.send(net::clientbound::kDamageEvent, payload);
    }
    if (io.broadcast) {
        io.broadcast(net::clientbound::kDamageEvent, payload);
    }
    return result;
}

SurvivalOutcome SurvivalSession::tick(const SurvivalPlayer& player, const SurvivalIo& io,
                                      gameplay::Difficulty difficulty,
                                      bool natural_regeneration, f64 world_bottom_y) {
    SurvivalOutcome outcome;

    if (needs_first_update) {
        send_state(io);
        needs_first_update  = false;
        pending_fall_damage = 0.0F;
        return outcome;
    }

    // The counters move before anything is allowed to hurt: the gap between two
    // hits is counted in these calls, and applying damage first would make a
    // second hit on the same tick look one tick older than it is. The measured
    // ten-tick boundary moves by one if this order is reversed.
    gameplay::tick_health(health, constants_damage);

    // ── end ── Only a death already carried out stops here. A death dealt
    // between two ticks — `/kill` hurts through the command path — used to be
    // swallowed by this early return with `health.dead` set: no Combat Death,
    // no `awaiting_respawn`, and the Client Command that followed respawned
    // nobody (found by the End's death-and-return e2e). It now falls through,
    // hurts nothing more, and reaches the death below on this pass.
    if (awaiting_respawn) {
        return outcome;
    }

    const bool mortal = takes_damage(player.game_mode) && !health.dead;

    // ── Falling ─────────────────────────────────────────────────────────────
    //
    // Accumulated by `note_movement` as the position packets arrive; this only
    // applies what it found. The split is so that the accumulation sees every
    // packet — a fall sampled once a tick loses whichever updates shared a tick
    // with another — while every packet still leaves from the tick thread.
    // ── effects: jump boost and slow falling ──
    f32 fall = pending_fall_damage;
    if (player.slow_falling) {
        fall = 0.0F;
    } else if (player.jump_boost >= 0) {
        fall = std::max(fall - static_cast<f32>(player.jump_boost + 1), 0.0F);
    }
    // ── end effects ──
    last_fall = 0.0F;  // ── sound ──
    if (fall > 0.0F && mortal) {
        (void)hurt(gameplay::DamageKind::Fall, fall, io, player.entity_id);
        last_fall = fall;  // ── sound ──
    }
    pending_fall_damage = 0.0F;

    // ── Breath ──────────────────────────────────────────────────────────────
    if (mortal && player.submerged && (player.breathes_underwater || player.respiration_saves)) {
        // ── effects: water breathing freezes the air, it does not refill it ──
        // ── enchanting: and so does a tick Respiration saves ──
    } else if (mortal) {
        const f32 drown = gameplay::tick_air(health, player.submerged, constants_damage);
        if (drown > 0.0F) {
            (void)hurt(gameplay::DamageKind::Drown, drown, io, player.entity_id);
        }
    } else {
        health.air = constants_damage.max_air;
    }

    // ── The void ────────────────────────────────────────────────────────────
    //
    // out_of_world is in #bypasses_invulnerability, so it does not wait for the
    // window — which is what makes falling out of the world quick rather than a
    // long slow drop through an untouchable ten ticks.
    if (player.y < world_bottom_y - kVoidMargin && player.game_mode != 3) {
        ++void_ticks_;
        if (void_ticks_ >= kVoidIntervalTick) {
            void_ticks_ = 0;
            (void)hurt(gameplay::DamageKind::OutOfWorld, kVoidDamage, io, player.entity_id);
        }
    } else {
        void_ticks_ = 0;
    }

    // ── Hunger ──────────────────────────────────────────────────────────────
    if (mortal) {
        // Sprinting and walking are charged where the movement is known; what
        // is charged here is only what hunger itself does.
        const gameplay::FoodTick hunger = gameplay::tick_food(
            food, health.health, health.max_health, difficulty, natural_regeneration,
            constants_food);
        if (hunger.heal > 0.0F) {
            health.health = std::min(health.max_health, health.health + hunger.heal);
        }
        if (hunger.damage > 0.0F) {
            (void)hurt(gameplay::DamageKind::Starve, hunger.damage, io, player.entity_id);
        }
    }

    // ── Death ───────────────────────────────────────────────────────────────
    if (health.dead) {
        awaiting_respawn = true;
        outcome.died     = true;

        outcome.dropped_experience = gameplay::death_experience(experience_level);
        outcome.orb_count =
            gameplay::split_into_orbs(outcome.dropped_experience, outcome.orbs);
        experience_level  = 0;
        experience_points = 0;
        experience_total  = 0;

        if (io.send) {
            // The message key comes from the damage type that killed them, and
            // the client owns the wording. `last_kill_` is set by `hurt`.
            std::array<char, 64> key{};
            const usize          length =
                gameplay::death_message_key(last_kill_, key.data(), key.size());
            // ── pvp ── the server's own message when it has one: names dressed
            // by their teams, a killer named. Then everyone's line of it.
            const std::string_view key_view{key.data(), length};
            std::string message = io.death_message ? io.death_message(key_view) : std::string{};
            if (message.empty()) {
                message = net::death_message_json(key_view, player.name);
            }
            io.send(net::clientbound::kCombatDeath,
                    net::encode_combat_death(player.entity_id, message));
            if (io.announce_death) {
                io.announce_death(message);
            }
        }
        send_state(io);
        return outcome;
    }

    send_health_if_changed(io);
    send_experience_if_changed(io);
    return outcome;
}

bool SurvivalSession::perform_respawn(const SurvivalPlayer& player, const SurvivalIo& io,
                                      SurvivalOutcome& outcome, i64 hashed_seed) {
    if (!awaiting_respawn) {
        return false;
    }
    awaiting_respawn = false;
    blamed.clear();  // ── pvp ── a new life owes nobody
    blamed_uuid = {};

    health              = gameplay::HealthState{};
    food                = gameplay::FoodState{};
    needs_first_update  = false;
    last_y_valid        = false;
    pending_fall_damage = 0.0F;
    sprinting           = false;
    void_ticks_         = 0;
    broadcast_health    = -1.0F;
    broadcast_food      = -1;
    broadcast_level     = -1;
    broadcast_points    = -1;

    if (io.send) {
        net::Respawn respawn;
        respawn.hashed_seed = hashed_seed;
        respawn.game_mode   = player.game_mode;
        // Nothing is kept across a death. Sending 3 here — attributes and
        // metadata both — leaves the client showing the corpse's health bar
        // until something else happens to change it.
        respawn.data_kept       = 0;
        respawn.death_dimension = std::string_view{death_dimension};  // ── end ──
        respawn.death_position  = net::WirePosition{static_cast<i32>(std::floor(player.x)),
                                                    static_cast<i32>(std::floor(player.y)),
                                                    static_cast<i32>(std::floor(player.z))};
        io.send(net::clientbound::kRespawn, net::encode_respawn(respawn));
    }

    outcome.respawned = true;
    outcome.respawn_x = spawn.x;
    outcome.respawn_y = spawn.y;
    outcome.respawn_z = spawn.z;
    send_state(io);
    return true;
}

}  // namespace ov::server
