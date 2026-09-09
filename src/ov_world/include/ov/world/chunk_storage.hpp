// Chunks to and from the Anvil chunk format.
//
// The disk format is **name-based**, and that is the whole reason a save can be
// shared between implementations at all. On the wire a palette entry is a
// numeric state id; on disk it is `{Name: "minecraft:oak_stairs", Properties:
// {facing: "north", half: "bottom", ...}}`. Writing wire ids to disk produces a
// file that loads perfectly here and is meaningless in Minecraft — and the
// version it was written by is the only thing that could ever decode it.
//
// So the conversion goes through the registry in both directions, and a state
// whose name the registry does not know is refused rather than guessed.
//
// Biome names are passed in rather than looked up. Biomes live in a registry the
// server *sends*, so their numeric ids belong to whoever built the codec; this
// module has no business knowing which id is which and would be wrong the moment
// a datapack added one.
#pragma once

#include "ov/nbt/binary.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/block_ticks.hpp"
#include "ov/world/chunk.hpp"

#include <optional>
#include <span>
#include <string_view>

namespace ov::world {

/// The DataVersion a 1.20.1 save carries.
inline constexpr i32 kDataVersion1201 = 3465;

/// Everything the conversion needs beyond the chunk itself.
struct ChunkCodecContext {
    const registry::BlockRegistry* blocks{nullptr};

    /// Biome names indexed by the numeric id used in memory.
    std::span<const std::string_view> biome_names;

    /// Needed to turn a block entity's stored **name** back into the numeric id
    /// the wire carries. Disk names them, the protocol numbers them, and a
    /// chunk loaded without this would be sent with every block entity typed as
    /// whatever id zero happens to be.
    const registry::Registries* registries{nullptr};

    AirStates air;

    /// The scheduled ticks falling inside the chunk being written, and the tick
    /// they are to be made relative to.
    ///
    /// Passed in rather than held on the chunk because the queues belong to the
    /// **level**: vanilla addresses a tick by absolute position and only sorts
    /// it into a chunk when that chunk is saved. A caller with no scheduler
    /// leaves both spans empty and writes the two empty lists, which is what
    /// this did unconditionally before there was anything to put in them.
    ///
    /// Filter with `ticks_to_nbt`, which takes the chunk's coordinates: handing
    /// the whole level's queue here would write every pending tick in the world
    /// into every region file.
    std::span<const ScheduledTick> block_ticks{};
    std::span<const ScheduledTick> fluid_ticks{};

    /// What `t` is measured from. A tick is stored as a **delay relative to the
    /// chunk's game time**, which is what lets a chunk sit unloaded and resume
    /// where it left off.
    i64 game_time{0};
};

/// The DataVersion a chunk file declares, if it declares one.
///
/// Checked before anything else is read. A file from another version may use
/// shapes this one does not have, and vanilla upgrades old saves through a
/// converter this project does not implement — so the only honest answers are
/// "this version" and "refuse".
[[nodiscard]] std::optional<i32> chunk_data_version(const nbt::Document& document);

/// Serialise a chunk to the Anvil chunk NBT.
///
/// Light is written out because recomputing it on load is expensive and, with
/// no light engine on the read path, would silently differ from what the client
/// was last shown.
[[nodiscard]] nbt::Document to_nbt(const Chunk& chunk, const ChunkCodecContext& context);

/// Rebuild a chunk from Anvil chunk NBT.
///
/// Returns nothing if the document is not a chunk, names a block this version
/// does not have, or declares a section shape that does not match its data. A
/// chunk that cannot be trusted is better absent — the generator will make a
/// fresh one — than half-read.
[[nodiscard]] std::optional<Chunk> from_nbt(const nbt::Document&     document,
                                            const ChunkCodecContext& context);

}  // namespace ov::world
