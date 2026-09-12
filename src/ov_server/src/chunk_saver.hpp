// ── streaming ── Chunks written to disk off the tick thread.
//
// Autosave encoded and wrote every dirty chunk on the tick thread: an NBT
// document and a deflate per chunk, and each region file read and rewritten
// whole. That was the worst tick of docs/provenance/performance-tick.md § 5.5
// (533 ms of a 557 ms tick), and the user's own log showed an autosave of 18 to
// 87 chunks every thirty seconds while exploring.
//
// CLAUDE.md principle 3 says how a chunk crosses to another thread: a
// `shared_ptr<const>`, copy-on-write. The tick takes snapshots — a section
// pointer each, nothing encoded — and moves them here; one thread encodes and
// writes them. Two rules keep that correct:
//
//   * **one thread, first in first out**: two saves of the same chunk reach the
//     disk in the order they were taken;
//   * **pinned until written**: the server keeps a chunk resident while a save
//     of it is in flight (`drain_saved` says when it has landed). Evicted and
//     read back from disk before its bytes arrived, it would come back as it
//     was before the edit.
//
// The mutex guards the job queue and nothing else. No chunk behind it is part
// of the world: a snapshot is immutable, and the tick thread detaches its own
// copy before it writes (chunk_section.hpp).
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/world/chunk.hpp"
#include "ov/world/chunk_storage.hpp"

#include <filesystem>
#include <memory>
#include <vector>

namespace ov::server {

/// One autosave's worth of chunks, and everything encoding them needs.
struct ChunkSaveJob {
    /// The `region` directory the `.mca` files live in.
    std::filesystem::path region_dir;

    /// Borrowed registries and names, which must outlive the saver; the two
    /// tick spans are rebound to `block_ticks` and `fluid_ticks` below.
    world::ChunkCodecContext context;

    /// The level's pending ticks, taken once for the whole job: `to_nbt`
    /// keeps each chunk's own share.
    std::vector<world::ScheduledTick> block_ticks;
    std::vector<world::ScheduledTick> fluid_ticks;

    std::vector<std::shared_ptr<const world::Chunk>> chunks;
};

class ChunkSaver {
public:
    /// Starts the thread.
    ChunkSaver();

    /// Writes everything still queued, then joins: a shutdown loses nothing.
    ~ChunkSaver();

    ChunkSaver(const ChunkSaver&)            = delete;
    ChunkSaver& operator=(const ChunkSaver&) = delete;

    /// Queue a job. Returns at once.
    void submit(ChunkSaveJob job);

    /// Block until every job submitted so far is on disk. For `/save-all` and
    /// shutdown, which promise exactly that — never for an autosave.
    void flush();

    /// The positions whose save has reached the disk since the last call,
    /// once per save (a chunk saved twice appears twice). Appends to `out`.
    usize drain_saved(std::vector<ChunkPos>& out);

    /// Jobs waiting or being written.
    [[nodiscard]] usize queued() const;

    /// Chunks written since start-up.
    [[nodiscard]] u64 chunks_written() const;

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

}  // namespace ov::server
