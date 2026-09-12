#define OV_LOG_CATEGORY "server"

#include "async_chunk_source.hpp"

#include "ov/base/job_pool.hpp"
#include "ov/base/log.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_set>
#include <utility>

namespace ov::server {

namespace {

/// How many blocks may wait, per worker, when they are asked for one by one
/// (`request`). The server's `prioritise` replaces the whole list every tick
/// and is bounded by `kMaxWaiting` instead.
constexpr usize kQueueDepthPerWorker = 3;

/// The longest waiting list `prioritise` keeps. A view distance of eight is
/// at most 36 blocks; past this the list is only the far edge of several
/// players at once, which the next tick will re-rank anyway.
constexpr usize kMaxWaiting = 96;

[[nodiscard]] u64 block_key(i32 block_x, i32 block_z) noexcept {
    return ChunkPos{block_x, block_z}.packed();
}

}  // namespace

struct AsyncChunkSource::Impl {
    Generate generate;

    /// Guards the lists below — and nothing else. There is no chunk behind this
    /// mutex that anyone can see: a worker builds into its own vector and hands
    /// the whole thing over by move. The world itself is never locked
    /// (CLAUDE.md § 2, principle 3).
    mutable std::mutex          mutex;
    std::vector<u64>            waiting;   ///< not started, best first
    std::unordered_set<u64>     running;   ///< started, not finished
    std::unordered_set<u64>     finished;  ///< finished, not drained
    std::vector<GeneratedBlock> done;
    usize                       loops{0};  ///< worker loops alive

    /// `prioritise` scratch, kept so that a tick does not allocate once warm.
    std::vector<u64>        ranked;
    std::unordered_set<u64> seen;

    std::atomic<bool> stopping{false};
    std::atomic<u64>  blocks_done{0};
    std::atomic<u64>  chunks_done{0};
    std::atomic<u64>  busy_micros{0};

    /// Declared last, and that is load-bearing. Members are destroyed in
    /// reverse declaration order, so the pool — which joins its threads — goes
    /// first, before the lists a running job is about to write into. With the
    /// pool declared at the top, a job finishing during shutdown would push a
    /// block into a vector that had already been destroyed.
    base::JobPool pool;

    Impl(Generate g, usize workers, ThreadRole role) : generate(std::move(g)), pool(workers, role) {}

    /// How many worker loops may run at once. An inline pool (no threads) runs
    /// one loop, inside `submit`, on the caller.
    [[nodiscard]] usize loop_limit() const noexcept {
        return std::max<usize>(1, pool.worker_count());
    }

    /// Loops to start now, reserved under the lock; started after it is
    /// released, because an inline pool runs the loop inside `submit` and the
    /// loop takes this lock.
    [[nodiscard]] usize reserve_loops() {
        const usize target = std::min(loop_limit(), waiting.size());
        const usize start  = target > loops ? target - loops : 0;
        loops += start;
        return start;
    }

    void start_loops(usize count) {
        // `Impl*`, never the AsyncChunkSource: see the note on `~unique_ptr` in
        // docs/provenance/chunkmap.md § 13 — the pool joins inside `~Impl`.
        Impl* self = this;
        for (usize i = 0; i < count; ++i) {
            pool.submit([self](usize worker) { self->work(worker); });
        }
    }

    /// One worker: take the best waiting block, generate it, repeat until
    /// nothing is waiting.
    void work(usize worker) {
        while (!stopping.load(std::memory_order_relaxed)) {
            u64 key = 0;
            {
                const std::scoped_lock lock{mutex};
                if (waiting.empty()) {
                    --loops;
                    return;
                }
                key = waiting.front();
                waiting.erase(waiting.begin());
                running.insert(key);
            }
            const ChunkPos block = ChunkPos::from_packed(key);

            GeneratedBlock out;
            out.block_x = block.x;
            out.block_z = block.z;
            out.chunks.reserve(static_cast<usize>(kBlockChunks) * kBlockChunks);
            const auto started = std::chrono::steady_clock::now();
            // Worker index plus one: stack zero belongs to the tick thread's own
            // synchronous fallback, and two threads on one stack is the race.
            generate(worker + 1, block.x * kBlockChunks, block.z * kBlockChunks, kBlockChunks,
                     out.chunks);
            busy_micros.fetch_add(
                static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now() - started)
                                     .count()),
                std::memory_order_relaxed);

            const u64 count = out.chunks.size();
            {
                const std::scoped_lock lock{mutex};
                running.erase(key);
                finished.insert(key);
                done.push_back(std::move(out));
            }
            blocks_done.fetch_add(1, std::memory_order_relaxed);
            chunks_done.fetch_add(count, std::memory_order_relaxed);
        }
        const std::scoped_lock lock{mutex};
        --loops;
    }
};

AsyncChunkSource::AsyncChunkSource(GeneratedWorld& world, usize workers, ThreadRole role)
    : AsyncChunkSource(
          [&world](usize stack, i32 x, i32 z, i32 side,
                   std::vector<std::pair<ChunkPos, world::Chunk>>& out) {
              world.generate_square(stack, x, z, side, out);
          },
          workers, role) {
    if (world.stack_count() < workers + 1) {
        // Named rather than clamped silently. A worker index that runs past the
        // stacks would be two threads on one noise router, which is the exact
        // race this design exists to make impossible.
        OV_LOG_ERROR(
            "chunk source: {} workers but only {} worldgen stacks — refusing to run more "
            "workers than stacks",
            workers, world.stack_count());
    }
    OV_LOG_INFO("chunk source: {} workers, blocks of {}x{} chunks, nearest block first",
                impl_->pool.worker_count(), kBlockChunks, kBlockChunks);
}

AsyncChunkSource::AsyncChunkSource(Generate generate, usize workers, ThreadRole role)
    : impl_(std::make_unique<Impl>(std::move(generate), workers, role)) {}

AsyncChunkSource::~AsyncChunkSource() {
    // Stop the loops at their next pick: a shutdown that waits for the whole
    // waiting list is a hang, not a courtesy. A block already running finishes.
    impl_->stopping.store(true, std::memory_order_relaxed);
    {
        const std::scoped_lock lock{impl_->mutex};
        impl_->waiting.clear();
    }
}

void AsyncChunkSource::prioritise(std::span<const ChunkPos> wanted) {
    Impl& impl = *impl_;
    impl.ranked.clear();
    impl.seen.clear();
    for (const ChunkPos pos : wanted) {
        const u64 key = block_key(block_of(pos.x), block_of(pos.z));
        if (impl.seen.insert(key).second) {
            impl.ranked.push_back(key);
            if (impl.ranked.size() >= kMaxWaiting) {
                break;
            }
        }
    }

    usize start = 0;
    {
        const std::scoped_lock lock{impl.mutex};
        impl.waiting.clear();
        for (const u64 key : impl.ranked) {
            if (!impl.running.contains(key) && !impl.finished.contains(key)) {
                impl.waiting.push_back(key);
            }
        }
        start = impl.reserve_loops();
    }
    impl.start_loops(start);
}

bool AsyncChunkSource::request(ChunkPos pos) {
    Impl&       impl  = *impl_;
    const u64   key   = block_key(block_of(pos.x), block_of(pos.z));
    const usize limit = impl.loop_limit() * kQueueDepthPerWorker;

    usize start = 0;
    {
        const std::scoped_lock lock{impl.mutex};
        if (impl.running.contains(key) || impl.finished.contains(key) ||
            std::ranges::find(impl.waiting, key) != impl.waiting.end()) {
            return false;
        }
        if (impl.waiting.size() >= limit) {
            return false;
        }
        impl.waiting.push_back(key);
        start = impl.reserve_loops();
    }
    impl.start_loops(start);
    return true;
}

usize AsyncChunkSource::drain(std::vector<GeneratedBlock>& out) {
    std::vector<GeneratedBlock> taken;
    {
        const std::scoped_lock lock{impl_->mutex};
        if (impl_->done.empty()) {
            return 0;
        }
        taken.swap(impl_->done);
        for (const GeneratedBlock& block : taken) {
            impl_->finished.erase(block_key(block.block_x, block.block_z));
        }
    }

    const usize count = taken.size();
    for (GeneratedBlock& block : taken) {
        out.push_back(std::move(block));
    }
    return count;
}

usize AsyncChunkSource::in_flight() const {
    const std::scoped_lock lock{impl_->mutex};
    return impl_->waiting.size() + impl_->running.size();
}

u64 AsyncChunkSource::blocks_done() const {
    return impl_->blocks_done.load(std::memory_order_relaxed);
}

u64 AsyncChunkSource::chunks_done() const {
    return impl_->chunks_done.load(std::memory_order_relaxed);
}

f64 AsyncChunkSource::busy_seconds() const {
    return static_cast<f64>(impl_->busy_micros.load(std::memory_order_relaxed)) / 1e6;
}

usize AsyncChunkSource::worker_count() const noexcept { return impl_->pool.worker_count(); }

void AsyncChunkSource::generate_here(usize stack, i32 block_x, i32 block_z, GeneratedBlock& out) {
    out.block_x = block_x;
    out.block_z = block_z;
    out.chunks.clear();
    out.chunks.reserve(static_cast<usize>(kBlockChunks) * kBlockChunks);
    impl_->generate(stack, block_x * kBlockChunks, block_z * kBlockChunks, kBlockChunks,
                    out.chunks);
}

}  // namespace ov::server
