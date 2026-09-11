#define OV_LOG_CATEGORY "net"

#include "ov/protocol/listener.hpp"

#include "ov/base/log.hpp"
#include "ov/protocol/framing.hpp"

#include <asio.hpp>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <system_error>

namespace ov::net {
namespace {

/// Idle connections are dropped after this long.
///
/// A connection that opens and says nothing costs a socket and a buffer. Left
/// unbounded, opening thousands of them is the cheapest denial of service there
/// is, and it needs no protocol knowledge at all.
constexpr std::chrono::seconds kHandshakeTimeout{30};

class AsioConnection;

using AsioConnectionPtr = std::shared_ptr<AsioConnection>;

class AsioListener;

class AsioConnection final : public Connection,
                             public std::enable_shared_from_this<AsioConnection> {
public:
    AsioConnection(asio::ip::tcp::socket socket, AsioListener& listener)
        : socket_{std::move(socket)}, listener_{listener}, timer_{socket_.get_executor()} {}

    void start();

    void send(std::span<const u8> bytes) override;
    void close() override;

    [[nodiscard]] std::string peer_address() const override {
        std::error_code ec;
        const auto      endpoint = socket_.remote_endpoint(ec);
        if (ec) {
            return "<disconnected>";
        }
        return endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
    }

    void set_compression_threshold(i32 threshold) override {
        decoder_.set_compression_threshold(threshold);
        write_threshold_ = threshold;
    }

private:
    void read_more();
    void write_next();
    void arm_timeout();
    void finish();

    asio::ip::tcp::socket       socket_;
    AsioListener&               listener_;
    asio::steady_timer          timer_;
    std::array<u8, 16384>       read_buffer_{};
    FrameDecoder                decoder_;
    std::deque<std::vector<u8>> write_queue_;
    bool                        writing_{false};
    bool                        closed_{false};
    i32                         write_threshold_{kNoCompression};
};

class AsioListener final : public Listener {
public:
    AsioListener(u16 port, const std::string& address) : acceptor_{context_} {
        const auto endpoint = address.empty()
                                  ? asio::ip::tcp::endpoint{asio::ip::tcp::v4(), port}
                                  : asio::ip::tcp::endpoint{asio::ip::make_address(address), port};

        acceptor_.open(endpoint.protocol());
        // Without this, restarting the server fails for a minute or two while
        // the old socket sits in TIME_WAIT — which reads as "port already in
        // use" and sends people hunting for a process that is not there.
        acceptor_.set_option(asio::socket_base::reuse_address{true});
        acceptor_.bind(endpoint);
        acceptor_.listen();
        port_ = acceptor_.local_endpoint().port();
    }

    void run() override {
        accept_next();
        context_.run();
    }

    void poll_for(i64 milliseconds) override {
        if (!accepting_) {
            accept_next();
        }
        context_.run_for(std::chrono::milliseconds{milliseconds});
    }

    void stop() override { context_.stop(); }

    [[nodiscard]] u16 port() const override { return port_; }

    [[nodiscard]] usize connection_count() const override {
        return connections_.load(std::memory_order_relaxed);
    }

    // Used by AsioConnection.
    asio::io_context& context() { return context_; }

    void connection_opened(const ConnectionPtr& connection) {
        connections_.fetch_add(1, std::memory_order_relaxed);
        if (on_connect_) {
            on_connect_(connection);
        }
    }

    void connection_closed(const ConnectionPtr& connection) {
        connections_.fetch_sub(1, std::memory_order_relaxed);
        if (on_disconnect_) {
            on_disconnect_(connection);
        }
    }

    [[nodiscard]] bool dispatch(const ConnectionPtr& connection, i32 packet_id,
                                std::span<const u8> body) {
        return on_packet_ ? on_packet_(connection, packet_id, body) : true;
    }

private:
    void accept_next() {
        accepting_ = true;
        acceptor_.async_accept([this](std::error_code ec, asio::ip::tcp::socket socket) {
            if (!ec) {
                auto connection = std::make_shared<AsioConnection>(std::move(socket), *this);
                connection->start();
            } else if (ec != asio::error::operation_aborted) {
                OV_LOG_WARN("accept failed: {}", ec.message());
            }

            if (acceptor_.is_open()) {
                accept_next();
            }
        });
    }

    asio::io_context        context_;
    asio::ip::tcp::acceptor acceptor_;
    u16                     port_{0};
    bool                    accepting_{false};
    std::atomic<usize>      connections_{0};
};

void AsioConnection::start() {
    listener_.connection_opened(shared_from_this());
    arm_timeout();
    read_more();
}

void AsioConnection::arm_timeout() {
    timer_.expires_after(kHandshakeTimeout);
    timer_.async_wait([self = shared_from_this()](std::error_code ec) {
        if (!ec) {
            // Fired rather than cancelled: the peer has said nothing for the
            // whole window.
            OV_LOG_WARN("{}: nothing read for {} s, closing", self->peer_address(),
                        kHandshakeTimeout.count());
            self->close();
        }
    });
}

void AsioConnection::read_more() {
    socket_.async_read_some(asio::buffer(read_buffer_), [self = shared_from_this()](
                                                            std::error_code ec, std::size_t bytes) {
        if (ec) {
            self->finish();
            return;
        }

        // Any traffic resets the idle timer.
        self->timer_.cancel();
        self->arm_timeout();

        self->decoder_.feed(std::span<const u8>{self->read_buffer_.data(), bytes});

        while (true) {
            auto packet = self->decoder_.next();
            if (!packet) {
                if (packet.error() == FrameError::Incomplete) {
                    break;  // normal: wait for more bytes
                }
                OV_LOG_WARN("{}: {}, closing", self->peer_address(), to_string(packet.error()));
                self->close();
                return;
            }

            // A byte stream cannot be resynchronised once a packet has been
            // misread, so a handler that rejects one closes the connection.
            if (!self->listener_.dispatch(self, packet->id, packet->body)) {
                OV_LOG_WARN("{}: packet 0x{:02X} ({} bytes) refused by its handler, closing",
                            self->peer_address(), packet->id, packet->body.size());
                self->close();
                return;
            }
        }

        if (!self->closed_) {
            self->read_more();
        }
    });
}

void AsioConnection::send(std::span<const u8> bytes) {
    if (closed_) {
        return;
    }
    // Posted rather than written directly: send() may be called from a handler
    // running on the loop, and queueing keeps the ordering obvious.
    auto payload = std::make_shared<std::vector<u8>>(bytes.begin(), bytes.end());
    asio::post(socket_.get_executor(), [self = shared_from_this(), payload] {
        self->write_queue_.push_back(std::move(*payload));
        if (!self->writing_) {
            self->write_next();
        }
    });
}

void AsioConnection::write_next() {
    if (write_queue_.empty()) {
        writing_ = false;
        return;
    }
    writing_ = true;
    asio::async_write(socket_, asio::buffer(write_queue_.front()),
                      [self = shared_from_this()](std::error_code ec, std::size_t) {
                          if (ec) {
                              self->finish();
                              return;
                          }
                          self->write_queue_.pop_front();
                          self->write_next();
                      });
}

void AsioConnection::close() {
    if (closed_) {
        return;
    }
    closed_ = true;
    asio::post(socket_.get_executor(), [self = shared_from_this()] {
        std::error_code ec;
        self->timer_.cancel();
        // shutdown before close so anything already queued reaches the peer;
        // closing outright can discard it.
        self->socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        self->socket_.close(ec);
    });
}

void AsioConnection::finish() {
    if (closed_) {
        return;
    }
    closed_ = true;
    std::error_code ec;
    timer_.cancel();
    socket_.close(ec);
    listener_.connection_closed(shared_from_this());
}

}  // namespace

std::unique_ptr<Listener> Listener::bind(u16 port, const std::string& address) {
    try {
        return std::make_unique<AsioListener>(port, address);
    } catch (const std::system_error& e) {
        OV_LOG_ERROR("cannot bind {}:{} — {}", address.empty() ? "0.0.0.0" : address, port,
                     e.what());
        return nullptr;
    } catch (const std::exception& e) {
        OV_LOG_ERROR("cannot bind {}:{} — {}", address.empty() ? "0.0.0.0" : address, port,
                     e.what());
        return nullptr;
    }
}

}  // namespace ov::net
