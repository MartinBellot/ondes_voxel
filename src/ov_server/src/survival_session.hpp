// One player's survival state, and the packets it owes their client.
//
// This exists so that server.cpp does not grow a fifth system inside its packet
// switch. Everything about being alive — health, the invulnerability window,
// hunger, experience, drowning, falling, dying and coming back — lives here,
// and server.cpp touches it in two short places: one case in the packet switch
// and one block in the tick.
//
// It is not in ov_gameplay because it sends packets, and ov_gameplay (layer 9)
// must never learn what a socket is. The *rules* are all up there; what is here
// is the wiring, plus the state that has to persist between two packets.
//
// Not a public header: it lives in src/ and nothing outside ov_server includes
// it, which is what lets it use std::function at a boundary that is not hot.
#pragma once

#include "ov/base/types.hpp"
#include "ov/gameplay/damage.hpp"
#include "ov/gameplay/experience.hpp"
#include "ov/gameplay/food.hpp"

#include <array>
#include <functional>
#include <span>
#include <string>

namespace ov::server {

/// How this session talks to the world. Two sinks, because everything a
/// survival system emits goes either to the player it happened to or to
/// everybody else watching them.
struct SurvivalIo {
    /// To this player's own client.
    std::function<void(i32 packet_id, std::span<const u8> payload)> send;
    /// To every other client.
    std::function<void(i32 packet_id, std::span<const u8> payload)> broadcast;
};

/// What the tick needs to know about the player it is ticking.
///
/// Passed in rather than held, because the server owns the player record and a
/// second copy of a position is a second thing that can be stale.
struct SurvivalPlayer {
    i32              entity_id{0};
    std::string_view name;
    f64              x{0.0};
    f64              y{0.0};
    f64              z{0.0};
    bool             on_ground{true};
    /// 0 survival, 1 creative, 2 adventure, 3 spectator. Creative and spectator
    /// take no damage and never get hungry, which is a rule and not an
    /// optimisation: a creative player who starves is a bug report.
    u8 game_mode{0};
    /// Whether the player's eyes are under water. The caller knows the world;
    /// this module does not.
    bool submerged{false};

    // ── effects ─────────────────────────────────────────────────────────────
    /// Water breathing or conduit power: under water the air is frozen,
    /// neither drained nor refilled. Measured on cows, see effect_session.hpp.
    bool breathes_underwater{false};
    /// Jump boost's amplifier, -1 for none: a landing costs `amp + 1` less.
    /// Measured, fifteen falls of fifteen.
    i32 jump_boost{-1};
    /// Slow falling: a landing costs nothing.
    bool slow_falling{false};
};

/// What one tick of survival asked the server to do.
struct SurvivalOutcome {
    /// The player died on this tick. The server owes the inventory drop.
    bool died{false};
    /// Experience to scatter where the player fell, already split into orbs by
    /// `orbs` / `orb_count`.
    i32   dropped_experience{0};
    usize orb_count{0};
    /// At most eight — 100 experience is four orbs and the cap makes more
    /// impossible, but the buffer has room for a mob's reward too.
    std::array<i32, 8> orbs{};
    /// The player asked to come back and did. The server owes them chunks.
    bool respawned{false};
    /// Where they came back.
    f64 respawn_x{0.0};
    f64 respawn_y{0.0};
    f64 respawn_z{0.0};
};

/// Everything one player carries between two packets.
class SurvivalSession {
public:
    /// The player's bed or anchor, or the world spawn when they have neither.
    struct SpawnPoint {
        f64  x{0.5};
        f64  y{0.0};
        f64  z{0.5};
        bool is_bed{false};
    };

    gameplay::HealthState  health{};
    gameplay::FoodState    food{};
    gameplay::ExperienceCurve curve{};

    /// Level, and points into the current level. The remainder is kept as an
    /// integer rather than as the float the vanilla client draws: a float
    /// accumulated one division at a time drifts, and CLAUDE.md principle 5
    /// does not allow that in the simulation. The float is computed for the
    /// packet and nowhere else.
    i32 experience_level{0};
    i32 experience_points{0};
    i32 experience_total{0};

    /// True until the first tick has told the client where it stands. A client
    /// that is never sent Set Health draws twenty hearts whatever happens.
    bool needs_first_update{true};

    /// Dead, waiting for the client to press the button.
    bool awaiting_respawn{false};

    /// The last position used for the fall accumulator, and whether there is
    /// one yet.
    f64  last_y{0.0};
    bool last_y_valid{false};

    SpawnPoint spawn{};

    /// Ticks since the last time hunger was ticked. Hunger runs at 20 Hz like
    /// everything else, but the *packets* only go out when a number changed.
    f32 broadcast_health{-1.0F};
    i32 broadcast_food{-1};
    i32 broadcast_level{-1};
    i32 broadcast_points{-1};

    /// Give experience, moving the level along.
    void award_experience(i32 amount);

    /// Take some, for an enchantment or an anvil. Never below zero.
    void spend_experience(i32 amount);

    /// Apply a hit. Returns what actually came off, for the packets.
    ///
    /// `window` replaces the damage constants for this one hit — an effect's
    /// periodic damage is measured against a window entered at more than ten
    /// rather than ten or more (see gameplay::effect_damage_constants).
    [[nodiscard]] gameplay::DamageResult hurt(gameplay::DamageKind kind, f32 amount,
                                              const SurvivalIo& io, i32 entity_id,
                                              const gameplay::DamageConstants* window = nullptr);

    /// Resistance, kept current by the effect session.
    gameplay::DamageMitigation mitigation{};

    /// One tick.
    [[nodiscard]] SurvivalOutcome tick(const SurvivalPlayer& player, const SurvivalIo& io,
                                       gameplay::Difficulty difficulty, bool natural_regeneration,
                                       f64 world_bottom_y);

    /// The client pressed "respawn". False when it was not dead, which happens
    /// and is not an error: a client sends the same packet to open its stats.
    [[nodiscard]] bool perform_respawn(const SurvivalPlayer& player, const SurvivalIo& io,
                                       SurvivalOutcome& outcome, i64 hashed_seed);

    /// Feed one movement report in.
    ///
    /// Called from the network thread, once per position packet, because that
    /// is where vanilla accumulates a fall too. Sampling the position once a
    /// tick instead loses whichever updates arrived in the same tick, and a
    /// nine-block fall then costs five points rather than six — measured on our
    /// own server before this existed.
    ///
    /// Sends nothing. The damage it finds is left in `pending_fall_damage` for
    /// the tick to apply, so every packet this system emits still leaves from
    /// one thread.
    void note_movement(f64 y, bool on_ground, f64 horizontal_distance);

    /// Fall damage found by `note_movement` and not yet applied.
    f32 pending_fall_damage{0.0F};

    /// Whether the client says it is sprinting. The only movement that charges
    /// hunger, and unknowable from positions alone.
    bool sprinting{false};

    /// Charge exhaustion for something the player did.
    void exhaust(f32 amount) { gameplay::add_exhaustion(food, amount, constants_food); }

    /// Send Set Health and Set Experience now, whatever they hold.
    void send_state(const SurvivalIo& io);

    gameplay::DamageConstants constants_damage{};
    gameplay::FoodConstants   constants_food{};
    gameplay::OrbConstants    constants_orbs{};

private:
    void send_health_if_changed(const SurvivalIo& io);
    void send_experience_if_changed(const SurvivalIo& io);

    /// What last hurt this player, which is what names the death.
    ///
    /// Kept rather than derived: by the time the health reaches zero the cause
    /// is gone, and a death message that says "died" for everything is the one
    /// thing a player always notices.
    gameplay::DamageKind last_kill_{gameplay::DamageKind::Generic};

    /// How long the player has been below the world. The void hits every ten
    /// ticks rather than every tick, which is why it needs a counter of its own
    /// — the invulnerability window cannot pace it, because out_of_world
    /// ignores that window by design.
    i32 void_ticks_{0};
};

/// The id "minecraft:fall" carries in the registry codec we send at login.
///
/// The damage_type registry is one of the six the server ships to the client
/// rather than one the client hard-codes, so this is ours — but it must match
/// the codec byte for byte or the client names the wrong cause of death. It is
/// the alphabetical index of the name, which is how a datapack registry is
/// ordered; a test in ov_gameplay reads the codec back and checks it.
[[nodiscard]] constexpr i32 damage_type_id(gameplay::DamageKind kind) noexcept {
    return static_cast<i32>(kind);
}

}  // namespace ov::server
