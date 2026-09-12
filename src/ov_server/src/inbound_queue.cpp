#include "inbound_queue.hpp"

#include "tick_profile.hpp"

#include "ov/base/log.hpp"

#include <algorithm>
#include <utility>

namespace ov::server {

void InboundQueue::push(InboundEvent event) {
    event.arrived_micros = monotonic_micros();
    {
        const std::scoped_lock lock{mutex_};
        pending_.push_back(std::move(event));
    }
    ready_.notify_one();
}

void InboundQueue::push_connect(const net::ConnectionPtr& connection) {
    push(InboundEvent{InboundKind::Connect, connection, 0, {}, 0});
}

void InboundQueue::push_packet(const net::ConnectionPtr& connection, i32 packet_id,
                               std::span<const u8> body) {
    push(InboundEvent{InboundKind::Packet, connection, packet_id,
                      std::vector<u8>(body.begin(), body.end()), 0});
}

void InboundQueue::push_disconnect(const net::ConnectionPtr& connection) {
    push(InboundEvent{InboundKind::Disconnect, connection, 0, {}, 0});
}

void InboundQueue::attach(net::Listener& listener) {
    listener.on_connect([this](const net::ConnectionPtr& connection) { push_connect(connection); });
    listener.on_disconnect(
        [this](const net::ConnectionPtr& connection) { push_disconnect(connection); });
    // Always true: whether a packet is acceptable is decided on the tick
    // thread, which closes the connection itself when it is not.
    listener.on_packet(
        [this](const net::ConnectionPtr& connection, i32 packet_id, std::span<const u8> body) {
            push_packet(connection, packet_id, body);
            return true;
        });
}

bool InboundQueue::wait_until(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock lock{mutex_};
    ready_.wait_until(lock, deadline, [this] { return !pending_.empty(); });
    return !pending_.empty();
}

InboundDispatch InboundQueue::dispatch(const InboundHandlers& handlers,
                                       LatencyHistogram*      queue_wait) {
    working_.clear();
    {
        const std::scoped_lock lock{mutex_};
        working_.swap(pending_);
    }

    InboundDispatch result;
    const auto      is_refused = [this](const net::Connection* connection) {
        return std::ranges::any_of(refused_, [connection](const net::ConnectionPtr& held) {
            return held.get() == connection;
        });
    };

    for (InboundEvent& event : working_) {
        ++result.events;
        if (queue_wait != nullptr) {
            queue_wait->record(monotonic_micros() - event.arrived_micros);
        }
        switch (event.kind) {
            case InboundKind::Connect:
                if (handlers.on_connect) {
                    handlers.on_connect(event.connection);
                }
                break;

            case InboundKind::Packet: {
                if (is_refused(event.connection.get())) {
                    ++result.dropped;
                    break;
                }
                ++packets_handled_;
                current_packet_id_ = event.packet_id;
                const bool accepted =
                    !handlers.on_packet ||
                    handlers.on_packet(event.connection, event.packet_id, event.body);
                current_packet_id_ = -1;
                if (!accepted) {
                    ++result.refused;
                    ++packets_refused_;
                    OV_LOG_WARN("{}: packet 0x{:02X} ({} bytes) refused by its handler, closing",
                                event.connection->peer_address(), event.packet_id,
                                event.body.size());
                    event.connection->close();
                    refused_.push_back(event.connection);
                    if (handlers.on_disconnect) {
                        handlers.on_disconnect(event.connection);
                    }
                }
                break;
            }

            case InboundKind::Disconnect: {
                const auto held = std::ranges::find_if(
                    refused_, [&](const net::ConnectionPtr& connection) {
                        return connection.get() == event.connection.get();
                    });
                if (held != refused_.end()) {
                    // Already run when the packet was refused.
                    refused_.erase(held);
                    break;
                }
                if (handlers.on_disconnect) {
                    handlers.on_disconnect(event.connection);
                }
                break;
            }
        }
    }
    working_.clear();

    // A refused connection is remembered only while something else still holds
    // it. Once the listener has let go, nothing more can arrive for it — and a
    // pointer kept past that point could be handed to a new connection by the
    // allocator, whose packets would then be dropped as the old one's.
    std::erase_if(refused_, [](const net::ConnectionPtr& connection) {
        return connection.use_count() == 1;
    });
    return result;
}

}  // namespace ov::server
