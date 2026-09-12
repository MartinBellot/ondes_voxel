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
//     race. N stacks costs memory and buys a design with no locks in it at all.
//     (The terrain cache every stack of one world shares is the exception, and
//     it holds only finished, never-written terrain: terrain_cache.hpp.)
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
// ── streaming: which block next ─────────────────────────────────────────────
//
// A worker does not get a block when the block is asked for; it takes the best
// block **when it becomes free**. The tick thread hands over, every tick, the
// chunks the tickets want and the map has not got, nearest first
// (`prioritise`); a free worker takes the first block of that list that nobody
// is generating. Two things follow, both measured in
// docs/provenance/chargement-terrain.md:
//
//   * **nearest first at the moment the work starts**, not at the moment it
//     was asked for — a player who flew on in the meantime is served where
//     they are now;
//   * **cancellation for free**: a block that was wanted and is not any more is
//     simply not in the next list, and is never started. Only a block already
//     running is finished (a job cannot be interrupted halfway through a
//     decoration, and its chunks are the ones the player just left: cheap to
//     keep, published as usual).
#pragma once

#include "generated_world.hpp"
#include "ov/base/thread.hpp"
#include "ov/base/types.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/world/chunk.hpp"

#include <functional>
#include <memory>
#include <span>
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
    /// generate more chunks nobody asked for; smaller blocks do the reverse. It
    /// is also part of the world's definition: what a chunk contains depends on
    /// the square it was decorated in, so changing it changes the world.
    static constexpr i32 kBlockChunks = 4;

    [[nodiscard]] static constexpr i32 block_of(i32 chunk_coordinate) noexcept {
        return chunk_coordinate >= 0 ? chunk_coordinate / kBlockChunks
                                     : -(((-chunk_coordinate) + kBlockChunks - 1) / kBlockChunks);
    }

    /// Generate one square on a stack: `(stack, origin_x, origin_z, side, out)`,
    /// the shape of `GeneratedWorld::generate_square`.
    using Generate = std::function<void(usize, i32, i32, i32,
                                        std::vector<std::pair<ChunkPos, world::Chunk>>&)>;

    /// `world` is borrowed and must outlive this. It must have been loaded with
    /// at least `workers + 1` stacks, one per worker plus the tick thread's own.
    /// `role` is the scheduling class of the workers (see thread.hpp and
    /// docs/provenance/chargement-terrain.md for why it is not a detail).
    AsyncChunkSource(GeneratedWorld& world, usize workers, ThreadRole role = ThreadRole::Worker);

    /// Any generator: the tests' stand-in, whose blocks cost nothing and which
    /// records the order it was asked in. Worker `i` is handed stack `i + 1`.
    AsyncChunkSource(Generate generate, usize workers, ThreadRole role = ThreadRole::Worker);

    AsyncChunkSource(const AsyncChunkSource&)            = delete;
    AsyncChunkSource& operator=(const AsyncChunkSource&) = delete;
    ~AsyncChunkSource();

    /// ── streaming ── What the workers should do next, best first.
    ///
    /// `wanted` is the chunks the tickets want and the map has not got, in the
    /// order they should arrive. Replaces the previous list: a block that is
    /// not in it any more is never started. Blocks running, or finished and not
    /// yet drained, are skipped. Call from the tick thread, once a tick.
    void prioritise(std::span<const ChunkPos> wanted);

    /// Ask for the block containing this chunk, behind whatever is waiting.
    ///
    /// Returns false when the request was refused — the block is already
    /// waiting, running or finished, or the waiting list is at its limit. The
    /// determinism check drives the source this way; the server uses
    /// `prioritise`.
    bool request(ChunkPos pos);

    /// Move every finished block out. Call from the tick thread only.
    ///
    /// Returns how many blocks were moved. `out` is appended to, not cleared,
    /// so a caller can reuse one vector across ticks without reallocating.
    usize drain(std::vector<GeneratedBlock>& out);

    /// Blocks waiting or running.
    [[nodiscard]] usize in_flight() const;

    /// Blocks finished since start-up, and the chunks they carried.
    [[nodiscard]] u64 blocks_done() const;
    [[nodiscard]] u64 chunks_done() const;

    /// Seconds of worker time spent generating, summed over every worker.
    [[nodiscard]] f64 busy_seconds() const;

    [[nodiscard]] usize worker_count() const noexcept;

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
