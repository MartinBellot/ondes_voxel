// The packets that make a player mortal.
//
// Health, hunger, experience, being hurt, dying and coming back. None of the
// ids below were read off a summary: a probe client joined a real 1.20.1
// server, one thing was changed at a time from the console, and the id that
// carried the change is what is written here. See scripts/measure_survival.py.
//
// Two of them disagree with what a reader would expect, and both disagreements
// are the kind that produce a disconnect with no useful message:
//
//   * **Combat Death carries no killer entity id.** The archived protocol page
//     for this era lists Player ID, then an Int entity id, then the message.
//     What a 1.20.1 server actually sends is Player ID and then the message,
//     315 bytes with no room for anything between them. Writing the extra four
//     bytes pushes the string length out of alignment and the client drops the
//     connection while reading a chat component.
//
//   * **There is no Hurt Animation.** Damage Event (0x18) is the whole of it in
//     1.20.1; the client derives the flinch, the red flash and the knockback
//     direction from it. Two hits of different damage types were captured and
//     compared byte for byte, and no second packet of the right shape was
//     constant between them — because there was no second packet.
#pragma once

#include "ov/base/types.hpp"
#include "ov/io/byte_reader.hpp"
#include "ov/protocol/entity.hpp"
#include "ov/protocol/play.hpp"

#include <optional>
#include <string_view>
#include <vector>

namespace ov::net {

namespace clientbound {
// Damage Event (0x18) and Hurt Animation (0x21) are already declared in
// entity.hpp, with an encoder each, and are not repeated here — two
// definitions of one packet id is how a reader and a writer come to disagree.
//
// Worth recording anyway, since this module is where a reader will look: a
// 1.20.1 server sends **only** Damage Event when something is hurt. Two hits of
// different damage types were captured against the vanilla jar and compared
// byte for byte; there was no Hurt Animation in either window. The client
// derives the flinch and the knockback direction from Damage Event.

/// Combat Death — the death screen, and its message.
inline constexpr i32 kCombatDeath = 0x38;
/// Respawn — a whole new world state, sent when a dead player comes back.
inline constexpr i32 kRespawn = 0x41;
/// Set Health: the hearts, the haunches and the hidden saturation.
inline constexpr i32 kSetHealth = 0x57;
/// Set Experience: the bar, the level and the running total.
inline constexpr i32 kSetExperience = 0x56;
/// Spawn Experience Orb. Its own packet, not the generic Spawn Entity: an orb
/// carries a value rather than a type, and it is a short rather than metadata.
inline constexpr i32 kSpawnExperienceOrb = 0x02;
}  // namespace clientbound

namespace serverbound {
/// Client Command. Action 0 is "perform respawn", which is the only way a dead
/// player comes back — the server must not do it unasked, or the death screen
/// vanishes before it has been read.
inline constexpr i32 kClientCommand = 0x07;

/// Player Command — sneaking, sprinting, and horses.
///
/// Hunger needs it: sprinting is the only movement in 1.20.1 that charges
/// exhaustion, and the *server* cannot tell a sprint from a walk by watching
/// positions. It has to be told, and this is the packet that tells it.
inline constexpr i32 kPlayerCommand = 0x1E;
}  // namespace serverbound

/// Game Event reasons this module cares about.
///
/// The numbers are the protocol's own. Reason 0 puts up "you have no home bed
/// or respawn anchor, or it was obstructed" — which is what a player sees when
/// their bed is gone, and without it they respawn at world spawn with no
/// explanation at all.
namespace game_event {
inline constexpr u8 kNoRespawnBlockAvailable = 0;
inline constexpr u8 kWinGame                 = 4;
inline constexpr u8 kStartWaitingForChunks   = 13;
}  // namespace game_event

/// The value of Game Event 4 that shows the credits rather than just respawning.
inline constexpr f32 kWinGameShowCredits = 1.0F;
inline constexpr f32 kWinGameRespawn     = 0.0F;

/// Health, food and saturation, all in one packet.
///
/// Saturation is sent even though the vanilla client never draws it: it uses it
/// to decide whether the haunches wobble. Sending a stale one makes a well-fed
/// player's hunger bar shake for no reason.
[[nodiscard]] std::vector<u8> encode_set_health(f32 health, i32 food, f32 saturation);

/// The experience bar, the level, and the lifetime total.
///
/// `bar` is 0..1 within the current level, not a fraction of the total. The
/// client draws it directly, so a total-based fraction leaves the bar almost
/// empty at every level.
[[nodiscard]] std::vector<u8> encode_set_experience(f32 bar, i32 level, i32 total);

/// The death screen.
///
/// `message` is a chat component as JSON. Building it is the caller's job
/// because the sentence depends on the damage type *and* on who is to blame,
/// which this layer knows nothing about.
[[nodiscard]] std::vector<u8> encode_combat_death(i32 player_entity_id,
                                                  std::string_view message_json);

/// A death message for one damage type and, optionally, a killer.
///
/// Produces `{"translate":"death.attack.<id>","with":[{"text":"<victim>"}]}`,
/// with the killer appended when there is one — which is what selects the
/// two-argument form of the sentence. The client owns the wording; we only
/// choose the key and hand it its arguments.
[[nodiscard]] std::string death_message_json(std::string_view translation_key,
                                             std::string_view victim,
                                             std::string_view killer = {});

/// Everything Respawn carries.
///
/// Measured field by field against a real server's own bytes, because this is
/// the packet with the most fields that are one byte each and therefore the
/// most ways to be off by one without noticing.
struct Respawn {
    std::string_view dimension_type{"minecraft:overworld"};
    std::string_view dimension_name{"minecraft:overworld"};
    i64              hashed_seed{0};
    u8               game_mode{0};
    i8               previous_game_mode{-1};
    bool             is_debug{false};
    bool             is_flat{true};

    /// What the client keeps. Bit 0 is attributes, bit 1 is metadata. Zero on a
    /// death — a respawning player keeps nothing — and 3 when the same player
    /// merely changes dimension.
    u8 data_kept{0};

    /// Where the player died, so the client can point its recovery compass.
    /// Absent on anything that is not a death.
    std::optional<std::string_view> death_dimension;
    WirePosition                    death_position;

    i32 portal_cooldown{0};
};

[[nodiscard]] std::vector<u8> encode_respawn(const Respawn& respawn);

/// One experience orb appearing.
///
/// `count` is the orb's value and it is a **short**, which is why the game
/// splits a large reward into several orbs instead of one big one — a value of
/// 2477 is the largest denomination it uses and the split is visible on the
/// wire as several of these packets.
[[nodiscard]] std::vector<u8> encode_spawn_experience_orb(i32 entity_id, f64 x, f64 y, f64 z,
                                                          i16 count);

/// What a client asked us to do about being dead, or about statistics.
enum class ClientCommand : u8 { PerformRespawn, RequestStats };

[[nodiscard]] std::optional<ClientCommand> parse_client_command(std::span<const u8> payload);

/// The actions a Player Command can carry, by their protocol numbers.
///
/// Only the four this server acts on are named. An unrecognised action is
/// refused rather than folded into the nearest one — treating "open horse
/// inventory" as "stop sprinting" would silently stop charging hunger.
enum class PlayerCommandAction : u8 {
    StartSneaking     = 0,
    StopSneaking      = 1,
    LeaveBed          = 2,
    StartSprinting    = 3,
    StopSprinting     = 4,
};

struct PlayerCommand {
    i32                 entity_id{0};
    PlayerCommandAction action{PlayerCommandAction::StopSprinting};
};

[[nodiscard]] std::optional<PlayerCommand> parse_player_command(std::span<const u8> payload);

}  // namespace ov::net
