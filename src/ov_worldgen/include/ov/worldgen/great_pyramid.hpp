// The Great Pyramid — an ORIGINAL Ondes VOXEL structure, not Minecraft content.
//
// Product decision (2026-09-11, docs/ARCHITECTURE.md § 9): the user asked for a
// colossal pyramid of our own in the desert, with passages, *added* to vanilla.
// The vanilla `minecraft:desert_pyramid` is untouched and stays exactly 1.20.1.
// Everything here lives under the `ondes_voxel:` namespace — the structure, its
// set, its pieces and its loot tables — and is never presented as official.
//
// It is always on in generated worlds. The one exception is the generator-level
// switch `OriginalStructures`: the parity instruments (ov_structblocks,
// ov_netherparity, the parity tests, and any export made under
// `OV_ORIGINAL_STRUCTURES=0`) turn it off, so that what they measure against
// the real game is the vanilla generator and nothing else.
//
// ── Shape ───────────────────────────────────────────────────────────────────
//
// A stepped pyramid, 101 x 101 at the base and 51 layers, on a 105 x 105
// sandstone plinth levelled at the median of thirteen terrain samples. Inside,
// in the pyramid's own frame (the entrance on the -v face, `u` across it):
// the grand entrance, a hypostyle hall, the grand gallery climbing to the
// Pharaoh's chamber behind a piston door, the Queen's chamber off a
// tripwire-guarded passage, a ring labyrinth one level up, and a crypt below the
// plinth reached by a hidden stair. docs/provenance/grande-pyramide.md has the
// plan, the numbers and the measurements.
//
// ── Determinism ─────────────────────────────────────────────────────────────
//
// Whether a chunk starts a pyramid, and every choice inside it (the facing,
// the maze, the traps, the loot seeds), is a pure function of the world seed,
// the start chunk and the generator's noise — decided once from the start's
// own random, `setLargeFeatureSeed(seed, chunkX, chunkZ)`. The blocks are then
// written chunk by chunk, each chunk its own column and nothing else, and the
// only thing a chunk reads is its own column (the fill under the plinth). So
// the result does not depend on the order chunks are generated in.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/nbt/tag.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/worldgen/structure.hpp"
#include "ov/worldgen/structure_set.hpp"
#include "ov/worldgen/structure_template.hpp"

#include <array>
#include <expected>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ov::worldgen {

/// The generator-level switch for the structures that are ours and not the
/// game's. Not a player option: a world is generated with them. The parity
/// instruments — and only they — ask for `vanilla_parity()`.
struct OriginalStructures {
    bool great_pyramid{true};

    [[nodiscard]] static constexpr OriginalStructures vanilla_parity() noexcept { return {false}; }
    [[nodiscard]] static constexpr OriginalStructures all() noexcept { return {true}; }
};

inline constexpr std::string_view kGreatPyramidId  = "ondes_voxel:great_pyramid";
inline constexpr std::string_view kGreatPyramidSet = "ondes_voxel:great_pyramids";

/// The set's grid: `random_spread`, linear, its own salt. Mirrored by
/// `data/ondes_voxel/worldgen/structure_set/great_pyramids.json`, and a test
/// keeps the two equal.
[[nodiscard]] RandomSpreadPlacement great_pyramid_placement() noexcept;

/// Why a chunk does or does not start a pyramid.
enum class PyramidDecision : u8 {
    /// The set's grid picked another chunk of this cell.
    NotCandidate,
    /// No sampler: the terrain and biome gates cannot be asked.
    NoSampler,
    /// The chunk's middle, or too much of the footprint, is not desert.
    NotDesert,
    /// More than `kMaxSpread` blocks between the lowest and highest sample.
    TooSteep,
    /// The plinth would sit at or under the sea.
    TooLow,
    /// A vanilla structure could start under or beside it.
    NearVanilla,
    Placed,
};

[[nodiscard]] std::string_view to_string(PyramidDecision decision) noexcept;

/// What sits at the end of a dead end of the labyrinth.
enum class DeadEnd : u8 { Empty, TntTrap, ArrowTrap, Chest };

/// One start: everything the blocks are a function of. Canonical coordinates
/// `(u, y, v)` are relative to the pyramid's centre column and floor, with the
/// entrance on the `v = -50` face; `world()` turns them into block positions.
struct GreatPyramidLayout {
    i32 chunk_x{0};
    i32 chunk_z{0};
    /// The centre column, in blocks: the start chunk's middle.
    i32 centre_x{0};
    i32 centre_z{0};
    /// World y of canonical y = 0: the first free block above the plinth.
    i32 base_y{0};
    /// Which side the entrance faces: 0 north, 1 east, 2 south, 3 west.
    u8 facing{0};
    /// Stairs down the causeway from the plinth to the sand.
    i32 causeway_steps{0};

    /// The thirteen terrain samples the plinth was levelled on, for reports.
    std::array<i32, 13> samples{};

    /// The labyrinth: 30 x 30 cells at odd canonical coordinates -29..29. Per
    /// cell, bit 0 = passage towards +u, bit 1 = passage towards +v, bit 2 =
    /// the cell belongs to the maze (the ring outside the core).
    std::vector<u8>      maze;
    std::vector<DeadEnd> dead_end_kind;  ///< per cell, Empty unless a dead end holds something
    std::vector<i64>     dead_end_seed;  ///< per cell, the corridor chest's loot seed

    std::array<i64, 4> treasure_seeds{};
    std::array<i64, 2> queen_seeds{};
    std::array<i64, 2> crypt_seeds{};
    /// Suspicious sand in the crypt's pit: canonical (u, v) and loot seed.
    std::vector<std::array<i32, 2>> suspicious;
    std::vector<i64>                suspicious_seeds;

    /// Everything the structure writes, in world blocks.
    BoundingBox box;

    /// Canonical to world.
    [[nodiscard]] BlockPos world(i32 u, i32 y, i32 v) const noexcept;
    /// A canonical direction (0 -v, 1 +u, 2 +v, 3 -u) as a world facing index
    /// (0 north, 1 east, 2 south, 3 west).
    [[nodiscard]] u8 world_facing(u8 canonical) const noexcept {
        return static_cast<u8>((canonical + facing) & 3U);
    }

    // The labyrinth, queried by cell index (i, j) in [0, 30).
    static constexpr i32 kMazeCells = 30;
    [[nodiscard]] static constexpr usize cell(i32 i, i32 j) noexcept {
        return static_cast<usize>(j * kMazeCells + i);
    }
    [[nodiscard]] bool in_maze(i32 i, i32 j) const noexcept;
    /// Is there a passage between cell (i, j) and its neighbour in direction
    /// `dir` (canonical, as for `world_facing`)?
    [[nodiscard]] bool open(i32 i, i32 j, u8 dir) const noexcept;
};

/// The canonical cell the labyrinth's stair from the hall arrives in.
inline constexpr std::array<i32, 2> kMazeEntry{5, 8};  // canonical (-19, -13)

/// Decides and builds the pyramid. Immutable after `create`; every query is
/// `const` with its scratch on the caller's stack, so one may be shared.
class GreatPyramid {
public:
    /// Resolve the blocks it is made of. Fails naming the first block or
    /// property the registry does not have.
    [[nodiscard]] static std::expected<GreatPyramid, std::string> create(
        const registry::BlockRegistry& blocks);

    /// Does chunk `(chunk_x, chunk_z)` start a pyramid? Fills `out` when it
    /// does. `placer` may be null, and the vanilla-overlap gate is then not
    /// applied (tests); `sampler` may not.
    [[nodiscard]] PyramidDecision decide(i64 level_seed, i32 chunk_x, i32 chunk_z,
                                         const StructureWorldSampler* sampler,
                                         const StructurePlacer*       placer,
                                         GreatPyramidLayout*          out) const;

    /// Write the part of the pyramid inside `clip` (a chunk column). Reads
    /// only inside `clip` too. Returns the blocks written.
    u64 place(StructureLevel& level, const GreatPyramidLayout& layout,
              const BoundingBox& clip) const;

    /// The start as the chunk format stores it: `{id, ChunkX, ChunkZ,
    /// references, Children}`, one child per room.
    [[nodiscard]] static nbt::Tag start_to_nbt(const GreatPyramidLayout& layout);

    /// The largest spread between terrain samples a site may have.
    static constexpr i32 kMaxSpread = 14;
    /// How far, in chunks, a start's blocks reach from its start chunk.
    static constexpr i32 kReach = 5;

    GreatPyramid(GreatPyramid&&) noexcept;
    GreatPyramid& operator=(GreatPyramid&&) noexcept;
    ~GreatPyramid();

    /// Defined in the implementation; public so its three files can share it.
    struct Impl;

private:
    GreatPyramid();
    std::unique_ptr<Impl> impl_;
};

/// The pyramids of one worldgen stack: starts decided on first use and cached,
/// and the part crossing each decorated chunk written into it. One per
/// `StructureStage`, so one per thread that generates; nothing shared.
class GreatPyramidStage {
public:
    /// All borrowed. `placer` may be null (no vanilla-overlap gate).
    GreatPyramidStage(const registry::BlockRegistry& blocks, const StructurePlacer* placer,
                      const StructureWorldSampler* sampler, i64 level_seed);
    GreatPyramidStage(const GreatPyramidStage&)            = delete;
    GreatPyramidStage& operator=(const GreatPyramidStage&) = delete;
    ~GreatPyramidStage();

    /// Whether the blocks could be resolved. A stage that is not ready places
    /// nothing, and says why once.
    [[nodiscard]] bool ready() const noexcept;

    /// Write the pyramids crossing chunk `(chunk_x, chunk_z)` — its column
    /// only.
    void place(StructureLevel& level, i32 chunk_x, i32 chunk_z);

    /// The pyramid starting in this chunk, or null.
    [[nodiscard]] const GreatPyramidLayout* start_at(i32 chunk_x, i32 chunk_z);

    /// Add to a chunk's `structures` compound (the one
    /// `chunk_structures_to_nbt` builds) the pyramid starting here and a
    /// reference to every pyramid crossing it. True when anything was added.
    bool record(i32 chunk_x, i32 chunk_z, nbt::Tag& structures);

    void trim(i32 centre_x, i32 centre_z, i32 keep);
    void clear();

    struct Stats {
        u64                              decided{0};
        std::map<PyramidDecision, u64>   decisions;
        u64                              placements{0};
        u64                              blocks_written{0};
    };
    [[nodiscard]] const Stats& stats() const noexcept;

    [[nodiscard]] const GreatPyramid* pyramid() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ov::worldgen
