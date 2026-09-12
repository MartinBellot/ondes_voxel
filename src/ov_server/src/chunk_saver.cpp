#define OV_LOG_CATEGORY "server"

#include "chunk_saver.hpp"

#include "ov/base/log.hpp"
#include "ov/base/thread.hpp"
#include "ov/nbt/region_writer.hpp"

#include <fmt/format.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace ov::server {

struct ChunkSaver::Impl {
    mutable std::mutex       mutex;
    std::condition_variable  wake;
    std::condition_variable  idle;
    std::deque<ChunkSaveJob> jobs;
    std::vector<ChunkPos>    saved;
    bool                     busy{false};
    bool                     stopping{false};
    u64                      written{0};

    /// Started last, in the constructor, once everything above exists.
    std::thread thread;

    void run() {
        set_thread_role("ov-save", ThreadRole::Io);
        while (true) {
            ChunkSaveJob job;
            {
                std::unique_lock lock{mutex};
                wake.wait(lock, [this] { return stopping || !jobs.empty(); });
                if (jobs.empty()) {
                    return;  // stopping, and nothing left to write
                }
                job = std::move(jobs.front());
                jobs.pop_front();
                busy = true;
            }

            write(job);

            {
                const std::scoped_lock lock{mutex};
                for (const auto& chunk : job.chunks) {
                    saved.push_back(chunk->position());
                }
                written += job.chunks.size();
                busy = false;
            }
            idle.notify_all();
        }
    }

    /// What `save_world` did on the tick thread, byte for byte: the same
    /// grouping by region, the same `to_nbt`, the same atomic region write.
    static void write(const ChunkSaveJob& job) {
        const auto started = std::chrono::steady_clock::now();

        world::ChunkCodecContext context = job.context;
        context.block_ticks              = job.block_ticks;
        context.fluid_ticks              = job.fluid_ticks;

        std::map<std::pair<i32, i32>, std::vector<const world::Chunk*>> by_region;
        for (const auto& chunk : job.chunks) {
            const ChunkPos pos = chunk->position();
            by_region[{pos.x >> 5, pos.z >> 5}].push_back(chunk.get());
        }
        for (const auto& [region, members] : by_region) {
            const auto path =
                job.region_dir / fmt::format("r.{}.{}.mca", region.first, region.second);
            auto writer = nbt::RegionWriter::open_or_empty(path);
            for (const world::Chunk* chunk : members) {
                const ChunkPos pos = chunk->position();
                writer.set_chunk(static_cast<u32>(pos.x & 31), static_cast<u32>(pos.z & 31),
                                 world::to_nbt(*chunk, context), 0);
            }
            if (!writer.write(path)) {
                OV_LOG_WARN("could not write {}", path.string());
            }
        }
        if (!job.chunks.empty()) {
            OV_LOG_INFO("saved {} chunks across {} regions in {:.0f} ms, off the tick thread",
                        job.chunks.size(), by_region.size(),
                        std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() -
                                                               started)
                            .count());
        }
    }
};

ChunkSaver::ChunkSaver() : impl_(std::make_unique<Impl>()) {
    // `Impl*`, not `this`: see the note on `~unique_ptr` in chunkmap.md § 13.
    Impl* impl   = impl_.get();
    impl->thread = std::thread([impl] { impl->run(); });
}

ChunkSaver::~ChunkSaver() {
    {
        const std::scoped_lock lock{impl_->mutex};
        impl_->stopping = true;
    }
    impl_->wake.notify_all();
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
}

void ChunkSaver::submit(ChunkSaveJob job) {
    {
        const std::scoped_lock lock{impl_->mutex};
        impl_->jobs.push_back(std::move(job));
    }
    impl_->wake.notify_one();
}

void ChunkSaver::flush() {
    std::unique_lock lock{impl_->mutex};
    impl_->idle.wait(lock, [this] { return impl_->jobs.empty() && !impl_->busy; });
}

usize ChunkSaver::drain_saved(std::vector<ChunkPos>& out) {
    const std::scoped_lock lock{impl_->mutex};
    const usize            count = impl_->saved.size();
    out.insert(out.end(), impl_->saved.begin(), impl_->saved.end());
    impl_->saved.clear();
    return count;
}

usize ChunkSaver::queued() const {
    const std::scoped_lock lock{impl_->mutex};
    return impl_->jobs.size() + (impl_->busy ? 1U : 0U);
}

u64 ChunkSaver::chunks_written() const {
    const std::scoped_lock lock{impl_->mutex};
    return impl_->written;
}

}  // namespace ov::server
