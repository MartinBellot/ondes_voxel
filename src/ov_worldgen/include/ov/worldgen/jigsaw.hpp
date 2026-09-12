// The jigsaw structures: villages, pillager outposts, bastion remnants, ancient
// cities, trail ruins — every `minecraft:jigsaw` structure of 1.20.1.
//
// A jigsaw structure is not drawn, it is *grown*. A start pool gives the first
// piece; each jigsaw block of a placed piece names a pool to draw a neighbour
// from and a name the neighbour's own jigsaw block must carry; the neighbour is
// turned and slid until the two blocks face each other, kept if its box fits in
// the space still free, and grown in turn, breadth first, until the depth runs
// out. Everything is drawn from one random, the structure's own
// (`setLargeFeatureSeed(seed, chunkX, chunkZ)` over a legacy core), in one
// order — the order is the whole specification, and a draw out of place grows
// a plausible village that shares nothing with the game's.
//
// The order and every rule below were fixed against the pieces the game itself
// stored in `structures.starts` of the reference worlds (template, position,
// rotation, box, ground delta, junctions — docs/provenance/jigsaw.md), not
// assumed:
//
//   * the start: the start height, then a rotation, then a template of the start
//     pool; a named start jigsaw (the ancient city's `city_anchor`) shuffles the
//     start piece's jigsaw blocks once more and puts the first of that name on
//     the start position; the piece is then lowered or raised so that its floor
//     (its box's bottom plus its ground delta) meets the projected height;
//   * the biome the placement filters on is read at the middle of the start
//     piece's box, at that projected height — the jigsaw anchor that
//     docs/provenance/structures.md § 5 could not find with a fixed column;
//   * a piece's jigsaw blocks are listed by (y, x, z) of the template and
//     shuffled; the candidates are the pool's weighted list shuffled, then the
//     fallback's; an empty element stops the search; each candidate tries the
//     four rotations shuffled, and each of its jigsaw blocks shuffled;
//   * a candidate fits when its box lies in the free space: the space around
//     the start (a cube of `max_distance_from_center` about the anchor, less
//     every box placed in it) or, for a jigsaw block pointing into its own
//     piece, the piece's box less what was placed inside it.
//
// Loaded eagerly and immutable afterwards: every query is `const` and draws
// only from the random it is handed (trap 17).
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/math/vec.hpp"
#include "ov/nbt/tag.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/world/heightmap.hpp"
#include "ov/worldgen/placement.hpp"
#include "ov/worldgen/structure.hpp"
#include "ov/worldgen/structure_template.hpp"

#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ov::worldgen {

struct StructurePiece;
struct StructureStart;
struct PiecePlaceResult;

/// How a pool element meets the ground.
enum class Projection : u8 {
    /// Placed as the template is, at the height the jigsaw joint gives it.
    Rigid,
    /// Placed on the terrain: each column of the template drops to the surface
    /// (the village roads and fields).
    TerrainMatching,
};

[[nodiscard]] std::string_view to_string(Projection projection) noexcept;

/// One jigsaw block of a template, as the file stores it.
struct JigsawConnector {
    /// Template-local position.
    BlockPos    pos;
    Direction   front{Direction::North};
    Direction   top{Direction::Up};
    /// `joint`: rollable (the neighbour may turn about the facing) or aligned
    /// (the two tops must agree). A jigsaw without the field is aligned when
    /// it faces sideways, rollable when it faces up or down.
    bool        rollable{false};
    std::string name;
    std::string target;
    std::string pool;
    std::string final_state;
};

enum class PoolElementType : u8 { Single, LegacySingle, List, Feature, Empty };

[[nodiscard]] std::string_view to_string(PoolElementType type) noexcept;

/// One element of a template pool.
struct PoolElement {
    PoolElementType type{PoolElementType::Empty};
    Projection      projection{Projection::Rigid};
    /// `single` and `legacy_single`: the template.
    std::string     location;
    /// `feature`: the placed feature.
    std::string     feature;
    /// The processor list by name, or empty for an inline list (every inline
    /// list of 1.20.1 is empty).
    std::string     processors;
    /// `list`: the elements, placed together; the first one's jigsaw blocks
    /// are the list's.
    std::vector<PoolElement> elements;

    // ── resolved at load ──
    /// The template, or null when the jar has none by that name: the game
    /// places an empty template then (size 0, no blocks, no jigsaw blocks) —
    /// `ancient_city/walls/intact_horizontal_wall_stairs_5` is named in a pool
    /// and absent from the 1.20.1 jar.
    const StructureTemplate*     tpl{nullptr};
    /// The template's jigsaw blocks, sorted by (y, x, z).
    std::vector<JigsawConnector> connectors;
    BlockPos                     size;
    /// The resolved processors; null when a processor of the list is not
    /// implemented (`refusal` then says which).
    std::shared_ptr<const ProcessorList> processor_list;
    std::string                          refusal;

    /// The element's box at a position and rotation (pivot zero).
    [[nodiscard]] BoundingBox box(BlockPos pos, Rotation rotation) const noexcept;
};

/// One `worldgen/template_pool/*.json`.
struct TemplatePool {
    std::string name;
    std::string fallback;
    /// Each element once per unit of weight, in file order: the list the game
    /// draws and shuffles.
    std::vector<const PoolElement*>           weighted;
    std::vector<std::unique_ptr<PoolElement>> elements;
    /// The tallest element, for the expansion hack.
    i32 max_size{0};
};

/// The `minecraft:jigsaw` fields of one `worldgen/structure/*.json`.
struct JigsawConfig {
    std::string                         name;
    std::string                         start_pool;
    std::string                         start_jigsaw_name;
    i32                                 size{0};
    i32                                 start_height{0};
    std::optional<world::HeightmapType> project_to;
    i32                                 max_distance{80};
    bool                                expansion_hack{false};
    /// `terrain_adaptation`: none, beard_thin, beard_box, bury, encapsulate.
    std::string                         terrain_adaptation{"none"};
    /// How far from its start chunk a piece can lie, in chunks: every piece
    /// fits in `max_distance_from_center` about the anchor, and the anchor —
    /// the middle of the start piece — lies within the start piece's extent
    /// (plus the named start jigsaw's offset) of the chunk corner. Computed
    /// at load from the start pool: 12 for the ancient city, 9 for the
    /// bastion, 7 or 8 for the rest. `max_distance / 16` alone would drop
    /// pieces (docs/provenance/jigsaw.md § 6).
    i32                                 reach_chunks{0};
};

/// A connection between two pieces, as the game stores it on both.
struct JigsawJunction {
    i32        source_x{0};
    i32        source_ground_y{0};
    i32        source_z{0};
    i32        delta_y{0};
    Projection dest_projection{Projection::Rigid};
};

/// The first step of a start: the start piece, and where the biome is read.
struct JigsawStartPoint {
    const PoolElement* element{nullptr};
    BlockPos           position;
    Rotation           rotation{Rotation::None};
    BoundingBox        box;
    /// The middle of the start piece's box, at the projected height.
    BlockPos           anchor;
};

class JigsawLibrary {
public:
    /// Read the pools, the processor lists and the jigsaw structures of a
    /// generated `data/minecraft` root; the templates come from `templates`,
    /// borrowed. A pool naming a template the jar does not have is kept (the
    /// game places an empty one); a pool that does not parse is refused.
    [[nodiscard]] static std::expected<JigsawLibrary, TemplateError> load(
        const std::filesystem::path& data_root, const TemplateLibrary& templates,
        const registry::BlockRegistry& blocks, const BlockTags& tags,
        std::string* detail = nullptr);

    [[nodiscard]] const JigsawConfig*  config(std::string_view structure) const noexcept;
    [[nodiscard]] const TemplatePool*  pool(std::string_view name) const noexcept;
    [[nodiscard]] usize                pool_count() const noexcept;
    /// Every template the pools name that the jar does not have.
    [[nodiscard]] const std::vector<std::string>& missing_templates() const noexcept;

    /// The start piece and anchor of a start, or nothing when the start pool
    /// draws the empty element or the named start jigsaw is absent. Cheap: one
    /// template, one height. `sampler` is asked the projected height only.
    [[nodiscard]] std::optional<JigsawStartPoint> start_point(
        const JigsawConfig& config, i64 level_seed, i32 chunk_x, i32 chunk_z,
        const StructureWorldSampler* sampler) const;

    /// Every piece of a start, grown from the seed. The heights of terrain
    /// matching joints are asked of `sampler`; a structure that needs one and
    /// has no sampler is refused, by name.
    [[nodiscard]] std::expected<StructureStart, std::string> assemble(
        const JigsawConfig& config, i64 level_seed, i32 chunk_x, i32 chunk_z,
        const StructureWorldSampler* sampler) const;

    /// The element a stored piece's `pool_element` compound names, matched
    /// against the pools. Null when no pool has it.
    [[nodiscard]] const PoolElement* element_from_nbt(const nbt::Tag& pool_element) const;

    /// The `pool_element` compound of an element, as the game writes it.
    [[nodiscard]] static nbt::Tag element_to_nbt(const PoolElement& element);

    /// Write the part of a pool element piece inside `clip`: the template
    /// through the element's processors — the structure blocks dropped (and
    /// the air, for a legacy element), the jigsaw blocks turned into their
    /// final state, a terrain matching element dropped onto the surface.
    /// `random` is the chunk's structure random, drawn by the containers.
    PiecePlaceResult place(StructureLevel& level, const StructurePiece& piece,
                           const BoundingBox& clip, FeatureRandom& random,
                           const registry::BlockRegistry& blocks) const;

    JigsawLibrary(JigsawLibrary&&) noexcept;
    JigsawLibrary& operator=(JigsawLibrary&&) noexcept;
    ~JigsawLibrary();

private:
    JigsawLibrary();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// A direction turned as a block turns with its structure.
[[nodiscard]] Direction rotate(Direction direction, Rotation rotation) noexcept;

/// A pool element piece as the game stores it in `Children`: `id`
/// `minecraft:jigsaw`, `BB`, `GD`, `O` −1, `PosX/Y/Z`, `pool_element`,
/// `rotation`, `ground_level_delta`, `junctions`.
[[nodiscard]] nbt::Tag jigsaw_piece_to_nbt(const StructurePiece& piece);

/// The inverse: a stored piece, its element found in the pools.
[[nodiscard]] std::expected<StructurePiece, std::string> jigsaw_piece_from_nbt(
    const JigsawLibrary& library, const nbt::Tag& child);

}  // namespace ov::worldgen
