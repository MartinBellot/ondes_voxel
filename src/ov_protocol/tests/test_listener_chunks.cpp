// Chunks encoded on the network thread, in send order.
//
// The tick thread used to encode every `Chunk Data` itself, up to eight per
// player per tick. It now hands the connection a snapshot
// (`Connection::send_chunk`), and the network thread encodes it in the same
// queue as the bytes already framed — so a `Block Update` sent after a chunk
// can never overtake it, and the tick does not pay for the encoding.
//
// Driven over a real loopback socket: the order and the isolation are
// properties of two threads, and a fake connection would prove neither.
#include "ov/protocol/framing.hpp"
#include "ov/protocol/listener.hpp"
#include "ov/protocol/play.hpp"
#include "ov/world/chunk.hpp"

#include <asio.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <future>
#include <memory>
#include <span>
#include <thread>
#include <vector>

using namespace ov;
using registry::BlockStateId;

namespace {

constexpr world::AirStates kAir{BlockStateId{0}, BlockStateId{12817}, BlockStateId{12818}};

[[nodiscard]] world::Chunk make_chunk() {
    world::Chunk chunk{ChunkPos{2, 5}, world::WorldShape::overworld(), kAir, nullptr};
    for (usize x = 0; x < 16; ++x) {
        chunk.set_block(x, 60, 3, BlockStateId{1});
    }
    return chunk;
}

/// A connection records bytes; this one records the packets they frame.
class Recorder final : public net::Connection {
public:
    void send(std::span<const u8> bytes) override { bytes_.insert(bytes_.end(), bytes.begin(), bytes.end()); }
    void close() override {}
    [[nodiscard]] std::string peer_address() const override { return "recorder"; }
    void set_compression_threshold(i32) override {}

    [[nodiscard]] const std::vector<u8>& bytes() const noexcept { return bytes_; }

private:
    std::vector<u8> bytes_;
};

[[nodiscard]] std::vector<u8> framed(i32 id, std::span<const u8> body) {
    auto result = net::encode_packet(id, body);
    REQUIRE(result.has_value());
    return *result;
}

}  // namespace

TEST_CASE("the default send_chunk sends what the tick thread used to", "[net][chunk]") {
    // Every connection that does not override it — the in-process channel,
    // test doubles — must put exactly the old bytes on the wire.
    const world::Chunk chunk = make_chunk();
    Recorder           recorder;
    recorder.send_chunk(std::make_shared<const world::Chunk>(chunk));

    const auto expected = framed(net::clientbound::kChunkDataAndLight, net::encode_chunk_data(chunk));
    REQUIRE(recorder.bytes() == expected);
}

TEST_CASE("a chunk sent between two packets arrives between them, as it was", "[net][chunk]") {
    auto listener = net::Listener::bind(0, "127.0.0.1");
    REQUIRE(listener != nullptr);

    std::promise<net::ConnectionPtr> accepted;
    auto                             accepted_future = accepted.get_future();
    listener->on_connect([&accepted](const net::ConnectionPtr& connection) {
        accepted.set_value(connection);
    });
    std::thread network{[&listener] { listener->run(); }};

    asio::io_context      client_context;
    asio::ip::tcp::socket socket{client_context};
    socket.connect({asio::ip::make_address("127.0.0.1"), listener->port()});

    REQUIRE(accepted_future.wait_for(std::chrono::seconds{10}) == std::future_status::ready);
    const net::ConnectionPtr connection = accepted_future.get();

    // What the tick does: frame a packet, hand over a snapshot, frame another,
    // and keep writing the chunk while the network thread encodes it.
    world::Chunk   chunk    = make_chunk();
    const auto     expected = net::encode_chunk_data(chunk);
    const std::array<u8, 3> before_body{1, 2, 3};
    const std::array<u8, 2> after_body{9, 8};

    connection->send(framed(0x23, before_body));
    connection->send_chunk(chunk.snapshot());
    for (i32 y = -64; y < 320; ++y) {
        chunk.set_block(0, y, 0, BlockStateId{2});
    }
    connection->send(framed(0x1B, after_body));

    net::FrameDecoder        decoder;
    std::vector<net::Packet> packets;
    std::array<u8, 16384>    buffer{};
    const auto               deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (packets.size() < 3 && std::chrono::steady_clock::now() < deadline) {
        std::error_code ec;
        const usize     read = socket.read_some(asio::buffer(buffer), ec);
        REQUIRE_FALSE(ec);
        decoder.feed(std::span<const u8>{buffer.data(), read});
        while (true) {
            auto packet = decoder.next();
            if (!packet) {
                REQUIRE(packet.error() == net::FrameError::Incomplete);
                break;
            }
            packets.push_back(std::move(*packet));
        }
    }

    listener->stop();
    network.join();

    REQUIRE(packets.size() == 3);
    CHECK(packets[0].id == 0x23);
    CHECK(packets[1].id == net::clientbound::kChunkDataAndLight);
    CHECK(packets[2].id == 0x1B);
    CHECK(packets[0].body == std::vector<u8>(before_body.begin(), before_body.end()));
    CHECK(packets[2].body == std::vector<u8>(after_body.begin(), after_body.end()));

    // The chunk as it was when handed over, not as the tick left it.
    CHECK(packets[1].body == expected);
    CHECK(packets[1].body != net::encode_chunk_data(chunk));
}
