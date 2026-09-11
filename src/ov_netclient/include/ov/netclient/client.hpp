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
#include "ov/protocol/chat.hpp"
#include "ov/protocol/chat_types.hpp"
#include "ov/protocol/client_play.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/world/chunk.hpp"

#include <expected>
#include <memory>
#include <optional>
#include <span>
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

    // ── weather ──
    /// Game Events 7 and 8 — the rain and thunder levels the sky darkens by —
    /// the last of each this poll. Game Event 1 (rain begins) sets the rain
    /// level to 0 and Game Event 2 (rain ends) to 1, as the game's client
    /// does, before the server's 7 and 8 that always follow them.
    std::optional<f32> rain_level;
    std::optional<f32> thunder_level;
    // ── end weather ──

    // ── What an interface needs ─────────────────────────────────────────────
    //
    // Everything below is here because a client that draws a HUD has to be
    // told these, and a client that only draws terrain never asked. The server
    // has been sending them all along.

    /// Set Health. The hearts, the haunches, and the death screen.
    std::optional<net::HealthUpdate> health;

    /// Set Experience. The bar and the level over the hotbar.
    std::optional<net::ExperienceUpdate> experience;

    /// Set Container Content, in arrival order. A vector rather than an
    /// optional: opening a chest sends the chest's window and the player's in
    /// the same breath, and keeping only the last loses the hotbar.
    std::vector<net::ContainerContent> containers;

    /// Set Container Slot, in arrival order. Order matters — two updates to
    /// one slot must be applied in the order the server sent them.
    std::vector<net::ContainerSlotUpdate> container_slots;

    /// Open Screen: the server is putting a window in front of the player.
    std::optional<net::OpenScreen> open_screen;

    /// Close Container, clientbound: the server is taking it away.
    std::optional<u8> close_window;

    /// The game mode, from Login (play). Creative hides the hearts, the
    /// haunches and the experience bar; a HUD that guesses shows a health bar
    /// nothing is maintaining.
    std::optional<u8> game_mode;

    // ── flight ──
    /// Player Abilities (0x34): what the server lets the player do. The flag
    /// bits are 0x01 invulnerable, 0x02 flying, 0x04 may fly, 0x08 instant
    /// build — creative sends 0x0D, spectator 0x07. A player who may fly
    /// decides for themself when to take off and land, and says so with
    /// Client::send_abilities; the server only grants or withdraws the right.
    struct Abilities {
        bool invulnerable{false};
        bool flying{false};
        bool may_fly{false};
        bool instant_build{false};
        /// Blocks a tick of horizontal push while flying: 0.05 in creative.
        f32 flying_speed{0.05F};
        /// The protocol calls it the field-of-view modifier; its value is the
        /// walking speed, 0.1.
        f32 walk_speed{0.1F};
    };
    std::optional<Abilities> abilities;

    // ── What a renderer needs to draw the things that move ──────────────────
    //
    // Ordered, and kept as one stream rather than as several vectors, because
    // the order between them is load-bearing: a spawn, three moves and a remove
    // for the same entity id, applied out of order, leaves a ghost. Everything
    // else in this struct is idempotent; this is not.

    /// Spawn Player and Spawn Experience Orb carry no entity type id: the
    /// packet *is* the type. These two negatives say so. Inventing a number for
    /// them would be inventing a Mojang id, which is the one thing the registry
    /// rule forbids — a reader maps them to `minecraft:player` and
    /// `minecraft:experience_orb` by name.
    static constexpr i32 kSpawnedAsPlayer         = -1;
    static constexpr i32 kSpawnedAsExperienceOrb  = -2;

    enum class EntityChangeKind : u8 {
        /// Spawn Entity (0x01), Spawn Player (0x03) or Spawn Experience Orb
        /// (0x02). `type` carries Mojang's entity type id.
        Spawn,
        /// Update Entity Position (0x2B) and its rotating variants: a **delta**
        /// in position, quantised to 1/4096 of a block on the wire.
        Move,
        /// Teleport Entity (0x68): an absolute position.
        Teleport,
        /// Entity Head Rotation (0x42). Sent for players only in this server.
        HeadRotation,
        /// Remove Entities (0x3E), or a pickup that ends in one.
        Remove,
        /// Set Entity Metadata (0x52), already decoded down to the fields this
        /// client understands. An index it does not know is skipped by name in
        /// the log, never by silently mis-parsing the rest of the packet.
        Metadata,
    };

    struct EntityChange {
        EntityChangeKind kind{EntityChangeKind::Remove};
        i32              id{0};
        /// Mojang's entity type id, on Spawn only.
        i32 type{0};
        /// Absolute for Spawn and Teleport, a delta for Move, in blocks.
        Vec3d position{};
        f32   yaw{0.0F};
        f32   pitch{0.0F};
        f32   head_yaw{0.0F};
        bool  on_ground{true};
        /// Spawn Entity's type-specific field, and an orb's value.
        i32 data{0};
        /// The player's name, on the spawn of a player.
        std::string name;
        /// The stack a dropped item carries, from metadata index 8. Empty when
        /// the change said nothing about it.
        std::optional<net::ItemStack> stack;
    };

    std::vector<EntityChange> entities;

    // ── chat ──
    //
    // Everything the chat, the action bar and the titles draw, in arrival
    // order: a title and its subtitle arrive as two packets and must be
    // applied in the order the server sent them.
    struct ChatEvent {
        enum class Kind : u8 {
            /// System Chat Message. `json`, and `overlay` for the action bar.
            System,
            /// Player Chat Message: `body` (plain text) or `unsigned_json`,
            /// decorated by chat type `chat_type` with `sender_json`/`target_json`.
            Player,
            /// Disguised Chat Message: `json` is the message, decorated the same way.
            Disguised,
            Title,
            Subtitle,
            /// Set Action Bar Text: `json`.
            ActionBar,
            /// Set Title Animation Times, in ticks.
            TitleTimes,
            /// Clear Titles; `reset` also restores the default times.
            ClearTitles,
        };
        Kind                       kind{Kind::System};
        std::string                json;
        bool                       overlay{false};
        i32                        chat_type{0};
        std::string                sender_json;
        std::optional<std::string> target_json;
        std::string                body;
        std::optional<std::string> unsigned_json;
        i32                        fade_in{0};
        i32                        stay{0};
        i32                        fade_out{0};
        bool                       reset{false};
    };
    std::vector<ChatEvent> chat;
    /// The chat types of the registry codec, from Login (play).
    std::optional<std::vector<net::ChatDecoration>> chat_types;
    /// The Commands packet: every command this player may run.
    std::optional<net::CommandGraphWire> commands;
    /// Command Suggestions Response, in arrival order.
    std::vector<net::SuggestionsResponse> suggestions;
    // ── end chat ──

    [[nodiscard]] bool empty() const noexcept {
        return loaded.empty() && unloaded.empty() && changed.empty() && !teleport &&
               !time_of_day && !health && !experience && containers.empty() &&
               container_slots.empty() && !open_screen && !close_window && !game_mode &&
               !abilities && entities.empty() && chat.empty() && !chat_types &&
               !commands && suggestions.empty() && !rain_level && !thunder_level;
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
    /// The stack's NBT is carried through untouched, exactly as the Slot
    /// encoder expects it: TAG_Compound, an empty name, the payload. A potion
    /// or an enchanted book without it is a *different item* — the same id
    /// with none of its meaning — so the creative screen needs the overload
    /// rather than the short one.
    void send_creative_slot(i16 slot, i32 item_id, i8 count, std::span<const u8> nbt = {});

    /// Click inside an open window, in the protocol's own terms.
    ///
    /// The server is authoritative: nothing moves here until it says so, and
    /// what it says arrives as a Set Container Slot or a whole new content.
    /// `carried` is what the client believes the cursor holds — the server
    /// ignores it, but vanilla sends it and so do we.
    ///
    /// `state_id` must be the last one the server sent for this window. A
    /// stale one makes a vanilla server resynchronise the whole window, which
    /// is not an error but does undo the click.
    void send_container_click(u8 window_id, i32 state_id, i16 slot, i8 button, i32 mode,
                              const net::ItemStack& carried);

    /// Tell the server the window is closed. Not optional: a server that still
    /// believes a container is open refuses the next one.
    void send_close_container(u8 window_id);

    /// Tell the server the player took off or landed (Player Abilities,
    /// serverbound). Only meaningful for a player the server lets fly; a
    /// vanilla server ignores the flag from anyone else.
    void send_abilities(bool flying);  // ── flight ──

    // ── chat ──
    /// Chat Message, unsigned: the text as typed, at most 256 UTF-16 units.
    void send_chat_message(std::string_view message);
    /// Chat Command: the command *without* its slash, unsigned.
    void send_chat_command(std::string_view command);
    /// Command Suggestions Request: the input up to the cursor, slash included.
    void send_suggestions_request(i32 transaction, std::string_view text);
    // ── end chat ──

private:
    struct Impl;

    Client();

    std::unique_ptr<Impl> impl_;
};

}  // namespace ov::netclient
