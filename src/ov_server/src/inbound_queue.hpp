// Network input, carried to the tick thread.
//
// The server used to run every packet handler on the network thread, and
// every handler reached into the world — so the world needed a mutex, the tick
// waited behind it, and a handler that touched a chunk nobody had generated yet
// ran the whole worldgen pipeline right there: a 19.7 s packet, and the tick
// frozen behind it for as long (docs/provenance/performance-tick.md § 5.4).
//
// This is the other arrangement, the one CLAUDE.md principle 3 asks for. The
// network thread only frames bytes: each decoded packet, each connection that
// opens or closes, is copied into this queue and the thread goes back to its
// socket. The tick thread drains the queue at the top of every loop iteration
// and while it waits for the next tick, and runs the **same** handlers there.
// The world then has exactly one thread that reads or writes it, and a lock
// around it protects nothing (tick_thread_lock.hpp).
//
// Waiting on the queue, rather than sleeping, is what keeps a dig answered in
// about a millisecond instead of up to a tick later: a packet that arrives
// while the loop is idle wakes it at once. That is also how the game's own
// server does it — its main thread runs queued network tasks while it waits
// for the next tick.
//
// The queue has a mutex and a condition variable. That is not the mutex
// principle 3 forbids: it guards a vector of byte buffers, never a chunk, and
// what it carries has left the network thread for good by the time the tick
// sees it — the same argument as the job pool's own queue.
#pragma once

#include "ov/base/types.hpp"
#include "ov/protocol/listener.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <span>
#include <vector>

namespace ov::server {

class LatencyHistogram;

enum class InboundKind : u8 {
    Connect,
    Packet,
    Disconnect,
};

/// One thing the network thread saw, waiting for the tick.
struct InboundEvent {
    InboundKind        kind{InboundKind::Packet};
    net::ConnectionPtr connection;
    i32                packet_id{0};
    /// A copy: the span the listener hands over dies with its callback.
    std::vector<u8> body;
    /// Monotonic microseconds at arrival. Instrumentation only — it measures
    /// how long input waited for the tick and never feeds a decision.
    i64 arrived_micros{0};
};

/// The three handlers the listener used to call directly.
struct InboundHandlers {
    net::ConnectionHandler on_connect;
    net::PacketHandler     on_packet;
    net::ConnectionHandler on_disconnect;
};

/// What one `dispatch` did.
struct InboundDispatch {
    usize events{0};
    /// Packets a handler returned false for: their connection was closed.
    usize refused{0};
    /// Packets that arrived for a connection already refused, and were dropped
    /// unread — exactly what the listener did when it stopped reading.
    usize dropped{0};
};

class InboundQueue {
public:
    InboundQueue() = default;

    InboundQueue(const InboundQueue&)            = delete;
    InboundQueue& operator=(const InboundQueue&) = delete;

    // ── The network side: any thread ────────────────────────────────────────

    void push_connect(const net::ConnectionPtr& connection);
    void push_packet(const net::ConnectionPtr& connection, i32 packet_id,
                     std::span<const u8> body);
    void push_disconnect(const net::ConnectionPtr& connection);

    /// Handlers for a listener that only enqueue. Borrow `this`, which must
    /// outlive the listener's event loop.
    void attach(net::Listener& listener);

    // ── The tick side: one thread ───────────────────────────────────────────

    /// Block until something is queued or `deadline` passes. True when events
    /// are waiting. Never sleeps past the deadline, which is the next tick.
    bool wait_until(std::chrono::steady_clock::time_point deadline);

    /// Run everything queued so far through `handlers`, in arrival order.
    ///
    /// A packet handler that returns false closes its connection, as the
    /// listener did — and then runs `on_disconnect` for it, which the listener
    /// never did: its `close()` marks the socket closed first, so the read that
    /// fails afterwards finds it already closed and reports nothing. A refused
    /// player used to stay in the player table for the life of the process.
    /// Packets already queued behind a refusal are dropped unread, and a later
    /// real disconnect for the same connection is not run a second time.
    ///
    /// `queue_wait`, when given, records how long each event waited here.
    InboundDispatch dispatch(const InboundHandlers& handlers,
                             LatencyHistogram*      queue_wait = nullptr);

    /// The packet id being handled right now, or -1 outside a packet handler.
    /// For attributing work a handler caused — a chunk generated on the spot,
    /// for instance — to the packet that caused it.
    [[nodiscard]] i32 current_packet_id() const noexcept { return current_packet_id_; }

    /// Events handled since start-up, by kind. Tick thread only.
    [[nodiscard]] u64 packets_handled() const noexcept { return packets_handled_; }
    [[nodiscard]] u64 packets_refused() const noexcept { return packets_refused_; }

private:
    void push(InboundEvent event);

    std::mutex              mutex_;
    std::condition_variable ready_;
    std::vector<InboundEvent> pending_;  // guarded by mutex_

    // Tick thread only.
    std::vector<InboundEvent>       working_;
    std::vector<net::ConnectionPtr> refused_;
    i32                             current_packet_id_{-1};
    u64                             packets_handled_{0};
    u64                             packets_refused_{0};
};

}  // namespace ov::server
