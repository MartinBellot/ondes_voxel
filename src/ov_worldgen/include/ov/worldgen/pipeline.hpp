// The layer that owns more than one chunk, and therefore the one that can
// decorate.
//
// `ChunkGenerator` turns noise into a chunk. Everything it does — the noise,
// the biome grid, the surface rules, the carving mask — is a pure function of
// the seed and the position, so it never has to look at a neighbour and a
// caller holding one chunk can run all of it.
//
// Decoration is not like that. `Decorator::decorate` takes a `FeatureLevel`
// covering the three-by-three neighbourhood because a feature writes across
// chunk borders: an ore vein whose origin is in the last column of a chunk
// finishes in the next one. Two previous attempts to wire the stage into
// `ChunkGenerator` were refused for that reason and the refusal was right — a
// single-chunk adaptor would drop every write that left the middle chunk, in
// silence, and a tree cut off at the border looks exactly like a tree.
//
// So the ownership of nine chunks has to exist somewhere, and this is it.
//
//   * A chunk climbs through statuses, the game's, in the game's order.
//   * A status has a **radius**: how far the neighbours must already have
//     climbed before this chunk may take the step. Ours is zero everywhere
//     except at `Features`, which needs one — and at `Full`, which needs its
//     eight neighbours to have *finished* their features, because that is when
//     the last write that can land in this chunk has landed.
//   * The statuses are cached, so the twenty-five chunks a single finished
//     chunk needs are each generated once rather than once per asker.
//
// The radius rule is enforced rather than hoped for: the only way into the
// feature stage is `promote()`, and `promote()` drives the eight neighbours to
// the previous status itself before it lets the stage run.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/world/chunk.hpp"
#include "ov/worldgen/chunk_generator.hpp"
#include "ov/worldgen/decoration.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <string_view>

namespace ov::worldgen {

/// How far a chunk has got. The game's statuses, in the game's order.
///
/// Named after the game's rather than after our stages, because the ordering
/// constraint they encode is the game's and a private vocabulary would hide
/// that. The ones we do not implement are present and pass through: they are
/// steps a chunk really does take, and renaming the ladder to hide the missing
/// rungs would make the gap invisible instead of visible.
enum class ChunkStatus : u8 {
    /// Allocated, empty, the world's shape and nothing else.
    Empty = 0,
    /// Where the structure starts would be chosen. Nothing happens here yet;
    /// `docs/provenance/features.md` records that no structure belongs to the
    /// ore step, which is why the ores do not wait for this.
    StructureStarts = 1,
    /// The 4x4x4 biome grid.
    Biomes = 2,
    /// Stone, water, lava, air.
    Noise = 3,
    /// Grass, dirt, sand, gravel, sandstone, the badlands' bands, bedrock.
    Surface = 4,
    /// The carving mask applied, then the heightmaps rebuilt.
    Carvers = 5,
    /// Decoration: the ores, the springs, the disks. Radius one.
    Features = 6,
    /// Every neighbour has run its features, so nothing more can be written
    /// into this chunk. The point at which it may be handed out.
    Full = 7,
};

[[nodiscard]] std::string_view to_string(ChunkStatus status) noexcept;

/// What generating cost, and — the number this whole layer exists for — how
/// many writes crossed a chunk border.
///
/// `border_writes` is the reason `FeatureLevel` is wider than a chunk. A
/// single-chunk adaptor would have turned every one of them into nothing, with
/// no error and no symptom, so counting them is not a statistic: it is the
/// proof that the thing the design was built to prevent is not happening.
struct PipelineStats {
    /// Chunks that reached each status at least once. Indexed by ChunkStatus.
    std::array<u64, 8> reached{};

    /// Calls to `Decorator::decorate`. One per chunk, not nine.
    u64 decorations{0};

    /// Block writes the decoration made, in total.
    u64 feature_writes{0};

    /// Of those, the ones that landed outside the chunk being decorated —
    /// kept, because the level covers the neighbourhood.
    u64 border_writes{0};

    /// Writes that fell outside the three-by-three altogether and really were
    /// dropped. Not a bug: the game drops them too, and generating the
    /// neighbour is what puts them back. A number rather than a shrug.
    u64 dropped_writes{0};

    /// Chunks resident in the cache right now, and the high-water mark.
    u64 resident{0};
    u64 peak_resident{0};
};

/// Drives chunks through the statuses, holding the neighbourhood decoration
/// needs.
///
/// Single-threaded on purpose, and the reason is the third principle of
/// CLAUDE.md rather than laziness: a chunk being decorated is written by its
/// eight neighbours' decoration too, so two threads decorating chunks whose
/// three-by-three neighbourhoods overlap are two writers on one chunk. The
/// answer to that is a geometric exclusion, never a mutex on the world —
/// `exclusive_class()` below is that geometry, written down and tested even
/// though nothing runs in parallel yet. See docs/provenance/pipeline-de-chunks.md
/// for what it would cost and why it is not paid yet.
class ChunkPipeline {
public:
    /// `generator` and `decorator` are borrowed and must outlive the pipeline;
    /// they are large, shared and immutable once built.
    ///
    /// The decorator may be null. That is the honest way to say "this world has
    /// terrain and no features yet": a pipeline with no decorator drives chunks
    /// to `Full` through the carvers and says so, rather than pretending the
    /// features happened.
    ChunkPipeline(const ChunkGenerator& generator, const Decorator* decorator,
                  const registry::BlockRegistry& blocks, world::WorldShape shape, i64 level_seed);

    ChunkPipeline(const ChunkPipeline&)            = delete;
    ChunkPipeline& operator=(const ChunkPipeline&) = delete;
    ~ChunkPipeline();

    /// Drive a chunk to a status, generating whatever neighbours that needs.
    ///
    /// Returns the chunk. Const, because a caller holding a reference while
    /// asking for another chunk would have it written under them by that
    /// chunk's decoration — which is exactly what is supposed to happen.
    [[nodiscard]] const world::Chunk& promote(i32 chunk_x, i32 chunk_z, ChunkStatus status);

    /// Drive a chunk to `Full` and hand it over, removing it from the cache.
    ///
    /// Safe to remove precisely because `Full` means every neighbour has
    /// decorated: nothing can write into it any more. Asking for the same
    /// chunk again regenerates it and re-runs the neighbours' decoration into
    /// the new copy, so a caller that keeps chunks — the server does — must
    /// consult its own store first.
    [[nodiscard]] world::Chunk take(i32 chunk_x, i32 chunk_z);

    /// Drop cached chunks that no chunk within `keep` of `centre` needs.
    ///
    /// A pipeline generating a region grows its cache without this: twenty-five
    /// chunks are live at any moment but nothing forgets the ones behind.
    void trim(i32 centre_x, i32 centre_z, i32 keep);

    /// Forget everything. The cheap way to bound the footprint between regions.
    void clear();

    [[nodiscard]] const PipelineStats& stats() const noexcept;

    /// The status a chunk has reached, `Empty` if it is not cached.
    [[nodiscard]] ChunkStatus status_of(i32 chunk_x, i32 chunk_z) const;

    /// Approximate bytes the cache holds, from the sections that exist.
    [[nodiscard]] usize footprint_bytes() const;

    /// The exclusion class of a chunk, 0..8.
    ///
    /// Decorating a chunk writes into the nine chunks around it, so two chunks
    /// may be decorated at the same time exactly when their neighbourhoods are
    /// disjoint, which is when they are three or more apart on one axis. The
    /// classes `(x mod 3, z mod 3)` have that property by construction: two
    /// distinct chunks of one class differ by a multiple of three on some axis.
    ///
    /// This is the shape "exclusive-region scheduling" has to take here, and it
    /// is a geometric fact rather than a lock — no shared mutable state, no
    /// mutex on the world. It is written down and unit-tested now so that
    /// whoever adds threads inherits a checked rule instead of inventing one.
    [[nodiscard]] static constexpr u8 exclusive_class(i32 chunk_x, i32 chunk_z) noexcept {
        const auto wrap = [](i32 value) { return static_cast<u8>(((value % 3) + 3) % 3); };
        return static_cast<u8>(wrap(chunk_x) * 3 + wrap(chunk_z));
    }

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

}  // namespace ov::worldgen
