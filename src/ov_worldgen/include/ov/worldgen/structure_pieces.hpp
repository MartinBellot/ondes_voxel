// The structures built from a fixed template or a handful of pieces: what the
// pieces are (decided from the seed alone, at `structure_starts`) and how each
// kind writes them into a level (at `features`, chunk by chunk).
//
// Two halves, measured separately because they fail separately:
//
//   * **generation** — which template, which rotation and mirror, where. A pure
//     function of the world seed and the start chunk, drawn from
//     `WorldgenRandom.setLargeFeatureSeed(seed, chunkX, chunkZ)` over a
//     `java.util.Random` core. Every draw order here was fixed by comparing
//     against the pieces the game itself stored in `structures.starts` of the
//     three reference worlds — 109 starts, including the ones in chunks the game
//     never finished (their pieces are decided and written long before their
//     blocks). See docs/provenance/structures.md § 13.
//   * **placement** — the blocks. A piece is written by every chunk it crosses,
//     each chunk writing the part inside its own column, so nothing here may
//     depend on the order chunks are generated in: every random draw of the
//     template layer is keyed on a world position.
//
// Kinds covered: igloo, shipwreck (and beached), ocean ruins (cold and warm,
// small, large, clusters), ruined portals (the seven variants), buried treasure.
// Kinds *not* covered are refused by `generate` with their name — the desert
// pyramid, the jungle temple and the swamp hut are built by code rather than
// from a template in the game, the nether fossils belong to a dimension this
// generator does not make, and the jigsaw structures are another piece of work.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/nbt/tag.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/worldgen/placement.hpp"
#include "ov/worldgen/structure.hpp"
#include "ov/worldgen/structure_template.hpp"

#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ov::worldgen {

enum class PieceKind : u8 {
    Igloo,
    Shipwreck,
    OceanRuin,
    RuinedPortal,
    BuriedTreasure,
    NetherFossil,  // ── nether-2 ──
};

[[nodiscard]] std::string_view to_string(PieceKind kind) noexcept;

/// The ruined portal's per-start choices, as the game stores them.
struct RuinedPortalProperties {
    /// `on_land_surface`, `partly_buried`, `on_ocean_floor`, `in_mountain`,
    /// `underground`, `in_nether`.
    std::string placement;
    bool        cold{false};
    f32         mossiness{0.0F};
    bool        air_pocket{false};
    bool        overgrown{false};
    bool        vines{false};
    bool        replace_with_blackstone{false};
};

struct StructurePiece {
    PieceKind kind{PieceKind::Igloo};
    /// `minecraft:igloo/top`. Empty for the buried treasure, which has none.
    std::string template_name;
    /// The template origin, `TPX/TPY/TPZ` in the game's NBT.
    BlockPos    origin;
    Rotation    rotation{Rotation::None};
    Mirror      mirror{Mirror::None};
    BlockPos    pivot;
    BoundingBox box;
    /// The ocean ruins' `block_rot` integrity.
    f32                    integrity{1.0F};
    bool                   large{false};
    bool                   warm{false};
    bool                   beached{false};
    RuinedPortalProperties portal;
    /// Whether the height has been settled. A piece fresh from `generate` sits
    /// at a placeholder y (90 for most kinds) and moves to the terrain the
    /// first time it is placed; a piece read from a finished chunk of the
    /// game's already sits where the game put it.
    bool height_settled{false};
    /// ── structures ── The template origin as generated, before the height
    /// settled. The igloo's stored `TPY` is this one — the game moves the
    /// igloo's box to the terrain and leaves its template position where the
    /// start put it (read on the reference worlds: bottom `TPY` 54, box at 35).
    BlockPos generated_origin;
};

struct StructureStart {
    std::string                 structure;
    i32                         chunk_x{0};
    i32                         chunk_z{0};
    std::vector<StructurePiece> pieces;
    BoundingBox                 box;
    /// What the game would add to this start and we do not, named. Empty when
    /// the start is complete. The pieces present are still right: a large
    /// ocean ruin is placed even when the small ruins around it are not.
    std::string incomplete;
};

/// What one placement did, for the harness.
struct PiecePlaceResult {
    u32 written{0};
    u32 dropped{0};
    u32 chests{0};
    /// Data markers seen and not understood — named, never silently skipped.
    std::vector<std::string> unknown_markers;
    /// Positions whose shape follows neighbours; see `update_shapes`.
    std::vector<BlockPos> shaped;
};

/// `GenerationStep.Decoration`'s ordinal for a step name, or -1.
[[nodiscard]] i32 step_ordinal(std::string_view step) noexcept;

/// A structure's index within its generation step: its position among every
/// structure of that step, **sorted by name**. It is the `index` of the
/// feature seed a structure's post-processing random is built with.
///
/// Measured, not assumed: the loot seeds of the chests in the reference worlds
/// match at exactly these indices — igloo 3, ocean_ruin_cold 7, ocean_ruin_warm
/// 8, ruined_portal 10, ruined_portal_mountain 13, shipwreck 17 in
/// `surface_structures`, buried_treasure 0 in `underground_structures`. The
/// registry's order is the name order, across every dimension's structures.
[[nodiscard]] i32 structure_step_index(const StructurePlacer& placer,
                                       std::string_view       structure) noexcept;

class StructureBuilder {
public:
    /// Read the templates of the covered families out of the server jar. The
    /// registry and the tags are borrowed and must outlive the builder.
    ///
    /// `data_root` is the generated `data/minecraft` directory: the ruined
    /// portals' setups are read from their `worldgen/structure/*.json`.
    [[nodiscard]] static std::expected<StructureBuilder, TemplateError> load(
        const std::filesystem::path& server_jar, const std::filesystem::path& data_root,
        const registry::BlockRegistry& blocks, const BlockTags& tags,
        std::string* detail = nullptr);

    /// The pieces of one start, from the seed alone.
    ///
    /// Refuses, naming the structure, any kind this file does not build.
    /// `sampler` answers the heights the ruined portals' placement needs; it
    /// may be null, and those pieces then keep a placeholder height.
    [[nodiscard]] std::expected<StructureStart, std::string> generate(
        const StructureDefinition& definition, i64 level_seed, i32 chunk_x, i32 chunk_z,
        const StructureWorldSampler* sampler) const;

    /// A piece exactly as the game stored it in a chunk's `structures.starts`
    /// — how the harness places the game's own pieces with our code.
    [[nodiscard]] std::expected<StructurePiece, std::string> piece_from_nbt(
        const nbt::Tag& child) const;

    /// Settle the piece's height on the level's terrain, as the game does the
    /// first time the piece is placed. A no-op for a settled piece.
    ///
    /// `random` is the chunk's structure random: a beached ship sinks by a
    /// further `nextInt(3)` drawn from it. Pass it, and then tell `place` the
    /// draw was made (`height_drawn`), so the chunk draws it once.
    void settle_height(const StructureLevel& level, StructurePiece& piece,
                       FeatureRandom* random = nullptr) const;

    /// Write the part of a piece inside `clip`.
    ///
    /// `random` is the chunk's structure-step random — the one the game
    /// hands a piece's post-processing — and is drawn for the loot seeds of
    /// the chests a data marker makes.
    PiecePlaceResult place(StructureLevel& level, const StructurePiece& piece,
                           const BoundingBox& clip, FeatureRandom& random,
                           bool height_drawn = false) const;

    [[nodiscard]] const TemplateLibrary& templates() const noexcept;

    StructureBuilder(StructureBuilder&&) noexcept;
    StructureBuilder& operator=(StructureBuilder&&) noexcept;
    ~StructureBuilder();

private:
    StructureBuilder();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ov::worldgen
