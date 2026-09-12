// Ondes VOXEL dedicated server.
//
// At M0 this ticks an empty world at 20 Hz and does nothing else. That is
// deliberately the first runnable artefact: the tick loop, its scheduling class
// and its behaviour under overload are the foundation everything else stands
// on, and they are far easier to get right on an empty world than on a full one.

#define OV_LOG_CATEGORY "server"

#include "ov/server/server.hpp"

#include "ov/base/alloc_scope.hpp"
#include "ov/base/assert.hpp"
#include "ov/base/log.hpp"
#include "ov/base/thread.hpp"
#include "ov/base/time.hpp"
#include "ov/gameplay/breaking.hpp"
#include "ov/gameplay/collision.hpp"
#include "ov/gameplay/connections.hpp"
#include "ov/gameplay/loot.hpp"
#include "ov/io/compression.hpp"
#include "ov/io/file.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/math/random.hpp"
#include "ov/nbt/binary.hpp"
#include "ov/nbt/region_writer.hpp"
#include "ov/protocol/framing.hpp"
#include "ov/protocol/listener.hpp"
#include "ov/protocol/login.hpp"
#include "ov/protocol/play.hpp"
#include "ov/protocol/status.hpp"
#include "ov/protocol/varint.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/entity/world.hpp"
#include "ov/gameplay/block_motion.hpp"  // ── movement physics ──
#include "ov/gameplay/entity_physics.hpp"
#include "ov/gameplay/mob_logic.hpp"
#include "ov/protocol/entity.hpp"
#include "ov/registry/registries.hpp"
// ── crafting and smelting ───────────────────────────────────────────────────
#include "workbench.hpp"
#include "enchant_session.hpp"  // ── enchanting ──
#include "commands/text.hpp"   // ── enchanting: hover names ──
#include "commands/names.hpp"  // ── enchanting: hover names ──
// ── containers, and the machines that move items between them ──────────────
#include "block_container.hpp"
#include "item_transport.hpp"
// ── scheduled ticks, natural spawning, and the player's own window ─────────
#include "natural_spawning.hpp"
#include "player_data.hpp"  // ── player data ──
#include "nether_travel.hpp"  // ── nether ──
#include "end_fight.hpp"      // ── end ──
#include "end_travel.hpp"     // ── end ──
#include "ov/math/raycast.hpp"  // ── dragon ──
#include "ov/gameplay/end_portal.hpp"  // ── end ──
#include "player_inventory.hpp"
#include "world_ticks.hpp"
#include "tick_profile.hpp"  // ── perf ──
#include "relight.hpp"       // ── perf ──
#include "sounds.hpp"  // ── sound ──
#include "destroy_stages.hpp"  // ── breaking ──
#include "agriculture.hpp"  // ── agriculture ──
#include "weather_session.hpp"  // ── weather ──
#include "campfire.hpp"      // ── fire ──
#include "fire_session.hpp"  // ── fire ──
#include "ov/world/chunk.hpp"
#include "ov/world/chunk_map.hpp"
#include "ov/world/chunk_storage.hpp"
#include "ov/protocol/survival.hpp"
#include "ov/world/level_dat.hpp"
#include "async_chunk_source.hpp"
#include "generated_world.hpp"
#include "survival_session.hpp"
#include "effect_session.hpp"  // ── effects ──
// ── commands and chat ──
#include "commands/console.hpp"
#include "commands/service.hpp"
#include "ov/protocol/chat.hpp"
// ── combat and interaction ───────────────────────────────────────────
#include "combat_session.hpp"
#include "mob_combat.hpp"
#include "player_level.hpp"
#include "ov/gameplay/food.hpp"
#include "ov/gameplay/item_use.hpp"
#include "ov/protocol/interaction.hpp"
// ── tnt and gravity ─────────────────────────────────────────────────
#include "tnt_gravity.hpp"
#include "rails_session.hpp"  // ── rails ──
#include "projectiles.hpp"  // ── projectiles ──
#include "brewing_session.hpp"  // ── brewing ──
#include "husbandry.hpp"    // ── husbandry ──
#include "taming.hpp"       // ── tame ──
#include "slimes.hpp"       // ── mobs-2 ──
#include "nether_mobs.hpp"  // ── nether-2 ──
#include "drowning.hpp"     // ── mobs-2 ──
#include "merchant_session.hpp"  // ── villagers ──
#include "mob_attacks.hpp"       // ── mobs-3 ──
#include "mob_despawn.hpp"       // ── mobs-3 ──
#include "entity_storage.hpp"    // ── mobs-3 ──
#include "zombie_villagers.hpp"  // ── mobs-3 ──
#include "dimension_entities.hpp"  // ── persistence ──
#include "ground_entities.hpp"     // ── persistence ── ItemEntity, GroundOrb

#include <fmt/format.h>

#include <algorithm>
#include <ranges>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Where the locally generated vanilla data lives. The build defines this as an
// absolute path; the fallback exists so the file still compiles on its own,
// which the header and syntax checks rely on. A wrong value is not silent — the
// path is printed when loading fails.
#ifndef OV_DATA_DIR
#define OV_DATA_DIR "data"
#endif

namespace {

using namespace ov;
using ov::server::SurvivalIo;
using ov::server::SurvivalOutcome;
using ov::server::SurvivalPlayer;
using ov::server::SurvivalSession;
using ov::server::EffectBearer;   // ── effects ──
using ov::server::NetherMobHost;  // ── nether-2 ──
using ov::server::NetherMobs;     // ── nether-2 ──
using ov::server::NetherMobStats; // ── nether-2 ──
using ov::server::NetherPlayer;   // ── nether-2 ──
using ov::server::EffectIo;       // ── effects ──
using ov::server::EffectSession;  // ── effects ──
// ── combat and interaction ───────────────────────────────────────────
using ov::server::CombatIo;
using ov::server::CombatOutcome;
using ov::server::CombatPlayer;
using ov::server::CombatSession;
// ── crafting and smelting ───────────────────────────────────────────────────
using namespace ov::server;

std::atomic<bool> g_stop_requested{false};

extern "C" void handle_signal(int) noexcept {
    // Only async-signal-safe work here: flip a flag and let the tick loop exit
    // on its own so the world is saved rather than truncated.
    g_stop_requested.store(true, std::memory_order_relaxed);
}

struct Options {
    ov::LogLevel log_level   = ov::LogLevel::Info;
    bool         show_help   = false;
    ov::i64      run_ticks   = -1;  // -1 runs until interrupted
    ov::u16      port        = 25565;
    std::string  motd        = "Ondes VOXEL";
    ov::i32      max_players = 20;

    /// Where to write every position a client reports, one line per packet.
    ///
    /// The client is the reference implementation of the player's physics and
    /// it says where it is twenty times a second. Recording that is the only
    /// way to measure gravity, drag and friction against the real thing rather
    /// than against a recollection of it.
    std::string record_motion;

    /// Creative keeps flight and instant breaking; survival makes blocks take
    /// the time the real game takes, which is the only mode where the break
    /// rules are exercised at all.
    bool survival = false;

    /// The world directory to serve, holding level.dat and region/.
    ///
    /// A flag rather than a constant because the test world is a *second*
    /// world: the bench in run/lab has to be servable without moving the
    /// player's own save out of the way first, and a bench nobody can open is
    /// a bench nobody uses.
    std::string world_dir = "run/world";

    /// Mobs to place near the spawn point, by registry name.
    ///
    /// A flag rather than a command because this server has no command parser
    /// yet, and because the point of it is to have something a real client can
    /// look at: `--mobs=zombie,cow,creeper` puts three of them on the ground in
    /// front of the spawn.
    std::vector<std::string> mobs;

    // ── effects ──
    /// Effects every player is given on joining: `--effect=speed:1:600,…`
    /// (name, amplifier, ticks). A test entry point until /effect exists —
    /// scripts/check_effects_e2e.py drives it — and named as one.
    std::vector<std::string> effects;

    // ── screens ──
    /// `--seed=<n>`: generate the overworld from this seed, as the client's
    /// Create World screen asks. Wins over OV_WORLDGEN_SEED; a world whose
    /// level.dat already declares a seeded generator needs neither.
    std::optional<ov::i64> seed;
    // ── end screens ──

    // ── player data ──
    /// The singleplayer host's name, set by ov_voxel --singleplayer. Their
    /// record also goes into level.dat's Data.Player, where vanilla looks for
    /// the player of a singleplayer world. Empty on a dedicated server.
    std::string host_player;
};

/// Where a connection is in the protocol's state machine.
///
/// A packet id means different things in different states — 0x00 is Handshake,
/// then Status Request, then Login Start — so the state has to be tracked per
/// connection or the ids are ambiguous.
enum class ConnectionState { Handshaking, Status, Login, Play };

/// The superflat preset, bottom to top: bedrock, two dirt, one grass.
///
/// A generator rather than a stored world, so a fresh server needs no save
/// files. It is also the one terrain whose block flags are known without the
/// flag table: all four of these stop movement, so MOTION_BLOCKING can be set
/// exactly rather than guessed.
struct Superflat {
    registry::BlockStateId bedrock{0};
    registry::BlockStateId dirt{0};
    registry::BlockStateId grass{0};
    world::AirStates       air{};
    u16                    biome{0};

    /// Needed by every chunk this makes: without it MOTION_BLOCKING and
    /// OCEAN_FLOOR would stay empty and the client would put rain in the ground.
    const registry::BlockRegistry* blocks{nullptr};

    /// The y of the topmost solid block. A player stands one above it.
    static constexpr i32 kSurfaceY = -61;

    [[nodiscard]] static Superflat from(const registry::BlockRegistry& blocks) {
        const auto state_of = [&blocks](std::string_view name) {
            const auto block = blocks.find_block(name);
            return block ? blocks.default_state(*block) : registry::BlockStateId{0};
        };
        return Superflat{
            state_of("minecraft:bedrock"),
            state_of("minecraft:dirt"),
            state_of("minecraft:grass_block"),
            world::AirStates::from(blocks),
            0,
            &blocks,
        };
    }

    [[nodiscard]] world::Chunk generate(ChunkPos position) const {
        world::Chunk chunk{position, world::WorldShape::overworld(), air, blocks};

        for (usize z = 0; z < 16; ++z) {
            for (usize x = 0; x < 16; ++x) {
                chunk.set_block(x, -64, z, bedrock);
                chunk.set_block(x, -63, z, dirt);
                chunk.set_block(x, -62, z, dirt);
                chunk.set_block(x, kSurfaceY, z, grass);
            }
        }

        chunk.fill_biome(biome);

        // Light comes from the heightmap, through the same function block edits
        // use. Two code paths that compute light differently agree right up
        // until someone digs.
        relight_chunk(chunk, blocks);
        if (blocks != nullptr) {
            relight_blocks(chunk, *blocks);
        }
        return chunk;
    }
};

/// The state a block takes when placed, given how the player placed it.
///
/// Vanilla decides this in each block's own Java code, so there is no data file
/// to read. What *is* data — and what the registry gives us — is which
/// properties a block has, so the rules below are applied only where the
/// property exists and the block is left at its default otherwise.
///
/// The conventions, and they are not uniform in vanilla either:
///
///   axis    from the face that was clicked. A log placed against a wall lies
///           down; one placed on the ground stands up.
///   facing  from the player's yaw. Which way is **not** uniform in vanilla, and
///           it is not guessable — see kFacesAwayFromPlayer below.
///   half    top when the click was on the underside of a block, or on the
///           upper half of a side. This is what puts a stair upside down.
///   type    the same rule for slabs. "double" needs to know that the target is
///           already a slab of the same kind, which is placement logic this
///           does not have yet.
///
/// Not handled: stair `shape`, which vanilla derives from the neighbours, so
/// corners will read as straight until that lands.
/// Blocks whose `facing` is the direction the player is looking, rather than
/// the opposite.
///
/// Vanilla is not uniform here and the split does not follow families, so this
/// was **measured** rather than reasoned: eighteen blocks were placed on a real
/// 1.20.1 server with the player facing south, and the resulting state read
/// back. Twelve came out north (facing the player) and four south.
///
/// Two of them are exactly why guessing does not work. An observer follows the
/// player's direction, which reads backwards from how the block behaves. A
/// trapdoor is opposite, although doors and fence gates — its obvious family —
/// are not.
///
/// Measured directly: stairs, fence gates, doors, observer. The suffix rules
/// extend those to the wood and stone variants, which share their Java class.
/// Anything not listed takes the majority convention, and an anvil is wrong in
/// a third way again — it came back rotated a quarter turn — so it is left at
/// its default rather than turned the wrong way with confidence.
[[nodiscard]] bool faces_away_from_player(std::string_view name) {
    return name == "minecraft:observer" || name.ends_with("_stairs") ||
           name.ends_with("_fence_gate") || name.ends_with("_door") || name.ends_with("_bed");
}

/// The compass direction a player at this yaw is looking along.
///
/// Yaw runs 0 at south and increases westward, so rounding to the nearest
/// quarter turn gives the direction faced.
[[nodiscard]] usize facing_index(f32 yaw) noexcept {
    const auto quarter = static_cast<i32>(std::floor(static_cast<f64>(yaw) / 90.0 + 0.5));
    // Modulo of a negative yaw is negative in C++, and a negative index would
    // read off the front of the array.
    return static_cast<usize>(((quarter % 4) + 4) % 4);
}

/// One step along a compass direction, in the order south, west, north, east.
[[nodiscard]] net::WirePosition step_towards(net::WirePosition from, usize direction) noexcept {
    switch (direction) {
        case 0: return {from.x, from.y, from.z + 1};   // south
        case 1: return {from.x - 1, from.y, from.z};   // west
        case 2: return {from.x, from.y, from.z - 1};   // north
        default: return {from.x + 1, from.y, from.z};  // east
    }
}

[[nodiscard]] registry::BlockStateId placed_state(const registry::BlockRegistry& blocks,
                                                  registry::BlockId              block,
                                                  const net::UseItemOn& place, f32 player_yaw) {
    registry::BlockStateId state = blocks.default_state(block);

    const auto set = [&](std::string_view name, std::string_view value) {
        const auto property = blocks.find_property(block, name);
        if (!property) {
            return;
        }
        for (u16 index = 0; index < property->values.size(); ++index) {
            if (property->values[index] == value) {
                state = blocks.with_property(state, *property, index);
                return;
            }
        }
    };

    // Faces are ordered -Y, +Y, -Z, +Z, -X, +X.
    switch (place.face) {
        case 0:
        case 1: set("axis", "y"); break;
        case 2:
        case 3: set("axis", "z"); break;
        default: set("axis", "x"); break;
    }

    // Yaw runs 0 at south and increases westward, so rounding to the nearest
    // quarter turn gives the direction the player faces.
    constexpr std::array<std::string_view, 4> kFacing{"south", "west", "north", "east"};
    const auto                                looking  = facing_index(player_yaw);
    const auto                                opposite = (looking + 2) % 4;
    set("facing", kFacing[faces_away_from_player(blocks.block_name(block)) ? looking : opposite]);

    const bool upper = place.face == 0 || (place.face >= 2 && place.cursor_y > 0.5F);
    set("half", upper ? "top" : "bottom");
    set("type", upper ? "top" : "bottom");

    // ── agriculture ── Leaves a player places never decay. Only leaves have
    // `persistent`; without it they land at distance 7 and the random tick
    // takes them.
    set("persistent", "true");

    return state;
}

/// An empty sign, as 1.20 stores one.
///
/// Both faces exist since 1.20 — a sign written only on the front and saved
/// without a `back_text` is refused by the client, which expects both. Each
/// line is a **chat component**, not a bare string: "hello" alone is not valid
/// where {"text":"hello"} is.
[[nodiscard]] nbt::Tag empty_sign_text() {
    nbt::Tag messages = nbt::Tag::make_list(nbt::TagType::String);
    for (int i = 0; i < 4; ++i) {
        messages.list()->push_back(nbt::Tag{std::string{R"({"text":""})"}});
    }
    nbt::Tag side = nbt::Tag::make_compound();
    side.compound()->push_back(nbt::CompoundEntry{"messages", std::move(messages)});
    side.compound()->push_back(nbt::CompoundEntry{"color", nbt::Tag{std::string{"black"}}});
    side.compound()->push_back(nbt::CompoundEntry{"has_glowing_text", nbt::Tag::make_bool(false)});
    return side;
}

[[nodiscard]] nbt::Tag new_sign_data() {
    nbt::Tag data = nbt::Tag::make_compound();
    data.compound()->push_back(nbt::CompoundEntry{"front_text", empty_sign_text()});
    data.compound()->push_back(nbt::CompoundEntry{"back_text", empty_sign_text()});
    data.compound()->push_back(nbt::CompoundEntry{"is_waxed", nbt::Tag::make_bool(false)});
    return data;
}

/// Escape a line into a chat component. A player can type a quote.
[[nodiscard]] std::string text_component(std::string_view line) {
    std::string json = R"({"text":")";
    for (const char c : line) {
        if (c == '"' || c == '\\') {
            json.push_back('\\');
        }
        json.push_back(c);
    }
    json += R"("})";
    return json;
}

/// The inventory of the block entity at a position, or nullopt when there is
/// no container there.
///
/// This replaced three hand-rolled readings of a chest's `Items` list, each of
/// which knew that a chest has twenty-seven slots and nothing else did. The
/// size, the shape and the sided rules all come from `block_container.hpp`
/// now, which is why a barrel and a hopper open at all.
[[nodiscard]] std::optional<BlockInventory> container_inventory(
    const world::BlockEntity* entity, const registry::Registries* registries,
    std::optional<registry::RegistryId> item_registry) {
    if (entity == nullptr) {
        return std::nullopt;
    }
    const ContainerSpec* spec = container_spec_for_entity(entity->type);
    if (spec == nullptr) {
        return std::nullopt;
    }
    BlockInventory inventory{*spec, registries, item_registry};
    inventory.load(entity->data);
    return inventory;
}

/// The numeric id a biome carries in the codec we sent.
///
/// The client learns biome ids from our codec and from nowhere else, so a chunk
/// must name biomes by *our* index. Reading it back out of the codec rather
/// than hard-coding a number keeps one source of truth: change the emitter and
/// this follows, instead of the world quietly rendering in the wrong biome's
/// colours.
[[nodiscard]] std::optional<u16> biome_id_in_codec(std::span<const u8> codec,
                                                   std::string_view    name) {
    const auto document = nbt::read(codec);
    if (!document) {
        return std::nullopt;
    }
    const nbt::Tag* registry = document->root.find("minecraft:worldgen/biome");
    if (registry == nullptr) {
        return std::nullopt;
    }
    const nbt::Tag* value = registry->find("value");
    if (value == nullptr || value->list() == nullptr) {
        return std::nullopt;
    }
    for (const nbt::Tag& entry : *value->list()) {
        const nbt::Tag* entry_name = entry.find("name");
        const nbt::Tag* entry_id   = entry.find("id");
        if (entry_name != nullptr && entry_id != nullptr && entry_name->as_string() == name) {
            return static_cast<u16>(entry_id->as_i64());
        }
    }
    return std::nullopt;
}

/// Every biome name in the codec, indexed by the id we assign it.
///
/// The disk format names biomes; we hold numbers. This is the table between the
/// two, and it comes from the codec so that saving and sending cannot disagree.
[[nodiscard]] std::vector<std::string> biome_names_in_codec(std::span<const u8> codec) {
    std::vector<std::string> names;
    const auto               document = nbt::read(codec);
    if (!document) {
        return names;
    }
    const nbt::Tag* registry = document->root.find("minecraft:worldgen/biome");
    const nbt::Tag* value    = registry == nullptr ? nullptr : registry->find("value");
    if (value == nullptr || value->list() == nullptr) {
        return names;
    }
    for (const nbt::Tag& entry : *value->list()) {
        const nbt::Tag* name = entry.find("name");
        const nbt::Tag* id   = entry.find("id");
        if (name == nullptr || id == nullptr) {
            continue;
        }
        const auto index = static_cast<usize>(id->as_i64());
        if (names.size() <= index) {
            names.resize(index + 1);
        }
        names[index] = std::string{name->as_string()};
    }
    return names;
}

/// The region file a chunk belongs to. 32x32 chunks per region, and the floor
/// division has to be arithmetic: chunk -1 lives in region -1, not region 0.
[[nodiscard]] std::filesystem::path region_path(const std::filesystem::path& directory, i32 cx,
                                                i32 cz) {
    return directory / fmt::format("r.{}.{}.mca", cx >> 5, cz >> 5);
}

/// What the server tracks per connected player.
/// The Efficiency level on an item, read from the tag the protocol carries.
///
/// The stack's NBT is kept as raw bytes on purpose — re-encoding a tag we do
/// not understand is how data gets lost — so it is decoded only when a number
/// is actually needed from it, which is here.
[[nodiscard]] u8 enchantment_level(const net::ItemStack& stack, std::string_view enchantment) {
    if (stack.nbt.empty()) {
        return 0;
    }
    const auto document = nbt::read(stack.nbt);
    if (!document) {
        return 0;
    }
    const nbt::Tag* list = document->root.find("Enchantments");
    if (list == nullptr || list->list() == nullptr) {
        return 0;
    }
    for (const nbt::Tag& entry : *list->list()) {
        const nbt::Tag* id  = entry.find("id");
        const nbt::Tag* lvl = entry.find("lvl");
        if (id != nullptr && lvl != nullptr && id->as_string() == enchantment) {
            return static_cast<u8>(std::clamp<i64>(lvl->as_i64(), 0, 255));
        }
    }
    return 0;
}

// ── persistence ── `ItemEntity` and `GroundOrb` live in ground_entities.hpp,
// with what writes them into entities/.

struct Player {
    /// Held so the tick thread can send keep-alives without going through the
    /// packet handler. Dropped in on_disconnect, which is what keeps this from
    /// pinning a closed socket forever.
    net::ConnectionPtr connection;

    i32  entity_id{0};
    i32  pending_teleport{-1};
    bool confirmed{false};
    i64  last_keep_alive_sent_ms{0};
    i64  keep_alive_id{0};
    bool awaiting_keep_alive{false};

    /// ── sound ── how far this player has walked, in footsteps.
    Sounds::Stride stride{};

    /// The player's own 46 slots, as the protocol numbers them: 0 is the
    /// crafting result, 1..4 the grid, 5..8 armour, 9..35 the main inventory,
    /// 36..44 the hotbar, 45 the off hand.
    ///
    /// In creative the client owns this and tells us about every change, which
    /// is the only way to know what a player is holding.
    std::array<net::ItemStack, 46> inventory{};
    i16                            held_slot{0};

    /// A drag in progress: the slots painted so far, and which button started
    /// it. Vanilla calls this "painting" and sends it as three packets — start,
    /// each slot, end — so the state has to live between them.
    std::vector<i16> drag_slots;
    i8               drag_button{-1};

    /// The same thing for the player's own screen, and deliberately not the
    /// same variable. A player can have a chest open and their own inventory
    /// showing beneath it; one drag state shared between the two windows would
    /// paint slots of one window with the other's numbering.
    DragState drag;

    // ── crafting and smelting ───────────────────────────────────────────────
    /// The crafting table or furnace screen this player has open. Separate from
    /// `window_open` below, which is the chest's: the two screens obey
    /// different rules and sharing one flag would make a click land in both.
    std::optional<Workbench> bench;
    // ── brewing ── the brewing stand screen, window 3.
    std::optional<BrewingWindow> brewing_window;

    // ── enchanting ──────────────────────────────────────────────────────────
    /// The table, anvil or grindstone this player has open.
    std::optional<EnchantWindow> enchant;
    /// `XpSeed`: what the table offers this player until they next enchant.
    i32 xp_seed{0};
    /// The player's own generator: the next seed, the anvil's 12 %, the
    /// grindstone's refund, which item Mending picks.
    math::LegacyRandomSource enchant_random{0x454E4348};
    /// The worn pieces' tags when their EPF was last computed.
    std::array<std::vector<u8>, 4> worn_seen{};
    /// Respiration on the helmet, cached with the EPF so a submerged tick
    /// parses no NBT.
    i32 respiration_level{0};
    // ── end enchanting ──────────────────────────────────────────────────────

    /// The container this player has open, if any.
    u8             window_id{0};
    bool           window_open{false};
    net::ItemStack carried{};
    i32            window_x{0};
    i32            window_y{0};
    i32            window_z{0};
    /// How many slots that container has, and which one it is.
    ///
    /// Hard-coded to a chest's 27 until the container model existed, which is
    /// why a barrel opened as a chest and a hopper could not be opened at all:
    /// the boundary between the container's slots and the player's is this
    /// number, and a wrong one moves items between the wrong two halves without
    /// any error anywhere.
    const ContainerSpec* window_spec{nullptr};
    f64            x{0.5};
    f64            y{static_cast<f64>(Superflat::kSurfaceY) + 1.0};
    f64            z{0.5};
    f32            yaw{0.0F};
    f32            pitch{0.0F};
    bool           on_ground{true};

    /// The block this player is breaking, and when they started.
    ///
    /// Vanilla's own shape, because the client predicts against it: the server
    /// does not finish the job on its own but waits to be told, and only falls
    /// back to its own clock when the claim arrives too early to believe.
    bool digging{false};
    bool delayed_dig{false};
    i32  dig_x{0};
    i32  dig_y{0};
    i32  dig_z{0};
    i64  dig_started_tick{0};
    /// ── breaking ── What the others were last told about this dig.
    DestroyStageState destroy_stage;

    /// Health, hunger and experience, and the packets they owe this client.
    /// Everything about it lives in survival_session.{hpp,cpp}.
    SurvivalSession survival;

    // ── effects ──────────────────────────────────────────────────────────
    /// Status effects and attributes, and the packets they owe this client.
    /// Everything about it lives in effect_session.{hpp,cpp}; /effect calls
    /// `effects.apply` / `remove` / `clear` on it.
    EffectSession effects;
    /// `--effect=` has been applied to this player.
    bool effects_started{false};

    // ── commands ─────────────────────────────────────────────────────────
    /// 0 survival, 1 creative, 2 adventure, 3 spectator. Per player, set by
    /// /gamemode; `--survival` only chooses the world's default.
    u8 game_mode{1};
    /// 0..4, from ops.json, set when the command engine greets the player.
    i32 permission{0};
    /// Survival and adventure take damage, get hungry and break blocks in
    /// time; creative and spectator do none of it.
    [[nodiscard]] bool mortal() const noexcept { return game_mode == 0 || game_mode == 2; }
    // ── end commands ─────────────────────────────────────────────────────
    // ── end effects ──────────────────────────────────────────────────────

    // ── combat and interaction ───────────────────────────────────────────
    /// The attack gauge, the eat in progress, and the four verbs' state.
    /// Everything about it lives in combat_session.{hpp,cpp}.
    CombatSession combat;
    /// Where this player was on the previous tick, so the tick can hand the
    /// combat session the step it needs for the sweep. A sweep wants a
    /// *standing* attacker, and only the server can measure that.
    f64  combat_last_x{0.0};
    f64  combat_last_z{0.0};
    bool combat_last_valid{false};
    /// The client's own sneak flag. It decides whether right-clicking a chest
    /// with a block in hand opens the chest or places the block, and it is
    /// unknowable from anything else the client sends.
    bool sneaking{false};
    /// Set by the network thread when the client presses "respawn", acted on by
    /// the tick. A flag rather than a call, because respawning moves the player
    /// and only the tick thread may do that.
    bool wants_respawn{false};

    /// The key this player is remembered under between sessions.
    std::string identity;

    /// Name and uuid, kept so other players can be told who this is.
    std::string name;
    net::Uuid   uuid;

    /// Where this player was last broadcast from, so a stationary player costs
    /// nothing. Twenty position packets a second per idle player is most of the
    /// traffic on a busy server.
    f64  broadcast_x{0.0};
    f64  broadcast_y{0.0};
    f64  broadcast_z{0.0};
    f32  broadcast_yaw{0.0F};
    bool broadcast_valid{false};

    /// Chunks this client currently holds, so streaming sends each one once and
    /// unloads exactly what left range. Recomputing the difference from the
    /// player's position alone would be equivalent right up to the first time a
    /// send fails or the view distance changes.
    std::unordered_set<i64> loaded_chunks;
    /// Chunks decided on but not yet sent, nearest first.
    ///
    /// Sending the whole square in one tick overran it: 289 chunks encoded and
    /// written while the tick clock keeps running, and the server reported
    /// "can't keep up" on every join. So the set is chosen when the player
    /// crosses a chunk boundary and drained under a budget afterwards.
    ///
    /// Nearest first matters as much as the budget. The old code iterated an
    /// unordered_set, so chunks arrived in hash order and the world assembled
    /// itself in patches around the player rather than outwards from it.
    std::vector<i64> pending_chunks;
    i32              centre_x{0};
    i32              centre_z{0};
    bool             streaming{false};

    // ── nether ──
    /// The level this player stands in. Everything that reads or writes a
    /// block for them, streams them chunks or shows them to others goes
    /// through it. See nether_travel.hpp.
    DimensionId dimension{DimensionId::Overworld};
    /// Standing in a portal, and the cooldown after a crossing.
    PortalTimer portal;
    /// The portal has fired and the crossing waits for the chunks round the
    /// portal it may have to build — generated by the workers, not the tick.
    bool crossing{false};
    i32  crossing_ticks{0};
    // ── end nether ──
    // ── end ── An End portal took the player, and the crossing waits for the
    // platform's chunks; or the exit portal did, and the credits are rolling
    // until the client asks to respawn.
    bool end_crossing{false};
    i32  end_crossing_ticks{0};
    bool won_game{false};
    bool seen_credits{false};
};

/// Where a player was when they last left.
///
/// In memory only: a server restart forgets it, exactly as it forgets the
/// chunks. Persisting one without the other would put someone back inside a
/// block that no longer exists, so both wait for the Anvil work together.
struct SavedPlayer {
    f64 x{0.5};
    f64 y{static_cast<f64>(Superflat::kSurfaceY) + 1.0};
    f64 z{0.5};
    f32 yaw{0.0F};
    f32 pitch{0.0F};
};

Options parse_args(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--help" || arg == "-h") {
            options.show_help = true;
        } else if (arg.starts_with("--record-motion=")) {
            options.record_motion = std::string{arg.substr(16)};
        } else if (arg == "--survival") {
            options.survival = true;
        } else if (arg.starts_with("--effect=")) {  // ── effects ──
            std::string_view list = arg.substr(9);
            while (!list.empty()) {
                const auto comma = list.find(',');
                if (!list.substr(0, comma).empty()) {
                    options.effects.emplace_back(list.substr(0, comma));
                }
                list = comma == std::string_view::npos ? std::string_view{}
                                                       : list.substr(comma + 1);
            }
        } else if (arg.starts_with("--host-player=")) {  // ── player data ──
            options.host_player = std::string{arg.substr(14)};
        } else if (arg.starts_with("--mobs=")) {
            std::string_view list = arg.substr(7);
            while (!list.empty()) {
                const auto  comma = list.find(',');
                const auto  piece = list.substr(0, comma);
                std::string name{piece};
                if (!name.empty()) {
                    // Bare names are accepted because typing the namespace on
                    // a command line is friction with no upside; anything with
                    // a colon is passed through as written.
                    if (name.find(':') == std::string::npos) {
                        name = "minecraft:" + name;
                    }
                    options.mobs.push_back(std::move(name));
                }
                if (comma == std::string_view::npos) {
                    break;
                }
                list = list.substr(comma + 1);
            }
        } else if (arg.starts_with("--port=")) {
            const auto value  = arg.substr(7);
            ov::i32    parsed = 0;
            if (std::from_chars(value.data(), value.data() + value.size(), parsed).ec ==
                    std::errc{} &&
                parsed > 0 && parsed < 65536) {
                options.port = static_cast<ov::u16>(parsed);
            } else {
                OV_LOG_WARN("invalid --port value '{}', ignoring", value);
            }
        } else if (arg.starts_with("--world=")) {
            options.world_dir = std::string{arg.substr(8)};
        } else if (arg.starts_with("--seed=")) {  // ── screens ──
            const auto value  = arg.substr(7);
            ov::i64    parsed = 0;
            const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (ec == std::errc{} && end == value.data() + value.size()) {
                options.seed = parsed;
            } else {
                OV_LOG_WARN("invalid --seed value '{}', ignoring", value);
            }
        } else if (arg.starts_with("--motd=")) {
            options.motd = arg.substr(7);
        } else if (arg.starts_with("--log-level=")) {
            options.log_level = ov::parse_log_level(arg.substr(12));
        } else if (arg.starts_with("--ticks=")) {
            // Bounded runs make the server usable from tests and CI without a
            // watchdog around it. from_chars, not strtoll: a string_view is not
            // null-terminated and strtoll would read past the end of it.
            const std::string_view value  = arg.substr(8);
            ov::i64                parsed = 0;
            const auto [ptr, ec] =
                std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (ec == std::errc{} && ptr == value.data() + value.size() && parsed >= 0) {
                options.run_ticks = parsed;
            } else {
                OV_LOG_WARN("invalid --ticks value '{}', ignoring", value);
            }
        } else {
            OV_LOG_WARN("unknown argument '{}' (try --help)", arg);
        }
    }
    return options;
}

void print_help() {
    fmt::print(
        "Ondes VOXEL dedicated server\n"
        "\n"
        "  --port=<n>                                      listen port (default: 25565)\n"
        "  --world=<dir>                                   world to serve (default: run/world)\n"
        "  --motd=<text>                                   server list description\n"
        "  --log-level=<trace|debug|info|warn|error|off>   verbosity (default: info)\n"
        "  --ticks=<n>                                     stop after n ticks\n"
        "  --survival                                      survival mode: blocks take time\n"
        "  --seed=<n>                                      generate the overworld from a seed\n"
        "  --record-motion=<file>                          log every reported position\n"
        "  --mobs=<name,name,...>                          place mobs near the spawn point\n"
        "  --effect=<name:amp:ticks,...>                   effects given to every joining player\n"
        "  --help, -h                                      this message\n"
        "\n"
        "Not an official Minecraft product. Not approved by or associated with Mojang.\n");
}

}  // namespace

int ov::server::run(int argc, char** argv, const std::atomic<bool>* external_stop,
                    const std::atomic<bool>* external_pause) {
    using namespace ov;

    const Options options = parse_args(argc, argv);
    if (options.show_help) {
        print_help();
        return 0;
    }

    set_log_level(options.log_level);

    // First statement on the thread that must hold 20 Hz. Without this, macOS
    // migrates it to an efficiency core under load and TPS quietly drops to the
    // mid-teens with nothing in a profile to explain it.
    set_thread_role("ov-tick", ThreadRole::Tick);

    OV_LOG_INFO("Ondes VOXEL dedicated server — target Minecraft 1.20.1 (protocol 763)");
    OV_LOG_INFO("workers available: {}", recommended_worker_count());

    // Only when nobody else owns the process. An integrated server shares one
    // with a window that has its own idea of when to quit, and two handlers
    // fighting over the same process is a bug nobody enjoys finding.
    if (external_stop == nullptr) {
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);
    }

    // ── Network ─────────────────────────────────────────────────────────────
    // Bound before the tick loop starts: failing to bind is worth saying
    // immediately rather than after the world has loaded.
    auto listener = net::Listener::bind(options.port);
    if (!listener) {
        OV_LOG_ERROR("could not listen on port {} — is another server running?", options.port);
        return 1;
    }

    net::ServerStatus status;
    status.description = options.motd;
    status.max_players = options.max_players;

    // The data a player needs before they can be let in. Both are generated
    // locally from the official jar and never committed; without them the
    // server can still answer a ping, so a status-only server is a useful
    // fallback rather than a fatal error.
    const std::filesystem::path data_dir{OV_DATA_DIR};
    const auto                  blocks =
        registry::BlockRegistry::load(data_dir / "vanilla" / "1.20.1" / "registry.ovpack");
    const auto codec_bytes = io::read_file(data_dir / "vanilla" / "1.20.1" / "registry_codec.nbt");

    const bool world_available = blocks.has_value() && codec_bytes.has_value();
    if (!world_available) {
        OV_LOG_WARN("no registry pack or codec under {} — players can ping but not join",
                    data_dir.string());
        OV_LOG_WARN("run tools/ov_datagen/datagen.py, then ovpack.py and codec.py");
    } else {
        OV_LOG_INFO("registry: {} blocks, {} states; codec {} bytes", blocks->block_count(),
                    blocks->state_count(), codec_bytes->size());
    }
    // ── movement physics ── what ice, ladders, slime, cobwebs and bubble
    // columns do to a mob, resolved once from the registry.
    std::optional<gameplay::BlockMotionTable> block_motion;
    if (blocks) {
        block_motion.emplace(*blocks);
    }

    // Item ids, needed to turn "the player is holding this" into a block.
    const auto registries =
        registry::Registries::load(data_dir / "vanilla" / "1.20.1" / "registry.ovpack");
    const auto item_registry = registries ? registries->find("minecraft:item") : std::nullopt;
    const auto block_entity_registry =
        registries ? registries->find("minecraft:block_entity_type") : std::nullopt;

    // The menu type a container opens. minecraft:menu is another registry the
    // client hard-codes, so a 9x3 chest is not the same number as a 9x6 one and
    // guessing opens a window of the wrong size over the right data.
    const auto menu_registry = registries ? registries->find("minecraft:menu") : std::nullopt;
    const auto menu_id       = [&](std::string_view name) -> i32 {
        // 2 is generic_9x3's own id and is the only sane fallback: a client
        // that is told a menu id it does not have closes the screen at once.
        return registries && menu_registry
                         ? registries->protocol_id(*menu_registry, name).value_or(2)
                         : 2;
    };

    // How long each block takes to break, and whether the held tool lets it
    // drop. Built once: every query otherwise walks the tag graph, and this
    // sits on the path of every dig packet.
    std::optional<gameplay::BreakRules>  break_rules;
    std::optional<gameplay::LootTables>  loot_tables;
    std::optional<gameplay::Connections> connections;
    if (blocks && registries) {
        break_rules.emplace(*blocks, *registries);
        loot_tables.emplace(*blocks, *registries);
        connections.emplace(*blocks, *registries);
    }

    // ── combat and interaction ──────────────────────────────────────────────
    //
    // The right-click rules, and the tables a killed mob draws on. Both were
    // written, measured and left unwired by the wave that produced them; this
    // is where they are read in. `ItemUse` resolves its registry lookups once
    // for the same reason `BreakRules` does — it sits on the path of every
    // Use Item On packet.
    std::optional<gameplay::ItemUse> item_use;
    if (blocks && registries) {
        item_use.emplace(*blocks, *registries);
    }

    // ── crafting and smelting ───────────────────────────────────────────────
    // The recipe book, and the handful of registry ids the crafting screens
    // need. Built once for the same reason the break rules are: matching a grid
    // otherwise walks the whole pack every time a player moves an item.
    std::optional<gameplay::RecipeBook> recipe_book;
    WorkbenchContext                    workbench_context;
    if (registries) {
        recipe_book.emplace(*registries);
        workbench_context.registries = &*registries;
        workbench_context.book       = &*recipe_book;
        if (const auto items = registries->find("minecraft:item")) {
            workbench_context.item_registry = *items;
        }
        if (const auto menus = registries->find("minecraft:menu")) {
            workbench_context.menu_registry = *menus;
        }
    }

    // ── enchanting ──
    EnchantContext enchant_context;
    if (registries) {
        enchant_context.registries    = &*registries;
        enchant_context.item_registry = workbench_context.item_registry;
        enchant_context.menu_registry = workbench_context.menu_registry;
        if (const auto block_ids = registries->find("minecraft:block")) {
            enchant_context.block_registry = *block_ids;
        }
    }

    // ── combat and interaction ──────────────────────────────────────────────
    //
    // What a mob leaves behind. Its own pack rather than a section of
    // registry.ovpack, for the reason the pack itself gives: the registry pack
    // is shared by every branch in flight. The recipe book is handed in because
    // nineteen tables smelt their own drop — a burning cow drops steak.
    std::optional<gameplay::EntityLootTables> entity_loot;
    std::optional<MobCombat>                  mob_combat;
    if (registries) {
        entity_loot = load_entity_loot(data_dir / "vanilla" / "1.20.1" / "entity_loot.ovpack",
                                       *registries, recipe_book ? &*recipe_book : nullptr);
        mob_combat.emplace(*registries, entity_loot ? &*entity_loot : nullptr);
    }
    /// The draw for a mob's table. A source of its own rather than
    /// `loot_random`: a block broken and a mob killed on the same tick must be
    /// two draws, and sharing one stream would make the block's drop depend on
    /// whether something died beside it.
    math::XoroshiroRandomSource mob_loot_random{0x0BADC0DEFEEDFACEULL, 0x00C0FFEE12345678ULL};
    gameplay::DamageConstants   mob_damage_constants{};

    Superflat superflat = world_available ? Superflat::from(*blocks) : Superflat{};
    if (world_available) {
        const auto plains = biome_id_in_codec(*codec_bytes, "minecraft:plains");
        if (!plains) {
            OV_LOG_WARN(
                "codec has no minecraft:plains — the world will render in "
                "whichever biome sits at id 0");
        } else {
            superflat.biome = *plains;
            OV_LOG_INFO("biome minecraft:plains is id {} in our codec", *plains);
        }
    }

    // Chunks are generated once and reused. A superflat column is identical
    // everywhere but its coordinates, so this is a cache of one shape rather
    // than a world — the real ChunkMap arrives with the tick scheduler.
    // The world on disk. Under run/, which is gitignored — a save is the
    // player's, not the repository's.
    // The tick the server is on, readable from the network threads. Breaking
    // is counted in ticks, and the packet handler runs on another thread.
    std::atomic<i64> server_tick{0};
    // ── loading: how much of the spawn area is resident, 0 to 100. Written by
    // the tick thread, read by the login path, which refuses with it until it
    // reaches 100 — vanilla's server does not even listen until then ──
    std::atomic<i32> spawn_ready_percent{0};
    // ── spawn eggs: asked for on the network thread, spawned by the tick ──
    std::mutex                                 egg_mutex;
    std::vector<std::pair<std::string, Vec3d>> egg_requests;

    std::FILE* motion_log = nullptr;
    if (!options.record_motion.empty()) {
        motion_log = std::fopen(options.record_motion.c_str(), "w");
        if (motion_log == nullptr) {
            OV_LOG_WARN("cannot write {}", options.record_motion);
        } else {
            OV_LOG_INFO("recording reported positions to {}", options.record_motion);
        }
    }

    // Les piles au sol. Sous le même verrou que les joueurs : elles n'existent
    // que pour être ramassées, et le ramassage lit les deux.
    std::vector<ItemEntity> ground_items;
    /// Experience orbs on the ground. Beside the items for the same reason:
    /// both are entities the tick owns and the network thread never touches.
    std::vector<GroundOrb> ground_orbs;

    // Le tirage des butins. Une seule source, sur le thread de tick : deux
    // joueurs qui cassent le même bloc au même tick doivent obtenir deux
    // tirages, pas le même.
    math::XoroshiroRandomSource loot_random{0x243F6A8885A308D3ULL, 0x13198A2E03707344ULL};

    const std::filesystem::path level_dir{options.world_dir};
    const std::filesystem::path world_dir = level_dir / "region";
    std::error_code             directory_error;
    std::filesystem::create_directories(world_dir, directory_error);

    // A save from another version is refused rather than opened. Vanilla
    // upgrades old worlds through a converter this project does not have, and a
    // newer one may use shapes it does not know — so opening either would mean
    // reading a file we do not understand and, worse, writing it back.
    // What the world says about itself. The defaults describe the superflat this
    // server generates; a level.dat on disk overrides them and is written back
    // unchanged, so that opening a world never silently moves its spawn. A test
    // world is only a bench if it is still the same world tomorrow.
    world::LevelSettings level_settings;
    level_settings.spawn_y = Superflat::kSurfaceY + 1;
    level_settings.layers  = {
        {"minecraft:bedrock", 1},
        {"minecraft:dirt", 2},
        {"minecraft:grass_block", 1},
    };

    if (const auto level_bytes = io::read_file(level_dir / "level.dat")) {
        i32 stored = -1;
        // level.dat is gzipped, and nbt::read takes plain NBT. Handing it the
        // compressed bytes fails quietly, which reads as "no version declared"
        // — and would refuse every world, including the ones we wrote.
        const auto plain = io::gzip_decompress(*level_bytes);
        if (const auto document = plain ? nbt::read(*plain) : nbt::read(*level_bytes)) {
            if (const nbt::Tag* data = document->root.find("Data")) {
                if (const nbt::Tag* version = data->find("DataVersion")) {
                    stored = static_cast<i32>(version->as_i64());
                }
                if (const nbt::Tag* name = data->find("LevelName")) {
                    level_settings.name = std::string{name->as_string()};
                }
                const auto spawn_of = [&](std::string_view key, i32& into) {
                    if (const nbt::Tag* value = data->find(key)) {
                        into = static_cast<i32>(value->as_i64());
                    }
                };
                spawn_of("SpawnX", level_settings.spawn_x);
                spawn_of("SpawnY", level_settings.spawn_y);
                spawn_of("SpawnZ", level_settings.spawn_z);
                // ── commands: the clock, weather, difficulty and rules too ──
                world::read_level_settings(*data, level_settings);
            }
        }
        if (stored != world::kDataVersion1201) {
            OV_LOG_ERROR("{} was written by data version {}, and this is {} (Minecraft 1.20.1)",
                         (level_dir / "level.dat").string(), stored, world::kDataVersion1201);
            OV_LOG_ERROR(
                "refusing to open it: there is no converter here, and writing it back "
                "would damage the save");
            return 1;
        }
        OV_LOG_INFO("world data version {} — Minecraft 1.20.1", stored);
    }

    // ── player data ─────────────────────────────────────────────────────────
    // world/playerdata/<uuid>.dat in vanilla's format; see player_data.hpp.
    PlayerDataStore player_data{level_dir,
                                ItemNames{registries ? &*registries : nullptr, item_registry},
                                options.host_player};
    player_data.load_level_player(level_dir / "level.dat");
    // ── end player data ─────────────────────────────────────────────────────

    std::vector<std::string> biome_name_storage =
        codec_bytes ? biome_names_in_codec(*codec_bytes) : std::vector<std::string>{};
    std::vector<std::string_view> biome_names;
    biome_names.reserve(biome_name_storage.size());
    for (const auto& name : biome_name_storage) {
        biome_names.emplace_back(name);
    }

    // ── Generated terrain ───────────────────────────────────────────────────
    //
    // Off unless `OV_WORLDGEN_SEED` names a seed, and the superflat path below
    // is untouched when it does not. An environment variable rather than a flag
    // on purpose: this is the first user of the chunk pipeline and it belongs
    // with the ChunkMap and the ticket system when those land, not bolted onto
    // an argument parser several people are editing at once.
    //
    //     OV_WORLDGEN_SEED=1234567890 ov_dedicated --world=run/generated
    //
    // What it gives is a real overworld: noise, biomes, surface rules, carvers
    // and — for the first time — the feature stage, so the stone has ores in
    // it. `GeneratedWorld` owns the whole worldgen stack and the pipeline that
    // holds the nine chunks decoration needs; see generated_world.hpp.
    std::unique_ptr<GeneratedWorld>   generated;
    std::unique_ptr<AsyncChunkSource>  chunk_source;
    usize                              generation_workers = 0;
    // ── screens ── --seed, then the environment, then a level.dat that already
    // declares a seeded overworld (reopened from the world list).
    std::optional<i64> requested_seed = options.seed;
    if (const char* seed_text = std::getenv("OV_WORLDGEN_SEED"); !requested_seed && seed_text) {
        requested_seed = std::strtoll(seed_text, nullptr, 10);
    }
    if (!requested_seed && level_settings.generated) {
        requested_seed = level_settings.seed;
    }
    if (requested_seed && world_available && registries) {
        const i64 world_seed      = *requested_seed;
        level_settings.seed       = world_seed;  // ── commands: what /seed answers ──
        level_settings.generated  = true;        // ── screens ──

        // One worldgen stack per worker, plus one for the tick thread's own
        // synchronous fallback. Overridable because the right number depends on
        // the machine and on what else it is doing, and because a run with one
        // worker is how the determinism check gets its serial arm.
        generation_workers = recommended_worker_count();
        if (const char* worker_text = std::getenv("OV_WORLDGEN_WORKERS")) {
            const long parsed  = std::strtol(worker_text, nullptr, 10);
            generation_workers = parsed < 0 ? usize{0} : static_cast<usize>(parsed);
        }
        generated = GeneratedWorld::load(data_dir, *blocks, *registries, biome_names, world_seed,
                                         generation_workers + 1);
        if (!generated) {
            OV_LOG_ERROR("OV_WORLDGEN_SEED was set but the generator could not be built");
            return 1;
        }
        chunk_source = std::make_unique<AsyncChunkSource>(*generated, generation_workers);
    }
    // ────────────────────────────────────────────────────────────────────────

    // ── commands ────────────────────────────────────────────────────────────
    //
    // The command engine, and the world state it owns: the clock, the weather,
    // the difficulty and the gamerules, read from level.dat above and written
    // back by save_world. See commands/service.hpp for who may call what from
    // which thread. `--survival` makes survival the world's default game mode.
    if (options.survival) {
        level_settings.game_type = 0;
    }
    std::unique_ptr<cmd::CommandService> commands;
    if (blocks && registries) {
        cmd::ServiceConfig command_config;
        command_config.blocks     = &*blocks;
        command_config.registries = &*registries;
        command_config.integrated = external_stop != nullptr;
        // ops.json beside the server, where vanilla keeps it. An integrated
        // server has none: its players are the owner, at level 4.
        command_config.ops_file =
            command_config.integrated ? std::filesystem::path{} : std::filesystem::path{"ops.json"};
        command_config.lang_file = data_dir.parent_path() / "run" / "assets" / "assets" /
                                   "minecraft" / "lang" / "en_us.json";
        // ── allow-commands ── Allow Cheats (level.dat) applies to the host.
        command_config.host_player = options.host_player;
        command_config.max_players = options.max_players;
        command_config.motd        = options.motd;
        commands = std::make_unique<cmd::CommandService>(std::move(command_config));
        commands->load_world(level_settings);
        commands->console = [](std::string_view line) { OV_LOG_INFO("{}", line); };
    }
    // ── end commands ────────────────────────────────────────────────────────

    /// What `chunk_mutex` still protects, and what it no longer does.
    ///
    /// It guards the map itself — a lookup, an insert, an eviction — because
    /// the network thread reads chunks while the tick thread publishes them,
    /// and that arrangement predates this work.
    ///
    /// What it used to guard as well was **generation**: `chunk_at` ran the
    /// whole worldgen pipeline with the lock held, hundreds of milliseconds at
    /// a time, and any thread that wanted a block waited behind it. That is
    /// gone. Terrain is built by `chunk_source` on its own threads, out of any
    /// lock, on chunks nobody can see, and arrives here by move on the tick
    /// thread. What is left under the lock is bookkeeping measured in
    /// microseconds. See docs/provenance/chunkmap.md § 5 for the numbers.
    std::mutex              chunk_mutex;
    world::ChunkMap         chunks;

    // ── light ── The Overworld's light, kept current edit by edit. Used only
    // under `chunk_mutex`, like the map it lights. A chunk joins lit: alone
    // first, then across its borders (`light_arrived`); an edit is noted where
    // it is written and repaired once per tick by `flush_tick_writes`.
    MapLightSource                    light_chunks{chunks};
    std::optional<world::LightEngine> light;
    if (blocks) {
        light.emplace(*blocks, kOverworldLight);
    }
    /// Measurement only: `OV_LIGHT_FULL=1` repairs edits the way the server did
    /// before this engine — the 3x3 around each chunk touched, from nothing —
    /// so that one binary measures both sides of scripts/bench_play.py.
    const bool light_full_recompute = [] {
        const char* setting = std::getenv("OV_LIGHT_FULL");
        return setting != nullptr && std::string_view{setting} == "1";
    }();
    /// Caller holds chunk_mutex. Pending edits go first, so that the stitch
    /// never floods from light an edit has already made stale.
    const auto light_arrived = [&](ChunkPos pos, bool keep_sky) {
        if (!light) {
            return;
        }
        world::Chunk* chunk = chunks.find(pos);
        if (chunk == nullptr) {
            return;
        }
        if (light->pending() > 0) {
            (void)light->propagate(light_chunks);
        }
        light->light_chunk(*chunk, keep_sky);
        (void)light->stitch(light_chunks, pos);
    };
    // ── end light ──

    /// The level a block behaviour writes through, and the two queues it wakes
    /// from. Declared here rather than where they are built because both
    /// `chunk_at` and `save_world` reach for them — a chunk read from disk
    /// carries pending ticks that have to land in the queues, and a chunk
    /// written back has to carry its own share of them out again.
    ///
    /// Empty until the registries are loaded, which is the only state in which
    /// fluids and redstone do nothing at all.
    std::optional<ServerLevel> level;
    std::optional<WorldTicks>  world_ticks;

    std::unordered_set<i64> dirty_chunks;

    /// Chunks the tick thread had to generate itself because something needed a
    /// block in them right now — a dig, a place, a fluid step. Counted rather
    /// than hidden: every one of these is a tick that paid full worldgen price,
    /// and if the number is not near zero the streaming path is not doing its
    /// job.
    u64 synchronous_generations = 0;
    /// The same, on any other thread — the network thread, reading a neighbour
    /// block across a border into a chunk that is not resident. ── perf ──
    /// Touched only under `chunk_mutex`, like the counter above.
    u64 synchronous_generations_elsewhere = 0;

    // Chunks that exist on disk and could not be read. They are served as
    // generated terrain so the player is not left in a hole, and never written
    // back: overwriting a chunk we failed to understand would destroy the save
    // we were asked to open.
    std::unordered_set<i64> read_only_chunks;
    const auto              chunk_key = [](i32 cx, i32 cz) {
        return (static_cast<i64>(cx) << 32) ^ static_cast<u32>(cz);
    };

    /// The block state an item places, or nothing if the item is not a block.
    ///
    /// Items and blocks are separate registries with separate ids, and the
    /// bridge between them is the name: `minecraft:stone` the item places
    /// `minecraft:stone` the block. Most items that are not blocks simply have
    /// no block of the same name, which is exactly the test.
    const auto block_for_item = [&](i32 item_id) -> std::optional<registry::BlockId> {
        if (!registries || !item_registry || !blocks) {
            return std::nullopt;
        }
        const std::string_view name = registries->entry_of(*item_registry, item_id);
        if (name.empty()) {
            return std::nullopt;
        }
        return blocks->find_block(name);
    };

    std::unordered_map<const net::Connection*, Player> players;
    std::unordered_map<std::string, SavedPlayer>       saved_players;
    std::mutex                                         players_mutex;
    std::atomic<i32>                                   next_entity_id{1};

    // The mobs. Their wire ids start a million above the players' so that the
    // two allocators cannot meet: EntityWorld hands out its own, and there is
    // no shared counter to forget to bump. A client does not care what the
    // numbers are, only that no two live entities share one.
    std::optional<entity::EntityWorld> mobs;
    if (registries) {
        mobs.emplace(*registries, 1'000'000);
    }

    /// A uuid derived from the wire id rather than drawn at random.
    ///
    /// Determinism (CLAUDE.md principle 5): two runs of the same server must
    /// produce the same world, and a random uuid would make every capture and
    /// every replay differ in sixteen bytes. The client only needs it to be
    /// unique, and this is — the mixing is SplitMix64's, which has no
    /// collisions over a counter.
    const auto uuid_for_entity = [](i32 network_id) {
        auto mix = [](u64 z) {
            z += 0x9E3779B97F4A7C15ULL;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
            return z ^ (z >> 31);
        };
        const auto id = static_cast<u64>(static_cast<u32>(network_id));
        return net::Uuid{mix(id), mix(id ^ 0xA5A5A5A5A5A5A5A5ULL)};
    };

    const world::ChunkCodecContext codec_context{
        blocks ? &*blocks : nullptr,
        biome_names,
        registries ? &*registries : nullptr,
        superflat.air,
    };

    // ── nether ──────────────────────────────────────────────────────────────
    //
    // The Nether: its own chunk map, generator and regions (nether_travel.hpp),
    // built on first need — the first portal crossed, or the first player who
    // logs back in standing there — so a server nobody takes to the Nether
    // never pays for a second worldgen stack. Under `chunk_mutex`, like the
    // overworld's map. `OV_NETHER=0` turns it off, and a crossing is then
    // refused rather than dropping the player anywhere.
    std::unique_ptr<NetherWorld>         nether;
    std::optional<ServerLevel>           nether_level;
    // ── light ── The Nether's and the End's light: block light only, the same
    // engine as the Overworld's. Used under chunk_mutex, like their maps.
    std::optional<world::LightEngine> nether_light;
    std::optional<world::LightEngine> end_light;
    if (blocks) {
        nether_light.emplace(*blocks, kNoSkyLight);
        end_light.emplace(*blocks, kNoSkyLight);
    }
    LookupLightSource nether_chunks{
        [&](i32 x, i32 z) -> world::Chunk* { return nether ? nether->resident(x, z) : nullptr; }};
    /// A chunk joining a dimension's map: alone, then across its borders.
    const auto light_joined = [](world::LightEngine& engine, world::LightChunkSource& source,
                                 world::Chunk& chunk) {
        if (engine.pending() > 0) {
            (void)engine.propagate(source);
        }
        engine.light_chunk(chunk);
        (void)engine.stitch(source, chunk.position());
    };
    // ── end light ──
    std::optional<gameplay::PortalRules> portal_rules;
    if (blocks && registries) {
        portal_rules.emplace(*blocks, *registries);
    }
    const bool nether_enabled = [] {
        const char* setting = std::getenv("OV_NETHER");
        return setting == nullptr || std::string_view{setting} != "0";
    }();
    /// Caller holds chunk_mutex.
    const auto nether_ready = [&]() -> bool {
        if (nether) {
            return true;
        }
        if (!nether_enabled || !blocks || !registries || !world_available) {
            return false;
        }
        NetherWorld::Hooks hooks;
        // ── light ── lit alone, then stitched to the Nether's loaded chunks
        hooks.relight_loaded = [&](world::Chunk& chunk) {
            light_joined(*nether_light, nether_chunks, chunk);
        };
        hooks.relight_generated = [&](world::Chunk& chunk) {
            light_joined(*nether_light, nether_chunks, chunk);
        };
        hooks.ticks_loaded      = [&](const nbt::Document& document) {
            if (!nether_level) {
                return;
            }
            const i64 now = server_tick.load(std::memory_order_relaxed);
            if (const nbt::Tag* pending = document.root.find("block_ticks")) {
                (void)world::ticks_from_nbt(*pending, now,
                                            nether_level->queue(world::TickQueue::Block));
            }
            if (const nbt::Tag* pending = document.root.find("fluid_ticks")) {
                (void)world::ticks_from_nbt(*pending, now,
                                            nether_level->queue(world::TickQueue::Fluid));
            }
        };
        // Half the overworld's workers when it generates too; the machine's
        // recommendation when the overworld is a superflat that needs none.
        // One worker filled 16 chunks in four seconds after a crossing.
        // Two at least: with one, the chunks round a new portal took longer
        // than a minute to arrive in a Debug build beside a generating
        // overworld (`--ours`, 2026-09-10).
        const usize workers = generation_workers == 0
                                  ? std::max<usize>(1, recommended_worker_count())
                                  : std::max<usize>(2, generation_workers / 2);
        nether = NetherWorld::open(level_dir, data_dir, *blocks, *registries, biome_names,
                                   codec_context, level_settings.seed, workers, std::move(hooks));
        return nether != nullptr;
    };
    // ── nether-2 ── The Nether's mobs: a world of their own (nether_mobs.hpp).
    // The host's fields are filled below, once what they call exists.
    std::optional<NetherMobs> nether_mobs;
    NetherMobHost             nether_mob_host;
    if (nether_enabled && blocks && registries) {
        nether_mobs.emplace(*registries, *blocks, mob_combat ? &*mob_combat : nullptr,
                            loot_tables ? &*loot_tables : nullptr, biome_names,
                            std::filesystem::path{OV_DATA_DIR} / "vanilla" / "1.20.1" / "generated",
                            level_settings.seed);
    }
    // ── end nether-2 ──
    // ── end nether ──────────────────────────────────────────────────────────

    // ── end ─────────────────────────────────────────────────────────────────
    // The End: the Nether's storage opened for it (`DIM1/region`, the "end"
    // settings), built on first need like the Nether. `OV_END=0` turns it off.
    // The crossing and the rules are in end_travel.hpp / end_portal.hpp.
    std::unique_ptr<NetherWorld>             end_world;
    // ── light ── the End's chunks as its light engine sees them
    LookupLightSource end_chunks{[&](i32 x, i32 z) -> world::Chunk* {
        return end_world ? end_world->resident(x, z) : nullptr;
    }};
    /// The engine and the chunks of a sky-less dimension, or none.
    const auto light_of = [&](DimensionId dimension)
        -> std::pair<world::LightEngine*, world::LightChunkSource*> {
        if (dimension == DimensionId::End) {
            return {end_light ? &*end_light : nullptr, &end_chunks};
        }
        return {nether_light ? &*nether_light : nullptr, &nether_chunks};
    };
    std::optional<ServerLevel>               end_level;
    std::optional<gameplay::EndPortalRules>  end_rules;
    if (blocks) {
        end_rules.emplace(*blocks);
    }
    const bool end_enabled = [] {
        const char* setting = std::getenv("OV_END");
        return setting == nullptr || std::string_view{setting} != "0";
    }();
    /// Caller holds chunk_mutex.
    const auto end_ready = [&]() -> bool {
        if (end_world) {
            return true;
        }
        if (!end_enabled || !blocks || !registries || !world_available) {
            return false;
        }
        NetherWorld::Hooks hooks;
        // ── light ── lit alone, then stitched to the End's loaded chunks
        hooks.relight_loaded = [&](world::Chunk& chunk) {
            light_joined(*end_light, end_chunks, chunk);
        };
        hooks.relight_generated = [&](world::Chunk& chunk) {
            light_joined(*end_light, end_chunks, chunk);
        };
        hooks.ticks_loaded      = [&](const nbt::Document& document) {
            if (!end_level) {
                return;
            }
            const i64 now = server_tick.load(std::memory_order_relaxed);
            if (const nbt::Tag* pending = document.root.find("block_ticks")) {
                (void)world::ticks_from_nbt(*pending, now, end_level->queue(world::TickQueue::Block));
            }
            if (const nbt::Tag* pending = document.root.find("fluid_ticks")) {
                (void)world::ticks_from_nbt(*pending, now, end_level->queue(world::TickQueue::Fluid));
            }
        };
        // Two: an End chunk is cheap (no carver, four features), and every
        // stack is built on the tick thread — five of them held it for
        // seconds in Debug on a loaded machine.
        const usize workers = 2;
        end_world = NetherWorld::open(level_dir, data_dir, *blocks, *registries, biome_names,
                                      codec_context, level_settings.seed, workers, std::move(hooks),
                                      DimensionId::End);
        return end_world != nullptr;
    };
    /// The storage of a level that is not the overworld, or null.
    const auto other_world = [&](DimensionId dimension) -> NetherWorld* {
        return dimension == DimensionId::End ? end_world.get()
                                             : (dimension == DimensionId::Nether ? nether.get()
                                                                                 : nullptr);
    };
    // ── end end ─────────────────────────────────────────────────────────────

    /// Fetch a chunk: from memory, then from disk, then generated.
    ///
    /// Disk before the generator, so a saved chunk always wins. The other order
    /// works until the first reload and then quietly discards everything anyone
    /// built.
    ///
    /// Caller holds chunk_mutex.
    const auto chunk_at = [&](i32 cx, i32 cz) -> world::Chunk& {  // NOLINT(misc-no-recursion)
        const i64 key = chunk_key(cx, cz);
        if (world::Chunk* resident = chunks.find(ChunkPos{cx, cz}); resident != nullptr) {
            return *resident;
        }

        const auto region = nbt::RegionFile::open(region_path(world_dir, cx, cz));
        if (region) {
            const auto local_x = static_cast<u32>(cx & 31);
            const auto local_z = static_cast<u32>(cz & 31);
            if (region->has_chunk(local_x, local_z)) {
                if (const auto document = region->read_chunk(local_x, local_z)) {
                    const auto version = world::chunk_data_version(*document);
                    if (version == world::kDataVersion1201) {
                        if (auto loaded = world::from_nbt(*document, codec_context)) {
                            chunks.publish(ChunkPos{cx, cz}, std::move(*loaded));
                            world::Chunk& placed = *chunks.find(ChunkPos{cx, cz});
                            // ── light ── A saved chunk carries the light it was
                            // written with, and both arrays are recomputed, then
                            // stitched to the loaded neighbours. Keeping the sky
                            // a vanilla save carries was the rule before, and it
                            // served black air: vanilla stores sky light in a
                            // fifth of the sections (297 of 1536 in a world it
                            // had just lit) and means "as above" by the others,
                            // which this format reads as dark. Where vanilla did
                            // store it, the engine agrees on 98.9 % of the cells
                            // (docs/provenance/incremental-light.md § 6.3), and
                            // a chunk lit by the engine is one the incremental
                            // repair can start from.
                            light_arrived(ChunkPos{cx, cz}, false);
                            // The ticks the chunk was written with. `t` on disk
                            // is a delay relative to the chunk's game time, so
                            // loading has to be told what "now" is — which is
                            // the whole reason a chunk can sit unloaded for an
                            // hour and resume where it left off.
                            if (level) {
                                const i64 now = server_tick.load(std::memory_order_relaxed);
                                if (const nbt::Tag* pending =
                                        document->root.find("block_ticks");
                                    pending != nullptr) {
                                    if (!world::ticks_from_nbt(
                                            *pending, now,
                                            level->queue(world::TickQueue::Block))) {
                                        OV_LOG_WARN("chunk {},{} has a malformed block_ticks list",
                                                    cx, cz);
                                    }
                                }
                                if (const nbt::Tag* pending =
                                        document->root.find("fluid_ticks");
                                    pending != nullptr) {
                                    if (!world::ticks_from_nbt(
                                            *pending, now,
                                            level->queue(world::TickQueue::Fluid))) {
                                        OV_LOG_WARN("chunk {},{} has a malformed fluid_ticks list",
                                                    cx, cz);
                                    }
                                }
                            }
                            return placed;
                        }
                    } else {
                        OV_LOG_ERROR("chunk {},{} was written by data version {} — this is {}", cx,
                                     cz, version ? *version : -1, world::kDataVersion1201);
                    }
                    // The chunk is on disk and could not be read. Whatever is
                    // served in its place must never be written back: replacing
                    // a chunk we failed to understand is how a save gets
                    // destroyed by the program meant to open it.
                    read_only_chunks.insert(key);
                }
            }
        }
        // Last resort, and for generated worlds an expensive one: this runs the
        // pipeline on the calling thread. The streaming path never reaches
        // here — it asks `chunk_source` and waits — so what lands here is a
        // gameplay packet touching a chunk that is not loaded yet. Counted, so
        // that "never happens" is a measurement rather than a belief.
        if (generated) {
            // ── perf ── by thread: the tick is not the only one that ends up here
            if (current_thread_name() == "ov-tick") {
                ++synchronous_generations;
            } else {
                ++synchronous_generations_elsewhere;
            }
            chunks.publish(ChunkPos{cx, cz}, generated->generate(cx, cz));
        } else {
            chunks.publish(ChunkPos{cx, cz}, superflat.generate(ChunkPos{cx, cz}));
        }
        light_arrived(ChunkPos{cx, cz}, false);  // ── light ── generated chunks carry none
        return *chunks.find(ChunkPos{cx, cz});
    };

    /// The chunk if it is already here, and nothing if it is not.
    ///
    /// The streaming path uses this and never `chunk_at`: a chunk that is not
    /// resident yet is asked for and waited on, never generated on the thread
    /// that owes a packet every fifty milliseconds.
    ///
    /// Caller holds chunk_mutex.
    const auto chunk_if_resident = [&](i32 cx, i32 cz) -> world::Chunk* {
        return chunks.find(ChunkPos{cx, cz});
    };

    // ── nether ── The same questions, asked of a given level. The overworld
    // answers from the locals above; the Nether from `nether`. A Nether
    // player cannot exist without `nether` — a crossing is refused first —
    // so the overworld fallbacks below are never reached for one.
    /// Caller holds chunk_mutex.
    const auto view_of = [&](DimensionId dimension) -> DimensionView {
        if (dimension == DimensionId::Nether && nether) {
            return nether->view();
        }
        if (dimension == DimensionId::End && end_world) {  // ── end ──
            return end_world->view();
        }
        return DimensionView{&chunks, &dirty_chunks, &read_only_chunks,
                             world::WorldShape::overworld()};
    };
    const auto chunk_at_in = [&](DimensionId dimension, i32 cx, i32 cz) -> world::Chunk& {
        if (dimension == DimensionId::Nether && nether_ready()) {
            return nether->chunk_at(cx, cz);
        }
        if (dimension == DimensionId::End && end_ready()) {  // ── end ──
            return end_world->chunk_at(cx, cz);
        }
        return chunk_at(cx, cz);
    };
    const auto chunk_if_resident_in = [&](DimensionId dimension, i32 cx,
                                          i32 cz) -> world::Chunk* {
        if (dimension != DimensionId::Overworld) {  // ── end ── the Nether's or the End's
            NetherWorld* other = other_world(dimension);
            return other != nullptr ? other->resident(cx, cz) : nullptr;
        }
        return chunk_if_resident(cx, cz);
    };
    // ── end nether ──

    /// Write every changed chunk, grouped by region so each file opens once.
    // ── rails ── Declared before the save, which writes its carts; emplaced
    // once the world ticks exist.
    std::optional<RailsSession> rails_session;
    // ── end rails ──
    // ── mobs-3 ── the mobs go into entities/ with every save; set once the
    // storage's host exists, further down.
    std::function<void()> mobs3_save_entities;
    const auto save_world = [&] {
        const std::scoped_lock lock{chunk_mutex};
        // ── entities ── entities/ has one writer, the storage: mobs, and the
        // carts it asks the rails session for (entity_storage.hpp).
        if (mobs3_save_entities) {
            mobs3_save_entities();
        }
        // ── nether ── DIM-1/region, with the Nether level's own ticks.
        if (nether) {
            std::vector<world::ScheduledTick> block_snapshot;
            std::vector<world::ScheduledTick> fluid_snapshot;
            if (nether_level) {
                block_snapshot = nether_level->queue(world::TickQueue::Block).snapshot();
                fluid_snapshot = nether_level->queue(world::TickQueue::Fluid).snapshot();
            }
            (void)nether->save(server_tick.load(std::memory_order_relaxed), block_snapshot,
                               fluid_snapshot);
        }
        // ── end ── DIM1/region, with the End level's own ticks.
        if (end_world) {
            std::vector<world::ScheduledTick> block_snapshot;
            std::vector<world::ScheduledTick> fluid_snapshot;
            if (end_level) {
                block_snapshot = end_level->queue(world::TickQueue::Block).snapshot();
                fluid_snapshot = end_level->queue(world::TickQueue::Fluid).snapshot();
            }
            (void)end_world->save(server_tick.load(std::memory_order_relaxed), block_snapshot,
                                  fluid_snapshot);
        }
        if (dirty_chunks.empty()) {
            // ── commands: level.dat still, when only the rules changed ──
            // A /time or /gamerule dirties no chunk; returning before this
            // lost them at the next restart, which a restart test caught.
            if (commands) {
                commands->store_world(level_settings);
                if (!io::write_file_atomic(level_dir / "level.dat",
                                           world::encode_level_dat(level_settings))) {
                    OV_LOG_WARN("could not write level.dat");
                }
            }
            return;
        }

        std::map<std::pair<i32, i32>, std::vector<i64>> by_region;
        for (const i64 key : dirty_chunks) {
            if (read_only_chunks.contains(key)) {
                continue;
            }
            const auto cx = static_cast<i32>(key >> 32);
            const auto cz = static_cast<i32>(static_cast<u32>(key & 0xFFFFFFFF));
            by_region[{cx >> 5, cz >> 5}].push_back(key);
        }

        usize written = 0;
        for (const auto& [region_pos, keys] : by_region) {
            const auto path =
                world_dir / fmt::format("r.{}.{}.mca", region_pos.first, region_pos.second);
            auto writer = nbt::RegionWriter::open_or_empty(path);
            for (const i64 key : keys) {
                const auto cx    = static_cast<i32>(key >> 32);
                const auto cz    = static_cast<i32>(static_cast<u32>(key & 0xFFFFFFFF));
                const auto* held = chunks.find(ChunkPos{cx, cz});
                if (held == nullptr) {
                    continue;
                }
                // The chunk's own pending ticks. A copy of the context per
                // chunk rather than one shared: the two spans and `game_time`
                // are the only fields that differ, and `ticks_to_nbt` filters
                // the level's queue down to this column.
                world::ChunkCodecContext with_ticks = codec_context;
                with_ticks.game_time                = server_tick.load(std::memory_order_relaxed);
                std::vector<world::ScheduledTick> block_snapshot;
                std::vector<world::ScheduledTick> fluid_snapshot;
                if (level) {
                    block_snapshot = level->queue(world::TickQueue::Block).snapshot();
                    fluid_snapshot = level->queue(world::TickQueue::Fluid).snapshot();
                    with_ticks.block_ticks = block_snapshot;
                    with_ticks.fluid_ticks = fluid_snapshot;
                }
                writer.set_chunk(static_cast<u32>(cx & 31), static_cast<u32>(cz & 31),
                                 world::to_nbt(*held, with_ticks), 0);
                ++written;
            }
            if (!writer.write(path)) {
                OV_LOG_WARN("could not write {}", path.string());
            }
        }
        // level.dat every time, not once at creation: it is small, and a world
        // whose regions are newer than its level.dat is the state a crash
        // leaves behind.
        if (commands) {
            commands->store_world(level_settings);  // ── commands ──
        }
        if (!io::write_file_atomic(level_dir / "level.dat",
                                   // ── player data: with Data.Player when there is one ──
                                   player_data.encode_level_dat(level_settings))) {
            OV_LOG_WARN("could not write level.dat");
        }

        OV_LOG_INFO("saved {} chunks across {} regions", written, by_region.size());
        dirty_chunks.clear();
    };

    /// Bring a player's loaded chunks in line with where they are.
    ///
    /// Called at join and whenever they cross a chunk boundary. Sending only the
    /// difference matters: re-sending the whole square on every boundary would
    /// be 289 chunks a few steps apart, and the client would spend its time
    /// rebuilding meshes it already had.
    const auto stream_chunks = [&](const net::ConnectionPtr& connection, Player& player) {
        constexpr i32 kRadius = 8;

        const i32 centre_x = static_cast<i32>(std::floor(player.x)) >> 4;
        const i32 centre_z = static_cast<i32>(std::floor(player.z)) >> 4;
        if (player.streaming && centre_x == player.centre_x && centre_z == player.centre_z) {
            return;
        }
        player.centre_x  = centre_x;
        player.centre_z  = centre_z;
        player.streaming = true;

        // The ticket. From here on the chunk is loaded because this player is
        // standing there, and it stops being loaded when they are not: the
        // level a ticket gives falls off by one per chunk, so `kRadius` of them
        // land inside `LoadLevel::kLoaded` and the ring beyond it does not.
        // Moving the ticket rather than adding one is what makes walking free.
        {
            const std::scoped_lock lock{chunk_mutex};
            // ── nether ── On the map of the level the player is in.
            world::ChunkMap& map = *view_of(player.dimension).chunks;
            map.set_ticket(world::TicketType::Player, static_cast<u64>(player.entity_id),
                           ChunkPos{centre_x, centre_z},
                           world::LoadLevel::for_view_distance(kRadius));
            world::LevelChanges changes;
            map.refresh(changes);
        }

        const auto send = [&](i32 id, std::span<const u8> payload) {
            if (const auto framed = net::encode_packet(id, payload)) {
                connection->send(*framed);
            }
        };

        // The centre first: a client that receives chunks it considers out of
        // range discards them.
        send(net::clientbound::kSetCenterChunk, net::encode_set_center_chunk(centre_x, centre_z));

        std::unordered_set<i64> wanted;
        wanted.reserve(static_cast<usize>((2 * kRadius + 1) * (2 * kRadius + 1)));
        for (i32 dz = -kRadius; dz <= kRadius; ++dz) {
            for (i32 dx = -kRadius; dx <= kRadius; ++dx) {
                wanted.insert(chunk_key(centre_x + dx, centre_z + dz));
            }
        }

        player.pending_chunks.clear();
        for (const i64 key : wanted) {
            if (!player.loaded_chunks.contains(key)) {
                player.pending_chunks.push_back(key);
            }
        }
        // Furthest first, because the drain takes from the back: what the
        // player is standing on arrives before what is at the horizon.
        std::ranges::sort(player.pending_chunks, [&](i64 a, i64 b) {
            const auto distance = [&](i64 key) {
                const auto cx = static_cast<i32>(key >> 32);
                const auto cz = static_cast<i32>(static_cast<u32>(key & 0xFFFFFFFF));
                const i64  dx = cx - centre_x;
                const i64  dz = cz - centre_z;
                return dx * dx + dz * dz;
            };
            return distance(a) > distance(b);
        });

        for (auto it = player.loaded_chunks.begin(); it != player.loaded_chunks.end();) {
            if (wanted.contains(*it)) {
                ++it;
                continue;
            }
            const auto cx = static_cast<i32>(*it >> 32);
            const auto cz = static_cast<i32>(static_cast<u32>(*it & 0xFFFFFFFF));
            send(net::clientbound::kUnloadChunk, net::encode_unload_chunk(cx, cz));
            it = player.loaded_chunks.erase(it);
        }
    };

    /// Send one packet to every player except `except`.
    ///
    /// Caller holds players_mutex.
    const auto broadcast = [&](const net::Connection* except, i32 id, std::span<const u8> payload) {
        const auto framed = net::encode_packet(id, payload);
        if (!framed) {
            return;
        }
        for (auto& [key, other] : players) {
            // ── nether ── The overworld's news, for the overworld's players:
            // an entity or a block update sent to someone in the Nether would
            // land at the same coordinates in the wrong world.
            if (key != except && other.connection && other.dimension == DimensionId::Overworld) {
                other.connection->send(*framed);
            }
        }
    };
    // ── nether ── The same for one level, and for everyone (the tab list).
    const auto broadcast_in = [&](DimensionId dimension, const net::Connection* except, i32 id,
                                  std::span<const u8> payload) {
        const auto framed = net::encode_packet(id, payload);
        if (!framed) {
            return;
        }
        for (auto& [key, other] : players) {
            if (key != except && other.connection && other.dimension == dimension) {
                other.connection->send(*framed);
            }
        }
    };
    const auto broadcast_all = [&](const net::Connection* except, i32 id,
                                   std::span<const u8> payload) {
        const auto framed = net::encode_packet(id, payload);
        if (!framed) {
            return;
        }
        for (auto& [key, other] : players) {
            if (key != except && other.connection) {
                other.connection->send(*framed);
            }
        }
    };
    // ── end nether ──

    // ── end ── The dragon fight (end_fight.hpp): built with the End's rules,
    // started by the first arrival, heard by the End's players. Touched only
    // with `players_mutex` held — by the tick's End section and by a hit.
    std::optional<EndFight>  end_fight;
    std::unordered_set<i32>  end_fight_viewers;
    EndFightHost             end_fight_host;
    end_fight_host.broadcast = [&](i32 id, std::span<const u8> payload) {
        broadcast_in(DimensionId::End, nullptr, id, payload);
    };
    end_fight_host.reserve_entity_ids = [&](i32 count) { return next_entity_id.fetch_add(count); };
    // ── dragon ── the End's players, what reaches each, the fight's save and
    // its line of sight (end_fight.hpp, docs/provenance/dragon.md).
    const auto end_player = [&](i32 id) -> Player* {
        for (auto& [end_key, who] : players) {
            if (who.entity_id == id && who.connection && who.dimension == DimensionId::End) {
                return &who;
            }
        }
        return nullptr;
    };
    end_fight_host.players = [&](std::vector<EndFightPlayer>& out) {
        for (const auto& [end_key, who] : players) {
            if (who.connection && who.confirmed && who.dimension == DimensionId::End) {
                out.push_back(EndFightPlayer{who.entity_id, Vec3d{who.x, who.y, who.z},
                                             who.mortal(),
                                             !who.survival.awaiting_respawn &&
                                                 !who.survival.health.dead});
            }
        }
    };
    end_fight_host.send_to = [&](i32 id, i32 packet, std::span<const u8> payload) {
        if (Player* who = end_player(id)) {
            if (const auto framed = net::encode_packet(packet, payload)) {
                who->connection->send(*framed);
            }
        }
    };
    end_fight_host.hurt_player = [&](i32 id, f32 amount, gameplay::DamageKind kind) {
        Player* who = end_player(id);
        if (who == nullptr || !who->mortal()) {
            return false;
        }
        const SurvivalIo io{
            .send =
                [&](i32 packet, std::span<const u8> payload) {
                    if (const auto framed = net::encode_packet(packet, payload)) {
                        who->connection->send(*framed);
                    }
                },
            .broadcast = [&](i32 packet, std::span<const u8> payload) {
                broadcast_in(DimensionId::End, who->connection.get(), packet, payload);
            }};
        return static_cast<bool>(who->survival.hurt(kind, amount, io, who->entity_id).applied);
    };
    end_fight_host.award_experience = [&](i32 id, i32 value) {
        if (Player* who = end_player(id)) {
            who->survival.award_experience(value);
        }
    };
    if (end_rules && end_rules->valid() && registries && blocks) {
        if (const auto types = registries->find("minecraft:entity_type")) {
            EndFightTypes fight_types;
            const auto    type_of = [&](std::string_view name, i32& into) {
                if (const auto id = registries->protocol_id(*types, name)) {
                    into = static_cast<i32>(*id);
                }
            };
            type_of("minecraft:ender_dragon", fight_types.dragon);
            type_of("minecraft:end_crystal", fight_types.crystal);
            type_of("minecraft:dragon_fireball", fight_types.fireball);
            type_of("minecraft:area_effect_cloud", fight_types.cloud);
            type_of("minecraft:experience_orb", fight_types.experience_orb);
            if (const auto particles = registries->find("minecraft:particle_type")) {
                if (const auto id = registries->protocol_id(*particles, "minecraft:dragon_breath")) {
                    fight_types.breath_particle = static_cast<i32>(*id);
                }
            }
            const auto tagged = [&](std::string_view name) {
                std::vector<registry::BlockId> out;
                if (const auto block_registry = registries->find("minecraft:block")) {
                    if (const auto tag = registries->find_tag(*block_registry, name)) {
                        for (const auto member : registries->tag_members(*tag)) {
                            if (const auto block = blocks->find_block(
                                    registries->entry_of(*block_registry, member))) {
                                out.push_back(*block);
                            }
                        }
                    }
                }
                return out;
            };
            end_fight.emplace(*end_rules, *blocks, level_settings.seed, fight_types,
                              tagged("minecraft:dragon_immune"),
                              tagged("minecraft:dragon_transparent"));
            if (level_settings.dragon_fight) {
                end_fight->load(*level_settings.dragon_fight);
            }
            level_settings.dragon_fight = end_fight->save();
            end_fight->set_sight([&](Vec3d from, Vec3d to) {
                const Vec3d along  = to - from;
                const f64   length = along.length();
                if (!end_level || length < 1e-6) {
                    return true;
                }
                return !raycast_voxels(from, along, length, [&](BlockPos cell) {
                            const registry::BlockStateId state = end_level->block_at(cell);
                            return state != registry::kAirState &&
                                   blocks->blocks_motion(blocks->block_of(state));
                        }).has_value();
            });
        }
    }
    // ── end end ──

    // ── sound ── what the server makes heard, and to whom (sounds.hpp). Built
    // before every packet handler below, which all capture it. `send_near` is
    // called with players_mutex held, like `broadcast`.
    std::optional<Sounds> sounds;
    if (blocks && registries) {
        sounds.emplace(*blocks, *registries, i64{0x6F76736F756E64});
    }
    /// The block a player's click is acting on, while it acts. What the click
    /// sets off elsewhere — a fence gate opened by the lever just pulled — is
    /// heard by everyone; the clicked block (and the other half of a clicked
    /// door) already has its own sound. Network thread only.
    std::optional<BlockPos> sound_click;
    SoundHost sound_host;
    sound_host.send_near = [&](const void* except, Vec3d at, f64 radius, i32 id,
                               std::span<const u8> payload) {
        const auto framed = net::encode_packet(id, payload);
        if (!framed) {
            return;
        }
        for (auto& [key, other] : players) {
            if (key == except || !other.connection || !other.confirmed) {
                continue;
            }
            const f64 dx = other.x - at.x;
            const f64 dy = other.y - at.y;
            const f64 dz = other.z - at.z;
            if (dx * dx + dy * dy + dz * dz <= radius * radius) {
                other.connection->send(*framed);
            }
        }
    };
    // ── end sound ──

    /// Confirm a change the client already predicted.
    ///
    /// Without this the client shows its guess, waits, and then rolls it back —
    /// which looks exactly like the server ignoring the player.
    const auto acknowledge = [](const net::ConnectionPtr& connection, i32 sequence) {
        if (const auto framed = net::encode_packet(net::clientbound::kAcknowledgeDig,
                                                   net::encode_acknowledge_dig(sequence))) {
            connection->send(*framed);
        }
    };

    /// Apply a block change to the world and tell everyone who can see it.
    ///
    /// Called from the network thread, which is why the chunk cache has a mutex
    /// — the single-writer rule the project is built on arrives with the tick
    /// scheduler, and until then this is honest locking rather than a race.
    // How much of a block one tick of digging removes, for a given player.
    // Shared by the packet handler and the tick loop: the two have to agree, and
    // writing it twice is how they stop agreeing.
    const auto dig_progress_for = [&](const Player& who, net::WirePosition where) -> f32 {
        if (!break_rules) {
            return 1.0F;
        }
        registry::BlockStateId state{0};
        {
            const std::scoped_lock chunk_lock{chunk_mutex};
            world::Chunk& dug = chunk_at_in(who.dimension, where.x >> 4, where.z >> 4);  // ── nether ──
            state                      = dug.get_block(static_cast<usize>(where.x & 15), where.y,
                                                       static_cast<usize>(where.z & 15));
        }
        const net::ItemStack& held = who.inventory[36 + static_cast<usize>(who.held_slot)];
        gameplay::Held        holding;
        if (held.item_id != 0 && held.count > 0) {
            holding.item       = held.item_id;
            holding.efficiency = enchantment_level(held, "minecraft:efficiency");
        }
        // ── effects: haste, conduit power and mining fatigue ──
        // ── enchanting ── Aqua Affinity is read off the helmet, slot 5.
        auto stance          = who.effects.dig_stance(who.on_ground);
        stance.aqua_affinity = stack_enchantment(who.inventory[5],
                                                 gameplay::Enchantment::AquaAffinity) > 0;
        return break_rules->destroy_progress(state, holding, stance);
    };

    /// Tirer le butin d'un bloc et le poser au sol.
    ///
    /// À appeler **avant** d'effacer le bloc : la table lit son état, et un
    /// bloc déjà remplacé par de l'air ne donne rien. Les entités sont créées
    /// ici mais diffusées par l'appelant, qui tient déjà le verrou.
    const auto drop_loot = [&](const Player& breaker, net::WirePosition where,
                               std::vector<ItemEntity>& out) {
        if (!loot_tables) {
            return;
        }
        registry::BlockStateId state{0};
        registry::BlockStateId above{0};
        registry::BlockStateId below{0};
        {
            const std::scoped_lock chunk_lock{chunk_mutex};
            world::Chunk& dug = chunk_at_in(breaker.dimension, where.x >> 4, where.z >> 4);  // ── nether ──
            const auto             lx  = static_cast<usize>(where.x & 15);
            const auto             lz  = static_cast<usize>(where.z & 15);
            state                      = dug.get_block(lx, where.y, lz);
            above                      = dug.get_block(lx, where.y + 1, lz);
            below                      = dug.get_block(lx, where.y - 1, lz);
        }

        const net::ItemStack& tool = breaker.inventory[36 + static_cast<usize>(breaker.held_slot)];
        gameplay::Held        held;
        if (tool.item_id != 0 && tool.count > 0) {
            held.item       = tool.item_id;
            held.silk_touch = enchantment_level(tool, "minecraft:silk_touch");
            held.fortune    = enchantment_level(tool, "minecraft:fortune");
        }

        std::vector<gameplay::Drop> drops;
        loot_tables->drops(state, held, loot_random, drops,
                           gameplay::Neighbours{.above = above, .below = below});

        for (const gameplay::Drop& drop : drops) {
            ItemEntity item;
            item.entity_id = next_entity_id.fetch_add(1);
            // A uuid derived from the id: unique, and free of a clock the
            // tick loop is not allowed to read.
            item.uuid  = net::Uuid{0x4f564954454d0000ULL | static_cast<u64>(item.entity_id),
                                   static_cast<u64>(item.entity_id) * 0x9E3779B97F4A7C15ULL};
            item.x     = static_cast<f64>(where.x) + 0.5;
            item.y     = static_cast<f64>(where.y) + 0.25;
            item.z     = static_cast<f64>(where.z) + 0.5;
            item.stack = net::ItemStack{drop.item, static_cast<i8>(std::min(drop.count, 64)), {}};
            item.born  = server_tick.load(std::memory_order_relaxed);
            item.dimension = breaker.dimension;  // ── nether ──
            out.push_back(std::move(item));
        }
    };

    /// Ranger une pile dans l'inventaire d'un joueur, et lui dire.
    ///
    /// Rend combien d'exemplaires ont trouvé place. Zéro veut dire un
    /// inventaire plein, et l'appelant doit alors laisser la pile au sol
    /// plutôt que de la faire disparaître.
    ///
    /// Caller holds players_mutex.
    const auto give_to_player = [&](Player& who, const net::ItemStack& stack) -> i8 {
        const i8 limit = registries ? registries->max_stack_size(stack.item_id) : i8{64};
        i8       left  = stack.count;

        const auto deposit = [&](usize slot) {
            net::ItemStack& target = who.inventory[slot];
            const bool      empty  = target.item_id == 0 || target.count <= 0;
            if (!empty && (target.item_id != stack.item_id || !target.nbt.empty())) {
                return;
            }
            const i8 room = static_cast<i8>(limit - (empty ? 0 : target.count));
            if (room <= 0) {
                return;
            }
            const i8 moved = std::min(room, left);
            target.item_id = stack.item_id;
            target.count   = static_cast<i8>((empty ? 0 : target.count) + moved);
            target.nbt     = stack.nbt;
            left           = static_cast<i8>(left - moved);
            if (who.connection) {
                if (const auto framed = net::encode_packet(
                        net::clientbound::kContainerSlot,
                        net::encode_container_slot(0, 0, static_cast<i16>(slot), target))) {
                    who.connection->send(*framed);
                }
            }
        };

        // Stacks that already hold this item first, then the hotbar, then the
        // main inventory — the order vanilla fills them in.
        for (usize slot = 36; slot < 45 && left > 0; ++slot) {
            if (who.inventory[slot].item_id == stack.item_id && who.inventory[slot].count > 0) {
                deposit(slot);
            }
        }
        for (usize slot = 9; slot < 36 && left > 0; ++slot) {
            if (who.inventory[slot].item_id == stack.item_id && who.inventory[slot].count > 0) {
                deposit(slot);
            }
        }
        for (usize slot = 36; slot < 45 && left > 0; ++slot) {
            deposit(slot);
        }
        for (usize slot = 9; slot < 36 && left > 0; ++slot) {
            deposit(slot);
        }
        return static_cast<i8>(stack.count - left);
    };

    /// Faire apparaître les piles au sol chez tout le monde.
    ///
    /// Caller holds players_mutex.
    const auto publish_items = [&](std::vector<ItemEntity>& items) {
        for (ItemEntity& item : items) {
            // ── nether ── Only to the players in the item's own level.
            broadcast_in(item.dimension, nullptr, net::clientbound::kSpawnEntity,
                         net::encode_spawn_entity(item.entity_id, item.uuid, net::kItemEntityType,
                                                  item.x, item.y, item.z));
            // Sans la métadonnée l'entité existe et ne rend rien du tout, ce
            // qui ressemble exactement à un paquet qui ne serait pas arrivé.
            broadcast_in(
                item.dimension, nullptr, net::clientbound::kEntityMetadata,
                net::encode_item_metadata(item.entity_id, item.stack.item_id, item.stack.count));
            ground_items.push_back(std::move(item));
        }
    };

    // ── tnt and gravity ─────────────────────────────────────────────────────
    // Declared here and emplaced once the world ticks exist: the join path
    // below hands it the entities it spawned, and the drain hands it blocks.
    std::optional<TntGravity> tnt_gravity;
    // ── projectiles ──
    std::optional<Projectiles> projectiles;
    std::optional<Brewing>     brewing;  // ── brewing ──
    std::vector<ChunkPos>      brewing_chunks;  // ── brewing ──
    // ── husbandry ──
    std::optional<Husbandry> husbandry;
    std::optional<Taming>    taming;  // ── tame ── owners, riders, collars
    // ── mobs-2: slimes — a size each, and the division on death ──
    std::optional<Slimes>             slimes;
    math::LegacyRandomSource          slime_random{0x4f56'534c'494d'4531LL};
    std::vector<gameplay::SlimeChild> slime_births;
    std::vector<gameplay::SlimeChild> slime_scratch;
    std::optional<Drowning>           drowning;  // zombies and husks under water
    std::vector<Drowning::Conversion> drowned_now;
    // ── mobs-3: hostile swings, and the mobs they killed ──
    std::optional<MobAttacks> mob_attacks;
    std::vector<MobKill>      mob_kills;
    MobRecords                mob_records;    // persistence, names, noActionTime
    std::optional<MobDespawn> mob_despawn;
    std::vector<Vec3d>        mobs3_players;  // every non-spectator, for despawn
    std::optional<ZombieVillagers> zombie_villagers;  // the risen, and their cure
    std::optional<EntityStorage>   entity_storage;    // entities/r.x.z.mca
    std::vector<ChunkPos>          mobs3_to_load;
    i64 mobs3_last_read{-10};  // ── persistence ── the tick of the last entities/ read
    std::vector<i32>               mobs3_unloaded;  // mobs a chunk took to disk, to remove
    // ── villagers ──
    std::optional<Villagers> villagers;

    /// The packets that make one mob appear.
    ///
    /// Three, in this order, and the order is what a real server sends: the
    /// entity, then its metadata, then its attributes. A client told about an
    /// entity it has no metadata for renders it — a mob is not an item, whose
    /// whole appearance is its metadata — but its health bar and its speed come
    /// from the two that follow.
    const auto mob_packets = [&](const entity::EntityState& state,
                                 const auto&                deliver) {
        // ── rails: a minecart is not a mob ──
        if (rails_session && mobs && rails_session->owns(state.type)) {
            rails_session->spawn_packets(*mobs, state, [&](i32 id, std::span<const u8> payload) {
                deliver(id, payload);
            });
            return;
        }
        // ── tnt and gravity: a primed TNT and a falling block are not mobs ──
        if (tnt_gravity && mobs && tnt_gravity->owns(state.type)) {
            tnt_gravity->spawn_packets(*mobs, state, [&](i32 id, std::span<const u8> payload) {
                deliver(id, payload);
            });
            return;
        }
        // ── projectiles: an arrow is not a mob either ──
        if (projectiles && mobs && projectiles->owns(state.type)) {
            projectiles->spawn_packets(*mobs, state, [&](i32 id, std::span<const u8> payload) {
                deliver(id, payload);
            });
            return;
        }
        net::SpawnEntity spawn;
        spawn.entity_id = state.network_id;
        spawn.uuid      = state.uuid;
        spawn.type      = state.type;
        // The position the other clients already hold, not the true one.
        //
        // They differ by at most one 1/4096 quantum, and sending the true one
        // would leave a client that joined mid-fall permanently that far from
        // everyone else: it would anchor on the truth and then receive deltas
        // computed against the quantised copy. Vanilla tracks a position per
        // viewer; this server broadcasts to all of them at once, so the copy
        // has to be the shared one.
        const Vec3d anchor = state.broadcast_valid ? state.broadcast_position : state.position;
        spawn.x            = anchor.x;
        spawn.y            = anchor.y;
        spawn.z            = anchor.z;
        spawn.yaw       = state.yaw;
        spawn.pitch     = state.pitch;
        spawn.head_yaw  = state.head_yaw;
        deliver(net::clientbound::kSpawnEntity, net::encode_spawn_entity(spawn));

        net::MetadataWriter fields;
        fields.float_value(net::metadata::kHealth, state.health);
        // ── husbandry: baby, fleece, saddle ──
        if (husbandry && mobs) {
            husbandry->spawn_metadata(*mobs, state, fields);
        }
        if (taming && mobs) {  // ── tame: owner, sitting, collar, variant, saddle ──
            taming->spawn_metadata(*mobs, state, fields);
        }
        if (slimes) {  // ── mobs-2 ──
            slimes->spawn_metadata(state, fields);
        }
        // ── villagers: type, profession, level, sleep ──
        if (villagers && mobs) {
            villagers->spawn_metadata(*mobs, state, fields);
        }
        if (zombie_villagers) {  // ── mobs-3: a zombie villager's villager, and its cure ──
            zombie_villagers->spawn_metadata(state, fields);
        }
        deliver(net::clientbound::kEntityMetadata,
                net::encode_entity_metadata(state.network_id, fields.take()));

        // Every attribute the type owns, at the base value the game reports for
        // it. Vanilla sends only the ones that differ from the client's own
        // default; sending all of them is more traffic and never wrong.
        if (registries) {
            std::vector<net::AttributeValue> values;
            const auto attribute_registry = registries->find("minecraft:attribute");
            for (const auto& owned : registries->entity_attributes(state.type)) {
                if (!attribute_registry) {
                    break;
                }
                const std::string_view name =
                    registries->entry_of(*attribute_registry, owned.attribute);
                if (!name.empty()) {
                    values.push_back(net::AttributeValue{name, owned.base});
                }
            }
            if (taming && mobs) {  // ── tame ── a horse's own drawn stats
                taming->attributes(*mobs, state, values);
            }
            if (!values.empty()) {
                deliver(net::clientbound::kUpdateAttributes,
                        net::encode_update_attributes(state.network_id, values));
            }
        }
        if (taming && mobs) {  // ── tame ── a horse's armour, and its rider
            taming->spawn_extra(*mobs, state, [&](i32 id, std::span<const u8> payload) {
                deliver(id, payload);
            });
        }
    };

    /// The world as the collision code reads it: a plain function and a
    /// context, because a public header may not carry a template.
    ///
    /// Caller holds chunk_mutex.
    struct WorldView {
        std::function<registry::BlockStateId(i32, i32, i32)> read;

        static registry::BlockStateId look_up(void* context, i32 x, i32 y, i32 z) {
            return static_cast<WorldView*>(context)->read(x, y, z);
        }
    };

    /// Read a block without holding the lock twice.
    ///
    /// Caller holds chunk_mutex.
    // ── nether ── In a given level; `block_at` is the overworld's.
    const auto block_at_in = [&](DimensionId dimension,
                                 net::WirePosition where) -> registry::BlockStateId {
        const auto shape = dimension_info(dimension).shape;
        if (!shape.contains_y(where.y)) {
            return registry::BlockStateId{0};
        }
        return chunk_at_in(dimension, where.x >> 4, where.z >> 4)
            .get_block(static_cast<usize>(where.x & 15), where.y, static_cast<usize>(where.z & 15));
    };
    const auto block_at = [&](net::WirePosition where) -> registry::BlockStateId {
        return block_at_in(DimensionId::Overworld, where);
    };

    /// The four horizontal neighbours, in the order the side properties name
    /// them: north, south, west, east.
    const auto neighbours_of_in = [&](DimensionId dimension, net::WirePosition where) {
        return std::array<registry::BlockStateId, 4>{
            block_at_in(dimension, {where.x, where.y, where.z - 1}),
            block_at_in(dimension, {where.x, where.y, where.z + 1}),
            block_at_in(dimension, {where.x - 1, where.y, where.z}),
            block_at_in(dimension, {where.x + 1, where.y, where.z})};
    };

    /// A block changed outside the queues — a player placed or broke one.
    ///
    /// This is what makes a lever do anything at all. Vanilla's `setBlock`
    /// notifies the six neighbours as part of writing; ours cannot do it where
    /// the write happens, because placement runs on the network thread while
    /// the rules must run on the tick thread. So the position is queued and the
    /// next tick picks it up — one tick of latency, and the single-writer
    /// rule intact.
    // ── perf ── the tick's phase clock (tick_profile.hpp). Built before every
    // lambda that feeds it; on the heap because it is ~100 KiB of counters and
    // an integrated server runs this on a thread with a small stack.
    const auto perf = std::make_unique<TickProfile>();
    // ── light ── The edits written since the last relight are noted in
    // `light` (declared beside the map) by every writer — the drain, a
    // command's fill, a player's dig or place on the network thread — and
    // repaired **once per tick, on the tick thread**, by `flush_tick_writes`.
    // ── end perf ──

    std::vector<net::WirePosition> pending_notifications;
    std::mutex                     notification_mutex;
    const auto                     notify_change = [&](net::WirePosition where) {
        const std::scoped_lock lock{notification_mutex};
        pending_notifications.push_back(where);
    };
    // ── nether ── The Nether's own queue: its level settles it.
    std::vector<net::WirePosition> nether_notifications;
    std::vector<net::WirePosition> end_notifications;  // ── end ──
    const auto notify_change_in = [&](DimensionId dimension, net::WirePosition where) {
        if (dimension == DimensionId::Overworld) {
            notify_change(where);
            return;
        }
        const std::scoped_lock lock{notification_mutex};
        (dimension == DimensionId::End ? end_notifications : nether_notifications)
            .push_back(where);  // ── end ──
    };

    /// Write a block and everything the world needs around it, **with the lock
    /// already held**, and say whether anything has to be sent.
    ///
    /// Split out of `set_block_and_broadcast` so that the tick driver — which
    /// runs hundreds of writes in one drain — can take `chunk_mutex` once
    /// instead of once per block, and so that it can defer relighting. Taking
    /// it per write is not merely slower: `chunk_mutex` is not recursive, and a
    /// rule that already holds it deadlocks the tick thread the first time
    /// water moves.
    ///
    /// `relight` false leaves the light stale on purpose. A puddle settling
    /// writes a few hundred blocks and relighting a 3x3 neighbourhood for each
    /// of them is the single most expensive thing this server could do per
    /// tick; the driver relights the chunks it touched once, at the end.
    /// Write a state and keep the block entity when the **block** has not
    /// changed.
    ///
    /// `Chunk::set_block` drops the block entity at the position it writes, and
    /// it is right to: a block entity outliving its block is a chest that
    /// cannot be opened and cannot be removed. But a redstone-driven property
    /// change is not a new block — a hopper going `enabled=false`, a furnace
    /// going `lit=true`, a dispenser going `triggered=true` — and dropping the
    /// block entity there **empties the container**.
    ///
    /// This cost an afternoon to find, because the symptom is not an error: the
    /// hopper is still a hopper, still has its facing, and simply cannot be
    /// opened any more, while its items are gone from the save. Vanilla's own
    /// `Level.setBlock` keeps the block entity when the block is the same, and
    /// so does this.
    const auto write_block = [&](world::Chunk& chunk, i32 x, i32 y, i32 z,
                                 registry::BlockStateId state) {
        const auto local_x = static_cast<usize>(x & 15);
        const auto local_z = static_cast<usize>(z & 15);
        const registry::BlockStateId before = chunk.get_block(local_x, y, local_z);

        const world::BlockEntity* existing = chunk.block_entity_at(local_x, y, local_z);
        const bool                same_block =
            blocks && existing != nullptr && blocks->block_of(before) == blocks->block_of(state);
        // Copied rather than moved: `set_block` erases the entry this points
        // at, so the copy has to be taken first and cannot be a reference.
        std::optional<world::BlockEntity> kept;
        if (same_block) {
            kept = *existing;
        }

        chunk.set_block(local_x, y, local_z, state);

        if (kept) {
            chunk.set_block_entity(std::move(*kept));
        }
    };

    // ── nether ── A write into the Nether: the same bookkeeping on its map,
    // block light only (it has no sky), and relit with the rest of its chunk.
    // ── end ── `dimension` makes it the End's too: the same storage type.
    const auto apply_block_change_in_nether = [&](net::WirePosition      position,
                                                  registry::BlockStateId state,
                                                  DimensionId dimension = DimensionId::Nether) {
        const i32     chunk_x = position.x >> 4;
        const i32     chunk_z = position.z >> 4;
        world::Chunk& chunk   = chunk_at_in(dimension, chunk_x, chunk_z);
        write_block(chunk, position.x, position.y, position.z, state);
        if (NetherWorld* other = other_world(dimension)) {
            other->mark_dirty(chunk_x, chunk_z);
        }
    };
    const auto apply_block_change = [&](net::WirePosition position, registry::BlockStateId state,
                                        bool relight) {
        const i32 chunk_x = position.x >> 4;
        const i32 chunk_z = position.z >> 4;
        world::Chunk& chunk = chunk_at(chunk_x, chunk_z);

        write_block(chunk, position.x, position.y, position.z, state);
        dirty_chunks.insert(chunk_key(chunk_x, chunk_z));

        if (relight && light) {  // ── light ── repaired on the tick
            light->block_changed(BlockPos{position.x, position.y, position.z});
        }

        for (i32 dz = -1; dz <= 1; ++dz) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                if (chunks.contains(ChunkPos{chunk_x + dx, chunk_z + dz})) {
                    dirty_chunks.insert(chunk_key(chunk_x + dx, chunk_z + dz));
                }
            }
        }
    };

    // ── nether ── A player's write into the Nether: the block, its chunk's
    // block light, the players there, and the Nether's own rules queue.
    // ── end ── and the End's, with `dimension`.
    const auto set_block_in_nether = [&](net::WirePosition position, registry::BlockStateId state,
                                         DimensionId dimension = DimensionId::Nether) {
        if (!dimension_info(dimension).shape.contains_y(position.y)) {
            return;
        }
        {
            const std::scoped_lock lock{chunk_mutex};
            apply_block_change_in_nether(position, state, dimension);
            // ── light ── repaired around the edit, across chunk borders
            if (const auto [engine, source] = light_of(dimension); engine != nullptr) {
                engine->block_changed(BlockPos{position.x, position.y, position.z});
                (void)engine->propagate(*source);
            }
        }
        broadcast_in(dimension, nullptr, net::clientbound::kBlockUpdate,
                     net::encode_block_update(position, static_cast<i32>(state.value())));
        notify_change_in(dimension, position);
    };
    const auto set_block_and_broadcast = [&](net::WirePosition      position,
                                             registry::BlockStateId state) {
        const ScopedLatency perf_edit{perf->block_edit};  // ── perf ──
        const auto shape = world::WorldShape::overworld();
        if (!shape.contains_y(position.y)) {
            return;
        }

        const i32 chunk_x = position.x >> 4;
        const i32 chunk_z = position.z >> 4;
        {
            const std::scoped_lock lock{chunk_mutex};
            world::Chunk&          chunk = chunk_at(chunk_x, chunk_z);

            write_block(chunk, position.x, position.y, position.z, state);
            dirty_chunks.insert(chunk_key(chunk_x, chunk_z));

            // ── perf ── The light has moved too. Not here, though: relighting
            // before the Block Update went out was the break latency players
            // felt (docs/provenance/performance-tick.md). The edit is noted
            // and the tick repairs the light around it (── light ──).
            if (light) {
                light->block_changed(BlockPos{position.x, position.y, position.z});
            }
            // ── end perf ──

            for (i32 dz = -1; dz <= 1; ++dz) {
                for (i32 dx = -1; dx <= 1; ++dx) {
                    if (chunks.contains(ChunkPos{chunk_x + dx, chunk_z + dz})) {
                        dirty_chunks.insert(chunk_key(chunk_x + dx, chunk_z + dz));
                    }
                }
            }
        }

        const auto framed =
            net::encode_packet(net::clientbound::kBlockUpdate,
                               net::encode_block_update(position, static_cast<i32>(state.value())));
        if (!framed) {
            return;
        }
        for (auto& [key, other] : players) {
            // ── nether ── An overworld block, for the overworld's players.
            if (other.connection && other.dimension == DimensionId::Overworld) {
                other.connection->send(*framed);
            }
        }

        // And tell the rules. Every write that is not the drain's own goes
        // through here — a placement, a break, a sign, a furnace lighting up —
        // so this one line is what wakes the fluid beside a broken dam and the
        // wire beside a flipped lever. The drain writes through
        // `apply_block_change` instead and does its own notifying, which is
        // what keeps a settling puddle from re-queueing itself for ever.
        notify_change(position);
    };

    /// Place a block, then let it and its neighbours reshape around each other.
    ///
    /// A fence placed next to a fence has to reach out, and so does the one
    /// already standing there — the change goes both ways, which is why this
    /// cannot live inside placement alone. Breaking a block runs it too: the
    /// fence that was reaching for it has to let go.
    // ── nether ── `set_block_connected` in a given level.
    const auto set_block_connected_in = [&](DimensionId dimension, net::WirePosition position,
                                            registry::BlockStateId state) {
        const auto write = [&](net::WirePosition at, registry::BlockStateId what) {
            if (dimension != DimensionId::Overworld) {  // ── end ── the Nether or the End
                set_block_in_nether(at, what, dimension);
            } else {
                set_block_and_broadcast(at, what);
            }
        };
        if (!connections) {
            write(position, state);
            return;
        }
        std::array<std::pair<net::WirePosition, registry::BlockStateId>, 6> changes{};
        usize                                                               count = 0;
        {
            const std::scoped_lock lock{chunk_mutex};
            const auto             around = neighbours_of_in(dimension, position);
            const auto above = block_at_in(dimension, {position.x, position.y + 1, position.z});
            changes[count++] = {position, connections->reshape(state, around, above)};

            constexpr std::array<net::WirePosition, 4> kSteps{
                net::WirePosition{0, 0, -1}, net::WirePosition{0, 0, 1},
                net::WirePosition{-1, 0, 0}, net::WirePosition{1, 0, 0}};
            for (usize i = 0; i < kSteps.size(); ++i) {
                const net::WirePosition      side{position.x + kSteps[i].x, position.y,
                                                  position.z + kSteps[i].z};
                const registry::BlockStateId before = around[i];
                // The neighbour sees the new block, not the old one, so its own
                // view has to be built with the change already in place.
                auto view   = neighbours_of_in(dimension, side);
                view[i ^ 1] = state;
                const registry::BlockStateId after = connections->reshape(
                    before, view, block_at_in(dimension, {side.x, side.y + 1, side.z}));
                if (after != before) {
                    changes[count++] = {side, after};
                }
            }

            // And the block underneath. A wall reads what sits on its head —
            // a stone above turns its sides from low to tall — so placing
            // anything has to give the block below a second look.
            const net::WirePosition      under{position.x, position.y - 1, position.z};
            const registry::BlockStateId before = block_at_in(dimension, under);
            const registry::BlockStateId after =
                connections->reshape(before, neighbours_of_in(dimension, under), state);
            if (after != before) {
                changes[count++] = {under, after};
            }
        }
        for (usize i = 0; i < count; ++i) {
            write(changes[i].first, changes[i].second);
        }
    };
    const auto set_block_connected = [&](net::WirePosition position, registry::BlockStateId state) {
        set_block_connected_in(DimensionId::Overworld, position, state);
    };
    // ── end nether ──

    // ── scheduled ticks: fluids and redstone ──────────────────────
    //
    // The two queues and the engines that answer them. Everything about how
    // they are driven is in world_ticks.{hpp,cpp}; what is here is the four
    // hooks that join a `LevelWriter` to this server's chunks, and the buffers
    // the drain needs.
    //
    // The hooks never take `chunk_mutex`. The drain takes it **once**, around
    // the whole thing, for two reasons: `chunk_mutex` is not recursive, so a
    // rule that wrote through `set_block_and_broadcast` would deadlock the tick
    // thread the first time water moved; and a puddle settling is a few hundred
    // writes, which would otherwise be a few hundred lock round-trips inside
    // one tick.

    /// Positions written by the drain, to be broadcast once the lock is
    /// released. Reused between ticks so the drain never allocates.
    std::vector<std::pair<net::WirePosition, registry::BlockStateId>> tick_broadcasts;

    // ── containers: the machines that move items on their own ─────
    //
    // Declared here because `hooks.set_block` below has to tell it about every
    // write: a dispenser fires on the rising edge of `triggered`, and the only
    // place both sides of that edge exist is inside the write.
    std::optional<ItemTransport> item_transport;
    if (blocks && registries) {
        item_transport.emplace(*blocks, *registries, recipe_book ? &*recipe_book : nullptr);
    }
    /// The three buffers the container pass reuses between ticks. Reused rather
    /// than built per tick for the reason every other buffer here is: the tick
    /// body must not allocate in steady state.
    std::vector<ChunkPos>                                     transport_chunks;
    std::vector<std::pair<BlockPos, net::ItemStack>>          transport_ejected;
    std::vector<net::WirePosition>                            transport_touched;
    // ── nether ── What the Nether level's drain wrote, sent once it is done.
    std::vector<std::pair<net::WirePosition, registry::BlockStateId>> nether_tick_broadcasts;
    std::vector<std::pair<net::WirePosition, registry::BlockStateId>> end_tick_broadcasts;  // ── end ──

    if (blocks && registries) {
        LevelHooks hooks;
        hooks.block_at = [&](BlockPos pos) -> registry::BlockStateId {
            // `chunk_if_resident`, never `chunk_at`: a rule reading into a
            // chunk that is not loaded yet must get "air, and not loaded"
            // rather than three hundred milliseconds of worldgen on the tick
            // thread. Fluids ask `is_loaded` for exactly this reason.
            const world::Chunk* chunk = chunk_if_resident(pos.x >> 4, pos.z >> 4);
            if (chunk == nullptr) {
                return registry::kAirState;
            }
            return chunk->get_block(static_cast<usize>(pos.x & 15), pos.y,
                                    static_cast<usize>(pos.z & 15));
        };
        hooks.is_loaded = [&](BlockPos pos) {
            return chunk_if_resident(pos.x >> 4, pos.z >> 4) != nullptr;
        };
        hooks.set_block = [&](BlockPos pos, registry::BlockStateId state) {
            const net::WirePosition where{pos.x, pos.y, pos.z};
            // Read before the write: a dispenser fires on the **rising edge**
            // of `triggered`, and the edge is a difference between two states.
            // Asking the world again after the write would only ever see the
            // new one, and a dispenser held triggered by a lever would fire
            // every tick instead of once.
            const registry::BlockStateId before = block_at(where);
            apply_block_change(where, state, false);
            if (item_transport) {
                item_transport->note_block_change(pos, before, state);
            }
            tick_broadcasts.emplace_back(where, state);
            if (sounds) {  // ── sound ── a button releasing, a door moved by power
                sounds->queue_changed(pos, before, state);
            }
            if (light) {  // ── light ──
                light->block_changed(pos);
            }
        };
        hooks.container_signal = [&](BlockPos pos) -> i32 {
            // ── rails ── a detector rail reads the container cart on it
            if (rails_session && level) {
                if (const i32 cart = rails_session->comparator_signal(*level, pos); cart >= 0) {
                    return cart;
                }
            }
            // A comparator behind a chest reads how full it is. -1 for "there
            // is no container here", which a comparator has to tell apart from
            // an empty one: an empty container gives 0 and a missing one lets
            // the ordinary signal through.
            world::Chunk* chunk = chunk_if_resident(pos.x >> 4, pos.z >> 4);
            if (chunk == nullptr) {
                return -1;
            }
            const world::BlockEntity* entity =
                chunk->block_entity_at(static_cast<usize>(pos.x & 15), pos.y,
                                       static_cast<usize>(pos.z & 15));
            // Every container, not just a chest: a barrel, a hopper, a dropper
            // and a furnace all answer a comparator, and each of them read as
            // "no container at all" until the model existed — which lets the
            // wire's own signal through and looks like a working circuit.
            const auto inventory =
                container_inventory(entity, registries ? &*registries : nullptr, item_registry);
            if (!inventory) {
                return -1;
            }
            return inventory->comparator_reading();
        };

        level.emplace(*blocks, std::move(hooks));
        world_ticks.emplace(*blocks, *registries);

        // ── nether ──
        // The Nether's level: the same engines over its own chunks, with its
        // own rules — `ultrawarm`, which is what makes lava run seven blocks
        // and every ten ticks rather than three and every thirty (fluid.hpp).
        // Its hooks read `nether` when called, so it may be built before the
        // Nether is.
        LevelHooks nether_hooks;
        nether_hooks.block_at = [&](BlockPos pos) -> registry::BlockStateId {
            const world::Chunk* chunk =
                chunk_if_resident_in(DimensionId::Nether, pos.x >> 4, pos.z >> 4);
            return chunk == nullptr ? registry::kAirState
                                    : chunk->get_block(static_cast<usize>(pos.x & 15), pos.y,
                                                       static_cast<usize>(pos.z & 15));
        };
        nether_hooks.is_loaded = [&](BlockPos pos) {
            return chunk_if_resident_in(DimensionId::Nether, pos.x >> 4, pos.z >> 4) != nullptr;
        };
        nether_hooks.set_block = [&](BlockPos pos, registry::BlockStateId state) {
            const net::WirePosition where{pos.x, pos.y, pos.z};
            apply_block_change_in_nether(where, state);
            nether_tick_broadcasts.emplace_back(where, state);
        };
        // No container in the Nether answers a comparator yet: named, not
        // defaulted — see docs/provenance/nether.md.
        nether_hooks.container_signal = [](BlockPos) -> i32 { return -1; };
        nether_level.emplace(*blocks, std::move(nether_hooks));
        nether_level->set_dimension(dimension_info(DimensionId::Nether).shape,
                                    dimension_info(DimensionId::Nether).traits);
        nether_tick_broadcasts.reserve(1024);
        if (portal_rules) {
            world_ticks->attach_portals(&*portal_rules);
        }
        // ── end nether ──
        // ── end ── The End's level: the same engines over its chunks. No
        // container there answers a comparator yet, as in the Nether.
        LevelHooks end_hooks;
        end_hooks.block_at = [&](BlockPos pos) -> registry::BlockStateId {
            const world::Chunk* chunk =
                chunk_if_resident_in(DimensionId::End, pos.x >> 4, pos.z >> 4);
            return chunk == nullptr ? registry::kAirState
                                    : chunk->get_block(static_cast<usize>(pos.x & 15), pos.y,
                                                       static_cast<usize>(pos.z & 15));
        };
        end_hooks.is_loaded = [&](BlockPos pos) {
            return chunk_if_resident_in(DimensionId::End, pos.x >> 4, pos.z >> 4) != nullptr;
        };
        end_hooks.set_block = [&](BlockPos pos, registry::BlockStateId state) {
            const net::WirePosition where{pos.x, pos.y, pos.z};
            apply_block_change_in_nether(where, state, DimensionId::End);
            end_tick_broadcasts.emplace_back(where, state);
        };
        end_hooks.container_signal = [](BlockPos) -> i32 { return -1; };
        end_level.emplace(*blocks, std::move(end_hooks));
        end_level->set_dimension(dimension_info(DimensionId::End).shape,
                                 dimension_info(DimensionId::End).traits);
        end_tick_broadcasts.reserve(1024);
        // ── end end ──
        // ── tnt and gravity ──
        tnt_gravity.emplace(*blocks, *registries, loot_tables ? &*loot_tables : nullptr,
                            mob_combat ? &*mob_combat : nullptr);
        tnt_gravity->set_redstone(&world_ticks->redstone());
        world_ticks->set_extension(&*tnt_gravity);
        // ── rails ── rails and carts; the carts come from entities/ through
        // the entity storage, chunk by chunk (── entities ──, below)
        rails_session.emplace(*blocks, *registries);
        world_ticks->set_rails_extension(&*rails_session);
        rails_session->set_blasts(&tnt_gravity->blasts());
        // ── end rails ──
        // ── projectiles ──
        projectiles.emplace(*registries, *blocks, mob_combat ? &*mob_combat : nullptr);
        brewing.emplace(*registries);  // ── brewing ──
        brewing_chunks.reserve(1024);  // ── brewing ──
        // ── husbandry ──
        husbandry.emplace(*registries, *blocks);
        taming.emplace(*registries);  // ── tame ──
        slimes.emplace(*registries);  // ── mobs-2 ──
        drowning.emplace(*registries);
        mob_attacks.emplace(*registries, mob_combat ? &*mob_combat : nullptr);  // ── mobs-3 ──
        mob_kills.reserve(16);                                                  // ── mobs-3 ──
        mob_despawn.emplace(*registries);                                       // ── mobs-3 ──
        mobs3_players.reserve(16);                                              // ── mobs-3 ──
        zombie_villagers.emplace(*registries);                                  // ── mobs-3 ──
        entity_storage.emplace(*registries, level_dir / "entities");            // ── mobs-3 ──
        entity_storage->add_adopter(*rails_session);  // ── entities ── the carts
        // ── persistence ── the TNT, the falling blocks, the projectiles, the
        // clouds; the items and orbs further down, once their host exists
        entity_storage->add_adopter(*tnt_gravity);
        entity_storage->add_adopter(*projectiles);
        entity_storage->add_loose(*brewing);
        // ── villagers ──
        villagers.emplace(*registries, *blocks);
        tick_broadcasts.reserve(4096);
    } else {
        OV_LOG_WARN("no block registry — fluids and redstone stay inert");
    }

    // ── weather ─────────────────────────────────────────────────────────────
    // Precipitation, lightning and beds (weather_session.hpp), over the world
    // state the command engine owns. A dedicated server announces the
    // sleeping count; an integrated one, like the game's, does not.
    std::optional<WeatherSession> weather;
    if (blocks && registries && level && commands) {
        weather.emplace(*blocks, *registries, mob_combat ? &*mob_combat : nullptr,
                        0x5745'4154'4845'5231ULL);
        weather->set_announce(external_stop == nullptr);
        // A measurement knob, like OV_RANDOM_TICK_SPEED: the end-to-end check
        // lowers the 1-in-100 000 so bolts fall in seconds. Read once.
        if (const char* chance = std::getenv("OV_THUNDER_CHANCE"); chance != nullptr) {
            weather->set_thunder_chance(static_cast<i32>(std::strtol(chance, nullptr, 10)));
            OV_LOG_INFO("thunder chance 1 in {} (OV_THUNDER_CHANCE)", chance);
        }
    }
    // ── end weather ─────────────────────────────────────────────────────────

    // ── agriculture ─────────────────────────────────────────────────────────
    //
    // The random tick and the plants that answer it. How they behave is in
    // gameplay/plants.{hpp,cpp}, how positions are picked in agriculture.{hpp,
    // cpp}; what is here is the light and the loot a plant reaches for over
    // this server's chunks. Two environments: the tick thread's, which runs
    // with `chunk_mutex` held, and the right-click's, which takes it itself.
    std::optional<gameplay::Plants>       plants;
    std::unique_ptr<TreeGrower>           tree_grower;
    std::optional<ServerPlantEnvironment> plant_env;
    std::optional<ServerPlantEnvironment> plant_env_player;
    /// `randomTickSpeed` lives here: `random_ticks.set_speed(n)` is the gamerule.
    RandomTicks                           random_ticks{0x4f56'4147'5249'0001ULL};
    std::vector<Vec3d>                    random_tick_players;
    /// Loot of blocks a rule broke — a leaf decaying, a crop uprooted —
    /// published after the tick, under `players_mutex`. Own mutex: a right-click
    /// can drop too, from the network thread.
    std::mutex                            plant_drops_mutex;
    std::vector<ItemEntity>               plant_drops;
    std::vector<ItemEntity>               plant_drops_out;
    math::XoroshiroRandomSource           plant_loot_random{0x6A09E667F3BCC908ULL, 0xBB67AE8584CAA73BULL};
    const auto stored_light = [&](BlockPos pos, bool sky) -> u8 {
        const world::Chunk* chunk = chunk_if_resident(pos.x >> 4, pos.z >> 4);
        if (chunk == nullptr) {
            return 0;
        }
        const world::ChunkSection* section = chunk->section_for_y(pos.y);
        if (section == nullptr) {
            return sky && pos.y > 0 ? u8{15} : u8{0};
        }
        const usize index = world::section_index(static_cast<usize>(pos.x & 15),
                                                 static_cast<usize>(pos.y & 15),
                                                 static_cast<usize>(pos.z & 15));
        return sky ? section->sky_light().get(index) : section->block_light().get(index);
    };
    /// Caller holds `chunk_mutex` (it reads the two neighbours a table asks about).
    const auto plant_drop = [&](BlockPos pos, registry::BlockStateId state) {
        if (!loot_tables) {
            return;
        }
        const registry::BlockStateId above = block_at({pos.x, pos.y + 1, pos.z});
        const registry::BlockStateId below = block_at({pos.x, pos.y - 1, pos.z});
        const std::scoped_lock       lock{plant_drops_mutex};
        std::vector<gameplay::Drop>  drops;
        loot_tables->drops(state, gameplay::Held{}, plant_loot_random, drops,
                           gameplay::Neighbours{.above = above, .below = below});
        for (const gameplay::Drop& drop : drops) {
            ItemEntity item;
            item.entity_id = next_entity_id.fetch_add(1);
            item.uuid      = net::Uuid{0x4f564954454d0000ULL | static_cast<u64>(item.entity_id),
                                       static_cast<u64>(item.entity_id) * 0x9E3779B97F4A7C15ULL};
            item.x         = static_cast<f64>(pos.x) + 0.5;
            item.y         = static_cast<f64>(pos.y) + 0.25;
            item.z         = static_cast<f64>(pos.z) + 0.5;
            item.stack = net::ItemStack{drop.item, static_cast<i8>(std::min(drop.count, 64)), {}};
            item.born  = server_tick.load(std::memory_order_relaxed);
            plant_drops.push_back(std::move(item));
        }
    };
    if (blocks && registries && world_ticks) {
        plants.emplace(*blocks, *registries);
        tree_grower = TreeGrower::load(std::filesystem::path{OV_DATA_DIR}, *blocks);
        if (!tree_grower) {
            OV_LOG_WARN("tree features could not be read — saplings will not grow");
        }
        PlantHooks tick_hooks;
        tick_hooks.block_light = [&](BlockPos pos) { return stored_light(pos, false); };
        tick_hooks.sky_light   = [&](BlockPos pos) { return stored_light(pos, true); };
        tick_hooks.sky_darken  = [&] {
            // ── weather: the day and the storm, not the tick count ──
            return commands ? WeatherSession::sky_darken(commands->world())
                            : sky_darken_for(server_tick.load(std::memory_order_relaxed));
        };
        tick_hooks.drop_block = plant_drop;
        // ── weather: farmland counts rain as water ──
        tick_hooks.is_raining_at = [&](BlockPos pos) {
            return weather && commands && weather->is_raining_at(chunks, pos, commands->world().weather);
        };
        plant_env.emplace(std::move(tick_hooks), tree_grower.get());

        PlantHooks player_hooks;
        player_hooks.block_light = [&](BlockPos pos) {
            const std::scoped_lock chunk_lock{chunk_mutex};
            return stored_light(pos, false);
        };
        player_hooks.sky_light = [&](BlockPos pos) {
            const std::scoped_lock chunk_lock{chunk_mutex};
            return stored_light(pos, true);
        };
        player_hooks.sky_darken = [&] {
            return sky_darken_for(server_tick.load(std::memory_order_relaxed));
        };
        player_hooks.drop_block = [&](BlockPos pos, registry::BlockStateId state) {
            const std::scoped_lock chunk_lock{chunk_mutex};
            plant_drop(pos, state);
        };
        plant_env_player.emplace(std::move(player_hooks), tree_grower.get());

        world_ticks->attach_plants(*plants, *plant_env);
        // A measurement knob until `/gamerule randomTickSpeed` exists: the
        // end-to-end campaign runs the server at a speed where a minute shows
        // something. Read once, at start-up, never in the tick.
        if (const char* speed = std::getenv("OV_RANDOM_TICK_SPEED"); speed != nullptr) {
            random_ticks.set_speed(static_cast<i32>(std::strtol(speed, nullptr, 10)));
            OV_LOG_INFO("randomTickSpeed {} (OV_RANDOM_TICK_SPEED)", random_ticks.speed());
        }
        random_tick_players.reserve(64);
        plant_drops.reserve(256);
        plant_drops_out.reserve(256);
    }
    // ── end agriculture ─────────────────────────────────────────────────────

    // ── fire ────────────────────────────────────────────────────────────────
    // The fire block, the lava that lights it, and what burns: fire_session.hpp.
    // Every read below runs on the tick thread with `chunk_mutex` held.
    std::optional<FireSession> fire_session;
    if (blocks && registries && world_ticks) {
        FireHost fire_host;
        fire_host.block_at = [&](BlockPos pos) {
            return block_at(net::WirePosition{pos.x, pos.y, pos.z});
        };
        fire_host.biome_at = [&](BlockPos pos) -> i32 {
            const world::Chunk* chunk = chunk_if_resident(pos.x >> 4, pos.z >> 4);
            return chunk == nullptr ? -1
                                    : static_cast<i32>(chunk->get_biome(
                                          static_cast<usize>(pos.x & 15), pos.y,
                                          static_cast<usize>(pos.z & 15)));
        };
        fire_host.rain_top = [&](i32 x, i32 z) -> i32 {
            const world::Chunk* chunk = chunk_if_resident(x >> 4, z >> 4);
            return chunk == nullptr ? (1 << 30)
                                    : chunk->heightmap(world::HeightmapType::MotionBlocking)
                                          .first_free(static_cast<usize>(x & 15),
                                                      static_cast<usize>(z & 15));
        };
        fire_host.sky_light   = [&](BlockPos pos) { return stored_light(pos, true); };
        fire_host.block_light = [&](BlockPos pos) { return stored_light(pos, false); };
        fire_host.prime_tnt   = [&](BlockPos pos) {
            if (tnt_gravity) {
                tnt_gravity->request_prime(pos, gameplay::kTntFuseTicks);
            }
        };
        fire_session.emplace(*blocks, *registries, std::move(fire_host),
                             mob_combat ? &*mob_combat : nullptr);
        world_ticks->set_fire_extension(&*fire_session);
        random_ticks.set_extension(&*fire_session);
        if (item_use) {
            item_use->set_fire_rules(&fire_session->rules());
        }
    }
    // Campfires: the grill (campfire.hpp), cooking by the `campfire_cooking`
    // recipes. The host runs with both locks held.
    std::optional<Campfires> campfires;
    std::vector<ChunkPos>    campfire_chunks;
    CampfireHost             campfire_host;
    if (blocks && registries && recipe_book) {
        campfires.emplace(*blocks, *registries, *recipe_book);
        campfire_chunks.reserve(1024);
        campfire_host.each_chunk = [&](const std::function<void(world::Chunk&)>& visit) {
            campfire_chunks.clear();
            chunks.for_each(
                [&](ChunkPos pos, const world::Chunk&) { campfire_chunks.push_back(pos); });
            for (const ChunkPos pos : campfire_chunks) {
                if (world::Chunk* chunk = chunks.find(pos); chunk != nullptr) {
                    visit(*chunk);
                }
            }
        };
        campfire_host.drop_item = [&](Vec3d at, const net::ItemStack& stack) {
            std::vector<ItemEntity> one(1);
            one[0].entity_id = next_entity_id.fetch_add(1);
            one[0].uuid      = net::Uuid{0x4f564954454d0000ULL | static_cast<u64>(one[0].entity_id),
                                         static_cast<u64>(one[0].entity_id) * 0x9E3779B97F4A7C15ULL};
            one[0].x         = at.x;
            one[0].y         = at.y;
            one[0].z         = at.z;
            one[0].stack     = stack;
            one[0].born      = server_tick.load(std::memory_order_relaxed);
            publish_items(one);
        };
        campfire_host.send_entity = [&](BlockPos pos, const world::BlockEntity& entity) {
            broadcast(nullptr, net::clientbound::kBlockEntityData,
                      net::encode_block_entity_data(net::WirePosition{pos.x, pos.y, pos.z},
                                                    entity.type_id, entity.data));
        };
        campfire_host.mark_dirty = [&](i32 cx, i32 cz) { dirty_chunks.insert(chunk_key(cx, cz)); };
    }
    // ── end fire ────────────────────────────────────────────────────────────

    /// Send what the drain wrote, and relight the chunks it touched.
    ///
    /// Called with `chunk_mutex` **released**: broadcasting walks the player
    /// map under `players_mutex`, and the two locks taken in the other order
    /// anywhere else would be a deadlock waiting for a busy server.
    const auto flush_tick_writes = [&] {
        {  // ── perf ── under the lock: the network thread feeds the set too
            const std::scoped_lock lock{chunk_mutex};
            if (light && light->pending() > 0) {  // ── light ── incremental
                const TickPhase perf_was = perf->enter(TickPhase::Relight);
                if (light_full_recompute) {
                    std::unordered_set<i64> relit;
                    const ChunkLookup       lookup = [&](i32 nx, i32 nz) -> world::Chunk* {
                        return chunks.find(ChunkPos{nx, nz});
                    };
                    for (const BlockPos pos : light->pending_positions()) {
                        if (relit.insert(chunk_key(pos.x >> 4, pos.z >> 4)).second) {
                            relight_after_edit(lookup, pos.x >> 4, pos.z >> 4, &*blocks);
                        }
                    }
                    light->discard_pending();
                } else {
                    (void)light->propagate(light_chunks);
                }
                perf->enter(perf_was);
            }
        }  // ── end perf ──

        if (tick_broadcasts.empty()) {
            return;
        }
        const std::unique_lock lock{players_mutex, std::try_to_lock};
        if (lock.owns_lock()) {
            if (sounds) {  // ── sound ── what the drain toggled, to everyone near
                sounds->flush(sound_host);
            }
            for (const auto& [where, state] : tick_broadcasts) {
                const auto framed = net::encode_packet(
                    net::clientbound::kBlockUpdate,
                    net::encode_block_update(where, static_cast<i32>(state.value())));
                if (!framed) {
                    continue;
                }
                for (auto& [key, other] : players) {
                    // ── nether ── The overworld's drain, the overworld's players.
                    if (other.connection && other.dimension == DimensionId::Overworld) {
                        other.connection->send(*framed);
                    }
                }
            }
            // ── rails ── Cleared only once sent. Cleared unconditionally, a
            // tick whose try-lock lost to the network thread threw its Block
            // Updates away: the world held the rails' new shapes and the
            // client kept the placement's, two rails of fourteen in one run
            // of scripts/check_rails_e2e.py. Kept, they go with the next flush.
            tick_broadcasts.clear();
        }
    };

    // ── nether ── The same for the Nether level's drain: block light only.
    // ── end ── And for the End's: its own writes, its own players.
    const auto flush_nether_tick_writes =
        [&](std::vector<std::pair<net::WirePosition, registry::BlockStateId>>& writes,
            DimensionId dimension) {
        if (writes.empty()) {
            return;
        }
        // ── light ── the drain's writes as one batch, repaired once
        if (const auto [engine, source] = light_of(dimension);
            engine != nullptr && other_world(dimension) != nullptr) {
            const std::scoped_lock lock{chunk_mutex};
            for (const auto& [where, state] : writes) {
                engine->block_changed(BlockPos{where.x, where.y, where.z});
            }
            (void)engine->propagate(*source);
        }
        const std::unique_lock lock{players_mutex, std::try_to_lock};
        if (lock.owns_lock()) {
            for (const auto& [where, state] : writes) {
                broadcast_in(dimension, nullptr, net::clientbound::kBlockUpdate,
                             net::encode_block_update(where, static_cast<i32>(state.value())));
            }
        }
        writes.clear();
    };

    // ── crafting and smelting ───────────────────────────────────────────────
    // What a crafting or furnace screen reaches outside itself for. The screen
    // itself is in workbench.cpp and knows nothing about connections, chunks or
    // players; this is the only place the two meet.
    //
    // Caller holds players_mutex. `chunk_mutex` is taken here rather than by
    // the caller, so that no path can take the two in the other order.
    const auto workbench_host = [&](Player& who, const auto& send) {
        WorkbenchHost host;
        host.send = [&send](i32 id, std::vector<u8> payload) { send(id, payload); };
        host.drop = [&](const net::ItemStack& stack) {
            ItemEntity item;
            item.entity_id = next_entity_id.fetch_add(1);
            item.uuid      = net::Uuid{0x4f564954454d0000ULL | static_cast<u64>(item.entity_id),
                                       static_cast<u64>(item.entity_id) * 0x9E3779B97F4A7C15ULL};
            item.x         = who.x;
            item.y         = who.y + 1.0;
            item.z         = who.z;
            item.stack     = stack;
            item.born      = server_tick.load(std::memory_order_relaxed);
            item.pickup_delay        = 40;
            std::vector<ItemEntity> one;
            one.push_back(std::move(item));
            publish_items(one);
        };
        host.block_entity = [&](i32 x, i32 y, i32 z) -> nbt::Tag* {
            world::BlockEntity* entity =
                chunk_at(x >> 4, z >> 4)
                    .block_entity_at(static_cast<usize>(x & 15), y, static_cast<usize>(z & 15));
            return entity != nullptr ? &entity->data : nullptr;
        };
        host.block_name = [&](i32 x, i32 y, i32 z) -> std::string_view {
            if (!blocks) {
                return {};
            }
            const registry::BlockStateId state = block_at({x, y, z});
            return blocks->block_name(blocks->block_of(state));
        };
        host.set_lit = [&](i32 x, i32 y, i32 z, bool lit) {
            if (!blocks) {
                return;
            }
            const registry::BlockStateId state = block_at({x, y, z});
            const registry::BlockId      block = blocks->block_of(state);
            // `lit` is a property, so the new state is the same block with one
            // value changed — not a different block. Looking the block up by
            // name would find `minecraft:furnace` either way and lose the
            // facing the player placed it with.
            const auto property = blocks->find_property(block, "lit");
            if (!property) {
                return;
            }
            const auto wanted = lit ? std::string_view{"true"} : std::string_view{"false"};
            for (u16 index = 0; index < property->values.size(); ++index) {
                if (property->values[index] != wanted) {
                    continue;
                }
                const registry::BlockStateId next =
                    blocks->with_property(state, *property, index);
                // Written here rather than through set_block_and_broadcast,
                // which takes chunk_mutex itself — and the caller already holds
                // it. std::mutex is not recursive, so going through it would
                // deadlock the tick thread the first time a furnace lit up.
                chunk_at(x >> 4, z >> 4)
                    .set_block(static_cast<usize>(x & 15), y, static_cast<usize>(z & 15), next);
                dirty_chunks.insert(chunk_key(x >> 4, z >> 4));
                const auto framed = net::encode_packet(
                    net::clientbound::kBlockUpdate,
                    net::encode_block_update({x, y, z}, static_cast<i32>(next.value())));
                if (framed) {
                    for (auto& [other_key, other] : players) {
                        if (other.connection) {
                            other.connection->send(*framed);
                        }
                    }
                }
                return;
            }
        };
        host.mark_dirty = [&](i32 x, i32 z) { dirty_chunks.insert(chunk_key(x >> 4, z >> 4)); };
        host.award_experience = [](f32) {
            // Experience orbs are another agent's milestone. The furnace stops
            // holding what it has handed over either way, so the amount is not
            // lost twice — but nothing shows it yet, and saying so is better
            // than a silent zero.
        };
        return host;
    };

    // ── enchanting ──────────────────────────────────────────────────────────
    // The table, the anvil and the grindstone reach outside themselves through
    // this, as the workbench does above — and borrow its first three sinks.
    // Caller holds players_mutex and chunk_mutex.
    const auto enchant_host = [&](Player& who, const auto& send) {
        EnchantHost         host;
        const WorkbenchHost bench = workbench_host(who, send);
        host.send                 = bench.send;
        host.drop                 = bench.drop;
        host.block_name           = bench.block_name;
        host.replace_block = [&](i32 x, i32 y, i32 z, std::string_view next) {
            if (!blocks) {
                return;
            }
            const registry::BlockStateId was = block_at({x, y, z});
            const auto target =
                blocks->find_block(next.empty() ? std::string_view{"minecraft:air"} : next);
            if (!target) {
                return;
            }
            registry::BlockStateId now = blocks->default_state(*target);
            // The next stage keeps the way the anvil faces.
            const auto from = blocks->find_property(blocks->block_of(was), "facing");
            const auto to   = blocks->find_property(*target, "facing");
            if (from && to) {
                const std::string_view facing = blocks->property_value(was, *from);
                for (u16 index = 0; index < to->values.size(); ++index) {
                    if (to->values[index] == facing) {
                        now = blocks->with_property(now, *to, index);
                    }
                }
            }
            chunk_at(x >> 4, z >> 4)
                .set_block(static_cast<usize>(x & 15), y, static_cast<usize>(z & 15), now);
            dirty_chunks.insert(chunk_key(x >> 4, z >> 4));
            broadcast(nullptr, net::clientbound::kBlockUpdate,
                      net::encode_block_update({x, y, z}, static_cast<i32>(now.value())));
        };
        host.hover_name = [&](const net::ItemStack& stack) -> std::string {
            if (!commands || !registries || !item_registry) {
                return {};
            }
            std::optional<nbt::Tag> tag;
            if (!stack.nbt.empty()) {
                if (auto document = nbt::read(stack.nbt)) {
                    tag = std::move(document->root);
                }
            }
            const std::string_view id = registries->entry_of(*item_registry, stack.item_id);
            return cmd::plain(cmd::item_name(id, tag ? &*tag : nullptr, commands->env(),
                                             commands->lang()),
                              commands->lang());
        };
        host.level       = [&who] { return who.survival.experience_level; };
        host.take_levels = [&who](i32 n) { ov::server::take_levels(who.survival, n); };
        host.creative    = [&who] { return who.game_mode == 1; };
        host.xp_seed     = [&who] { return who.xp_seed; };
        host.set_xp_seed = [&who](i32 seed) { who.xp_seed = seed; };
        host.spawn_experience = [&](i32 value, f64 x, f64 y, f64 z) {
            GroundOrb orb;
            orb.entity_id = next_entity_id.fetch_add(1);
            orb.x         = x;
            orb.y         = y;
            orb.z         = z;
            orb.value     = value;
            orb.born      = server_tick.load(std::memory_order_relaxed);
            orb.dimension = who.dimension;  // ── persistence ──
            broadcast(nullptr, net::clientbound::kSpawnExperienceOrb,
                      net::encode_spawn_experience_orb(orb.entity_id, orb.x, orb.y, orb.z,
                                                       static_cast<i16>(orb.value)));
            ground_orbs.push_back(orb);
        };
        host.level_event = [&](i32 event, i32 x, i32 y, i32 z) {
            broadcast(nullptr, net::clientbound::kWorldEvent,
                      net::encode_world_event(event, {x, y, z}, 0, false));
        };
        host.random = &who.enchant_random;
        return host;
    };
    // ── end enchanting ──────────────────────────────────────────────────────
    // ── brewing ─────────────────────────────────────────────────────────────
    // What a brewing stand reaches outside itself for (brewing_session.hpp).
    // Built once; every caller holds players_mutex then chunk_mutex.
    StandHost brewing_stand_host;
    brewing_stand_host.chunk      = [&](i32 cx, i32 cz) { return chunk_if_resident(cx, cz); };
    brewing_stand_host.mark_dirty = [&](i32 cx, i32 cz) { dirty_chunks.insert(chunk_key(cx, cz)); };
    brewing_stand_host.set_bottles = [&](BlockPos at, std::array<bool, 3> bottles) {
        world::Chunk* chunk = blocks ? chunk_if_resident(at.x >> 4, at.z >> 4) : nullptr;
        if (chunk == nullptr) {
            return;
        }
        const net::WirePosition      where{at.x, at.y, at.z};
        const registry::BlockStateId before = block_at(where);
        registry::BlockStateId       state  = before;
        constexpr std::array<std::string_view, 3> kNames{"has_bottle_0", "has_bottle_1",
                                                         "has_bottle_2"};
        for (usize i = 0; i < kNames.size(); ++i) {
            const auto property = blocks->find_property(blocks->block_of(state), kNames[i]);
            if (!property) {
                return;
            }
            for (u16 v = 0; v < property->values.size(); ++v) {
                if (property->values[v] == (bottles[i] ? "true" : "false")) {
                    state = blocks->with_property(state, *property, v);
                }
            }
        }
        if (state == before) {
            return;
        }
        // `write_block`: a property change keeps the block entity — the stand's
        // potions — as vanilla's `setBlock` does.
        write_block(*chunk, at.x, at.y, at.z, state);
        dirty_chunks.insert(chunk_key(at.x >> 4, at.z >> 4));
        broadcast(nullptr, net::clientbound::kBlockUpdate,
                  net::encode_block_update(where, static_cast<i32>(state.value())));
    };
    brewing_stand_host.drop = [&](BlockPos at, const net::ItemStack& stack) {
        ItemEntity item;
        item.entity_id = next_entity_id.fetch_add(1);
        item.uuid      = net::Uuid{0x4f564954454d0000ULL | static_cast<u64>(item.entity_id),
                                   static_cast<u64>(item.entity_id) * 0x9E3779B97F4A7C15ULL};
        item.x         = static_cast<f64>(at.x) + 0.5;
        item.y         = static_cast<f64>(at.y) + 0.5;
        item.z         = static_cast<f64>(at.z) + 0.5;
        item.stack     = stack;
        item.born      = server_tick.load(std::memory_order_relaxed);
        std::vector<ItemEntity> one;
        one.push_back(std::move(item));
        publish_items(one);
    };
    // ── end brewing ─────────────────────────────────────────────────────────

    // ── combat and interaction ──────────────────────────────────────────────
    //
    // Six short pieces, and between them they turn `combat_session.hpp` and
    // `ItemUse` from written code into a lever that moves.
    //
    //   1. `player_level` — the LevelWriter a right-click writes through. See
    //      player_level.hpp for why `ServerLevel` is the wrong one.
    //   2. `held_name`, `held_weapon` and the four inventory sinks around them.
    //   3. `combat_view` — the Player record, as the session wants to read it.
    //   4. `hurt_mob` — the damage path, the death, and the loot.
    //   5. `combat_io` — the sinks, assembled per packet.
    //
    // Everything here runs on the **network thread**, inside the packet switch,
    // with `players_mutex` already held by the caller — which is also what
    // makes touching `mobs` safe against the tick's own mob block, since that
    // one takes the same lock before it runs.

    /// The blocks a player's right-click reads and writes.
    ///
    /// Built once. The hooks capture this frame, so a second one would be a
    /// second copy of the same six lambdas.
    PlayerLevel player_level{[&] {
        PlayerLevelHooks hooks;
        hooks.blocks   = blocks ? &*blocks : nullptr;
        hooks.block_at = [&](BlockPos pos) -> registry::BlockStateId {
            const std::scoped_lock chunk_lock{chunk_mutex};
            return block_at({pos.x, pos.y, pos.z});
        };
        hooks.is_loaded = [&](BlockPos pos) {
            const std::scoped_lock chunk_lock{chunk_mutex};
            return chunk_if_resident(pos.x >> 4, pos.z >> 4) != nullptr;
        };
        hooks.set_block = [&](BlockPos pos, registry::BlockStateId state) {
            // Through the placement path, not the drain's: this is a player's
            // edit, so it relights, broadcasts and queues the neighbour
            // notification exactly as placing a block does. Called with
            // `chunk_mutex` released — it takes it itself.
            registry::BlockStateId before{};  // ── sound ──
            if (sounds) {
                const std::scoped_lock chunk_lock{chunk_mutex};
                before = block_at({pos.x, pos.y, pos.z});
            }
            set_block_and_broadcast({pos.x, pos.y, pos.z}, state);
            // ── sound ── what the click set off, heard by everyone: the capture's
            // lever opens the gate beside it and both players hear the gate.
            if (sounds && !(sound_click && pos.x == sound_click->x && pos.z == sound_click->z &&
                            std::abs(pos.y - sound_click->y) <= 1)) {
                sounds->block_changed(sound_host, nullptr, pos, before, state);
            }
        };
        hooks.schedule_tick = [&](BlockPos pos, std::string_view what, i64 delay,
                                  world::TickQueue queue, world::TickPriority priority) {
            if (!level) {
                return;
            }
            // Straight into the tick thread's own queue. `BlockTickScheduler`
            // is not thread-safe and does not have to be: every access to it in
            // this server happens under `chunk_mutex`, and this is one more.
            const std::scoped_lock chunk_lock{chunk_mutex};
            level->schedule_tick(pos, what, delay, queue, priority);
        };
        hooks.has_scheduled_tick = [&](BlockPos pos, std::string_view what,
                                       world::TickQueue queue) {
            if (!level) {
                return false;
            }
            const std::scoped_lock chunk_lock{chunk_mutex};
            return level->has_scheduled_tick(pos, what, queue);
        };
        hooks.game_time = [&] { return server_tick.load(std::memory_order_relaxed); };
        return hooks;
    }()};

    // ── nether ── The same, for a right-click in the Nether: its chunks, its
    // queue, and its rules — `ultrawarm`, so a water bucket boils away.
    PlayerLevel nether_player_level{[&] {
        PlayerLevelHooks hooks;
        hooks.blocks   = blocks ? &*blocks : nullptr;
        hooks.block_at = [&](BlockPos pos) -> registry::BlockStateId {
            const std::scoped_lock chunk_lock{chunk_mutex};
            return block_at_in(DimensionId::Nether, {pos.x, pos.y, pos.z});
        };
        hooks.is_loaded = [&](BlockPos pos) {
            const std::scoped_lock chunk_lock{chunk_mutex};
            return chunk_if_resident_in(DimensionId::Nether, pos.x >> 4, pos.z >> 4) != nullptr;
        };
        hooks.set_block = [&](BlockPos pos, registry::BlockStateId state) {
            set_block_in_nether({pos.x, pos.y, pos.z}, state);
        };
        hooks.schedule_tick = [&](BlockPos pos, std::string_view what, i64 delay,
                                  world::TickQueue queue, world::TickPriority priority) {
            if (!nether_level) {
                return;
            }
            const std::scoped_lock chunk_lock{chunk_mutex};
            nether_level->schedule_tick(pos, what, delay, queue, priority);
        };
        hooks.has_scheduled_tick = [&](BlockPos pos, std::string_view what,
                                       world::TickQueue queue) {
            if (!nether_level) {
                return false;
            }
            const std::scoped_lock chunk_lock{chunk_mutex};
            return nether_level->has_scheduled_tick(pos, what, queue);
        };
        hooks.game_time = [&] { return server_tick.load(std::memory_order_relaxed); };
        return hooks;
    }()};
    nether_player_level.set_dimension(dimension_info(DimensionId::Nether).shape,
                                      dimension_info(DimensionId::Nether).traits);
    // ── end nether ──
    // ── end ── The same for a right-click in the End.
    PlayerLevel end_player_level{[&] {
        PlayerLevelHooks hooks;
        hooks.blocks   = blocks ? &*blocks : nullptr;
        hooks.block_at = [&](BlockPos pos) -> registry::BlockStateId {
            const std::scoped_lock chunk_lock{chunk_mutex};
            return block_at_in(DimensionId::End, {pos.x, pos.y, pos.z});
        };
        hooks.is_loaded = [&](BlockPos pos) {
            const std::scoped_lock chunk_lock{chunk_mutex};
            return chunk_if_resident_in(DimensionId::End, pos.x >> 4, pos.z >> 4) != nullptr;
        };
        hooks.set_block = [&](BlockPos pos, registry::BlockStateId state) {
            set_block_in_nether({pos.x, pos.y, pos.z}, state, DimensionId::End);
        };
        hooks.schedule_tick = [&](BlockPos pos, std::string_view what, i64 delay,
                                  world::TickQueue queue, world::TickPriority priority) {
            if (end_level) {
                const std::scoped_lock chunk_lock{chunk_mutex};
                end_level->schedule_tick(pos, what, delay, queue, priority);
            }
        };
        hooks.has_scheduled_tick = [&](BlockPos pos, std::string_view what,
                                       world::TickQueue queue) {
            if (!end_level) {
                return false;
            }
            const std::scoped_lock chunk_lock{chunk_mutex};
            return end_level->has_scheduled_tick(pos, what, queue);
        };
        hooks.game_time = [&] { return server_tick.load(std::memory_order_relaxed); };
        return hooks;
    }()};
    end_player_level.set_dimension(dimension_info(DimensionId::End).shape,
                                   dimension_info(DimensionId::End).traits);
    // ── end end ──

    /// The registry name of what a player is holding, empty for a bare hand.
    const auto held_name = [&](const Player& who) -> std::string_view {
        const net::ItemStack& held = who.inventory[36 + static_cast<usize>(who.held_slot)];
        if (held.item_id == 0 || held.count <= 0 || !registries || !item_registry) {
            return {};
        }
        return registries->entry_of(*item_registry, held.item_id);
    };

    /// What a player is holding, as the combat rules read it.
    ///
    /// Read per swing rather than stored: the player can change hands between
    /// two of them, and a cached weapon is a second thing that can be stale.
    const auto held_weapon = [&](const Player& who) {
        const net::ItemStack& held = who.inventory[36 + static_cast<usize>(who.held_slot)];
        gameplay::Weapon      weapon;
        weapon.item          = held_name(who);
        weapon.sharpness     = enchantment_level(held, "minecraft:sharpness");
        weapon.knockback     = enchantment_level(held, "minecraft:knockback");
        weapon.sweeping_edge = enchantment_level(held, "minecraft:sweeping");
        weapon.fire_aspect   = enchantment_level(held, "minecraft:fire_aspect");
        weapon.looting       = enchantment_level(held, "minecraft:looting");
        weapon.unbreaking    = enchantment_level(held, "minecraft:unbreaking");
        return weapon;
    };

    /// The `Damage` an item carries.
    ///
    /// The stack's NBT is kept as raw bytes on purpose — re-encoding a tag we
    /// do not understand loses data — so it is decoded only when a number is
    /// actually wanted from it, which is here and in `enchantment_level`.
    const auto held_damage = [&](const Player& who) -> i32 {
        const net::ItemStack& held = who.inventory[36 + static_cast<usize>(who.held_slot)];
        if (held.nbt.empty()) {
            return 0;
        }
        const auto document = nbt::read(held.nbt);
        if (!document) {
            return 0;
        }
        const nbt::Tag* damage = document->root.find("Damage");
        return damage == nullptr ? 0 : static_cast<i32>(damage->as_i64());
    };

    /// Tell one client about one of its own slots. Without it the client shows
    /// its predicted durability and then rolls it back.
    const auto send_slot = [&](Player& who, usize slot) {
        if (!who.connection) {
            return;
        }
        if (const auto framed = net::encode_packet(
                net::clientbound::kContainerSlot,
                net::encode_container_slot(0, 0, static_cast<i16>(slot), who.inventory[slot]))) {
            who.connection->send(*framed);
        }
    };

    const auto set_held_damage = [&](Player& who, i32 damage) {
        const usize     slot = 36 + static_cast<usize>(who.held_slot);
        net::ItemStack& held = who.inventory[slot];
        if (held.item_id == 0 || held.count <= 0) {
            return;
        }
        nbt::Document document;
        if (!held.nbt.empty()) {
            if (auto parsed = nbt::read(held.nbt)) {
                document = std::move(*parsed);
            }
        }
        if (document.root.type() != nbt::TagType::Compound) {
            document.root = nbt::Tag::make_compound();
        }
        (void)document.root.put("Damage", nbt::Tag{damage});
        held.nbt = nbt::write(document);
        send_slot(who, slot);
    };

    const auto break_held_item = [&](Player& who) {
        const usize slot    = 36 + static_cast<usize>(who.held_slot);
        who.inventory[slot] = net::ItemStack{};
        send_slot(who, slot);
        // Status 47 is "main-hand item broke" on a living entity: the crack and
        // the particle burst every client that can see the player plays.
        broadcast(nullptr, net::clientbound::kEntityEvent,
                  net::encode_entity_event(who.entity_id, 47));
    };

    const auto consume_one_held = [&](Player& who) {
        const usize     slot = 36 + static_cast<usize>(who.held_slot);
        net::ItemStack& held = who.inventory[slot];
        if (held.item_id == 0 || held.count <= 0) {
            return;
        }
        held.count = static_cast<i8>(held.count - 1);
        if (held.count <= 0) {
            held = net::ItemStack{};
        }
        send_slot(who, slot);
    };

    // ── end ── An eye went into a frame: consumed outside creative, the
    // frame's event (1503) for the level, and — when the ring opened — the
    // portal's (1038), which every player hears. Caller holds players_mutex.
    const auto end_eye_used = [&](Player& who, const net::UseItemOn& place,
                                  const gameplay::EyeOutcome& eye) {
        if (who.game_mode != 1) {
            consume_one_held(who);
        }
        broadcast_in(who.dimension, nullptr, net::clientbound::kWorldEvent,
                     net::encode_world_event(kWorldEventEyePlaced, place.position, 0, false));
        if (eye.result == gameplay::EyeUse::Activated) {
            const net::WirePosition centre{eye.portal_centre.x, eye.portal_centre.y,
                                           eye.portal_centre.z};
            broadcast_all(nullptr, net::clientbound::kWorldEvent,
                          net::encode_world_event(kWorldEventEndPortalOpened, centre, 0, true));
            registry::BlockStateId there{};
            {
                const std::scoped_lock chunk_lock{chunk_mutex};
                there = block_at_in(who.dimension, centre);
            }
            OV_LOG_INFO("{} completed an End portal at ({}, {}, {}): {}", who.name, centre.x,
                        centre.y, centre.z,
                        blocks ? blocks->block_name(blocks->block_of(there)) : "?");
        }
    };
    // ── end end ──

    // ── effects ─────────────────────────────────────────────────────────────
    /// The two sinks an effect packet goes to, for one player.
    const auto effect_io_for = [&](Player& who) {
        return EffectIo{
            .send =
                [&who](i32 id, std::span<const u8> payload) {
                    if (const auto framed = net::encode_packet(id, payload);
                        framed && who.connection) {
                        who.connection->send(*framed);
                    }
                },
            .broadcast = [&broadcast, &who](i32 id, std::span<const u8> payload) {
                broadcast(who.connection.get(), id, payload);
            }};
    };
    /// Who the effects are on: mortal only in survival, and the sneak and
    /// sprint bits of index 0 so an invisibility does not erase them.
    const auto effect_bearer_for = [&](const Player& who) {
        return EffectBearer{
            .entity_id    = who.entity_id,
            .mortal       = who.mortal(),  // ── commands: per player ──
            .shared_flags = static_cast<u8>((who.sneaking ? 0x02 : 0) |
                                            (who.survival.sprinting ? 0x08 : 0) |
                                            (who.survival.fire.on_fire() ? 0x01 : 0))};  // ── fire ──
    };
    // ── end effects ─────────────────────────────────────────────────────────

    // ── player data ─────────────────────────────────────────────────────────
    /// ── persistence ── The `RootVehicle` of a player leaving in a cart, by
    /// wire id, between the cart's taking and the file's writing.
    std::unordered_map<i32, nbt::Tag> leaving_vehicles;
    /// A player's record as they stand, their own game mode included.
    const auto player_record_of = [&](const Player& who) {
        usize        overflow = 0;
        PlayerRecord record   = capture_player(
            PlayerPose{who.x, who.y, who.z, who.yaw, who.pitch, who.on_ground},
            who.game_mode, who.inventory, who.carried, who.held_slot, who.survival,
            who.effects, &overflow);
        record.dimension = std::string{dimension_info(who.dimension).name};  // ── nether ──
        record.xp_seed = who.xp_seed;  // ── enchanting ──
        if (const auto vehicle = leaving_vehicles.find(who.entity_id);  // ── persistence ──
            vehicle != leaving_vehicles.end()) {
            record.root_vehicle = vehicle->second;
        }
        if (overflow > 0) {
            OV_LOG_WARN("{}: {} stacks from the crafting grid or the cursor found no free slot "
                        "and are not in the save",
                        who.name, overflow);
        }
        return record;
    };
    const auto save_player = [&](const net::Uuid& uuid, const std::string& name,
                                 const PlayerRecord& record) {
        if (!player_data.save(uuid, name, record)) {
            OV_LOG_WARN("could not write {} for {}", player_data.file_of(uuid).string(), name);
        }
    };
    /// Everyone online, snapshotted under the lock and written outside it.
    const auto save_online_players = [&] {
        std::vector<std::tuple<net::Uuid, std::string, PlayerRecord>> online;
        {
            const std::scoped_lock lock{players_mutex};
            for (const auto& [key, who] : players) {
                if (who.connection) {
                    online.emplace_back(who.uuid, who.name, player_record_of(who));
                }
            }
        }
        for (const auto& [uuid, name, record] : online) {
            save_player(uuid, name, record);
        }
    };
    // ── end player data ─────────────────────────────────────────────────────

    /// The player, as the combat session reads them.
    const auto combat_view = [&](const Player& who) {
        CombatPlayer view;
        view.entity_id     = who.entity_id;
        view.x             = who.x;
        view.y             = who.y;
        view.z             = who.z;
        view.yaw           = who.yaw;
        view.on_ground     = who.on_ground;
        view.sprinting     = who.survival.sprinting;
        view.sneaking      = who.sneaking;
        view.fall_distance = who.survival.health.fall_distance;
        // Named rather than guessed: this server does not yet know whether a
        // player's eyes are in water, whether they are on a ladder, blind or
        // riding. All four disqualify a critical, so leaving them false makes a
        // critical slightly too easy — a stated gap, not a silent one.
        view.in_water     = false;
        view.on_climbable = false;
        view.blind        = who.effects.blind();  // ── effects ──
        view.riding       = false;
        view.game_mode    = who.game_mode;  // ── commands: per player ──
        view.food         = who.survival.food.food;
        // Twenty. `FoodState` has no maximum of its own — the bar is
            // always twenty haunches — and `begin_use` compares against
            // this to refuse an eat a full player cannot make.
            view.max_food     = 20;
        view.strength = who.effects.strength();  // ── effects ──
        view.weakness = who.effects.weakness();  // ── effects ──
        return view;
    };

    /// How hard a mob resists being knocked back, by attribute id.
    const auto attribute_registry_id =
        registries ? registries->find("minecraft:attribute") : std::nullopt;
    const auto knockback_resistance_id =
        registries && attribute_registry_id
            ? registries->protocol_id(*attribute_registry_id,
                                      "minecraft:generic.knockback_resistance")
            : std::nullopt;

    /// Hurt one mob, and finish it if that was the last point.
    ///
    /// Caller holds players_mutex.
    const auto hurt_mob = [&](Player& attacker, i32 target_id, f32 damage, u8 looting) -> bool {
        if (nether_mobs && NetherMobs::owns(target_id)) {  // ── nether-2 ── one of the Nether's
            return nether_mobs->hurt(target_id, damage, attacker.entity_id, looting,
                                     nether_mob_host);
        }
        if (!mobs || !mob_combat) {
            return false;
        }
        const entity::EntityHandle handle = mobs->find(target_id);
        if (handle == entity::kNoEntity) {
            return false;
        }
        entity::EntityState* state = mobs->mutable_state(handle);
        if (state == nullptr || state->removed) {
            return false;
        }
        // ── tnt and gravity: a primed TNT or a falling block takes no damage ──
        if (tnt_gravity && tnt_gravity->owns(state->type)) {
            return true;
        }
        // ── projectiles: an arrow in the air is swung through ──
        if (projectiles && projectiles->owns(state->type)) {
            return true;
        }

        const MobHurt result = mob_combat->hurt(*state, damage, mob_damage_constants);
        if (!result.applied) {
            // The mob is there and the window swallowed the hit. True, not
            // false: false means "no such entity", and the session uses the
            // difference to decide whether the swing reached anything at all.
            return true;
        }
        // ── panic ── A hit frightens: `PanicGoal` reads what `frighten` records
        // (goals.hpp), and nothing on this server called it — a cow that was
        // hit went on grazing. For as long as the game remembers who last hurt
        // a mob, 100 ticks. A villager's panic is switched on by the same call.
        if (auto* hit = dynamic_cast<gameplay::Mob*>(mobs->logic(handle)); hit != nullptr) {
            constexpr i32 kLastHurtByMemoryTicks = 100;
            hit->frighten(kLastHurtByMemoryTicks);
        }
        if (taming) {  // ── tame ── a wolf pack's anger, a pet standing up, an owner's fight
            taming->on_player_hit(*mobs, attacker.entity_id, target_id,
                                  static_cast<i64>(server_tick.load(std::memory_order_relaxed)));
        }

        broadcast(nullptr, net::clientbound::kDamageEvent,
                  net::encode_damage_event(state->network_id,
                                           damage_type_id(gameplay::DamageKind::PlayerAttack),
                                           attacker.entity_id, attacker.entity_id));
        net::MetadataWriter fields;
        fields.float_value(net::metadata::kHealth, state->health);
        broadcast(nullptr, net::clientbound::kEntityMetadata,
                  net::encode_entity_metadata(state->network_id, fields.take()));

        if (sounds) {  // ── sound ── the hurt, or the death cry: everyone near
            if (result.killed) {
                sounds->mob_death(sound_host, state->type, state->position);
            } else {
                sounds->mob_hurt(sound_host, state->type, state->position);
            }
        }

        if (!result.killed) {
            return true;
        }

        // Status 3 on a living entity is the death animation. Sent before the
        // removal so the clients play it rather than making the mob vanish.
        broadcast(nullptr, net::clientbound::kEntityEvent,
                  net::encode_entity_event(state->network_id, 3));

        std::vector<gameplay::Drop> drops;
        const gameplay::DrawResult  drawn =
            mob_combat->loot(*state, true, looting, mob_loot_random, drops);
        if (!drawn.complete()) {
            // Counted rather than silent: two of 1.20.1's entity tables
            // delegate to a fishing table, which is not an entity table and is
            // not in this pack.
            OV_LOG_DEBUG("{} loot: {} referenced tables, {} unsupported entries",
                         mob_combat->type_name(*state), drawn.referenced_tables,
                         drawn.unsupported_entries);
        }

        std::vector<ItemEntity> dropped;
        dropped.reserve(drops.size());
        for (const gameplay::Drop& drop : drops) {
            ItemEntity item;
            item.entity_id = next_entity_id.fetch_add(1);
            item.uuid      = uuid_for_entity(item.entity_id);
            item.x         = state->position.x;
            // Half the mob's height, which is where vanilla drops from — not
            // its feet. A stack spawned at the feet of a mob standing on a slab
            // falls through it.
            item.y     = state->position.y + static_cast<f64>(state->height) * 0.5;
            item.z     = state->position.z;
            item.stack = net::ItemStack{drop.item, static_cast<i8>(std::min(drop.count, 64)), {}};
            item.born  = server_tick.load(std::memory_order_relaxed);
            dropped.push_back(std::move(item));
        }
        const std::string_view victim = mob_combat->type_name(*state);
        publish_items(dropped);

        OV_LOG_INFO("{} killed {} ({} stacks dropped)", attacker.name, victim, drops.size());

        // Flagged, never removed here: the entity world applies removals at the
        // end of its own tick, and taking one out from under the tick's loop is
        // how a list gets modified underneath itself.
        if (slimes) {  // ── mobs-2: a big slime splits, next tick ──
            slimes->on_death(*state, slime_random, slime_scratch);
            slime_births.insert(slime_births.end(), slime_scratch.begin(), slime_scratch.end());
        }
        state->removed = true;
        mob_combat->forget(state->network_id);
        return true;
    };

    /// Everything the combat session reaches outside itself for.
    ///
    /// Rebuilt per packet rather than stored: it captures the player by
    /// reference, and a `Player&` is only valid while `players_mutex` is held.
    ///
    /// Caller holds players_mutex.
    const auto combat_io = [&](Player& who) {
        CombatIo io;
        io.send = [&](i32 id, std::span<const u8> payload) {
            if (const auto framed = net::encode_packet(id, payload); framed && who.connection) {
                who.connection->send(*framed);
            }
        };
        io.broadcast = [&](i32 id, std::span<const u8> payload) {
            // nullptr, not the attacker's connection: a player has to see their
            // own arm swing, and the client does not draw it for itself.
            broadcast(nullptr, id, payload);
        };
        io.broadcast_others = [&](i32 id, std::span<const u8> payload) {  // ── breaking ──
            broadcast(who.connection.get(), id, payload);
        };
        io.hurt_entity = [&](i32 entity_id, f32 damage, bool /*critical*/) {
            // ── end ── the dragon's parts and the crystals are the fight's
            if (end_fight && who.dimension == DimensionId::End &&
                end_fight->hurt(entity_id, damage, end_fight_host, who.entity_id, who.mortal())) {
                return true;
            }
            return hurt_mob(who, entity_id, damage, held_weapon(who).looting);
        };
        // ── enchanting ── Smite, Bane and Impaling need to know what is hit.
        io.target_bonus = [&](i32 entity_id) -> f32 {
            if (!mobs || !registries) {
                return 0.0F;
            }
            const entity::EntityHandle handle = mobs->find(entity_id);
            const entity::EntityState* state =
                handle == entity::kNoEntity ? nullptr : mobs->state(handle);
            const auto types = registries->find("minecraft:entity_type");
            if (state == nullptr || !types) {
                return 0.0F;
            }
            return target_enchantment_bonus(who.inventory[36 + static_cast<usize>(who.held_slot)],
                                            registries->entry_of(*types, state->type));
        };
        io.entity_position = [&](i32 entity_id) -> std::optional<Vec3d> {
            if (!mobs) {
                return std::nullopt;
            }
            const entity::EntityHandle handle = mobs->find(entity_id);
            if (handle == entity::kNoEntity) {
                return std::nullopt;
            }
            const entity::EntityState* state = mobs->state(handle);
            return state == nullptr ? std::nullopt : std::optional<Vec3d>{state->position};
        };
        io.entity_on_ground = [&](i32 entity_id) {
            if (!mobs) {
                return true;
            }
            const entity::EntityHandle handle = mobs->find(entity_id);
            if (handle == entity::kNoEntity) {
                return true;
            }
            const entity::EntityState* state = mobs->state(handle);
            return state == nullptr || state->on_ground;
        };
        io.entity_knockback_resistance = [&](i32 entity_id) -> f32 {
            if (!mobs || !knockback_resistance_id) {
                return 0.0F;
            }
            const entity::EntityHandle handle = mobs->find(entity_id);
            if (handle == entity::kNoEntity) {
                return 0.0F;
            }
            const auto value = mobs->attribute(handle, *knockback_resistance_id);
            return value ? static_cast<f32>(*value) : 0.0F;
        };
        io.set_entity_velocity = [&](i32 entity_id, Vec3d velocity) {
            // A player's own id reaches here too — a sprinting hit slows the
            // attacker — and a player is not in the entity world, so for them
            // the packet is the whole of it.
            if (mobs) {
                if (const entity::EntityHandle handle = mobs->find(entity_id);
                    handle != entity::kNoEntity) {
                    if (entity::EntityState* state = mobs->mutable_state(handle)) {
                        state->velocity = velocity;
                    }
                }
            }
            broadcast(nullptr, net::clientbound::kEntityVelocity,
                      net::encode_entity_velocity(entity_id, velocity.x, velocity.y, velocity.z));
        };
        io.entities_near = [&](Vec3d centre, f64 radius, i32 exclude,
                               const std::function<void(i32)>& visit) {
            if (!mobs) {
                return;
            }
            for (const entity::EntityHandle handle : mobs->handles()) {
                const entity::EntityState* state = mobs->state(handle);
                if (state == nullptr || state->removed || state->network_id == exclude) {
                    continue;
                }
                // The attacker's box grown by `radius` on each horizontal axis
                // and by a quarter of a block vertically — vanilla's own sweep
                // box, which is why a sheep on the step above is not swept.
                if (std::abs(state->position.x - centre.x) <= radius + 1.0 &&
                    std::abs(state->position.z - centre.z) <= radius + 1.0 &&
                    std::abs(state->position.y - centre.y) <= 1.25) {
                    visit(state->network_id);
                }
            }
        };
        io.exhaust          = [&](f32 amount) { who.survival.exhaust(amount); };
        io.held_item        = [&] { return held_name(who); };
        io.held_damage      = [&] { return held_damage(who); };
        io.set_held_damage  = [&](i32 damage) { set_held_damage(who, damage); };
        io.break_held_item  = [&] { break_held_item(who); };
        io.consume_one_held = [&] { consume_one_held(who); };
        io.held_weapon      = [&] { return held_weapon(who); };
        io.set_on_fire      = [&](i32 entity_id, i32 seconds) {  // ── fire ── Fire Aspect
            if (fire_session) {
                fire_session->set_mob_on_fire(entity_id, seconds);
            }
        };
        return io;
    };
    // ── end combat and interaction ──────────────────────────────────────────

    // ── commands ────────────────────────────────────────────────────────────
    //
    // What the command engine reaches outside itself for. Every callback runs
    // on the tick thread with `players_mutex` held — `CommandService::run` is
    // only ever called that way — and takes `chunk_mutex` itself, in the
    // order every other path here takes the two.
    cmd::CommandHost command_host;
    /// A container placed by a command needs its block entity, as one placed
    /// by hand does. Caller holds chunk_mutex.
    const auto command_block_entity = [&](net::WirePosition at, registry::BlockStateId state) {
        if (!blocks || !registries || !block_entity_registry) {
            return;
        }
        const ContainerSpec* spec =
            container_spec_for_block(blocks->block_name(blocks->block_of(state)));
        if (spec == nullptr) {
            return;
        }
        chunk_at(at.x >> 4, at.z >> 4)
            .set_block_entity(
                new_container_entity(*spec, at.x, at.y, at.z, &*registries, block_entity_registry));
        dirty_chunks.insert(chunk_key(at.x >> 4, at.z >> 4));
    };
    command_host.for_each_player = [&](const std::function<void(cmd::PlayerRef&)>& visit) {
        for (auto& [key, who] : players) {
            if (!who.connection) {
                continue;
            }
            cmd::PlayerRef ref;
            ref.entity_id     = who.entity_id;
            ref.name          = who.name;
            ref.uuid          = who.uuid;
            ref.x             = &who.x;
            ref.y             = &who.y;
            ref.z             = &who.z;
            ref.yaw           = &who.yaw;
            ref.pitch         = &who.pitch;
            ref.game_mode     = &who.game_mode;
            ref.permission    = &who.permission;
            ref.inventory     = &who.inventory;
            ref.held_slot     = who.held_slot;
            ref.carried       = &who.carried;
            ref.survival      = &who.survival;
            ref.effects       = &who.effects;
            ref.effect_bearer = effect_bearer_for(who);
            ref.send          = [&who](i32 id, std::span<const u8> payload) {
                if (const auto framed = net::encode_packet(id, payload); framed && who.connection) {
                    who.connection->send(*framed);
                }
            };
            ref.broadcast_others = [&broadcast, &who](i32 id, std::span<const u8> payload) {
                broadcast(who.connection.get(), id, payload);
            };
            visit(ref);
        }
    };
    command_host.entities = [&](std::vector<cmd::EntityInfo>& out) {
        const auto types = registries ? registries->find("minecraft:entity_type") : std::nullopt;
        if (mobs && types) {
            for (const entity::EntityHandle handle : mobs->handles()) {
                const entity::EntityState* state = mobs->state(handle);
                if (state == nullptr || state->removed) {
                    continue;
                }
                cmd::EntityInfo info;
                info.id         = state->network_id;
                info.type       = std::string{registries->entry_of(*types, state->type)};
                info.uuid       = state->uuid;
                info.position   = state->position;
                info.yaw        = state->yaw;
                info.pitch      = state->pitch;
                info.width      = state->width;
                info.height     = state->height;
                info.eye_height = state->eye_height;
                out.push_back(std::move(info));
            }
        }
        for (const ItemEntity& item : ground_items) {
            cmd::EntityInfo info;
            info.id       = item.entity_id;
            info.type     = "minecraft:item";
            info.uuid     = item.uuid;
            info.position = Vec3d{item.x, item.y, item.z};
            info.width    = 0.25F;
            info.height   = 0.25F;
            if (registries && item_registry) {
                info.item = std::string{registries->entry_of(*item_registry, item.stack.item_id)};
            }
            out.push_back(std::move(info));
        }
        for (const GroundOrb& orb : ground_orbs) {
            cmd::EntityInfo info;
            info.id       = orb.entity_id;
            info.type     = "minecraft:experience_orb";
            info.uuid     = uuid_for_entity(orb.entity_id);
            info.position = Vec3d{orb.x, orb.y, orb.z};
            info.width    = 0.5F;
            info.height   = 0.5F;
            out.push_back(std::move(info));
        }
        // ── dragon ── the fight's entities: the dragon, crystals, fireballs,
        // clouds, the End's orbs.
        if (end_fight) {
            std::vector<EndFightEntity> fight_entities;
            end_fight->entities(fight_entities);
            for (const EndFightEntity& entity : fight_entities) {
                cmd::EntityInfo info;
                info.id       = entity.id;
                info.type     = std::string{entity.type};
                info.uuid     = entity.uuid;
                info.position = entity.position;
                info.width    = entity.width;
                info.height   = entity.height;
                out.push_back(std::move(info));
            }
        }
    };
    command_host.broadcast = [&](i32 id, std::span<const u8> payload) {
        broadcast(nullptr, id, payload);
    };
    command_host.teleport_player = [&](i32 id, Vec3d to, f32 yaw, f32 pitch, u8 relative) {
        for (auto& [key, who] : players) {
            if (who.entity_id != id || !who.connection) {
                continue;
            }
            // A relative field travels as the offset from where the server
            // believes the player is — vanilla's `tp ~ ~5 ~` is flags 0x1F and
            // a y of 5.
            const auto offset = [&](u8 flag, f64 target, f64 now) {
                return (relative & flag) != 0 ? target - now : target;
            };
            const f64 dx     = offset(cmd::teleport_flags::kX, to.x, who.x);
            const f64 dy     = offset(cmd::teleport_flags::kY, to.y, who.y);
            const f64 dz     = offset(cmd::teleport_flags::kZ, to.z, who.z);
            const f32 dyaw   = (relative & cmd::teleport_flags::kYaw) != 0 ? yaw - who.yaw : yaw;
            const f32 dpitch = (relative & cmd::teleport_flags::kPitch) != 0 ? pitch - who.pitch : pitch;
            who.x                = to.x;
            who.y                = to.y;
            who.z                = to.z;
            who.yaw              = yaw;
            who.pitch            = pitch;
            who.pending_teleport = who.pending_teleport + 1;
            if (const auto framed = net::encode_packet(
                    net::clientbound::kSynchronizePosition,
                    net::encode_synchronize_position_relative(dx, dy, dz, dyaw, dpitch, relative,
                                                              who.pending_teleport))) {
                who.connection->send(*framed);
            }
            saved_players[who.identity] = SavedPlayer{who.x, who.y, who.z, who.yaw, who.pitch};
            stream_chunks(who.connection, who);
            // ── nether ── /tp moves a player within the level they are in.
            broadcast_in(who.dimension, who.connection.get(), net::clientbound::kEntityTeleport,
                         net::encode_entity_teleport(who.entity_id, who.x, who.y, who.z, who.yaw,
                                                     who.pitch, false));
        }
    };
    command_host.teleport_entity = [&](i32 id, Vec3d to, f32 yaw, f32 pitch) -> bool {
        if (mobs) {
            if (const entity::EntityHandle handle = mobs->find(id); handle != entity::kNoEntity) {
                if (entity::EntityState* state = mobs->mutable_state(handle)) {
                    state->position           = to;
                    state->yaw                = yaw;
                    state->pitch              = pitch;
                    state->velocity           = Vec3d{0.0, 0.0, 0.0};
                    state->broadcast_position = to;
                    state->broadcast_valid    = true;
                    broadcast(nullptr, net::clientbound::kEntityTeleport,
                              net::encode_entity_teleport(id, to.x, to.y, to.z, yaw, pitch, false));
                    return true;
                }
            }
        }
        for (ItemEntity& item : ground_items) {
            if (item.entity_id == id) {
                item.x = to.x;
                item.y = to.y;
                item.z = to.z;
                broadcast(nullptr, net::clientbound::kEntityTeleport,
                          net::encode_entity_teleport(id, to.x, to.y, to.z, 0.0F, 0.0F, false));
                return true;
            }
        }
        return false;
    };
    command_host.kill_entity = [&](i32 id) -> bool {
        // ── dragon ── /kill on the fight's own: no animation, no orbs (measured)
        if (end_fight && end_fight->kill(id, end_fight_host)) {
            return true;
        }
        if (mobs && mob_combat) {
            if (const entity::EntityHandle handle = mobs->find(id); handle != entity::kNoEntity) {
                entity::EntityState* state = mobs->mutable_state(handle);
                if (state == nullptr || state->removed) {
                    return false;
                }
                // The death animation, then the loot a death without a player
                // behind it draws, then gone at the end of the entity tick.
                broadcast(nullptr, net::clientbound::kEntityEvent, net::encode_entity_event(id, 3));
                if (sounds) {  // ── sound ── /kill: the death cry and nothing before it
                    sounds->mob_death(sound_host, state->type, state->position);
                }
                std::vector<gameplay::Drop> drops;
                (void)mob_combat->loot(*state, false, 0, mob_loot_random, drops);
                std::vector<ItemEntity> dropped;
                for (const gameplay::Drop& drop : drops) {
                    ItemEntity item;
                    item.entity_id = next_entity_id.fetch_add(1);
                    item.uuid      = uuid_for_entity(item.entity_id);
                    item.x         = state->position.x;
                    item.y         = state->position.y + static_cast<f64>(state->height) * 0.5;
                    item.z         = state->position.z;
                    item.stack = net::ItemStack{drop.item, static_cast<i8>(std::min(drop.count, 64)), {}};
                    item.born  = server_tick.load(std::memory_order_relaxed);
                    dropped.push_back(std::move(item));
                }
                publish_items(dropped);
                if (slimes) {  // ── mobs-2: a big slime splits, next tick ──
                    slimes->on_death(*state, slime_random, slime_scratch);
                    slime_births.insert(slime_births.end(), slime_scratch.begin(),
                                        slime_scratch.end());
                }
                state->removed = true;
                mob_combat->forget(id);
                return true;
            }
        }
        for (usize i = 0; i < ground_items.size(); ++i) {
            if (ground_items[i].entity_id == id) {
                broadcast(nullptr, net::clientbound::kRemoveEntities, net::encode_remove_entity(id));
                ground_items.erase(ground_items.begin() + static_cast<isize>(i));
                return true;
            }
        }
        for (usize i = 0; i < ground_orbs.size(); ++i) {
            if (ground_orbs[i].entity_id == id) {
                broadcast(nullptr, net::clientbound::kRemoveEntities, net::encode_remove_entity(id));
                ground_orbs.erase(ground_orbs.begin() + static_cast<isize>(i));
                return true;
            }
        }
        return false;
    };
    command_host.summon = [&](std::string_view type, Vec3d at) -> std::optional<cmd::EntityInfo> {
        if (!mobs || !registries) {
            return std::nullopt;
        }
        const auto spawned = mobs->spawn(type, at, net::Uuid{});
        if (!spawned) {
            OV_LOG_DEBUG("summon {}: {}", type, entity::to_string(spawned.error()));
            return std::nullopt;
        }
        entity::EntityState* state = mobs->mutable_state(*spawned);
        state->uuid                = uuid_for_entity(state->network_id);
        state->broadcast_position  = state->position;
        state->broadcast_valid     = true;
        if (slimes && slimes->owns(state->type)) {  // ── mobs-2 ──
            slimes->on_spawn(*state, slime_random, 0.0F);
        }
        // The same behaviour a natural spawn gets: a brain for a species with
        // goals, the falling floor for one without — refused, not invented.
        if (rails_session && rails_session->owns(state->type)) {  // ── rails ── a cart
            rails_session->adopt(*mobs, *spawned);
        } else if (const gameplay::MobKind* kind = gameplay::mob_kind(type)) {
            const auto entity_types = registries->find("minecraft:entity_type");
            const auto player_type =
                entity_types ? registries->protocol_id(*entity_types, "minecraft:player") : std::nullopt;
            mobs->set_logic(*spawned, std::make_unique<gameplay::Mob>(
                                          *kind, state->width, state->height, state->network_id,
                                          player_type ? *player_type : gameplay::kNoQuarry));
        } else {
            mobs->set_logic(*spawned, std::make_unique<gameplay::FallingMob>());
        }
        mob_packets(*state, [&](i32 id, std::span<const u8> payload) { broadcast(nullptr, id, payload); });
        cmd::EntityInfo info;
        info.id       = state->network_id;
        info.type     = std::string{type};
        info.uuid     = state->uuid;
        info.position = state->position;
        info.width    = state->width;
        info.height   = state->height;
        return info;
    };
    // ── nether-2 ── A player standing in the Nether summons into the Nether.
    command_host.summon_by = [&](i32 source, std::string_view type,
                                 Vec3d at) -> std::optional<cmd::EntityInfo> {
        for (const auto& [key, who] : players) {
            if (nether_mobs && who.entity_id == source && who.dimension == DimensionId::Nether) {
                const auto id = nether_mobs->summon(type, at, nether_mob_host);
                if (!id) {
                    return std::nullopt;
                }
                cmd::EntityInfo info;
                info.id       = *id;
                info.type     = std::string{type};
                info.position = at;
                return info;
            }
        }
        return command_host.summon(type, at);
    };
    // ── end nether-2 ──
    command_host.drop_item = [&](i32 id, const net::ItemStack& stack) {
        for (auto& [key, who] : players) {
            if (who.entity_id != id) {
                continue;
            }
            ItemEntity item;
            item.entity_id    = next_entity_id.fetch_add(1);
            item.uuid         = uuid_for_entity(item.entity_id);
            item.x            = who.x;
            item.y            = who.y + 1.32;
            item.z            = who.z;
            item.stack        = stack;
            item.born         = server_tick.load(std::memory_order_relaxed);
            item.pickup_delay = 0;
            std::vector<ItemEntity> one;
            one.push_back(std::move(item));
            publish_items(one);
        }
    };
    command_host.is_loaded = [&](i32 cx, i32 cz) {
        // A superflat is generated on demand in microseconds, so every chunk
        // of one counts as loaded; a generated world's must be resident.
        const std::scoped_lock chunk_lock{chunk_mutex};
        return !chunk_source || chunk_if_resident(cx, cz) != nullptr;
    };
    command_host.block_at = [&](BlockPos at) {
        const std::scoped_lock chunk_lock{chunk_mutex};
        return block_at({at.x, at.y, at.z});
    };
    command_host.set_block = [&](BlockPos at, registry::BlockStateId state) {
        set_block_and_broadcast({at.x, at.y, at.z}, state);
        const std::scoped_lock chunk_lock{chunk_mutex};
        command_block_entity({at.x, at.y, at.z}, state);
    };
    command_host.destroy_block = [&](BlockPos at) {
        const net::WirePosition where{at.x, at.y, at.z};
        registry::BlockStateId  state{0};
        {
            const std::scoped_lock chunk_lock{chunk_mutex};
            state = block_at(where);
        }
        if (!blocks || blocks->is_air(blocks->block_of(state))) {
            return;
        }
        // Broken as by a player with an empty hand: the particles (World
        // Event 2001 with the state), the loot, then air.
        const Player            breaker{};
        std::vector<ItemEntity> dropped;
        drop_loot(breaker, where, dropped);
        broadcast(nullptr, net::clientbound::kWorldEvent,
                  net::encode_world_event(net::kWorldEventBlockBreak, where,
                                          static_cast<i32>(state.value()), false));
        set_block_and_broadcast(where, superflat.air.air);
        publish_items(dropped);
    };
    command_host.set_blocks = [&](std::span<const cmd::BlockChange> changes) {
        // One write each under one lock, one relight per chunk touched, one
        // Update Section Blocks per section — a 32768-block fill must not
        // relight a neighbourhood per block.
        std::map<std::tuple<i32, i32, i32>, std::vector<net::SectionBlock>> sections;
        {
            const std::scoped_lock chunk_lock{chunk_mutex};
            for (const cmd::BlockChange& change : changes) {
                const net::WirePosition at{change.pos.x, change.pos.y, change.pos.z};
                apply_block_change(at, change.state, false);
                command_block_entity(at, change.state);
                if (light) {  // ── light ── one batch, repaired on the tick
                    light->block_changed(BlockPos{at.x, at.y, at.z});
                }
                sections[{at.x >> 4, at.y >> 4, at.z >> 4}].push_back(
                    net::SectionBlock{static_cast<u8>(at.x & 15), static_cast<u8>(at.y & 15),
                                      static_cast<u8>(at.z & 15),
                                      static_cast<i32>(change.state.value())});
            }
        }
        for (const auto& [section, list] : sections) {
            broadcast(nullptr, net::clientbound::kUpdateSectionBlocks,
                      net::encode_update_section_blocks(std::get<0>(section), std::get<1>(section),
                                                        std::get<2>(section), list));
        }
        for (const cmd::BlockChange& change : changes) {
            notify_change({change.pos.x, change.pos.y, change.pos.z});
        }
    };
    command_host.break_blocks = [&](std::span<const BlockPos> positions) {
        std::vector<ItemEntity> dropped;
        const Player            breaker{};
        for (const BlockPos& at : positions) {
            const net::WirePosition where{at.x, at.y, at.z};
            registry::BlockStateId  state{0};
            {
                const std::scoped_lock chunk_lock{chunk_mutex};
                state = block_at(where);
            }
            drop_loot(breaker, where, dropped);
            broadcast(nullptr, net::clientbound::kWorldEvent,
                      net::encode_world_event(net::kWorldEventBlockBreak, where,
                                              static_cast<i32>(state.value()), false));
        }
        publish_items(dropped);
    };
    command_host.kick = [&](i32 id, std::string_view reason_json) {
        for (auto& [key, who] : players) {
            if (who.entity_id == id && who.connection) {
                if (const auto framed = net::encode_packet(net::clientbound::kDisconnect,
                                                           net::encode_component_packet(reason_json))) {
                    who.connection->send(*framed);
                }
                who.connection->close();
            }
        }
    };
    command_host.save = [&] { save_world(); };
    command_host.stop = [] { g_stop_requested.store(true, std::memory_order_relaxed); };
    command_host.set_world_spawn = [&](i32 x, i32 y, i32 z, f32 angle) {
        level_settings.spawn_x     = x;
        level_settings.spawn_y     = y;
        level_settings.spawn_z     = z;
        level_settings.spawn_angle = angle;
        broadcast(nullptr, net::clientbound::kSetDefaultSpawn,
                  net::encode_set_default_spawn(x, y, z, angle));
    };
    // ── end commands ────────────────────────────────────────────────────────
    // ── projectiles ─────────────────────────────────────────────────────────
    /// The player pressing or releasing the button, as the projectile module
    /// reads them. Caller holds players_mutex.
    const auto shooter_of = [&](Player& who) {
        Shooter shooter;
        shooter.entity_id = who.entity_id;
        shooter.feet      = Vec3d{who.x, who.y, who.z};
        shooter.yaw       = who.yaw;
        shooter.pitch     = who.pitch;
        shooter.creative  = who.game_mode == 1;  // ── commands: per player ──
        shooter.inventory = std::span<net::ItemStack>{who.inventory};
        shooter.held_slot = 36 + static_cast<usize>(who.held_slot);
        shooter.send_slot = [&](usize slot) { send_slot(who, slot); };
        shooter.send      = [&](i32 id, std::span<const u8> payload) {
            if (const auto framed = net::encode_packet(id, payload); framed && who.connection) {
                who.connection->send(*framed);
            }
        };
        shooter.held_broke = [&] {
            broadcast(nullptr, net::clientbound::kEntityEvent,
                      net::encode_entity_event(who.entity_id, 47));
        };
        return shooter;
    };
    // ── end projectiles ─────────────────────────────────────────────────────

    // Per-connection protocol state. A packet id means different things in
    // different states, so this cannot be global.
    std::unordered_map<const net::Connection*, ConnectionState> states;
    std::mutex                                                  states_mutex;

    listener->on_connect([&](const net::ConnectionPtr& connection) {
        const std::scoped_lock lock{states_mutex};
        states[connection.get()] = ConnectionState::Handshaking;
        OV_LOG_DEBUG("{} connected", connection->peer_address());
    });

    listener->on_disconnect([&](const net::ConnectionPtr& connection) {
        // ── player data: captured under the lock, written after it ──
        std::optional<std::tuple<net::Uuid, std::string, PlayerRecord>> leaving;
        {
            const std::scoped_lock lock{players_mutex};
            if (const auto it = players.find(connection.get()); it != players.end()) {
                const i32       entity_id = it->second.entity_id;
                const net::Uuid uuid      = it->second.uuid;
                // ── enchanting ── an open window's inputs go back to the
                // inventory before it is saved, as vanilla closes the menu.
                if (it->second.enchant) {
                    const auto quiet = [](i32, const std::vector<u8>&) {};
                    close_enchant_screen(enchant_context, enchant_host(it->second, quiet),
                                         *it->second.enchant, it->second.inventory);
                    it->second.enchant.reset();
                }
                // ── persistence ── the cart they sit in leaves with them
                if (rails_session && mobs) {
                    if (auto taken = rails_session->take_vehicle(*mobs, entity_id)) {
                        leaving_vehicles[entity_id] = std::move(taken->root_vehicle);
                        if (taken->cart_id >= 0) {
                            broadcast(connection.get(), net::clientbound::kRemoveEntities,
                                      net::encode_remove_entity(taken->cart_id));
                        }
                    }
                }
                leaving.emplace(uuid, it->second.name, player_record_of(it->second));  // ── player data ──
                leaving_vehicles.erase(entity_id);  // ── persistence ──
                players.erase(it);
                // ── projectiles: a draw does not outlive its archer ──
                if (projectiles) {
                    projectiles->cancel(entity_id);
                }

                // The reason goes with the player. Without this the chunks they
                // were standing on stay loaded for the life of the process,
                // which is what the ticket system exists to stop.
                {
                    const std::scoped_lock chunk_lock{chunk_mutex};
                    if (weather && level) {  // ── weather: their bed is free again ──
                        weather->forget(*level, entity_id);
                    }
                    chunks.remove_ticket(world::TicketType::Player,
                                         static_cast<u64>(entity_id));
                    world::LevelChanges changes;
                    chunks.refresh(changes);
                    // ── nether ── Whichever level they were in.
                    if (nether) {
                        nether->chunks().remove_ticket(world::TicketType::Player,
                                                       static_cast<u64>(entity_id));
                        nether->chunks().refresh(changes);
                    }
                    if (end_world) {  // ── end ──
                        end_world->chunks().remove_ticket(world::TicketType::Player,
                                                          static_cast<u64>(entity_id));
                        end_world->chunks().refresh(changes);
                    }
                }

                // After the erase, so the leaving player is not sent their own
                // removal on a socket that is already closing.
                // ── nether ── Everyone: an unknown id is harmless, a stale one not.
                broadcast_all(nullptr, net::clientbound::kRemoveEntities,
                              net::encode_remove_entity(entity_id));
                broadcast_all(nullptr, net::clientbound::kPlayerInfoRemove,
                              net::encode_player_info_remove(uuid));
            }
        }
        if (leaving) {  // ── player data ──
            save_player(std::get<0>(*leaving), std::get<1>(*leaving), std::get<2>(*leaving));
        }
        const std::scoped_lock lock{states_mutex};
        states.erase(connection.get());
        OV_LOG_DEBUG("{} disconnected", connection->peer_address());
    });

    listener->on_packet([&](const net::ConnectionPtr& connection, i32 packet_id,
                            std::span<const u8> body) -> bool {
        ConnectionState state{};
        {
            const std::scoped_lock lock{states_mutex};
            const auto             it = states.find(connection.get());
            if (it == states.end()) {
                return false;
            }
            state = it->second;
        }

        auto send_packet = [&](i32 id, std::span<const u8> payload) {
            const auto framed = net::encode_packet(id, payload);
            if (framed) {
                connection->send(*framed);
            }
        };

        switch (state) {
            case ConnectionState::Handshaking: {
                if (packet_id != 0x00) {
                    return false;
                }
                const auto handshake = net::parse_handshake(body);
                if (!handshake) {
                    return false;
                }
                OV_LOG_INFO("{} handshake: protocol {}, next {}", connection->peer_address(),
                            handshake->protocol_version,
                            handshake->next_state == net::NextState::Status ? "status" : "login");

                const std::scoped_lock lock{states_mutex};
                states[connection.get()] = handshake->next_state == net::NextState::Status
                                               ? ConnectionState::Status
                                               : ConnectionState::Login;
                return true;
            }

            case ConnectionState::Status: {
                if (packet_id == static_cast<i32>(net::StatusPacket::Request)) {
                    send_packet(static_cast<i32>(net::StatusPacket::Response),
                                net::encode_status_response(status));
                    return true;
                }
                if (packet_id == static_cast<i32>(net::StatusPacket::Ping)) {
                    // The client times the round trip from this and checks the
                    // payload came back unchanged.
                    io::ByteReader reader{body};
                    const auto     payload = reader.read_i64();
                    if (!payload) {
                        return false;
                    }
                    send_packet(static_cast<i32>(net::StatusPacket::Pong),
                                net::encode_pong(*payload));
                    connection->close();
                    return true;
                }
                return false;
            }

            case ConnectionState::Login: {
                if (packet_id != static_cast<i32>(net::LoginPacket::Start)) {
                    return false;
                }
                const auto login = net::parse_login_start(body);
                if (!login) {
                    return false;
                }

                if (!net::is_valid_player_name(login->name)) {
                    // Refused with a reason the client displays. Login is the
                    // one place a server can explain itself: anywhere later, a
                    // disconnect is just a disconnect.
                    send_packet(static_cast<i32>(net::LoginPacket::Disconnect),
                                net::encode_login_disconnect("Invalid player name"));
                    return true;
                }

                // Offline mode: the identity comes from the name, never from
                // the client. See docs/ARCHITECTURE.md § 8.
                const net::Uuid uuid = net::Uuid::offline_player(login->name);
                OV_LOG_INFO("{} logging in as {} ({})", connection->peer_address(), login->name,
                            uuid.to_string());

                if (!world_available) {
                    // Saying so beats leaving the client waiting for a join
                    // that never comes, which from the inside is
                    // indistinguishable from a hung server.
                    send_packet(static_cast<i32>(net::LoginPacket::Disconnect),
                                net::encode_login_disconnect(
                                    "Ondes VOXEL — no world data on this server.\n"
                                    "The operator needs to run the data generator."));
                    return true;
                }

                // ── player data: read before the door opens; refused, never
                // overwritten — a player let in would be saved over the file ──
                std::optional<LoadedPlayer> stored;
                if (auto loaded = player_data.load(uuid, login->name); !loaded) {
                    OV_LOG_ERROR("refusing {}: {}", login->name, loaded.error().message);
                    send_packet(static_cast<i32>(net::LoginPacket::Disconnect),
                                net::encode_login_disconnect(
                                    "Ondes VOXEL — your saved player data cannot be used by this "
                                    "server.\nIt has been left untouched; the server log names "
                                    "the file and the reason."));
                    return true;
                } else if (*loaded) {
                    stored = std::move(**loaded);
                    if (stored->unknown_items + stored->unknown_effects > 0) {
                        OV_LOG_WARN("{}: {} items and {} effects this server does not know, "
                                    "kept in the file as they are",
                                    login->name, stored->unknown_items, stored->unknown_effects);
                    }
                }
                // ── end player data ──

                // ── loading ──
                // A generated world is not open until its spawn area is. A
                // player let in earlier stands in a void the workers cannot
                // fill fast enough, and this path used to wait for it on the
                // network thread — twenty seconds in which no other player got
                // a keep-alive — then refuse *after* Login Success, where the
                // client is already in Play and a login packet id means
                // something else. Refused here instead, at once, with the key
                // vanilla's own loading screen uses: a vanilla client shows
                // "Preparing spawn area: N%", and ours waits and knocks again.
                if (chunk_source) {
                    const i32 ready = spawn_ready_percent.load(std::memory_order_relaxed);
                    if (ready < 100) {
                        OV_LOG_INFO("{} asked to join; spawn area {}% ready — refused for now",
                                    login->name, ready);
                        io::ByteWriter refusal;
                        net::write_string(
                            refusal,
                            fmt::format(R"({{"translate":"menu.preparingSpawn","with":["{}"]}})",
                                        ready));
                        send_packet(static_cast<i32>(net::LoginPacket::Disconnect), refusal.take());
                        return true;
                    }
                }
                // ── end loading ──

                send_packet(static_cast<i32>(net::LoginPacket::Success),
                            net::encode_login_success(uuid, login->name));

                Player player;
                player.entity_id = next_entity_id.fetch_add(1);
                player.identity  = uuid.to_string();
                bool remembered  = false;
                {
                    const std::scoped_lock lock{players_mutex};
                    if (const auto saved = saved_players.find(player.identity);
                        saved != saved_players.end()) {
                        player.x     = saved->second.x;
                        player.y     = saved->second.y;
                        player.z     = saved->second.z;
                        player.yaw   = saved->second.yaw;
                        player.pitch = saved->second.pitch;
                        remembered   = true;
                    }
                }
                if (stored) {  // ── player data: the file wins over the memory ──
                    PlayerPose pose{};
                    restore_player(stored->record, pose, player.inventory, player.held_slot,
                                   player.survival, player.effects);
                    player.x         = pose.x;
                    player.y         = pose.y;
                    player.z         = pose.z;
                    player.yaw       = pose.yaw;
                    player.pitch     = pose.pitch;
                    player.on_ground = pose.on_ground;
                    remembered       = true;
                }
                if (!remembered) {
                    // A first arrival stands on whatever the world's surface
                    // happens to be, at the spawn the world declares. The
                    // superflat's height is a constant, a real save's is not,
                    // and spawning at the flat world's y in a generated one
                    // buries the player in stone.
                    player.x = static_cast<f64>(level_settings.spawn_x) + 0.5;
                    player.z = static_cast<f64>(level_settings.spawn_z) + 0.5;

                    // **Wait** for the spawn chunk; never generate it here.
                    //
                    // This runs on the network thread, but it used to hold
                    // `chunk_mutex` across a `chunk_at` that could run the whole
                    // worldgen pipeline — and the tick thread waits behind that
                    // lock. Measured on this server, the first connection to a
                    // generated world logged `joined` and then "can't keep up"
                    // 43 ms later, once, every time: one tick lost to a chunk
                    // the tick loop was already asking for.
                    //
                    // It is already asking for it: the loop plants a `Forced`
                    // ticket on the spawn column at start-up, so the chunk is
                    // on its way before anyone connects. All that is needed is
                    // to let it arrive, with the lock **released** between
                    // looks so the thread that publishes it can get in.
                    constexpr auto kSpawnWait = std::chrono::seconds{20};
                    const auto     deadline   = std::chrono::steady_clock::now() + kSpawnWait;
                    const i32      home_x     = level_settings.spawn_x >> 4;
                    const i32      home_z     = level_settings.spawn_z >> 4;
                    const auto local_x = static_cast<usize>(level_settings.spawn_x & 15);
                    const auto local_z = static_cast<usize>(level_settings.spawn_z & 15);

                    // Waiting is for the **streaming** path only. Without a
                    // chunk source there is nothing on its way: a saved world
                    // reaches `chunk_at` and gets a disk read measured in
                    // microseconds, and a superflat gets a chunk built in about
                    // as long. Waiting for those would wait for ever, because
                    // nothing else ever publishes them — which is exactly what
                    // the first run of this code did, refusing every join to
                    // the lab world after twenty seconds.
                    bool have_home = false;
                    if (!chunk_source) {
                        const std::scoped_lock chunk_lock{chunk_mutex};
                        const world::Chunk&    home = chunk_at(home_x, home_z);
                        player.y                    = static_cast<f64>(
                            home.heightmap(world::HeightmapType::WorldSurface)
                                .first_free(local_x, local_z));
                        have_home = true;
                    }
                    while (!have_home && std::chrono::steady_clock::now() < deadline) {
                        {
                            const std::scoped_lock chunk_lock{chunk_mutex};
                            if (const world::Chunk* home = chunk_if_resident(home_x, home_z);
                                home != nullptr) {
                                player.y = static_cast<f64>(
                                    home->heightmap(world::HeightmapType::WorldSurface)
                                        .first_free(local_x, local_z));
                                have_home = true;
                            }
                        }
                        if (have_home) {
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds{5});
                    }

                    if (!have_home) {
                        // Twenty seconds and the spawn chunk is still not here.
                        // Refused with a reason rather than generated on this
                        // thread: a join that costs the tick loop a second is
                        // worse for everyone already playing than a join that
                        // does not happen and says why.
                        OV_LOG_WARN("spawn chunk {},{} did not arrive in {} s — refusing the join",
                                    home_x, home_z,
                                    std::chrono::duration_cast<std::chrono::seconds>(kSpawnWait)
                                        .count());
                        // ── loading: after Login Success the client is in Play,
                        // so this is the Play Disconnect — same JSON reason ──
                        send_packet(net::clientbound::kDisconnect,
                                    net::encode_login_disconnect(
                                        "Ondes VOXEL — the spawn chunk is still generating.\n"
                                        "Try again in a moment."));
                        return true;
                    }
                }
                {
                    const std::scoped_lock lock{states_mutex};
                    states[connection.get()] = ConnectionState::Play;
                }

                // Login (play) first, and nothing before it: until the client
                // has the registry codec it cannot make sense of a single
                // other packet.
                net::LoginPlay join;
                join.entity_id = player.entity_id;
                // Survival is what makes the break timings apply at all; creative
                // stays the default so flight works without food or damage.
                // ── commands: the world's default, per player from here on ──
                player.game_mode = commands ? commands->default_game_mode()
                                            : (options.survival ? u8{0} : u8{1});
                // ── player data: a returning player keeps their own mode, as
                // vanilla does without force-gamemode ──
                if (stored && stored->record.game_type >= 0 && stored->record.game_type <= 3) {
                    player.game_mode = static_cast<u8>(stored->record.game_type);
                }
                // ── nether ── A player saved in the Nether logs back in there —
                // or, with the Nether turned off, is refused rather than set
                // down at Nether coordinates in the overworld.
                if (stored &&
                    stored->record.dimension == dimension_info(DimensionId::Nether).name) {
                    bool ready = false;
                    {
                        const std::scoped_lock nether_lock{chunk_mutex};
                        ready = nether_ready();
                    }
                    if (!ready) {
                        OV_LOG_ERROR("refusing {}: saved in the Nether, which is unavailable",
                                     login->name);
                        send_packet(net::clientbound::kDisconnect,
                                    net::encode_play_disconnect(
                                        "{\"text\":\"Ondes VOXEL — you are in the Nether, and "
                                        "this server has it turned off.\"}"));
                        return true;
                    }
                    player.dimension = DimensionId::Nether;
                }
                // ── end ── The same for a player saved in the End.
                if (stored && stored->record.dimension == dimension_info(DimensionId::End).name) {
                    bool ready = false;
                    {
                        const std::scoped_lock end_lock{chunk_mutex};
                        ready = end_ready();
                    }
                    if (!ready) {
                        OV_LOG_ERROR("refusing {}: saved in the End, which is unavailable",
                                     login->name);
                        send_packet(net::clientbound::kDisconnect,
                                    net::encode_play_disconnect(
                                        "{\"text\":\"Ondes VOXEL — you are in the End, and this "
                                        "server has it turned off.\"}"));
                        return true;
                    }
                    player.dimension = DimensionId::End;
                }
                join.dimension_type = dimension_info(player.dimension).type;
                join.dimension_name = dimension_info(player.dimension).name;
                // ── enchanting ── the table's seed, and the player's own generator.
                if (stored) {
                    player.xp_seed = stored->record.xp_seed;
                }
                player.enchant_random.set_seed(enchant_random_seed(player.uuid));
                join.game_mode           = player.game_mode;
                join.registry_codec      = *codec_bytes;
                join.view_distance       = 10;
                join.simulation_distance = 10;
                send_packet(net::clientbound::kLoginPlay, net::encode_login_play(join));

                send_packet(net::clientbound::kPlayerAbilities,
                            cmd::CommandService::abilities_for(player.game_mode));  // ── commands ──

                player.pending_teleport = 1;
                send_packet(
                    net::clientbound::kSynchronizePosition,
                    net::encode_synchronize_position(player.x, player.y, player.z, player.yaw,
                                                     player.pitch, player.pending_teleport));

                {
                    // ── commands: the world's spawn, which /setworldspawn moves ──
                    const std::scoped_lock spawn_lock{players_mutex};
                    send_packet(net::clientbound::kSetDefaultSpawn,
                                net::encode_set_default_spawn(level_settings.spawn_x,
                                                              level_settings.spawn_y,
                                                              level_settings.spawn_z,
                                                              level_settings.spawn_angle));
                }

                // The centre has to arrive before the chunks: a client that
                // receives chunks with no centre keeps them and renders
                // nothing.
                stream_chunks(connection, player);

                // Reason 13: "start waiting for level chunks". This is what
                // takes the client off the loading screen.
                send_packet(net::clientbound::kGameEvent, net::encode_game_event(13, 0.0F));

                player.connection = connection;
                player.name       = login->name;
                player.uuid       = uuid;
                {
                    const std::scoped_lock lock{players_mutex};

                    // Everyone already here learns about the newcomer, and the
                    // newcomer learns about them. Both halves are needed: doing
                    // only the first leaves the new player alone in a world
                    // other people are walking around in.
                    // ── nether ── The tab list is everyone's; a body is seen
                    // only by the players of its own level.
                    broadcast_all(nullptr, net::clientbound::kPlayerInfoUpdate,
                                  net::encode_player_info_add(player.uuid, player.name,
                                                              player.game_mode));
                    broadcast_in(
                        player.dimension, nullptr, net::clientbound::kSpawnPlayer,
                        net::encode_spawn_player(player.entity_id, player.uuid, player.x, player.y,
                                                 player.z, player.yaw, player.pitch));

                    for (const auto& [key, other] : players) {
                        send_packet(net::clientbound::kPlayerInfoUpdate,
                                    net::encode_player_info_add(other.uuid, other.name, other.game_mode));
                        if (other.dimension != player.dimension) {  // ── nether ──
                            continue;
                        }
                        send_packet(
                            net::clientbound::kSpawnPlayer,
                            net::encode_spawn_player(other.entity_id, other.uuid, other.x, other.y,
                                                     other.z, other.yaw, other.pitch));
                    }

                    // The stacks already lying around. A joiner who is not told
                    // walks through invisible items and picks them up out of
                    // nowhere.
                    for (const ItemEntity& item : ground_items) {
                        if (item.dimension != player.dimension) {  // ── nether ──
                            continue;
                        }
                        send_packet(
                            net::clientbound::kSpawnEntity,
                            net::encode_spawn_entity(item.entity_id, item.uuid,
                                                     net::kItemEntityType, item.x, item.y, item.z));
                        send_packet(net::clientbound::kEntityMetadata,
                                    net::encode_item_metadata(item.entity_id, item.stack.item_id,
                                                              item.stack.count));
                    }

                    // And the mobs. Same reason: an entity a client was never
                    // told about is one it will happily walk through, and the
                    // first packet it gets about it — a movement delta — is
                    // dropped for naming an entity it does not have.
                    // ── nether ── The mobs live in the overworld.
                    if (mobs && player.dimension == DimensionId::Overworld) {
                        for (const entity::EntityHandle handle : mobs->handles()) {
                            if (const entity::EntityState* mob = mobs->state(handle)) {
                                mob_packets(*mob, send_packet);
                            }
                        }
                    }
                    // ── nether-2 ── and the Nether's, to a player who is there
                    if (nether_mobs && player.dimension == DimensionId::Nether) {
                        nether_mobs->send_all(send_packet);
                    }
                    if (weather) {  // ── weather: who is asleep ──
                        weather->join_packets(send_packet);
                    }

                    players[connection.get()] = player;
                }
                // ── persistence ── the cart they left in comes back with them
                if (stored && stored->record.root_vehicle && rails_session) {
                    rails_session->request_restore(player.entity_id, *stored->record.root_vehicle);
                }
                // ── crafting and smelting ───────────────────────────────────
                // Every recipe, once, after the join sequence — which is when
                // vanilla sends it. Without it the client plays fine and its
                // recipe book stays empty.
                send_recipe_book(workbench_context,
                                 [&](i32 id, std::vector<u8> payload) { send_packet(id, payload); });
                // ── player data: what they carry, and which slot is in hand ──
                if (stored) {
                    send_packet(net::clientbound::kContainerContent,
                                net::encode_container_content(
                                    0, 0,
                                    player_window_contents(registries ? &*registries : nullptr,
                                                           recipe_book ? &*recipe_book : nullptr,
                                                           player.inventory),
                                    net::ItemStack{}));
                    // Set Held Item, clientbound, 0x4D in protocol 763 — the
                    // packet vanilla sends at join (measure_player_data.py).
                    constexpr i32           kSetHeldItemClientbound = 0x4D;
                    const std::array<u8, 1> held{static_cast<u8>(player.held_slot)};
                    send_packet(kSetHeldItemClientbound, held);
                    // The restored effects, now, with the file's durations —
                    // on the record the tick owns, so it does not send them
                    // again one tick shorter.
                    const std::scoped_lock lock{players_mutex};
                    if (const auto it = players.find(connection.get()); it != players.end()) {
                        it->second.effects.announce(it->second.survival,
                                                    effect_io_for(it->second),
                                                    effect_bearer_for(it->second));
                    }
                }
                // ── end player data ──
                OV_LOG_INFO("{} joined at ({:.1f}, {:.1f}, {:.1f})", login->name, player.x,
                            player.y, player.z);
                return true;
            }

            case ConnectionState::Play: {
                // ── perf ── the packet, and the part of it spent waiting for the tick
                const ScopedLatency perf_packet{perf->network_packet};
                const auto          perf_wait = std::chrono::steady_clock::now();
                const std::scoped_lock lock{players_mutex};
                perf->network_lock_wait.record(std::chrono::duration_cast<std::chrono::microseconds>(
                                                   std::chrono::steady_clock::now() - perf_wait)
                                                   .count());
                // ── end perf ──
                const auto             it = players.find(connection.get());
                if (it == players.end()) {
                    return false;
                }
                Player& player = it->second;

                switch (packet_id) {
                    case net::serverbound::kConfirmTeleport: {
                        const auto id = net::parse_confirm_teleport(body);
                        if (id && *id == player.pending_teleport) {
                            player.confirmed = true;
                        }
                        return true;
                    }

                    case net::serverbound::kKeepAlive: {
                        const auto id = net::parse_keep_alive(body);
                        // A reply carrying the wrong id means the client is
                        // answering a keep-alive we did not send. Vanilla drops
                        // the connection; so do we, rather than trusting it.
                        if (!id || *id != player.keep_alive_id) {
                            // ── enchanting: its end-to-end probe was dropped here
                            // silently; the listener closes without a word ──
                            OV_LOG_WARN("{}: keep-alive reply {} does not match the one sent ({}), "
                                        "dropping the connection",
                                        player.name, id ? *id : -1, player.keep_alive_id);
                            return false;
                        }
                        player.awaiting_keep_alive = false;
                        return true;
                    }

                    case net::serverbound::kSetPlayerPosition:
                    case net::serverbound::kSetPlayerPositionRot:
                    case net::serverbound::kSetPlayerRotation:
                    case net::serverbound::kSetPlayerOnGround: {
                        const auto movement = net::parse_movement(packet_id, body);
                        if (!movement) {
                            return false;
                        }
                        // Believed as sent, for now. Server-authoritative
                        // movement needs collision, which needs the block flag
                        // table; accepting the client's word until then is a
                        // stated gap rather than a silent one.
                        const f64 before_x = player.x;
                        const f64 before_z = player.z;
                        if (movement->x) {
                            player.x = *movement->x;
                            player.y = *movement->y;
                            player.z = *movement->z;
                        }
                        if (movement->yaw) {
                            player.yaw   = *movement->yaw;
                            player.pitch = *movement->pitch;
                        }
                        // Standing or not divides the breaking speed by five,
                        // so this is not decoration.
                        player.on_ground = movement->on_ground;

                        // ── survival: the fall, and the sprint ─────────────
                        //
                        // Fed here rather than in the tick because a fall is
                        // accumulated per movement packet: two updates in one
                        // tick would otherwise count as one, and a nine-block
                        // fall came out at five points instead of six.
                        if (player.mortal() && movement->x) {  // ── commands: per player ──
                            const f64 dx = *movement->x - before_x;
                            const f64 dz = *movement->z - before_z;
                            player.survival.note_movement(player.y, player.on_ground,
                                                          std::sqrt(dx * dx + dz * dz));
                        }
                        // ── sound ── footsteps, for everyone near but the walker.
                        // Sneaking and spectating are silent: not measured, and
                        // said so in docs/provenance/son.md.
                        if (sounds && movement->x && player.on_ground && !player.sneaking &&
                            player.game_mode != 3) {
                            const f64 dx = player.x - before_x;
                            const f64 dz = player.z - before_z;
                            if (Sounds::advance(player.stride, std::sqrt(dx * dx + dz * dz))) {
                                registry::BlockStateId under{};
                                {
                                    const std::scoped_lock chunk_lock{chunk_mutex};
                                    under = block_at(net::WirePosition{
                                        static_cast<i32>(std::floor(player.x)),
                                        static_cast<i32>(std::floor(player.y - 0.2)),
                                        static_cast<i32>(std::floor(player.z))});
                                }
                                sounds->step(sound_host, connection.get(),
                                             Vec3d{player.x, player.y, player.z}, under);
                            }
                        }
                        // ── end sound ──
                        // ── end survival ───────────────────────────────────

                        if (motion_log != nullptr) {
                            // Tick, name, position, look, ground. One line per
                            // packet, in the order they arrive: the derivation
                            // needs the gaps as much as the values.
                            fmt::print(motion_log, "{} {} {:.6f} {:.6f} {:.6f} {:.2f} {:.2f} {}\n",
                                       server_tick.load(std::memory_order_relaxed), player.name,
                                       player.x, player.y, player.z, player.yaw, player.pitch,
                                       player.on_ground ? 1 : 0);
                            std::fflush(motion_log);
                        }
                        // Remembered on every update rather than on disconnect:
                        // a client that is killed never sends a clean close, and
                        // losing the last position of a crashed session is the
                        // case people actually notice.
                        saved_players[player.identity] =
                            SavedPlayer{player.x, player.y, player.z, player.yaw, player.pitch};

                        // Crossing a chunk boundary is what triggers streaming;
                        // stream_chunks returns immediately otherwise, so this
                        // is cheap to call twenty times a second.
                        if (movement->x) {
                            stream_chunks(connection, player);
                        }

                        // Only when something actually changed. A client sends
                        // an update every tick whether or not the player moved,
                        // and forwarding all of them is most of the traffic on
                        // a busy server.
                        const bool moved = !player.confirmed || !player.broadcast_valid ||
                                           std::abs(player.x - player.broadcast_x) > 0.01 ||
                                           std::abs(player.y - player.broadcast_y) > 0.01 ||
                                           std::abs(player.z - player.broadcast_z) > 0.01 ||
                                           std::abs(player.yaw - player.broadcast_yaw) > 0.5F;
                        if (moved) {
                            player.broadcast_x     = player.x;
                            player.broadcast_y     = player.y;
                            player.broadcast_z     = player.z;
                            player.broadcast_yaw   = player.yaw;
                            player.broadcast_valid = true;

                            // ── nether ── Seen by the players of the same level.
                            broadcast_in(player.dimension, connection.get(),
                                         net::clientbound::kEntityTeleport,
                                         net::encode_entity_teleport(
                                             player.entity_id, player.x, player.y, player.z,
                                             player.yaw, player.pitch, movement->on_ground));
                            // The head turns independently of the body; without
                            // this a player renders looking permanently ahead.
                            broadcast_in(
                                player.dimension, connection.get(),
                                net::clientbound::kEntityHeadRotation,
                                net::encode_entity_head_rotation(player.entity_id, player.yaw));
                        }
                        return true;
                    }

                    // ── survival: the respawn button ───────────────────────
                    //
                    // The only packet the survival system needs from the
                    // network thread. Everything else it does happens in the
                    // tick, which is where the world can be read safely.
                    case net::serverbound::kClientCommand: {
                        const auto command = net::parse_client_command(body);
                        if (command && *command == net::ClientCommand::PerformRespawn) {
                            player.wants_respawn = true;
                        }
                        return true;
                    }

                    // Sprinting is the only movement that costs hunger, and it
                    // cannot be told from a walk by watching positions.
                    // ── rails ── Player Input: a rider's controls, and 0x02 to
                    // get off. 0x1F: a probe riding a cart on the real server
                    // sent it with that flag and was set down.
                    case 0x1F: {
                        if (body.size() >= 9 && rails_session) {
                            const auto word = [&](usize at) {
                                u32 bits = 0;
                                for (usize i = 0; i < 4; ++i) {
                                    bits = (bits << 8U) | body[at + i];
                                }
                                return std::bit_cast<f32>(bits);
                            };
                            rails_session->request_input(
                                RiderInput{player.entity_id, word(0), word(4), body[8], player.yaw});
                        }
                        if (body.size() >= 9 && taming) {  // ── tame ── off a horse
                            taming->queue_input(player.entity_id, body[8]);
                        }
                        return true;
                    }
                    // ── end rails ──
                    // ── tame ── Move Vehicle: where a rider's client put its
                    // horse (x, y, z doubles, then yaw and pitch floats).
                    case 0x18: {
                        if (body.size() >= 32 && taming) {
                            const auto bits = [&](usize at, usize n) {
                                u64 value = 0;
                                for (usize i = 0; i < n; ++i) {
                                    value = (value << 8U) | body[at + i];
                                }
                                return value;
                            };
                            const auto real = [&](usize at) { return std::bit_cast<f64>(bits(at, 8)); };
                            const auto flt  = [&](usize at) {
                                return std::bit_cast<f32>(static_cast<u32>(bits(at, 4)));
                            };
                            taming->queue_vehicle_move(player.entity_id,
                                                       Vec3d{real(0), real(8), real(16)}, flt(24),
                                                       flt(28));
                        }
                        return true;
                    }

                    case net::serverbound::kPlayerCommand: {
                        if (const auto command = net::parse_player_command(body)) {
                            if (command->action == net::PlayerCommandAction::StartSprinting) {
                                player.survival.sprinting = true;
                            } else if (command->action ==
                                       net::PlayerCommandAction::StopSprinting) {
                                player.survival.sprinting = false;
                            }
                            // ── combat and interaction ──────────────────────
                            // Sneaking decides whether a right-click reaches
                            // the block or the item, and nothing else the
                            // client sends carries it.
                            else if (command->action ==
                                     net::PlayerCommandAction::StartSneaking) {
                                player.sneaking = true;
                            } else if (command->action ==
                                       net::PlayerCommandAction::StopSneaking) {
                                player.sneaking = false;
                            } else if (command->action == net::PlayerCommandAction::LeaveBed &&
                                       weather) {  // ── weather ──
                                weather->request_leave(player.entity_id);
                            }
                            // ── end combat and interaction ──────────────────
                        }
                        return true;
                    }
                    // ── end survival ───────────────────────────────────────

                    case net::serverbound::kSetHeldItem: {
                        const auto slot = net::parse_set_held_item(body);
                        if (slot && *slot >= 0 && *slot < 9) {
                            // ── combat and interaction ──────────────────────
                            // Changing hands abandons whatever was being eaten.
                            // A player who switches slots at tick 31 has eaten
                            // nothing, which is a rule rather than a timer.
                            if (*slot != player.held_slot) {
                                player.combat.on_release(combat_io(player));
                            }
                            // ── projectiles: so does a drawn bow ──
                            if (*slot != player.held_slot && projectiles) {
                                projectiles->cancel(player.entity_id);
                            }
                            // ── end combat and interaction ──────────────────
                            player.held_slot = *slot;
                        }
                        return true;
                    }

                    // ── combat and interaction ─────────────────────────────
                    //
                    // The three packets the combat session owns outright. Each
                    // one parses and delegates: everything about hitting,
                    // using and swinging lives in combat_session.{hpp,cpp},
                    // and this switch must not grow a fifth system inside it.
                    case net::serverbound::kInteract: {
                        const auto interact = net::parse_interact(body);
                        if (!interact) {
                            return false;
                        }
                        // The client sends its sneak flag with every
                        // interaction, and it is more current than the last
                        // Player Command.
                        player.sneaking = interact->sneaking;

                        // ── nether-2 ── a right-click on a Nether mob, for the tick
                        if (nether_mobs && NetherMobs::owns(interact->entity_id) &&
                            interact->kind == net::InteractKind::Interact) {
                            nether_mobs->queue_interact(player.entity_id, interact->entity_id);
                            return true;
                        }
                        // ── villagers: a right-click on a villager, for the tick ──
                        if (villagers && interact->kind == net::InteractKind::Interact) {
                            villagers->queue_interact(player.entity_id, interact->entity_id,
                                                      interact->hand.value_or(net::Hand::Main),
                                                      interact->sneaking);
                        }
                        // ── rails ── a cart ridden, fed or hit, for the tick. Every
                        // touch goes: the session ignores what is not a cart.
                        if (rails_session && interact->kind != net::InteractKind::InteractAt) {
                            CartTouch touch;
                            touch.player_id   = player.entity_id;
                            touch.cart_id     = interact->entity_id;
                            touch.attack      = interact->kind == net::InteractKind::Attack;
                            touch.creative    = player.game_mode == 1;
                            touch.sneaking    = interact->sneaking;
                            touch.player_feet = Vec3d{player.x, player.y, player.z};
                            const net::ItemStack& touch_hand =
                                player.inventory[36 + static_cast<usize>(player.held_slot)];
                            if (!touch_hand.empty() && registries && item_registry) {
                                touch.held = std::string{
                                    registries->entry_of(*item_registry, touch_hand.item_id)};
                            }
                            rails_session->request_touch(std::move(touch));
                        }
                        // ── mobs-3: a golden apple offered to a zombie villager, for the tick ──
                        if (zombie_villagers && interact->kind == net::InteractKind::Interact) {
                            zombie_villagers->queue_interact(player.entity_id, interact->entity_id);
                        }
                        // ── tame: a bone, a saddle, an empty hand on a horse ──
                        if (taming && interact->kind == net::InteractKind::Interact) {
                            taming->queue_interact(player.entity_id, interact->entity_id,
                                                   interact->hand.value_or(net::Hand::Main),
                                                   interact->sneaking);
                        }
                        // ── husbandry: a right-click on an entity, for the tick ──
                        if (husbandry && interact->kind == net::InteractKind::Interact) {
                            husbandry->queue_interact(player.entity_id, interact->entity_id,
                                                      interact->hand.value_or(net::Hand::Main));
                            return true;
                        }

                        const CombatOutcome out = player.combat.on_interact(
                            *interact, combat_view(player), combat_io(player));
                        if (sounds && out.hit) {  // ── sound ── the swing, heard by all
                            sounds->player_attack(sound_host, Vec3d{player.x, player.y, player.z},
                                                  out.critical, out.swept, out.sprint_knockback,
                                                  out.strength_scale);
                        }
                        if (!out.unsupported.empty()) {
                            OV_LOG_DEBUG("interact: {} is recognised and not carried out",
                                         out.unsupported);
                        }
                        if (out.hit) {
                            OV_LOG_DEBUG("{} hit entity {} for {:.2f}{}{}", player.name,
                                         interact->entity_id, out.damage,
                                         out.critical ? " (critical)" : "",
                                         out.swept ? " (swept)" : "");
                        }
                        return true;
                    }

                    case net::serverbound::kUseItem: {
                        const auto use = net::parse_use_item(body);
                        if (!use) {
                            return false;
                        }
                        // ── dragon ── a glass bottle in one of the dragon's clouds
                        // fills with its breath (the cloud loses half a block).
                        if (end_fight && player.dimension == DimensionId::End && registries &&
                            item_registry && use->hand == net::Hand::Main) {
                            const usize     slot = 36 + static_cast<usize>(player.held_slot);
                            net::ItemStack& held = player.inventory[slot];
                            if (!held.empty() &&
                                registries->entry_of(*item_registry, held.item_id) ==
                                    "minecraft:glass_bottle" &&
                                end_fight->take_breath(Vec3d{player.x, player.y, player.z})) {
                                if (player.mortal()) {
                                    held.count = static_cast<i8>(held.count - 1);
                                    if (held.count <= 0) {
                                        held = net::ItemStack{};
                                    }
                                    send_slot(player, slot);
                                }
                                (void)give_to_player(
                                    player, net::ItemStack{registries->protocol_id(*item_registry,
                                                                                   "minecraft:dragon_breath")
                                                               .value_or(0),
                                                           1,
                                                           {}});
                                acknowledge(connection, use->sequence);
                                return true;
                            }
                        }
                        // ── projectiles: a bow, a crossbow, a trident or a throw ──
                        if (projectiles && use->hand == net::Hand::Main &&
                            projectiles->on_use_item(shooter_of(player),
                                                     server_tick.load(std::memory_order_relaxed))) {
                            acknowledge(connection, use->sequence);
                            return true;
                        }
                        const CombatOutcome out = player.combat.on_use_item(
                            *use, combat_view(player), combat_io(player));
                        if (!out.unsupported.empty()) {
                            OV_LOG_DEBUG("use item: {} does nothing in the hand yet",
                                         out.unsupported);
                        }
                        return true;
                    }

                    case net::serverbound::kSwingArm: {
                        const auto hand = net::parse_swing_arm(body);
                        if (!hand) {
                            return false;
                        }
                        // Animation only. A server that took this for an attack
                        // would let a client hit fifty times a second.
                        player.combat.on_swing(*hand, combat_view(player), combat_io(player));
                        return true;
                    }
                    // ── end combat and interaction ─────────────────────────

                    case net::serverbound::kSetCreativeSlot: {
                        if (player.game_mode != 1) {  // ── commands: per player ──
                            // In survival the server owns the inventory. Vanilla
                            // ignores this packet outside creative, and honouring
                            // it would let any client hand itself anything.
                            return true;
                        }
                        const auto creative = net::parse_set_creative_slot(body);
                        if (!creative) {
                            return false;
                        }
                        if (creative->slot >= 0 &&
                            creative->slot < static_cast<i16>(player.inventory.size())) {
                            // ── enchanting ── the tag is kept, not dropped.
                            player.inventory[static_cast<usize>(creative->slot)] =
                                net::ItemStack{creative->item_id.value_or(0),
                                               creative->item_id ? creative->count : i8{0},
                                               creative->item_id ? creative->nbt
                                                                 : std::vector<u8>{}};
                        }
                        return true;
                    }

                    case net::serverbound::kPlayerAction: {
                        const auto action = net::parse_player_action(body);
                        if (!action) {
                            return false;
                        }
                        acknowledge(connection, action->sequence);

                        // ── combat and interaction ──────────────────────────
                        // Status 5 is "release use item": the button was let
                        // go. A player who lets go at tick 31 has eaten
                        // nothing, so this is a cancel and not a completion.
                        if (action->status == 5) {
                            // ── projectiles: the bow is let go ──
                            if (projectiles) {
                                (void)projectiles->on_release(
                                    shooter_of(player), server_tick.load(std::memory_order_relaxed));
                            }
                            player.combat.on_release(combat_io(player));
                            return true;
                        }
                        // ── end combat and interaction ──────────────────────

                        if (player.game_mode == 1) {  // ── commands: per player ──
                            // Creative breaks on the first packet: the block is
                            // already gone on the client when it arrives.
                            if (action->status == 0 || action->status == 2) {
                                // ── sound ── the others are told what broke
                                registry::BlockStateId broken{};
                                {
                                    const std::scoped_lock chunk_lock{chunk_mutex};
                                    broken = block_at_in(player.dimension, action->position);
                                }
                                set_block_connected_in(player.dimension, action->position, superflat.air.air);
                                if (sounds) {
                                    sounds->block_broken(sound_host, connection.get(),
                                                         BlockPos{action->position.x,
                                                                  action->position.y,
                                                                  action->position.z},
                                                         broken);
                                }
                            }
                            return true;
                        }

                        const i64 now_tick = server_tick.load(std::memory_order_relaxed);

                        if (action->status == 1) {  // cancelled
                            player.digging = player.delayed_dig = false;
                            return true;
                        }

                        if (action->status == 0) {  // started
                            const f32 progress = dig_progress_for(player, action->position);
                            if (progress >= 1.0F) {
                                // Fast enough to be instant, which the client
                                // has already assumed.
                                std::vector<ItemEntity> dropped;
                                drop_loot(player, action->position, dropped);
                                registry::BlockStateId broken{};  // ── sound ──
                                {
                                    const std::scoped_lock chunk_lock{chunk_mutex};
                                    broken = block_at_in(player.dimension, action->position);
                                }
                                set_block_connected_in(player.dimension, action->position, superflat.air.air);
                                if (sounds) {  // ── sound ──
                                    sounds->block_broken(sound_host, connection.get(),
                                                         BlockPos{action->position.x,
                                                                  action->position.y,
                                                                  action->position.z},
                                                         broken);
                                }
                                publish_items(dropped);
                                return true;
                            }
                            player.digging          = progress > 0.0F;
                            player.delayed_dig      = false;
                            player.dig_x            = action->position.x;
                            player.dig_y            = action->position.y;
                            player.dig_z            = action->position.z;
                            player.dig_started_tick = now_tick;
                            return true;
                        }

                        if (action->status == 2) {  // the client says it is done
                            if (!player.digging || action->position.x != player.dig_x ||
                                action->position.y != player.dig_y ||
                                action->position.z != player.dig_z) {
                                return true;
                            }
                            const f32  progress = dig_progress_for(player, action->position);
                            const auto elapsed =
                                static_cast<f32>(now_tick - player.dig_started_tick);
                            // Vanilla's own threshold. Below it the claim is not
                            // taken at its word: the server keeps the block and
                            // finishes it on its own clock, which is exactly the
                            // behaviour the break times were measured through.
                            if (progress * (elapsed + 1.0F) >= 0.7F) {
                                player.digging = player.delayed_dig = false;
                                std::vector<ItemEntity> dropped;
                                drop_loot(player, action->position, dropped);
                                registry::BlockStateId broken{};  // ── sound ──
                                {
                                    const std::scoped_lock chunk_lock{chunk_mutex};
                                    broken = block_at_in(player.dimension, action->position);
                                }
                                set_block_connected_in(player.dimension, action->position, superflat.air.air);
                                if (sounds) {  // ── sound ──
                                    sounds->block_broken(sound_host, connection.get(),
                                                         BlockPos{action->position.x,
                                                                  action->position.y,
                                                                  action->position.z},
                                                         broken);
                                }
                                publish_items(dropped);
                            } else if (progress > 0.0F) {
                                player.digging     = false;
                                player.delayed_dig = true;
                            }
                        }
                        return true;
                    }

                    case net::serverbound::kUseItemOn: {
                        const auto place = net::parse_use_item_on(body);
                        if (!place) {
                            return false;
                        }
                        acknowledge(connection, place->sequence);

                        // ── end ── An eye of ender on an End portal frame: in
                        // it goes, and a ring it completes opens the portal
                        // (end_portal.hpp). Any level: the rules read the one
                        // the player is in.
                        const net::ItemStack& eye_hand =
                            player.inventory[36 + static_cast<usize>(player.held_slot)];
                        if (end_rules && registries && item_registry && !eye_hand.empty() &&
                            registries->entry_of(*item_registry, eye_hand.item_id) ==
                                "minecraft:ender_eye") {
                            world::LevelWriter& here =
                                player.dimension == DimensionId::End      ? end_player_level
                                : player.dimension == DimensionId::Nether ? nether_player_level
                                                                          : player_level;
                            const auto eye = end_rules->use_eye(
                                here, BlockPos{place->position.x, place->position.y,
                                               place->position.z});
                            if (eye.result != gameplay::EyeUse::Pass) {
                                end_eye_used(player, *place, eye);
                                return true;
                            }
                        }

                        // ── dragon ── An End crystal on obsidian or bedrock with
                        // two free blocks above: the fight's (four round the
                        // exit portal respawn the dragon). Outside the End it is
                        // refused and named: this server's crystals live in the
                        // dragon fight.
                        if (end_fight && registries && item_registry && blocks && !eye_hand.empty() &&
                            registries->entry_of(*item_registry, eye_hand.item_id) ==
                                "minecraft:end_crystal") {
                            const BlockPos on{place->position.x, place->position.y,
                                              place->position.z};
                            world::LevelWriter& here =
                                player.dimension == DimensionId::End      ? end_player_level
                                : player.dimension == DimensionId::Nether ? nether_player_level
                                                                          : player_level;
                            const std::string_view base =
                                blocks->block_name(blocks->block_of(here.block_at(on)));
                            const bool clear =
                                here.block_at(BlockPos{on.x, on.y + 1, on.z}) == registry::kAirState &&
                                here.block_at(BlockPos{on.x, on.y + 2, on.z}) == registry::kAirState;
                            if ((base == "minecraft:obsidian" || base == "minecraft:bedrock") && clear) {
                                if (player.dimension != DimensionId::End) {
                                    OV_LOG_INFO("{} put an End crystal outside the End: refused, "
                                                "this server's crystals live in the dragon fight",
                                                player.name);
                                    return true;
                                }
                                end_fight->place_crystal(on, end_fight_host);
                                if (player.mortal()) {
                                    const usize slot = 36 + static_cast<usize>(player.held_slot);
                                    net::ItemStack& held = player.inventory[slot];
                                    held.count           = static_cast<i8>(held.count - 1);
                                    if (held.count <= 0) {
                                        held = net::ItemStack{};
                                    }
                                    send_slot(player, slot);
                                }
                                return true;
                            }
                        }
                        // ── nether ── A right-click in the Nether: the block's
                        // and the item's own rules — doors, buckets, flint and
                        // steel — then a plain placement. Containers, signs,
                        // crafting screens, planting and TNT stay overworld-only
                        // for now, named in docs/provenance/nether.md.
                        // ── end ── The End takes the same path.
                        if (player.dimension != DimensionId::Overworld) {
                            if (item_use) {
                                const CombatOutcome used = player.combat.on_use_item_on(
                                    *place, combat_view(player), combat_io(player),
                                    player.dimension == DimensionId::End ? end_player_level
                                                                         : nether_player_level,
                                    *item_use);
                                if (used.spawn_primed_tnt) {
                                    OV_LOG_WARN("TNT lit in the Nether is not primed: the entity "
                                                "engine lives in the overworld");
                                }
                                if (used.result != gameplay::UseResult::Pass) {
                                    return true;
                                }
                            }
                            const net::ItemStack& in_hand =
                                player.inventory[36 + static_cast<usize>(player.held_slot)];
                            const auto nether_block =
                                in_hand.empty() ? std::nullopt : block_for_item(in_hand.item_id);
                            if (!nether_block || !blocks) {
                                return true;
                            }
                            const auto target = net::offset_by_face(place->position, place->face);
                            const auto placed =
                                placed_state(*blocks, *nether_block, *place, player.yaw);
                            bool blocked = false;
                            if (!blocks->collision_boxes(placed).empty()) {
                                const AABB cell{Vec3d{static_cast<f64>(target.x),
                                                      static_cast<f64>(target.y),
                                                      static_cast<f64>(target.z)},
                                                Vec3d{static_cast<f64>(target.x) + 1.0,
                                                      static_cast<f64>(target.y) + 1.0,
                                                      static_cast<f64>(target.z) + 1.0}};
                                for (const auto& [occupant_key, occupant] : players) {
                                    blocked = blocked ||
                                              (occupant.dimension == player.dimension &&  // ── end ──
                                               cell.intersects(gameplay::player_box(
                                                   Vec3d{occupant.x, occupant.y, occupant.z})));
                                }
                            }
                            if (blocked) {
                                registry::BlockStateId there{0};
                                {
                                    const std::scoped_lock chunk_lock{chunk_mutex};
                                    there = block_at_in(player.dimension, target);  // ── end ──
                                }
                                send_packet(net::clientbound::kBlockUpdate,
                                            net::encode_block_update(
                                                target, static_cast<i32>(there.value())));
                                return true;
                            }
                            set_block_connected_in(player.dimension, target, placed);  // ── end ──
                            return true;
                        }
                        // ── end nether ──
                        // ── weather: a bed is slept in, not built against ──
                        if (weather &&
                            (!player.sneaking ||
                             player.inventory[36 + static_cast<usize>(player.held_slot)].empty())) {
                            bool clicked_bed = false;
                            {
                                const std::scoped_lock chunk_lock{chunk_mutex};
                                clicked_bed = weather->is_bed(block_at(place->position));
                            }
                            if (clicked_bed) {
                                weather->request_bed(player.entity_id,
                                                     BlockPos{place->position.x, place->position.y,
                                                              place->position.z});
                                return true;
                            }
                        }
                        // ── enchanting ── the table, the anvil and the
                        // grindstone open the same way, under the same rule.
                        if (!player.sneaking ||
                            player.inventory[36 + static_cast<usize>(player.held_slot)].empty()) {
                            const std::scoped_lock chunk_lock{chunk_mutex};
                            if (open_enchant_screen(enchant_context,
                                                    enchant_host(player, send_packet),
                                                    place->position.x, place->position.y,
                                                    place->position.z, 3, player.inventory,
                                                    player.enchant)) {
                                player.window_open = false;
                                return true;
                            }
                        }

                        // ── crafting and smelting ───────────────────────────
                        // A crafting table or a furnace opens instead of being
                        // built against, the same way a chest does below.
                        //
                        // Sneaking with something in hand builds instead, which
                        // is the same rule the containers below follow and the
                        // only way to put a hopper under a furnace.
                        if (!player.sneaking ||
                            player.inventory[36 + static_cast<usize>(player.held_slot)].empty()) {
                            const std::scoped_lock chunk_lock{chunk_mutex};
                            if (open_workbench(workbench_context,
                                               workbench_host(player, send_packet), place->position.x,
                                               place->position.y, place->position.z, 2,
                                               player.inventory, player.bench)) {
                                player.window_open = false;
                                return true;
                            }
                        }

                        // ── brewing ── a brewing stand opens its own screen
                        if (brewing &&
                            (!player.sneaking ||
                             player.inventory[36 + static_cast<usize>(player.held_slot)].empty())) {
                            const std::scoped_lock chunk_lock{chunk_mutex};
                            if (brewing->open(brewing_stand_host,
                                              BlockPos{place->position.x, place->position.y,
                                                       place->position.z},
                                              send_packet, player.inventory, player.brewing_window)) {
                                player.window_open = false;
                                return true;
                            }
                        }

                        // Clicking a container opens it rather than placing
                        // against it — **unless** the player is sneaking, which
                        // is vanilla's own rule and is what makes it possible to
                        // put a block on top of a chest at all.
                        //
                        // `sneaking` was tracked and unused here, and the cost
                        // showed the first time somebody tried to build a hopper
                        // under a chest on the test bench: every click opened the
                        // hopper and nothing could ever be placed on one.
                        //
                        // Every container the model knows, not only a chest: a
                        // barrel used to open as nothing at all and a hopper
                        // could not be opened, because the size and the menu
                        // were two literals here.
                        const net::ItemStack& in_hand =
                            player.inventory[36 + static_cast<usize>(player.held_slot)];
                        if (!player.sneaking || in_hand.empty()) {
                            const std::scoped_lock chunk_lock{chunk_mutex};
                            world::Chunk&          clicked_chunk =
                                chunk_at(place->position.x >> 4, place->position.z >> 4);
                            const world::BlockEntity* entity = clicked_chunk.block_entity_at(
                                static_cast<usize>(place->position.x & 15), place->position.y,
                                static_cast<usize>(place->position.z & 15));
                            const auto inventory = container_inventory(
                                entity, registries ? &*registries : nullptr, item_registry);
                            // A furnace is a container too, but its screen is
                            // the workbench's — it was opened above, and
                            // reaching it here would show its three slots with
                            // no fire bar and no progress arrow.
                            if (inventory && !inventory->spec().workbench) {
                                player.window_id   = 1;
                                player.window_open = true;
                                player.window_spec = &inventory->spec();
                                player.window_x    = place->position.x;
                                player.window_y    = place->position.y;
                                player.window_z    = place->position.z;
                                if (sounds) {  // ── sound ── the first viewer opens the lid
                                    bool watched = false;
                                    for (const auto& [viewer_key, viewer] : players) {
                                        watched = watched ||
                                                  (&viewer != &player && viewer.window_open &&
                                                   viewer.window_spec != nullptr &&
                                                   viewer.window_x == player.window_x &&
                                                   viewer.window_y == player.window_y &&
                                                   viewer.window_z == player.window_z);
                                    }
                                    if (!watched) {
                                        sounds->container(
                                            sound_host,
                                            BlockPos{place->position.x, place->position.y,
                                                     place->position.z},
                                            clicked_chunk.get_block(
                                                static_cast<usize>(place->position.x & 15),
                                                place->position.y,
                                                static_cast<usize>(place->position.z & 15)),
                                            true);
                                    }
                                }

                                std::vector<net::ItemStack> slots;
                                slots.reserve(static_cast<usize>(inventory->size()) + 36);
                                slots.insert(slots.end(), inventory->stacks().begin(),
                                             inventory->stacks().end());
                                // Then the player's own 27 main slots and 9
                                // hotbar slots, in that order: a window shows
                                // the container first and the player second.
                                for (usize i = 9; i < 45; ++i) {
                                    slots.push_back(player.inventory[i]);
                                }

                                send_packet(
                                    net::clientbound::kOpenScreen,
                                    net::encode_open_screen(player.window_id,
                                                            menu_id(inventory->spec().menu),
                                                            inventory->spec().title));
                                send_packet(
                                    net::clientbound::kContainerContent,
                                    net::encode_container_content(player.window_id, 1, slots, {}));
                                return true;
                            }
                        }

                        // ── combat and interaction ──────────────────────────
                        //
                        // The block gets first refusal, then the item — the
                        // game's own order, and `ItemUse::use_on` is the whole
                        // of it. **Before** the placement path below, because a
                        // door clicked with a block in hand has to open rather
                        // than be built against; getting this the other way
                        // round makes every container in the game impossible to
                        // open with something in hand.
                        //
                        // Anything but Pass stops here. Fail is not Pass: an
                        // iron door clicked bare-handed refuses, and letting
                        // that fall through would place a block through it.
                        if (item_use) {
                            registry::BlockStateId clicked_before{};  // ── sound ──
                            {
                                const std::scoped_lock chunk_lock{chunk_mutex};
                                clicked_before = block_at(place->position);
                            }
                            sound_click = BlockPos{place->position.x, place->position.y,
                                                   place->position.z};  // ── sound ──
                            const CombatOutcome used = player.combat.on_use_item_on(
                                *place, combat_view(player), combat_io(player), player_level,
                                *item_use);
                            sound_click.reset();  // ── sound ──
                            if (sounds) {  // ── sound ── a door, a lever, a button…
                                registry::BlockStateId clicked_after{};
                                {
                                    const std::scoped_lock chunk_lock{chunk_mutex};
                                    clicked_after = block_at(place->position);
                                }
                                sounds->block_changed(sound_host, connection.get(),
                                                      BlockPos{place->position.x,
                                                               place->position.y,
                                                               place->position.z},
                                                      clicked_before, clicked_after);
                            }
                            if (!used.unsupported.empty()) {
                                OV_LOG_DEBUG("use on block: {} is recognised and not carried out",
                                             used.unsupported);
                            }
                            if (used.screen != gameplay::ScreenKind::None) {
                                // Named rather than silently dropped. The
                                // screens this server does open — every
                                // container in the model, the crafting table,
                                // the three furnaces — are handled above and
                                // never reach here; what lands here is an
                                // anvil, a lectern, a brewing stand, and saying
                                // so is better than a click that does nothing.
                                //
                                // A container this project knows it does not
                                // model is named as such, so that "nothing
                                // happened" can be told from "not implemented".
                                const std::string_view clicked =
                                    blocks ? blocks->block_name(blocks->block_of(block_at(
                                                 net::WirePosition{used.screen_position.x,
                                                                   used.screen_position.y,
                                                                   used.screen_position.z})))
                                           : std::string_view{};
                                if (std::ranges::find(unmodelled_containers(), clicked) !=
                                    unmodelled_containers().end()) {
                                    OV_LOG_DEBUG("use on block: {} is a container this server "
                                                 "does not model",
                                                 clicked);
                                }
                                OV_LOG_DEBUG("use on block: a screen at ({}, {}, {}) is not "
                                             "opened by this server yet",
                                             used.screen_position.x, used.screen_position.y,
                                             used.screen_position.z);
                            }
                            // ── tnt and gravity: spawned by the next tick ──
                            if (used.spawn_primed_tnt && tnt_gravity) {
                                tnt_gravity->request_prime(used.tnt_position,
                                                           gameplay::kTntFuseTicks);
                            }
                            if (used.result != gameplay::UseResult::Pass) {
                                return true;
                            }
                        }
                        // ── end combat and interaction ──────────────────────

                        // ── spawn eggs ──────────────────────────────────────
                        // Used on a block, an egg asks for its creature on the
                        // face clicked. Asked for here, on the network thread,
                        // and spawned by the tick through the path /summon takes:
                        // the entity world has one writer. Consumed outside
                        // creative, as vanilla does. A species this server does
                        // not model yet is named in the log by the tick, never
                        // spawned as something else.
                        if (registries && item_registry) {
                            const net::ItemStack& hand =
                                player.inventory[36 + static_cast<usize>(player.held_slot)];
                            const std::string_view item =
                                hand.empty() ? std::string_view{}
                                             : registries->entry_of(*item_registry, hand.item_id);
                            constexpr std::string_view kEgg = "_spawn_egg";
                            if (item.size() > kEgg.size() && item.ends_with(kEgg)) {
                                const auto target =
                                    net::offset_by_face(place->position, place->face);
                                std::string type{item.substr(0, item.size() - kEgg.size())};
                                {
                                    const std::scoped_lock egg_lock{egg_mutex};
                                    egg_requests.emplace_back(
                                        std::move(type),
                                        Vec3d{static_cast<f64>(target.x) + 0.5,
                                              static_cast<f64>(target.y),
                                              static_cast<f64>(target.z) + 0.5});
                                }
                                if (player.game_mode != 1) {
                                    consume_one_held(player);
                                }
                                return true;
                            }
                        }
                        // ── end spawn eggs ──────────────────────────────────

                        // ── rails ── a cart item on a rail puts a cart on it;
                        // on anything else it does nothing, as vanilla's.
                        if (rails_session && registries && item_registry) {
                            const net::ItemStack& cart_hand =
                                player.inventory[36 + static_cast<usize>(player.held_slot)];
                            const std::string_view cart_item =
                                cart_hand.empty()
                                    ? std::string_view{}
                                    : registries->entry_of(*item_registry, cart_hand.item_id);
                            if (const auto cart_kind = gameplay::minecart_for_item(cart_item)) {
                                registry::BlockStateId under{0};
                                {
                                    const std::scoped_lock rail_lock{chunk_mutex};
                                    under = block_at(place->position);
                                }
                                if (rails_session->request_cart(
                                        *cart_kind,
                                        BlockPos{place->position.x, place->position.y,
                                                 place->position.z},
                                        under, player.yaw) &&
                                    player.game_mode != 1) {
                                    consume_one_held(player);
                                }
                                return true;
                            }
                        }
                        // ── end rails ──

                        // ── agriculture ─────────────────────────────────────
                        // ── fire ── raw food onto a campfire's grill: the
                        // campfire's own use, before anything is placed.
                        if (campfires) {
                            const net::ItemStack& grill_hand =
                                player.inventory[36 + static_cast<usize>(player.held_slot)];
                            if (!grill_hand.empty() && campfires->cook_time(grill_hand.item_id)) {
                                bool placed = false;
                                {
                                    const std::scoped_lock grill_lock{chunk_mutex};
                                    if (world::Chunk* grill = chunk_if_resident(
                                            place->position.x >> 4, place->position.z >> 4)) {
                                        placed = campfires->place_food(
                                            *grill,
                                            BlockPos{place->position.x, place->position.y,
                                                     place->position.z},
                                            grill_hand.item_id, campfire_host);
                                    }
                                }
                                if (placed) {
                                    if (player.game_mode != 1) {
                                        consume_one_held(player);
                                    }
                                    return true;
                                }
                            }
                        }
                        // Bone meal and planting. After the block's own
                        // interaction — a chest clicked with seeds opens — and
                        // before the generic placement, which would put a
                        // sapling on stone and seeds nowhere at all.
                        if (plants && plant_env_player && registries && item_registry) {
                            const net::ItemStack& hand =
                                player.inventory[36 + static_cast<usize>(player.held_slot)];
                            const std::string_view item =
                                hand.empty() ? std::string_view{}
                                             : registries->entry_of(*item_registry, hand.item_id);
                            const BlockPos clicked{place->position.x, place->position.y,
                                                   place->position.z};
                            if (item == "minecraft:bone_meal") {
                                // Seeded from the position and the tick: the
                                // same click on the same tick grows the same
                                // way, and no generator is shared between the
                                // network thread and the tick.
                                gameplay::PlantRandom random{static_cast<i64>(math::mix_stafford_13(
                                    (static_cast<u64>(static_cast<u32>(clicked.x)) << 32) ^
                                    (static_cast<u64>(static_cast<u32>(clicked.z)) << 8) ^
                                    static_cast<u64>(static_cast<u32>(clicked.y)) ^
                                    (static_cast<u64>(server_tick.load(std::memory_order_relaxed))
                                     * 0x9E3779B97F4A7C15ULL)))};
                                const gameplay::UseOutcome grown = plants->bone_meal(
                                    player_level, *plant_env_player, clicked, random);
                                if (!grown.unsupported.empty()) {
                                    OV_LOG_DEBUG("use on block: {}", grown.unsupported);
                                }
                                if (grown.result != gameplay::UseResult::Pass) {
                                    if (grown.consume_one && player.game_mode != 1) {
                                        consume_one_held(player);
                                    }
                                    return true;
                                }
                            } else if (plants->is_plantable(item)) {
                                if (plants->plant(player_level, clicked, place->face, item).planted &&
                                    player.game_mode != 1) {
                                    consume_one_held(player);
                                }
                                return true;
                            }
                        }
                        // ── end agriculture ─────────────────────────────────

                        const net::ItemStack& held =
                            player.inventory[36 + static_cast<usize>(player.held_slot)];
                        const auto held_block =
                            held.empty() ? std::nullopt : block_for_item(held.item_id);
                        if (!held_block || !blocks) {
                            // An empty hand or a non-block item. The
                            // acknowledgement above still matters: without it
                            // the client waits, then rolls back its guess.
                            return true;
                        }

                        const std::string_view held_name = blocks->block_name(*held_block);

                        // Clicking a slab of the same kind fills it out rather
                        // than placing a second one beside it. Measured on a
                        // real server: the result is one block of type=double,
                        // not two halves.
                        {
                            registry::BlockStateId clicked{0};
                            {
                                const std::scoped_lock chunk_lock{chunk_mutex};
                                world::Chunk&          at =
                                    chunk_at(place->position.x >> 4, place->position.z >> 4);
                                clicked = at.get_block(static_cast<usize>(place->position.x & 15),
                                                       place->position.y,
                                                       static_cast<usize>(place->position.z & 15));
                            }
                            const registry::BlockId clicked_block = blocks->block_of(clicked);
                            const auto type = blocks->find_property(clicked_block, "type");
                            if (clicked_block == *held_block && type &&
                                blocks->property_value(clicked, *type) != "double") {
                                const auto values = type->values;
                                const auto index  = static_cast<u16>(std::distance(
                                    values.begin(), std::ranges::find(values, "double")));
                                if (index < values.size()) {
                                    set_block_and_broadcast(
                                        place->position,
                                        blocks->with_property(clicked, *type, index));
                                    return true;
                                }
                            }
                        }

                        // The clicked block is not where the new one goes: the
                        // face says which side, and it lands one step along it.
                        const auto target = net::offset_by_face(place->position, place->face);
                        const auto placed = placed_state(*blocks, *held_block, *place, player.yaw);

                        // Refuse a block that would land inside somebody. The
                        // client predicted it, so it has to be told: without the
                        // block update below, the player keeps seeing a block
                        // that is not there.
                        {
                            WorldView              view;
                            const std::scoped_lock chunk_lock{chunk_mutex};
                            view.read = [&](i32 bx, i32 by, i32 bz) {
                                return block_at({bx, by, bz});
                            };
                            const gameplay::CollisionWorld collisions{*blocks, &WorldView::look_up,
                                                                      &view};

                            std::vector<AABB> shapes;
                            for (const registry::BlockRegistry::Box& box :
                                 blocks->collision_boxes(placed)) {
                                constexpr f64 kUnit = 1.0 / 32.0;
                                shapes.push_back(
                                    AABB{Vec3d{static_cast<f64>(target.x) +
                                                   static_cast<f64>(box.min_x) * kUnit,
                                               static_cast<f64>(target.y) +
                                                   static_cast<f64>(box.min_y) * kUnit,
                                               static_cast<f64>(target.z) +
                                                   static_cast<f64>(box.min_z) * kUnit},
                                         Vec3d{static_cast<f64>(target.x) +
                                                   static_cast<f64>(box.max_x) * kUnit,
                                               static_cast<f64>(target.y) +
                                                   static_cast<f64>(box.max_y) * kUnit,
                                               static_cast<f64>(target.z) +
                                                   static_cast<f64>(box.max_z) * kUnit}});
                            }

                            bool blocked = false;
                            for (const auto& [occupant_key, occupant] : players) {
                                const AABB hitbox =
                                    gameplay::player_box(Vec3d{occupant.x, occupant.y, occupant.z});
                                for (const AABB& shape : shapes) {
                                    blocked = blocked || shape.intersects(hitbox);
                                }
                            }
                            if (blocked) {
                                if (const auto framed = net::encode_packet(
                                        net::clientbound::kBlockUpdate,
                                        net::encode_block_update(
                                            target, static_cast<i32>(block_at(target).value())))) {
                                    if (player.connection) {
                                        player.connection->send(*framed);
                                    }
                                }
                                return true;
                            }
                        }
                        set_block_connected(target, placed);
                        // ── rails ── shaped among its neighbours on the next tick
                        if (rails_session &&
                            rails_session->rails().kind_of_block(*held_block) !=
                                gameplay::RailKind::None) {
                            const usize facing = facing_index(player.yaw);
                            rails_session->request_shape(BlockPos{target.x, target.y, target.z},
                                                         facing == 1 || facing == 3);
                        }
                        if (sounds) {  // ── sound ── heard by all but the placer
                            sounds->block_placed(sound_host, connection.get(),
                                                 BlockPos{target.x, target.y, target.z}, placed);
                        }

                        // Doors and beds take two blocks. Placing only the half
                        // the player clicked leaves a door that cannot open and
                        // a bed the client draws as a hole.
                        if (const auto half = blocks->find_property(*held_block, "half");
                            half &&
                            std::ranges::find(half->values, "upper") != half->values.end()) {
                            const auto upper = static_cast<u16>(std::distance(
                                half->values.begin(), std::ranges::find(half->values, "upper")));
                            set_block_and_broadcast({target.x, target.y + 1, target.z},
                                                    blocks->with_property(placed, *half, upper));
                        }
                        if (const auto part = blocks->find_property(*held_block, "part");
                            part && std::ranges::find(part->values, "head") != part->values.end()) {
                            const auto head = static_cast<u16>(std::distance(
                                part->values.begin(), std::ranges::find(part->values, "head")));
                            // The head lies one step along the way the player is
                            // looking, which is also the way the bed faces.
                            set_block_and_broadcast(step_towards(target, facing_index(player.yaw)),
                                                    blocks->with_property(placed, *part, head));
                        }

                        // A sign needs a block entity to hold its text, and the
                        // editor has to be opened or it can never be written on.
                        const std::string_view block_name = held_name;

                        // Every container block gets its block entity, not only
                        // a chest. A barrel placed without one is a block that
                        // opens nothing and says nothing, which is exactly the
                        // failure the lab's own plots were built with.
                        if (const ContainerSpec* spec = container_spec_for_block(block_name);
                            spec != nullptr && registries && block_entity_registry) {
                            const std::scoped_lock chunk_lock{chunk_mutex};
                            world::Chunk& target_chunk = chunk_at(target.x >> 4, target.z >> 4);
                            target_chunk.set_block_entity(
                                new_container_entity(*spec, target.x, target.y, target.z,
                                                     &*registries, block_entity_registry));
                            dirty_chunks.insert(chunk_key(target.x >> 4, target.z >> 4));
                        }

                        if (block_name.ends_with("_sign") && registries && block_entity_registry) {
                            const auto type_id =
                                registries->protocol_id(*block_entity_registry, "minecraft:sign");
                            const std::scoped_lock chunk_lock{chunk_mutex};
                            world::Chunk& target_chunk = chunk_at(target.x >> 4, target.z >> 4);

                            world::BlockEntity entity;
                            entity.x       = static_cast<u8>(target.x & 15);
                            entity.y       = target.y;
                            entity.z       = static_cast<u8>(target.z & 15);
                            entity.type    = "minecraft:sign";
                            entity.type_id = type_id.value_or(0);
                            entity.data    = new_sign_data();
                            target_chunk.set_block_entity(std::move(entity));
                            dirty_chunks.insert(chunk_key(target.x >> 4, target.z >> 4));

                            send_packet(net::clientbound::kOpenSignEditor,
                                        net::encode_open_sign_editor(target, true));
                        }
                        return true;
                    }

                    // ── enchanting ── the table's three buttons, the
                    // anvil's text box.
                    // ── villagers ── Select Trade, for the tick
                    case kSelectTrade: {
                        if (villagers) {
                            if (const auto index = parse_select_trade(body)) {
                                (void)villagers->queue_select(player.entity_id, *index);
                            }
                        }
                        return true;
                    }
                    case kClickContainerButton: {
                        const auto pressed = parse_click_button(body);
                        if (pressed && player.enchant &&
                            pressed->first == player.enchant->window_id) {
                            const std::scoped_lock chunk_lock{chunk_mutex};
                            enchant_button(enchant_context, enchant_host(player, send_packet),
                                           *player.enchant, pressed->second, player.inventory,
                                           player.carried);
                        }
                        return true;
                    }
                    case kRenameItem: {
                        const auto typed = parse_rename_item(body);
                        if (typed && player.enchant) {
                            const std::scoped_lock chunk_lock{chunk_mutex};
                            enchant_rename(enchant_context, enchant_host(player, send_packet),
                                           *player.enchant, *typed, player.inventory,
                                           player.carried);
                        }
                        return true;
                    }

                    case net::serverbound::kCloseContainer: {
                        // ── villagers ── a trading screen closes on the tick
                        if (villagers) {
                            if (const auto shut = net::parse_close_container(body);
                                shut && villagers->queue_close(player.entity_id, *shut)) {
                                return true;
                            }
                        }
                        // ── enchanting ── the inputs go back to the player
                        if (player.enchant) {
                            const std::scoped_lock chunk_lock{chunk_mutex};
                            close_enchant_screen(enchant_context,
                                                 enchant_host(player, send_packet),
                                                 *player.enchant, player.inventory);
                            player.enchant.reset();
                            send_packet(net::clientbound::kContainerContent,
                                        net::encode_container_content(0, 0, player.inventory,
                                                                      player.carried));
                        }
                        player.brewing_window.reset();  // ── brewing ──
                        // ── crafting and smelting ───────────────────────────
                        if (player.bench) {
                            const std::scoped_lock chunk_lock{chunk_mutex};
                            close_workbench(workbench_host(player, send_packet), *player.bench);
                            player.bench.reset();
                        }
                        if (sounds && player.window_open &&
                            player.window_spec != nullptr) {  // ── sound ── the last one shuts it
                            bool watched = false;
                            for (const auto& [viewer_key, viewer] : players) {
                                watched = watched ||
                                          (&viewer != &player && viewer.window_open &&
                                           viewer.window_spec != nullptr &&
                                           viewer.window_x == player.window_x &&
                                           viewer.window_y == player.window_y &&
                                           viewer.window_z == player.window_z);
                            }
                            if (!watched) {
                                registry::BlockStateId lid{};
                                {
                                    const std::scoped_lock chunk_lock{chunk_mutex};
                                    lid = block_at(net::WirePosition{
                                        player.window_x, player.window_y, player.window_z});
                                }
                                sounds->container(sound_host,
                                                  BlockPos{player.window_x, player.window_y,
                                                           player.window_z},
                                                  lid, false);
                            }
                        }
                        player.window_open = false;
                        player.window_spec = nullptr;
                        return true;
                    }

                    case net::serverbound::kClickContainer: {
                        // ── villagers ── a click on a trading screen, for the tick
                        if (villagers) {
                            if (const auto traded = net::parse_container_click(body);
                                traded && villagers->queue_click(player.entity_id, *traded)) {
                                return true;
                            }
                        }
                        // ── enchanting ──
                        if (player.enchant) {
                            const auto clicked = net::parse_container_click(body);
                            if (clicked && clicked->window_id == player.enchant->window_id) {
                                const std::scoped_lock chunk_lock{chunk_mutex};
                                enchant_click(enchant_context, enchant_host(player, send_packet),
                                              *player.enchant, *clicked, player.inventory,
                                              player.carried, player.enchant);
                                return true;
                            }
                        }
                        // ── crafting and smelting ───────────────────────────
                        if (player.bench) {
                            const auto crafted = net::parse_container_click(body);
                            if (crafted && crafted->window_id == player.bench->window_id) {
                                const std::scoped_lock chunk_lock{chunk_mutex};
                                handle_click(workbench_context,
                                             workbench_host(player, send_packet), *player.bench,
                                             *crafted, player.inventory, player.carried,
                                             player.bench);
                                return true;
                            }
                        }

                        const auto click = net::parse_container_click(body);
                        if (!click) {
                            return true;
                        }
                        // ── brewing ── the brewing stand's screen
                        if (brewing && player.brewing_window &&
                            click->window_id == player.brewing_window->window_id) {
                            const std::scoped_lock chunk_lock{chunk_mutex};
                            brewing->click(brewing_stand_host, *player.brewing_window, *click,
                                           player.inventory, player.carried, send_packet,
                                           [&](const net::ItemStack& stack) {
                                               workbench_host(player, send_packet).drop(stack);
                                           });
                            return true;
                        }

                        // ── the player's own screen ─────────────────────────
                        //
                        // Window 0 is the one the client opens by itself, when
                        // the player presses E. The server is never told, sends
                        // no `Open Screen`, and so has no `window_open` and no
                        // `window_id` to match — which is exactly why every
                        // click on it used to be dropped here, and why moving a
                        // stack inside one's own inventory did nothing at all.
                        if (click->window_id == 0) {
                            const auto outcome = apply_player_click(
                                registries ? &*registries : nullptr,
                                recipe_book ? &*recipe_book : nullptr, *click, player.inventory,
                                player.carried, player.drag);

                            for (const net::ItemStack& stack : outcome.dropped) {
                                if (stack.empty()) {
                                    continue;
                                }
                                ItemEntity item;
                                item.entity_id = next_entity_id.fetch_add(1);
                                item.uuid      = net::Uuid{
                                    0x4f564954454d0000ULL | static_cast<u64>(item.entity_id),
                                    static_cast<u64>(item.entity_id) * 0x9E3779B97F4A7C15ULL};
                                item.x     = player.x;
                                item.y     = player.y + 1.0;
                                item.z     = player.z;
                                item.stack = stack;
                                item.born  = server_tick.load(std::memory_order_relaxed);
                                // Longer than a broken block's: a player who
                                // throws something away must be able to step
                                // aside before it comes back.
                                item.pickup_delay = 40;
                                std::vector<ItemEntity> thrown_items;
                                thrown_items.push_back(std::move(item));
                                publish_items(thrown_items);
                            }

                            // Resent whether the click was handled or not. The
                            // client has already applied its own guess, and
                            // anything this server did differently would
                            // otherwise stay on screen as an item that does not
                            // exist.
                            send_packet(net::clientbound::kContainerContent,
                                        net::encode_container_content(
                                            0, click->state_id + 1,
                                            player_window_contents(
                                                registries ? &*registries : nullptr,
                                                recipe_book ? &*recipe_book : nullptr,
                                                player.inventory),
                                            player.carried));
                            return true;
                        }

                        if (!player.window_open || click->window_id != player.window_id) {
                            return true;
                        }

                        const std::scoped_lock chunk_lock{chunk_mutex};
                        world::Chunk&          window_chunk =
                            chunk_at(player.window_x >> 4, player.window_z >> 4);
                        world::BlockEntity* entity = window_chunk.block_entity_at(
                            static_cast<usize>(player.window_x & 15), player.window_y,
                            static_cast<usize>(player.window_z & 15));
                        auto container = container_inventory(
                            entity, registries ? &*registries : nullptr, item_registry);
                        // The container has to still be the one the screen was
                        // opened over. A chest broken while its screen is open
                        // otherwise takes the click, and where it lands depends
                        // on what replaced it.
                        if (!container || player.window_spec == nullptr ||
                            &container->spec() != player.window_spec) {
                            player.window_open = false;
                            player.window_spec = nullptr;
                            return true;
                        }

                        std::span<net::ItemStack> chest = container->stacks();

                        // Slot numbering inside the window: the container's own
                        // slots first, then the player's 27 main slots, then the
                        // 9 hotbar slots. The player's own slots are 9..44, so
                        // the mapping is not the identity and getting it wrong
                        // moves items between the wrong two places.
                        //
                        // `count` is the container's size and used to be the
                        // literal 27 in five places here, which is why a hopper
                        // and a dropper could not have a window at all.
                        const auto count = static_cast<i16>(chest.size());
                        const auto total = static_cast<i16>(count + 36);
                        const auto slot_ref = [&](i16 index) -> net::ItemStack* {
                            if (index >= 0 && index < count) {
                                return &chest[static_cast<usize>(index)];
                            }
                            if (index >= count && index < total) {
                                return &player.inventory[static_cast<usize>(index - count + 9)];
                            }
                            return nullptr;
                        };

                        bool handled = false;

                        // Number keys: the clicked slot and a hotbar slot trade
                        // places. The hotbar is 54..62 in the window and 36..44
                        // in the player, which is the same off-by-nine as
                        // everywhere else in this numbering.
                        if (click->mode == 2 && click->button >= 0 && click->button < 9) {
                            net::ItemStack* slot = slot_ref(click->slot);
                            net::ItemStack* hotbar =
                                &player.inventory[36 + static_cast<usize>(click->button)];
                            if (slot != nullptr && slot != hotbar) {
                                std::swap(*slot, *hotbar);
                            }
                            handled = true;
                        }

                        // Dropping. Vanilla sends this as a click on a slot with
                        // mode 4, or on nothing at all with slot -999 to throw
                        // what the cursor holds.
                        if (click->mode == 4) {
                            net::ItemStack* slot =
                                click->slot == -999 ? &player.carried : slot_ref(click->slot);
                            if (slot != nullptr && !slot->empty()) {
                                const i8 amount =
                                    click->button == 1 ? slot->count : static_cast<i8>(1);
                                net::ItemStack thrown = *slot;
                                thrown.count          = amount;
                                slot->count           = static_cast<i8>(slot->count - amount);
                                if (slot->count <= 0) {
                                    *slot = {};
                                }

                                ItemEntity item;
                                item.entity_id = next_entity_id.fetch_add(1);
                                item.uuid      = net::Uuid{
                                    0x4f564954454d0000ULL | static_cast<u64>(item.entity_id),
                                    static_cast<u64>(item.entity_id) * 0x9E3779B97F4A7C15ULL};
                                item.x     = player.x;
                                item.y     = player.y + 1.0;
                                item.z     = player.z;
                                item.stack = thrown;
                                item.born  = server_tick.load(std::memory_order_relaxed);
                                // Longer than a broken block's: a player who
                                // throws something away must be able to step
                                // aside before it comes back.
                                item.pickup_delay = 40;
                                std::vector<ItemEntity> thrown_items;
                                thrown_items.push_back(std::move(item));
                                publish_items(thrown_items);
                            }
                            handled = true;
                        }

                        // Dragging, which vanilla calls painting: one packet to
                        // start, one per slot crossed, one to end. Nothing moves
                        // until the end, so a drag that is never finished costs
                        // nothing.
                        if (click->mode == 5) {
                            const i8 phase = static_cast<i8>(click->button % 4);
                            if (phase == 0) {
                                player.drag_slots.clear();
                                player.drag_button = static_cast<i8>(click->button);
                            } else if (phase == 1) {
                                if (player.drag_button >= 0 &&
                                    std::ranges::find(player.drag_slots, click->slot) ==
                                        player.drag_slots.end()) {
                                    player.drag_slots.push_back(click->slot);
                                }
                            } else if (phase == 2 && player.drag_button >= 0) {
                                const bool one_each = player.drag_button >= 4;
                                const i8   limit =
                                    registries ? registries->max_stack_size(player.carried.item_id)
                                               : i8{64};
                                // Left drag splits evenly and keeps the
                                // remainder on the cursor; right drag puts one
                                // in each. Slots that already hold something
                                // else are skipped rather than overwritten.
                                std::vector<net::ItemStack*> targets;
                                for (const i16 index : player.drag_slots) {
                                    net::ItemStack* slot = slot_ref(index);
                                    if (slot == nullptr || player.carried.empty()) {
                                        continue;
                                    }
                                    if (slot->empty() || (slot->item_id == player.carried.item_id &&
                                                          slot->count < limit)) {
                                        targets.push_back(slot);
                                    }
                                }
                                if (!targets.empty() && !player.carried.empty()) {
                                    const i8 share =
                                        one_each ? i8{1}
                                                 : static_cast<i8>(player.carried.count /
                                                                   static_cast<i8>(targets.size()));
                                    for (net::ItemStack* slot : targets) {
                                        if (player.carried.count <= 0 || share <= 0) {
                                            break;
                                        }
                                        const i8 room = static_cast<i8>(
                                            limit - (slot->empty() ? 0 : slot->count));
                                        const i8 moved =
                                            std::min({share, room, player.carried.count});
                                        if (moved <= 0) {
                                            continue;
                                        }
                                        if (slot->empty()) {
                                            *slot       = player.carried;
                                            slot->count = moved;
                                        } else {
                                            slot->count = static_cast<i8>(slot->count + moved);
                                        }
                                        player.carried.count =
                                            static_cast<i8>(player.carried.count - moved);
                                    }
                                    if (player.carried.count <= 0) {
                                        player.carried = {};
                                    }
                                }
                                player.drag_slots.clear();
                                player.drag_button = -1;
                            }
                            handled = true;
                        }

                        if (click->mode == 1 && click->slot >= 0) {
                            // Shift-click: the stack moves to the other half of
                            // the window. Merging comes first and only then an
                            // empty slot, which is what vanilla does and what
                            // makes a shift-click feel like one move rather than
                            // scattering a stack across free slots.
                            net::ItemStack* from = slot_ref(click->slot);
                            if (from != nullptr && !from->empty()) {
                                const bool from_chest = click->slot < count;
                                const i16  first      = from_chest ? count : 0;
                                const i16  last       = from_chest ? total : count;
                                const i8   limit =
                                    registries ? registries->max_stack_size(from->item_id) : i8{64};

                                for (i16 index = first; index < last && !from->empty(); ++index) {
                                    net::ItemStack* into = slot_ref(index);
                                    if (into == nullptr || into->empty() ||
                                        into->item_id != from->item_id) {
                                        continue;
                                    }
                                    const i8 room = static_cast<i8>(limit - into->count);
                                    if (room <= 0) {
                                        continue;
                                    }
                                    const i8 moved = std::min(room, from->count);
                                    into->count    = static_cast<i8>(into->count + moved);
                                    from->count    = static_cast<i8>(from->count - moved);
                                    if (from->count <= 0) {
                                        *from = {};
                                    }
                                }
                                for (i16 index = first; index < last && !from->empty(); ++index) {
                                    net::ItemStack* into = slot_ref(index);
                                    if (into != nullptr && into->empty()) {
                                        // A stack that is already legal moves
                                        // whole; there is no splitting to do,
                                        // because it came from a slot that held
                                        // it.
                                        *into = *from;
                                        *from = {};
                                    }
                                }
                                handled = true;
                            }
                        }

                        if (click->mode == 0 && click->slot >= 0) {
                            net::ItemStack* slot = slot_ref(click->slot);
                            if (slot != nullptr) {
                                if (click->button == 0) {
                                    // Left click: onto a matching stack it
                                    // merges up to the item's limit, otherwise
                                    // the two trade places.
                                    const i8 limit =
                                        registries
                                            ? registries->max_stack_size(player.carried.item_id)
                                            : i8{64};
                                    if (!player.carried.empty() && !slot->empty() &&
                                        slot->item_id == player.carried.item_id &&
                                        slot->count < limit) {
                                        const i8 moved =
                                            std::min(static_cast<i8>(limit - slot->count),
                                                     player.carried.count);
                                        slot->count = static_cast<i8>(slot->count + moved);
                                        player.carried.count =
                                            static_cast<i8>(player.carried.count - moved);
                                        if (player.carried.count <= 0) {
                                            player.carried = {};
                                        }
                                    } else {
                                        std::swap(*slot, player.carried);
                                    }
                                    handled = true;
                                } else if (click->button == 1) {
                                    // Right click: put one down, or pick up
                                    // half. Rounding up on the half is what
                                    // vanilla does, and rounding down loses an
                                    // item on odd stacks.
                                    if (player.carried.empty()) {
                                        if (!slot->empty()) {
                                            const i8 half  = static_cast<i8>((slot->count + 1) / 2);
                                            player.carried = *slot;
                                            player.carried.count = half;
                                            slot->count = static_cast<i8>(slot->count - half);
                                            if (slot->count <= 0) {
                                                *slot = {};
                                            }
                                        }
                                    } else {
                                        const i8 limit =
                                            registries
                                                ? registries->max_stack_size(player.carried.item_id)
                                                : i8{64};
                                        const bool same = !slot->empty() &&
                                                          slot->item_id == player.carried.item_id;
                                        if (slot->empty() || (same && slot->count < limit)) {
                                            if (slot->empty()) {
                                                *slot       = player.carried;
                                                slot->count = 1;
                                            } else {
                                                slot->count = static_cast<i8>(slot->count + 1);
                                            }
                                            player.carried.count =
                                                static_cast<i8>(player.carried.count - 1);
                                            if (player.carried.count <= 0) {
                                                player.carried = {};
                                            }
                                        }
                                    }
                                    handled = true;
                                }
                            }
                        }

                        if (handled) {
                            container->store(entity->data);
                            dirty_chunks.insert(
                                chunk_key(player.window_x >> 4, player.window_z >> 4));
                        }

                        // Whether the click was handled or not, the window is
                        // resent in full. The client has already applied its own
                        // guess; anything the server did not implement — shift
                        // clicks, drags, number keys — would otherwise stay on
                        // screen as an item that does not exist.
                        std::vector<net::ItemStack> slots;
                        slots.reserve(static_cast<usize>(total));
                        slots.insert(slots.end(), chest.begin(), chest.end());
                        for (usize i = 9; i < 45; ++i) {
                            slots.push_back(player.inventory[i]);
                        }
                        send_packet(
                            net::clientbound::kContainerContent,
                            net::encode_container_content(player.window_id, click->state_id + 1,
                                                          slots, player.carried));
                        return true;
                    }

                    case net::serverbound::kUpdateSign: {
                        const auto update = net::parse_update_sign(body);
                        if (!update) {
                            return false;
                        }
                        const std::scoped_lock chunk_lock{chunk_mutex};
                        world::Chunk&          sign_chunk =
                            chunk_at(update->position.x >> 4, update->position.z >> 4);
                        world::BlockEntity* entity = sign_chunk.block_entity_at(
                            static_cast<usize>(update->position.x & 15), update->position.y,
                            static_cast<usize>(update->position.z & 15));
                        if (entity == nullptr || entity->data.compound() == nullptr) {
                            // The player wrote on something that is not a sign,
                            // or on one this server never created. Ignored
                            // rather than trusted into existence.
                            return true;
                        }

                        nbt::Tag side = empty_sign_text();
                        for (usize i = 0; i < update->lines.size(); ++i) {
                            (*side.compound()->front().value.list())[i] =
                                nbt::Tag{text_component(update->lines[i])};
                        }
                        const std::string_view field = update->front ? "front_text" : "back_text";
                        for (auto& field_entry : *entity->data.compound()) {
                            if (field_entry.name == field) {
                                field_entry.value = std::move(side);
                                break;
                            }
                        }

                        dirty_chunks.insert(
                            chunk_key(update->position.x >> 4, update->position.z >> 4));

                        // Everyone, including the writer: their client shows the
                        // text it typed, and confirming it is what keeps the two
                        // from drifting.
                        //
                        // No lock taken here. The Play handler already holds
                        // players_mutex for its whole body, and std::mutex is
                        // not recursive — taking it again deadlocks the
                        // connection, which looks exactly like the packet being
                        // ignored.
                        broadcast(nullptr, net::clientbound::kBlockEntityData,
                                  net::encode_block_entity_data(update->position, entity->type_id,
                                                                entity->data));
                        return true;
                    }

                    // ── commands and chat ──────────────────────────────────
                    //
                    // A command is queued, never run here: it changes the
                    // world, and the tick thread runs the queue. A chat line
                    // is relayed at once, unsigned, as an offline vanilla
                    // server relays it; a completion is answered at once from
                    // the tree, which nothing changes after start-up.
                    case net::serverbound::kChatCommand: {
                        const auto command = net::parse_chat_command(body);
                        if (!command) {
                            return false;
                        }
                        if (commands) {
                            commands->enqueue(player.entity_id, command->command, command->timestamp,
                                              command->salt);
                        }
                        return true;
                    }
                    case net::serverbound::kChatMessage: {
                        const auto chat = net::parse_chat_message(body);
                        if (!chat) {
                            return false;
                        }
                        broadcast(nullptr, net::clientbound::kPlayerChat,
                                  cmd::CommandService::chat_packet(player.name, player.uuid,
                                                                   chat->message, chat->timestamp,
                                                                   chat->salt, cmd::kChatTypeChat));
                        OV_LOG_INFO("[Not Secure] <{}> {}", player.name, chat->message);
                        return true;
                    }
                    case net::serverbound::kCommandSuggestionsRequest: {
                        const auto request = net::parse_suggestions_request(body);
                        if (!request) {
                            return false;
                        }
                        if (commands) {
                            cmd::CommandSource source;
                            source.kind       = cmd::CommandSource::Kind::Player;
                            source.entity_id  = player.entity_id;
                            source.name       = player.name;
                            source.uuid       = player.uuid;
                            source.position   = Vec3d{player.x, player.y, player.z};
                            source.yaw        = player.yaw;
                            source.pitch      = player.pitch;
                            source.permission = player.permission;
                            std::vector<std::string> names;
                            for (const auto& [other_key, other] : players) {
                                names.push_back(other.name);
                            }
                            send_packet(net::clientbound::kCommandSuggestions,
                                        net::encode_suggestions_response(commands->suggest(
                                            source, request->transaction, request->text, names)));
                        }
                        return true;
                    }
                    case net::serverbound::kMessageAcknowledgment:
                    case net::serverbound::kPlayerSession:
                        // Signatures are not checked in offline mode, and this
                        // server relays nothing signed: read and dropped.
                        return true;
                    // ── end commands and chat ──────────────────────────────

                    case net::serverbound::kClientInformation:
                    case net::serverbound::kPluginMessage:
                        // Read and ignored: neither changes anything the server
                        // does yet, and refusing them would drop the player.
                        return true;

                    default:
                        // Unknown play packets are ignored rather than fatal.
                        // A client sends a dozen the server does not implement,
                        // and disconnecting on the first one makes the world
                        // unenterable.
                        return true;
                }
            }
        }
        return false;
    });

    // The event loop runs on its own thread so the tick keeps its cadence
    // regardless of network activity — and so that the two never share state
    // implicitly.
    std::thread network_thread{[&] {
        set_thread_role("ov-net", ThreadRole::Network);
        listener->run();
    }};

    OV_LOG_INFO("listening on port {}", listener->port());

    TickClock clock;
    i64       behind_events = 0;

    // ── Tick-time histogram (instrumentation, not logic) ────────────────────
    //
    // A tick that runs past 50 ms is a lost tick, and it is the only number
    // that says whether work really left this thread: "can't keep up" only
    // fires once the clock has already slipped a whole tick, so a server
    // sitting at 45 ms looks identical to one sitting at 5 ms until it does
    // not. steady_clock is read here and nowhere else in the loop — the
    // measurement never feeds a decision, which is what keeps principle 5
    // intact.
    std::vector<i64> tick_micros;
    tick_micros.reserve(64 * 1024);

    // ── Natural spawning, on its own line ───────────────────────────────────
    //
    // The tick histogram says the tick got slower and cannot say what made it
    // so. This one says. It is the same discipline as the histogram above —
    // steady_clock, never feeding a decision — and it exists because the
    // spawner was named as the cost of the last wave from a *difference of two
    // whole-tick numbers*, which is an inference and not a measurement.
    std::vector<i64> spawn_micros;
    spawn_micros.reserve(64 * 1024);
    /// How many chunk lists were rebuilt, and what that cost. The rebuild is a
    /// spike every twentieth tick rather than a cost every tick, and a
    /// percentile over the sum of the two hides it.
    std::vector<i64> spawn_rebuild_micros;
    spawn_rebuild_micros.reserve(4 * 1024);

    // Reused across ticks rather than built inside one. Both grow to their
    // working size in the first few seconds and never allocate again.
    std::vector<GeneratedBlock> finished_blocks;
    std::vector<ChunkPos>       to_evict;
    std::vector<ChunkPos>       wanted_scratch;
    // ── Natural spawning ────────────────────────────────────────────────────
    //
    // The spawner itself has been complete and measured since M2 and nothing
    // called it: a world only ever held the mobs `--mobs=` put there by hand.
    // What it needed was a light source over our chunks and the three spans
    // `SpawnEnvironment` asks for, and both are here.
    //
    // The seed is fixed, not drawn from a clock: two runs of the same server
    // must produce the same world (principle 5), and a spawner seeded from the
    // wall clock makes every replay differ.
    gameplay::NaturalSpawner spawner{0x4f56'4d4f'4253'0001ULL};
    /// The mob names the spawner hands out are **views** into this. It has to
    /// outlive every request, which is why it is not a local of the loader.
    std::vector<std::string>          spawner_names;
    bool                              spawning_ready = false;
    /// The light the spawn rule reads, built **once**.
    ///
    /// Three `std::function`s over capturing lambdas: constructing one of these
    /// allocates, and constructing it inside the tick body is straight through
    /// the rule that forbids allocating there — the `NoAllocScope` guard exists
    /// to catch exactly this. It is also simply wasted work, three heap
    /// allocations twenty times a second for an object that never changes.
    ///
    /// The tick it reports is read through `server_tick` rather than captured,
    /// so the object can outlive any one tick without going stale.
    const ChunkLight spawn_light{[&] {
        LightHooks hooks;
        hooks.block_light = [&](BlockPos pos) -> u8 {
            const world::Chunk* chunk = chunk_if_resident(pos.x >> 4, pos.z >> 4);
            if (chunk == nullptr) {
                return 0;
            }
            const world::ChunkSection* section = chunk->section_for_y(pos.y);
            return section == nullptr ? u8{0}
                                      : section->block_light().get(world::section_index(
                                            static_cast<usize>(pos.x & 15),
                                            static_cast<usize>(pos.y & 15),
                                            static_cast<usize>(pos.z & 15)));
        };
        hooks.sky_light = [&](BlockPos pos) -> u8 {
            const world::Chunk* chunk = chunk_if_resident(pos.x >> 4, pos.z >> 4);
            if (chunk == nullptr) {
                return 0;
            }
            const world::ChunkSection* section = chunk->section_for_y(pos.y);
            return section == nullptr ? u8{0}
                                      : section->sky_light().get(world::section_index(
                                            static_cast<usize>(pos.x & 15),
                                            static_cast<usize>(pos.y & 15),
                                            static_cast<usize>(pos.z & 15)));
        };
        hooks.sky_darken = [&] {
            // ── commands: the sun the spawner sees is the clock /time sets ──
            // ── weather: a thunderstorm darkens the sky enough to spawn ──
            return commands ? WeatherSession::sky_darken(commands->world())
                            : sky_darken_for(server_tick.load(std::memory_order_relaxed));
        };
        return hooks;
    }()};

    std::vector<gameplay::SpawnRequest> spawn_requests;
    std::vector<Vec3d>                  spawn_players;
    std::vector<ChunkPos>               spawn_ticking;
    std::array<i32, 8>                  spawn_live{};

    /// Every furnace in a loaded chunk, rebuilt once a second. See the headless
    /// furnace pass for why it is an index and not a scan.
    std::vector<net::WirePosition> furnace_index;
    /// The screen a headless furnace is ticked through. One, reused: building a
    /// `Workbench` per furnace per tick would allocate inside the tick body.
    Workbench headless_furnace;
    u64                         chunks_published = 0;
    // ── loading ──
    i32        spawn_percent_seen = -1;
    const auto spawn_prep_started = std::chrono::steady_clock::now();

    // The spawn is a reason of its own, and it has to exist before anyone is
    // there to ask for it. Without this ticket the chunk the first player lands
    // in is generated at start-up and then thrown away by the first unload
    // pass — nothing wanted it, because nobody had joined yet — and the join
    // then waits for a whole generation block. `kTicking` gives it a radius of
    // two, so the five-by-five around spawn is resident and warm.
    {
        const std::scoped_lock lock{chunk_mutex};
        chunks.set_ticket(world::TicketType::Forced, 0,
                          ChunkPos{level_settings.spawn_x >> 4, level_settings.spawn_z >> 4},
                          world::LoadLevel::kTicking);
        world::LevelChanges spawn_changes;
        chunks.refresh(spawn_changes);
    }
    // The mob lists, read out of the data generator's own output rather than
    // invented. `spawning.hpp` refuses a made-up distribution in as many words,
    // and a spawner configured with an empty list is indistinguishable from one
    // that is not wired up — so a failure here turns spawning **off** and says
    // so, rather than running with nothing in it.
    // ── mobs-2: every biome's lists, drawn from the biome of each position ──
    std::vector<std::string> spawner_biome_names;
    const ChunkBiomes        spawn_biomes{[&](BlockPos pos) -> u16 {
        const world::Chunk* chunk = chunk_if_resident(pos.x >> 4, pos.z >> 4);
        return chunk == nullptr ? u16{0}
                                       : chunk->get_biome(static_cast<usize>(pos.x & 15), pos.y,
                                                   static_cast<usize>(pos.z & 15));
    }};
    const bool per_biome = load_all_biome_spawners(std::filesystem::path{OV_DATA_DIR} / "vanilla" /
                                                       "1.20.1" / "generated",
                                                   biome_names, spawner, spawner_biome_names) > 0;
    // ── end mobs-2 ──
    spawning_ready = per_biome ||
                     load_biome_spawners(std::filesystem::path{OV_DATA_DIR} / "vanilla" /
                                             "1.20.1" / "generated",
                                         "minecraft:plains", spawner, spawner_names);

    bool      mobs_placed   = false;
    auto      last_autosave = std::chrono::steady_clock::now();

    // ── tnt and gravity ─────────────────────────────────────────────────────
    // What an explosion reaches outside the module for. Built once, before the
    // loop: five std::functions built per tick would allocate in the tick.
    // Every callback runs inside the entity pass, which holds players_mutex
    // and chunk_mutex.
    const Deliver tnt_deliver = [&](i32 id, std::span<const u8> payload) {
        broadcast(nullptr, id, payload);
    };
    TntGravityHost tnt_host;
    tnt_host.broadcast   = tnt_deliver;
    tnt_host.each_player = [&](const std::function<void(BlastPlayer&)>& visit) {
        for (auto& [blast_key, who] : players) {
            if (!who.connection || !who.confirmed) {
                continue;
            }
            BlastPlayer view;
            view.feet     = Vec3d{who.x, who.y, who.z};
            view.creative = !who.mortal();  // ── commands: per player ──
            view.send     = [&who](i32 id, std::span<const u8> payload) {
                if (const auto framed = net::encode_packet(id, payload); framed && who.connection) {
                    who.connection->send(*framed);
                }
            };
            if (who.mortal()) {  // ── commands: per player ──
                view.hurt = [&](f32 amount) {
                    const SurvivalIo io{
                        .send =
                            [&](i32 id, std::span<const u8> payload) {
                                if (const auto framed = net::encode_packet(id, payload);
                                    framed && who.connection) {
                                    who.connection->send(*framed);
                                }
                            },
                        .broadcast = [&](i32 id, std::span<const u8> payload) {
                            broadcast(who.connection.get(), id, payload);
                        }};
                    // ── mobs-3 ── `explosion` scales with difficulty: `always`
                    (void)who.survival.hurt(
                        gameplay::DamageKind::Explosion,
                        gameplay::scale_for_difficulty(
                            amount, commands ? static_cast<gameplay::Difficulty>(
                                                   commands->world().difficulty)
                                             : gameplay::Difficulty::Normal),
                        io, who.entity_id);
                };
            }
            visit(view);
        }
    };
    tnt_host.drop_item = [&](Vec3d at, const net::ItemStack& stack) {
        ItemEntity item;
        item.entity_id = next_entity_id.fetch_add(1);
        item.uuid      = uuid_for_entity(item.entity_id);
        item.x         = at.x;
        item.y         = at.y;
        item.z         = at.z;
        item.stack     = stack;
        item.born      = server_tick.load(std::memory_order_relaxed);
        std::vector<ItemEntity> one;
        one.push_back(std::move(item));
        publish_items(one);
    };
    tnt_host.sweep_items = [&](const std::function<bool(Vec3d)>& destroyed) {
        for (usize i = ground_items.size(); i-- > 0;) {
            const ItemEntity& item = ground_items[i];
            if (!destroyed(Vec3d{item.x, item.y, item.z})) {
                continue;
            }
            broadcast(nullptr, net::clientbound::kRemoveEntities,
                      net::encode_remove_entity(item.entity_id));
            ground_items.erase(ground_items.begin() + static_cast<isize>(i));
        }
    };
    tnt_host.creeper_targets = [&](std::vector<Vec3d>& out) {
        // A creative player is never a creeper's target.
        for (const auto& [target_key, who] : players) {
            if (who.mortal() && who.connection && who.confirmed &&  // ── commands: per player ──
                !who.survival.awaiting_respawn) {
                out.push_back(Vec3d{who.x, who.y, who.z});
            }
        }
    };
    if (tnt_gravity && sounds) {  // ── sound ── the fuse, heard by everyone near
        tnt_gravity->on_primed([&](Vec3d at) { sounds->tnt_primed(sound_host, at); });
    }
    // ── end tnt and gravity ─────────────────────────────────────────────────

    // ── commands: the dedicated server's console ────────────────────────────
    // Lines typed on stdin run at level 4. Only when nobody else owns the
    // process — an integrated server's window has no console.
    std::optional<cmd::ConsoleReader> console;
    if (external_stop == nullptr && commands) {
        console.emplace([&commands](std::string line) { commands->enqueue_console(std::move(line)); });
    }
    // ── end commands ────────────────────────────────────────────────────────
    // ── projectiles ─────────────────────────────────────────────────────────
    // Built once, before the loop, like the TNT's. Every callback runs inside
    // the entity pass, which holds players_mutex and chunk_mutex.
    ProjectileHost projectile_host;
    const auto     projectile_player = [&](i32 id) -> Player* {
        for (auto& [key, who] : players) {
            if (who.entity_id == id && who.connection && who.confirmed) {
                return &who;
            }
        }
        return nullptr;
    };
    projectile_host.players = [&](std::vector<ProjectilePlayer>& out) {
        for (const auto& [key, who] : players) {
            if (who.connection && who.confirmed) {
                // ── commands: per player — creative and spectator are not hurt ──
                out.push_back(ProjectilePlayer{who.entity_id, Vec3d{who.x, who.y, who.z},
                                               !who.mortal(),
                                               !who.survival.awaiting_respawn});
            }
        }
    };
    projectile_host.hurt_player = [&](i32 id, f32 amount, gameplay::DamageKind kind) {
        Player* who = projectile_player(id);
        // A creative player is not hurt, and the arrow bounces off it.
        if (who == nullptr || !who->mortal()) {  // ── commands: per player ──
            return false;
        }
        const SurvivalIo io{
            .send =
                [&](i32 packet, std::span<const u8> payload) {
                    if (const auto framed = net::encode_packet(packet, payload);
                        framed && who->connection) {
                        who->connection->send(*framed);
                    }
                },
            .broadcast = [&](i32 packet, std::span<const u8> payload) {
                broadcast(who->connection.get(), packet, payload);
            }};
        return who->survival.hurt(kind, amount, io, who->entity_id).applied;
    };
    projectile_host.give = [&](i32 id, const net::ItemStack& stack) -> i8 {
        Player* who = projectile_player(id);
        return who == nullptr ? i8{0} : give_to_player(*who, stack);
    };
    projectile_host.teleport = [&](i32 id, Vec3d to) {
        Player* who = projectile_player(id);
        if (who == nullptr) {
            return;
        }
        who->x                = to.x;
        who->y                = to.y;
        who->z                = to.z;
        who->pending_teleport = who->entity_id * 1000 + 11;
        if (const auto framed = net::encode_packet(
                net::clientbound::kSynchronizePosition,
                net::encode_synchronize_position(who->x, who->y, who->z, who->yaw, who->pitch,
                                                 who->pending_teleport));
            framed && who->connection) {
            who->connection->send(*framed);
        }
    };
    // ── rails ── what the carts reach outside themselves for
    RailsHost rails_host;
    rails_host.broadcast = [&](i32 id, std::span<const u8> payload) {
        broadcast(nullptr, id, payload);
    };
    rails_host.drop_item = [&](Vec3d at, std::string_view name, i32 count) {
        const auto id = registries && item_registry
                            ? registries->protocol_id(*item_registry, name)
                            : std::nullopt;
        if (!id) {
            OV_LOG_WARN("rails: {} is not an item this server knows", name);
            return;
        }
        net::ItemStack stack;
        stack.item_id = *id;
        stack.count   = static_cast<i8>(std::clamp(count, 1, 64));
        tnt_host.drop_item(at, stack);
    };
    rails_host.player_ready = [&](i32 id) {  // ── persistence ── RootVehicle
        const Player* who = projectile_player(id);
        return who != nullptr && who->confirmed;
    };
    rails_host.carry_rider = [&](i32 id, Vec3d seat) {
        Player* who = projectile_player(id);
        if (who == nullptr) {
            return false;
        }
        who->x = seat.x;
        who->y = seat.y;
        who->z = seat.z;
        return true;
    };
    rails_host.set_down     = [&](i32 id, Vec3d at) { projectile_host.teleport(id, at); };
    rails_host.consume_held = [&](i32 id) {
        if (Player* who = projectile_player(id)) {
            consume_one_held(*who);
        }
    };
    // ── end rails ──
    projectile_host.spawn_mob = [&](std::string_view type, Vec3d at) {
        if (!mobs || !registries) {
            return;
        }
        const auto spawned = mobs->spawn(type, at, net::Uuid{});
        if (!spawned) {
            return;
        }
        entity::EntityState* state = mobs->mutable_state(*spawned);
        state->uuid                = uuid_for_entity(state->network_id);
        state->broadcast_position  = state->position;
        state->broadcast_valid     = true;
        if (const gameplay::MobKind* kind = gameplay::mob_kind(type)) {
            const auto entity_types = registries->find("minecraft:entity_type");
            const auto player_type =
                entity_types ? registries->protocol_id(*entity_types, "minecraft:player")
                             : std::nullopt;
            mobs->set_logic(*spawned, std::make_unique<gameplay::Mob>(
                                          *kind, state->width, state->height, state->network_id,
                                          player_type ? *player_type : gameplay::kNoQuarry));
        } else {
            mobs->set_logic(*spawned, std::make_unique<gameplay::FallingMob>());
        }
        // ── husbandry: an egg hatches a chick — measured, Age -24000 ──
        if (husbandry) {
            husbandry->make_baby(*mobs, *spawned);
        }
        mob_packets(*state, [&](i32 id, std::span<const u8> payload) {
            broadcast(nullptr, id, payload);
        });
    };
    projectile_host.spawn_orb = [&](Vec3d at, i32 value) {
        GroundOrb orb;
        orb.entity_id = next_entity_id.fetch_add(1);
        orb.x         = at.x;
        orb.y         = at.y;
        orb.z         = at.z;
        orb.value     = value;
        orb.born      = server_tick.load(std::memory_order_relaxed);
        broadcast(nullptr, net::clientbound::kSpawnExperienceOrb,
                  net::encode_spawn_experience_orb(orb.entity_id, orb.x, orb.y, orb.z,
                                                   static_cast<i16>(orb.value)));
        ground_orbs.push_back(orb);
    };
    projectile_host.drop_item                 = tnt_host.drop_item;
    // ── brewing ── what a broken potion, a cloud and a tipped arrow reach for
    PotionHost potion_host;
    potion_host.players = [&](std::vector<PotionPlayer>& out) {
        for (const auto& [key, who] : players) {
            if (who.connection && who.confirmed) {
                out.push_back(PotionPlayer{who.entity_id, Vec3d{who.x, who.y, who.z},
                                           !who.survival.awaiting_respawn &&
                                               !who.survival.health.dead});
            }
        }
    };
    potion_host.affect = [&](i32 id, const EffectRule& rule) {
        if (Player* who = projectile_player(id); who != nullptr) {
            who->effects.with_target(who->survival, effect_io_for(*who), effect_bearer_for(*who),
                                     rule);
        }
    };
    potion_host.next_entity_id = [&] { return next_entity_id.fetch_add(1); };
    potion_host.broadcast      = [&](i32 id, std::span<const u8> payload) {
        broadcast(nullptr, id, payload);
    };
    projectile_host.potion_broke = [&](Vec3d at, const net::ItemStack& potion, i32 target,
                                       bool is_player) {
        // ── mobs-3 ── a potion's Weakness reaches the zombie villagers round it
        if (zombie_villagers && mobs) {
            i32 weakness = 0;
            for (const gameplay::PotionEffect& effect : potion_contents(potion).effects) {
                if (effect.effect == gameplay::Effect::Weakness) {
                    weakness = std::max(weakness, effect.duration);
                }
            }
            zombie_villagers->on_splash(*mobs, at, weakness);
        }
        if (brewing) {
            brewing->potion_broke(potion_host, at, potion, target, is_player);
        }
    };
    projectile_host.arrow_hit = [&](const net::ItemStack& arrow, i32 target, bool is_player) {
        if (brewing) {
            brewing->arrow_hit(potion_host, arrow, target, is_player);
        }
    };
    // ── end brewing ──
    // ── fire ── a Flame arrow's victim, and what a burning mob's death drops
    projectile_host.set_on_fire = [&](i32 entity_id, i32 seconds) {
        if (fire_session) {
            fire_session->set_mob_on_fire(entity_id, seconds);
        }
    };
    const FireMobHost fire_mob_host{tnt_deliver, tnt_host.drop_item};
    // ── end fire ──
    const ProjectileDeliver projectile_deliver = tnt_deliver;
    // ── mobs-3 ── what a hostile mob's swing reaches for (mob_attacks.hpp)
    /// The entities despawn must never touch: their own modules remove them.
    const std::function<bool(i32)> mobs3_transient = [&](i32 type) {
        return (tnt_gravity && tnt_gravity->owns(type)) || (projectiles && projectiles->owns(type));
    };
    MobAttackHost mob_attack_host;
    mob_attack_host.hurt_player  = projectile_host.hurt_player;
    mob_attack_host.knock_player = [&](i32 id, Vec3d from) {
        Player* who = projectile_player(id);
        if (who == nullptr || !registries) {
            return;
        }
        const WornArmour worn = worn_armour(
            *registries, item_registry, std::span<const net::ItemStack>{who->inventory}.subspan(5, 4));
        const Vec3d push = gameplay::apply_knockback(
            Vec3d{}, who->on_ground, gameplay::kMobHitKnockback, from.x - who->x, from.z - who->z,
            std::min(worn.knockback_resistance, 1.0F), gameplay::CombatConstants{});
        if (const auto framed = net::encode_packet(
                net::clientbound::kEntityVelocity,
                net::encode_entity_velocity(who->entity_id, push.x, push.y, push.z));
            framed && who->connection) {
            who->connection->send(*framed);
        }
    };
    mob_attack_host.give_effect = [&](i32 id, const gameplay::EffectInstance& effect) {
        if (Player* who = projectile_player(id); who != nullptr) {
            (void)who->effects.apply(effect, who->survival, effect_io_for(*who),
                                     effect_bearer_for(*who));
        }
    };
    // ── end mobs-3 ──
    // ── end projectiles ─────────────────────────────────────────────────────

    // ── husbandry ───────────────────────────────────────────────────────────
    // Everything here runs on the tick thread with players_mutex held (the
    // entity block takes it), which is what lets it touch an inventory.
    HusbandryHost          husbandry_host;
    const HusbandryDeliver husbandry_deliver = tnt_deliver;
    husbandry_host.with_hand = [&](i32 id, net::Hand hand,
                                   const std::function<void(HusbandryHand&)>& visit) {
        for (auto& [key, who] : players) {
            if (who.entity_id != id || !who.connection || who.survival.awaiting_respawn) {
                continue;
            }
            HusbandryHand lent;
            lent.entity_id = who.entity_id;
            lent.eyes      = Vec3d{who.x, who.y + 1.62, who.z};
            lent.creative  = who.game_mode == 1;
            lent.inventory = who.inventory;
            lent.slot = hand == net::Hand::Main ? 36 + static_cast<usize>(who.held_slot) : 45;
            lent.send_slot = [&](usize slot) { send_slot(who, slot); };
            lent.give      = [&](const net::ItemStack& stack) { return give_to_player(who, stack); };
            lent.wear      = [&, hand](i32 amount) {
                // The main hand only: the damage helpers read the held slot.
                // Shears in the off hand are sheared with and not worn — named.
                if (hand != net::Hand::Main) {
                    return;
                }
                const i32 worn = held_damage(who) + amount;
                const auto max = gameplay::max_damage(held_name(who));
                if (max && worn >= *max) {
                    break_held_item(who);
                } else {
                    set_held_damage(who, worn);
                }
            };
            visit(lent);
            return true;
        }
        return false;
    };
    husbandry_host.tempters = [&](std::vector<gameplay::Tempter>& out) {
        for (const auto& [key, who] : players) {
            if (!who.connection || !who.confirmed || who.game_mode == 3 ||
                who.survival.awaiting_respawn) {
                continue;
            }
            const net::ItemStack& off = who.inventory[45];
            out.push_back(gameplay::Tempter{
                Vec3d{who.x, who.y, who.z}, held_name(who),
                off.item_id != 0 && off.count > 0 && registries && item_registry
                    ? registries->entry_of(*item_registry, off.item_id)
                    : std::string_view{}});
        }
    };
    husbandry_host.create_mob = [&](std::string_view type, Vec3d at) -> entity::EntityHandle {
        if (!mobs || !registries) {
            return entity::kNoEntity;
        }
        const auto spawned = mobs->spawn(type, at, net::Uuid{});
        const gameplay::MobKind* kind = gameplay::mob_kind(type);
        if (!spawned || kind == nullptr) {
            return entity::kNoEntity;
        }
        entity::EntityState* state = mobs->mutable_state(*spawned);
        state->uuid                = uuid_for_entity(state->network_id);
        state->broadcast_position  = state->position;
        state->broadcast_valid     = true;
        mobs->set_logic(*spawned, std::make_unique<gameplay::Mob>(
                                      *kind, state->width, state->height, state->network_id,
                                      gameplay::kNoQuarry));
        return *spawned;
    };
    husbandry_host.announce = [&](const entity::EntityState& state) {
        mob_packets(state, [&](i32 id, std::span<const u8> payload) {
            broadcast(nullptr, id, payload);
        });
    };
    husbandry_host.drop_item = tnt_host.drop_item;
    husbandry_host.spawn_orb = projectile_host.spawn_orb;
    // ── tame ── a pup's owner, a foal's stats; then what taming reaches for
    husbandry_host.on_birth = [&](entity::EntityHandle mother, entity::EntityHandle father,
                                  entity::EntityHandle child) {
        if (taming && mobs) {
            taming->on_birth(*mobs, mother, father, child);
        }
    };
    TamingHost taming_host;
    taming_host.with_hand = husbandry_host.with_hand;
    taming_host.players   = [&](std::vector<TamingPlayer>& out) {
        for (const auto& [key, who] : players) {
            if (!who.connection || !who.confirmed || who.survival.awaiting_respawn ||
                who.dimension != DimensionId::Overworld) {
                continue;
            }
            out.push_back(TamingPlayer{who.entity_id, who.uuid, Vec3d{who.x, who.y, who.z},
                                       who.on_ground});
        }
    };
    taming_host.carry_rider = rails_host.carry_rider;
    taming_host.set_down    = rails_host.set_down;
    // ── end tame ──
    // ── nether-2 ── What the Nether's mobs reach for. Caller holds
    // players_mutex, and chunk_mutex for the block reads.
    nether_mob_host.block_at = [&](BlockPos pos) -> registry::BlockStateId {
        const world::Chunk* chunk =
            chunk_if_resident_in(DimensionId::Nether, pos.x >> 4, pos.z >> 4);
        return chunk == nullptr || pos.y < 0 || pos.y >= 256
                   ? registry::kAirState
                   : chunk->get_block(static_cast<usize>(pos.x & 15), pos.y,
                                      static_cast<usize>(pos.z & 15));
    };
    nether_mob_host.loaded = [&](BlockPos pos) {
        return chunk_if_resident_in(DimensionId::Nether, pos.x >> 4, pos.z >> 4) != nullptr;
    };
    nether_mob_host.ticking = [&](ChunkPos pos) { return nether && nether->chunks().is_ticking(pos); };
    nether_mob_host.biome_at = [&](BlockPos pos) -> u16 {
        const world::Chunk* chunk =
            chunk_if_resident_in(DimensionId::Nether, pos.x >> 4, pos.z >> 4);
        return chunk == nullptr || pos.y < 0 || pos.y >= 256
                   ? u16{0}
                   : chunk->get_biome(static_cast<usize>(pos.x & 15), pos.y,
                                      static_cast<usize>(pos.z & 15));
    };
    nether_mob_host.block_light = [&](BlockPos pos) -> u8 {
        const world::Chunk* chunk =
            chunk_if_resident_in(DimensionId::Nether, pos.x >> 4, pos.z >> 4);
        const world::ChunkSection* section = chunk == nullptr ? nullptr : chunk->section_for_y(pos.y);
        return section == nullptr ? u8{0}
                                  : section->block_light().get(world::section_index(
                                        static_cast<usize>(pos.x & 15),
                                        static_cast<usize>(pos.y & 15),
                                        static_cast<usize>(pos.z & 15)));
    };
    nether_mob_host.players = [&](std::vector<NetherPlayer>& out) {
        for (const auto& [key, who] : players) {
            if (who.dimension != DimensionId::Nether || !who.connection) {
                continue;
            }
            bool gold = false;
            for (usize slot = 5; slot <= 8 && registries && item_registry; ++slot) {
                gold = gold || registries->entry_of(*item_registry, who.inventory[slot].item_id)
                                   .starts_with("minecraft:golden_");
            }
            out.push_back(NetherPlayer{who.entity_id, Vec3d{who.x, who.y, who.z}, !who.mortal(),
                                       !who.survival.awaiting_respawn, gold});
        }
    };
    nether_mob_host.broadcast = [&](i32 id, std::span<const u8> payload) {
        broadcast_in(DimensionId::Nether, nullptr, id, payload);
    };
    nether_mob_host.drop_item = [&](Vec3d at, const net::ItemStack& stack) {
        ItemEntity item;
        item.entity_id = next_entity_id.fetch_add(1);
        item.uuid      = uuid_for_entity(item.entity_id);
        item.x         = at.x;
        item.y         = at.y;
        item.z         = at.z;
        item.stack     = stack;
        item.born      = server_tick.load(std::memory_order_relaxed);
        item.dimension = DimensionId::Nether;
        std::vector<ItemEntity> one;
        one.push_back(std::move(item));
        publish_items(one);
    };
    nether_mob_host.hurt_player = projectile_host.hurt_player;
    nether_mob_host.take_held   = [&](i32 id, std::string_view item) {
        for (auto& [key, who] : players) {
            if (who.entity_id != id || held_name(who) != item) {
                continue;
            }
            if (who.mortal()) {
                consume_one_held(who);
            }
            return true;
        }
        return false;
    };
    nether_mob_host.send_to = [&](i32 id, i32 packet, std::span<const u8> payload) {
        for (const auto& [key, who] : players) {
            if (who.entity_id == id && who.connection) {
                if (const auto framed = net::encode_packet(packet, payload)) {
                    who.connection->send(*framed);
                }
            }
        }
    };
    // ── end nether-2 ──
    // ── end husbandry ───────────────────────────────────────────────────────

    // ── mobs-3 ── the zombie villagers' and the Anvil storage's reach: after
    // the husbandry host, whose `create_mob` both use.
    const auto mobs3_announce = [&](const entity::EntityState& state) {
        mob_packets(state, [&](i32 id, std::span<const u8> payload) {
            broadcast(nullptr, id, payload);
        });
    };
    ZombieVillagerHost zombie_villager_host;
    zombie_villager_host.create_mob = husbandry_host.create_mob;
    zombie_villager_host.announce   = mobs3_announce;
    zombie_villager_host.held       = [&](i32 id) -> std::string_view {
        const Player* who = projectile_player(id);
        return who != nullptr ? held_name(*who) : std::string_view{};
    };
    zombie_villager_host.creative = [&](i32 id) {
        const Player* who = projectile_player(id);
        return who != nullptr && who->game_mode == 1;
    };
    zombie_villager_host.consume_held = [&](i32 id) {
        if (Player* who = projectile_player(id); who != nullptr) {
            consume_one_held(*who);
        }
    };
    zombie_villager_host.speeds_cure = [&](BlockPos pos) {
        const world::Chunk* chunk = chunk_if_resident(pos.x >> 4, pos.z >> 4);
        if (chunk == nullptr || !blocks || !world::WorldShape::overworld().contains_y(pos.y)) {
            return false;
        }
        const std::string_view name = blocks->block_name(blocks->block_of(chunk->get_block(
            static_cast<usize>(pos.x & 15), pos.y, static_cast<usize>(pos.z & 15))));
        return name == "minecraft:iron_bars" || name.ends_with("_bed");
    };
    EntityStorageHost entity_storage_host;
    entity_storage_host.attach = [&](entity::EntityHandle handle, std::string_view type) {
        const entity::EntityState* state = mobs->state(handle);
        if (const gameplay::MobKind* kind = gameplay::mob_kind(type); kind != nullptr && state) {
            mobs->set_logic(handle, std::make_unique<gameplay::Mob>(
                                        *kind, state->width, state->height, state->network_id,
                                        mob_attacks ? mob_attacks->player_type()
                                                    : gameplay::kNoQuarry));
        } else {
            mobs->set_logic(handle, std::make_unique<gameplay::FallingMob>());
        }
    };
    entity_storage_host.announce   = mobs3_announce;
    entity_storage_host.transient  = mobs3_transient;
    entity_storage_host.slime_size = [&](i32 id) { return slimes ? slimes->size_of(id) : 1; };
    entity_storage_host.set_slime_size = [&](entity::EntityState& state, i32 size) {
        if (slimes) {
            slimes->set_size(state, size);
        }
    };
    entity_storage_host.creeper_powered = [&](i32 id) {
        return tnt_gravity && tnt_gravity->creeper_powered(id);
    };
    entity_storage_host.charge_creeper = [&](i32 id) {
        if (tnt_gravity) {
            tnt_gravity->charge_creeper(id);
        }
    };
    entity_storage_host.zombie_villager = [&](i32 id) -> gameplay::VillagerState* {
        return zombie_villagers ? zombie_villagers->kept(id) : nullptr;
    };
    entity_storage_host.conversion_time = [&](i32 id) {
        return zombie_villagers ? zombie_villagers->conversion_time(id) : -1;
    };
    entity_storage_host.set_conversion_time = [&](i32 id, i32 ticks) {
        if (zombie_villagers) {
            zombie_villagers->set_conversion_time(id, ticks);
        }
    };
    // ── tame ── Owner, Sitting, CollarColor, Tame, Temper, SaddleItem,
    // ArmorItem, ChestedHorse, Strength, Variant, the drawn Attributes…
    entity_storage_host.write_extra = [&](const entity::EntityState& state, nbt::Tag& out) {
        if (taming && mobs) {
            taming->write_nbt(*mobs, state, out);
        }
    };
    entity_storage_host.read_extra = [&](entity::EntityState& state, const nbt::Tag& compound) {
        if (taming && mobs) {
            taming->read_nbt(*mobs, state, compound);
        }
    };
    // ── persistence ── The items and orbs of every level, the Nether's mobs,
    // the End's dragon and crystals: each dimension through its own storage,
    // the only writer of its own entities/ (ground_entities.hpp,
    // dimension_entities.hpp).
    GroundHost ground_host;
    ground_host.now            = [&] { return server_tick.load(std::memory_order_relaxed); };
    ground_host.next_entity_id = [&] { return next_entity_id.fetch_add(1); };
    ground_host.uuid_for       = [&](i32 id) { return uuid_for_entity(id); };
    ground_host.announce_item  = [&](const ItemEntity& item) {
        broadcast_in(item.dimension, nullptr, net::clientbound::kSpawnEntity,
                     net::encode_spawn_entity(item.entity_id, item.uuid, net::kItemEntityType,
                                              item.x, item.y, item.z));
        broadcast_in(item.dimension, nullptr, net::clientbound::kEntityMetadata,
                     net::encode_item_metadata(item.entity_id, item.stack.item_id, item.stack.count));
    };
    ground_host.announce_orb = [&](const GroundOrb& orb) {
        broadcast_in(orb.dimension, nullptr, net::clientbound::kSpawnExperienceOrb,
                     net::encode_spawn_experience_orb(orb.entity_id, orb.x, orb.y, orb.z,
                                                      static_cast<i16>(std::min(orb.value, 32767))));
    };
    std::optional<GroundEntities>    overworld_ground, nether_ground, end_ground;
    std::optional<DimensionEntities> nether_entities, end_entities;
    std::vector<ChunkPos>            persist_evicted;
    std::vector<i32>                 persist_removed;
    if (registries) {
        overworld_ground.emplace(DimensionId::Overworld, *registries, ground_items, ground_orbs,
                                 ground_host);
        if (entity_storage) {
            entity_storage->add_loose(*overworld_ground);
        }
        if (brewing) {
            brewing->set_persistence_host(&potion_host);
        }
        if (projectiles) {
            projectiles->set_owner_lookup([&](i32 id) -> std::optional<net::Uuid> {
                const Player* who = projectile_player(id);
                return who != nullptr ? std::optional<net::Uuid>{who->uuid} : std::nullopt;
            });
        }
        if (nether_mobs) {
            nether_ground.emplace(DimensionId::Nether, *registries, ground_items, ground_orbs,
                                  ground_host);
            nether_entities.emplace(*registries, level_dir / "DIM-1" / "entities",
                                    &nether_mobs->world(), nether_mobs->storage_host(nether_mob_host));
            nether_entities->storage().add_loose(*nether_ground);
        }
        if (end_fight) {
            end_ground.emplace(DimensionId::End, *registries, ground_items, ground_orbs, ground_host);
            end_entities.emplace(*registries, level_dir / "DIM1" / "entities", nullptr,
                                 EntityStorageHost{});
            end_entities->storage().add_loose(*end_fight);
            end_entities->storage().add_loose(*end_ground);
            end_fight->set_host(&end_fight_host);
        }
    }
    mobs3_save_entities = [&] {
        if (entity_storage && mobs) {
            const EntityStorageStats saved =
                entity_storage->save_all(*mobs, mob_records, entity_storage_host);
            OV_LOG_INFO("entities: saved {} mobs across {} chunks", saved.entities, saved.chunks);
        }
        for (auto* level : {nether_entities ? &*nether_entities : nullptr,  // ── persistence ──
                            end_entities ? &*end_entities : nullptr}) {
            if (level != nullptr) {
                const EntityStorageStats saved = level->save();
                OV_LOG_INFO("entities: saved {} in {} across {} chunks", saved.entities,
                            level->storage().directory().parent_path().filename().string(),
                            saved.chunks);
            }
        }
    };
    // ── end mobs-3 ──
    // ── villagers ───────────────────────────────────────────────────────────
    // Runs on the tick thread with players_mutex held, as husbandry's host.
    VillagerHost villager_host;
    villager_host.with_player = [&](i32 id,
                                    const std::function<void(MerchantPlayer&)>& visit) {
        for (auto& [key, who] : players) {
            if (who.entity_id != id || !who.connection || who.survival.awaiting_respawn) {
                continue;
            }
            MerchantPlayer lent;
            lent.entity_id = who.entity_id;
            lent.eyes      = Vec3d{who.x, who.y + 1.62, who.z};
            lent.inventory = who.inventory;
            lent.carried   = &who.carried;
            lent.send      = [&who](i32 packet, std::span<const u8> payload) {
                if (const auto framed = net::encode_packet(packet, payload);
                    framed && who.connection) {
                    who.connection->send(*framed);
                }
            };
            lent.drop = [&](const net::ItemStack& stack) {
                if (tnt_host.drop_item) {
                    tnt_host.drop_item(Vec3d{who.x, who.y + 1.32, who.z}, stack);
                }
            };
            visit(lent);
            return true;
        }
        return false;
    };
    villager_host.spawn_orb = projectile_host.spawn_orb;
    // ── end villagers ───────────────────────────────────────────────────────

    // ── weather ─────────────────────────────────────────────────────────────
    // Built once, like the TNT's. Every callback runs in the weather block of
    // the tick, which holds players_mutex and chunk_mutex.
    WeatherHost weather_host;
    const auto  weather_player = [&](i32 id) -> Player* {
        for (auto& [key, who] : players) {
            if (who.entity_id == id && who.connection) {
                return &who;
            }
        }
        return nullptr;
    };
    weather_host.broadcast = tnt_deliver;
    weather_host.send_to   = [&](i32 id, i32 packet, std::span<const u8> payload) {
        Player* who = weather_player(id);
        if (who == nullptr) {
            return;
        }
        if (const auto framed = net::encode_packet(packet, payload)) {
            who->connection->send(*framed);
        }
    };
    weather_host.players = [&](std::vector<WeatherPlayer>& out) {
        for (const auto& [key, who] : players) {
            if (who.connection && who.confirmed) {
                out.push_back(WeatherPlayer{who.entity_id, who.uuid, Vec3d{who.x, who.y, who.z},
                                            who.yaw, who.pitch, !who.mortal(), who.game_mode == 3,
                                            !who.survival.awaiting_respawn});
            }
        }
    };
    weather_host.allocate_entity_id = [&] { return next_entity_id.fetch_add(1); };
    weather_host.place_player = [&](i32 id, Vec3d feet, f32 yaw, f32 pitch, bool sync) {
        Player* who = weather_player(id);
        if (who == nullptr) {
            return;
        }
        who->x     = feet.x;
        who->y     = feet.y;
        who->z     = feet.z;
        who->yaw   = yaw;
        who->pitch = pitch;
        if (sync) {
            who->pending_teleport = who->entity_id * 1000 + 13;
            if (const auto framed = net::encode_packet(
                    net::clientbound::kSynchronizePosition,
                    net::encode_synchronize_position(who->x, who->y, who->z, who->yaw, who->pitch,
                                                     who->pending_teleport))) {
                who->connection->send(*framed);
            }
        }
    };
    weather_host.hurt_player = [&](i32 id, f32 amount) {
        (void)projectile_host.hurt_player(id, amount, gameplay::DamageKind::LightningBolt);
    };
    weather_host.convert_mob = [&](entity::EntityHandle old, std::string_view type) {
        if (!mobs || !registries || mobs->state(old) == nullptr) {
            return false;
        }
        const Vec3d at      = mobs->state(old)->position;
        const f32   yaw     = mobs->state(old)->yaw;
        const auto  spawned = mobs->spawn(type, at, net::Uuid{});
        if (!spawned) {
            return false;
        }
        if (entity::EntityState* was = mobs->mutable_state(old)) {
            was->removed = true;
        }
        entity::EntityState* state = mobs->mutable_state(*spawned);
        state->uuid                = uuid_for_entity(state->network_id);
        state->yaw                 = yaw;
        state->head_yaw            = yaw;
        state->broadcast_position  = state->position;
        state->broadcast_valid     = true;
        if (const gameplay::MobKind* kind = gameplay::mob_kind(type)) {
            const auto entity_types = registries->find("minecraft:entity_type");
            const auto player_type =
                entity_types ? registries->protocol_id(*entity_types, "minecraft:player") : std::nullopt;
            mobs->set_logic(*spawned, std::make_unique<gameplay::Mob>(
                                          *kind, state->width, state->height, state->network_id,
                                          player_type ? *player_type : gameplay::kNoQuarry));
        } else {
            mobs->set_logic(*spawned, std::make_unique<gameplay::FallingMob>());
        }
        mob_packets(*state, [&](i32 id, std::span<const u8> payload) { broadcast(nullptr, id, payload); });
        return true;
    };
    weather_host.charge_creeper = [&](i32 id) {
        if (tnt_gravity) {
            tnt_gravity->charge_creeper(id);
        }
        // Index 17, a boolean: measured by measure_weather.py `strike`.
        net::MetadataWriter fields;
        fields.boolean_value(17, true);
        broadcast(nullptr, net::clientbound::kEntityMetadata,
                  net::encode_entity_metadata(id, fields.take()));
    };
    weather_host.sweep_items = tnt_host.sweep_items;
    weather_host.set_spawn   = [&](const net::Uuid& uuid, BlockPos head, f32 angle) {
        return commands && commands->set_personal_spawn(uuid, cmd::PersonalSpawn{head.x, head.y, head.z, angle});
    };
    weather_host.drop_item = tnt_host.drop_item;
    // ── fire ── a bolt lights the block it struck (the weather tick holds the
    // chunk lock and settles the writes after).
    weather_host.ignite = [&](BlockPos at) {
        return fire_session && level && fire_session->ignite(*level, at);
    };
    // ── end weather ─────────────────────────────────────────────────────────

    const auto should_stop = [&]() {
        return g_stop_requested.load(std::memory_order_relaxed) ||
               (external_stop != nullptr && external_stop->load(std::memory_order_relaxed));
    };

    // ── nether ──────────────────────────────────────────────────────────────
    //
    // Standing in a portal, and crossing. The rules — the frame, the scale,
    // the closest-portal search, the new portal — are in
    // ov_gameplay/nether_portal.hpp; what is here is the part that needs the
    // server: both levels' chunks, the players, and the packets.

    /// Does any block the player's box touches hold a portal? The game asks
    /// the same of every block inside the entity's box, whatever its shape.
    const auto portal_inside = [&](const Player& who) -> bool {
        const AABB box = gameplay::player_box(Vec3d{who.x, who.y, who.z});
        const i32  x0  = static_cast<i32>(std::floor(box.min.x + 1.0E-7));
        const i32  x1  = static_cast<i32>(std::floor(box.max.x - 1.0E-7));
        const i32  y0  = static_cast<i32>(std::floor(box.min.y + 1.0E-7));
        const i32  y1  = static_cast<i32>(std::floor(box.max.y - 1.0E-7));
        const i32  z0  = static_cast<i32>(std::floor(box.min.z + 1.0E-7));
        const i32  z1  = static_cast<i32>(std::floor(box.max.z - 1.0E-7));
        const std::scoped_lock lock{chunk_mutex};
        for (i32 x = x0; x <= x1; ++x) {
            for (i32 z = z0; z <= z1; ++z) {
                const world::Chunk* chunk = chunk_if_resident_in(who.dimension, x >> 4, z >> 4);
                if (chunk == nullptr) {
                    continue;
                }
                for (i32 y = y0; y <= y1; ++y) {
                    if (!chunk->shape().contains_y(y)) {
                        continue;
                    }
                    const auto state = chunk->get_block(static_cast<usize>(x & 15), y,
                                                        static_cast<usize>(z & 15));
                    if (state != registry::kAirState && portal_rules->is_portal(state)) {
                        return true;
                    }
                }
            }
        }
        return false;
    };

    /// The closest portal block to `target` within `radius` in a level: the
    /// resident chunks from memory, the others from their region files — the
    /// game's `nether_portal` point of interest, read at the moment it is
    /// needed rather than kept. Caller holds chunk_mutex.
    std::vector<BlockPos> portal_scratch;
    const auto portal_near = [&](DimensionId dimension, BlockPos target,
                                 i32 radius) -> std::optional<BlockPos> {
        portal_scratch.clear();
        for (i32 cz = (target.z - radius) >> 4; cz <= (target.z + radius) >> 4; ++cz) {
            for (i32 cx = (target.x - radius) >> 4; cx <= (target.x + radius) >> 4; ++cx) {
                if (const world::Chunk* here = chunk_if_resident_in(dimension, cx, cz)) {
                    portal_blocks_in(*here, *portal_rules, portal_scratch);
                    continue;
                }
                if (dimension == DimensionId::Nether) {
                    if (nether) {
                        if (const auto document = nether->read_from_disk(cx, cz)) {
                            portal_blocks_in(*document, portal_scratch);
                        }
                    }
                    continue;
                }
                const auto region = nbt::RegionFile::open(region_path(world_dir, cx, cz));
                if (region && region->has_chunk(static_cast<u32>(cx & 31), static_cast<u32>(cz & 31))) {
                    if (const auto document =
                            region->read_chunk(static_cast<u32>(cx & 31), static_cast<u32>(cz & 31))) {
                        portal_blocks_in(*document, portal_scratch);
                    }
                }
            }
        }
        return gameplay::closest_portal(portal_scratch, target, radius);
    };

    /// Is a chunk of the overworld saved on disk? A chunk read from disk costs
    /// microseconds on the tick thread; one generated there costs a stall.
    const auto overworld_on_disk = [&](i32 cx, i32 cz) {
        const auto region = nbt::RegionFile::open(region_path(world_dir, cx, cz));
        return region && region->has_chunk(static_cast<u32>(cx & 31), static_cast<u32>(cz & 31));
    };

    // ── end ── Defined below `travel`, which calls it.
    std::function<void(Player&, DimensionId, DimensionId, Vec3d, f32, u8)> arrive_in;

    /// Cross: find or build the portal at the other end, and move the player
    /// there — Respawn, position, chunks, and who can see whom.
    ///
    /// Two phases. When no portal is found and the chunks a new one may reach
    /// are not all here, a `Transient` ticket round the target asks the
    /// workers for them and this returns false; the tick asks again until they
    /// have arrived. Measured without it: the first crossing held the tick
    /// thread for 9.2 s generating sixteen Nether chunks.
    ///
    /// Caller holds players_mutex. True when the crossing is done, or given up.
    const auto travel = [&](Player& who) -> bool {
        const DimensionId from = who.dimension;
        const DimensionId to   = from == DimensionId::Overworld ? DimensionId::Nether
                                                                : DimensionId::Overworld;
        const DimensionInfo& to_info = dimension_info(to);
        // The player's box, in the game's floats: 0.6 by 1.8. The arrival's
        // last digits — 120.0000122 rather than 120 — come from these being
        // floats widened, and are measured, not rounded away.
        const f64 width  = static_cast<f64>(0.6F);
        const f64 height = static_cast<f64>(1.8F);

        Vec3d arrival{};
        f32   yaw = who.yaw;
        {
            const std::scoped_lock lock{chunk_mutex};
            if (to == DimensionId::Nether && !nether_ready()) {
                OV_LOG_WARN("{} stepped through a portal, and the Nether is unavailable", who.name);
                return true;
            }
            ServerLevel& from_level = from == DimensionId::Nether ? *nether_level : *level;
            ServerLevel& to_level   = to == DimensionId::Nether ? *nether_level : *level;

            // The portal being left, measured around the block the player is in.
            std::optional<gameplay::PortalRect> entrance;
            const AABB box = gameplay::player_box(Vec3d{who.x, who.y, who.z});
            for (i32 y = static_cast<i32>(std::floor(box.min.y));
                 !entrance && y <= static_cast<i32>(std::floor(box.max.y)); ++y) {
                for (i32 x = static_cast<i32>(std::floor(box.min.x));
                     !entrance && x <= static_cast<i32>(std::floor(box.max.x)); ++x) {
                    for (i32 z = static_cast<i32>(std::floor(box.min.z));
                         !entrance && z <= static_cast<i32>(std::floor(box.max.z)); ++z) {
                        entrance = portal_rules->rectangle_at(from_level, BlockPos{x, y, z});
                    }
                }
            }
            if (!entrance) {
                // Stepped out while the chunks were on their way. Said, and not
                // silently: a crossing that vanishes looks exactly like one that
                // never started.
                if (who.crossing_ticks > 0) {
                    OV_LOG_INFO("{} left the portal after {} ticks waiting for the chunks round "
                                "the destination; the crossing is dropped",
                                who.name, who.crossing_ticks);
                }
                return true;
            }

            const f64      scale  = from == DimensionId::Overworld ? 1.0 / 8.0 : 8.0;
            const BlockPos target = gameplay::scaled_target(Vec3d{who.x, who.y, who.z}, scale);
            world::ChunkMap& to_map = *view_of(to).chunks;

            std::optional<gameplay::PortalRect> exit;
            if (const auto found = portal_near(to, target, to_info.search_radius)) {
                (void)chunk_at_in(to, found->x >> 4, found->z >> 4);
                exit = portal_rules->rectangle_at(to_level, *found);
            }
            if (!exit) {
                // Everything a new portal can reach — 16 blocks round the
                // target and the frame's own two beyond — has to be here.
                // What is on disk is read now; what is not is asked of the
                // workers through a ticket, and the crossing waits.
                bool ready = true;
                for (i32 cz = (target.z - gameplay::kCreateRadius - 2) >> 4;
                     cz <= (target.z + gameplay::kCreateRadius + 2) >> 4; ++cz) {
                    for (i32 cx = (target.x - gameplay::kCreateRadius - 2) >> 4;
                         cx <= (target.x + gameplay::kCreateRadius + 2) >> 4; ++cx) {
                        if (chunk_if_resident_in(to, cx, cz) != nullptr) {
                            continue;
                        }
                        const bool on_disk = to == DimensionId::Nether
                                                 ? nether->read_from_disk(cx, cz).has_value()
                                                 : overworld_on_disk(cx, cz);
                        if (on_disk || (to == DimensionId::Overworld && !chunk_source)) {
                            (void)chunk_at_in(to, cx, cz);
                        } else {
                            ready = false;
                        }
                    }
                }
                if (!ready) {
                    if (who.crossing_ticks == 0) {
                        OV_LOG_INFO("{}: waiting for the chunks round ({}, {}, {}) in {} before "
                                    "building a portal there",
                                    who.name, target.x, target.y, target.z, to_info.name);
                    }
                    to_map.set_ticket(world::TicketType::Transient, static_cast<u64>(who.entity_id),
                                      ChunkPos{target.x >> 4, target.z >> 4},
                                      world::LoadLevel::for_view_distance(2));
                    world::LevelChanges changes;
                    to_map.refresh(changes);
                    return false;
                }
                exit = portal_rules->create(to_level, target, entrance->axis, to_info.portal_top);
                OV_LOG_INFO("{}: no portal within {} of ({}, {}, {}) in {}; built one at ({}, {}, {})",
                            who.name, to_info.search_radius, target.x, target.y, target.z,
                            to_info.name, exit->min_corner.x, exit->min_corner.y,
                            exit->min_corner.z);
            }
            const Vec3d relative =
                gameplay::relative_position(*entrance, Vec3d{who.x, who.y, who.z}, width, height);
            arrival = gameplay::exit_position(*exit, entrance->axis, relative, width, height, yaw);

            // The level being left lets go of the player's chunks, and the
            // destination of the crossing's own ticket.
            world::LevelChanges changes;
            view_of(from).chunks->remove_ticket(world::TicketType::Player,
                                               static_cast<u64>(who.entity_id));
            view_of(from).chunks->refresh(changes);
            to_map.remove_ticket(world::TicketType::Transient, static_cast<u64>(who.entity_id));
            to_map.refresh(changes);
        }
        // A built portal went through its level's drain hooks: its Block
        // Updates and its relighting go out with the next tick's flush — not
        // from here, which holds `players_mutex` that the flush try-locks (a
        // `try_lock` by the owner is undefined). The traveller is sent the
        // chunks themselves below, portal included.
        arrive_in(who, from, to, arrival, yaw, 3);  // ── end ── shared with the End
        return true;
    };

    // ── end nether ──────────────────────────────────────────────────────────
    //
    // Unchanged from here to the end of `arrive_in`: the second half of
    // `travel`, lifted out so that the End's crossings share it.
    // ── end ──
    /// Put a player into `to` at `arrival`: Respawn (`data_kept`), position,
    /// chunks from nothing, and who sees whom. Caller holds players_mutex.
    arrive_in = [&](Player& who, DimensionId from, DimensionId to, Vec3d arrival, f32 yaw,
                    u8 data_kept) {
        const DimensionInfo& to_info = dimension_info(to);
        // Gone from the old level, for everyone there.
        broadcast_in(from, who.connection.get(), net::clientbound::kRemoveEntities,
                     net::encode_remove_entity(who.entity_id));

        who.dimension = to;
        who.x         = arrival.x;
        who.y         = arrival.y;
        who.z         = arrival.z;
        who.yaw       = yaw;

        const auto send = [&](i32 id, std::span<const u8> payload) {
            if (const auto framed = net::encode_packet(id, payload)) {
                who.connection->send(*framed);
            }
        };
        net::Respawn respawn;
        respawn.dimension_type     = to_info.type;
        respawn.dimension_name     = to_info.name;
        respawn.game_mode          = who.game_mode;
        respawn.previous_game_mode = -1;
        // A crossing keeps everything: attributes and metadata both.
        respawn.data_kept       = data_kept;  // ── end ──
        respawn.portal_cooldown = who.portal.cooldown;
        send(net::clientbound::kRespawn, net::encode_respawn(respawn));
        send(net::clientbound::kPlayerAbilities, cmd::CommandService::abilities_for(who.game_mode));
        who.pending_teleport = who.pending_teleport + 1;
        send(net::clientbound::kSynchronizePosition,
             net::encode_synchronize_position(who.x, who.y, who.z, who.yaw, who.pitch,
                                              who.pending_teleport));

        // The client threw its world away: stream the new one from nothing.
        who.loaded_chunks.clear();
        who.pending_chunks.clear();
        who.streaming       = false;
        who.broadcast_valid = false;
        stream_chunks(who.connection, who);

        // Who sees whom, in the new level.
        broadcast_in(to, who.connection.get(), net::clientbound::kSpawnPlayer,
                     net::encode_spawn_player(who.entity_id, who.uuid, who.x, who.y, who.z,
                                              who.yaw, who.pitch));
        for (const auto& [other_key, other] : players) {
            if (&other == &who || other.dimension != to || !other.connection) {
                continue;
            }
            send(net::clientbound::kSpawnPlayer,
                 net::encode_spawn_player(other.entity_id, other.uuid, other.x, other.y, other.z,
                                          other.yaw, other.pitch));
        }
        for (const ItemEntity& item : ground_items) {
            if (item.dimension != to) {
                continue;
            }
            send(net::clientbound::kSpawnEntity,
                 net::encode_spawn_entity(item.entity_id, item.uuid, net::kItemEntityType, item.x,
                                          item.y, item.z));
            send(net::clientbound::kEntityMetadata,
                 net::encode_item_metadata(item.entity_id, item.stack.item_id, item.stack.count));
        }

        if (to == DimensionId::Nether && nether_mobs) {  // ── nether-2 ── its mobs
            nether_mobs->send_all(send);
        }
        // What the client rebuilt from scratch: the inventory, the bars, the
        // effects. Health and experience are resent by forgetting what was
        // last sent.
        send(net::clientbound::kContainerContent,
             net::encode_container_content(
                 0, 0,
                 player_window_contents(registries ? &*registries : nullptr,
                                        recipe_book ? &*recipe_book : nullptr, who.inventory),
                 net::ItemStack{}));
        who.survival.broadcast_health = -1.0F;
        who.survival.broadcast_food   = -1;
        who.survival.broadcast_level  = -1;
        who.survival.broadcast_points = -1;
        who.effects.announce(who.survival, effect_io_for(who), effect_bearer_for(who));

        OV_LOG_INFO("{} crossed into {} at ({:.3f}, {:.3f}, {:.3f})", who.name, to_info.name,
                    who.x, who.y, who.z);
    };
    // ── end nether ──────────────────────────────────────────────────────────

    // ── end ─────────────────────────────────────────────────────────────────
    // Into the End through a portal, and out through the exit portal.
    // The rules are end_portal.hpp's; the arrival is end_travel.hpp's.

    /// Into the End: the platform's chunks (asked of the workers through a
    /// `Transient` ticket, as for a Nether crossing), the platform rebuilt,
    /// the arrival. False while the chunks are on their way. Caller holds
    /// players_mutex.
    const auto enter_end = [&](Player& who) -> bool {
        const DimensionId from = who.dimension;
        {
            const std::scoped_lock lock{chunk_mutex};
            if (!end_ready() || !end_rules || !end_level) {
                OV_LOG_WARN("{} stepped into an End portal, and the End is unavailable", who.name);
                return true;
            }
            world::ChunkMap& map   = end_world->chunks();
            bool             ready = true;
            // The platform's two chunks, and nothing else: the fight waits for
            // its own arena on later ticks, as the game's does.
            for (const ChunkPos pos : end_platform_chunks()) {
                if (end_world->resident(pos.x, pos.z) != nullptr) {
                    continue;
                }
                if (end_world->read_from_disk(pos.x, pos.z)) {
                    (void)end_world->chunk_at(pos.x, pos.z);
                } else {
                    ready = false;
                }
            }
            if (!ready) {
                if (who.end_crossing_ticks == 0) {
                    OV_LOG_INFO("{}: waiting for the End's platform chunks", who.name);
                }
                // Radius one round (6, 0): the two 4 x 4 generation blocks the
                // platform's chunks fall in, and no more.
                const BlockPos spawn = gameplay::kEndSpawnPoint;
                map.set_ticket(world::TicketType::Transient, static_cast<u64>(who.entity_id),
                               ChunkPos{spawn.x >> 4, spawn.z >> 4},
                               world::LoadLevel::for_view_distance(1));
                world::LevelChanges changes;
                map.refresh(changes);
                return false;
            }
            end_level->set_game_time(server_tick.load(std::memory_order_relaxed));
            end_rules->build_platform(*end_level);
            world::LevelChanges changes;
            view_of(from).chunks->remove_ticket(world::TicketType::Player,
                                               static_cast<u64>(who.entity_id));
            view_of(from).chunks->refresh(changes);
            map.remove_ticket(world::TicketType::Transient, static_cast<u64>(who.entity_id));
            map.refresh(changes);
        }
        const EndArrival arrival = end_arrival();
        who.pitch                = arrival.pitch;
        arrive_in(who, from, DimensionId::End, arrival.position, arrival.yaw, 3);
        return true;
    };

    /// Is a player's box in an End portal's slab, in the level they stand in?
    const auto in_end_portal = [&](const Player& who) -> bool {
        if (!end_rules || who.dimension == DimensionId::Nether) {
            return false;
        }
        const std::scoped_lock lock{chunk_mutex};
        ServerLevel* here = who.dimension == DimensionId::End ? (end_level ? &*end_level : nullptr)
                                                              : (level ? &*level : nullptr);
        return here != nullptr &&
               end_rules->box_in_portal(*here, gameplay::player_box(Vec3d{who.x, who.y, who.z}));
    };

    /// Out through the exit portal: the credits. The client asks to respawn
    /// when they are over (Client Command 0), and `won_game` sends it home.
    const auto leave_end = [&](Player& who) {
        if (who.won_game) {
            return;
        }
        who.won_game = true;
        if (const auto framed = net::encode_packet(
                net::clientbound::kGameEvent,
                net::encode_game_event(kGameEventWinGame, who.seen_credits ? 0.0F : 1.0F))) {
            who.connection->send(*framed);
        }
        who.seen_credits = true;
        OV_LOG_INFO("{} went through the exit portal: the credits", who.name);
    };
    // ── end end ─────────────────────────────────────────────────────────────

    while (!should_stop()) {
        // ── screens ── the integrated server behind a pause menu runs no tick.
        // The network thread still answers; the clock restarts on resume so
        // the pause is not caught up afterwards.
        if (external_pause != nullptr && external_pause->load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            clock.reset();
            continue;
        }
        // ── end screens ──
        perf->begin_tick();  // ── perf ──
        const auto tick_started = std::chrono::steady_clock::now();
        const i32  ticks        = clock.advance();
        server_tick.store(clock.tick_count(), std::memory_order_relaxed);

        perf->enter(TickPhase::ChunkPublish);  // ── perf ──
        // ── Terrain comes home ──────────────────────────────────────────────
        //
        // The single-writer rule in one place: workers build chunks nothing
        // else can reach, and this — the tick thread, and only the tick thread
        // — is what makes them part of the world. A chunk moves once, from a
        // vector the worker has already let go of into the map, and is never
        // touched by two threads at any point in its life.
        //
        // A chunk that is already resident wins. It was either read from disk,
        // which must beat anything generated, or it has been built in, which
        // must beat everything.
        if (chunk_source) {
            finished_blocks.clear();
            if (chunk_source->drain(finished_blocks) != 0) {
                const std::scoped_lock lock{chunk_mutex};
                for (GeneratedBlock& block : finished_blocks) {
                    for (auto& [pos, chunk] : block.chunks) {
                        if (chunks.contains(pos)) {
                            continue;
                        }
                        chunks.publish(pos, std::move(chunk));
                        light_arrived(pos, false);  // ── light ──
                        ++chunks_published;
                    }
                }
            }
        }

        perf->enter(TickPhase::SpawnArea);  // ── perf ──
        // ── loading ──
        // How much of the spawn area is resident: the nine chunks around the
        // spawn column, which the Forced ticket keeps once they arrive. Logged
        // the way vanilla's server logs its own preparation, and published for
        // the login path. Only until it is complete; then it costs nothing.
        if (chunk_source && spawn_percent_seen < 100) {
            const i32 spawn_cx = level_settings.spawn_x >> 4;
            const i32 spawn_cz = level_settings.spawn_z >> 4;
            i32       resident = 0;
            {
                const std::scoped_lock lock{chunk_mutex};
                for (i32 dz = -1; dz <= 1; ++dz) {
                    for (i32 dx = -1; dx <= 1; ++dx) {
                        if (chunk_if_resident(spawn_cx + dx, spawn_cz + dz) != nullptr) {
                            ++resident;
                        }
                    }
                }
            }
            const i32 percent = resident * 100 / 9;
            if (percent != spawn_percent_seen) {
                spawn_percent_seen = percent;
                spawn_ready_percent.store(percent, std::memory_order_relaxed);
                if (percent < 100) {
                    OV_LOG_INFO("Preparing spawn area: {}%", percent);
                } else {
                    OV_LOG_INFO("Spawn area ready in {:.1f} s — players may join",
                                std::chrono::duration<f64>(std::chrono::steady_clock::now() -
                                                           spawn_prep_started)
                                    .count());
                }
            }
        }
        // ── end loading ──
        // ── nether: its terrain comes home too, and its tickets are served ──
        if (nether) {
            const std::scoped_lock lock{chunk_mutex};
            nether->tick(clock.tick_count());
        }
        if (end_world) {  // ── end ── and the End's
            const std::scoped_lock lock{chunk_mutex};
            end_world->tick(clock.tick_count());
        }

        perf->enter(TickPhase::ChunkRequests);  // ── perf ──
        // ── What the tickets want and the map has not got ───────────────────
        //
        // This is the whole model in six lines: a chunk is generated because
        // something asked for it to be loaded, not because a client is waiting
        // for a packet. The two used to be the same thing, and that is why a
        // chunk nobody had asked for yet was generated on this thread while a
        // player watched.
        //
        // `wanted_chunks` comes back ordered by level, which is nearest-first
        // without the map having to know where anybody is. The source refuses
        // once its queue is full and the scan simply continues; next tick asks
        // again, from where the player is then.
        if (chunk_source) {
            const std::scoped_lock lock{chunk_mutex};
            chunks.wanted_chunks(wanted_scratch);
            for (const ChunkPos pos : wanted_scratch) {
                if (chunks.contains(pos)) {
                    continue;
                }
                (void)chunk_source->request(pos);
            }
        }

        perf->enter(TickPhase::ChunkEviction);  // ── perf ──
        // ── Chunks nothing wants any more ───────────────────────────────────
        //
        // Every five seconds rather than every tick: eviction walks the
        // resident set, and a walk of a few thousand entries twenty times a
        // second would be the new thing that eats the budget. A dirty chunk
        // stays until the next save, because dropping it would throw away
        // somebody's building.
        if (clock.tick_count() % 100 == 0) {
            const std::scoped_lock lock{chunk_mutex};
            to_evict.clear();
            chunks.for_each([&](ChunkPos pos, const world::Chunk&) {
                if (chunks.is_wanted(pos)) {
                    return;
                }
                const i64 key = chunk_key(pos.x, pos.z);
                if (dirty_chunks.contains(key) || read_only_chunks.contains(key)) {
                    return;
                }
                to_evict.push_back(pos);
            });
            // ── mobs-3 ── the mobs of the chunks leaving go to entities/ first.
            // Under the players' lock like every other change to the entity
            // world; without it, nothing is evicted this round.
            if (entity_storage && mobs && !to_evict.empty()) {
                std::unique_lock mobs3_lock{players_mutex, std::try_to_lock};
                if (mobs3_lock.owns_lock()) {
                    (void)entity_storage->unload_chunks(to_evict, *mobs, mob_records,
                                                        entity_storage_host, mobs3_unloaded);
                } else {
                    to_evict.clear();
                }
            }
            for (const ChunkPos pos : to_evict) {
                (void)chunks.evict(pos);
            }
            if (!to_evict.empty()) {
                OV_LOG_DEBUG("unloaded {} chunks no ticket reaches, {} resident", to_evict.size(),
                             chunks.resident());
            }
        }

        perf->enter(TickPhase::WorldClock);  // ── perf ──
        // Finish the digs the client claimed too early to be believed. Vanilla
        // keeps its own clock for those, and so do we: the block comes off on
        // the tick the rule says, not on the tick the client asked for.
        // The world clock, every second. The client places the sun with it, and
        // it is the only packet carrying a tick number — which is what lets the
        // break times be measured from outside at all.
        if (clock.tick_count() % 20 == 0) {
            std::unique_lock time_lock{players_mutex, std::try_to_lock};
            if (time_lock.owns_lock()) {
                // ── commands: the world's own clock, which /time sets ──
                const std::vector<u8> time_payload =
                    commands ? commands->world().update_time_payload()
                             : net::encode_update_time(clock.tick_count(), i64{-6000});
                if (const auto framed =
                        net::encode_packet(net::clientbound::kUpdateTime, time_payload)) {
                    for (auto& [clock_key, watcher] : players) {
                        if (watcher.connection) {
                            watcher.connection->send(*framed);
                        }
                    }
                }
            }
        }

        // The mobs asked for on the command line, placed once.
        //
        // Three seconds in, not at start-up. The chunk under them has to exist
        // before anything can fall onto it, and a client that connects while
        // the server is starting should see them appear and drop rather than
        // find them already standing there — which is also what makes the fall
        // observable from outside at all.
        if (!mobs_placed && mobs && blocks && clock.tick_count() >= 60) {
            mobs_placed = true;
            for (usize index = 0; index < options.mobs.size(); ++index) {
                // A line in front of the spawn point, two blocks apart, three
                // blocks up so the fall is visible rather than instantaneous.
                const Vec3d where{static_cast<f64>(index) * 2.0 + 0.5,
                                  static_cast<f64>(Superflat::kSurfaceY) + 4.0, 3.5};
                const auto spawned = mobs->spawn(options.mobs[index], where, net::Uuid{});
                if (!spawned) {
                    OV_LOG_WARN("cannot spawn {}: {}", options.mobs[index],
                                entity::to_string(spawned.error()));
                    continue;
                }
                entity::EntityState* state = mobs->mutable_state(*spawned);
                state->uuid                = uuid_for_entity(state->network_id);
                state->broadcast_position  = state->position;
                state->broadcast_valid     = true;
                mob_records.pin(state->network_id);  // ── mobs-3 ── a fixture: never despawned
                // Behaviour, in the one component that carries it: a brain for
                // a species we have goals for, and the bare falling floor for
                // one we do not. Refused by name rather than given a plausible
                // default — a mob invented a behaviour for is worse than one
                // that only falls, because only one of them says so.
                //
                // The seed is the wire id, so a world replayed from the same
                // sequence of spawns behaves identically.
                if (const gameplay::MobKind* kind = gameplay::mob_kind(options.mobs[index])) {
                    // What a hostile one hunts. Mojang's minecraft:player id,
                    // and it is deliberately *not* "anything": handed that, a
                    // skeleton and a spider two blocks apart lock onto each
                    // other and never move again. Players are not yet entities
                    // in this world, so today nothing matches and hostiles
                    // wander — which is the honest behaviour rather than a
                    // brawl between the scenery.
                    const auto entity_types = registries->find("minecraft:entity_type");
                    const auto player_type =
                        entity_types ? registries->protocol_id(*entity_types, "minecraft:player")
                                     : std::nullopt;
                    mobs->set_logic(
                        *spawned,
                        std::make_unique<gameplay::Mob>(
                            *kind, state->width, state->height, state->network_id,
                            player_type ? *player_type : gameplay::kNoQuarry));
                } else {
                    mobs->set_logic(*spawned, std::make_unique<gameplay::FallingMob>());
                }
                OV_LOG_INFO("spawned {} as entity {} at {:.1f} {:.1f} {:.1f} "
                            "({:.2f} wide, {:.2f} tall, {:.0f} health)",
                            options.mobs[index], state->network_id, where.x, where.y, where.z,
                            state->width, state->height, state->health);
                const std::unique_lock lock{players_mutex, std::try_to_lock};
                if (lock.owns_lock()) {
                    mob_packets(*state, [&](i32 id, std::span<const u8> payload) {
                        broadcast(nullptr, id, payload);
                    });
                }
            }
        }

        perf->enter(TickPhase::Commands);  // ── perf ──
        // ── commands ────────────────────────────────────────────────────────
        // The clock and the weather move every tick. The queue runs at the
        // first tick the player map is free, as every other pass here waits
        // for it; before the scheduled ticks, so a /setblock is settled on
        // the tick it ran.
        if (commands) {
            commands->tick_world();
            std::unique_lock command_lock{players_mutex, std::try_to_lock};
            if (command_lock.owns_lock()) {
                commands->run(command_host);
                // ── spawn eggs: the requests of the network thread, through
                // /summon's own path, with the player map held as it needs ──
                std::vector<std::pair<std::string, Vec3d>> eggs;
                {
                    const std::scoped_lock egg_lock{egg_mutex};
                    eggs.swap(egg_requests);
                }
                for (const auto& [type, at] : eggs) {
                    if (!command_host.summon(type, at)) {
                        OV_LOG_INFO("spawn egg: {} is not a creature this server can "
                                    "spawn yet",
                                    type);
                    }
                }
            }
        }
        // ── end commands ────────────────────────────────────────────────────

        perf->enter(TickPhase::ScheduledTicks);  // ── perf ──
        // ── fire ── the rule, the rain and the difficulty a fire reads this tick
        if (fire_session) {
            fire_session->set_world(FireWorld{
                .fire_tick  = !commands || commands->world().rules.flag("doFireTick"),
                .raining    = commands && commands->world().weather.is_raining(),
                .difficulty = commands ? static_cast<i32>(commands->world().difficulty) : 2,
                .sky_darken = sky_darken_for(commands ? commands->world().day_time
                                                      : clock.tick_count())});
        }

        // ── Scheduled ticks ─────────────────────────────────────────
        //
        // Water flows and levers light things here, and nowhere else. Two
        // queues drained in order, then the notifications the drain produced
        // settled to a fixed point, all of it under one `chunk_mutex` and none
        // of it under `players_mutex` — the packets go out afterwards.
        if (level && world_ticks) {
            // Whatever a player did since the last tick, first. A lever flipped
            // on the network thread is only a block until this runs.
            std::vector<net::WirePosition> edits;
            {
                const std::scoped_lock lock{notification_mutex};
                edits.swap(pending_notifications);
            }

            WorldTickStats stats;
            {
                const std::scoped_lock chunk_lock{chunk_mutex};
                level->set_game_time(clock.tick_count());
                for (const net::WirePosition& where : edits) {
                    (void)world_ticks->notify(*level, BlockPos{where.x, where.y, where.z});
                }
                stats = world_ticks->run(*level, clock.tick_count());
            }
            flush_tick_writes();

            // Loud only when something happened, and at debug: a server with
            // nothing flowing must not print twenty lines a second.
            if (stats.fluid_ticks + stats.block_ticks > 0) {
                OV_LOG_DEBUG("tick {}: {} fluid, {} block, {} refused, {} waves, {} notified",
                             clock.tick_count(), stats.fluid_ticks, stats.block_ticks,
                             stats.refused, stats.waves, stats.notifications);
            }
        }

        // ── nether: the Nether level's drain, and its portals ──────────────
        if (nether && nether_level && world_ticks) {
            std::vector<net::WirePosition> edits;
            {
                const std::scoped_lock lock{notification_mutex};
                edits.swap(nether_notifications);
            }
            {
                const std::scoped_lock chunk_lock{chunk_mutex};
                nether_level->set_game_time(clock.tick_count());
                for (const net::WirePosition& where : edits) {
                    (void)world_ticks->notify(*nether_level, BlockPos{where.x, where.y, where.z});
                }
                (void)world_ticks->run(*nether_level, clock.tick_count());
            }
            flush_nether_tick_writes(nether_tick_broadcasts, DimensionId::Nether);  // ── end ──
        }
        if (portal_rules && level && nether_level) {
            std::unique_lock portal_lock{players_mutex, std::try_to_lock};
            if (portal_lock.owns_lock()) {
                for (auto& [portal_key, who] : players) {
                    if (!who.confirmed || !who.connection || who.survival.awaiting_respawn) {
                        continue;
                    }
                    if (who.portal.tick(portal_inside(who), !who.mortal())) {
                        who.crossing       = true;
                        who.crossing_ticks = 0;
                    }
                    if (who.crossing) {
                        // A minute is far past any generation this server
                        // does; a crossing still waiting then is given up and
                        // said so, rather than kept pending for ever.
                        constexpr i32 kCrossingPatience = 1200;
                        if (travel(who)) {
                            who.crossing = false;
                        } else if (++who.crossing_ticks > kCrossingPatience) {
                            OV_LOG_WARN("{}: the chunks round the destination portal did not "
                                        "arrive in {} ticks; the crossing is abandoned",
                                        who.name, kCrossingPatience);
                            who.crossing = false;
                        }
                    }
                }
            }
        }
        // ── end nether ──────────────────────────────────────────────────────

        // ── end: the End level's drain, and its portals ─────────────────────
        if (end_world && end_level && world_ticks) {
            std::vector<net::WirePosition> edits;
            {
                const std::scoped_lock lock{notification_mutex};
                edits.swap(end_notifications);
            }
            {
                const std::scoped_lock chunk_lock{chunk_mutex};
                end_level->set_game_time(clock.tick_count());
                for (const net::WirePosition& where : edits) {
                    (void)world_ticks->notify(*end_level, BlockPos{where.x, where.y, where.z});
                }
                (void)world_ticks->run(*end_level, clock.tick_count());
            }
            flush_nether_tick_writes(end_tick_broadcasts, DimensionId::End);
        }
        if (end_rules) {
            // No wait and no cooldown: an End portal takes whoever touches its
            // slab, on the tick the move is processed (end_portal.hpp).
            std::unique_lock end_lock{players_mutex, std::try_to_lock};
            if (end_lock.owns_lock()) {
                for (auto& [end_key, who] : players) {
                    if (!who.confirmed || !who.connection || who.survival.awaiting_respawn ||
                        who.won_game) {
                        continue;
                    }
                    if (!who.end_crossing && !in_end_portal(who)) {
                        continue;
                    }
                    if (!who.end_crossing) {
                        OV_LOG_INFO("{} is in an End portal at ({:.3f}, {:.3f}, {:.3f}) in {}",
                                    who.name, who.x, who.y, who.z,
                                    dimension_info(who.dimension).name);
                    }
                    if (who.dimension == DimensionId::End) {
                        leave_end(who);
                        continue;
                    }
                    who.end_crossing = true;
                    // Five minutes: a Debug build on a machine at load 35
                    // took more than one to generate the platform's blocks.
                    constexpr i32 kEndCrossingPatience = 6000;
                    if (enter_end(who)) {
                        who.end_crossing       = false;
                        who.end_crossing_ticks = 0;
                    } else if (++who.end_crossing_ticks > kEndCrossingPatience) {
                        OV_LOG_WARN("{}: the End's platform chunks did not arrive in {} ticks; "
                                    "the crossing is abandoned",
                                    who.name, kEndCrossingPatience);
                        who.end_crossing       = false;
                        who.end_crossing_ticks = 0;
                    }
                }
                // The fight starts once someone is in the End and its arena —
                // the four chunks round the origin, where the exit portal goes —
                // is loaded: the game's own condition (it waits for the arena).
                if (end_fight && !end_fight->started() && end_world && end_level &&
                    std::ranges::any_of(players, [](const auto& entry) {
                        return entry.second.dimension == DimensionId::End;
                    })) {
                    const std::scoped_lock arena_lock{chunk_mutex};
                    world::ChunkMap&       map   = end_world->chunks();
                    constexpr std::array<ChunkPos, 4> kArena{
                        ChunkPos{-1, -1}, ChunkPos{0, -1}, ChunkPos{-1, 0}, ChunkPos{0, 0}};
                    const bool loaded = std::ranges::all_of(kArena, [&](ChunkPos pos) {
                        return end_world->resident(pos.x, pos.z) != nullptr;
                    });
                    if (loaded) {
                        const i32 top = end_world->resident(0, 0)
                                            ->heightmap(world::HeightmapType::MotionBlockingNoLeaves)
                                            .first_free(0, 0);
                        end_level->set_game_time(clock.tick_count());
                        end_fight->start(*end_level, top, end_fight_host);
                        map.remove_ticket(world::TicketType::Transient, kEndFightTicket);
                    } else {
                        map.set_ticket(world::TicketType::Transient, kEndFightTicket,
                                       ChunkPos{0, 0}, world::LoadLevel::for_view_distance(1));
                    }
                    world::LevelChanges changes;
                    map.refresh(changes);
                }
                // The fight: its tick, and once a second who sees it — the
                // game's own cadence for its boss bar's players.
                if (end_fight && end_fight->started() && end_level) {
                    {
                        const std::scoped_lock fight_lock{chunk_mutex};
                        end_fight->tick(*end_level, end_fight_host);
                    }
                    // ── dragon ── level.dat's DragonFight follows the fight.
                    if (clock.tick_count() % 20 == 0) {
                        level_settings.dragon_fight = end_fight->save();
                    }
                    if (clock.tick_count() % 20 == 0) {
                        for (auto& [viewer_key, viewer] : players) {
                            if (!viewer.connection) {
                                continue;
                            }
                            const bool sees = viewer.dimension == DimensionId::End &&
                                              EndFight::within_range(
                                                  Vec3d{viewer.x, viewer.y, viewer.z});
                            const auto send_viewer = [&](i32 id, std::span<const u8> payload) {
                                if (const auto framed = net::encode_packet(id, payload)) {
                                    viewer.connection->send(*framed);
                                }
                            };
                            if (sees && end_fight_viewers.insert(viewer.entity_id).second) {
                                end_fight->show_to(send_viewer);
                            } else if (!sees && end_fight_viewers.erase(viewer.entity_id) != 0) {
                                end_fight->hide_from(send_viewer);
                            }
                        }
                    }
                }
            }
        }
        // ── end end ─────────────────────────────────────────────────────────

        perf->enter(TickPhase::RandomTicks);  // ── perf ──
        // ── agriculture: the random tick ────────────────────────────
        //
        // After the scheduled ticks, as in the game. The chunk set is
        // rebuilt once a second — it only changes when somebody crosses a
        // chunk border — and only chunks within 128 blocks of a player tick:
        // the game's rule, and why an empty server grows nothing.
        if (plants && plant_env && level && world_ticks) {
            if (clock.tick_count() % 20 == 0 || random_ticks.selected().empty()) {
                random_tick_players.clear();
                {
                    const std::unique_lock lock{players_mutex, std::try_to_lock};
                    if (lock.owns_lock()) {
                        // No spectators on this server: every player counts.
                        for (const auto& [key, who] : players) {
                            if (who.dimension == DimensionId::Overworld) {  // ── nether ──
                                random_tick_players.push_back(Vec3d{who.x, who.y, who.z});
                            }
                        }
                    }
                }
                const std::scoped_lock chunk_lock{chunk_mutex};
                random_ticks.select(chunks, random_tick_players);
            }
            {
                const std::scoped_lock chunk_lock{chunk_mutex};
                level->set_game_time(clock.tick_count());
                level->clear_changed();
                (void)random_ticks.run(*level, chunks, *plants, *plant_env);
                (void)world_ticks->settle_writes(*level);
            }
            flush_tick_writes();
            {
                const std::scoped_lock lock{plant_drops_mutex};
                plant_drops_out.swap(plant_drops);
            }
            if (!plant_drops_out.empty()) {
                const std::scoped_lock lock{players_mutex};
                publish_items(plant_drops_out);
                plant_drops_out.clear();
            }
        }
        // ── end agriculture ─────────────────────────────────────────

        // ── weather ─────────────────────────────────────────────────
        // Precipitation and lightning over the chunks the random tick
        // selected, the bolts, and the beds — after the random tick, as in
        // the game's chunk tick. Both locks: a bolt hurts players and a bed
        // moves them. The flush after, with neither held.
        if (weather && commands && level && world_ticks) {
            {
                std::unique_lock weather_lock{players_mutex, std::try_to_lock};
                if (weather_lock.owns_lock()) {
                    const std::scoped_lock chunk_lock{chunk_mutex};
                    level->set_game_time(clock.tick_count());
                    level->clear_changed();
                    const WeatherStats weather_stats =
                        weather->tick(*level, chunks, random_ticks.selected(), commands->world(),
                                      mobs ? &*mobs : nullptr, weather_host);
                    (void)world_ticks->settle_writes(*level);
                    if (weather_stats.strikes + weather_stats.slept + weather_stats.woke > 0 ||
                        weather_stats.night_skipped) {
                        OV_LOG_DEBUG("tick {}: {} bolts, {} asleep, {} woke, night skipped {}",
                                     clock.tick_count(), weather_stats.strikes, weather_stats.slept,
                                     weather_stats.woke, weather_stats.night_skipped);
                    }
                }
            }
            flush_tick_writes();
        }
        // ── end weather ─────────────────────────────────────────────

        perf->enter(TickPhase::Containers);  // ── perf ──
        // ── brewing ── Every brewing stand in the loaded chunks, watched or
        // not: a ticked block entity, like the furnace (brewing_session.hpp).
        if (brewing) {
            const std::scoped_lock brewing_pass{players_mutex, chunk_mutex};
            brewing_chunks.clear();
            chunks.for_each(
                [&](ChunkPos pos, const world::Chunk&) { brewing_chunks.push_back(pos); });
            const BrewingStats brewed =
                brewing->tick_stands(brewing_stand_host, brewing_chunks, clock.tick_count());
            if (brewed.brewed + brewed.refuelled > 0) {
                OV_LOG_INFO("tick {}: {} stands, {} brewed, {} refuelled", clock.tick_count(),
                            brewed.stands, brewed.brewed, brewed.refuelled);
            }
            for (auto& [key, player] : players) {
                if (!player.brewing_window || !player.connection) {
                    continue;
                }
                const auto send = [&](i32 id, std::span<const u8> payload) {
                    if (const auto framed = net::encode_packet(id, payload)) {
                        player.connection->send(*framed);
                    }
                };
                if (!brewing->refresh(brewing_stand_host, *player.brewing_window, player.inventory,
                                      player.carried, send)) {
                    send(net::clientbound::kCloseContainer,
                         net::encode_close_container(player.brewing_window->window_id));
                    player.brewing_window.reset();
                }
            }
        }
        // ── end brewing ──

        // ── Containers: hoppers, droppers, dispensers ───────────────
        //
        // After the redstone drain and never before it: a hopper's `enabled`
        // flag and a dispenser's `triggered` flag are both written by the drain,
        // and a pass that ran first would act on last tick's signal — one tick
        // of lag on every filter, and the kind of error that only shows up in a
        // build somebody spent an evening on.
        //
        // `players_mutex` **then** `chunk_mutex`, which is the order every other
        // path in this file takes them in: the pass swallows item entities and
        // throws new ones, and both of those live under the first lock while the
        // containers live under the second.
        if (item_transport) {
            const std::scoped_lock item_pass{players_mutex, chunk_mutex};
            transport_chunks.clear();
            transport_ejected.clear();
            transport_touched.clear();
            chunks.for_each(
                [&](ChunkPos pos, const world::Chunk&) { transport_chunks.push_back(pos); });

            TransportHost host;
            host.chunk = [&](i32 cx, i32 cz) -> world::Chunk* {
                // Resident only. A hopper must never pull three hundred
                // milliseconds of worldgen onto the tick thread.
                return chunk_if_resident(cx, cz);
            };
            host.mark_dirty = [&](i32 cx, i32 cz) { dirty_chunks.insert(chunk_key(cx, cz)); };
            host.eject = [&](BlockPos where, Direction facing, const net::ItemStack& stack) {
                (void)facing;
                transport_ejected.emplace_back(where, stack);
            };
            host.collect = [&](BlockPos                                    where,
                               const std::function<bool(net::ItemStack&)>& take) {
                for (usize i = ground_items.size(); i-- > 0;) {
                    ItemEntity& item = ground_items[i];
                    if (!hopper_suck_contains(where, item.x, item.y, item.z)) {
                        continue;
                    }
                    if (!take(item.stack)) {
                        continue;
                    }
                    if (item.stack.count <= 0) {
                        broadcast(nullptr, net::clientbound::kRemoveEntities,
                                  net::encode_remove_entity(item.entity_id));
                        ground_items.erase(ground_items.begin() + static_cast<isize>(i));
                    }
                    // One item per hopper per cycle, which is what the eight
                    // ticks are eight ticks of. Returning after the first
                    // success is what enforces it.
                    return true;
                }
                return false;
            };
            host.container_changed = [&](BlockPos where) {
                transport_touched.push_back(net::WirePosition{where.x, where.y, where.z});
            };

            const TransportStats transport =
                item_transport->tick(host, transport_chunks, clock.tick_count());

            // What a dropper threw. Published after the pass rather than inside
            // it, so that the spawn packets go out once per tick in one place.
            if (!transport_ejected.empty()) {
                std::vector<ItemEntity> thrown;
                thrown.reserve(transport_ejected.size());
                for (const auto& [where, stack] : transport_ejected) {
                    ItemEntity item;
                    item.entity_id = next_entity_id.fetch_add(1);
                    item.uuid = net::Uuid{0x4f564954454d0000ULL | static_cast<u64>(item.entity_id),
                                          static_cast<u64>(item.entity_id) *
                                              0x9E3779B97F4A7C15ULL};
                    // The centre of the square in front, on its floor: where
                    // vanilla puts it, and what a hopper under the target square
                    // is positioned to catch.
                    item.x            = static_cast<f64>(where.x) + 0.5;
                    item.y            = static_cast<f64>(where.y) + 0.5;
                    item.z            = static_cast<f64>(where.z) + 0.5;
                    item.stack        = stack;
                    item.born         = server_tick.load(std::memory_order_relaxed);
                    item.pickup_delay = 10;
                    thrown.push_back(std::move(item));
                }
                publish_items(thrown);
            }

            // A container whose contents moved under a screen somebody has open
            // has to be resent, or that player keeps clicking on items that are
            // no longer there and every click is refused with no explanation.
            if (!transport_touched.empty()) {
                for (auto& [key, who] : players) {
                    if (!who.window_open || !who.connection || who.window_spec == nullptr) {
                        continue;
                    }
                    const bool touched =
                        std::ranges::any_of(transport_touched, [&](const net::WirePosition& at) {
                            return at.x == who.window_x && at.y == who.window_y &&
                                   at.z == who.window_z;
                        });
                    if (!touched) {
                        continue;
                    }
                    const world::BlockEntity* entity =
                        chunk_at(who.window_x >> 4, who.window_z >> 4)
                            .block_entity_at(static_cast<usize>(who.window_x & 15), who.window_y,
                                             static_cast<usize>(who.window_z & 15));
                    const auto inventory = container_inventory(
                        entity, registries ? &*registries : nullptr, item_registry);
                    if (!inventory) {
                        continue;
                    }
                    std::vector<net::ItemStack> slots;
                    slots.reserve(static_cast<usize>(inventory->size()) + 36);
                    slots.insert(slots.end(), inventory->stacks().begin(),
                                 inventory->stacks().end());
                    for (usize i = 9; i < 45; ++i) {
                        slots.push_back(who.inventory[i]);
                    }
                    if (const auto framed = net::encode_packet(
                            net::clientbound::kContainerContent,
                            net::encode_container_content(who.window_id, 1, slots, who.carried))) {
                        who.connection->send(*framed);
                    }
                }
            }

            if (transport.hopper_moves + transport.fired + transport.collected > 0) {
                OV_LOG_DEBUG("tick {}: {} hoppers, {} moved, {} collected, {} fired, {} refused",
                             clock.tick_count(), transport.hoppers, transport.hopper_moves,
                             transport.collected, transport.fired, transport.unsupported);
            }
        }

        // ── fire: campfires cook ──
        if (campfires) {
            const std::scoped_lock grill_pass{players_mutex, chunk_mutex};
            (void)campfires->tick(campfire_host);
        }

        perf->enter(TickPhase::NaturalSpawning);  // ── perf ──
        // ── Natural spawning ────────────────────────────────────────
        //
        // Once a tick, over the chunks a ticket actually ticks, with the light
        // read out of the same arrays the client is sent. Everything the rule
        // needs that a `LevelView` cannot answer — the light, the players, the
        // ticking set, the live counts — is assembled here and nowhere else.
        if (spawning_ready && level && mobs && registries && blocks &&
            (!commands || commands->world().rules.flag("doMobSpawning"))) {  // ── commands ──
            const auto spawn_started = std::chrono::steady_clock::now();
            spawn_players.clear();
            {
                const std::unique_lock lock{players_mutex, std::try_to_lock};
                if (lock.owns_lock()) {
                    for (const auto& [key, who] : players) {
                        // ── nether ── The overworld spawns around its own players.
                        if (who.dimension == DimensionId::Overworld) {
                            spawn_players.push_back(Vec3d{who.x, who.y, who.z});
                        }
                    }
                }
            }

            // No players means no spawning — the game's rule, not an
            // optimisation, and the one that made a whole measurement campaign
            // read zero everywhere before the rig learned to keep a probe
            // client connected.
            if (!spawn_players.empty()) {
                // How many of each category are already alive. The cap is
                // compared against this, so counting them as `Monster` because
                // that is the enum's first value would stop every kind of
                // spawning as soon as enough cows existed. `EntityState` carries
                // Mojang's numeric type and not a name, so the name comes back
                // through the registry.
                spawn_live.fill(0);
                if (const auto entity_registry = registries->find("minecraft:entity_type")) {
                    for (const entity::EntityHandle handle : mobs->handles()) {
                        const entity::EntityState* state = mobs->state(handle);
                        if (state == nullptr) {
                            continue;
                        }
                        const std::string_view name =
                            registries->entry_of(*entity_registry, state->type);
                        if (name.empty()) {
                            continue;
                        }
                        ++spawn_live[static_cast<usize>(gameplay::category_of(name))];
                    }
                }

                spawn_requests.clear();
                {
                    const std::scoped_lock chunk_lock{chunk_mutex};

                    // The ticking set, rebuilt once a second and not once a
                    // tick.
                    //
                    // Measured, and the reason this is written down: rebuilding
                    // it every tick walked the whole resident map and sorted the
                    // result, and it took an idle tick on the lab world from
                    // **4 us to 2599 us** — six hundred times, for a list that
                    // only changes when somebody walks across a chunk border.
                    // A second of latency on that is invisible against a spawn
                    // cycle; two and a half milliseconds a tick is not.
                    if (clock.tick_count() % 20 == 0 || spawn_ticking.empty()) {
                        const auto rebuild_started = std::chrono::steady_clock::now();
                        spawn_ticking.clear();
                        // The **spawn square**: the 17x17 of chunks around each
                        // player, not every ticking chunk in the world.
                        //
                        // Not a cost cut but the rule. `effective_cap` scales a
                        // category's cap by `eligible_chunks / 289`, and 289 is
                        // 17x17 — the square around one player. Handing it every
                        // ticking chunk instead inflates the denominator, so a
                        // world with plenty loaded would allow several times the
                        // real cap, and each extra chunk is also three spawn
                        // attempts a tick spent nowhere near anybody.
                        //
                        // Walked out from each player rather than filtered from
                        // the resident set: with two players the square is a few
                        // hundred lookups, while the resident set grows with the
                        // world.
                        for (const Vec3d& who : spawn_players) {
                            const i32 centre_x = static_cast<i32>(std::floor(who.x)) >> 4;
                            const i32 centre_z = static_cast<i32>(std::floor(who.z)) >> 4;
                            for (i32 dz = -8; dz <= 8; ++dz) {
                                for (i32 dx = -8; dx <= 8; ++dx) {
                                    const ChunkPos pos{centre_x + dx, centre_z + dz};
                                    if (chunks.is_ticking(pos) &&
                                        std::ranges::find(spawn_ticking, pos) ==
                                            spawn_ticking.end()) {
                                        spawn_ticking.push_back(pos);
                                    }
                                }
                            }
                        }

                        // Sorted, because `for_each` walks a hash map: the order
                        // decides which chunk the spawner offers a position in
                        // first, and a hash order is neither stable across runs
                        // nor across a rebuild of the map. Determinism is
                        // principle 5.
                        std::ranges::sort(spawn_ticking, [](ChunkPos a, ChunkPos b) {
                            return a.z != b.z ? a.z < b.z : a.x < b.x;
                        });
                        spawn_rebuild_micros.push_back(
                            std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - rebuild_started)
                                .count());
                    }

                    gameplay::SpawnEnvironment environment;
                    environment.level             = &*level;
                    environment.light             = &spawn_light;
                    environment.players           = spawn_players;
                    environment.ticking_chunks    = spawn_ticking;
                    environment.live_per_category = spawn_live;
                    environment.registries        = &*registries;
                    // ── mobs-2 ──
                    environment.biomes     = per_biome ? &spawn_biomes : nullptr;
                    environment.world_seed = level_settings.seed;
                    environment.day_time   = commands ? commands->world().day_time
                                                      : server_tick.load(std::memory_order_relaxed);

                    spawner.spawn_tick(environment, spawn_requests);
                }

                for (const gameplay::SpawnRequest& request : spawn_requests) {
                    const auto spawned =
                        mobs->spawn(request.type_name, request.position, net::Uuid{});
                    if (!spawned) {
                        continue;
                    }
                    entity::EntityState* state = mobs->mutable_state(*spawned);
                    state->uuid               = uuid_for_entity(state->network_id);
                    state->broadcast_position = state->position;
                    state->broadcast_valid    = true;
                    if (slimes && slimes->owns(state->type)) {  // ── mobs-2 ──
                        slimes->on_spawn(*state, slime_random, 0.0F);
                    }
                    // Behaviour, by name, and refused rather than invented: a
                    // mob given a plausible default brain is worse than one that
                    // only falls, because only one of them says so.
                    if (const gameplay::MobKind* kind = gameplay::mob_kind(request.type_name)) {
                        const auto entity_types = registries->find("minecraft:entity_type");
                        const auto player_type =
                            entity_types
                                ? registries->protocol_id(*entity_types, "minecraft:player")
                                : std::nullopt;
                        mobs->set_logic(*spawned,
                                        std::make_unique<gameplay::Mob>(
                                            *kind, state->width, state->height, state->network_id,
                                            player_type ? *player_type : gameplay::kNoQuarry));
                    } else {
                        mobs->set_logic(*spawned, std::make_unique<gameplay::FallingMob>());
                    }
                    const std::unique_lock lock{players_mutex, std::try_to_lock};
                    if (lock.owns_lock()) {
                        mob_packets(*state, [&](i32 id, std::span<const u8> payload) {
                            broadcast(nullptr, id, payload);
                        });
                    }
                }
                if (!spawn_requests.empty()) {
                    OV_LOG_DEBUG("tick {}: spawned {} over {} ticking chunks",
                                 clock.tick_count(), spawn_requests.size(), spawn_ticking.size());
                }
            }
            spawn_micros.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::steady_clock::now() - spawn_started)
                                       .count());
        }

        perf->enter(TickPhase::Entities);  // ── perf ──

        // ── mobs-2: the slimes a dead slime left, each half its size ──
        if (!slime_births.empty() && mobs && slimes && registries) {
            const auto entity_types = registries->find("minecraft:entity_type");
            const auto player_type =
                entity_types ? registries->protocol_id(*entity_types, "minecraft:player")
                             : std::nullopt;
            const gameplay::MobKind* slime_kind = gameplay::mob_kind("minecraft:slime");
            for (const gameplay::SlimeChild& child : slime_births) {
                const auto born = mobs->spawn("minecraft:slime", child.offset, net::Uuid{});
                if (!born || slime_kind == nullptr) {
                    continue;
                }
                entity::EntityState* state = mobs->mutable_state(*born);
                state->uuid                = uuid_for_entity(state->network_id);
                state->broadcast_position  = state->position;
                state->broadcast_valid     = true;
                slimes->set_size(*state, child.size);
                mobs->set_logic(*born, std::make_unique<gameplay::Mob>(
                                           *slime_kind, state->width, state->height,
                                           state->network_id,
                                           player_type ? *player_type : gameplay::kNoQuarry));
                const std::unique_lock lock{players_mutex, std::try_to_lock};
                if (lock.owns_lock()) {
                    mob_packets(*state, [&](i32 id, std::span<const u8> payload) {
                        broadcast(nullptr, id, payload);
                    });
                }
            }
            OV_LOG_DEBUG("{} slime(s) born of a division", slime_births.size());
            slime_births.clear();
        }
        // A zombie whose eyes stay in water becomes a drowned; a husk, a zombie.
        if (mobs && drowning && blocks && registries) {
            drowned_now.clear();
            {
                const std::scoped_lock chunk_lock{chunk_mutex};
                // ── perf ── resident chunks only: `block_at` would generate a
                // missing chunk on the tick thread (performance-tick.md § 5.4).
                const auto resident_block = [&](BlockPos at) -> registry::BlockStateId {
                    const world::Chunk* chunk = chunk_if_resident(at.x >> 4, at.z >> 4);
                    if (chunk == nullptr || !world::WorldShape::overworld().contains_y(at.y)) {
                        return registry::kAirState;
                    }
                    return chunk->get_block(static_cast<usize>(at.x & 15), at.y,
                                            static_cast<usize>(at.z & 15));
                };
                struct WaterContext {
                    const decltype(resident_block)* read;
                    const registry::BlockRegistry*  registry;
                };
                const WaterContext water_context{&resident_block, &*blocks};
                const auto         water = [](const void* context, BlockPos pos) -> bool {
                    const auto& in = *static_cast<const WaterContext*>(context);
                    const registry::BlockStateId state = (*in.read)({pos.x, pos.y, pos.z});
                    return in.registry->holds_fluid(state) &&
                           in.registry->block_name(in.registry->block_of(state)) ==
                               "minecraft:water";
                };
                drowning->tick(*mobs, water, &water_context, drowned_now);
            }
            for (const Drowning::Conversion& conversion : drowned_now) {
                entity::EntityState* old = mobs->mutable_state(conversion.handle);
                const gameplay::MobKind* kind = gameplay::mob_kind(conversion.to);
                if (old == nullptr || kind == nullptr) {
                    continue;
                }
                const auto born = mobs->spawn(conversion.to, old->position, net::Uuid{});
                if (!born) {
                    continue;
                }
                entity::EntityState* state = mobs->mutable_state(*born);
                old                        = mobs->mutable_state(conversion.handle);
                state->uuid                = uuid_for_entity(state->network_id);
                state->yaw                 = old->yaw;
                state->head_yaw            = old->head_yaw;
                state->broadcast_position  = state->position;
                state->broadcast_valid     = true;
                const auto entity_types = registries->find("minecraft:entity_type");
                const auto player_type =
                    entity_types ? registries->protocol_id(*entity_types, "minecraft:player")
                                 : std::nullopt;
                mobs->set_logic(*born, std::make_unique<gameplay::Mob>(
                                           *kind, state->width, state->height, state->network_id,
                                           player_type ? *player_type : gameplay::kNoQuarry));
                old->removed = true;
                const std::unique_lock lock{players_mutex, std::try_to_lock};
                if (lock.owns_lock()) {
                    mob_packets(*state, [&](i32 id, std::span<const u8> payload) {
                        broadcast(nullptr, id, payload);
                    });
                }
                OV_LOG_INFO("a mob drowned and became {}", conversion.to);
            }
        }
        // ── end mobs-2 ──

        // ── mobs-3: the mobs of chunks that have just arrived, from entities/,
        // and the Remove Entities of those an unload took to disk ──
        // ── persistence ── every ten ticks *since the last read*: the clock
        // swallows the ticks a slow server misses (piège 22), and a modulo it
        // steps over never reads a chunk — measured, 16 iterations and no read
        if (entity_storage && mobs && clock.tick_count() - mobs3_last_read >= 10) {
            std::unique_lock mobs3_lock{players_mutex, std::try_to_lock};
            if (mobs3_lock.owns_lock()) {
                mobs3_last_read = clock.tick_count();  // ── persistence ──
                const std::scoped_lock mobs3_chunks{chunk_mutex};
                for (const i32 gone : mobs3_unloaded) {
                    broadcast(nullptr, net::clientbound::kRemoveEntities,
                              net::encode_remove_entity(gone));
                    if (mob_combat) {
                        mob_combat->forget(gone);
                    }
                }
                mobs3_unloaded.clear();
                mobs3_to_load.clear();
                chunks.for_each([&](ChunkPos pos, const world::Chunk&) {
                    if (!entity_storage->is_loaded(pos)) {
                        mobs3_to_load.push_back(pos);
                    }
                });
                usize read = 0;
                for (const ChunkPos pos : mobs3_to_load) {
                    read += entity_storage->load_chunk(pos, *mobs, mob_records, entity_storage_host)
                                .entities;
                }
                if (!mobs3_to_load.empty()) {
                    OV_LOG_DEBUG("entities: {} mobs read from {} new chunks", read,
                                 mobs3_to_load.size());
                }
                // ── persistence ── the Nether's and the End's: what their levels
                // evicted goes to disk and leaves, what is resident comes back
                const auto persist_level = [&](DimensionEntities& level, NetherWorld& from,
                                               DimensionId dimension) {
                    persist_evicted.clear();
                    persist_removed.clear();
                    from.take_evicted(persist_evicted);
                    level.unload(persist_evicted, persist_removed);
                    if (dimension == DimensionId::Nether && nether_mobs) {
                        nether_mobs->forget(persist_removed);
                    }
                    for (const i32 gone : persist_removed) {
                        broadcast_in(dimension, nullptr, net::clientbound::kRemoveEntities,
                                     net::encode_remove_entity(gone));
                    }
                    (void)level.load_resident(from.chunks());
                };
                if (nether_entities && nether) {
                    persist_level(*nether_entities, *nether, DimensionId::Nether);
                }
                if (end_entities && end_world) {
                    persist_level(*end_entities, *end_world, DimensionId::End);
                    end_fight->set_arena_read(
                        end_entities->is_loaded(ChunkPos{-1, -1}) && end_entities->is_loaded(ChunkPos{0, -1}) &&
                        end_entities->is_loaded(ChunkPos{-1, 0}) && end_entities->is_loaded(ChunkPos{0, 0}));
                }
            }
        }
        // ── end mobs-3 ──
        // Mobs: gravity, collision, and only the movement that actually
        // happened. A delta packet when the move fits in one — six bytes rather
        // than twenty-eight — and a teleport when it does not.
        if (mobs && blocks &&
            (!mobs->handles().empty() || (tnt_gravity && tnt_gravity->has_pending()) ||
             (projectiles && projectiles->has_pending()) ||    // ── projectiles ──
             (rails_session && rails_session->has_pending()))) {  // ── rails ──
            std::unique_lock mob_lock{players_mutex, std::try_to_lock};
            if (mob_lock.owns_lock()) {
                const std::scoped_lock chunk_lock{chunk_mutex};
                // ── perf ── resident chunks only. `block_at` goes through
                // `chunk_at`, which *generates* a missing chunk — on this thread,
                // under `chunk_mutex`. A mob at the edge of the loaded area did
                // exactly that: 31 chunks generated on the tick thread in a
                // minute, ticks of 7 s, and every dig on the network thread
                // waiting behind the lock (docs/provenance/performance-tick.md
                // § 5.4). A chunk that is not here reads as air and not loaded,
                // the convention the fluid hooks already follow.
                const auto resident_block = [&](i32 bx, i32 by,
                                                i32 bz) -> registry::BlockStateId {
                    const world::Chunk* chunk = chunk_if_resident(bx >> 4, bz >> 4);
                    if (chunk == nullptr || !world::WorldShape::overworld().contains_y(by)) {
                        return registry::kAirState;
                    }
                    return chunk->get_block(static_cast<usize>(bx & 15), by,
                                            static_cast<usize>(bz & 15));
                };
                // ── end perf ──
                WorldView              view;
                view.read = [&](i32 bx, i32 by, i32 bz) { return resident_block(bx, by, bz); };
                const gameplay::CollisionWorld collisions{
                    *blocks, &WorldView::look_up, &view,
                    block_motion ? &*block_motion : nullptr};  // ── movement physics ──

                // The behaviour runs here, through the entity world, rather
                // than being applied to each state by hand: the whole point of
                // IEntityLogic is that a zombie and a dropped stack differ in
                // what they do, not in who calls them.
                // The blocks a goal reads. `CollisionWorld` answers boxes and a
                // LevelView answers states, and pathfinding needs the second:
                // it has to tell water from lava from a fence, and all three
                // are the same shape. Both read the same chunks through the
                // same lock, so this is an adapter and not a second world.
                struct MobLevel final : world::LevelView {
                    std::function<registry::BlockStateId(BlockPos)> read;
                    std::function<bool(BlockPos)>                   loaded;  // ── perf ──
                    const registry::BlockRegistry*                  registry{nullptr};

                    [[nodiscard]] registry::BlockStateId block_at(BlockPos pos) const override {
                        return read(pos);
                    }
                    // ── perf ── the truth, so a path is never planned into
                    // terrain that does not exist yet
                    [[nodiscard]] bool is_loaded(BlockPos pos) const override {
                        return loaded ? loaded(pos) : true;
                    }
                    [[nodiscard]] world::WorldShape shape() const override {
                        return world::WorldShape::overworld();
                    }
                    [[nodiscard]] world::DimensionTraits traits() const override { return {}; }
                    [[nodiscard]] const registry::BlockRegistry& blocks() const override {
                        return *registry;
                    }
                };
                MobLevel mob_level;
                mob_level.read = [&](BlockPos pos) {  // ── perf ── resident only
                    return resident_block(pos.x, pos.y, pos.z);
                };
                mob_level.loaded = [&](BlockPos pos) {  // ── perf ──
                    return chunk_if_resident(pos.x >> 4, pos.z >> 4) != nullptr;
                };
                mob_level.registry = &*blocks;

                gameplay::MobContext mob_context{&collisions, &mob_level, false};
                // ── mobs-3: the players hostile mobs may hunt — alive, in survival
                // or adventure, in the overworld; nobody on Peaceful ──
                const auto mobs3_difficulty =
                    commands ? static_cast<gameplay::Difficulty>(commands->world().difficulty)
                             : gameplay::Difficulty::Normal;
                mobs3_players.clear();
                for (const auto& [mobs3_key, who] : players) {  // despawn measures from these
                    if (who.connection && who.confirmed && who.game_mode != 3 &&
                        !who.survival.awaiting_respawn && who.dimension == DimensionId::Overworld) {
                        mobs3_players.push_back(Vec3d{who.x, who.y, who.z});
                    }
                }
                if (mob_attacks) {
                    mob_attacks->begin_tick();
                    for (const auto& [mobs3_key, who] : players) {
                        if (mobs3_difficulty != gameplay::Difficulty::Peaceful && who.connection &&
                            who.confirmed && who.mortal() && !who.survival.awaiting_respawn &&
                            !who.survival.health.dead && who.dimension == DimensionId::Overworld) {
                            mob_attacks->quarries().push_back(
                                gameplay::Quarry{.network_id = who.entity_id,
                                                 .type       = mob_attacks->player_type(),
                                                 .feet       = Vec3d{who.x, who.y, who.z}});
                        }
                    }
                    mob_context.quarries      = mob_attacks->quarries();
                    mob_context.attacks       = &mob_attacks->attacks();
                    mob_context.villager_type = mob_attacks->villager_type();
                }
                // ── end mobs-3 ──
                // ── tnt and gravity: what the drain and the network asked for ──
                if (tnt_gravity) {
                    tnt_gravity->spawn_pending(*mobs, tnt_deliver);
                }
                // ── projectiles: the shots asked for, the boxes they may hit,
                // and the skeletons (on normal: this server has no difficulty
                // setting of its own) ──
                if (projectiles) {
                    projectiles->spawn_pending(*mobs, projectile_deliver);
                    projectiles->before_entity_tick(*mobs, projectile_host);
                    projectiles->tick_skeletons(*mobs, collisions,
                                                static_cast<i32>(mobs3_difficulty),  // ── mobs-3 ──
                                                projectile_deliver);
                }
                // ── villagers: clicks, screens and the time of day, before ──
                if (villagers) {
                    const i64 day  = commands ? commands->world().day_time : 6000;
                    const i64 game = commands ? commands->world().game_time
                                              : static_cast<i64>(clock.tick_count());
                    (void)villagers->before_entity_tick(*mobs, mob_context, villager_host,
                                                        husbandry_deliver, day, game);
                }
                // ── husbandry: clicks and tempters before, births and eggs after ──
                if (husbandry) {
                    (void)husbandry->before_entity_tick(*mobs, mob_context, husbandry_host,
                                                       husbandry_deliver);
                }
                // ── tame: clicks, riders getting off, the owners the goals see ──
                if (taming) {
                    (void)taming->before_entity_tick(*mobs, mob_context, taming_host,
                                                    husbandry_deliver,
                                                    static_cast<i64>(clock.tick_count()));
                }
                // ── rails: shapes, placed carts, touches and riders' controls ──
                if (rails_session && level && world_ticks) {
                    rails_session->before_entity_tick(*mobs, *level, *world_ticks, rails_host);
                }
                mobs->tick(entity::TickContext{clock.tick_count(), &mob_context});
                // ── rails: detector and activator rails, riders carried ──
                if (rails_session && level && world_ticks) {
                    (void)rails_session->after_entity_tick(*mobs, *level, *world_ticks, rails_host);
                }
                if (husbandry) {
                    const HusbandryStats bred = husbandry->after_entity_tick(
                        *mobs, level ? &*level : nullptr, husbandry_host, husbandry_deliver);
                    if (bred.births + bred.eggs + bred.grazed + bred.grown > 0) {
                        OV_LOG_DEBUG("tick {}: {} born, {} grown, {} eggs, {} grazed",
                                     clock.tick_count(), bred.births, bred.grown, bred.eggs,
                                     bred.grazed);
                    }
                }
                // ── end husbandry ──
                // ── tame: tamed, thrown, teleported; what hurt an owner; the
                // riders steered and carried — before the swings resolve ──
                if (taming) {
                    const std::span<const gameplay::MobAttack> tame_swings =
                        mob_attacks ? std::span<const gameplay::MobAttack>{mob_attacks->attacks()}
                                    : std::span<const gameplay::MobAttack>{};
                    const TamingStats tamed = taming->after_entity_tick(
                        *mobs, tame_swings, taming_host, husbandry_deliver,
                        static_cast<i64>(clock.tick_count()));
                    if (tamed.tamed + tamed.thrown + tamed.teleported > 0) {
                        OV_LOG_DEBUG("tick {}: {} tamed, {} thrown, {} teleported",
                                     clock.tick_count(), tamed.tamed, tamed.thrown,
                                     tamed.teleported);
                    }
                }
                // ── villagers: what changed on a villager, told ──
                if (villagers) {
                    (void)villagers->after_entity_tick(*mobs, husbandry_deliver);
                }
                // ── mobs-3: the swings, finished; a mob they killed is told dead ──
                if (mob_attacks) {
                    mob_kills.clear();
                    const i64 mobs3_game = commands ? commands->world().game_time
                                                    : static_cast<i64>(clock.tick_count());
                    const i64 mobs3_day = commands ? commands->world().day_time : 18000;
                    const MobAttackStats swung =
                        mob_attacks->resolve(*mobs, mobs3_difficulty, mobs3_game, mobs3_day,
                                             mob_attack_host, tnt_deliver, mob_kills);
                    for (const MobKill& kill : mob_kills) {
                        // A villager a zombie killed may rise; otherwise it dies.
                        if (!(zombie_villagers &&
                              zombie_villagers->on_villager_killed(*mobs, kill, mobs3_difficulty,
                                                                   zombie_villager_host))) {
                            broadcast(nullptr, net::clientbound::kEntityEvent,
                                      net::encode_entity_event(kill.victim, 3));
                        }
                    }
                    if (swung.swings > 0) {
                        OV_LOG_DEBUG("tick {}: {} mob swings, {} on players, {} landed, {} kills",
                                     clock.tick_count(), swung.swings, swung.on_players,
                                     swung.landed, swung.kills);
                    }
                }
                // Despawn: every mob, every tick, against the nearest player.
                for (const i32 gone : mobs->removed_ids()) {
                    mob_records.forget(gone);
                }
                if (mob_despawn) {
                    const DespawnStats gone = mob_despawn->tick(*mobs, mob_records, mobs3_players,
                                                                mobs3_difficulty, mobs3_transient);
                    if (gone.immediate + gone.random + gone.peaceful > 0) {
                        OV_LOG_DEBUG("tick {}: despawned {} far, {} at random, {} peaceful",
                                     clock.tick_count(), gone.immediate, gone.random,
                                     gone.peaceful);
                    }
                }
                if (zombie_villagers) {  // clicks, Weakness, cures
                    const ZombieVillagerStats zv =
                        zombie_villagers->tick(*mobs, zombie_villager_host, tnt_deliver);
                    if (zv.cures_started + zv.cured > 0) {
                        OV_LOG_INFO("zombie villagers: {} cures started, {} cured",
                                    zv.cures_started, zv.cured);
                    }
                }
                // ── end mobs-3 ──
                // ── tnt and gravity: creepers, landings, explosions ──
                if (tnt_gravity && level && world_ticks) {
                    tnt_gravity->tick_creepers(*mobs, mob_level, tnt_host, tnt_deliver);
                    const TntGravityStats tnt_stats = tnt_gravity->after_entity_tick(
                        *mobs, *level, *world_ticks, tnt_host, tnt_deliver);
                    if (tnt_stats.blasts > 0) {
                        OV_LOG_DEBUG("tick {}: {} explosions, {} blocks, {} TNT lit",
                                     clock.tick_count(), tnt_stats.blasts,
                                     tnt_stats.blocks_destroyed, tnt_stats.primed);
                    }
                }
                // ── projectiles: hits, landings, pickups ──
                if (projectiles) {
                    const ProjectileStats shots =
                        projectiles->after_entity_tick(*mobs, projectile_host, projectile_deliver);
                    if (shots.hits + shots.stuck + shots.broke + shots.picked_up > 0) {
                        OV_LOG_DEBUG("tick {}: projectiles hit {}, stuck {}, broke {}, picked {}",
                                     clock.tick_count(), shots.hits, shots.stuck, shots.broke,
                                     shots.picked_up);
                    }
                }
                // ── brewing: the lingering clouds ──
                if (brewing) {
                    brewing->tick_clouds(potion_host);
                }
                // ── fire: burning mobs, zombies in the sun ──
                if (fire_session) {
                    (void)fire_session->tick_mobs(*mobs, fire_mob_host);
                }

                for (const i32 gone : mobs->removed_ids()) {
                    broadcast(nullptr, net::clientbound::kRemoveEntities,
                              net::encode_remove_entity(gone));
                    // ── combat and interaction ──────────────────────────────
                    // Its damage window goes with it. Without this a long
                    // server keeps one row per mob that ever lived.
                    if (mob_combat) {
                        mob_combat->forget(gone);
                    }
                    // ── end combat and interaction ──────────────────────────
                }

                for (const entity::EntityHandle handle : mobs->handles()) {
                    entity::EntityState* state = mobs->mutable_state(handle);
                    if (state == nullptr) {
                        continue;
                    }
                    if (!state->broadcast_valid) {
                        state->broadcast_position = state->position;
                        state->broadcast_valid    = true;
                    }
                    // Against what the client has, not against where the mob
                    // was. The wire quantises to 1/4096 and a delta taken from
                    // the true position throws the remainder away every tick:
                    // measured on this server, nine ticks of a fall already put
                    // the client 0.000244 out, and nothing ever corrects it.
                    const f64 dx = state->position.x - state->broadcast_position.x;
                    const f64 dy = state->position.y - state->broadcast_position.y;
                    const f64 dz = state->position.z - state->broadcast_position.z;
                    // Below half a quantum the packet would carry a zero, and
                    // broadcasting that twenty times a second for a mob standing
                    // still is most of the traffic on a busy server.
                    constexpr f64 kHalfQuantum = 0.5 / 4096.0;
                    if (std::abs(dx) < kHalfQuantum && std::abs(dy) < kHalfQuantum &&
                        std::abs(dz) < kHalfQuantum) {
                        continue;
                    }
                    if (net::fits_in_delta(dx, dy, dz)) {
                        broadcast(nullptr, net::clientbound::kEntityPosition,
                                  net::encode_entity_position(state->network_id, dx, dy, dz,
                                                              state->on_ground));
                        // Advance by what was actually sent, so the remainder
                        // is carried into the next delta rather than lost.
                        state->broadcast_position.x += net::quantised_delta(dx);
                        state->broadcast_position.y += net::quantised_delta(dy);
                        state->broadcast_position.z += net::quantised_delta(dz);
                    } else {
                        broadcast(nullptr, net::clientbound::kEntityTeleport,
                                  net::encode_entity_teleport(
                                      state->network_id, state->position.x, state->position.y,
                                      state->position.z, state->yaw, state->pitch,
                                      state->on_ground));
                        // A teleport is absolute, so the client is exactly here.
                        state->broadcast_position = state->position;
                    }
                }
            }
        }
        // ── nether-2 ── The Nether's mobs, under both locks as the overworld's.
        if (nether_mobs && nether) {
            std::unique_lock nether_mob_lock{players_mutex, std::try_to_lock};
            if (nether_mob_lock.owns_lock()) {
                const std::scoped_lock chunk_lock{chunk_mutex};
                nether_mob_host.level      = nether_level ? &*nether_level : nullptr;
                const NetherMobStats ticked = nether_mobs->tick(clock.tick_count(), nether_mob_host);
                if (ticked.spawned + ticked.shots + ticked.barters + ticked.blasts > 0) {
                    OV_LOG_DEBUG("nether tick {}: {} spawned, {} alive, {} shots, {} barters, "
                                 "{} blasts", clock.tick_count(), ticked.spawned, ticked.alive,
                                 ticked.shots, ticked.barters, ticked.blasts);
                }
            }
        }
        // ── end nether-2 ──
        // ── tnt and gravity: the craters and landings, sent and relit ──
        flush_tick_writes();

        perf->enter(TickPhase::GroundItems);  // ── perf ──
        // Les piles au sol : elles se ramassent, et au bout de cinq minutes
        // elles s'en vont. Sans cette seconde moitié un monde de test finit
        // par porter des milliers d'entités que personne ne voit passer.
        {
            std::unique_lock item_lock{players_mutex, std::try_to_lock};
            if (item_lock.owns_lock() && !ground_items.empty()) {
                const i64 now = clock.tick_count();
                for (usize i = ground_items.size(); i-- > 0;) {
                    ItemEntity& item = ground_items[i];
                    if (item.pickup_delay > 0) {
                        --item.pickup_delay;
                        continue;
                    }

                    Player* taker = nullptr;
                    for (auto& [key, candidate] : players) {
                        if (candidate.dimension != item.dimension) {  // ── nether ──
                            continue;
                        }
                        const f64 dx = candidate.x - item.x;
                        const f64 dy = candidate.y - item.y;
                        const f64 dz = candidate.z - item.z;
                        // A box rather than a sphere, and taller than it is
                        // wide: the player's feet are the reported position,
                        // and an item at their head still counts.
                        if (std::abs(dx) < 1.2 && std::abs(dz) < 1.2 && dy > -1.2 && dy < 2.0) {
                            taker = &candidate;
                            break;
                        }
                    }

                    if (taker != nullptr) {
                        const i8 taken = give_to_player(*taker, item.stack);
                        if (taken > 0) {
                            broadcast(
                                nullptr, net::clientbound::kTakeItem,
                                net::encode_take_item(item.entity_id, taker->entity_id, taken));
                            item.stack.count = static_cast<i8>(item.stack.count - taken);
                        }
                        if (item.stack.count > 0) {
                            // Inventory full: the stack stays where it is, and
                            // whatever fit has already gone. Dropping the rest
                            // would be a quiet loss.
                            continue;
                        }
                    } else if (now - item.born < 6000) {
                        continue;
                    }

                    broadcast(nullptr, net::clientbound::kRemoveEntities,
                              net::encode_remove_entity(item.entity_id));
                    ground_items.erase(ground_items.begin() + static_cast<isize>(i));
                }
            }
        }

        perf->enter(TickPhase::Players);  // ── perf ──
        // ── combat and interaction: the gauge, and the eating ──────────────
        //
        // One block, and it delegates like the survival one does. Two things
        // happen here that cannot happen on the network thread: the attack
        // gauge advances by one tick, and an eat that finishes on this tick is
        // applied — nutrition into hunger and one off the stack.
        //
        // Not gated on `--survival`: a creative player's gauge still charges,
        // and a swing sent while it is cold still does a fifth of its damage.
        // Only the eating is a survival rule, and `begin_use` already refuses
        // to start one for a player who is not hungry.
        {
            std::unique_lock combat_lock{players_mutex, std::try_to_lock};
            if (combat_lock.owns_lock()) {
                // Before any damage, and on the same thread that will apply it:
                // `tick_health` counts the gap between two hits in these calls,
                // so a window ticked after a hit is a window one tick young.
                if (mob_combat) {
                    mob_combat->tick(mob_damage_constants);
                }

                for (auto& [combat_key, who] : players) {
                    if (!who.confirmed || !who.connection) {
                        continue;
                    }
                    // How far they moved since the last tick. The sweep wants a
                    // *standing* attacker and this is the only place that can
                    // measure it — a position packet knows where the player is,
                    // not how fast.
                    const f64 step =
                        who.combat_last_valid
                            ? std::sqrt((who.x - who.combat_last_x) * (who.x - who.combat_last_x) +
                                        (who.z - who.combat_last_z) * (who.z - who.combat_last_z))
                            : 0.0;
                    who.combat_last_x     = who.x;
                    who.combat_last_z     = who.z;
                    who.combat_last_valid = true;

                    const i32              using_left = who.combat.use.remaining;  // ── sound ──
                    const std::string_view using_item = who.combat.use.item;
                    const std::string_view finished = who.combat.tick(combat_view(who), step);
                    if (sounds && who.connection && gameplay::food_for(using_item)) {  // ── sound ──
                        const Vec3d feet{who.x, who.y, who.z};
                        // Seven mouthfuls in a 32-tick use, as captured: the last
                        // one falls on the very tick the use finishes, before the
                        // burp. Playing it only while the use was still active
                        // gave six, which the comparison caught.
                        if (using_left > 0 && using_left <= 25 && using_left % 4 == 1) {
                            sounds->eating(sound_host, who.connection.get(), feet);
                        }
                        if (!finished.empty()) {
                            sounds->ate(sound_host, feet);
                        }
                    }
                    if (finished.empty()) {
                        continue;
                    }

                    // The item's use has just completed. What it *does* is the
                    // caller's, which is this: `tick_use` says when, and
                    // `food_for` says what.
                    const auto value = gameplay::food_for(finished);
                    // ── effects: milk is not food; it clears, and the bucket
                    // comes back empty ──────────────────────────────────────
                    if (!value && finished == "minecraft:milk_bucket") {
                        (void)who.effects.on_consumed(finished, who.survival, effect_io_for(who),
                                                      effect_bearer_for(who));
                        if (who.mortal() && registries && item_registry) {  // ── commands ──
                            const usize slot = 36 + static_cast<usize>(who.held_slot);
                            if (const auto bucket =
                                    registries->protocol_id(*item_registry, "minecraft:bucket")) {
                                who.inventory[slot]         = net::ItemStack{};
                                who.inventory[slot].item_id = *bucket;
                                who.inventory[slot].count   = 1;
                                send_slot(who, slot);
                            }
                        }
                        continue;
                    }
                    // ── end effects ─────────────────────────────────────────
                    // ── brewing: a potion is drunk, and leaves its bottle ──
                    if (!value && finished == "minecraft:potion" && registries) {
                        const usize slot = 36 + static_cast<usize>(who.held_slot);
                        const DrinkOutcome drunk =
                            drink_potion(*registries, who.inventory[slot], !who.mortal(),
                                         who.effects, who.survival, effect_io_for(who),
                                         effect_bearer_for(who));
                        if (drunk.give_bottle && item_registry) {
                            (void)give_to_player(
                                who, net::ItemStack{registries->protocol_id(*item_registry,
                                                                            "minecraft:glass_bottle")
                                                        .value_or(0),
                                                    1,
                                                    {}});
                        }
                        send_slot(who, slot);
                        continue;
                    }
                    if (!value) {
                        // Refused and named. A potion, a milk bucket and a
                        // chorus fruit all finish a use and none of them is
                        // food; treating them as nutrition zero would look
                        // exactly like eating that did nothing.
                        OV_LOG_DEBUG("{} finished using {}, which this server does not apply yet",
                                     who.name, finished);
                        continue;
                    }
                    gameplay::eat(who.survival.food, *value);
                    // ── effects: the food's own, and honey's cure — after the
                    // nutrition, in the game's order ──
                    (void)who.effects.on_consumed(finished, who.survival, effect_io_for(who),
                                                  effect_bearer_for(who));
                    // ── brewing: a suspicious stew's effects are in its NBT ──
                    const bool stew = finished == "minecraft:suspicious_stew";
                    if (stew) {
                        (void)eat_stew(who.inventory[36 + static_cast<usize>(who.held_slot)],
                                       who.effects, who.survival, effect_io_for(who),
                                       effect_bearer_for(who));
                    }
                    if (who.mortal()) {  // ── commands: per player ──
                        consume_one_held(who);
                    }
                    if (stew && who.mortal() && registries && item_registry) {  // ── brewing ──
                        // The bowl comes back into the hand (`stew` campaign).
                        const usize slot = 36 + static_cast<usize>(who.held_slot);
                        if (who.inventory[slot].empty()) {
                            who.inventory[slot] = net::ItemStack{
                                registries->protocol_id(*item_registry, "minecraft:bowl").value_or(0),
                                1,
                                {}};
                            send_slot(who, slot);
                        }
                    }
                    who.survival.send_state(SurvivalIo{
                        .send =
                            [&](i32 id, std::span<const u8> payload) {
                                if (const auto framed = net::encode_packet(id, payload);
                                    framed && who.connection) {
                                    who.connection->send(*framed);
                                }
                            },
                        .broadcast = [&](i32 id, std::span<const u8> payload) {
                            broadcast(who.connection.get(), id, payload);
                        }});
                    OV_LOG_INFO("{} ate {} (+{} food, hunger now {})", who.name, finished,
                                value->nutrition, who.survival.food.food);
                }
            }
        }
        // ── end combat and interaction ─────────────────────────────────────

        // ── effects: every effect ticks, and its packets go out ─────────────
        //
        // Before the survival block. Survival's `tick_health` counts the
        // window down after this; the gap between two effect hits is then N
        // decrements for N ticks, exactly as in the game, where the measured
        // gap is ten (effets.md). Not gated on --survival: a creative
        // player's effects still run out, they just cannot hurt.
        {
            std::unique_lock effects_lock{players_mutex, std::try_to_lock};
            if (effects_lock.owns_lock()) {
                for (auto& [effects_key, who] : players) {
                    if (!who.confirmed || !who.connection) {
                        continue;
                    }
                    if (!who.effects_started) {
                        who.effects_started = true;
                        for (const std::string& spec : options.effects) {
                            const auto instance = ov::server::parse_effect_spec(spec);
                            if (!instance) {
                                OV_LOG_WARN("--effect={}: not an effect this server knows", spec);
                                continue;
                            }
                            (void)who.effects.apply(*instance, who.survival, effect_io_for(who),
                                                    effect_bearer_for(who));
                        }
                    }
                    who.effects.tick(who.survival, effect_io_for(who), effect_bearer_for(who));
                }
            }
        }
        // ── end effects ─────────────────────────────────────────────────────

        // ── survival: health, hunger, experience, death and respawn ────────
        //
        // One block, and it delegates: SurvivalSession owns the state and the
        // packets, ov_gameplay owns the rules. What is here is the part that
        // needs the server — the inventory to drop, the orbs to place, and the
        // teleport that puts a respawned player back on the ground.
        {  // ── commands: every player, by their own game mode ──
            std::unique_lock survival_lock{players_mutex, std::try_to_lock};
            if (survival_lock.owns_lock()) {
                std::vector<ItemEntity> death_drops;
                for (auto& [survival_key, who] : players) {
                    if (!who.confirmed || !who.connection) {
                        continue;
                    }
                    const SurvivalIo io{
                        .send      = [&](i32 id, std::span<const u8> payload) {
                            if (const auto framed = net::encode_packet(id, payload)) {
                                who.connection->send(*framed);
                            }
                        },
                        .broadcast = [&](i32 id, std::span<const u8> payload) {
                            broadcast(who.connection.get(), id, payload);
                        }};
                    // Are their eyes under water? Vanilla measures the block
                    // at eye height, not at the feet: standing waist-deep is
                    // not drowning, and using the feet would suffocate anyone
                    // who waded in. 1.62 is the player's eye height.
                    bool submerged = false;
                    if (blocks) {
                        const std::scoped_lock water_lock{chunk_mutex};
                        const auto             eyes = net::WirePosition{
                            static_cast<i32>(std::floor(who.x)),
                            static_cast<i32>(std::floor(who.y + 1.62)),
                            static_cast<i32>(std::floor(who.z))};
                        const std::string_view name = blocks->block_name(
                            blocks->block_of(block_at_in(who.dimension, eyes)));  // ── nether ──
                        submerged = name == "minecraft:water";
                    }
                    const SurvivalPlayer view{.entity_id = who.entity_id,
                                              .name      = who.name,
                                              .x         = who.x,
                                              .y         = who.y,
                                              .z         = who.z,
                                              .on_ground = who.on_ground,
                                              // ── commands: per player; the
                                              // session spares creative and
                                              // spectator itself ──
                                              .game_mode = who.game_mode,
                                              .submerged = submerged,
                                              // ── effects ──
                                              .breathes_underwater =
                                                  who.effects.breathes_underwater(),
                                              .jump_boost   = who.effects.jump_boost(),
                                              .slow_falling = who.effects.slow_falling(),
                                              // ── enchanting ──
                                              .respiration_saves =
                                                  submerged && who.respiration_level > 0 &&
                                                  gameplay::respiration_saves_air(
                                                      who.respiration_level, who.enchant_random)};
                    // ── enchanting ── the worn pieces' EPF, recomputed only
                    // when one of them changed: no NBT is parsed on a quiet tick.
                    {
                        bool changed = false;
                        for (usize piece = 0; piece < 4; ++piece) {
                            changed = changed || who.worn_seen[piece] != who.inventory[5 + piece].nbt;
                        }
                        if (changed) {
                            for (usize piece = 0; piece < 4; ++piece) {
                                who.worn_seen[piece] = who.inventory[5 + piece].nbt;
                            }
                            const auto worn = worn_enchantments(who.inventory);
                            who.respiration_level =
                                worn[0].level(gameplay::Enchantment::Respiration);
                            for (usize kind = 0; kind < gameplay::kDamageKindCount; ++kind) {
                                who.survival.mitigation.protection[kind] =
                                    static_cast<u8>(std::min(
                                        255, gameplay::total_epf(
                                                 worn, static_cast<gameplay::DamageKind>(kind))));
                            }
                        }
                    }
                    // ── mobs-3 ── the worn armour's points, every tick: four lookups
                    if (registries) {
                        const WornArmour mobs3_worn = worn_armour(
                            *registries, item_registry,
                            std::span<const net::ItemStack>{who.inventory}.subspan(5, 4));
                        who.survival.mitigation.armour    = mobs3_worn.armour;
                        who.survival.mitigation.toughness = mobs3_worn.toughness;
                    }
                    // ── fire ── the player's counter, before the survival tick
                    if (fire_session) {
                        gameplay::FireContact touching;
                        {
                            const std::scoped_lock fire_lock{chunk_mutex};
                            touching = fire_session->contact(Vec3d{who.x, who.y, who.z}, 0.6, 1.8);
                        }
                        fire_session->tick_player(
                            who.survival.fire, who.survival.fire_flag_sent, touching, who.mortal(),
                            who.effects.effects.has(gameplay::Effect::FireResistance),
                            FirePlayerIo{
                                .hurt =
                                    [&](gameplay::DamageKind kind, f32 amount) {
                                        return who.survival
                                            .hurt(kind, amount, io, who.entity_id,
                                                  &fire_session->damage_window())
                                            .applied;
                                    },
                                .flag =
                                    [&](bool) {
                                        const auto& active = who.effects.effects;
                                        const u8    bits   = static_cast<u8>(
                                            effect_bearer_for(who).shared_flags |
                                            (active.has(gameplay::Effect::Invisibility) ? 0x20 : 0) |
                                            (active.has(gameplay::Effect::Glowing) ? 0x40 : 0));
                                        net::MetadataWriter fields;
                                        fields.byte_value(net::metadata::kSharedFlags,
                                                          static_cast<i8>(bits));
                                        const auto payload =
                                            net::encode_entity_metadata(who.entity_id, fields.take());
                                        io.send(net::clientbound::kEntityMetadata, payload);
                                        io.broadcast(net::clientbound::kEntityMetadata, payload);
                                    }});
                    }
                    SurvivalOutcome outcome = who.survival.tick(
                        view, io,
                        // ── commands: /difficulty and naturalRegeneration ──
                        commands ? static_cast<gameplay::Difficulty>(commands->world().difficulty)
                                 : gameplay::Difficulty::Normal,
                        !commands || commands->world().rules.flag("naturalRegeneration"),
                        // ── nether ── The void is under the player's own level.
                        static_cast<f64>(dimension_info(who.dimension).shape.min_y));

                    if (sounds && who.survival.last_fall > 0.0F) {  // ── sound ── the landing
                        registry::BlockStateId under{};
                        {
                            const std::scoped_lock chunk_lock{chunk_mutex};
                            under = block_at(net::WirePosition{
                                static_cast<i32>(std::floor(who.x)),
                                static_cast<i32>(std::floor(who.y - 0.2)),
                                static_cast<i32>(std::floor(who.z))});
                        }
                        sounds->fall(sound_host, who.connection.get(), Vec3d{who.x, who.y, who.z},
                                     under, who.survival.last_fall);
                    }

                    // ── end ── The credits are over: home, keeping everything —
                    // the respawn a win is, not a death's.
                    if (who.wants_respawn && who.won_game) {
                        who.wants_respawn = false;
                        who.won_game      = false;
                        const DimensionId from = who.dimension;
                        {
                            const std::scoped_lock ticket_lock{chunk_mutex};
                            world::LevelChanges    changes;
                            view_of(from).chunks->remove_ticket(world::TicketType::Player,
                                                                static_cast<u64>(who.entity_id));
                            view_of(from).chunks->refresh(changes);
                        }
                        Vec3d home{static_cast<f64>(level_settings.spawn_x) + 0.5,
                                   static_cast<f64>(level_settings.spawn_y),
                                   static_cast<f64>(level_settings.spawn_z) + 0.5};
                        if (const auto own =
                                commands ? commands->personal_spawn(who.uuid) : std::nullopt) {
                            home = Vec3d{static_cast<f64>(own->x) + 0.5, static_cast<f64>(own->y),
                                         static_cast<f64>(own->z) + 0.5};
                        }
                        arrive_in(who, from, DimensionId::Overworld, home, who.yaw,
                                  kEndExitDataKept);
                    }
                    if (who.wants_respawn) {
                        who.wants_respawn = false;
                        // ── nether ── The world spawn is in the overworld: a
                        // player who died in the Nether comes back there. The
                        // Respawn packet the session sends names the overworld.
                        if (who.dimension != DimensionId::Overworld) {
                            const std::scoped_lock ticket_lock{chunk_mutex};
                            world::LevelChanges    changes;
                            view_of(who.dimension)
                                .chunks->remove_ticket(world::TicketType::Player,
                                                       static_cast<u64>(who.entity_id));
                            view_of(who.dimension).chunks->refresh(changes);
                            who.dimension = DimensionId::Overworld;
                        }
                        // ── commands: /spawnpoint's, else /setworldspawn's ──
                        who.survival.spawn = SurvivalSession::SpawnPoint{
                            static_cast<f64>(level_settings.spawn_x) + 0.5,
                            static_cast<f64>(level_settings.spawn_y),
                            static_cast<f64>(level_settings.spawn_z) + 0.5, false};
                        if (const auto own =
                                commands ? commands->personal_spawn(who.uuid) : std::nullopt) {
                            who.survival.spawn = SurvivalSession::SpawnPoint{
                                static_cast<f64>(own->x) + 0.5, static_cast<f64>(own->y),
                                static_cast<f64>(own->z) + 0.5, false};
                        }
                        // ── weather: a bed's respawn point needs its bed ──
                        bool no_respawn_bed = false;
                        if (const auto own = commands ? commands->personal_spawn(who.uuid) : std::nullopt;
                            own && weather && level &&
                            weather->spawn_is_bed(who.uuid, BlockPos{own->x, own->y, own->z})) {
                            std::optional<Vec3d> stand;
                            {
                                const std::scoped_lock chunk_lock{chunk_mutex};
                                stand = weather->bed_respawn(*level, BlockPos{own->x, own->y, own->z},
                                                             own->angle);
                            }
                            if (stand) {
                                who.survival.spawn =
                                    SurvivalSession::SpawnPoint{stand->x, stand->y, stand->z, true};
                            } else {
                                no_respawn_bed = true;
                                commands->clear_personal_spawn(who.uuid);
                                who.survival.spawn = SurvivalSession::SpawnPoint{
                                    static_cast<f64>(level_settings.spawn_x) + 0.5,
                                    static_cast<f64>(level_settings.spawn_y),
                                    static_cast<f64>(level_settings.spawn_z) + 0.5, false};
                            }
                        }
                        if (who.survival.perform_respawn(view, io, outcome, 0)) {
                            if (no_respawn_bed) {  // ── weather: Game Event 0 ──
                                io.send(net::clientbound::kGameEvent, net::encode_game_event(0, 0.0F));
                            }
                            who.x = outcome.respawn_x;
                            who.y = outcome.respawn_y;
                            who.z = outcome.respawn_z;
                            who.pending_teleport = who.entity_id * 1000 + 7;
                            io.send(net::clientbound::kSynchronizePosition,
                                    net::encode_synchronize_position(who.x, who.y, who.z, who.yaw,
                                                                     who.pitch,
                                                                     who.pending_teleport));
                            // A Respawn packet makes the client throw its
                            // world away, so the server has to forget what it
                            // thinks the client holds. Without this the
                            // difference-based streaming sends nothing at all
                            // and the player comes back standing in the void.
                            who.loaded_chunks.clear();
                            who.pending_chunks.clear();
                            who.streaming       = false;
                            who.broadcast_valid = false;
                            stream_chunks(who.connection, who);
                            broadcast(who.connection.get(), net::clientbound::kEntityTeleport,
                                      net::encode_entity_teleport(who.entity_id, who.x, who.y,
                                                                  who.z, who.yaw, who.pitch,
                                                                  true));
                        }
                    }

                    if (!outcome.died) {
                        continue;
                    }
                    // ── end ── The Respawn's death location names this level.
                    who.survival.death_dimension = std::string{dimension_info(who.dimension).name};
                    // ── effects: gone with the body; Respawn tells the client ──
                    who.effects.on_death();
                    OV_LOG_INFO("{} died at {:.1f} {:.1f} {:.1f}, dropping {} experience",
                                who.name, who.x, who.y, who.z, outcome.dropped_experience);
                    // The inventory goes on the floor, as vanilla does with
                    // keepInventory off. Cleared first, so a client that
                    // reconnects before the tick finishes cannot be handed the
                    // same stacks twice.
                    const bool keep_inventory =  // ── commands ──
                        commands && commands->world().rules.flag("keepInventory");
                    for (net::ItemStack& stack : who.inventory) {
                        if (keep_inventory || stack.item_id == 0 || stack.count <= 0) {
                            continue;
                        }
                        if (vanishes_on_death(stack)) {  // ── enchanting ──
                            stack = net::ItemStack{};
                            continue;
                        }
                        ItemEntity item;
                        item.entity_id = next_entity_id.fetch_add(1);
                        item.uuid = net::Uuid{0x4f564954454d0000ULL | static_cast<u64>(item.entity_id),
                                              static_cast<u64>(item.entity_id) *
                                                  0x9E3779B97F4A7C15ULL};
                        item.x     = who.x;
                        item.y     = who.y + 1.0;
                        item.z     = who.z;
                        item.stack = stack;
                        item.born  = clock.tick_count();
                        item.dimension = who.dimension;  // ── persistence ──
                        death_drops.push_back(std::move(item));
                        stack = net::ItemStack{};
                    }
                    // The orbs, at the same place. Their own packet: an orb
                    // carries a value rather than a type, so Spawn Entity
                    // cannot express one.
                    for (usize orb = 0; orb < outcome.orb_count; ++orb) {
                        GroundOrb dropped;
                        dropped.entity_id = next_entity_id.fetch_add(1);
                        dropped.x         = who.x;
                        dropped.y         = who.y + 0.5;
                        dropped.z         = who.z;
                        dropped.value     = outcome.orbs[orb];
                        dropped.born      = clock.tick_count();
                        dropped.dimension = who.dimension;  // ── persistence ──
                        // Half a second before the player who died can walk back
                        // into their own experience, the same grace a dropped
                        // stack gets.
                        dropped.delay = 10;
                        broadcast(nullptr, net::clientbound::kSpawnExperienceOrb,
                                  net::encode_spawn_experience_orb(
                                      dropped.entity_id, dropped.x, dropped.y, dropped.z,
                                      static_cast<i16>(dropped.value)));
                        ground_orbs.push_back(dropped);
                    }
                }
                if (!death_drops.empty()) {
                    // publish_items moves each stack into ground_items itself.
                    publish_items(death_drops);
                }

                // The orbs on the ground: they merge, they chase, and they are
                // collected. Without this an orb is a packet the client draws
                // forever and nobody can ever pick up.
                const gameplay::OrbConstants orb_rules{};
                const i64                    now = clock.tick_count();
                for (usize i = ground_orbs.size(); i-- > 0;) {
                    GroundOrb& orb = ground_orbs[i];
                    if (orb.delay > 0) {
                        --orb.delay;
                        continue;
                    }

                    // Merge with a neighbour, oldest first so the survivor keeps
                    // the earlier birthday and the pile still expires.
                    bool merged = false;
                    for (usize j = 0; j < i; ++j) {
                        GroundOrb& other = ground_orbs[j];
                        const f64  dx    = other.x - orb.x;
                        const f64  dy    = other.y - orb.y;
                        const f64  dz    = other.z - orb.z;
                        const gameplay::OrbState a{orb.value,
                                                   static_cast<i32>(now - orb.born), orb.delay};
                        const gameplay::OrbState b{other.value,
                                                   static_cast<i32>(now - other.born),
                                                   other.delay};
                        if (other.dimension == orb.dimension &&  // ── persistence ──
                            gameplay::orbs_can_merge(a, b, dx * dx + dy * dy + dz * dz)) {
                            other.value += orb.value;
                            other.born = std::min(other.born, orb.born);
                            broadcast(nullptr, net::clientbound::kRemoveEntities,
                                      net::encode_remove_entity(orb.entity_id));
                            ground_orbs.erase(ground_orbs.begin() + static_cast<isize>(i));
                            merged = true;
                            break;
                        }
                    }
                    if (merged) {
                        continue;
                    }

                    Player* taker  = nullptr;
                    f64     nearest = orb_rules.follow_range * orb_rules.follow_range;
                    for (auto& [orb_key, candidate] : players) {
                        if (!candidate.confirmed || candidate.survival.awaiting_respawn ||
                            candidate.dimension != orb.dimension) {  // ── persistence ──
                            continue;
                        }
                        const f64 dx = candidate.x - orb.x;
                        const f64 dy = candidate.y + 0.9 - orb.y;
                        const f64 dz = candidate.z - orb.z;
                        const f64 d2 = dx * dx + dy * dy + dz * dz;
                        if (d2 < nearest) {
                            nearest = d2;
                            taker   = &candidate;
                        }
                    }

                    if (taker != nullptr) {
                        const f64 dx = taker->x - orb.x;
                        const f64 dy = taker->y + 0.9 - orb.y;
                        const f64 dz = taker->z - orb.z;
                        if (nearest <= orb_rules.pickup_range * orb_rules.pickup_range) {
                            const i32 level_before = taker->survival.experience_level;  // ── sound ──
                            // ── enchanting ── Mending takes its share first.
                            const i32 for_player = apply_mending(
                                taker->inventory, taker->held_slot, orb.value,
                                taker->enchant_random, [&](usize slot) {
                                    if (const auto framed = net::encode_packet(
                                            net::clientbound::kContainerSlot,
                                            net::encode_container_slot(
                                                0, 0, static_cast<i16>(slot), taker->inventory[slot]));
                                        framed && taker->connection) {
                                        taker->connection->send(*framed);
                                    }
                                });
                            taker->survival.award_experience(for_player);
                            if (sounds && taker->survival.experience_level > level_before) {
                                sounds->level_up(sound_host, Vec3d{taker->x, taker->y, taker->z},
                                                 taker->survival.experience_level);
                            }
                            broadcast(nullptr, net::clientbound::kTakeItem,
                                      net::encode_take_item(orb.entity_id, taker->entity_id, 1));
                            broadcast(nullptr, net::clientbound::kRemoveEntities,
                                      net::encode_remove_entity(orb.entity_id));
                            ground_orbs.erase(ground_orbs.begin() + static_cast<isize>(i));
                            continue;
                        }
                        // Otherwise it moves toward them, harder the closer it
                        // already is — which is what makes a pile of orbs
                        // converge rather than drift in at one speed.
                        const f64 distance = std::sqrt(nearest);
                        const f64 pull =
                            orb_rules.follow_speed * (1.0 - distance / orb_rules.follow_range);
                        orb.x += dx / distance * pull;
                        orb.y += dy / distance * pull;
                        orb.z += dz / distance * pull;
                        broadcast(nullptr, net::clientbound::kEntityTeleport,
                                  net::encode_entity_teleport(orb.entity_id, orb.x, orb.y, orb.z,
                                                              0.0F, 0.0F, false));
                        continue;
                    }

                    if (now - orb.born >= orb_rules.lifetime_ticks) {
                        broadcast(nullptr, net::clientbound::kRemoveEntities,
                                  net::encode_remove_entity(orb.entity_id));
                        ground_orbs.erase(ground_orbs.begin() + static_cast<isize>(i));
                    }
                }
            }
        }
        // ── end survival ───────────────────────────────────────────────────

        perf->enter(TickPhase::Digs);  // ── perf ──
        {  // ── breaking ── the cracks the others see (destroy_stages.hpp)
            std::unique_lock stage_lock{players_mutex, std::try_to_lock};
            if (stage_lock.owns_lock()) {
                for (auto& [key, digger] : players) {
                    const bool counting = digger.digging || digger.delayed_dig;
                    const net::WirePosition where{digger.dig_x, digger.dig_y, digger.dig_z};
                    // The start was recorded on the network thread a tick
                    // early, so the elapsed count already holds vanilla's +1.
                    const f32 count =
                        counting ? dig_progress_for(digger, where) *
                                       static_cast<f32>(clock.tick_count() - digger.dig_started_tick)
                                 : 0.0F;
                    if (const auto stage = next_destroy_stage(digger.destroy_stage, digger.entity_id,
                                                              counting, where, count)) {
                        sound_host.send_near(key, Vec3d{where.x + 0.5, where.y + 0.5, where.z + 0.5},
                                             kDestroyStageRadius,
                                             net::clientbound::kSetBlockDestroyStage,
                                             net::encode_block_destroy_stage(*stage));
                    }
                }
            }
        }
        {  // ── commands: a delayed dig only ever starts in survival ──
            std::unique_lock dig_lock{players_mutex, std::try_to_lock};
            if (dig_lock.owns_lock()) {
                for (auto& [key, digger] : players) {
                    if (!digger.delayed_dig) {
                        continue;
                    }
                    const net::WirePosition where{digger.dig_x, digger.dig_y, digger.dig_z};
                    const f32               progress = dig_progress_for(digger, where);
                    if (progress <= 0.0F) {
                        digger.delayed_dig = false;
                        continue;
                    }
                    // Vanilla writes this as `progress * (elapsed + 1)`, with
                    // its start recorded inside the same tick that will first
                    // test it. Ours is recorded on the network thread, one tick
                    // earlier, so the plus one is already in the count —
                    // keeping it would break every block a tick early, which is
                    // exactly what the first measurement against our own server
                    // showed: 149 where the real game takes 150.
                    const auto elapsed =
                        static_cast<f32>(clock.tick_count() - digger.dig_started_tick);
                    if (progress * elapsed >= 1.0F) {
                        digger.delayed_dig = false;
                        forget_destroy_stage(digger.destroy_stage);  // ── breaking ── no -1 here
                        std::vector<ItemEntity> dropped;
                        drop_loot(digger, where, dropped);
                        registry::BlockStateId broken{};  // ── sound ──
                        {
                            const std::scoped_lock chunk_lock{chunk_mutex};
                            broken = block_at_in(digger.dimension, where);
                        }
                        set_block_connected_in(digger.dimension, where, superflat.air.air);
                        if (sounds) {  // ── sound ── everyone but the digger
                            sounds->block_broken(sound_host, digger.connection.get(),
                                                 BlockPos{where.x, where.y, where.z}, broken);
                        }
                        publish_items(dropped);
                    }
                }
            }
        }

        for (i32 i = 0; i < ticks; ++i) {
            // The world tick lives here. Everything inside must be
            // deterministic, and must not allocate once running: in debug
            // builds this guard aborts on the first allocation, naming it.
            const NoAllocScope no_alloc{"server tick"};
        }

        perf->enter(TickPhase::Autosave);  // ── perf ──
        // Autosave. A clean shutdown saves too, but a server that is killed
        // never gets one — and losing an hour of building to a crash is the
        // failure people remember. Thirty seconds is short enough to matter and
        // long enough that a world with nothing dirty costs a map lookup.
        if (const auto now = std::chrono::steady_clock::now();
            now - last_autosave >= std::chrono::seconds{30}) {
            last_autosave = now;
            save_online_players();  // ── player data ── before level.dat, for the host
            {
                // ── persistence ── the ground items and the entity world are
                // the players' lock's, as when /save-all runs
                const std::scoped_lock persist_lock{players_mutex};
                save_world();
            }
        }

        perf->enter(TickPhase::ChunkSend);  // ── perf ──
        // Keep-alive. The client drops a server that goes quiet, and vanilla
        // sends one every fifteen seconds — ten leaves room for a slow link
        // without being chatty. Outside the no-alloc scope on purpose: this
        // builds a packet, and the tick body is where allocation is forbidden.
        {
            const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count();

            // try_lock, not lock. The packet handler holds this mutex while it
            // generates and sends chunks, which for a long jump is hundreds of
            // milliseconds — and a tick thread waiting behind that misses its
            // deadline and logs an overload. Measured: exactly that, during a
            // five-chunk jump.
            //
            // A keep-alive deferred by one tick is harmless; a tick blocked
            // behind the network is not. The real fix is the world moving onto
            // the tick thread — the single-writer rule the project is built on,
            // arriving with ov_sim. This keeps the two apart until then rather
            // than pretending they already are.
            //
            // The whole pass sits inside the check: skipping it with `continue`
            // would jump past the sleep at the bottom of the loop and spin the
            // tick thread exactly when the server is already busy, and walking
            // the map without the lock would be a race of its own.
            std::unique_lock lock{players_mutex, std::try_to_lock};
            if (lock.owns_lock()) {
                // The chunk queue, drained under a budget.
                //
                // Sending the whole square at once overran the tick and the
                // server said so on every join. Eight a tick is 160 a second,
                // so a view distance of 8 fills in under two seconds while the
                // tick clock stays inside its 50 ms.
                // Eight a tick is 160 a second, which fills a view distance of
                // eight in under two seconds — when the chunks exist. The
                // budget is unchanged; what changed is that a chunk which does
                // not exist yet is **asked for and left in the queue** instead
                // of generated here. Generating it here is what used to cost
                // three hundred milliseconds and print "can't keep up".
                constexpr usize kChunksPerTick = 8;
                std::array<usize, kChunksPerTick> sent_at{};
                for (auto& [key, player] : players) {
                    if (player.pending_chunks.empty() || !player.connection) {
                        continue;
                    }
                    usize sent = 0;
                    // Backwards: `pending_chunks` is sorted furthest-first, so
                    // the back is what the player is standing on. Requests go
                    // out in that order too, which is why the ground arrives
                    // before the horizon.
                    for (usize index = player.pending_chunks.size();
                         index-- > 0 && sent < kChunksPerTick;) {
                        const i64  chunk = player.pending_chunks[index];
                        const auto cx    = static_cast<i32>(chunk >> 32);
                        const auto cz    = static_cast<i32>(static_cast<u32>(chunk & 0xFFFFFFFF));
                        if (player.loaded_chunks.contains(chunk)) {
                            sent_at[sent] = index;
                            ++sent;
                            continue;
                        }

                        std::vector<u8> payload;
                        {
                            const std::scoped_lock chunk_lock{chunk_mutex};
                            // ── nether ── From the player's own level; the
                            // Nether always streams from its workers.
                            world::Chunk* ready =
                                chunk_if_resident_in(player.dimension, cx, cz);
                            if (ready == nullptr) {
                                if (chunk_source ||
                                    player.dimension != DimensionId::Overworld) {  // ── end ──
                                    // Not here yet. The fill pass above has
                                    // already asked for it; this loop's job is
                                    // to send, not to generate.
                                    continue;
                                }
                                // Superflat: a chunk costs microseconds, so
                                // there is nothing to move off this thread.
                                ready = &chunk_at(cx, cz);
                            }
                            payload = net::encode_chunk_data(*ready);
                        }
                        if (const auto framed =
                                net::encode_packet(net::clientbound::kChunkDataAndLight, payload)) {
                            player.connection->send(*framed);
                        }
                        player.loaded_chunks.insert(chunk);
                        sent_at[sent] = index;
                        ++sent;
                    }
                    // Erase back to front so the earlier indices stay valid.
                    // `sent_at` is filled in descending index order because the
                    // scan runs backwards.
                    for (usize i = 0; i < sent; ++i) {
                        player.pending_chunks.erase(
                            player.pending_chunks.begin() +
                            static_cast<std::ptrdiff_t>(sent_at[i]));
                    }
                }

                perf->enter(TickPhase::Screens);  // ── perf ──
                // ── crafting and smelting ───────────────────────────────────
                // The screens someone has open. What runs a furnace **nobody**
                // is watching is the pass below this one; this one exists
                // separately because an open screen also owes its viewer four
                // property packets and a resend, which a headless furnace does
                // not.
                for (auto& [key, player] : players) {
                    if (!player.bench || !player.connection) {
                        continue;
                    }
                    auto send = [&](i32 id, std::span<const u8> payload) {
                        if (const auto framed = net::encode_packet(id, payload)) {
                            player.connection->send(*framed);
                        }
                    };
                    const std::scoped_lock chunk_lock{chunk_mutex};
                    tick_open_workbench(workbench_context, workbench_host(player, send),
                                        *player.bench, player.inventory);
                }

                // ── Furnaces nobody is watching ─────────────────────────────
                //
                // A furnace is a **ticked block entity**: it cooks whether a
                // player is standing there or not, and one left burning with
                // eight ores in it finishes them while its owner is away. That
                // is the whole reason a furnace is worth building.
                //
                // The index rather than a scan every tick: walking every block
                // entity of every loaded chunk twenty times a second is a cost
                // that grows with the world and buys nothing, because a smelt
                // takes 200 ticks. It is rebuilt once a second, so a furnace
                // placed now starts cooking at most a second later — stated,
                // and invisible against a ten-second smelt.
                if (clock.tick_count() % 20 == 0) {
                    const std::scoped_lock chunk_lock{chunk_mutex};
                    furnace_index.clear();
                    chunks.for_each([&](ChunkPos pos, const world::Chunk& chunk) {
                        for (const world::BlockEntity& entity : chunk.block_entities()) {
                            const auto kind = workbench_of_block(entity.type);
                            if (!kind || !furnace_of(*kind)) {
                                continue;
                            }
                            // A block entity stores its position **local** to
                            // the chunk; the index holds world coordinates,
                            // because that is what the tick below looks blocks
                            // up by. Mixing the two conventions puts every
                            // furnace in a different chunk, silently.
                            furnace_index.push_back(net::WirePosition{
                                pos.x * 16 + static_cast<i32>(entity.x), entity.y,
                                pos.z * 16 + static_cast<i32>(entity.z)});
                        }
                    });
                }

                if (!furnace_index.empty() && registries) {
                    const std::scoped_lock chunk_lock{chunk_mutex};
                    for (const net::WirePosition& where : furnace_index) {
                        // Skip the ones a player has open. Those already tick
                        // above, holding their own copy of the state, and
                        // ticking the block entity underneath one would advance
                        // the same furnace twice a tick and show its viewer a
                        // bar that jumps.
                        bool watched = false;
                        for (const auto& [key, other] : players) {
                            if (other.bench && other.bench->x == where.x &&
                                other.bench->y == where.y && other.bench->z == where.z) {
                                watched = true;
                                break;
                            }
                        }
                        if (watched) {
                            continue;
                        }

                        world::Chunk* chunk = chunk_if_resident(where.x >> 4, where.z >> 4);
                        if (chunk == nullptr) {
                            continue;
                        }
                        world::BlockEntity* entity = chunk->block_entity_at(
                            static_cast<usize>(where.x & 15), where.y,
                            static_cast<usize>(where.z & 15));
                        if (entity == nullptr) {
                            continue;
                        }
                        const auto kind = workbench_of_block(entity->type);
                        if (!kind || !furnace_of(*kind)) {
                            // The block entity changed under the index. Not an
                            // error: the index is a second old by design.
                            continue;
                        }

                        // A scratch screen, reused. `Workbench` is what
                        // `tick_furnace` reads and writes, and building one per
                        // furnace per tick would allocate inside the tick.
                        headless_furnace           = Workbench{};
                        headless_furnace.kind      = *kind;
                        headless_furnace.window_id = 0;
                        headless_furnace.x         = where.x;
                        headless_furnace.y         = where.y;
                        headless_furnace.z         = where.z;
                        load_furnace(workbench_context, entity->data, headless_furnace);

                        // Nothing to do, and the common case by far: an empty
                        // furnace with a cold fire. Checked before the tick so
                        // that a world full of decorative furnaces costs a load
                        // and a comparison.
                        if (!headless_furnace.furnace_state.lit() &&
                            headless_furnace.furnace_slots.input.empty()) {
                            continue;
                        }

                        const gameplay::FurnaceTick step =
                            tick_furnace(workbench_context, headless_furnace);
                        if (step.slots_changed) {
                            store_furnace(workbench_context, entity->data, headless_furnace);
                            dirty_chunks.insert(chunk_key(where.x >> 4, where.z >> 4));
                        }
                        if (step.lit_changed && blocks) {
                            // The `lit` property, not a different block: looking
                            // `minecraft:furnace` up by name would find it
                            // either way and lose the facing it was placed
                            // with. Written straight into the chunk because
                            // `chunk_mutex` is already held here and is not
                            // recursive.
                            const registry::BlockStateId state = block_at(where);
                            const registry::BlockId      block = blocks->block_of(state);
                            const auto property = blocks->find_property(block, "lit");
                            if (!property) {
                                continue;
                            }
                            const auto wanted = headless_furnace.furnace_state.lit()
                                                    ? std::string_view{"true"}
                                                    : std::string_view{"false"};
                            for (u16 index = 0; index < property->values.size(); ++index) {
                                if (property->values[index] != wanted) {
                                    continue;
                                }
                                const registry::BlockStateId next =
                                    blocks->with_property(state, *property, index);
                                // `write_block`, never `chunk->set_block`: a
                                // furnace that lights would otherwise lose its
                                // block entity — its ore, its fuel and its
                                // stored experience — on the tick it catches.
                                write_block(*chunk, where.x, where.y, where.z, next);
                                dirty_chunks.insert(chunk_key(where.x >> 4, where.z >> 4));
                                if (const auto framed = net::encode_packet(
                                        net::clientbound::kBlockUpdate,
                                        net::encode_block_update(
                                            where, static_cast<i32>(next.value())))) {
                                    for (auto& [other_key, other] : players) {
                                        if (other.connection) {
                                            other.connection->send(*framed);
                                        }
                                    }
                                }
                                break;
                            }
                        }
                    }
                }

                for (auto& [key, player] : players) {
                    // One challenge in flight at a time, as vanilla. The
                    // version before this sent a fresh id every ten seconds
                    // whether or not the last had been answered, and the
                    // reply handler drops a connection whose id is not the
                    // current one: any client more than ten seconds behind —
                    // a loaded machine, a burst after a death — was cut off
                    // for answering correctly, while a client that never
                    // answered at all was never cut off. The protocol's rule
                    // (763): no answer for thirty seconds is a timeout.
                    if (player.awaiting_keep_alive) {  // ── keep-alive ──
                        if (now_ms - player.last_keep_alive_sent_ms >= 30000 &&
                            player.connection) {
                            OV_LOG_WARN("{} did not answer a keep-alive in 30 s — timed out",
                                        player.name);
                            if (const auto framed = net::encode_packet(
                                    net::clientbound::kDisconnect,
                                    net::encode_component_packet(
                                        R"({"translate":"disconnect.timeout"})"))) {
                                player.connection->send(*framed);
                            }
                            player.connection->close();
                        }
                        continue;
                    }
                    if (now_ms - player.last_keep_alive_sent_ms < 10000) {
                        continue;
                    }
                    player.last_keep_alive_sent_ms = now_ms;
                    player.keep_alive_id           = now_ms;
                    player.awaiting_keep_alive     = true;

                    if (const auto framed =
                            net::encode_packet(net::clientbound::kKeepAlive,
                                               net::encode_keep_alive(player.keep_alive_id));
                        framed && player.connection) {
                        player.connection->send(*framed);
                    }
                }
            }
        }

        perf->end_tick();  // ── perf ──
        tick_micros.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - tick_started)
                                  .count());

        if (clock.is_behind()) {
            ++behind_events;
            OV_LOG_WARN(
                "can't keep up — is the server overloaded? (running behind, dropped ticks)");
        }

        if (options.run_ticks >= 0 && clock.tick_count() >= options.run_ticks) {
            OV_LOG_INFO("reached --ticks={}, stopping", options.run_ticks);
            break;
        }

        // Sleep until the next tick is due rather than spinning. A spinning
        // tick thread on a laptop is a battery and thermal problem, and on a
        // shared host it steals time from the workers.
        if (const Duration idle = clock.time_until_next_tick(); idle > Duration::zero()) {
            std::this_thread::sleep_for(idle);
        }
    }

    if constexpr (kAllocationTrackingEnabled) {
        const auto stats = allocation_stats();
        OV_LOG_INFO("allocations: {} ({} bytes), tick violations: {}", stats.allocations,
                    stats.bytes, stats.violations);
    }
    // ── persistence ── as at a leave: a player sitting in a cart takes it with
    // them into their file (RootVehicle), and the world is saved without it
    {
        const std::scoped_lock persist_lock{players_mutex};
        for (const auto& [key, who] : players) {
            if (rails_session && mobs && who.connection) {
                if (auto taken = rails_session->take_vehicle(*mobs, who.entity_id)) {
                    leaving_vehicles[who.entity_id] = std::move(taken->root_vehicle);
                }
            }
        }
    }
    save_online_players();  // ── player data ──
    {
        const std::scoped_lock persist_lock{players_mutex};  // ── persistence ──
        save_world();
    }

    listener->stop();
    network_thread.join();

    if (!tick_micros.empty()) {
        std::ranges::sort(tick_micros);
        const auto at = [&](double quantile) {
            const auto index = static_cast<usize>(quantile * static_cast<double>(
                                                                 tick_micros.size() - 1));
            return tick_micros[index];
        };
        usize over_budget = 0;
        for (const i64 sample : tick_micros) {
            over_budget += static_cast<usize>(sample > 50'000);
        }
        OV_LOG_INFO("tick time over {} ticks: p50 {} us, p90 {} us, p99 {} us, max {} us, "
                    "{} over 50 ms",
                    tick_micros.size(), at(0.50), at(0.90), at(0.99), tick_micros.back(),
                    over_budget);
    }

    // The same shape, for the one phase that was named as a regression.
    const auto report_phase = [](std::string_view what, std::vector<i64>& samples) {
        if (samples.empty()) {
            return;
        }
        std::ranges::sort(samples);
        const auto at = [&](double quantile) {
            return samples[static_cast<usize>(quantile *
                                              static_cast<double>(samples.size() - 1))];
        };
        i64 total = 0;
        for (const i64 sample : samples) {
            total += sample;
        }
        OV_LOG_INFO("{} over {} runs: p50 {} us, p90 {} us, p99 {} us, max {} us, mean {} us",
                    what, samples.size(), at(0.50), at(0.90), at(0.99), samples.back(),
                    total / static_cast<i64>(samples.size()));
    };
    report_phase("natural spawning", spawn_micros);
    report_phase("spawn chunk list rebuild", spawn_rebuild_micros);
    for (const std::string& line : perf->report(clock.tick_count())) {  // ── perf ──
        OV_LOG_INFO("{}", line);
    }

    if (chunk_source) {
        OV_LOG_INFO(
            "chunk source: {} blocks generated ({} chunks), {} published, {} generated on the "
            "tick thread, {} on other threads",  // ── perf ── the network thread too
            chunk_source->blocks_done(), chunk_source->chunks_done(), chunks_published,
            synchronous_generations, synchronous_generations_elsewhere);
    }

    OV_LOG_INFO("stopped after {} ticks ({} overload events)", clock.tick_count(), behind_events);
    return 0;
}
