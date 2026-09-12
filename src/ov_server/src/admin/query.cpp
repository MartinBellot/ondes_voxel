#include "query.hpp"

#include "ov/base/log.hpp"
#include "ov/base/thread.hpp"

#include <asio.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>

namespace ov::server::admin {
namespace {

constexpr u8  kHandshake    = 9;
constexpr u8  kStat         = 0;
constexpr i64 kChallengeTtl = 30'000;

void put_session(std::vector<u8>& out, i32 session) {
    const auto u = static_cast<u32>(session);
    out.push_back(static_cast<u8>(u >> 24));
    out.push_back(static_cast<u8>(u >> 16));
    out.push_back(static_cast<u8>(u >> 8));
    out.push_back(static_cast<u8>(u));
}

/// The jar's strings go out as ISO-8859-1 (measured: "é" is the one byte
/// 0xE9); a character outside it becomes '?', as String.getBytes does.
[[nodiscard]] std::string latin1(std::string_view utf8) {
    std::string out;
    usize       i = 0;
    while (i < utf8.size()) {
        const auto c   = static_cast<u8>(utf8[i]);
        u32        cp  = c;
        usize      len = 1;
        if (c >= 0xF0 && i + 3 < utf8.size()) {
            cp  = 0x10000;
            len = 4;
        } else if (c >= 0xE0 && i + 2 < utf8.size()) {
            cp  = ((c & 0x0FU) << 12) | ((static_cast<u8>(utf8[i + 1]) & 0x3FU) << 6) |
                 (static_cast<u8>(utf8[i + 2]) & 0x3FU);
            len = 3;
        } else if (c >= 0xC0 && i + 1 < utf8.size()) {
            cp  = ((c & 0x1FU) << 6) | (static_cast<u8>(utf8[i + 1]) & 0x3FU);
            len = 2;
        }
        out.push_back(cp <= 0xFF ? static_cast<char>(cp) : '?');
        i += len;
    }
    return out;
}

void put_cstring(std::vector<u8>& out, std::string_view text) {
    const std::string bytes = latin1(text);
    out.insert(out.end(), bytes.begin(), bytes.end());
    out.push_back(0);
}

[[nodiscard]] i32 get_i32_be(std::span<const u8> bytes, usize at) {
    return static_cast<i32>((static_cast<u32>(bytes[at]) << 24) |
                            (static_cast<u32>(bytes[at + 1]) << 16) |
                            (static_cast<u32>(bytes[at + 2]) << 8) | static_cast<u32>(bytes[at + 3]));
}

}  // namespace

std::vector<u8> query_basic_stat(i32 session, const QueryInfo& info) {
    std::vector<u8> out;
    out.push_back(kStat);
    put_session(out, session);
    put_cstring(out, info.motd);
    put_cstring(out, info.game_type);
    put_cstring(out, info.map);
    put_cstring(out, std::to_string(info.players.size()));
    put_cstring(out, std::to_string(info.max_players));
    // The port is the one little-endian number of the protocol.
    out.push_back(static_cast<u8>(info.host_port & 0xFF));
    out.push_back(static_cast<u8>(info.host_port >> 8));
    put_cstring(out, info.host_ip);
    return out;
}

std::vector<u8> query_full_stat(i32 session, const QueryInfo& info) {
    std::vector<u8> out;
    out.push_back(kStat);
    put_session(out, session);
    put_cstring(out, "splitnum");
    out.push_back(0x80);
    out.push_back(0);
    const std::array<std::pair<std::string_view, std::string>, 10> pairs{{
        {"hostname", info.motd},
        {"gametype", info.game_type},
        {"game_id", info.game_id},
        {"version", info.version},
        {"plugins", info.plugins},
        {"map", info.map},
        {"numplayers", std::to_string(info.players.size())},
        {"maxplayers", std::to_string(info.max_players)},
        {"hostport", std::to_string(info.host_port)},
        {"hostip", info.host_ip},
    }};
    for (const auto& [key, value] : pairs) {
        put_cstring(out, key);
        put_cstring(out, value);
    }
    out.push_back(0);
    out.push_back(1);
    put_cstring(out, "player_");
    out.push_back(0);
    for (const std::string& name : info.players) {
        put_cstring(out, name);
    }
    out.push_back(0);
    return out;
}

std::optional<std::vector<u8>> QueryResponder::answer(std::span<const u8> request,
                                                      const std::string& sender, i64 now_ms,
                                                      const QueryInfo& info) {
    std::erase_if(challenges_,
                  [&](const Challenge& c) { return now_ms - c.created_ms >= kChallengeTtl; });
    if (request.size() < 7 || request[0] != 0xFE || request[1] != 0xFD) {
        return std::nullopt;
    }
    const u8  type    = request[2];
    const i32 session = get_i32_be(request, 3);
    if (type == kHandshake) {
        const i32 token = next_token_();
        std::erase_if(challenges_, [&](const Challenge& c) { return c.sender == sender; });
        challenges_.push_back(Challenge{sender, token, now_ms});
        std::vector<u8> out;
        out.push_back(kHandshake);
        put_session(out, session);
        put_cstring(out, std::to_string(token));
        return out;
    }
    if (type != kStat || request.size() < 11) {
        return std::nullopt;
    }
    const i32  token = get_i32_be(request, 7);
    const auto it    = std::ranges::find_if(challenges_, [&](const Challenge& c) {
        return c.sender == sender && c.token == token;
    });
    if (it == challenges_.end()) {
        return std::nullopt;
    }
    // Four more bytes after the token ask for the full stat.
    if (request.size() >= 15) {
        return query_full_stat(session, info);
    }
    return query_basic_stat(session, info);
}

namespace {

class AsioQuery final : public QueryServer {
public:
    AsioQuery(u16 port, const std::string& address, std::function<QueryInfo()> info)
        : socket_{io_}, info_{std::move(info)}, responder_{[this] { return next_token(); }} {
        const asio::ip::address bind_to =
            address.empty() ? asio::ip::address{asio::ip::address_v4::any()}
                            : asio::ip::make_address(address);
        const asio::ip::udp::endpoint endpoint{bind_to, port};
        socket_.open(endpoint.protocol());
        socket_.bind(endpoint);
        receive();
        thread_ = std::thread{[this] {
            set_thread_role("ov-query", ThreadRole::Io);
            io_.run();
        }};
    }

    ~AsioQuery() override {
        io_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    i32 next_token() {
        // A token only has to be unguessable enough that a spoofed sender
        // cannot answer it; vanilla draws one below 2^24.
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<i32>((state_ >> 33) % 16777216ULL);
    }

    void receive() {
        socket_.async_receive_from(
            asio::buffer(buffer_), sender_, [this](std::error_code ec, std::size_t got) {
                if (ec == asio::error::operation_aborted) {
                    return;
                }
                if (!ec) {
                    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count();
                    const std::string who =
                        sender_.address().to_string() + ":" + std::to_string(sender_.port());
                    if (auto out = responder_.answer(std::span<const u8>{buffer_.data(), got}, who,
                                                     now, info_())) {
                        std::error_code ignored;
                        socket_.send_to(asio::buffer(*out), sender_, 0, ignored);
                    }
                }
                receive();
            });
    }

    asio::io_context           io_;
    asio::ip::udp::socket      socket_;
    asio::ip::udp::endpoint    sender_;
    std::array<u8, 1460>       buffer_{};
    std::function<QueryInfo()> info_;
    u64                        state_{static_cast<u64>(
        std::chrono::steady_clock::now().time_since_epoch().count())};
    QueryResponder             responder_;
    std::thread                thread_;
};

}  // namespace

std::string local_address() {
    // What the jar reports as hostip when server-ip is empty: this machine's
    // own address (measured: its LAN address, not 0.0.0.0) — the first IPv4
    // address its host name resolves to.
    try {
        asio::io_context        io;
        asio::ip::tcp::resolver resolver{io};
        for (const auto& entry : resolver.resolve(asio::ip::host_name(), "")) {
            if (const auto address = entry.endpoint().address(); address.is_v4()) {
                return address.to_string();
            }
        }
    } catch (const std::exception&) {
    }
    return "0.0.0.0";
}

std::unique_ptr<QueryServer> QueryServer::start(u16 port, const std::string& address,
                                                std::function<QueryInfo()> info) {
    try {
        return std::make_unique<AsioQuery>(port, address, std::move(info));
    } catch (const std::exception& e) {
        OV_LOG_ERROR("could not start Query on port {}: {}", port, e.what());
        return nullptr;
    }
}

}  // namespace ov::server::admin
