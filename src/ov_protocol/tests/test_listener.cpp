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

#include <array>
#include <chrono>
#include <memory>
#include <vector>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
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

TEST_CASE("what is sent before a close reaches the peer", "[protocol][listener]") {
    // A kick: a chat line, a Disconnect, then close(). All of it must arrive
    // before the end of the stream.
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

    std::vector<u8> sent;
    for (int packet = 0; packet < 8; ++packet) {
        const std::vector<u8> bytes(20000, static_cast<u8>(packet));
        opened->send(bytes);
        sent.insert(sent.end(), bytes.begin(), bytes.end());
    }
    opened->close();

    std::vector<u8> received;
    std::array<u8, 65536> buffer{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    while (std::chrono::steady_clock::now() < deadline) {
        listener->poll_for(5);
        timeval wait{0, 1000};
        (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof wait);
        const ssize_t got = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (got == 0) {
            break;  // the end of the stream, after everything
        }
        if (got > 0) {
            received.insert(received.end(), buffer.begin(), buffer.begin() + got);
        }
    }
    CHECK(received.size() == sent.size());
    CHECK(received == sent);
    CHECK(poll_until(*listener, [&] { return closed > 0; }));
    CHECK(closed == 1);
    ::close(fd);
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
