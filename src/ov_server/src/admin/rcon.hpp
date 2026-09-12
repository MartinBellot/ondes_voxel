// RCON: the console over TCP, as the `RCON` page documents it.
//
//     int32 LE length | int32 LE request id | int32 LE type | body | 0x00 0x00
//
// Type 3 logs in (the answer echoes the id, or -1 for a wrong password), type
// 2 runs a command; answers are type 0, their body cut into pieces of at most
// 4096 bytes, one packet each. The commands run on the tick thread like any
// console line — this thread only waits for their words.
//
// asio stays inside rcon.cpp, so this header costs nothing to include.
#pragma once

#include "ov/base/types.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ov::server::admin {

/// The packet codec, separate from the socket so it has tests.
struct RconPacket {
    i32         request{0};
    i32         type{0};
    std::string body;
};

inline constexpr i32 kRconLogin    = 3;
inline constexpr i32 kRconCommand  = 2;
inline constexpr i32 kRconAuthOk   = 2;
inline constexpr i32 kRconResponse = 0;

/// Encode one packet.
[[nodiscard]] std::vector<u8> encode_rcon(const RconPacket& packet);

/// The packets an answer is sent as: 4096 bytes of body at most in each, one
/// empty packet for an empty answer.
[[nodiscard]] std::vector<RconPacket> split_rcon_response(i32 request, std::string_view text);

/// What the server does with one packet read from an authenticated or not
/// connection. `authenticated` is updated; `run` executes a command and
/// returns its words.
struct RconSession {
    std::string                                 password;
    bool                                        authenticated{false};
    std::function<std::string(std::string)>     run;

    [[nodiscard]] std::vector<RconPacket> handle(const RconPacket& packet);
};

/// Parse one whole packet (length prefix included). Nullopt when the bytes
/// are not a packet: too short, a length that disagrees, a body without its
/// terminator.
[[nodiscard]] std::optional<RconPacket> decode_rcon(std::span<const u8> bytes);

class RconServer {
public:
    virtual ~RconServer() = default;

    /// Listen on `port` (every interface when `address` is empty) and serve
    /// on a thread of its own. Null, with a logged reason, when the port
    /// cannot be bound.
    [[nodiscard]] static std::unique_ptr<RconServer> start(
        u16 port, const std::string& address, std::string password,
        std::function<std::string(std::string)> run);
};

}  // namespace ov::server::admin
