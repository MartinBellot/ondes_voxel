// The generated overworld, as one thing the server can hold.
//
// Everything worldgen needs — the router, the biome source, the surface rules,
// the carvers, the feature registry, the decorator, the generator and the
// pipeline — is built once, lives together and borrows from its neighbours.
// Half of it holds pointers into the other half, so the whole is deliberately
// neither copyable nor movable and comes back from `load` behind a
// `unique_ptr`: moving it would leave a generator pointing at a router that has
// gone.
//
// It exists as its own file rather than inline in server.cpp for a plain
// reason: server.cpp is two thousand lines that several people work in at once,
// and a new subsystem wedged into the middle of it is a merge conflict waiting
// to happen. What server.cpp gets is one include and one call.
#pragma once

#include "ov/base/types.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/chunk.hpp"

#include "ov/math/block_pos.hpp"

#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ov::server {

/// A real overworld, generated on demand.
class GeneratedWorld {
public:
    /// Build the whole stack, or nothing.
    ///
    /// Returns null and says why in the log when a piece is missing: the data
    /// generator has not been run, the pack is from another version, the tag
    /// the carvers need is absent, the codec does not name a biome the
    /// generator can produce. Refused and named, never a half-built generator
    /// that quietly produces stone.
    ///
    /// `codec_biomes` is the server's in-memory biome numbering — the order the
    /// registry codec sends to the client. It is not the block registry's, and
    /// assuming the two agree is the kind of assumption that is invisible while
    /// it holds: the generator writes block-registry indices, the chunk packet
    /// reads codec ids, and a world whose every biome is off by a few would
    /// render in the wrong colours with nothing anywhere saying so. The
    /// translation is built here, once, by name.
    /// `stacks` is how many independent worldgen stacks to build: one per
    /// thread that will ever generate, plus one for whoever calls `generate`
    /// directly. Nothing is shared between them because the noise router and
    /// the surface system keep `mutable` memo caches — see the comment on
    /// `Stack` in the .cpp. Zero is read as one.
    ///
    /// ── nether ── `settings` names the noise settings — "overworld", or
    /// "nether" for the Nether's stack: its router, its five biomes, its
    /// surface rules, its one carver and a 256-block chunk.
    [[nodiscard]] static std::unique_ptr<GeneratedWorld> load(
        const std::filesystem::path& data_root, const registry::BlockRegistry& blocks,
        const registry::Registries& registries, std::span<const std::string_view> codec_biomes,
        i64 seed, usize stacks = 1, std::string_view settings = "overworld");

    /// How many stacks were built. A caller must never pass an index at or
    /// above this to `generate_square`.
    [[nodiscard]] usize stack_count() const noexcept;

    /// One finished chunk: noise, biomes, surface, carvers, features.
    ///
    /// Costs more than one chunk's worth of work the first time it is asked
    /// near an untouched area — decorating a chunk needs its eight neighbours
    /// carved, and each of those needs its own eight — but the pipeline caches
    /// by status, so a scan across a region pays for each chunk once.
    ///
    /// The cache is trimmed to a neighbourhood of the last chunk asked for.
    /// Chunk streaming asks for chunks near each other, so that keeps the
    /// support resident while bounding what an afternoon of walking holds; the
    /// server keeps the finished chunks itself and never asks twice.
    [[nodiscard]] world::Chunk generate(i32 chunk_x, i32 chunk_z);

    /// Generate a `side` x `side` square of finished chunks on stack
    /// `stack_index`, appending them to `out`.
    ///
    /// This is the shape the job pool uses, and the square rather than the
    /// chunk is the unit for a reason that is about correctness, not speed.
    /// The pipeline's cache is cleared before and after, so what this produces
    /// is a pure function of the seed and `(origin_x, origin_z, side)`: the
    /// same square, whichever stack runs it, in whatever order, on any run.
    /// Asked chunk by chunk against a warm cache the answer would instead
    /// depend on what that stack had already been asked for, which is to say
    /// on where the players happened to walk.
    ///
    /// Safe to call from any thread as long as no two threads share a
    /// `stack_index`. Nothing in the returned chunks is aliased.
    void generate_square(usize stack_index, i32 origin_x, i32 origin_z, i32 side,
                         std::vector<std::pair<ChunkPos, world::Chunk>>& out);

    /// Rewrite a chunk's biome cells from block-registry indices into the ids
    /// the chunk packet carries. Already done by `generate` and
    /// `generate_square`; exposed for a caller that builds a chunk another way.
    void to_codec_biomes(world::Chunk& chunk) const;

    [[nodiscard]] i64 seed() const noexcept;

    GeneratedWorld(const GeneratedWorld&)            = delete;
    GeneratedWorld& operator=(const GeneratedWorld&) = delete;
    ~GeneratedWorld();

    /// One worldgen stack. Public only so the .cpp can name it in a member
    /// declaration; it is an incomplete type everywhere else.
    struct Stack;

private:
    struct Impl;

    explicit GeneratedWorld(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

}  // namespace ov::server
