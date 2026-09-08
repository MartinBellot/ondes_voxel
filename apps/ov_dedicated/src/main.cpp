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
        relight_chunk(chunk);
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
           name.ends_with("_fence_gate") || name.ends_with("_door");
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
    const auto quarter = static_cast<i32>(std::floor(static_cast<f64>(player_yaw) / 90.0 + 0.5));
    // Modulo of a negative yaw is negative in C++, and a negative index would
    // read off the front of the array.
    const auto looking  = static_cast<usize>(((quarter % 4) + 4) % 4);
    const auto opposite = (looking + 2) % 4;
    set("facing", kFacing[faces_away_from_player(blocks.block_name(block)) ? looking : opposite]);

    const bool upper = place.face == 0 || (place.face >= 2 && place.cursor_y > 0.5F);
    set("half", upper ? "top" : "bottom");
    set("type", upper ? "top" : "bottom");

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

/// A chest's 27 slots, read out of its block entity NBT.
///
/// Vanilla stores them as a list of `{Slot, id, Count}` and omits empty ones,
/// so the list is not indexed by slot and its length says nothing about the
/// chest's size.
[[nodiscard]] std::array<net::ItemStack, 27> chest_items(
    const nbt::Tag& data, const registry::Registries* registries,
    std::optional<registry::RegistryId> item_registry) {
    std::array<net::ItemStack, 27> slots{};
    const nbt::Tag*                items = data.find("Items");
    if (items == nullptr || items->list() == nullptr || registries == nullptr || !item_registry) {
        return slots;
    }
    for (const nbt::Tag& entry : *items->list()) {
        const nbt::Tag* slot  = entry.find("Slot");
        const nbt::Tag* id    = entry.find("id");
        const nbt::Tag* count = entry.find("Count");
        if (slot == nullptr || id == nullptr || count == nullptr) {
            continue;
        }
        const auto index = static_cast<usize>(slot->as_i64());
        if (index >= slots.size()) {
            continue;
        }
        const auto item = registries->protocol_id(*item_registry, id->as_string());
        if (!item) {
            continue;  // an item this version does not have
        }
        slots[index] = net::ItemStack{*item, static_cast<i8>(count->as_i64()), {}};
    }
    return slots;
}

/// Write the slots back, in the shape vanilla reads.
void set_chest_items(nbt::Tag& data, std::span<const net::ItemStack> slots,
                     const registry::Registries*         registries,
                     std::optional<registry::RegistryId> item_registry) {
    nbt::Tag items = nbt::Tag::make_list(nbt::TagType::Compound);
    if (registries != nullptr && item_registry) {
        for (usize i = 0; i < slots.size(); ++i) {
            if (slots[i].empty()) {
                continue;  // empty slots are omitted, not stored as air
            }
            const std::string_view name = registries->entry_of(*item_registry, slots[i].item_id);
            if (name.empty()) {
                continue;
            }
            nbt::Tag entry = nbt::Tag::make_compound();
            entry.compound()->push_back(nbt::CompoundEntry{"Slot", nbt::Tag{static_cast<i8>(i)}});
            entry.compound()->push_back(nbt::CompoundEntry{"id", nbt::Tag{std::string{name}}});
            entry.compound()->push_back(nbt::CompoundEntry{"Count", nbt::Tag{slots[i].count}});
            items.list()->push_back(std::move(entry));
        }
    }
    if (data.compound() == nullptr) {
        data = nbt::Tag::make_compound();
    }
    std::erase_if(*data.compound(), [](const nbt::CompoundEntry& e) { return e.name == "Items"; });
    data.compound()->push_back(nbt::CompoundEntry{"Items", std::move(items)});
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

    /// The player's own 46 slots, as the protocol numbers them: 0 is the
    /// crafting result, 1..4 the grid, 5..8 armour, 9..35 the main inventory,
    /// 36..44 the hotbar, 45 the off hand.
    ///
    /// In creative the client owns this and tells us about every change, which
    /// is the only way to know what a player is holding.
    std::array<net::ItemStack, 46> inventory{};
    i16                            held_slot{0};

    /// The container this player has open, if any.
    u8             window_id{0};
    bool           window_open{false};
    net::ItemStack carried{};
    i32            window_x{0};
    i32            window_y{0};
    i32            window_z{0};
    f64            x{0.5};
    f64            y{static_cast<f64>(Superflat::kSurfaceY) + 1.0};
    f64            z{0.5};
    f32            yaw{0.0F};
    f32            pitch{0.0F};

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

    // The menu type a chest opens. minecraft:menu is another registry the
    // client hard-codes, so a 9x3 chest is not the same number as a 9x6 one and
    // guessing opens a window of the wrong size over the right data.
    const auto menu_registry = registries ? registries->find("minecraft:menu") : std::nullopt;
    const i32  menu_generic_9x3 =
        registries && menu_registry
            ? registries->protocol_id(*menu_registry, "minecraft:generic_9x3").value_or(2)
            : 2;

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
                chunk_x, chunk_z);

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
            if (const auto it = players.find(connection.get()); it != players.end()) {
                const i32       entity_id = it->second.entity_id;
                const net::Uuid uuid      = it->second.uuid;
                players.erase(it);

                // After the erase, so the leaving player is not sent their own
                // removal on a socket that is already closing.
                broadcast(nullptr, net::clientbound::kRemoveEntities,
                          net::encode_remove_entity(entity_id));
                broadcast(nullptr, net::clientbound::kPlayerInfoRemove,
                          net::encode_player_info_remove(uuid));
            }
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
                player.name       = login->name;
                player.uuid       = uuid;
                {
                    const std::scoped_lock lock{players_mutex};

                    // Everyone already here learns about the newcomer, and the
                    // newcomer learns about them. Both halves are needed: doing
                    // only the first leaves the new player alone in a world
                    // other people are walking around in.
                    broadcast(nullptr, net::clientbound::kPlayerInfoUpdate,
                              net::encode_player_info_add(player.uuid, player.name, 1));
                    broadcast(
                        nullptr, net::clientbound::kSpawnPlayer,
                        net::encode_spawn_player(player.entity_id, player.uuid, player.x, player.y,
                                                 player.z, player.yaw, player.pitch));

                    for (const auto& [key, other] : players) {
                        send_packet(net::clientbound::kPlayerInfoUpdate,
                                    net::encode_player_info_add(other.uuid, other.name, 1));
                        send_packet(
                            net::clientbound::kSpawnPlayer,
                            net::encode_spawn_player(other.entity_id, other.uuid, other.x, other.y,
                                                     other.z, other.yaw, other.pitch));
                    }

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

                        // Clicking a chest opens it rather than placing against
                        // it. Vanilla only places when the player is sneaking,
                        // which is not tracked yet, so the chest always wins —
                        // stated rather than silently surprising.
                        {
                            const std::scoped_lock chunk_lock{chunk_mutex};
                            world::Chunk&          clicked_chunk =
                                chunk_at(place->position.x >> 4, place->position.z >> 4);
                            const world::BlockEntity* entity = clicked_chunk.block_entity_at(
                                static_cast<usize>(place->position.x & 15), place->position.y,
                                static_cast<usize>(place->position.z & 15));
                            if (entity != nullptr && entity->type == "minecraft:chest") {
                                player.window_id   = 1;
                                player.window_open = true;
                                player.window_x    = place->position.x;
                                player.window_y    = place->position.y;
                                player.window_z    = place->position.z;

                                std::vector<net::ItemStack> slots;
                                slots.reserve(63);
                                const auto chest =
                                    chest_items(entity->data, registries ? &*registries : nullptr,
                                                item_registry);
                                slots.insert(slots.end(), chest.begin(), chest.end());
                                // Then the player's own 27 main slots and 9
                                // hotbar slots, in that order: a window shows
                                // the container first and the player second.
                                for (usize i = 9; i < 45; ++i) {
                                    slots.push_back(player.inventory[i]);
                                }

                                send_packet(net::clientbound::kOpenScreen,
                                            net::encode_open_screen(player.window_id,
                                                                    menu_generic_9x3, "Chest"));
                                send_packet(
                                    net::clientbound::kContainerContent,
                                    net::encode_container_content(player.window_id, 1, slots, {}));
                                return true;
                            }
                        }

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

                        // The clicked block is not where the new one goes: the
                        // face says which side, and it lands one step along it.
                        const auto target = net::offset_by_face(place->position, place->face);
                        set_block_and_broadcast(
                            target, placed_state(*blocks, *held_block, *place, player.yaw));

                        // A sign needs a block entity to hold its text, and the
                        // editor has to be opened or it can never be written on.
                        const std::string_view block_name = blocks->block_name(*held_block);

                        if (block_name == "minecraft:chest" && registries &&
                            block_entity_registry) {
                            const auto type_id =
                                registries->protocol_id(*block_entity_registry, "minecraft:chest");
                            const std::scoped_lock chunk_lock{chunk_mutex};
                            world::Chunk& target_chunk = chunk_at(target.x >> 4, target.z >> 4);

                            world::BlockEntity entity;
                            entity.x       = static_cast<u8>(target.x & 15);
                            entity.y       = target.y;
                            entity.z       = static_cast<u8>(target.z & 15);
                            entity.type    = "minecraft:chest";
                            entity.type_id = type_id.value_or(0);
                            entity.data    = nbt::Tag::make_compound();
                            set_chest_items(entity.data, std::array<net::ItemStack, 27>{},
                                            &*registries, item_registry);
                            target_chunk.set_block_entity(std::move(entity));
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
                        player.window_open = false;
                        return true;
                    }

                    case net::serverbound::kClickContainer: {
                        const auto click = net::parse_container_click(body);
                        if (!click || !player.window_open || click->window_id != player.window_id) {
                            return true;
                        }

                        const std::scoped_lock chunk_lock{chunk_mutex};
                        world::Chunk&          chest_chunk =
                            chunk_at(player.window_x >> 4, player.window_z >> 4);
                        world::BlockEntity* entity = chest_chunk.block_entity_at(
                            static_cast<usize>(player.window_x & 15), player.window_y,
                            static_cast<usize>(player.window_z & 15));
                        if (entity == nullptr || entity->type != "minecraft:chest") {
                            player.window_open = false;
                            return true;
                        }

                        auto chest = chest_items(entity->data, registries ? &*registries : nullptr,
                                                 item_registry);

                        // Slot numbering inside the window: 0..26 the chest,
                        // 27..53 the player's main inventory, 54..62 the hotbar.
                        // The player's own slots are 9..44, so the mapping is
                        // not the identity and getting it wrong moves items
                        // between the wrong two places.
                        const auto slot_ref = [&](i16 index) -> net::ItemStack* {
                            if (index >= 0 && index < 27) {
                                return &chest[static_cast<usize>(index)];
                            }
                            if (index >= 27 && index < 63) {
                                return &player.inventory[static_cast<usize>(index - 27 + 9)];
                            }
                            return nullptr;
                        };

                        bool handled = false;
                        if (click->mode == 0 && click->slot >= 0) {
                            net::ItemStack* slot = slot_ref(click->slot);
                            if (slot != nullptr) {
                                if (click->button == 0) {
                                    // Left click: the carried stack and the slot
                                    // trade places.
                                    std::swap(*slot, player.carried);
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
                                    } else if (slot->empty()) {
                                        *slot       = player.carried;
                                        slot->count = 1;
                                        player.carried.count =
                                            static_cast<i8>(player.carried.count - 1);
                                        if (player.carried.count <= 0) {
                                            player.carried = {};
                                        }
                                    }
                                    handled = true;
                                }
                            }
                        }

                        if (handled) {
                            set_chest_items(entity->data, chest,
                                            registries ? &*registries : nullptr, item_registry);
                            dirty_chunks.insert(
                                chunk_key(player.window_x >> 4, player.window_z >> 4));
                        }

                        // Whether the click was handled or not, the window is
                        // resent in full. The client has already applied its own
                        // guess; anything the server did not implement — shift
                        // clicks, drags, number keys — would otherwise stay on
                        // screen as an item that does not exist.
                        std::vector<net::ItemStack> slots;
                        slots.reserve(63);
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
