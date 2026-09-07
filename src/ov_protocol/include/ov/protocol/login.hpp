// The login sequence, offline mode.
//
// Ondes VOXEL does not authenticate against Mojang, so the exchange is short:
//
//   client -> Login Start      name, and optionally the uuid the client knows
//   server -> Set Compression  optional, and it changes the framing immediately
//   server -> Login Success    the identity the server assigns
//   ... state becomes Play
//
// Two things absent here that a Mojang-authenticating server would need, and
// their absence is a decision rather than an omission: no Encryption Request or
// Response, and no call to the session server. See docs/ARCHITECTURE.md § 8.
//
// The consequence is worth stating where someone will read it: anyone may
// connect under any name. That is what an offline server is, it is appropriate
// for a local or trusted network, and it is not appropriate for a public one.
//
// One ordering trap: Set Compression takes effect from the *next* packet in
// both directions. Sending it and then writing Login Success with the old
// framing desynchronises the client immediately, and the symptom is a
// disconnect with no useful message.
#pragma once

#include "ov/base/types.hpp"
#include "ov/protocol/types.hpp"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ov::net {

/// Packet ids in the Login state.
enum class LoginPacket : i32 {
    // client -> server
    Start = 0x00,

    // server -> client
    Disconnect     = 0x00,
    SetCompression = 0x03,
    Success        = 0x02,
};

/// Vanilla's limit on a player name.
inline constexpr u32 kMaxPlayerNameLength = 16;

struct LoginStart {
    std::string name;
    /// 1.19.3 added an optional uuid the client already believes it has. It is
    /// advisory: an offline server assigns the identity itself, since trusting
    /// a client-supplied uuid would let anyone claim anyone else's data.
    std::optional<Uuid> claimed_uuid;
};

/// Parse a Login Start body. Returns nullopt when it is malformed, or when the
/// name is empty or over the limit.
[[nodiscard]] std::optional<LoginStart> parse_login_start(std::span<const u8> body);

/// Encode Login Success: the identity the server assigns, plus an empty
/// property list. Properties carry skin and cape data from Mojang, which an
/// offline server has none of.
[[nodiscard]] std::vector<u8> encode_login_success(const Uuid& uuid, std::string_view name);

/// Encode Set Compression. A negative threshold disables it.
[[nodiscard]] std::vector<u8> encode_set_compression(i32 threshold);

/// Encode a Login Disconnect: a chat component, as JSON.
///
/// Login is the one place a server can tell a client *why* it was refused, and
/// the message is shown verbatim. Anywhere later, a disconnect is just a
/// disconnect.
[[nodiscard]] std::vector<u8> encode_login_disconnect(std::string_view reason);

/// Whether a name is acceptable offline.
///
/// Vanilla allows 3 to 16 characters of [A-Za-z0-9_]. The rule is enforced here
/// because the name becomes a UUID, a file name and a scoreboard entry, and
/// each of those has its own opinion about what a name may contain.
[[nodiscard]] bool is_valid_player_name(std::string_view name) noexcept;

}  // namespace ov::net
