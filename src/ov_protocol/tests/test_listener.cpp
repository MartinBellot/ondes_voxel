// The listener over a real socket: a connection is reported gone exactly
// once, whichever side closed it.
//
// Until this test, only a peer that went away was reported. A close the
// server asked for — a kick, a keep-alive timeout, a refused packet, a
// duplicate login — left the connection in the server's player table:
// unsaved, still counted against max-players. Found by the dedicated
// server's measurement (docs/provenance/serveur-dedie.md), where a fourth
// player was refused as "server full" with two players on.
#include "ov/protocol/listener.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace ov;

#if !defined(_WIN32)
namespace {

// Inside the dedicated server wave's own port range.
constexpr u16 kPort = 25613;

int connect_local() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family      = AF_INET;
    address.sin_port        = htons(kPort);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

/// Poll the listener until `done` or a second has passed.
template <typename Fn>
bool poll_until(net::Listener& listener, Fn&& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        listener.poll_for(10);
    }
    return done();
}

}  // namespace

TEST_CASE("a connection the server closes is reported, once", "[protocol][listener]") {
    auto listener = net::Listener::bind(kPort);
    if (!listener) {
        SKIP("port " << kPort << " is busy");
    }
    net::ConnectionPtr opened;
    int                closed = 0;
    listener->on_connect([&](const net::ConnectionPtr& c) { opened = c; });
    listener->on_disconnect([&](const net::ConnectionPtr&) { ++closed; });

    const int fd = connect_local();
    REQUIRE(fd >= 0);
    REQUIRE(poll_until(*listener, [&] { return opened != nullptr; }));

    opened->close();  // what a kick does
    CHECK(poll_until(*listener, [&] { return closed > 0; }));
    // The peer's side of the socket going away afterwards adds nothing.
    ::close(fd);
    (void)poll_until(*listener, [] { return false; });
    CHECK(closed == 1);
    listener->stop();
}

TEST_CASE("a connection the peer closes is reported, once", "[protocol][listener]") {
    auto listener = net::Listener::bind(kPort);
    if (!listener) {
        SKIP("port " << kPort << " is busy");
    }
    net::ConnectionPtr opened;
    int                closed = 0;
    listener->on_connect([&](const net::ConnectionPtr& c) { opened = c; });
    listener->on_disconnect([&](const net::ConnectionPtr&) { ++closed; });

    const int fd = connect_local();
    REQUIRE(fd >= 0);
    REQUIRE(poll_until(*listener, [&] { return opened != nullptr; }));
    ::close(fd);
    CHECK(poll_until(*listener, [&] { return closed > 0; }));
    opened->close();  // closing it again afterwards adds nothing
    (void)poll_until(*listener, [] { return false; });
    CHECK(closed == 1);
    listener->stop();
}
#endif
