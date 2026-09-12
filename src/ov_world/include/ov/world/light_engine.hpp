// Sky light and block light, kept current one edit at a time.
//
// The engine it replaces recomputed the 3x3 chunks around every edited chunk
// from nothing, once per tick per chunk touched: tens of milliseconds of flood
// fill to move the light of one torch. Light is a fixed point — every cell holds
// the most any neighbour can give it, or what it gives off itself — and an edit
// disturbs that fixed point only near the edit. So this engine repairs it there:
//
//   * **Increase** is a breadth-first fill outward from the cells that can now
//     give more than their neighbours hold.
//   * **Decrease** is the classic two-queue removal. A cell whose level could
//     have come from a darkened neighbour is darkened too, and remembers what
//     it held; a neighbour holding more than the darkened cell could have given
//     it is lit by something else and goes on the increase queue, to fill the
//     hole back in from outside. Emitters and the top of the world that the
//     removal wave crossed are re-seeded afterwards.
//
// The rules are the documented ones (minecraft.wiki, "Light"): a level loses one
// per block of taxicab distance, a block that stops sky light stops both kinds,
// and sky light at 15 falls straight down without losing a level. Which blocks
// stop light and how much each gives off are the measured tables of the
// registry. See docs/provenance/incremental-light.md.
//
// The result is the same fixed point a full recompute of every loaded chunk
// reaches, nibble for nibble — tests/test_light_engine.cpp checks it after
// thousands of random edits on real worlds. Chunks outside the loaded set do not
// exist for the engine: they neither give light nor take it.
//
// No mutex, no global: one engine per dimension, owned by the thread that
// writes that dimension's chunks.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/world/chunk.hpp"

#include <memory>
#include <span>

namespace ov::world {

/// Where the engine finds chunks. It never loads one: a chunk that is not
/// resident answers nullptr and is treated as absent.
class LightChunkSource {
public:
    LightChunkSource()                                   = default;
    LightChunkSource(const LightChunkSource&)            = delete;
    LightChunkSource& operator=(const LightChunkSource&) = delete;
    LightChunkSource(LightChunkSource&&)                 = delete;
    LightChunkSource& operator=(LightChunkSource&&)      = delete;
    virtual ~LightChunkSource()                          = default;

    [[nodiscard]] virtual Chunk* light_chunk(i32 chunk_x, i32 chunk_z) = 0;
};

/// What a dimension's light does.
struct LightRules {
    /// The Nether and the End have no sky light at all.
    bool has_sky{true};

    /// Do the light-filtering blocks — water, leaves, ice, the registry's
    /// `Attenuating` class — stop sky light falling at 15 without loss? The
    /// wiki says they do: "decrease sky light by 1 level (but do not affect
    /// block light)". Off reproduces the engine this one replaced, which let
    /// them through at full strength.
    bool filtering_dims_sky{false};
};

/// What one call did, for the tick profile and the benches.
struct LightStats {
    usize edits{0};
    /// Cells the removal pass darkened.
    usize removed{0};
    /// Cells the increase pass raised.
    usize raised{0};
};

class LightEngine {
public:
    explicit LightEngine(const registry::BlockRegistry& blocks, LightRules rules = {});
    ~LightEngine();
    LightEngine(const LightEngine&)            = delete;
    LightEngine& operator=(const LightEngine&) = delete;
    LightEngine(LightEngine&&) noexcept;
    LightEngine& operator=(LightEngine&&) noexcept;

    [[nodiscard]] const LightRules& rules() const noexcept;

    /// Say that the block at `pos` has changed. Costs a push: nothing is
    /// propagated until `propagate`, so a fill of thirty thousand blocks is one
    /// batch and not thirty thousand floods. The light stored at `pos` must
    /// still be the light from before the edit — the removal starts from it.
    void block_changed(BlockPos pos);

    /// Edits waiting for `propagate`.
    [[nodiscard]] usize pending() const noexcept;

    /// The positions themselves, and forgetting them — for a caller that
    /// repairs the light some other way (the server's full-recompute
    /// measurement mode, `OV_LIGHT_FULL=1`).
    [[nodiscard]] std::span<const BlockPos> pending_positions() const noexcept;
    void                                    discard_pending() noexcept;

    /// Bring the light of every loaded chunk in line with the edits noted since
    /// the last call. Sections whose light turned uniform are compacted.
    LightStats propagate(LightChunkSource& chunks);

    /// Light one chunk as if nothing were around it: emitters and direct
    /// sunlight, flooded inside the chunk. `keep_sky` leaves the sky light the
    /// chunk already holds (a file written by vanilla) untouched. Safe to call
    /// on a chunk nothing else can see yet, on any thread that owns this
    /// engine.
    void light_chunk(Chunk& chunk, bool keep_sky = false);

    /// Let the light of a chunk that has just joined the loaded set cross its
    /// four borders, both ways. With `light_chunk` before it, the loaded set is
    /// back at its fixed point: adding a chunk can only add light.
    LightStats stitch(LightChunkSource& chunks, ChunkPos pos);

    /// Light a set of chunks from nothing: each alone, then every border.
    void light_region(LightChunkSource& chunks, std::span<const ChunkPos> positions);

    /// The measured tables, as the engine reads them.
    [[nodiscard]] u8   emission(registry::BlockStateId state) const noexcept;
    [[nodiscard]] bool stops_light(registry::BlockStateId state) const noexcept;

    /// The sections whose light the last `propagate`, `stitch` or
    /// `light_chunk` wrote — what a client has to mesh again. Valid until the
    /// next call.
    [[nodiscard]] std::span<const SectionPos> changed_sections() const noexcept;

    /// The control of the equivalence test: propagate without the removal
    /// pass. Never set outside a test — the light it leaves is wrong, which is
    /// the point.
    void testing_skip_removal(bool skip) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ov::world
