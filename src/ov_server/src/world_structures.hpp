// The structures of a generated dimension, as the server generates them.
//
// Everything below existed already and was measured outside the server: the
// placement of the nineteen structure sets, the pieces decided from the seed,
// the blocks written chunk by chunk by `StructureStage` inside the pipeline's
// three-by-three. What was missing was the server attaching it, so a world
// made by `ov_dedicated` carried no structure at all. This file is that
// attachment, kept out of `generated_world.cpp` so the latter only gains a few
// short blocks.
//
// ── What is shared and what is not ──────────────────────────────────────────
//
// The structure sets, the biome and block tags, the placer and the builder
// (the templates read out of the server jar) are loaded once per dimension and
// shared by every generating thread: after `load` nothing in them is written,
// and every query they answer is `const` with its scratch on the caller's
// stack. The **sampler** and the **stage** are one per worldgen stack: the
// sampler asks that stack's generator, whose density nodes keep `mutable`
// caches, and the stage caches starts and pending shape updates for the chunks
// that stack is building. Nothing needs a lock.
//
// ── Determinism ─────────────────────────────────────────────────────────────
//
// A worker generates a square from a cold pipeline (async_chunk_source.hpp,
// invariant 3). The stage is cleared with it, so the structures written into a
// square are, like its terrain, a function of the seed and the square alone.
//
// ── What is refused ─────────────────────────────────────────────────────────
//
// Anything the builder does not make (villages, bastions, outposts, ancient
// cities, trail ruins — the jigsaw; the fortress, the mineshaft, the
// stronghold, the monument, the mansion, the temples, the end city) is counted
// and named by the stage and logged once. Two kinds the builder *can* make are
// refused here as well, because what it makes of them is not the game's
// structure but an invention at a placeholder height: the ruined portals
// (their height search is not implemented; they would stand at y 0) and the
// buried treasure (its downward search is not implemented; its chest would
// hang at y 90).
#pragma once

#include "ov/base/types.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/chunk.hpp"

#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace ov::worldgen {
class ChunkGenerator;
class StructurePlacer;
class StructureStage;
class StructureWorldSampler;
}  // namespace ov::worldgen

namespace ov::server {

/// What a dimension's structures share across stacks. Neither copyable nor
/// movable: the placer borrows the sets and the builder the tags.
class WorldStructures {
public:
    /// Load, or return null and say why. `data` is the generated
    /// `data/minecraft` directory; `biomes` the names the dimension's biome
    /// source can produce, which is what keeps a Nether fossil out of the
    /// overworld.
    [[nodiscard]] static std::unique_ptr<WorldStructures> load(
        const std::filesystem::path& data, const std::filesystem::path& server_jar,
        const registry::BlockRegistry& blocks, const std::vector<std::string_view>& biomes);

    /// Where the server jar is expected: `OV_SERVER_JAR`, or
    /// `tools/vanilla/server.jar` beside the data root.
    [[nodiscard]] static std::filesystem::path default_jar(const std::filesystem::path& data_root);

    WorldStructures(const WorldStructures&)            = delete;
    WorldStructures& operator=(const WorldStructures&) = delete;
    ~WorldStructures();

    /// A stage for one stack. `generator` must outlive the result; the sampler
    /// the stage reads through is owned by the returned object.
    struct StackStage;
    [[nodiscard]] std::unique_ptr<StackStage> make_stage(const worldgen::ChunkGenerator& generator,
                                                         const registry::BlockRegistry& blocks,
                                                         const registry::Registries&    registries,
                                                         i64 seed) const;

    struct Impl;

private:
    explicit WorldStructures(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

/// One stack's sampler and stage, and the refusals it has already reported.
struct WorldStructures::StackStage {
    std::unique_ptr<worldgen::StructureWorldSampler> sampler;
    std::unique_ptr<worldgen::StructureStage>        stage;
    std::set<std::string>                            reported;
    /// The shared placer, for a start's definition (its terrain adaptation).
    const worldgen::StructurePlacer* placer{nullptr};

    StackStage();
    StackStage(const StackStage&)            = delete;
    StackStage& operator=(const StackStage&) = delete;
    ~StackStage();

    /// Write into a finished chunk its `structures` compound: the starts whose
    /// start chunk it is, and a reference to every start crossing it. The
    /// stage must still hold the starts of the chunk's neighbourhood — call
    /// before clearing it.
    void record(world::Chunk& chunk);

    /// Log, once per stack, every refusal reason not logged yet.
    void report(std::string_view dimension);
};

}  // namespace ov::server
