#include "rcon.hpp"

#include "ov/base/log.hpp"
#include "ov/base/thread.hpp"

#include <asio.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <deque>
#include <thread>

namespace ov::server::admin {
namespace {

void put_i32(std::vector<u8>& out, i32 value) {
    const auto u = static_cast<u32>(value);
    out.push_back(static_cast<u8>(u & 0xFF));
    out.push_back(static_cast<u8>((u >> 8) & 0xFF));
    out.push_back(static_cast<u8>((u >> 16) & 0xFF));
    out.push_back(static_cast<u8>((u >> 24) & 0xFF));
}

[[nodiscard]] i32 get_i32(std::span<const u8> bytes, usize at) {
    return static_cast<i32>(static_cast<u32>(bytes[at]) | (static_cast<u32>(bytes[at + 1]) << 8) |
                            (static_cast<u32>(bytes[at + 2]) << 16) |
                            (static_cast<u32>(bytes[at + 3]) << 24));
}

/// The largest packet a client may send, as the page states (the jar reads
/// into a buffer of this size).
constexpr usize kMaxRequest = 1460;
constexpr usize kMaxChunk   = 4096;

}  // namespace

std::vector<u8> encode_rcon(const RconPacket& packet) {
    std::vector<u8> out;
    out.reserve(14 + packet.body.size());
    put_i32(out, static_cast<i32>(10 + packet.body.size()));
    put_i32(out, packet.request);
    put_i32(out, packet.type);
    out.insert(out.end(), packet.body.begin(), packet.body.end());
    out.push_back(0);
    out.push_back(0);
    return out;
}

std::vector<RconPacket> split_rcon_response(i32 request, std::string_view text) {
    std::vector<RconPacket> out;
    do {
        const usize n = std::min(kMaxChunk, text.size());
        out.push_back(RconPacket{request, kRconResponse, std::string{text.substr(0, n)}});
        text.remove_prefix(n);
    } while (!text.empty());
    return out;
}

std::optional<RconPacket> decode_rcon(std::span<const u8> bytes) {
    if (bytes.size() < 14) {
        return std::nullopt;
    }
    const i32 length = get_i32(bytes, 0);
    if (length < 10 || static_cast<usize>(length) + 4 != bytes.size()) {
        return std::nullopt;
    }
    RconPacket packet;
    packet.request = get_i32(bytes, 4);
    packet.type    = get_i32(bytes, 8);
    // The body is read up to its first NUL, as a C string.
    const auto body = bytes.subspan(12);
    usize      end  = 0;
    while (end < body.size() && body[end] != 0) {
        ++end;
    }
    if (end == body.size()) {
        return std::nullopt;
    }
    packet.body.assign(reinterpret_cast<const char*>(body.data()), end);
    return packet;
}

std::vector<RconPacket> RconSession::handle(const RconPacket& packet) {
    if (packet.type == kRconLogin) {
        if (!password.empty() && packet.body == password) {
            authenticated = true;
            return {RconPacket{packet.request, kRconAuthOk, ""}};
        }
        authenticated = false;
        return {RconPacket{-1, kRconAuthOk, ""}};
    }
    if (!authenticated) {
        return {RconPacket{-1, kRconAuthOk, ""}};
    }
    if (packet.type == kRconCommand) {
        return split_rcon_response(packet.request, run ? run(packet.body) : std::string{});
    }
    char hex[16];
    std::snprintf(hex, sizeof hex, "%x", static_cast<u32>(packet.type));
    return split_rcon_response(packet.request, std::string{"Unknown request "} + hex);
}

namespace {

class Client : public std::enable_shared_from_this<Client> {
public:
    Client(asio::ip::tcp::socket socket, RconSession session)
        : socket_{std::move(socket)}, session_{std::move(session)} {}

    void start() { read_more(); }

private:
    void read_more() {
        auto self = shared_from_this();
        socket_.async_read_some(asio::buffer(chunk_), [self](std::error_code ec, std::size_t got) {
            if (ec) {
                return;
            }
            self->pending_.insert(self->pending_.end(), self->chunk_.begin(),
                                  self->chunk_.begin() + static_cast<std::ptrdiff_t>(got));
            if (!self->drain()) {
                std::error_code ignored;
                self->socket_.close(ignored);
                return;
            }
            self->read_more();
        });
    }

    /// Every whole packet in the buffer; false closes the connection.
    bool drain() {
        while (pending_.size() >= 4) {
            const i32 length = get_i32(pending_, 0);
            if (length < 10 || static_cast<usize>(length) + 4 > kMaxRequest) {
                return false;
            }
            if (pending_.size() < static_cast<usize>(length) + 4) {
                return true;  // the rest of it is on its way
            }
            const auto packet = decode_rcon(std::span<const u8>{pending_}.first(
                static_cast<usize>(length) + 4));
            pending_.erase(pending_.begin(), pending_.begin() + length + 4);
            if (!packet) {
                return false;
            }
            for (const RconPacket& answer : session_.handle(*packet)) {
                const auto bytes = encode_rcon(answer);
                std::error_code ec;
                asio::write(socket_, asio::buffer(bytes), ec);
                if (ec) {
                    return false;
                }
            }
        }
        return true;
    }

    asio::ip::tcp::socket  socket_;
    RconSession            session_;
    std::array<u8, 4096>   chunk_{};
    std::vector<u8>        pending_;
};

class AsioRcon final : public RconServer {
public:
    AsioRcon(u16 port, const std::string& address, std::string password,
             std::function<std::string(std::string)> run)
        : acceptor_{io_}, password_{std::move(password)}, run_{std::move(run)} {
        const asio::ip::address bind_to =
            address.empty() ? asio::ip::address{asio::ip::address_v4::any()}
                            : asio::ip::make_address(address);
        const asio::ip::tcp::endpoint endpoint{bind_to, port};
        acceptor_.open(endpoint.protocol());
        acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true));
        acceptor_.bind(endpoint);
        acceptor_.listen();
        accept();
        thread_ = std::thread{[this] {
            set_thread_role("ov-rcon", ThreadRole::Io);
            io_.run();
        }};
    }

    ~AsioRcon() override {
        io_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    void accept() {
        acceptor_.async_accept([this](std::error_code ec, asio::ip::tcp::socket socket) {
            if (ec) {
                return;
            }
            std::error_code endpoint_ec;
            const auto      peer = socket.remote_endpoint(endpoint_ec);
            OV_LOG_INFO("Thread RCON Client /{} started",
                        endpoint_ec ? std::string{"?"} : peer.address().to_string());
            std::make_shared<Client>(std::move(socket), RconSession{password_, false, run_})->start();
            accept();
        });
    }

    asio::io_context                        io_;
    asio::ip::tcp::acceptor                 acceptor_;
    std::string                             password_;
    std::function<std::string(std::string)> run_;
    std::thread                             thread_;
};

}  // namespace

std::unique_ptr<RconServer> RconServer::start(u16 port, const std::string& address,
                                              std::string                             password,
                                              std::function<std::string(std::string)> run) {
    try {
        return std::make_unique<AsioRcon>(port, address, std::move(password), std::move(run));
    } catch (const std::exception& e) {
        OV_LOG_ERROR("could not start RCON on port {}: {}", port, e.what());
        return nullptr;
    }
}

}  // namespace ov::server::admin
