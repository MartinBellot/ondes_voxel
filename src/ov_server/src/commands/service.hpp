// The command engine, as the server holds it.
//
// Three doors, and the threading is the design:
//
//   * **any thread** — the network thread queues a player's command and
//     answers a completion request; the console thread queues a line. Neither
//     runs anything: a command changes the world, and only the tick thread may.
//   * **the tick thread, no lock** — `tick_world` moves the clock and the
//     weather, whose state is this object's and nobody else's.
//   * **the tick thread, `players_mutex` held** — `run` greets new players
//     (difficulty, server data, their permission, their command tree), sends
//     what the clock and the weather produced, and runs the queue.
//
// Everything the engine does to the server goes through `CommandHost`, a set
// of callbacks the server fills in over its own records — the same pattern as
// `WorkbenchHost` and `LevelHooks`, and for the same reason: this directory
// must not know what a chunk map or a connection is.
//
// `/effect` goes through `EffectSession` (effect_session.hpp), the status
// effects wave's own API; it sits between `difficulty` and `me` in
// `register_commands()`, vanilla's registration order.
#pragma once

#include "context.hpp"
#include "dispatcher.hpp"
#include "env.hpp"
#include "ops.hpp"
#include "text.hpp"
#include "world_state.hpp"

#include "../effect_session.hpp"
#include "../scoreboard/scoreboard.hpp"  // ── scoreboard ──
#include "../survival_session.hpp"

#include "ov/math/random.hpp"
#include "ov/protocol/chat.hpp"
#include "ov/protocol/play.hpp"

#include <array>
#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ov::server::cmd {

/// `minecraft:chat_type` as our registry codec numbers it: the datapack's
/// files, sorted, which is also vanilla's order — the capture's Player Chat
/// packets carry 0 for chat, 1 for /me, 2/3 for /msg and 4 for /say.
inline constexpr i32 kChatTypeChat        = 0;
inline constexpr i32 kChatTypeEmote       = 1;
inline constexpr i32 kChatTypeMsgIncoming = 2;
inline constexpr i32 kChatTypeMsgOutgoing = 3;
inline constexpr i32 kChatTypeSay         = 4;

/// One player, as a command may touch it. Pointers into the server's own
/// record, valid only while `players_mutex` is held — which is exactly while
/// a command runs.
struct PlayerRef {
    i32                             entity_id{0};
    std::string_view                name;
    net::Uuid                       uuid{};
    f64*                            x{nullptr};
    f64*                            y{nullptr};
    f64*                            z{nullptr};
    f32*                            yaw{nullptr};
    f32*                            pitch{nullptr};
    u8*                             game_mode{nullptr};
    i32*                            permission{nullptr};
    std::array<net::ItemStack, 46>* inventory{nullptr};
    i16                             held_slot{0};
    net::ItemStack*                 carried{nullptr};
    SurvivalSession*                survival{nullptr};
    /// Null when the server has no effect system for this player.
    EffectSession*                  effects{nullptr};
    EffectBearer                    effect_bearer{};
    std::function<void(i32, std::span<const u8>)> send;
    /// To everyone but this player.
    std::function<void(i32, std::span<const u8>)> broadcast_others;
};

struct BlockChange {
    BlockPos               pos{};
    registry::BlockStateId state{};
};

/// Synchronize Position's relative flags.
namespace teleport_flags {
inline constexpr u8 kX     = 0x01;
inline constexpr u8 kY     = 0x02;
inline constexpr u8 kZ     = 0x04;
inline constexpr u8 kYaw   = 0x08;
inline constexpr u8 kPitch = 0x10;
}  // namespace teleport_flags

struct CommandHost {
    std::function<void(const std::function<void(PlayerRef&)>&)> for_each_player;
    /// Every entity that is not a player: mobs, items, orbs.
    std::function<void(std::vector<EntityInfo>&)> entities;
    std::function<void(i32 packet_id, std::span<const u8> payload)> broadcast;
    /// Absolute position; `relative` says which fields the client should be
    /// sent as offsets, which is how vanilla sends a `~` teleport.
    std::function<void(i32 player, Vec3d position, f32 yaw, f32 pitch, u8 relative)> teleport_player;
    std::function<bool(i32 entity, Vec3d position, f32 yaw, f32 pitch)> teleport_entity;
    /// A mob dies (animation and loot) or an item vanishes.
    std::function<bool(i32 entity)> kill_entity;
    /// ── mobs-4 ── `/effect give` on a mob. Nullopt: `entity` is no mob that
    /// bears effects (an item, an arrow, a Nether mob).
    std::function<std::optional<gameplay::AddResult>(i32 entity,
                                                     const gameplay::EffectInstance& instance)>
        give_mob_effect;
    /// `/effect clear` on a mob: one effect, or every one with nullopt.
    /// Nullopt: no such mob; else how many went.
    std::function<std::optional<usize>(i32 entity, std::optional<gameplay::Effect> effect)>
        clear_mob_effect;
    std::function<std::optional<EntityInfo>(std::string_view type, Vec3d position)> summon;
    /// ── nether-2 ── The same, knowing who asked (-1: the console), so that a
    /// player standing in the Nether summons into the Nether. Empty: `summon`.
    std::function<std::optional<EntityInfo>(i32 source, std::string_view type, Vec3d position)>
        summon_by;
    std::function<void(i32 player, const net::ItemStack& stack)> drop_item;
    std::function<bool(i32 chunk_x, i32 chunk_z)> is_loaded;
    std::function<registry::BlockStateId(BlockPos)> block_at;
    /// One block, as a player's edit: relit, broadcast, neighbours told.
    std::function<void(BlockPos, registry::BlockStateId)> set_block;
    /// Break it as a player would: particles, drops, air.
    std::function<void(BlockPos)> destroy_block;
    /// Many at once, relit once per chunk and sent section by section.
    std::function<void(std::span<const BlockChange>)> set_blocks;
    /// The breaking half of `fill … destroy`: particles and loot for each,
    /// without writing — the writes follow through `set_blocks`, so a fill
    /// does not relight a neighbourhood per block.
    std::function<void(std::span<const BlockPos>)> break_blocks;
    std::function<void(i32 player, std::string_view reason_json)> kick;
    std::function<void()> save;
    std::function<void()> stop;
    std::function<void(i32 x, i32 y, i32 z, f32 angle)> set_world_spawn;
};

struct ServiceConfig {
    const registry::BlockRegistry* blocks{nullptr};
    const registry::Registries*    registries{nullptr};
    std::filesystem::path          ops_file;
    std::filesystem::path          lang_file;
    /// An integrated server has no ops.json: its host gets level 4 when the
    /// world allows commands (level.dat's allowCommands, read by
    /// load_world) and 0 when it does not. A dedicated one reads ops.json.
    bool        integrated{false};
    i32         max_players{20};
    std::string motd{"Ondes VOXEL"};
    // ── allow-commands ── the singleplayer host's name (--host-player), the
    // one player an integrated server's Allow Cheats applies to. Last, so the
    // positional initialisations above it keep their meaning.
    std::string host_player;
};

// ── allow-commands ──
/// The permission level a player joins with. Dedicated: ops.json's level, or
/// 0. Integrated: ops.json is not read; the host has 4 when the world allows
/// commands and 0 when it does not, and any other player 0 (Open to LAN's
/// "Allow Cheats", which would raise them, is not implemented). Measured on
/// the vanilla client, docs/provenance/commandes-solo.md.
[[nodiscard]] i32 join_permission(bool integrated, bool allow_commands, bool is_host,
                                  std::optional<i32> ops_level) noexcept;

struct PersonalSpawn {
    i32 x{0};
    i32 y{0};
    i32 z{0};
    f32 angle{0.0F};
};

class CommandService {
public:
    explicit CommandService(ServiceConfig config);
    CommandService(const CommandService&)            = delete;
    CommandService& operator=(const CommandService&) = delete;

    // ── Any thread ──────────────────────────────────────────────────────────

    void enqueue(i32 player_entity_id, std::string command, i64 timestamp, i64 salt);
    void enqueue_console(std::string command);
    /// ── scoreboard ── A chat line, broadcast on the tick: the sender's name
    /// wears its team, and the teams are the tick thread's to read.
    void enqueue_chat(i32 player_entity_id, std::string message, i64 timestamp, i64 salt);
    /// ── scoreboard ── A kill, for the kill criteria on the next tick: the
    /// holder names (a player's name, anything else's UUID). Any thread.
    void enqueue_kill(std::string killer, std::string victim, bool victim_is_player);

    [[nodiscard]] net::SuggestionsResponse suggest(const CommandSource& source, i32 transaction,
                                                   std::string_view                text,
                                                   std::span<const std::string>    player_names) const;

    [[nodiscard]] u8 default_game_mode() const noexcept {
        return default_mode_.load(std::memory_order_relaxed);
    }

    /// The Player Chat Message a chat line becomes: unsigned, chat type
    /// `minecraft:chat`, the sender's display name — with its team's colour,
    /// prefix and suffix (── scoreboard ──: a member, since it reads the
    /// teams). Tick thread.
    [[nodiscard]] std::vector<u8> chat_packet(std::string_view name, const net::Uuid& uuid,
                                              std::string_view message, i64 timestamp, i64 salt,
                                              i32 chat_type,
                                              const std::optional<Text>& target = std::nullopt) const;

    // ── scoreboard ── Tick thread, like the rest of the world's state.
    [[nodiscard]] Scoreboard&       scoreboard() noexcept { return scoreboard_; }
    [[nodiscard]] const Scoreboard& scoreboard() const noexcept { return scoreboard_; }
    /// Every player name in `text` — a component with a player's hover —
    /// dressed in its team: colour, prefix, suffix. What vanilla's display
    /// name is, and what every feedback line naming a player shows.
    [[nodiscard]] Text decorate(Text text) const;
    /// Send what the scoreboard queued. The server calls it after feeding the
    /// criteria; commands flush on their own, before each line they answer.
    void flush_scoreboard(const std::function<void(i32, std::span<const u8>)>& send);

    /// Player Abilities for a game mode, as vanilla sends them.
    [[nodiscard]] static std::vector<u8> abilities_for(u8 game_mode);

    // ── Tick thread ─────────────────────────────────────────────────────────

    [[nodiscard]] WorldState&       world() noexcept { return world_; }
    [[nodiscard]] const WorldState& world() const noexcept { return world_; }
    void load_world(const world::LevelSettings& settings);
    void store_world(world::LevelSettings& settings) const { world_.store(settings); }

    /// One tick of clock and weather. No lock.
    void tick_world();

    /// Greet, broadcast, and run the queue. `players_mutex` held.
    void run(CommandHost& host);

    /// Run one command now, as `source`. The queue goes through this.
    Parsed<i32> execute(const CommandSource& source, std::string_view command, CommandHost& host,
                        i64 timestamp = 0, i64 salt = 0);

    [[nodiscard]] std::optional<PersonalSpawn> personal_spawn(const net::Uuid& uuid) const;
    // ── weather ── A bed sets the same respawn point /spawnpoint does. True
    // when it moved — the game says "respawn point set" only then.
    bool set_personal_spawn(const net::Uuid& uuid, PersonalSpawn spawn) {
        PersonalSpawn& slot    = spawns_[uuid.to_string()];
        const bool     changed = slot.x != spawn.x || slot.y != spawn.y || slot.z != spawn.z;
        slot                   = spawn;
        return changed;
    }
    /// A bed that is gone is forgotten as a respawn point.
    void clear_personal_spawn(const net::Uuid& uuid) { spawns_.erase(uuid.to_string()); }
    [[nodiscard]] const Lang*       lang() const noexcept { return lang_ ? &*lang_ : nullptr; }
    [[nodiscard]] const Dispatcher& dispatcher() const noexcept { return dispatcher_; }
    [[nodiscard]] const ParseEnv&   env() const noexcept { return env_; }
    [[nodiscard]] const std::vector<u8>& commands_packet(i32 permission) const;

    /// What is written to the console. Tests capture it; the server logs it.
    std::function<void(std::string_view)> console;

private:
    struct Pending {
        i32         player{-1};  // -1: the console
        std::string command;
        i64         timestamp{0};
        i64         salt{0};
        bool        chat{false};  // ── scoreboard ── a chat line, not a command
    };

    void register_commands();
    // ── scoreboard ── scoreboard_commands.cpp, each called at vanilla's place
    // in the registration order.
    void register_scoreboard_command();
    void register_team_commands();
    void register_trigger_command();
    /// `single`: a `*` names nobody there, as the capture's `players list *`.
    [[nodiscard]] Parsed<std::vector<std::string>> score_holders(const CommandContext& ctx,
                                                                 std::string_view     name,
                                                                 bool                 single = false);
    void flush_scoreboard();

    // Players.
    void refresh_players();
    [[nodiscard]] PlayerRef* player(i32 entity_id);
    void welcome(PlayerRef& player);
    void set_permission(PlayerRef& player, i32 level);
    void set_game_mode(PlayerRef& player, u8 mode);
    [[nodiscard]] std::vector<EntityInfo> snapshot() const;
    [[nodiscard]] CommandSource source_for(const PlayerRef& player) const;
    /// ── allow-commands ── join_permission for this player and this world.
    [[nodiscard]] i32 permission_for(const PlayerRef& player) const;

    // Feedback, the way vanilla's CommandSourceStack gives it.
    void reply(const CommandSource& source, const Text& text);
    void success(const CommandSource& source, const Text& text, bool broadcast_to_ops);
    void failure(const CommandSource& source, const CommandError& error);
    [[nodiscard]] Text source_name(const CommandSource& source) const;

    // Arguments that need the world.
    [[nodiscard]] Parsed<std::vector<const EntityInfo*>> entities(const CommandContext& ctx,
                                                                  std::string_view     name);
    [[nodiscard]] Parsed<std::vector<PlayerRef*>> players(const CommandContext& ctx,
                                                          std::string_view     name);
    [[nodiscard]] Parsed<PlayerRef*> source_player(const CommandSource& source);
    [[nodiscard]] Text resolve(const Text& text, const CommandSource& source);
    [[nodiscard]] std::string resolve(const MessageArg& message, const CommandSource& source);
    [[nodiscard]] Text display(const EntityInfo& entity) const;

    ServiceConfig          config_;
    ParseEnv               env_;
    Dispatcher             dispatcher_;
    WorldState             world_;
    Scoreboard             scoreboard_;  // ── scoreboard ──
    OpList                 ops_;
    std::optional<Lang>    lang_;
    std::array<std::vector<u8>, 5> graphs_;

    mutable std::mutex     queue_mutex_;
    std::vector<Pending>   queue_;
    std::vector<Pending>   running_;
    // ── scoreboard ── kills waiting for the tick (under queue_mutex_), who
    // was dead at the last look (a death is the edge), who is tracked for the
    // player criteria.
    struct Kill {
        std::string killer;
        std::string victim;
        bool        victim_is_player{false};
    };
    std::vector<Kill>                    kills_;
    std::unordered_set<i32>              dead_;
    std::unordered_map<i32, std::string> criteria_names_;
    std::vector<Broadcast> outbox_;
    std::unordered_set<i32> welcomed_;
    std::unordered_map<std::string, PersonalSpawn> spawns_;
    std::atomic<u8>        default_mode_{1};
    bool                   allow_commands_{true};  // ── allow-commands ── level.dat's
    math::XoroshiroRandomSource random_{0x4F56434F4D4D414EULL, 0x44535F52414E4430ULL};

    // Valid only inside run() / execute().
    CommandHost*            host_{nullptr};
    std::vector<PlayerRef>  players_;
    std::vector<EntityInfo> world_snapshot_;
    i64                     timestamp_{0};
    i64                     salt_{0};
    Text                    source_shown_;  // ── scoreboard ── the source's name, at the start
};

}  // namespace ov::server::cmd
