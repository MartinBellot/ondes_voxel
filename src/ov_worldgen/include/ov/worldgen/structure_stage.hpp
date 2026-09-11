// The structure blocks, at the `features` status: the point where the pieces
// decided at `structure_starts` finally become blocks.
//
// The game places structures chunk by chunk. When a chunk decorates, every
// structure start whose box crosses it writes the part of its pieces that falls
// inside *that chunk's column*, before the features of the same generation
// step. A ship lying across two chunks is written by both, each writing its own
// half. The reads are wider than the writes — a piece settles on terrain, a
// fence joins a neighbour, a processor asks the world what is there — so the
// stage works over the pipeline's three-by-three neighbourhood and writes only
// the middle.
//
// Two things make that order-independent, both measured rather than assumed:
//
//   * every random draw of the template layer is keyed on a world position,
//     and the only other random — the one the loot seeds come from — is the
//     chunk's own `setFeatureSeed(decoration, index, step)`, with the index the
//     structure's rank by name within its step;
//   * a block whose shape follows its neighbours (stairs, fences, doors) is
//     updated again whenever a neighbouring chunk is placed, the way the game's
//     post-processing reaches it once the neighbour exists.
//
// What the stage does *not* reproduce is named in `StructureStageStats`: a
// start the builder refuses (a kind it does not build) or builds only in part
// is counted by reason, never dropped in silence.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/chunk.hpp"
#include "ov/worldgen/structure.hpp"
#include "ov/worldgen/structure_pieces.hpp"

#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ov::worldgen {

struct StructureStageStats {
    /// Starts built, and starts the builder refused, by reason.
    u64                        starts_built{0};
    std::map<std::string, u64> refused;
    /// Starts built in part, by what is missing.
    std::map<std::string, u64> incomplete;
    /// Piece placements (one piece, one chunk) and the blocks they wrote.
    u64 placements{0};
    u64 blocks_written{0};
};

class StructureStage {
public:
    /// All borrowed; they must outlive the stage. `sampler` answers heights for
    /// columns outside the neighbourhood and the placer's biome filter; it may
    /// be null. `registries` resolves block entity ids for the wire; it may be
    /// null, and the ids are then zero.
    StructureStage(const StructurePlacer& placer, const StructureBuilder& builder,
                   const StructureWorldSampler* sampler, const registry::BlockRegistry& blocks,
                   const registry::Registries* registries, i64 level_seed);

    StructureStage(const StructureStage&)            = delete;
    StructureStage& operator=(const StructureStage&) = delete;
    ~StructureStage();

    /// Write the structures crossing the middle chunk of a neighbourhood.
    ///
    /// `neighbourhood` is row-major, `[(dz + 1) * 3 + (dx + 1)]`, the middle
    /// chunk at index 4; any neighbour may be null. `sea_level` is the
    /// dimension's.
    void place(std::span<world::Chunk* const, 9> neighbourhood, i32 chunk_x, i32 chunk_z,
               i32 sea_level);

    /// The starts whose start chunk is this one, built on first use.
    [[nodiscard]] const std::vector<StructureStart>& starts_at(i32 chunk_x, i32 chunk_z);

    /// Put a start in by hand, replacing whatever the chunk had. For tests and
    /// harnesses that place the game's own pieces.
    void add_start(StructureStart start);

    /// Forget cached starts and pending shape updates far from `centre`.
    void trim(i32 centre_x, i32 centre_z, i32 keep);

    [[nodiscard]] const StructureStageStats& stats() const noexcept;

    /// How far, in chunks, a start's pieces can reach from its start chunk.
    /// Three covers every kind this stage builds: a ship is at most 28 blocks
    /// long and lies from its chunk corner, an ocean ruin cluster is not built.
    static constexpr i32 kReach = 3;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ov::worldgen
