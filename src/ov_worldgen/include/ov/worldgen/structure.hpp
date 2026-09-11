// Which structure actually starts in a chunk, and why not when it does not.
//
// `structure_set.hpp` answers the arithmetic half: is this chunk the one the
// grid picked, and did it survive the frequency roll. That half is exact and
// costs nothing. This file adds the two halves that are not arithmetic:
//
//   * the **biome filter** — every structure carries a biome tag, and the game
//     samples the biome at *one* position that depends on the structure's type
//     (the middle of the chunk at the surface, at the ocean floor, or at a
//     fixed y for the mineshaft) and refuses the start when the tag does not
//     contain it;
//   * the **exclusion zone** — the pillager outposts alone, which refuse to
//     start within ten chunks of a village candidate.
//
// The result is a *reason*, not a boolean. `PlacementDecision` names which of
// the four gates rejected a chunk, because a parity harness that only knows
// "no structure here" cannot tell a wrong grid from a wrong biome tag, and
// those two are fixed in completely different places.
//
// What is deliberately absent, and refused rather than approximated:
//
//   * the strongholds' concentric rings, which are not a per-chunk function at
//     all — the game walks 128 positions outward from the origin once per
//     world and snaps each to a biome it likes;
//   * every structure whose *own* generation can fail after the biome check
//     passes (the ocean monument's depth probe, the shipwreck's beaching test,
//     the ruined portal's placement search). Those come back as
//     `PlacementDecision::PlacedByPlacement` — the placement said yes and the
//     structure was never asked — and the harness counts them separately so the
//     residual is attributed rather than hidden.
#pragma once

#include "ov/base/types.hpp"
#include "ov/worldgen/structure_set.hpp"

#include <array>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ov::worldgen {

/// The `type` field of a structure. Every one 1.20.1 has, named — including the
/// ones whose geometry we do not build, because the *placement* of a structure
/// we cannot build is still exactly measurable and still worth having right.
enum class StructureKind : u8 {
    DesertPyramid,
    JungleTemple,
    SwampHut,
    Igloo,
    Mineshaft,
    OceanMonument,
    OceanRuin,
    Shipwreck,
    BuriedTreasure,
    RuinedPortal,
    WoodlandMansion,
    Stronghold,
    Fortress,
    NetherFossil,
    EndCity,
    /// Villages, pillager outposts, ancient cities, bastions, trail ruins: the
    /// template-pool system. One kind, nine structures.
    Jigsaw,
};

[[nodiscard]] std::string_view to_string(StructureKind kind) noexcept;

/// Where the game samples the biome to decide whether a structure may start.
///
/// One position, not a region: a desert pyramid whose chunk centre is one block
/// outside the desert does not generate, however much desert the rest of its
/// footprint covers.
enum class GenerationAnchor : u8 {
    /// The chunk's middle column, at the first free height above the terrain.
    SurfaceCentre,
    /// The chunk's middle column, at the top of the solid column with fluids
    /// ignored — the sea bed rather than the sea surface. Trap 5 of the
    /// project's list is this distinction, paid for in the carvers.
    OceanFloorCentre,
    /// The chunk's middle column at a fixed height. The mineshaft's, y = 50.
    FixedHeight,
    /// No biome check on the chunk at all — the stronghold, whose biome was
    /// consulted when its ring position was chosen.
    None,
};

/// One `worldgen/structure/*.json`.
struct StructureDefinition {
    std::string      name;
    StructureKind    kind{StructureKind::Jigsaw};
    /// `#minecraft:has_structure/…` or a bare biome name.
    std::string   biomes;
    /// The generation step, as the file names it. Kept as text: the enum lives
    /// in `decoration.hpp`, whose header drags in the whole feature stack, and
    /// this layer only ever reports the step.
    std::string   step;

    GenerationAnchor anchor{GenerationAnchor::SurfaceCentre};
    i32              anchor_height{0};

    /// Sample the anchor column at the chunk's *corner* rather than its middle.
    ///
    /// Only ever true for the jigsaw structures, and only when
    /// `OV_STRUCT_JIGSAW_ANCHOR=corner` asks for it. It is a measuring
    /// instrument, not a supported setting.
    ///
    /// The reason it exists: a jigsaw structure's start piece is a *template*
    /// chosen from a pool, placed so that its own bounding box straddles the
    /// start position, and the biome is read where that piece lands — not at a
    /// fixed column of the chunk. Neither the corner nor the middle reproduces
    /// that. Measured, on the same two worlds: the middle gets the plains
    /// village of chunk (3006, 10) right and the pillager outpost of chunk
    /// (-84, 105) wrong, the corner gets exactly the opposite, and our biome
    /// field agrees with the game's to the block at all four columns — so the
    /// residual is the anchor rule and nothing else. See
    /// docs/provenance/structures.md § 5.
    bool anchor_at_corner{false};

    /// ── structures ── `terrain_adaptation` is anything but `none` (the
    /// villages, the outposts, the ancient city, the trail ruins, the
    /// stronghold — and, of what is built here, the Nether fossil). The box
    /// such a start is *referenced* by is 12 blocks wider than its pieces:
    /// measured on the 84 finished fossil starts of reference-nether-987654321,
    /// 12 is the only widening that misses none of the game's `References`
    /// (11 misses 47). docs/provenance/structures.md § 20.3.
    bool terrain_adaptation{false};
};

/// ── structures ── How much wider than its pieces a start with terrain
/// adaptation is referenced by, in blocks.
inline constexpr i32 kTerrainAdaptationMargin = 12;

/// Biome tags, `tags/worldgen/biome/`, with `#other` references resolved.
///
/// A separate resolver from `BlockTags` and not a generalisation of it: the
/// values here are biome *names* that no registry of ours has ids for, the
/// directory is a different one, and merging the two would drag the block
/// registry into a question that has nothing to do with blocks.
class BiomeTags {
public:
    [[nodiscard]] static std::expected<BiomeTags, StructureSetError> load(
        const std::filesystem::path& data_root);

    /// Whether a biome is in a tag. `tag` may carry the leading `#` or not.
    [[nodiscard]] bool contains(std::string_view tag, std::string_view biome) const;

    /// Whether the tag exists. An unknown tag is not silently empty: a
    /// structure whose biome tag is missing is refused when it is loaded
    /// rather than never generating for the life of the world.
    [[nodiscard]] bool known(std::string_view tag) const;

    [[nodiscard]] usize tag_count() const noexcept;

    /// Every biome a tag resolves to, sorted. For the harness's error report.
    [[nodiscard]] std::vector<std::string_view> members(std::string_view tag) const;

private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};

/// What the world looks like where a structure wants to start.
///
/// An interface rather than a `ChunkGenerator&` so that `ov_worldgen`'s
/// structure layer does not depend on the noise stack it happens to sit next
/// to: a harness measuring the *grid* alone supplies a sampler that refuses
/// every question, and the placer then reports `BiomeUnknown` instead of
/// silently guessing.
class StructureWorldSampler {
public:
    StructureWorldSampler()                                        = default;
    StructureWorldSampler(const StructureWorldSampler&)            = delete;
    StructureWorldSampler& operator=(const StructureWorldSampler&) = delete;
    virtual ~StructureWorldSampler();

    /// The biome at a block position, `minecraft:plains` shaped. Empty when the
    /// sampler cannot answer.
    [[nodiscard]] virtual std::string_view biome_at(i32 x, i32 y, i32 z) const = 0;

    /// The first free y above the terrain in a column, ignoring nothing.
    [[nodiscard]] virtual i32 surface_height(i32 x, i32 z) const = 0;

    /// The first free y above the terrain in a column, treating water and lava
    /// as free. The sea bed.
    [[nodiscard]] virtual i32 ocean_floor_height(i32 x, i32 z) const = 0;

    /// ── nether-2 ── Is the generator's **base** column solid here — the
    /// noise alone, before surface rules and carvers (`getBaseColumn`)? A
    /// Nether fossil looks for its floor in it. Nothing: this sampler cannot
    /// say, and a structure that needs it is refused rather than guessed.
    [[nodiscard]] virtual std::optional<bool> base_solid(i32 /*x*/, i32 /*y*/, i32 /*z*/) const {
        return std::nullopt;
    }
};

/// Why a chunk does or does not start a structure.
enum class PlacementDecision : u8 {
    /// The grid picked another chunk of this cell.
    NotCandidate,
    /// The candidate lost the frequency roll.
    FrequencyRejected,
    /// A village candidate sits too close.
    ExcludedByZone,
    /// The biome at the anchor is not in the structure's tag.
    BiomeRejected,
    /// The sampler could not name the biome, so the filter was not applied.
    /// Distinct from a pass: a harness that counts this as a pass is measuring
    /// its own sampler.
    BiomeUnknown,
    /// Every gate this layer implements said yes. The structure's own
    /// generation was not run, so this is an upper bound on the truth for the
    /// types whose generation can still fail.
    PlacedByPlacement,
    /// The set's placement type is not one we implement (the strongholds).
    Unsupported,
    /// No structure of the set can occur in this dimension's biomes, so the
    /// dimension never asks the set at all.
    ///
    /// The game filters the sets once, when it builds a dimension's generator
    /// state, by intersecting each structure's biome tag with the biomes the
    /// dimension's source can produce. Without that filter an overworld chunk
    /// happily "places" a nether fossil every second chunk — 1 286 of them in
    /// 5 092 chunks, measured, before the filter was added.
    OtherDimension,
};

[[nodiscard]] std::string_view to_string(PlacementDecision decision) noexcept;

/// One answer, for one structure, in one chunk.
struct StructurePlacementResult {
    std::string_view  structure;
    std::string_view  set;
    PlacementDecision decision{PlacementDecision::NotCandidate};
    /// The biome the anchor named, when one was sampled.
    std::string_view  biome;
    i32               anchor_y{0};
};

/// The structures of a pack, and the question "what starts here".
class StructurePlacer {
public:
    /// Read `worldgen/structure/` and `tags/worldgen/biome/`, and take the
    /// sets. The registry is borrowed and must outlive the placer.
    [[nodiscard]] static std::expected<StructurePlacer, StructureSetError> load(
        const std::filesystem::path& data_root, const StructureSetRegistry& sets);

    /// Keep only the sets a dimension can actually produce.
    ///
    /// `biomes` is the list the dimension's biome source can name. A set none
    /// of whose structures has a biome in that list is marked and answers
    /// `OtherDimension` from then on. Calling this with an empty list leaves
    /// every set active — the honest reading of "no dimension was named".
    void restrict_to_biomes(const std::vector<std::string_view>& biomes);

    /// Every set's verdict for one chunk, in set order.
    ///
    /// `sampler` may be null: the biome gate is then reported as
    /// `BiomeUnknown` rather than skipped.
    [[nodiscard]] std::vector<StructurePlacementResult> decide(
        i64 level_seed, i32 chunk_x, i32 chunk_z, const StructureWorldSampler* sampler) const;

    /// The four anchor columns of one chunk, computed at most once each.
    ///
    /// Not an optimisation that happens to be visible — it is the difference
    /// between a usable step and an unusable one. Nineteen sets ask about the
    /// same two columns, and answering each of them separately means nineteen
    /// scans of a 384-block column with a full density evaluation per block.
    /// Measured, debug build: **+72,58 s per chunk** without this, **+0,213 s**
    /// with it. See docs/provenance/structures.md § 8.
    struct AnchorColumns {
        /// Indexed [corner ? 1 : 0]. -1 means "not computed yet".
        std::array<i32, 2> surface{-1, -1};
        std::array<i32, 2> ocean_floor{-1, -1};
    };

    /// One set's verdict. The set must belong to the registry given to `load`.
    ///
    /// `columns` is reused across the sets of one chunk and must not be carried
    /// to another chunk; pass null and the call allocates its own.
    [[nodiscard]] StructurePlacementResult decide_set(const StructureSet& set, i64 level_seed,
                                                      i32 chunk_x, i32 chunk_z,
                                                      const StructureWorldSampler* sampler,
                                                      AnchorColumns* columns = nullptr) const;

    [[nodiscard]] const StructureDefinition* find(std::string_view name) const noexcept;
    [[nodiscard]] const std::vector<StructureDefinition>& structures() const noexcept;
    [[nodiscard]] const BiomeTags&                        biome_tags() const noexcept;

    StructurePlacer(StructurePlacer&&) noexcept;
    StructurePlacer& operator=(StructurePlacer&&) noexcept;
    ~StructurePlacer();

private:
    StructurePlacer();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ov::worldgen
