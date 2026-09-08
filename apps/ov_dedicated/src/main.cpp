// Ondes VOXEL dedicated server.
//
// At M0 this ticks an empty world at 20 Hz and does nothing else. That is
// deliberately the first runnable artefact: the tick loop, its scheduling class
// and its behaviour under overload are the foundation everything else stands
// on, and they are far easier to get right on an empty world than on a full one.

#define OV_LOG_CATEGORY "server"

#include "ov/base/alloc_scope.hpp"
#include "ov/base/assert.hpp"
#include "ov/base/log.hpp"
#include "ov/base/thread.hpp"
#include "ov/base/time.hpp"
#include "ov/io/file.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/nbt/binary.hpp"
#include "ov/nbt/region_writer.hpp"
#include "ov/protocol/framing.hpp"
#include "ov/protocol/listener.hpp"
#include "ov/protocol/login.hpp"
#include "ov/protocol/play.hpp"
#include "ov/protocol/status.hpp"
#include "ov/protocol/varint.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/chunk.hpp"
#include "ov/world/chunk_storage.hpp"
#include "ov/world/level_dat.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
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
};

/// Where a connection is in the protocol's state machine.
///
/// A packet id means different things in different states — 0x00 is Handshake,
/// then Status Request, then Login Start — so the state has to be tracked per
/// connection or the ids are ambiguous.
enum class ConnectionState { Handshaking, Status, Login, Play };

/// Sky light for a whole chunk: direct sunlight, then flood fill.
///
/// Two phases, and the second is the one that makes builds look right.
///
/// Direct sunlight first: every column is lit to 15 from the top down to its
/// first obstruction, which WORLD_SURFACE already knows. That alone lights open
/// ground and the inside of a shaft correctly.
///
/// Then the light spreads sideways and downward, losing one level per step.
/// Without this, a single block placed as a roof leaves a hard black square
/// underneath with a sharp edge against the lit ground beside it — measured, and
/// exactly what a player notices first.
///
/// Two limits, both stated rather than implied:
///
///   * Propagation stops at the chunk's edge. A build straddling a border casts
///     no shadow into its neighbour, so a seam is visible there. Cross-chunk
///     light needs the neighbours loaded and a scheduler to order the work.
///   * Non-air stands in for opaque, so glass would cast a shadow. That ends
///     with the block-state flag table.
void relight_chunk(world::Chunk& chunk, const world::AirStates& air) {
    const auto  shape   = chunk.shape();
    const auto& surface = chunk.heightmap(world::HeightmapType::WorldSurface);

    // Only the occupied band needs work. Everything above the tallest column is
    // open sky and everything below the world is nothing, and walking all 384
    // levels of 289 chunks on every join would be felt.
    i32 highest = shape.min_y;
    for (usize z = 0; z < 16; ++z) {
        for (usize x = 0; x < 16; ++x) {
            highest = std::max(highest, surface.first_free(x, z));
        }
    }
    const i32 top = std::min(highest + 1, shape.max_y());

    const auto light_at = [&](usize x, i32 y, usize z) -> u8 {
        const world::ChunkSection* section = chunk.section_for_y(y);
        return section == nullptr ? 0
                                  : section->sky_light().get(
                                        world::section_index(x, static_cast<usize>(y & 15), z));
    };
    const auto set_light = [&](usize x, i32 y, usize z, u8 value) {
        world::ChunkSection* section = chunk.section_for_y(y);
        if (section != nullptr) {
            section->sky_light().set(world::section_index(x, static_cast<usize>(y & 15), z), value);
        }
    };

    // ── Direct sunlight ─────────────────────────────────────────────────────
    struct Cell {
        u8  x;
        i32 y;
        u8  z;
    };

    std::vector<Cell> frontier;

    for (usize z = 0; z < 16; ++z) {
        for (usize x = 0; x < 16; ++x) {
            const i32 first_free = surface.first_free(x, z);
            for (usize i = 0; i < shape.section_count(); ++i) {
                const i32            bottom  = shape.min_y + static_cast<i32>(i) * 16;
                world::ChunkSection* section = chunk.section_for_y(bottom);
                if (section == nullptr) {
                    continue;
                }
                for (usize local_y = 0; local_y < 16; ++local_y) {
                    const i32 y = bottom + static_cast<i32>(local_y);
                    section->sky_light().set(world::section_index(x, local_y, z),
                                             y >= first_free ? world::kMaxLightLevel : 0);
                }
            }
            // Every directly lit cell in the band is a source, not just the
            // lowest one of each column. Seeding only the lowest assumes
            // everything above it is surrounded by light, which is false wherever
            // two columns have different heights — and that boundary is exactly
            // where a build casts its shadow. Measured: the cell under a
            // one-block roof came out at 13 instead of 14, because the lit cell
            // beside it at the same height was never a source.
            for (i32 y = std::max(first_free, shape.min_y); y <= top; ++y) {
                frontier.push_back(Cell{static_cast<u8>(x), y, static_cast<u8>(z)});
            }
        }
    }

    // ── Flood fill ──────────────────────────────────────────────────────────
    for (usize head = 0; head < frontier.size(); ++head) {
        const Cell cell    = frontier[head];
        const u8   current = light_at(cell.x, cell.y, cell.z);
        if (current <= 1) {
            continue;
        }
        const u8 spread = static_cast<u8>(current - 1);

        const std::array<Cell, 6> neighbours{{
            {static_cast<u8>(cell.x - 1), cell.y, cell.z},
            {static_cast<u8>(cell.x + 1), cell.y, cell.z},
            {cell.x, cell.y, static_cast<u8>(cell.z - 1)},
            {cell.x, cell.y, static_cast<u8>(cell.z + 1)},
            {cell.x, cell.y - 1, cell.z},
            {cell.x, cell.y + 1, cell.z},
        }};

        for (const Cell& next : neighbours) {
            // Unsigned wrap makes an x of -1 become 255, so one comparison
            // covers both edges.
            if (next.x >= 16 || next.z >= 16 || next.y < shape.min_y || next.y > top) {
                continue;
            }
            if (!air.is_air(chunk.get_block(next.x, next.y, next.z))) {
                continue;
            }
            if (light_at(next.x, next.y, next.z) >= spread) {
                continue;
            }
            set_light(next.x, next.y, next.z, spread);
            frontier.push_back(next);
        }
    }

    for (usize i = 0; i < shape.section_count(); ++i) {
        world::ChunkSection* section = chunk.section_for_y(shape.min_y + static_cast<i32>(i) * 16);
        if (section != nullptr) {
            section->sky_light().compact();
        }
    }
}

/// Sky light across a chunk and its eight neighbours.
///
/// The single-chunk version stops propagation at the border, so a build sitting
/// against one casts no shadow into the chunk beside it and the seam shows as a
/// straight line of wrongly-lit ground. Light does not respect chunk boundaries
/// and neither can the fill.
///
/// Only chunks already loaded take part. Pulling neighbours in would cascade —
/// generating one chunk would generate its neighbours, and theirs — so a build
/// against the edge of the loaded area still seams there. That edge moves with
/// the player and is out of sight; a chunk boundary in the middle of a base is
/// not.
///
/// The correction goes to storage and is not resent. The client lights its own
/// edits locally, so the seam is invisible until the chunk is loaded again —
/// which is exactly when the stored value is the one that matters.
void relight_neighbourhood(const std::function<world::Chunk*(i32, i32)>& lookup, i32 centre_x,
                           i32 centre_z, const world::AirStates& air) {
    const auto shape = world::WorldShape::overworld();

    struct Loaded {
        world::Chunk* chunk;
        i32           origin_x;
        i32           origin_z;
    };

    std::vector<Loaded> loaded;
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            if (world::Chunk* chunk = lookup(centre_x + dx, centre_z + dz)) {
                loaded.push_back(Loaded{chunk, (centre_x + dx) * 16, (centre_z + dz) * 16});
            }
        }
    }
    if (loaded.empty()) {
        return;
    }

    // World coordinates throughout: the whole point is that the fill does not
    // know where the borders are.
    const auto chunk_for = [&](i32 x, i32 z) -> world::Chunk* {
        for (const Loaded& entry : loaded) {
            if (x >= entry.origin_x && x < entry.origin_x + 16 && z >= entry.origin_z &&
                z < entry.origin_z + 16) {
                return entry.chunk;
            }
        }
        return nullptr;
    };

    i32 top = shape.min_y;
    for (const Loaded& entry : loaded) {
        const auto& surface = entry.chunk->heightmap(world::HeightmapType::WorldSurface);
        for (usize z = 0; z < 16; ++z) {
            for (usize x = 0; x < 16; ++x) {
                top = std::max(top, surface.first_free(x, z));
            }
        }
    }
    top = std::min(top + 1, shape.max_y());

    const auto light_at = [&](i32 x, i32 y, i32 z) -> u8 {
        world::Chunk* chunk = chunk_for(x, z);
        if (chunk == nullptr) {
            return 0;
        }
        const world::ChunkSection* section = chunk->section_for_y(y);
        return section == nullptr ? 0
                                  : section->sky_light().get(world::section_index(
                                        static_cast<usize>(x & 15), static_cast<usize>(y & 15),
                                        static_cast<usize>(z & 15)));
    };
    const auto set_light = [&](i32 x, i32 y, i32 z, u8 value) {
        world::Chunk* chunk = chunk_for(x, z);
        if (chunk == nullptr) {
            return;
        }
        world::ChunkSection* section = chunk->section_for_y(y);
        if (section != nullptr) {
            section->sky_light().set(
                world::section_index(static_cast<usize>(x & 15), static_cast<usize>(y & 15),
                                     static_cast<usize>(z & 15)),
                value);
        }
    };

    struct Cell {
        i32 x;
        i32 y;
        i32 z;
    };

    std::vector<Cell> frontier;

    for (const Loaded& entry : loaded) {
        const auto& surface = entry.chunk->heightmap(world::HeightmapType::WorldSurface);
        for (usize lz = 0; lz < 16; ++lz) {
            for (usize lx = 0; lx < 16; ++lx) {
                const i32 first_free = surface.first_free(lx, lz);
                const i32 x          = entry.origin_x + static_cast<i32>(lx);
                const i32 z          = entry.origin_z + static_cast<i32>(lz);
                for (i32 y = shape.min_y; y <= top; ++y) {
                    const bool lit = y >= first_free;
                    set_light(x, y, z, lit ? world::kMaxLightLevel : 0);
                    if (lit) {
                        frontier.push_back(Cell{x, y, z});
                    }
                }
            }
        }
    }

    for (usize head = 0; head < frontier.size(); ++head) {
        const Cell cell    = frontier[head];
        const u8   current = light_at(cell.x, cell.y, cell.z);
        if (current <= 1) {
            continue;
        }
        const u8 spread = static_cast<u8>(current - 1);

        const std::array<Cell, 6> neighbours{{
            {cell.x - 1, cell.y, cell.z},
            {cell.x + 1, cell.y, cell.z},
            {cell.x, cell.y, cell.z - 1},
            {cell.x, cell.y, cell.z + 1},
            {cell.x, cell.y - 1, cell.z},
            {cell.x, cell.y + 1, cell.z},
        }};

        for (const Cell& next : neighbours) {
            if (next.y < shape.min_y || next.y > top) {
                continue;
            }
            world::Chunk* chunk = chunk_for(next.x, next.z);
            if (chunk == nullptr) {
                continue;  // outside the loaded neighbourhood
            }
            if (!air.is_air(chunk->get_block(static_cast<usize>(next.x & 15), next.y,
                                             static_cast<usize>(next.z & 15)))) {
                continue;
            }
            if (light_at(next.x, next.y, next.z) >= spread) {
                continue;
            }
            set_light(next.x, next.y, next.z, spread);
            frontier.push_back(next);
        }
    }

    for (const Loaded& entry : loaded) {
        for (usize i = 0; i < shape.section_count(); ++i) {
            world::ChunkSection* section =
                entry.chunk->section_for_y(shape.min_y + static_cast<i32>(i) * 16);
            if (section != nullptr) {
                section->sky_light().compact();
            }
        }
    }
}

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
        };
    }

    [[nodiscard]] world::Chunk generate(ChunkPos position) const {
        world::Chunk chunk{position, world::WorldShape::overworld(), air};

        for (usize z = 0; z < 16; ++z) {
            for (usize x = 0; x < 16; ++x) {
                chunk.set_block(x, -64, z, bedrock);
                chunk.set_block(x, -63, z, dirt);
                chunk.set_block(x, -62, z, dirt);
                chunk.set_block(x, kSurfaceY, z, grass);

                // Every block here stops movement, so the two heightmaps
                // coincide. Saying so is exact for this terrain; it is not a
                // general rule and does not become one.
                chunk.heightmap(world::HeightmapType::MotionBlocking).set_surface(x, z, kSurfaceY);
            }
        }

        chunk.fill_biome(biome);

        // Light comes from the heightmap, through the same function block edits
        // use. Two code paths that compute light differently agree right up
        // until someone digs.
        relight_chunk(chunk, air);
        return chunk;
    }
};

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

    /// The creative hotbar, as item ids. Slots 36..44 of the inventory are the
    /// hotbar; the client tells us what it puts there, and that is the only way
    /// to know which block a placement means.
    std::array<i32, 9> hotbar{};
    i16                held_slot{0};
    f64                x{0.5};
    f64                y{static_cast<f64>(Superflat::kSurfaceY) + 1.0};
    f64                z{0.5};
    f32                yaw{0.0F};
    f32                pitch{0.0F};

    /// The key this player is remembered under between sessions.
    std::string identity;

    /// Chunks this client currently holds, so streaming sends each one once and
    /// unloads exactly what left range. Recomputing the difference from the
    /// player's position alone would be equivalent right up to the first time a
    /// send fails or the view distance changes.
    std::unordered_set<i64> loaded_chunks;
    i32                     centre_x{0};
    i32                     centre_z{0};
    bool                    streaming{false};
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
        "  --motd=<text>                                   server list description\n"
        "  --log-level=<trace|debug|info|warn|error|off>   verbosity (default: info)\n"
        "  --ticks=<n>                                     stop after n ticks\n"
        "  --help, -h                                      this message\n"
        "\n"
        "Not an official Minecraft product. Not approved by or associated with Mojang.\n");
}

}  // namespace

int main(int argc, char** argv) {
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

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

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

    // Item ids, needed to turn "the player is holding this" into a block.
    const auto registries =
        registry::Registries::load(data_dir / "vanilla" / "1.20.1" / "registry.ovpack");
    const auto item_registry = registries ? registries->find("minecraft:item") : std::nullopt;

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
    const std::filesystem::path level_dir = std::filesystem::path{"run"} / "world";
    const std::filesystem::path world_dir = level_dir / "region";
    std::error_code             directory_error;
    std::filesystem::create_directories(world_dir, directory_error);

    std::vector<std::string> biome_name_storage =
        codec_bytes ? biome_names_in_codec(*codec_bytes) : std::vector<std::string>{};
    std::vector<std::string_view> biome_names;
    biome_names.reserve(biome_name_storage.size());
    for (const auto& name : biome_name_storage) {
        biome_names.emplace_back(name);
    }

    std::mutex                            chunk_mutex;
    std::unordered_map<i64, world::Chunk> chunk_cache;
    std::unordered_set<i64>               dirty_chunks;
    const auto                            chunk_key = [](i32 cx, i32 cz) {
        return (static_cast<i64>(cx) << 32) ^ static_cast<u32>(cz);
    };

    /// The block state an item places, or nothing if the item is not a block.
    ///
    /// Items and blocks are separate registries with separate ids, and the
    /// bridge between them is the name: `minecraft:stone` the item places
    /// `minecraft:stone` the block. Most items that are not blocks simply have
    /// no block of the same name, which is exactly the test.
    const auto block_state_for_item = [&](i32 item_id) -> std::optional<registry::BlockStateId> {
        if (!registries || !item_registry || !blocks) {
            return std::nullopt;
        }
        const std::string_view name = registries->entry_of(*item_registry, item_id);
        if (name.empty()) {
            return std::nullopt;
        }
        const auto block = blocks->find_block(name);
        if (!block) {
            return std::nullopt;
        }
        return blocks->default_state(*block);
    };

    std::unordered_map<const net::Connection*, Player> players;
    std::unordered_map<std::string, SavedPlayer>       saved_players;
    std::mutex                                         players_mutex;
    std::atomic<i32>                                   next_entity_id{1};

    const world::ChunkCodecContext codec_context{
        blocks ? &*blocks : nullptr,
        biome_names,
        superflat.air,
    };

    /// Fetch a chunk: from memory, then from disk, then generated.
    ///
    /// Disk before the generator, so a saved chunk always wins. The other order
    /// works until the first reload and then quietly discards everything anyone
    /// built.
    ///
    /// Caller holds chunk_mutex.
    const auto chunk_at = [&](i32 cx, i32 cz) -> world::Chunk& {
        const i64 key = chunk_key(cx, cz);
        if (const auto it = chunk_cache.find(key); it != chunk_cache.end()) {
            return it->second;
        }

        const auto region = nbt::RegionFile::open(region_path(world_dir, cx, cz));
        if (region) {
            const auto local_x = static_cast<u32>(cx & 31);
            const auto local_z = static_cast<u32>(cz & 31);
            if (region->has_chunk(local_x, local_z)) {
                if (const auto document = region->read_chunk(local_x, local_z)) {
                    if (auto loaded = world::from_nbt(*document, codec_context)) {
                        return chunk_cache.emplace(key, std::move(*loaded)).first->second;
                    }
                }
            }
        }
        return chunk_cache.emplace(key, superflat.generate(ChunkPos{cx, cz})).first->second;
    };

    /// Write every changed chunk, grouped by region so each file opens once.
    const auto save_world = [&] {
        const std::scoped_lock lock{chunk_mutex};
        if (dirty_chunks.empty()) {
            return;
        }

        std::map<std::pair<i32, i32>, std::vector<i64>> by_region;
        for (const i64 key : dirty_chunks) {
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
                const auto it = chunk_cache.find(key);
                if (it == chunk_cache.end()) {
                    continue;
                }
                const auto cx = static_cast<i32>(key >> 32);
                const auto cz = static_cast<i32>(static_cast<u32>(key & 0xFFFFFFFF));
                writer.set_chunk(static_cast<u32>(cx & 31), static_cast<u32>(cz & 31),
                                 world::to_nbt(it->second, codec_context), 0);
                ++written;
            }
            if (!writer.write(path)) {
                OV_LOG_WARN("could not write {}", path.string());
            }
        }
        // level.dat every time, not once at creation: it is small, and a world
        // whose regions are newer than its level.dat is the state a crash
        // leaves behind.
        world::LevelSettings settings;
        settings.name    = "Ondes VOXEL";
        settings.spawn_y = Superflat::kSurfaceY + 1;
        settings.layers  = {
            {"minecraft:bedrock", 1},
            {"minecraft:dirt", 2},
            {"minecraft:grass_block", 1},
        };
        if (!io::write_file_atomic(level_dir / "level.dat", world::encode_level_dat(settings))) {
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

        for (const i64 key : wanted) {
            if (player.loaded_chunks.contains(key)) {
                continue;
            }
            const auto cx = static_cast<i32>(key >> 32);
            const auto cz = static_cast<i32>(static_cast<u32>(key & 0xFFFFFFFF));
            {
                const std::scoped_lock lock{chunk_mutex};
                send(net::clientbound::kChunkDataAndLight,
                     net::encode_chunk_data(chunk_at(cx, cz)));
            }
            player.loaded_chunks.insert(key);
        }

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
    const auto set_block_and_broadcast = [&](net::WirePosition      position,
                                             registry::BlockStateId state) {
        const auto shape = world::WorldShape::overworld();
        if (!shape.contains_y(position.y)) {
            return;
        }

        const i32 chunk_x = position.x >> 4;
        const i32 chunk_z = position.z >> 4;
        {
            const std::scoped_lock lock{chunk_mutex};
            world::Chunk&          chunk = chunk_at(chunk_x, chunk_z);

            const auto local_x = static_cast<usize>(position.x & 15);
            const auto local_z = static_cast<usize>(position.z & 15);
            chunk.set_block(local_x, position.y, local_z, state);
            dirty_chunks.insert(chunk_key(chunk_x, chunk_z));

            // WORLD_SURFACE has just moved, so the chunk's sky light has too.
            // Relighting the whole chunk rather than the column: light spreads
            // sideways, so one block placed changes cells several away from it.
            // Without this a hole stays lit as if it were still filled, and the
            // error only shows after a reload — the client lights its own edits
            // locally and never notices the server disagreeing.
            // Across the neighbourhood, not just this chunk: light does not
            // respect chunk borders, so a block placed against one changes cells
            // on the other side of it.
            relight_neighbourhood(
                [&](i32 nx, i32 nz) -> world::Chunk* {
                    const auto found = chunk_cache.find(chunk_key(nx, nz));
                    return found == chunk_cache.end() ? nullptr : &found->second;
                },
                chunk_x, chunk_z, superflat.air);

            for (i32 dz = -1; dz <= 1; ++dz) {
                for (i32 dx = -1; dx <= 1; ++dx) {
                    if (chunk_cache.contains(chunk_key(chunk_x + dx, chunk_z + dz))) {
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
            if (other.connection) {
                other.connection->send(*framed);
            }
        }
    };

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
        {
            const std::scoped_lock lock{players_mutex};
            players.erase(connection.get());
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

                send_packet(static_cast<i32>(net::LoginPacket::Success),
                            net::encode_login_success(uuid, login->name));

                Player player;
                player.entity_id = next_entity_id.fetch_add(1);
                player.identity  = uuid.to_string();
                {
                    const std::scoped_lock lock{players_mutex};
                    if (const auto saved = saved_players.find(player.identity);
                        saved != saved_players.end()) {
                        player.x     = saved->second.x;
                        player.y     = saved->second.y;
                        player.z     = saved->second.z;
                        player.yaw   = saved->second.yaw;
                        player.pitch = saved->second.pitch;
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
                join.entity_id           = player.entity_id;
                join.game_mode           = 1;  // creative, so flying works without food
                join.registry_codec      = *codec_bytes;
                join.view_distance       = 10;
                join.simulation_distance = 10;
                send_packet(net::clientbound::kLoginPlay, net::encode_login_play(join));

                send_packet(net::clientbound::kPlayerAbilities,
                            net::encode_player_abilities(true, false, true, true, 0.05F, 0.1F));

                player.pending_teleport = 1;
                send_packet(
                    net::clientbound::kSynchronizePosition,
                    net::encode_synchronize_position(player.x, player.y, player.z, player.yaw,
                                                     player.pitch, player.pending_teleport));

                send_packet(net::clientbound::kSetDefaultSpawn,
                            net::encode_set_default_spawn(0, Superflat::kSurfaceY + 1, 0, 0.0F));

                // The centre has to arrive before the chunks: a client that
                // receives chunks with no centre keeps them and renders
                // nothing.
                stream_chunks(connection, player);

                // Reason 13: "start waiting for level chunks". This is what
                // takes the client off the loading screen.
                send_packet(net::clientbound::kGameEvent, net::encode_game_event(13, 0.0F));

                player.connection = connection;
                {
                    const std::scoped_lock lock{players_mutex};
                    players[connection.get()] = player;
                }
                OV_LOG_INFO("{} joined at ({:.1f}, {:.1f}, {:.1f})", login->name, player.x,
                            player.y, player.z);
                return true;
            }

            case ConnectionState::Play: {
                const std::scoped_lock lock{players_mutex};
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
                        if (movement->x) {
                            player.x = *movement->x;
                            player.y = *movement->y;
                            player.z = *movement->z;
                        }
                        if (movement->yaw) {
                            player.yaw   = *movement->yaw;
                            player.pitch = *movement->pitch;
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
                        return true;
                    }

                    case net::serverbound::kSetHeldItem: {
                        const auto slot = net::parse_set_held_item(body);
                        if (slot && *slot >= 0 && *slot < 9) {
                            player.held_slot = *slot;
                        }
                        return true;
                    }

                    case net::serverbound::kSetCreativeSlot: {
                        const auto creative = net::parse_set_creative_slot(body);
                        if (!creative) {
                            return false;
                        }
                        // Inventory slots 36..44 are the hotbar. Anything else
                        // is a slot the server does not model yet.
                        const int index = creative->slot - 36;
                        if (index >= 0 && index < 9) {
                            player.hotbar[static_cast<usize>(index)] =
                                creative->item_id.value_or(0);
                        }
                        return true;
                    }

                    case net::serverbound::kPlayerAction: {
                        const auto action = net::parse_player_action(body);
                        if (!action) {
                            return false;
                        }
                        // 0 is "started digging" — which in creative means the
                        // block is already gone on the client — and 2 is
                        // "finished" in survival. Handling only one of them
                        // makes the other game mode do nothing.
                        if (action->status != 0 && action->status != 2) {
                            return true;
                        }
                        set_block_and_broadcast(action->position, superflat.air.air);
                        acknowledge(connection, action->sequence);
                        return true;
                    }

                    case net::serverbound::kUseItemOn: {
                        const auto place = net::parse_use_item_on(body);
                        if (!place) {
                            return false;
                        }
                        acknowledge(connection, place->sequence);

                        const i32  item       = player.hotbar[static_cast<usize>(player.held_slot)];
                        const auto held_state = block_state_for_item(item);
                        if (!held_state) {
                            // An empty hand or a non-block item. The
                            // acknowledgement above still matters: without it
                            // the client waits, then rolls back its guess.
                            return true;
                        }

                        // The clicked block is not where the new one goes: the
                        // face says which side, and it lands one step along it.
                        const auto target = net::offset_by_face(place->position, place->face);
                        set_block_and_broadcast(target, *held_state);
                        return true;
                    }

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
    auto      last_autosave = std::chrono::steady_clock::now();

    while (!g_stop_requested.load(std::memory_order_relaxed)) {
        const i32 ticks = clock.advance();

        for (i32 i = 0; i < ticks; ++i) {
            // The world tick lives here. Everything inside must be
            // deterministic, and must not allocate once running: in debug
            // builds this guard aborts on the first allocation, naming it.
            const NoAllocScope no_alloc{"server tick"};
        }

        // Autosave. A clean shutdown saves too, but a server that is killed
        // never gets one — and losing an hour of building to a crash is the
        // failure people remember. Thirty seconds is short enough to matter and
        // long enough that a world with nothing dirty costs a map lookup.
        if (const auto now = std::chrono::steady_clock::now();
            now - last_autosave >= std::chrono::seconds{30}) {
            last_autosave = now;
            save_world();
        }

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
                for (auto& [key, player] : players) {
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
    save_world();

    listener->stop();
    network_thread.join();

    OV_LOG_INFO("stopped after {} ticks ({} overload events)", clock.tick_count(), behind_events);
    return 0;
}
