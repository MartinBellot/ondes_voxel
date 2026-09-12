// Network input carried to the tick thread: order, refusal, and waking.
#include "../src/inbound_queue.hpp"
#include "../src/tick_profile.hpp"
#include "../src/tick_thread_lock.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace ov;
using namespace ov::server;

namespace {

/// A connection that remembers whether it was closed.
class FakeConnection final : public net::Connection {
public:
    explicit FakeConnection(std::string name) : name_{std::move(name)} {}

    void send(std::span<const u8>) override {}
    void close() override { closed = true; }
    [[nodiscard]] std::string peer_address() const override { return name_; }
    void set_compression_threshold(i32) override {}

    bool closed{false};

private:
    std::string name_;
};

/// Records every handler call as one line.
struct Journal {
    std::vector<std::string> lines;

    InboundHandlers handlers(i32 refuse_id = -1) {
        InboundHandlers out;
        out.on_connect = [this](const net::ConnectionPtr& c) {
            lines.push_back("connect " + c->peer_address());
        };
        out.on_disconnect = [this](const net::ConnectionPtr& c) {
            lines.push_back("disconnect " + c->peer_address());
        };
        out.on_packet = [this, refuse_id](const net::ConnectionPtr& c, i32 id,
                                          std::span<const u8> body) {
            lines.push_back("packet " + c->peer_address() + " " + std::to_string(id) + " " +
                            std::to_string(body.size()));
            return id != refuse_id;
        };
        return out;
    }
};

}  // namespace

TEST_CASE("events come out in the order they went in, across kinds and connections",
          "[inbound]") {
    InboundQueue queue;
    const auto   a = std::make_shared<FakeConnection>("a");
    const auto   b = std::make_shared<FakeConnection>("b");

    const std::vector<u8> two{1, 2};
    queue.push_connect(a);
    queue.push_packet(a, 0x10, two);
    queue.push_connect(b);
    queue.push_packet(b, 0x11, {});
    queue.push_packet(a, 0x12, two);
    queue.push_disconnect(a);

    Journal         journal;
    InboundDispatch result = queue.dispatch(journal.handlers());
    CHECK(result.events == 6);
    CHECK(result.refused == 0);
    CHECK(journal.lines == std::vector<std::string>{
                               "connect a", "packet a 16 2", "connect b", "packet b 17 0",
                               "packet a 18 2", "disconnect a"});
    CHECK(queue.packets_handled() == 3);

    // Drained: a second dispatch finds nothing.
    journal.lines.clear();
    CHECK(queue.dispatch(journal.handlers()).events == 0);
    CHECK(journal.lines.empty());
}

TEST_CASE("the body is a copy, not the listener's buffer", "[inbound]") {
    InboundQueue queue;
    const auto   a = std::make_shared<FakeConnection>("a");
    {
        std::vector<u8> transient{7, 8, 9};
        queue.push_packet(a, 1, transient);
        transient.assign(3, 0);  // the listener reuses its buffer
    }
    std::vector<u8> seen;
    InboundHandlers handlers;
    handlers.on_packet = [&](const net::ConnectionPtr&, i32, std::span<const u8> body) {
        seen.assign(body.begin(), body.end());
        return true;
    };
    (void)queue.dispatch(handlers);
    CHECK(seen == std::vector<u8>{7, 8, 9});
}

TEST_CASE("a refused packet closes its connection, runs the disconnect once, and drops the rest",
          "[inbound]") {
    InboundQueue queue;
    const auto   a = std::make_shared<FakeConnection>("a");
    const auto   b = std::make_shared<FakeConnection>("b");

    queue.push_packet(a, 5, {});
    queue.push_packet(a, 99, {});  // refused
    queue.push_packet(b, 6, {});   // another connection is untouched
    queue.push_packet(a, 7, {});   // already queued behind the refusal: dropped
    queue.push_disconnect(a);      // the socket's own close: not run twice

    Journal         journal;
    InboundDispatch result = queue.dispatch(journal.handlers(99));
    CHECK(a->closed);
    CHECK_FALSE(b->closed);
    CHECK(result.refused == 1);
    CHECK(result.dropped == 1);
    CHECK(journal.lines == std::vector<std::string>{"packet a 5 0", "packet a 99 0",
                                                    "disconnect a", "packet b 6 0"});
    CHECK(queue.packets_refused() == 1);
}

TEST_CASE("a refused connection is forgotten once nobody else holds it", "[inbound]") {
    InboundQueue queue;
    Journal      journal;
    {
        auto a = std::make_shared<FakeConnection>("a");
        queue.push_packet(a, 99, {});
        (void)queue.dispatch(journal.handlers(99));
        // The listener still holds `a` here, through this local.
        queue.push_packet(a, 1, {});
        CHECK(queue.dispatch(journal.handlers(99)).dropped == 1);
    }
    // `a` is gone. A new connection — possibly at the same address — is
    // served normally: the queue let go of its copy after the last dispatch.
    (void)queue.dispatch(journal.handlers(99));
    auto fresh = std::make_shared<FakeConnection>("fresh");
    queue.push_packet(fresh, 2, {});
    const InboundDispatch result = queue.dispatch(journal.handlers(99));
    CHECK(result.dropped == 0);
    CHECK(journal.lines.back() == "packet fresh 2 0");
}

TEST_CASE("the packet being handled is named while its handler runs, and only then",
          "[inbound]") {
    InboundQueue queue;
    const auto   a = std::make_shared<FakeConnection>("a");
    queue.push_packet(a, 0x2E, {});
    queue.push_connect(a);

    std::vector<i32> seen;
    InboundHandlers  handlers;
    handlers.on_packet = [&](const net::ConnectionPtr&, i32, std::span<const u8>) {
        seen.push_back(queue.current_packet_id());
        return true;
    };
    handlers.on_connect = [&](const net::ConnectionPtr&) {
        seen.push_back(queue.current_packet_id());
    };
    (void)queue.dispatch(handlers);
    CHECK(seen == std::vector<i32>{0x2E, -1});
    CHECK(queue.current_packet_id() == -1);
}

TEST_CASE("a waiting tick wakes when a packet arrives, and not later than its deadline",
          "[inbound]") {
    using Clock = std::chrono::steady_clock;
    InboundQueue queue;

    // Nothing queued: returns false at the deadline, not before.
    const auto quiet_start = Clock::now();
    CHECK_FALSE(queue.wait_until(quiet_start + std::chrono::milliseconds{30}));
    CHECK(Clock::now() - quiet_start >= std::chrono::milliseconds{29});

    // A packet from another thread wakes the waiter long before a far deadline.
    const auto   a = std::make_shared<FakeConnection>("a");
    std::jthread producer{[&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        queue.push_packet(a, 1, {});
    }};
    const auto start = Clock::now();
    CHECK(queue.wait_until(start + std::chrono::seconds{30}));
    CHECK(Clock::now() - start < std::chrono::seconds{10});
    producer.join();

    InboundHandlers handlers;
    handlers.on_packet = [](const net::ConnectionPtr&, i32, std::span<const u8>) { return true; };
    CHECK(queue.dispatch(handlers).events == 1);
}

TEST_CASE("handlers run on the dispatching thread — the one that owns the world",
          "[inbound]") {
    // The whole point: many producers, one consumer, and every handler call on
    // the consumer. A world lock owned by the consumer must never see a
    // foreign caller.
    InboundQueue   queue;
    TickThreadLock world{TickThreadLock::Policy::Count};
    const auto     a = std::make_shared<FakeConnection>("a");

    constexpr int              kProducers  = 4;
    constexpr int              kPerProducer = 500;
    std::vector<std::jthread>  producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                queue.push_packet(a, p, {});
            }
        });
    }

    std::atomic<int> handled{0};
    InboundHandlers  handlers;
    handlers.on_packet = [&](const net::ConnectionPtr&, i32, std::span<const u8>) {
        const std::scoped_lock touch{world};
        handled.fetch_add(1, std::memory_order_relaxed);
        return true;
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    while (handled.load() < kProducers * kPerProducer &&
           std::chrono::steady_clock::now() < deadline) {
        if (queue.wait_until(std::chrono::steady_clock::now() + std::chrono::milliseconds{5})) {
            (void)queue.dispatch(handlers);
        }
    }
    producers.clear();
    (void)queue.dispatch(handlers);
    CHECK(handled.load() == kProducers * kPerProducer);
    CHECK(world.foreign_locks() == 0);
}

TEST_CASE("the time input waited for the tick is recorded when asked", "[inbound]") {
    InboundQueue     queue;
    LatencyHistogram wait;
    const auto       a = std::make_shared<FakeConnection>("a");
    queue.push_packet(a, 1, {});
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
    InboundHandlers handlers;
    (void)queue.dispatch(handlers, &wait);
    CHECK(wait.count() == 1);
    CHECK(wait.max() >= 4'000);
}
