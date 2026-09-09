#define OV_LOG_CATEGORY "server"

#include "async_chunk_source.hpp"

#include "ov/base/job_pool.hpp"
#include "ov/base/log.hpp"

#include <atomic>
#include <mutex>
#include <unordered_set>
#include <utility>

namespace ov::server {

namespace {

/// How many blocks may be queued or running at once, per worker.
///
/// The queue is a promise to do work, and a promise made while a player is
/// standing still becomes wrong the moment they walk. Bounding it means the
/// caller re-asks every tick from where the player is *now*, so the order the
/// pool works in tracks the view rather than the history of the view. Three per
/// worker keeps every worker fed across the gap between one drain and the next
/// without queueing a minute of terrain nobody will look at.
constexpr usize kQueueDepthPerWorker = 3;

}  // namespace

struct AsyncChunkSource::Impl {
    GeneratedWorld* world{nullptr};

    /// Guards the two queues and the requested set — and nothing else. There
    /// is no chunk behind this mutex that anyone can see: a worker builds into
    /// its own vector and hands the whole thing over by move. The world itself
    /// is never locked (CLAUDE.md § 2, principle 3).
    mutable std::mutex          mutex;
    std::unordered_set<u64>     requested;
    std::vector<GeneratedBlock> done;
    usize                       outstanding{0};

    std::atomic<u64> blocks_done{0};
    std::atomic<u64> chunks_done{0};

    /// Declared last, and that is load-bearing. Members are destroyed in
    /// reverse declaration order, so the pool — which joins its threads — goes
    /// first, before the queues a running job is about to write into. With the
    /// pool declared at the top, a job finishing during shutdown would push a
    /// block into a vector that had already been destroyed.
    base::JobPool pool;

    explicit Impl(usize workers) : pool(workers) {}
};

AsyncChunkSource::AsyncChunkSource(GeneratedWorld& world, usize workers)
    : impl_(std::make_unique<Impl>(workers)) {
    impl_->world = &world;
    if (world.stack_count() < workers + 1) {
        // Named rather than clamped silently. A worker index that runs past the
        // stacks would be two threads on one noise router, which is the exact
        // race this design exists to make impossible.
        OV_LOG_ERROR(
            "chunk source: {} workers but only {} worldgen stacks — refusing to run more "
            "workers than stacks",
            workers, world.stack_count());
    }
    OV_LOG_INFO("chunk source: {} workers, blocks of {}x{} chunks", impl_->pool.worker_count(),
                kBlockChunks, kBlockChunks);
}

AsyncChunkSource::~AsyncChunkSource() = default;

bool AsyncChunkSource::request(ChunkPos pos) {
    const i32 block_x = block_of(pos.x);
    const i32 block_z = block_of(pos.z);
    const u64 key     = ChunkPos{block_x, block_z}.packed();

    const usize workers = impl_->pool.worker_count();
    const usize limit   = (workers == 0 ? usize{1} : workers) * kQueueDepthPerWorker;

    {
        const std::scoped_lock lock{impl_->mutex};
        if (impl_->requested.contains(key)) {
            return false;
        }
        if (impl_->outstanding >= limit) {
            return false;
        }
        impl_->requested.insert(key);
        ++impl_->outstanding;
    }

    // The job captures `Impl*`, never `this`, and that distinction cost a
    // segfault to learn. `impl_` is a `unique_ptr` member, and libc++'s
    // `~unique_ptr` **nulls its stored pointer before** running the deleter. So
    // for the whole of `~Impl` — which is where the pool joins its threads, and
    // therefore where a job is most likely still running — a job reading
    // `impl_->mutex` reads it through a null pointer. Holding the raw pointer
    // instead keeps the job pointing at the object that is still alive until
    // the pool has joined, which is the order the member layout guarantees.
    Impl* impl = impl_.get();
    impl->pool.submit([impl, block_x, block_z](usize worker) {
        GeneratedBlock block;
        block.block_x = block_x;
        block.block_z = block_z;
        block.chunks.reserve(static_cast<usize>(kBlockChunks) * kBlockChunks);
        // Worker index plus one: stack zero belongs to the tick thread's own
        // synchronous fallback, and two threads on one stack is the race.
        impl->world->generate_square(worker + 1, block_x * kBlockChunks, block_z * kBlockChunks,
                                     kBlockChunks, block.chunks);

        const u64 count = block.chunks.size();
        {
            const std::scoped_lock lock{impl->mutex};
            impl->done.push_back(std::move(block));
        }
        impl->blocks_done.fetch_add(1, std::memory_order_relaxed);
        impl->chunks_done.fetch_add(count, std::memory_order_relaxed);
    });

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
        impl_->outstanding -= taken.size();
        for (const GeneratedBlock& block : taken) {
            impl_->requested.erase(ChunkPos{block.block_x, block.block_z}.packed());
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
    return impl_->outstanding;
}

u64 AsyncChunkSource::blocks_done() const {
    return impl_->blocks_done.load(std::memory_order_relaxed);
}

u64 AsyncChunkSource::chunks_done() const {
    return impl_->chunks_done.load(std::memory_order_relaxed);
}

void AsyncChunkSource::generate_here(usize stack, i32 block_x, i32 block_z, GeneratedBlock& out) {
    out.block_x = block_x;
    out.block_z = block_z;
    out.chunks.clear();
    out.chunks.reserve(static_cast<usize>(kBlockChunks) * kBlockChunks);
    impl_->world->generate_square(stack, block_x * kBlockChunks, block_z * kBlockChunks,
                                  kBlockChunks, out.chunks);
}

}  // namespace ov::server
