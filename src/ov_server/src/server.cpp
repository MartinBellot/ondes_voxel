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
#include "ov/gameplay/entity_physics.hpp"
#include "ov/gameplay/mob_logic.hpp"
#include "ov/protocol/entity.hpp"
#include "ov/registry/registries.hpp"
// ── crafting and smelting ───────────────────────────────────────────────────
#include "workbench.hpp"
// ── containers, and the machines that move items between them ──────────────
#include "block_container.hpp"
#include "item_transport.hpp"
// ── scheduled ticks, natural spawning, and the player's own window ─────────
#include "natural_spawning.hpp"
#include "player_data.hpp"  // ── player data ──
#include "player_inventory.hpp"
#include "world_ticks.hpp"
#include "sounds.hpp"  // ── sound ──
#include "agriculture.hpp"  // ── agriculture ──
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
#include "projectiles.hpp"  // ── projectiles ──
#include "husbandry.hpp"    // ── husbandry ──

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

/// The registry the light engine consults for opacity.
///
/// Set once at start-up. Relighting is called from the generator, from block
/// edits and from the neighbourhood pass, and threading the registry through
/// every one of those would add a parameter that never varies.
const registry::BlockRegistry* light_blocks = nullptr;

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
///   * Opacity comes from the registry's measured table rather than from
///     "anything that is not air". A sign or a torch no longer casts a shadow,
///     which it did — and which only showed after a reload, because a client
///     lights its own edits itself.
/// The lowest y a column still sees full sunlight.
///
/// Not WORLD_SURFACE: that counts the highest **non-air** block, and a sign or
/// a torch raises it while letting light straight through. Direct sunlight
/// descends without losing a level until it meets something opaque, so this
/// scans for that instead — which is why it needs the measured opacity and
/// could not be written before.
[[nodiscard]] i32 sky_floor(const world::Chunk& chunk, usize x, usize z, i32 top);

/// Does sky light stop at this block?
///
/// Measured, not assumed. "Anything that is not air" was the previous rule and
/// it made signs, torches and fences cast full shadows — visible only after a
/// reload, since a client lights its own placements locally.
///
/// Attenuating blocks (water, leaves, ice) are treated as transparent for now:
/// passing light through at full strength is wrong by a level or two, whereas
/// stopping it is wrong by fifteen.
[[nodiscard]] bool stops_sky_light(const registry::BlockRegistry* blocks,
                                   registry::BlockStateId         state) {
    if (blocks == nullptr) {
        return false;
    }
    return blocks->blocks_sky_light(blocks->block_of(state));
}

i32 sky_floor(const world::Chunk& chunk, usize x, usize z, i32 top) {
    for (i32 y = top; y >= chunk.shape().min_y; --y) {
        if (stops_sky_light(light_blocks, chunk.get_block(x, y, z))) {
            return y + 1;
        }
    }
    return chunk.shape().min_y;
}

/// Fill in the light the blocks themselves give off.
///
/// Separate from the sky pass and stored in its own nibble array, because the
/// two answer different questions: sky light is what reaches a cell from above
/// and block light is what a torch puts there. A client shown one in place of
/// the other lights caves like noon.
///
/// The emission is per state — a candle by how many are in the cluster, an ore
/// by whether it is lit — and it is measured, not guessed.
void relight_blocks(world::Chunk& chunk, const registry::BlockRegistry& blocks) {
    const auto shape = chunk.shape();

    struct Cell {
        u8  x;
        i32 y;
        u8  z;
    };

    const auto light_at = [&](usize x, i32 y, usize z) -> u8 {
        const world::ChunkSection* section = chunk.section_for_y(y);
        return section == nullptr ? 0
                                  : section->block_light().get(
                                        world::section_index(x, static_cast<usize>(y & 15), z));
    };
    const auto set_light = [&](usize x, i32 y, usize z, u8 value) {
        world::ChunkSection* section = chunk.section_for_y(y);
        if (section != nullptr) {
            section->block_light().set(world::section_index(x, static_cast<usize>(y & 15), z),
                                       value);
        }
    };

    std::vector<Cell> frontier;

    // Clear first, then seed. Relighting without clearing leaves the light of a
    // torch that was broken, which is invisible until someone walks back into
    // the room they lit yesterday.
    for (usize i = 0; i < shape.section_count(); ++i) {
        world::ChunkSection* section = chunk.section_for_y(shape.min_y + static_cast<i32>(i) * 16);
        if (section == nullptr) {
            continue;
        }
        for (usize index = 0;
             index < world::kSectionSize * world::kSectionSize * world::kSectionSize; ++index) {
            section->block_light().set(index, 0);
        }
    }

    for (usize z = 0; z < 16; ++z) {
        for (usize x = 0; x < 16; ++x) {
            for (i32 y = shape.min_y; y <= shape.max_y(); ++y) {
                const u8 emission = blocks.light_emission(chunk.get_block(x, y, z));
                if (emission > 0) {
                    set_light(x, y, z, emission);
                    frontier.push_back(Cell{static_cast<u8>(x), y, static_cast<u8>(z)});
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
            {static_cast<u8>(cell.x - 1), cell.y, cell.z},
            {static_cast<u8>(cell.x + 1), cell.y, cell.z},
            {cell.x, cell.y, static_cast<u8>(cell.z - 1)},
            {cell.x, cell.y, static_cast<u8>(cell.z + 1)},
            {cell.x, cell.y - 1, cell.z},
            {cell.x, cell.y + 1, cell.z},
        }};

        for (const Cell& next : neighbours) {
            if (next.x >= 16 || next.z >= 16 || next.y < shape.min_y || next.y > shape.max_y()) {
                continue;
            }
            if (stops_sky_light(&blocks, chunk.get_block(next.x, next.y, next.z))) {
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
            section->block_light().compact();
        }
    }
}

void relight_chunk(world::Chunk& chunk) {
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
            const i32 first_free = sky_floor(chunk, x, z, top);
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
            if (stops_sky_light(light_blocks, chunk.get_block(next.x, next.y, next.z))) {
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
                           i32 centre_z) {
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
        for (usize lz = 0; lz < 16; ++lz) {
            for (usize lx = 0; lx < 16; ++lx) {
                const i32 first_free = sky_floor(*entry.chunk, lx, lz, top);
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
            if (stops_sky_light(light_blocks,
                                chunk->get_block(static_cast<usize>(next.x & 15), next.y,
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
        relight_chunk(chunk);
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

/// A stack lying on the ground, waiting to be walked into.
///
/// Kept in a flat list on the tick thread. There are a handful at a time, and
/// an index would cost more than the scan it saves.
struct ItemEntity {
    i32            entity_id{0};
    net::Uuid      uuid{};
    f64            x{0.0};
    f64            y{0.0};
    f64            z{0.0};
    net::ItemStack stack{};
    /// The tick it appeared, for the five minutes vanilla gives it.
    i64 born{0};
    /// Ticks before anyone may pick it up. Vanilla gives a dropped stack half a
    /// second so the player who broke the block does not instantly re-absorb a
    /// block they meant to place.
    i32 pickup_delay{10};
};

/// One experience orb lying in the world.
///
/// Its own type rather than an ItemEntity with a special item: an orb carries a
/// *value* and no stack, it is attracted to a player instead of waiting to be
/// walked into, and two of them can become one. None of that is true of a
/// dropped stack.
struct GroundOrb {
    i32 entity_id{0};
    f64 x{0.0};
    f64 y{0.0};
    f64 z{0.0};
    i32 value{1};
    /// The tick it appeared, for the five minutes vanilla gives it.
    i64 born{0};
    /// Ticks before anyone may pick it up.
    i32 delay{0};
};

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
        "  --record-motion=<file>                          log every reported position\n"
        "  --mobs=<name,name,...>                          place mobs near the spawn point\n"
        "  --effect=<name:amp:ticks,...>                   effects given to every joining player\n"
        "  --help, -h                                      this message\n"
        "\n"
        "Not an official Minecraft product. Not approved by or associated with Mojang.\n");
}

}  // namespace

int ov::server::run(int argc, char** argv, const std::atomic<bool>* external_stop) {
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

    light_blocks = blocks ? &*blocks : nullptr;

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
    if (const char* seed_text = std::getenv("OV_WORLDGEN_SEED");
        seed_text != nullptr && world_available && registries) {
        const i64 world_seed = std::strtoll(seed_text, nullptr, 10);
        level_settings.seed  = world_seed;  // ── commands: what /seed answers ──

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
                            // A saved chunk carries the light it was written
                            // with, which may have come from another
                            // implementation. Recomputing costs a pass and
                            // removes a whole class of "the cave is lit and I
                            // do not know why".
                            if (blocks) {
                                relight_blocks(placed, *blocks);
                            }
                            // Sky light is recomputed only when the file
                            // carries none. A world written by a tool that has
                            // no light engine — ov-lab, for one — would
                            // otherwise be served pitch dark, while a world
                            // vanilla wrote keeps the light vanilla computed
                            // across chunk borders, which a per-chunk pass
                            // here could only make worse.
                            const bool has_sky_light =
                                std::ranges::any_of(placed.sections(),
                                                    [](const world::ChunkSection& section) {
                                                        return !section.sky_light().is_absent();
                                                    });
                            if (!has_sky_light) {
                                relight_chunk(placed);
                            }
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
            ++synchronous_generations;
            chunks.publish(ChunkPos{cx, cz}, generated->generate(cx, cz));
        } else {
            chunks.publish(ChunkPos{cx, cz}, superflat.generate(ChunkPos{cx, cz}));
        }
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

    /// Write every changed chunk, grouped by region so each file opens once.
    const auto save_world = [&] {
        const std::scoped_lock lock{chunk_mutex};
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
            chunks.set_ticket(world::TicketType::Player, static_cast<u64>(player.entity_id),
                              ChunkPos{centre_x, centre_z},
                              world::LoadLevel::for_view_distance(kRadius));
            world::LevelChanges changes;
            chunks.refresh(changes);
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
            if (key != except && other.connection) {
                other.connection->send(*framed);
            }
        }
    };

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
            world::Chunk&          dug = chunk_at(where.x >> 4, where.z >> 4);
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
        return break_rules->destroy_progress(state, holding, who.effects.dig_stance(who.on_ground));
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
            world::Chunk&          dug = chunk_at(where.x >> 4, where.z >> 4);
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
            broadcast(nullptr, net::clientbound::kSpawnEntity,
                      net::encode_spawn_entity(item.entity_id, item.uuid, net::kItemEntityType,
                                               item.x, item.y, item.z));
            // Sans la métadonnée l'entité existe et ne rend rien du tout, ce
            // qui ressemble exactement à un paquet qui ne serait pas arrivé.
            broadcast(
                nullptr, net::clientbound::kEntityMetadata,
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
    // ── husbandry ──
    std::optional<Husbandry> husbandry;

    /// The packets that make one mob appear.
    ///
    /// Three, in this order, and the order is what a real server sends: the
    /// entity, then its metadata, then its attributes. A client told about an
    /// entity it has no metadata for renders it — a mob is not an item, whose
    /// whole appearance is its metadata — but its health bar and its speed come
    /// from the two that follow.
    const auto mob_packets = [&](const entity::EntityState& state,
                                 const auto&                deliver) {
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
            if (!values.empty()) {
                deliver(net::clientbound::kUpdateAttributes,
                        net::encode_update_attributes(state.network_id, values));
            }
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
    const auto block_at = [&](net::WirePosition where) -> registry::BlockStateId {
        const auto shape = world::WorldShape::overworld();
        if (!shape.contains_y(where.y)) {
            return registry::BlockStateId{0};
        }
        return chunk_at(where.x >> 4, where.z >> 4)
            .get_block(static_cast<usize>(where.x & 15), where.y, static_cast<usize>(where.z & 15));
    };

    /// The four horizontal neighbours, in the order the side properties name
    /// them: north, south, west, east.
    const auto neighbours_of = [&](net::WirePosition where) {
        return std::array<registry::BlockStateId, 4>{
            block_at({where.x, where.y, where.z - 1}), block_at({where.x, where.y, where.z + 1}),
            block_at({where.x - 1, where.y, where.z}), block_at({where.x + 1, where.y, where.z})};
    };

    /// A block changed outside the queues — a player placed or broke one.
    ///
    /// This is what makes a lever do anything at all. Vanilla's `setBlock`
    /// notifies the six neighbours as part of writing; ours cannot do it where
    /// the write happens, because placement runs on the network thread while
    /// the rules must run on the tick thread. So the position is queued and the
    /// next tick picks it up — one tick of latency, and the single-writer
    /// rule intact.
    std::vector<net::WirePosition> pending_notifications;
    std::mutex                     notification_mutex;
    const auto                     notify_change = [&](net::WirePosition where) {
        const std::scoped_lock lock{notification_mutex};
        pending_notifications.push_back(where);
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

    const auto apply_block_change = [&](net::WirePosition position, registry::BlockStateId state,
                                        bool relight) {
        const i32 chunk_x = position.x >> 4;
        const i32 chunk_z = position.z >> 4;
        world::Chunk& chunk = chunk_at(chunk_x, chunk_z);

        write_block(chunk, position.x, position.y, position.z, state);
        dirty_chunks.insert(chunk_key(chunk_x, chunk_z));

        if (relight) {
            relight_neighbourhood(
                [&](i32 nx, i32 nz) -> world::Chunk* { return chunks.find(ChunkPos{nx, nz}); },
                chunk_x, chunk_z);
            if (blocks) {
                for (i32 dz = -1; dz <= 1; ++dz) {
                    for (i32 dx = -1; dx <= 1; ++dx) {
                        if (world::Chunk* found = chunks.find(ChunkPos{chunk_x + dx, chunk_z + dz});
                            found != nullptr) {
                            relight_blocks(*found, *blocks);
                        }
                    }
                }
            }
        }

        for (i32 dz = -1; dz <= 1; ++dz) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                if (chunks.contains(ChunkPos{chunk_x + dx, chunk_z + dz})) {
                    dirty_chunks.insert(chunk_key(chunk_x + dx, chunk_z + dz));
                }
            }
        }
    };

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

            write_block(chunk, position.x, position.y, position.z, state);
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
                [&](i32 nx, i32 nz) -> world::Chunk* { return chunks.find(ChunkPos{nx, nz}); },
                chunk_x, chunk_z);

            // Block light too, chunk by chunk. A torch placed at a border lights
            // the chunk next door, so the whole neighbourhood is redone rather
            // than only the one that changed.
            if (blocks) {
                for (i32 dz = -1; dz <= 1; ++dz) {
                    for (i32 dx = -1; dx <= 1; ++dx) {
                        if (world::Chunk* found = chunks.find(ChunkPos{chunk_x + dx, chunk_z + dz});
                            found != nullptr) {
                            relight_blocks(*found, *blocks);
                        }
                    }
                }
            }

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
            if (other.connection) {
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
    const auto set_block_connected = [&](net::WirePosition position, registry::BlockStateId state) {
        if (!connections) {
            set_block_and_broadcast(position, state);
            return;
        }

        std::array<std::pair<net::WirePosition, registry::BlockStateId>, 6> changes{};
        usize                                                               count = 0;
        {
            const std::scoped_lock lock{chunk_mutex};
            const auto             around = neighbours_of(position);
            const auto             above  = block_at({position.x, position.y + 1, position.z});
            changes[count++]              = {position, connections->reshape(state, around, above)};

            constexpr std::array<net::WirePosition, 4> kSteps{
                net::WirePosition{0, 0, -1}, net::WirePosition{0, 0, 1},
                net::WirePosition{-1, 0, 0}, net::WirePosition{1, 0, 0}};
            for (usize i = 0; i < kSteps.size(); ++i) {
                const net::WirePosition      side{position.x + kSteps[i].x, position.y,
                                                  position.z + kSteps[i].z};
                const registry::BlockStateId before = around[i];
                // The neighbour sees the new block, not the old one, so its own
                // view has to be built with the change already in place.
                auto view   = neighbours_of(side);
                view[i ^ 1] = state;
                const registry::BlockStateId after =
                    connections->reshape(before, view, block_at({side.x, side.y + 1, side.z}));
                if (after != before) {
                    changes[count++] = {side, after};
                }
            }

            // And the block underneath. A wall reads what sits on its head —
            // a stone above turns its sides from low to tall — so placing
            // anything has to give the block below a second look.
            const net::WirePosition      under{position.x, position.y - 1, position.z};
            const registry::BlockStateId before = block_at(under);
            const registry::BlockStateId after =
                connections->reshape(before, neighbours_of(under), state);
            if (after != before) {
                changes[count++] = {under, after};
            }
        }
        for (usize i = 0; i < count; ++i) {
            set_block_and_broadcast(changes[i].first, changes[i].second);
        }
    };

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
    /// The chunks the drain touched, relit once at the end instead of once per
    /// block. Reused for the same reason.
    std::unordered_set<i64> tick_relight;

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
            tick_relight.insert(chunk_key(pos.x >> 4, pos.z >> 4));
        };
        hooks.container_signal = [&](BlockPos pos) -> i32 {
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
        // ── tnt and gravity ──
        tnt_gravity.emplace(*blocks, *registries, loot_tables ? &*loot_tables : nullptr,
                            mob_combat ? &*mob_combat : nullptr);
        tnt_gravity->set_redstone(&world_ticks->redstone());
        world_ticks->set_extension(&*tnt_gravity);
        // ── projectiles ──
        projectiles.emplace(*registries, *blocks, mob_combat ? &*mob_combat : nullptr);
        // ── husbandry ──
        husbandry.emplace(*registries, *blocks);
        tick_broadcasts.reserve(4096);
    } else {
        OV_LOG_WARN("no block registry — fluids and redstone stay inert");
    }

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
            return sky_darken_for(server_tick.load(std::memory_order_relaxed));
        };
        tick_hooks.drop_block = plant_drop;
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
        if (!tick_relight.empty()) {
            const std::scoped_lock lock{chunk_mutex};
            for (const i64 key : tick_relight) {
                const auto cx = static_cast<i32>(key >> 32);
                const auto cz = static_cast<i32>(static_cast<u32>(key & 0xFFFFFFFF));
                relight_neighbourhood(
                    [&](i32 nx, i32 nz) -> world::Chunk* { return chunks.find(ChunkPos{nx, nz}); },
                    cx, cz);
                if (blocks) {
                    for (i32 dz = -1; dz <= 1; ++dz) {
                        for (i32 dx = -1; dx <= 1; ++dx) {
                            if (world::Chunk* found = chunks.find(ChunkPos{cx + dx, cz + dz});
                                found != nullptr) {
                                relight_blocks(*found, *blocks);
                            }
                        }
                    }
                }
            }
            tick_relight.clear();
        }

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
                    if (other.connection) {
                        other.connection->send(*framed);
                    }
                }
            }
        }
        tick_broadcasts.clear();
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
    /// A player's record as they stand, their own game mode included.
    const auto player_record_of = [&](const Player& who) {
        usize        overflow = 0;
        PlayerRecord record   = capture_player(
            PlayerPose{who.x, who.y, who.z, who.yaw, who.pitch, who.on_ground},
            who.game_mode, who.inventory, who.carried, who.held_slot, who.survival,
            who.effects, &overflow);
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
        io.hurt_entity = [&](i32 entity_id, f32 damage, bool /*critical*/) {
            return hurt_mob(who, entity_id, damage, held_weapon(who).looting);
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
            broadcast(who.connection.get(), net::clientbound::kEntityTeleport,
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
        // The same behaviour a natural spawn gets: a brain for a species with
        // goals, the falling floor for one without — refused, not invented.
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
        cmd::EntityInfo info;
        info.id       = state->network_id;
        info.type     = std::string{type};
        info.uuid     = state->uuid;
        info.position = state->position;
        info.width    = state->width;
        info.height   = state->height;
        return info;
    };
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
        std::unordered_set<i64>                                             touched;
        {
            const std::scoped_lock chunk_lock{chunk_mutex};
            for (const cmd::BlockChange& change : changes) {
                const net::WirePosition at{change.pos.x, change.pos.y, change.pos.z};
                apply_block_change(at, change.state, false);
                command_block_entity(at, change.state);
                touched.insert(chunk_key(at.x >> 4, at.z >> 4));
                sections[{at.x >> 4, at.y >> 4, at.z >> 4}].push_back(
                    net::SectionBlock{static_cast<u8>(at.x & 15), static_cast<u8>(at.y & 15),
                                      static_cast<u8>(at.z & 15),
                                      static_cast<i32>(change.state.value())});
            }
            for (const i64 key : touched) {
                const auto cx = static_cast<i32>(key >> 32);
                const auto cz = static_cast<i32>(static_cast<u32>(key & 0xFFFFFFFF));
                relight_neighbourhood(
                    [&](i32 nx, i32 nz) -> world::Chunk* { return chunks.find(ChunkPos{nx, nz}); },
                    cx, cz);
                for (i32 dz = -1; dz <= 1 && blocks; ++dz) {
                    for (i32 dx = -1; dx <= 1; ++dx) {
                        if (world::Chunk* found = chunks.find(ChunkPos{cx + dx, cz + dz})) {
                            relight_blocks(*found, *blocks);
                        }
                    }
                }
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
                leaving.emplace(uuid, it->second.name, player_record_of(it->second));  // ── player data ──
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
                    chunks.remove_ticket(world::TicketType::Player,
                                         static_cast<u64>(entity_id));
                    world::LevelChanges changes;
                    chunks.refresh(changes);
                }

                // After the erase, so the leaving player is not sent their own
                // removal on a socket that is already closing.
                broadcast(nullptr, net::clientbound::kRemoveEntities,
                          net::encode_remove_entity(entity_id));
                broadcast(nullptr, net::clientbound::kPlayerInfoRemove,
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
                    broadcast(nullptr, net::clientbound::kPlayerInfoUpdate,
                              net::encode_player_info_add(player.uuid, player.name, player.game_mode));
                    broadcast(
                        nullptr, net::clientbound::kSpawnPlayer,
                        net::encode_spawn_player(player.entity_id, player.uuid, player.x, player.y,
                                                 player.z, player.yaw, player.pitch));

                    for (const auto& [key, other] : players) {
                        send_packet(net::clientbound::kPlayerInfoUpdate,
                                    net::encode_player_info_add(other.uuid, other.name, other.game_mode));
                        send_packet(
                            net::clientbound::kSpawnPlayer,
                            net::encode_spawn_player(other.entity_id, other.uuid, other.x, other.y,
                                                     other.z, other.yaw, other.pitch));
                    }

                    // The stacks already lying around. A joiner who is not told
                    // walks through invisible items and picks them up out of
                    // nowhere.
                    for (const ItemEntity& item : ground_items) {
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
                    if (mobs) {
                        for (const entity::EntityHandle handle : mobs->handles()) {
                            if (const entity::EntityState* mob = mobs->state(handle)) {
                                mob_packets(*mob, send_packet);
                            }
                        }
                    }

                    players[connection.get()] = player;
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

                            broadcast(connection.get(), net::clientbound::kEntityTeleport,
                                      net::encode_entity_teleport(
                                          player.entity_id, player.x, player.y, player.z,
                                          player.yaw, player.pitch, movement->on_ground));
                            // The head turns independently of the body; without
                            // this a player renders looking permanently ahead.
                            broadcast(
                                connection.get(), net::clientbound::kEntityHeadRotation,
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
                            player.inventory[static_cast<usize>(creative->slot)] =
                                net::ItemStack{creative->item_id.value_or(0),
                                               creative->item_id ? creative->count : i8{0},
                                               {}};
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
                                    broken = block_at(action->position);
                                }
                                set_block_connected(action->position, superflat.air.air);
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
                                    broken = block_at(action->position);
                                }
                                set_block_connected(action->position, superflat.air.air);
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
                                    broken = block_at(action->position);
                                }
                                set_block_connected(action->position, superflat.air.air);
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

                    case net::serverbound::kCloseContainer: {
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
            return sky_darken_for(commands ? commands->world().day_time
                                           : server_tick.load(std::memory_order_relaxed));
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
    spawning_ready = load_biome_spawners(std::filesystem::path{OV_DATA_DIR} / "vanilla" /
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
                    (void)who.survival.hurt(gameplay::DamageKind::Explosion, amount, io,
                                            who.entity_id);
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
    // ── fire ── a Flame arrow's victim, and what a burning mob's death drops
    projectile_host.set_on_fire = [&](i32 entity_id, i32 seconds) {
        if (fire_session) {
            fire_session->set_mob_on_fire(entity_id, seconds);
        }
    };
    const FireMobHost fire_mob_host{tnt_deliver, tnt_host.drop_item};
    // ── end fire ──
    const ProjectileDeliver projectile_deliver = tnt_deliver;
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
    // ── end husbandry ───────────────────────────────────────────────────────

    const auto should_stop = [&]() {
        return g_stop_requested.load(std::memory_order_relaxed) ||
               (external_stop != nullptr && external_stop->load(std::memory_order_relaxed));
    };

    while (!should_stop()) {
        const auto tick_started = std::chrono::steady_clock::now();
        const i32  ticks        = clock.advance();
        server_tick.store(clock.tick_count(), std::memory_order_relaxed);

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
                        ++chunks_published;
                    }
                }
            }
        }

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
            for (const ChunkPos pos : to_evict) {
                (void)chunks.evict(pos);
            }
            if (!to_evict.empty()) {
                OV_LOG_DEBUG("unloaded {} chunks no ticket reaches, {} resident", to_evict.size(),
                             chunks.resident());
            }
        }

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
            }
        }
        // ── end commands ────────────────────────────────────────────────────

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
                            random_tick_players.push_back(Vec3d{who.x, who.y, who.z});
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
                        spawn_players.push_back(Vec3d{who.x, who.y, who.z});
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

        // Mobs: gravity, collision, and only the movement that actually
        // happened. A delta packet when the move fits in one — six bytes rather
        // than twenty-eight — and a teleport when it does not.
        if (mobs && blocks &&
            (!mobs->handles().empty() || (tnt_gravity && tnt_gravity->has_pending()) ||
             (projectiles && projectiles->has_pending()))) {  // ── projectiles ──
            std::unique_lock mob_lock{players_mutex, std::try_to_lock};
            if (mob_lock.owns_lock()) {
                const std::scoped_lock chunk_lock{chunk_mutex};
                WorldView              view;
                view.read = [&](i32 bx, i32 by, i32 bz) { return block_at({bx, by, bz}); };
                const gameplay::CollisionWorld collisions{*blocks, &WorldView::look_up, &view};

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
                    const registry::BlockRegistry*                  registry{nullptr};

                    [[nodiscard]] registry::BlockStateId block_at(BlockPos pos) const override {
                        return read(pos);
                    }
                    [[nodiscard]] bool is_loaded(BlockPos) const override { return true; }
                    [[nodiscard]] world::WorldShape shape() const override {
                        return world::WorldShape::overworld();
                    }
                    [[nodiscard]] world::DimensionTraits traits() const override { return {}; }
                    [[nodiscard]] const registry::BlockRegistry& blocks() const override {
                        return *registry;
                    }
                };
                MobLevel mob_level;
                mob_level.read     = [&](BlockPos pos) {
                    return block_at({pos.x, pos.y, pos.z});
                };
                mob_level.registry = &*blocks;

                gameplay::MobContext mob_context{&collisions, &mob_level, false};
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
                    projectiles->tick_skeletons(*mobs, collisions, 2, projectile_deliver);
                }
                // ── husbandry: clicks and tempters before, births and eggs after ──
                if (husbandry) {
                    (void)husbandry->before_entity_tick(*mobs, mob_context, husbandry_host,
                                                       husbandry_deliver);
                }
                mobs->tick(entity::TickContext{clock.tick_count(), &mob_context});
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
        // ── tnt and gravity: the craters and landings, sent and relit ──
        flush_tick_writes();

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
                    if (who.mortal()) {  // ── commands: per player ──
                        consume_one_held(who);
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
                        const std::string_view name =
                            blocks->block_name(blocks->block_of(block_at(eyes)));
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
                                              .slow_falling = who.effects.slow_falling()};
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
                                        return who.survival.hurt(kind, amount, io, who.entity_id)
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
                        static_cast<f64>(world::WorldShape::overworld().min_y));

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

                    if (who.wants_respawn) {
                        who.wants_respawn = false;
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
                        if (who.survival.perform_respawn(view, io, outcome, 0)) {
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
                        if (gameplay::orbs_can_merge(a, b, dx * dx + dy * dy + dz * dz)) {
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
                        if (!candidate.confirmed || candidate.survival.awaiting_respawn) {
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
                            taker->survival.award_experience(orb.value);
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
                        std::vector<ItemEntity> dropped;
                        drop_loot(digger, where, dropped);
                        registry::BlockStateId broken{};  // ── sound ──
                        {
                            const std::scoped_lock chunk_lock{chunk_mutex};
                            broken = block_at(where);
                        }
                        set_block_connected(where, superflat.air.air);
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

        // Autosave. A clean shutdown saves too, but a server that is killed
        // never gets one — and losing an hour of building to a crash is the
        // failure people remember. Thirty seconds is short enough to matter and
        // long enough that a world with nothing dirty costs a map lookup.
        if (const auto now = std::chrono::steady_clock::now();
            now - last_autosave >= std::chrono::seconds{30}) {
            last_autosave = now;
            save_online_players();  // ── player data ── before level.dat, for the host
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
                            world::Chunk*          ready = chunk_if_resident(cx, cz);
                            if (ready == nullptr) {
                                if (chunk_source) {
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
    save_online_players();  // ── player data ──
    save_world();

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

    if (chunk_source) {
        OV_LOG_INFO(
            "chunk source: {} blocks generated ({} chunks), {} published, {} generated on the "
            "tick thread",
            chunk_source->blocks_done(), chunk_source->chunks_done(), chunks_published,
            synchronous_generations);
    }

    OV_LOG_INFO("stopped after {} ticks ({} overload events)", clock.tick_count(), behind_events);
    return 0;
}
