// Chat, commands and the text the server shows — protocol 763.
//
// Every field here was read off the archived protocol page for 1.20.1
// (`Minecraft_Wiki:Projects/wiki.vg_merge/Protocol?oldid=2773082`) and then
// **checked against a real server**: `scripts/capture_commands.py` joins the
// vanilla jar with a probe that sends these packets, and the jar would have
// disconnected it on the first field read wrong. The ids agree with the ones
// the rest of `play.hpp` was captured with.
//
// Offline mode means no signatures are *checked*, but a vanilla client that has
// a Microsoft account signs anyway: a Chat Message then carries 256 bytes of
// signature and a Chat Command one per signed argument. They are decoded in
// full and ignored, because a parser that stopped at the text would read the
// signature as the next packet.
//
// Chat components travel as JSON strings in this version (NBT arrived in
// 1.20.3). The encoders take the JSON already written: building it is the
// command engine's business, and this layer only frames it.
#pragma once

#include "ov/base/types.hpp"
#include "ov/protocol/play.hpp"
#include "ov/protocol/types.hpp"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ov::net {

namespace clientbound {
inline constexpr i32 kChangeDifficulty      = 0x0C;
inline constexpr i32 kClearTitles           = 0x0E;
inline constexpr i32 kCommandSuggestions    = 0x0F;
inline constexpr i32 kCommands              = 0x10;
inline constexpr i32 kDisguisedChat         = 0x1B;
inline constexpr i32 kPlayerChat            = 0x35;
inline constexpr i32 kUpdateSectionBlocks   = 0x43;
inline constexpr i32 kServerData            = 0x45;
inline constexpr i32 kSetActionBarText      = 0x46;
inline constexpr i32 kSetSubtitleText       = 0x5D;
inline constexpr i32 kSetTitleText          = 0x5F;
inline constexpr i32 kSetTitleAnimationTimes = 0x60;
inline constexpr i32 kSystemChat            = 0x64;
}  // namespace clientbound

namespace serverbound {
inline constexpr i32 kChangeDifficulty          = 0x02;
inline constexpr i32 kMessageAcknowledgment     = 0x03;
inline constexpr i32 kChatCommand               = 0x04;
inline constexpr i32 kChatMessage               = 0x05;
inline constexpr i32 kPlayerSession             = 0x06;
inline constexpr i32 kCommandSuggestionsRequest = 0x09;
}  // namespace serverbound

/// The length limits the protocol page gives, in UTF-16 code units.
inline constexpr u32 kMaxChatMessageLength = 256;
inline constexpr u32 kMaxChatComponentLength = 262144;
inline constexpr u32 kMaxSuggestionRequestLength = 32500;

/// Every signature in the chat system is an RSA-2048 signature: 256 bytes,
/// never length-prefixed.
using ChatSignature = std::array<u8, 256>;

/// "Fixed BitSet (20)" — the last-seen acknowledgements, three bytes.
using AcknowledgedBits = std::array<u8, 3>;

// ── Serverbound ─────────────────────────────────────────────────────────────

/// A command typed in chat, without its leading slash.
struct ChatCommand {
    struct ArgumentSignature {
        std::string   name;
        ChatSignature signature{};
    };

    std::string                    command;
    i64                            timestamp{0};
    i64                            salt{0};
    std::vector<ArgumentSignature> signatures;
    i32                            message_count{0};
    AcknowledgedBits               acknowledged{};
};

[[nodiscard]] std::optional<ChatCommand> parse_chat_command(std::span<const u8> payload);
[[nodiscard]] std::vector<u8>            encode_chat_command(const ChatCommand& command);

/// A line of chat.
struct ChatMessage {
    std::string                  message;
    i64                          timestamp{0};
    i64                          salt{0};
    std::optional<ChatSignature> signature;
    i32                          message_count{0};
    AcknowledgedBits             acknowledged{};
};

[[nodiscard]] std::optional<ChatMessage> parse_chat_message(std::span<const u8> payload);
[[nodiscard]] std::vector<u8>            encode_chat_message(const ChatMessage& message);

/// Message Acknowledgment: how many messages the client has seen.
[[nodiscard]] std::optional<i32> parse_message_acknowledgment(std::span<const u8> payload);

/// The client asks what could complete the text before its cursor.
struct SuggestionsRequest {
    i32         transaction{0};
    std::string text;
};

[[nodiscard]] std::optional<SuggestionsRequest> parse_suggestions_request(
    std::span<const u8> payload);
[[nodiscard]] std::vector<u8> encode_suggestions_request(const SuggestionsRequest& request);

/// Change Difficulty, serverbound: the singleplayer difficulty button.
[[nodiscard]] std::optional<u8> parse_change_difficulty(std::span<const u8> payload);

// ── Clientbound ─────────────────────────────────────────────────────────────

/// System Chat Message: a component, in the chat box or above the hotbar.
[[nodiscard]] std::vector<u8> encode_system_chat(std::string_view component_json, bool overlay);

struct SystemChat {
    std::string content;
    bool        overlay{false};
};

[[nodiscard]] std::optional<SystemChat> parse_system_chat(std::span<const u8> payload);

/// Player Chat Message, unsigned: no signature, no previous messages, not
/// filtered. `chat_type` is an index into the `minecraft:chat_type` registry
/// **as this server's codec numbers it**, which is ours and not Mojang's.
struct PlayerChat {
    Uuid                       sender{};
    i32                        index{0};
    std::optional<ChatSignature> signature;
    std::string                body;
    i64                        timestamp{0};
    i64                        salt{0};
    std::optional<std::string> unsigned_content;
    i32                        chat_type{0};
    std::string                name_json;
    std::optional<std::string> target_json;
};

[[nodiscard]] std::vector<u8>           encode_player_chat(const PlayerChat& chat);
[[nodiscard]] std::optional<PlayerChat> parse_player_chat(std::span<const u8> payload);

/// Disguised Chat Message: a chat type applied to a message with no sender.
struct DisguisedChat {
    std::string                message_json;
    i32                        chat_type{0};
    std::string                name_json;
    std::optional<std::string> target_json;
};

[[nodiscard]] std::vector<u8>              encode_disguised_chat(const DisguisedChat& chat);
[[nodiscard]] std::optional<DisguisedChat> parse_disguised_chat(std::span<const u8> payload);

/// One completion. The text replaces `[start, start + length)` of the request.
struct Suggestion {
    std::string                text;
    std::optional<std::string> tooltip_json;

    friend bool operator==(const Suggestion&, const Suggestion&) = default;
};

struct SuggestionsResponse {
    i32                     transaction{0};
    i32                     start{0};
    i32                     length{0};
    std::vector<Suggestion> matches;
};

[[nodiscard]] std::vector<u8> encode_suggestions_response(const SuggestionsResponse& response);
[[nodiscard]] std::optional<SuggestionsResponse> parse_suggestions_response(
    std::span<const u8> payload);

/// One node of the Commands graph as it travels.
///
/// `properties` holds the parser's properties **already encoded**: their shape
/// depends on the parser, and the parser is the command engine's to know.
/// `parse_commands` below does know every shape of 1.20.1, because a client has
/// to — an unknown parser makes the rest of the packet unreadable.
struct CommandNodeWire {
    /// Bits 0-1 the node type (0 root, 1 literal, 2 argument), 0x04
    /// executable, 0x08 has redirect, 0x10 has suggestions type.
    u8               flags{0};
    std::vector<i32> children;
    i32              redirect{-1};
    std::string      name;
    i32              parser{-1};
    std::vector<u8>  properties;
    std::string      suggestions;

    friend bool operator==(const CommandNodeWire&, const CommandNodeWire&) = default;
};

namespace command_flags {
inline constexpr u8 kRoot           = 0x00;
inline constexpr u8 kLiteral        = 0x01;
inline constexpr u8 kArgument       = 0x02;
inline constexpr u8 kTypeMask       = 0x03;
inline constexpr u8 kExecutable     = 0x04;
inline constexpr u8 kHasRedirect    = 0x08;
inline constexpr u8 kHasSuggestions = 0x10;
}  // namespace command_flags

struct CommandGraphWire {
    std::vector<CommandNodeWire> nodes;
    i32                          root{0};
};

[[nodiscard]] std::vector<u8> encode_commands(const CommandGraphWire& graph);

/// Read a Commands packet back, properties included. Nullopt for anything
/// malformed, for an unknown parser id, and for a child or redirect index that
/// points outside the node array — which a vanilla client also refuses.
[[nodiscard]] std::optional<CommandGraphWire> parse_commands(std::span<const u8> payload);

/// Server Data: the MOTD, the icon, and whether chat must be signed.
[[nodiscard]] std::vector<u8> encode_server_data(std::string_view motd_json,
                                                 std::span<const u8> icon_png,
                                                 bool enforces_secure_chat);

/// Change Difficulty, clientbound. 0 peaceful … 3 hard.
[[nodiscard]] std::vector<u8> encode_change_difficulty(u8 difficulty, bool locked);

/// Set Title Text, Set Subtitle Text and Set Action Bar Text share one body: a
/// single component.
[[nodiscard]] std::vector<u8> encode_component_packet(std::string_view component_json);

[[nodiscard]] std::vector<u8> encode_title_animation_times(i32 fade_in, i32 stay, i32 fade_out);
[[nodiscard]] std::vector<u8> encode_clear_titles(bool reset);

/// One block of an Update Section Blocks packet.
struct SectionBlock {
    u8  x{0};  // 0..15, local to the section
    u8  y{0};
    u8  z{0};
    i32 state{0};

    friend bool operator==(const SectionBlock&, const SectionBlock&) = default;
};

/// Update Section Blocks: many changes inside one 16×16×16 section.
[[nodiscard]] std::vector<u8> encode_update_section_blocks(i32 section_x, i32 section_y,
                                                           i32 section_z,
                                                           std::span<const SectionBlock> blocks);

struct SectionBlocks {
    i32                       section_x{0};
    i32                       section_y{0};
    i32                       section_z{0};
    std::vector<SectionBlock> blocks;
};

[[nodiscard]] std::optional<SectionBlocks> parse_update_section_blocks(
    std::span<const u8> payload);

/// Player Info Update with only the Update Game Mode action (mask 0x04).
[[nodiscard]] std::vector<u8> encode_player_info_game_mode(const Uuid& uuid, i32 game_mode);

// ── What commands make happen ───────────────────────────────────────────────

namespace clientbound {
inline constexpr i32 kWorldEvent = 0x25;
inline constexpr i32 kLookAt     = 0x3B;
}  // namespace clientbound

/// World Event: 2001 is "block broken", with the state id as data — the
/// particles and sound `setblock … destroy` and `fill … destroy` make.
inline constexpr i32 kWorldEventBlockBreak = 2001;

[[nodiscard]] std::vector<u8> encode_world_event(i32 event, WirePosition position, i32 data,
                                                 bool global);

/// Look At: turn the client to face a point. `eyes` false aims the feet, as
/// `tp … facing` does.
[[nodiscard]] std::vector<u8> encode_look_at(bool eyes, f64 x, f64 y, f64 z);

/// Synchronize Position with relative flags (0x01 x, 0x02 y, 0x04 z, 0x08 yaw,
/// 0x10 pitch): a flagged field is an offset, which is how vanilla sends a
/// `tp ~ ~5 ~` — the capture shows flags 0x1F and a y of 5.
[[nodiscard]] std::vector<u8> encode_synchronize_position_relative(f64 x, f64 y, f64 z, f32 yaw,
                                                                   f32 pitch, u8 flags,
                                                                   i32 teleport_id);

}  // namespace ov::net
