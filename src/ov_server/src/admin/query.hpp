// Query: GameSpy 4 over UDP, as the `Query` page documents it.
//
//     request:  0xFE 0xFD | type | session id (int32 BE) | payload
//
// Type 9 is the handshake: the answer is a challenge token, as decimal text.
// Type 0 with the token is the basic stat; with the token and four more bytes
// it is the full stat. A packet with a wrong or stale token gets no answer.
//
// The answers are built from a snapshot the tick thread refreshes, so this
// thread never reads a player table. asio stays inside query.cpp.
#pragma once

#include "ov/base/types.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ov::server::admin {

/// What the answers say.
struct QueryInfo {
    std::string              motd;
    std::string              game_type{"SMP"};
    std::string              game_id{"MINECRAFT"};
    std::string              version{"1.20.1"};
    std::string              plugins;
    std::string              map{"world"};
    std::vector<std::string> players;
    i32                      max_players{20};
    u16                      host_port{25565};
    std::string              host_ip{"0.0.0.0"};
};

/// The protocol without the socket, so it has tests. Tokens come from the
/// caller's generator; they are remembered per sender for 30 seconds.
class QueryResponder {
public:
    explicit QueryResponder(std::function<i32()> next_token) : next_token_{std::move(next_token)} {}

    /// The answer to one datagram from `sender` at `now_ms`, or none.
    [[nodiscard]] std::optional<std::vector<u8>> answer(std::span<const u8> request,
                                                        const std::string&  sender, i64 now_ms,
                                                        const QueryInfo& info);

private:
    struct Challenge {
        std::string sender;
        i32         token{0};
        i64         created_ms{0};
    };
    std::function<i32()>   next_token_;
    std::vector<Challenge> challenges_;
};

[[nodiscard]] std::vector<u8> query_basic_stat(i32 session, const QueryInfo& info);
[[nodiscard]] std::vector<u8> query_full_stat(i32 session, const QueryInfo& info);

class QueryServer {
public:
    virtual ~QueryServer() = default;

    /// Serve on `port`. `info` is called on the query thread and must be
    /// safe there (the server hands it a copy under its own lock).
    [[nodiscard]] static std::unique_ptr<QueryServer> start(u16 port, const std::string& address,
                                                            std::function<QueryInfo()> info);
};

}  // namespace ov::server::admin
