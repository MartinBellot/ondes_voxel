// The client half of the protocol: a module that plays Minecraft without
// drawing anything.
//
// This is the point of the layer graph, and the one place it is easiest to
// destroy. ov_netclient is a *client* with no renderer in it: no Vulkan, no
// window, no GLFW. A single #include <vulkan/...> here and the separation the
// whole project is built on stops being enforceable — see risk R2. It is also
// what makes "the solo game is multiplayer" true rather than aspirational:
// the same bytes go over a socket whether the server is across the world or on
// the next thread.
//
// The world the server describes is assembled here into the very same
// world::Chunk the server uses. Nothing is re-implemented: the chunk packet is
// parsed by ov_protocol, which is tested by encoding a real chunk and reading
// it back.
//
// Threading. The socket runs on its own thread, because a blocking read must
// never stall a frame. Everything it produces crosses to the caller through
// one queue, and the caller is the only thread that ever touches the level —
// so the "single writer" rule survives, with the hand-off explicit instead of
// a lock around the world.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/vec.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/world/chunk.hpp"

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ov::netclient {

enum class ClientError : u8 {
    /// The address would not resolve, or nothing was listening.
    CannotConnect,
    /// The peer closed the connection, or the stream stopped making sense.
    /// There is no resynchronising a byte stream once a packet is misread.
    Disconnected,
    /// The server refused the login and said why; see Client::disconnect_reason.
    Rejected,
};

[[nodiscard]] std::string_view to_string(ClientError error) noexcept;

struct ClientDesc {
    std::string host{"127.0.0.1"};
    u16         port{25565};
    std::string username{"Player"};
    /// Needed to build chunks: air states, and the block ids the packet uses.
    const registry::BlockRegistry* registry{nullptr};
    /// What the server is told to send. Not cosmetic: a server that believes
    /// the view distance is 2 sends nine chunks and no more.
    u8 view_distance{12};
};

/// What the player is doing, as the client will report it.
struct PlayerInput {
    Vec3d position{};
    f32   yaw{0.0F};
    f32   pitch{0.0F};
    bool  on_ground{true};
};

/// One thing the server said, already turned into something usable.
struct ClientEvents {
    /// Chunks that arrived, ready to be put into a level. Moved out, because a
    /// chunk is a quarter of a megabyte and copying one per packet would show.
    std::vector<std::unique_ptr<world::Chunk>> loaded;
    /// Chunks the server dropped, as chunk coordinates.
    std::vector<std::pair<i32, i32>> unloaded;
    /// Single block changes, in world coordinates.
    struct BlockChange {
        i32                    x{0};
        i32                    y{0};
        i32                    z{0};
        registry::BlockStateId state{};
    };
    std::vector<BlockChange> changed;

    /// Set when the server moved the player — a spawn, a teleport, a
    /// correction after it disagreed with where we said we were.
    std::optional<PlayerInput> teleport;

    /// The world's clock, when it was sent this poll.
    std::optional<i64> time_of_day;

    [[nodiscard]] bool empty() const noexcept {
        return loaded.empty() && unloaded.empty() && changed.empty() && !teleport && !time_of_day;
    }
    void clear();
};

class Client {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<Client>, ClientError> connect(
        const ClientDesc& desc);

    Client(const Client&)            = delete;
    Client& operator=(const Client&) = delete;
    ~Client();

    /// Take everything that has arrived since the last call. Never blocks.
    void poll(ClientEvents& out);

    /// True once the server has sent Login (play) and the world is coming.
    [[nodiscard]] bool in_game() const noexcept;
    /// False once the socket has closed, for any reason.
    [[nodiscard]] bool connected() const noexcept;
    /// Why, when the server said. Empty when it just went away.
    [[nodiscard]] std::string disconnect_reason() const;

    /// Tell the server where the player is. Sent as one packet; the server
    /// decides whether to believe it.
    void send_position(const PlayerInput& input);

    /// Start, cancel or finish breaking the block at a position. The status
    /// values are the protocol's own.
    void send_dig(i32 x, i32 y, i32 z, i32 status, i32 face);

    /// Place whatever is held against a face of a block.
    void send_place(i32 x, i32 y, i32 z, i32 face, f32 cursor_x, f32 cursor_y, f32 cursor_z);

    /// Which hotbar slot is selected, 0 to 8.
    void send_held_slot(i16 slot);

    /// Put an item into an inventory slot. Creative only — a server in
    /// survival ignores this, and honouring it there would let any client hand
    /// itself anything.
    ///
    /// Slot 36 is the first of the hotbar in the player's own window; the
    /// hotbar is slots 36 to 44 and not 0 to 8, which is the mistake this
    /// comment exists to stop.
    void send_creative_slot(i16 slot, i32 item_id, i8 count);

private:
    struct Impl;

    Client();

    std::unique_ptr<Impl> impl_;
};

}  // namespace ov::netclient
