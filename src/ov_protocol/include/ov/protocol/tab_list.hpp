// The tab list's packets, and the horse's screen, in protocol 763.
//
// Player Info Update (0x3A), Player Info Remove (0x39), Set Tab List Header
// And Footer (0x65) and Open Horse Screen (0x20). Field layouts from the frozen
// protocol page (oldid=2773082, "1.20.1, protocol 763"); the byte tests in
// tests/test_tab_list_packets.cpp are written out by hand from its tables.
//
// Player Info Update is the tab list *and* the list the client builds player
// entities from, so every action is read even when nothing draws it: the
// entries are in one array of records whose width depends on the action mask,
// and a skipped field misreads every player after it.
//
// Every decoder is written for a hostile peer: nullopt on a short read, on
// trailing bytes, on a count the remaining bytes could not hold.
#pragma once

#include "ov/base/types.hpp"
#include "ov/protocol/types.hpp"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ov::net {

namespace clientbound {
inline constexpr i32 kOpenHorseScreen          = 0x20;
inline constexpr i32 kSetTabListHeaderFooter   = 0x65;
}  // namespace clientbound

/// The action mask of Player Info Update. Each set bit adds its fields to
/// every entry, in this order.
namespace player_info {
inline constexpr u8 kAddPlayer         = 0x01;
inline constexpr u8 kInitializeChat    = 0x02;
inline constexpr u8 kUpdateGameMode    = 0x04;
inline constexpr u8 kUpdateListed      = 0x08;
inline constexpr u8 kUpdateLatency     = 0x10;
inline constexpr u8 kUpdateDisplayName = 0x20;
inline constexpr u8 kAllActions        = 0x3F;
}  // namespace player_info

/// A skin property of Add Player. Kept whole, value and signature: the name is
/// "textures" and what a skin renderer would read.
struct ProfileProperty {
    std::string                name;
    std::string                value;
    std::optional<std::string> signature;

    friend bool operator==(const ProfileProperty&, const ProfileProperty&) = default;
};

/// Initialize Chat's signature data. Read so the entry after it is not
/// misread; nothing on this client verifies a signature.
struct ChatSessionData {
    Uuid            session{};
    i64             expires_ms{0};
    std::vector<u8> public_key;
    std::vector<u8> key_signature;

    friend bool operator==(const ChatSessionData&, const ChatSessionData&) = default;
};

/// One player's record. Only the fields of the packet's actions are
/// meaningful; the others keep their defaults.
struct PlayerInfoEntry {
    Uuid uuid{};
    // Add Player.
    std::string                  name;
    std::vector<ProfileProperty> properties;
    // Initialize Chat: absent when "Has Signature Data" is false.
    std::optional<ChatSessionData> chat;
    // Update Game Mode: 0 survival, 1 creative, 2 adventure, 3 spectator.
    i32 game_mode{0};
    // Update Listed.
    bool listed{false};
    // Update Latency, in milliseconds.
    i32 latency{0};
    // Update Display Name: a chat component as JSON, absent for "the name".
    std::optional<std::string> display_name_json;

    friend bool operator==(const PlayerInfoEntry&, const PlayerInfoEntry&) = default;
};

struct PlayerInfoUpdate {
    u8                           actions{0};
    std::vector<PlayerInfoEntry> entries;

    friend bool operator==(const PlayerInfoUpdate&, const PlayerInfoUpdate&) = default;
};

[[nodiscard]] std::vector<u8> encode_player_info_update(const PlayerInfoUpdate& update);
[[nodiscard]] std::optional<PlayerInfoUpdate> parse_player_info_update(std::span<const u8> payload);

/// Player Info Remove: a count, then the UUIDs.
[[nodiscard]] std::vector<u8> encode_player_info_remove_many(std::span<const Uuid> uuids);
[[nodiscard]] std::optional<std::vector<Uuid>> parse_player_info_remove(std::span<const u8> payload);

/// Set Tab List Header And Footer: two chat components. `{"text":""}` removes
/// a line (the page's words); the client decides what "empty" looks like.
struct TabListHeaderFooter {
    std::string header_json;
    std::string footer_json;

    friend bool operator==(const TabListHeaderFooter&, const TabListHeaderFooter&) = default;
};

[[nodiscard]] std::vector<u8> encode_tab_list_header_footer(const TabListHeaderFooter& texts);
[[nodiscard]] std::optional<TabListHeaderFooter> parse_tab_list_header_footer(
    std::span<const u8> payload);

/// Open Horse Screen: window id (unsigned byte), slot count (VarInt), the
/// horse's entity id (Int, not a VarInt).
struct OpenHorseScreen {
    u8  window_id{0};
    i32 slot_count{0};
    i32 entity_id{0};

    friend bool operator==(const OpenHorseScreen&, const OpenHorseScreen&) = default;
};

[[nodiscard]] std::vector<u8> encode_open_horse_screen(const OpenHorseScreen& screen);
[[nodiscard]] std::optional<OpenHorseScreen> parse_open_horse_screen(std::span<const u8> payload);

}  // namespace ov::net
