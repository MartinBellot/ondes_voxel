// The server list ping.
//
// Before anything else, a client asks a server what it is. That exchange is
// four packets and no authentication, and it is the first thing a real client
// will do to us — which makes it the cheapest possible proof that the framing,
// the VarInts and the string encoding are all right.
//
//   client -> Handshake       protocol version, address, port, next state
//   client -> Status Request  empty
//   server -> Status Response a JSON document
//   client -> Ping Request    an arbitrary long
//   server -> Pong Response   the same long, echoed
//
// The JSON is not free-form: a client that cannot parse it shows the server as
// unreachable rather than reporting an error, so a missing field looks exactly
// like a network problem.
#pragma once

#include "ov/base/types.hpp"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ov::net {

/// Protocol version 763 is Minecraft 1.20 and 1.20.1.
inline constexpr i32         kProtocolVersion = 763;
inline constexpr const char* kVersionName     = "1.20.1";

/// What the server list shows.
struct ServerStatus {
    std::string version_name{kVersionName};
    i32         protocol{kProtocolVersion};

    i32 max_players{20};
    i32 online_players{0};

    /// Names shown when hovering over the player count. Vanilla shows at most
    /// a handful.
    std::vector<std::string> sample;

    /// The text under the server name.
    std::string description{"Ondes VOXEL"};

    /// A 64x64 PNG as a data URI, or empty for none. A client silently ignores
    /// an icon of the wrong size rather than complaining.
    std::string favicon;

    /// 1.19.1 added this. Reported false: chat signing is not implemented, and
    /// claiming otherwise makes a client expect signatures it will not get.
    bool enforces_secure_chat{false};

    /// Serialize to the JSON the client expects.
    ///
    /// Hand-written rather than routed through a JSON library: this is one
    /// small document of a fixed shape, and ov_data does not exist yet.
    [[nodiscard]] std::string to_json() const;
};

/// Packet ids in the Status state. Two in each direction, and the request and
/// response share an id — the direction disambiguates them.
enum class StatusPacket : i32 {
    Request  = 0x00,  // client -> server
    Response = 0x00,  // server -> client
    Ping     = 0x01,  // client -> server
    Pong     = 0x01,  // server -> client
};

/// What a Handshake asked for next.
enum class NextState : i32 {
    Status = 1,
    Login  = 2,
};

/// The first packet of any connection.
struct Handshake {
    i32         protocol_version{0};
    std::string server_address;
    u16         server_port{0};
    NextState   next_state{NextState::Status};
};

/// Parse a Handshake body. Returns nullopt when it is malformed.
[[nodiscard]] std::optional<Handshake> parse_handshake(std::span<const u8> body);

/// Encode a Status Response body: one JSON string.
[[nodiscard]] std::vector<u8> encode_status_response(const ServerStatus& status);

/// Encode a Pong body: the client's long, echoed unchanged.
[[nodiscard]] std::vector<u8> encode_pong(i64 payload);

}  // namespace ov::net
