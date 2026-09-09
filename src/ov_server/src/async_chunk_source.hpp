// Terrain generated off the tick thread, published on it.
//
// The measured problem this file exists for: generating one chunk of real
// overworld costs a few hundred milliseconds against a tick budget of fifty, so
// a server that generated on the tick thread logged "can't keep up" once a
// second from the moment a player joined and never delivered the whole view
// distance. See docs/provenance/chunkmap.md for the before and after numbers.
//
// ── The three invariants ────────────────────────────────────────────────────
//
//  1. **Nothing shared is mutable.** Every worker owns a whole worldgen stack —
//     its own noise router, its own surface rules, its own pipeline. That is
//     not thrift being ignored: the router and the interpolated density nodes
//     keep `mutable` memo caches, so one router driven by two threads is a data
//     race, and `src/ov_worldgen/` is not this file's to change. N stacks costs
//     memory and buys a design with no locks in it at all.
//
//  2. **The tick thread is the only writer of a published chunk.** A worker
//     builds chunks nothing else can see, moves them into a result queue, and
//     forgets them. `drain()` moves them out on the tick thread, which is the
//     only caller allowed to hand them to `world::ChunkMap::publish`.
//
//  3. **The unit of work is a block of chunks, generated from a cold cache.**
//     This is what makes the result deterministic, and it is the part that is
//     easy to get wrong. `ChunkPipeline::promote` decorates neighbours, and a
//     chunk that has already been decorated is not decorated again — so what a
//     chunk finally contains depends on what its pipeline had already been
//     asked for. Per-chunk requests on a warm shared cache therefore give a
//     world that depends on the order players happened to walk in. Clearing the
//     cache, generating a fixed square in a fixed order, and clearing it again
//     makes the block's contents a pure function of the seed and the block's
//     coordinates — the same on one worker or eight, in any order, on any run.
//
// The cost of invariant 3 is measured rather than assumed: a block of
// `kBlockChunks` squared output chunks needs `(kBlockChunks + 4)` squared
// terrain generations and `(kBlockChunks + 2)` squared decorations, and
// docs/provenance/chunkmap.md gives the numbers for the size chosen.
#pragma once

#include "generated_world.hpp"
#include "ov/base/types.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/world/chunk.hpp"

#include <memory>
#include <utility>
#include <vector>

namespace ov::server {

/// One finished square of world, on its way to the tick thread.
struct GeneratedBlock {
    /// Block coordinates, i.e. chunk coordinates divided by `kBlockChunks`.
    i32 block_x{0};
    i32 block_z{0};

    std::vector<std::pair<ChunkPos, world::Chunk>> chunks;
};

class AsyncChunkSource {
public:
    /// The side of the square a single job produces, in chunks.
    ///
    /// Four is a measured compromise, not a round number — see
    /// docs/provenance/chunkmap.md § 3. Larger blocks amortise the support ring
    /// better (a block of side S pays (S+4)^2 terrain for S^2 chunks) and
    /// generate more chunks nobody asked for; smaller blocks do the reverse. At
    /// four, filling a view distance of eight generates about twice the chunks
    /// the client is waiting for, and every one of them is a chunk the player
    /// walks into next.
    static constexpr i32 kBlockChunks = 4;

    [[nodiscard]] static constexpr i32 block_of(i32 chunk_coordinate) noexcept {
        return chunk_coordinate >= 0 ? chunk_coordinate / kBlockChunks
                                     : -(((-chunk_coordinate) + kBlockChunks - 1) / kBlockChunks);
    }

    /// `world` is borrowed and must outlive this. It must have been loaded with
    /// at least `workers` stacks, one per worker plus the tick thread's own.
    AsyncChunkSource(GeneratedWorld& world, usize workers);

    AsyncChunkSource(const AsyncChunkSource&)            = delete;
    AsyncChunkSource& operator=(const AsyncChunkSource&) = delete;
    ~AsyncChunkSource();

    /// Ask for the block containing this chunk.
    ///
    /// Returns false when the request was refused — either the block is already
    /// queued or running, or the queue is at its depth limit. A refusal is not
    /// an error: the caller asks again next tick, which is what keeps the queue
    /// tracking where the player is now rather than where they were a minute
    /// ago.
    bool request(ChunkPos pos);

    /// Move every finished block out. Call from the tick thread only.
    ///
    /// Returns how many blocks were moved. `out` is appended to, not cleared,
    /// so a caller can reuse one vector across ticks without reallocating.
    usize drain(std::vector<GeneratedBlock>& out);

    /// Blocks queued or running.
    [[nodiscard]] usize in_flight() const;

    /// Blocks finished since start-up, and the chunks they carried.
    [[nodiscard]] u64 blocks_done() const;
    [[nodiscard]] u64 chunks_done() const;

    /// Generate one block on the calling thread, bypassing the pool.
    ///
    /// Exists for the determinism check: the same block, generated serially
    /// here and through the pool above, must come back cell for cell identical.
    void generate_here(usize stack, i32 block_x, i32 block_z, GeneratedBlock& out);

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

}  // namespace ov::server
