// Structure templates: the `.nbt` files a structure is built from, read at run
// time from the server jar, turned and mirrored, filtered through processors and
// written into a level.
//
// Nothing here is committed data. The 1 010 templates of 1.20.1 live in
// `data/minecraft/structures/` *inside* the inner jar the server bundler carries
// (`META-INF/versions/1.20.1/server-1.20.1.jar` in `tools/vanilla/server.jar`),
// and `TemplateLibrary::open` reads them from there, in memory, every time. They
// are Mojang's data: they never reach the repository and never reach the disk
// outside `tools/vanilla/`.
//
// What a template is, as the file stores it:
//
//   size      [x, y, z]
//   palette   [{Name, Properties}]            one palette, or
//   palettes  [[{Name, Properties}], …]       several — the shipwrecks' eight
//                                             wood combinations
//   blocks    [{state, pos:[x,y,z], nbt?}]    `state` indexes the palette
//   entities  [{pos:[dx,dy,dz], blockPos:[x,y,z], nbt}]
//
// What placement does with it, established against the reference worlds (see
// docs/provenance/structures.md § 12) rather than assumed:
//
//   * a local position is mirrored first (LEFT_RIGHT negates z, FRONT_BACK
//     negates x), then rotated about the **pivot**, then offset by the
//     template origin. The pivot is the piece's, not the template's: the
//     igloo's three pieces each have their own, the ruined portals use the
//     middle of their footprint, the ocean ruins use the corner;
//   * a template with several palettes picks one with
//     `java.util.Random(Mth.getSeed(origin)).nextInt(count)` — the origin, not
//     the chunk: every chunk the piece crosses picks the same palette;
//   * a processor that draws gets a fresh `java.util.Random` seeded from
//     `Mth.getSeed` of the block's **world** position, so the answer for a
//     block does not depend on which chunk is being generated when it is asked.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/nbt/tag.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/worldgen/placement.hpp"

#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ov::worldgen {

enum class TemplateError : u8 {
    /// The jar, or the jar inside it, cannot be opened.
    JarUnreadable,
    /// No template by that name.
    NotFound,
    /// The file is not a structure template: no size, no blocks, a state index
    /// outside the palette.
    Malformed,
    /// A palette names a block this registry does not have.
    UnknownBlock,
    /// A processor list names a processor type that is not implemented. Refused
    /// rather than skipped: a processor that silently does nothing places a
    /// structure that looks right and is not.
    UnknownProcessor,
    /// A processor or rule the parser understood the type of but not the body.
    BadProcessor,
};

[[nodiscard]] std::string_view to_string(TemplateError error) noexcept;

/// The game's `Rotation`, in the game's order. The order is load-bearing: a
/// structure's rotation is `values()[nextInt(4)]`.
enum class Rotation : u8 { None = 0, Clockwise90 = 1, Clockwise180 = 2, CounterClockwise90 = 3 };

/// The game's `Mirror`, in the game's order.
enum class Mirror : u8 { None = 0, LeftRight = 1, FrontBack = 2 };

[[nodiscard]] std::string_view to_string(Rotation rotation) noexcept;
[[nodiscard]] std::string_view to_string(Mirror mirror) noexcept;
/// `NONE`, `CLOCKWISE_90`, … — the spelling the chunk NBT stores.
[[nodiscard]] std::optional<Rotation> parse_rotation(std::string_view name) noexcept;
[[nodiscard]] std::optional<Mirror>   parse_mirror(std::string_view name) noexcept;

/// Two rotations in sequence.
[[nodiscard]] constexpr Rotation compose(Rotation a, Rotation b) noexcept {
    return static_cast<Rotation>((static_cast<u8>(a) + static_cast<u8>(b)) & 3);
}

/// An inclusive box of blocks, as the game's `BoundingBox` is.
struct BoundingBox {
    i32 min_x{0};
    i32 min_y{0};
    i32 min_z{0};
    i32 max_x{-1};
    i32 max_y{-1};
    i32 max_z{-1};

    [[nodiscard]] bool contains(i32 x, i32 y, i32 z) const noexcept {
        return x >= min_x && x <= max_x && y >= min_y && y <= max_y && z >= min_z && z <= max_z;
    }

    [[nodiscard]] bool intersects(const BoundingBox& other) const noexcept {
        return max_x >= other.min_x && min_x <= other.max_x && max_z >= other.min_z &&
               min_z <= other.max_z && max_y >= other.min_y && min_y <= other.max_y;
    }

    /// The smallest box holding both.
    void encapsulate(const BoundingBox& other) noexcept;

    void move(i32 dx, i32 dy, i32 dz) noexcept {
        min_x += dx;
        max_x += dx;
        min_y += dy;
        max_y += dy;
        min_z += dz;
        max_z += dz;
    }

    /// One chunk's whole column.
    [[nodiscard]] static BoundingBox chunk_column(i32 chunk_x, i32 chunk_z, i32 min_y,
                                                  i32 max_y) noexcept {
        return {chunk_x * 16, min_y, chunk_z * 16, chunk_x * 16 + 15, max_y, chunk_z * 16 + 15};
    }
};

/// `Mth.getSeed(x, y, z)`: the seed a position-keyed draw is made from.
///
/// The `x * 3129871` is an **int** multiplication and wraps at 32 bits before
/// it is widened; the `z` product is a long one. Getting that asymmetry wrong
/// moves every processor draw in every structure.
[[nodiscard]] i64 position_seed(i32 x, i32 y, i32 z) noexcept;

/// Mirror, then rotate about the pivot. The template-local position of a block
/// becomes an offset from the template origin.
[[nodiscard]] BlockPos transform(BlockPos local, Mirror mirror, Rotation rotation,
                                 BlockPos pivot) noexcept;

/// A block state, turned and mirrored as the block itself would be: facings,
/// axes, the sixteen-step `rotation`, the four connection sides, rail shapes,
/// door hinges, chest halves, stair shapes.
[[nodiscard]] registry::BlockStateId transform_state(const registry::BlockRegistry& blocks,
                                                     registry::BlockStateId state, Mirror mirror,
                                                     Rotation rotation) noexcept;

struct TemplateBlock {
    BlockPos pos;
    /// Index into each palette.
    u32 state{0};
    /// Index into `StructureTemplate::block_nbt`, or -1.
    i32 nbt{-1};
};

struct TemplateEntity {
    f64         x{0.0};
    f64         y{0.0};
    f64         z{0.0};
    BlockPos    block;
    std::string id;
};

/// One parsed `.nbt`.
struct StructureTemplate {
    std::string name;
    BlockPos    size;
    /// One or more palettes; the same length each.
    std::vector<std::vector<registry::BlockStateId>> palettes;
    std::vector<TemplateBlock>                       blocks;
    std::vector<nbt::Tag>                            block_nbt;
    std::vector<TemplateEntity>                      entities;

    [[nodiscard]] static std::expected<StructureTemplate, TemplateError> parse(
        std::span<const u8> bytes, const registry::BlockRegistry& blocks, std::string* detail);

    /// The box the template covers once placed.
    [[nodiscard]] BoundingBox bounding_box(BlockPos origin, Mirror mirror, Rotation rotation,
                                           BlockPos pivot) const noexcept;
};

/// Every template under the families asked for, read out of the jar once.
///
/// Loaded eagerly and immutable afterwards, on purpose: a lazily filled cache
/// behind a `const` interface is a data race the first time two threads
/// generate (trap 17 of the project's list, paid in the noise router).
class TemplateLibrary {
public:
    /// Open `server_jar` — the bundler jar, or an already unwrapped server jar
    /// — and parse every template whose path starts with one of `families`
    /// (`"igloo/"`, `"shipwreck/"`…). An empty list loads all 1 010.
    [[nodiscard]] static std::expected<TemplateLibrary, TemplateError> open(
        const std::filesystem::path& server_jar, const registry::BlockRegistry& blocks,
        std::span<const std::string_view> families, std::string* detail = nullptr);

    /// `minecraft:igloo/top` or `igloo/top`. Null when absent.
    [[nodiscard]] const StructureTemplate* find(std::string_view name) const noexcept;

    [[nodiscard]] usize size() const noexcept;

    TemplateLibrary(TemplateLibrary&&) noexcept;
    TemplateLibrary& operator=(TemplateLibrary&&) noexcept;
    ~TemplateLibrary();

private:
    TemplateLibrary();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ── The level a template is written into ────────────────────────────────────

/// A `FeatureLevel` that can also hold block entities — a chest's loot table,
/// a sign, a furnace.
class StructureLevel : public FeatureLevel {
public:
    /// Store a block entity. `data` carries the template's compound with `id`,
    /// and the position is written by the level.
    virtual void set_block_entity(i32 x, i32 y, i32 z, nbt::Tag data) = 0;
    /// The block entity already there, or null.
    [[nodiscard]] virtual const nbt::Tag* block_entity(i32 x, i32 y, i32 z) const = 0;
};

// ── Processors ──────────────────────────────────────────────────────────────

/// One block on its way into the world.
struct ProcessorBlock {
    BlockPos                pos;
    registry::BlockStateId  state{registry::kAirState};
    std::optional<nbt::Tag> nbt;
};

/// What every processor may ask.
struct ProcessorContext {
    const StructureLevel*          level{nullptr};
    const registry::BlockRegistry* blocks{nullptr};
    BlockPos                       origin;
    BlockPos                       pivot;
    Rotation                       rotation{Rotation::None};
    Mirror                         mirror{Mirror::None};
};

/// A `structure_processor`. Returns nothing to drop the block.
class StructureProcessor {
public:
    StructureProcessor()                                     = default;
    StructureProcessor(const StructureProcessor&)            = delete;
    StructureProcessor& operator=(const StructureProcessor&) = delete;
    virtual ~StructureProcessor();

    /// `original` is the template's block at its template-local position;
    /// `current` is the block so far, at its world position.
    [[nodiscard]] virtual std::optional<ProcessorBlock> process(const ProcessorContext& context,
                                                                const ProcessorBlock&   original,
                                                                ProcessorBlock current) const = 0;

    [[nodiscard]] virtual std::string_view type() const noexcept = 0;
};

using ProcessorRef = std::shared_ptr<const StructureProcessor>;

/// A `worldgen/processor_list/*.json`, or a list a structure type builds in
/// code (the ocean ruins' integrity, the ruined portals' ageing).
struct ProcessorList {
    std::vector<ProcessorRef> processors;

    /// Parse the `{"processors": [...]}` body. Every processor type not
    /// implemented is refused and named in `detail`.
    [[nodiscard]] static std::expected<ProcessorList, TemplateError> parse_json(
        std::string_view json, const registry::BlockRegistry& blocks, const BlockTags* tags,
        std::string* detail);
};

/// The processors a structure type builds in code rather than reading.
///
/// Built here so that the piece code never spells a rule: every one of them
/// has a JSON twin with the same semantics.
[[nodiscard]] ProcessorRef make_block_rot(f32 integrity);
[[nodiscard]] ProcessorRef make_block_ignore(const registry::BlockRegistry&    blocks,
                                             std::span<const std::string_view> names);
[[nodiscard]] ProcessorRef make_protected_blocks(const BlockTags& tags, std::string tag);
/// `block_age`: stone bricks crack, turn to stairs, go mossy; slabs, stairs
/// and walls go mossy; obsidian cries. The ruined portals' ageing.
[[nodiscard]] ProcessorRef make_block_age(const registry::BlockRegistry& blocks,
                                          const BlockTags& tags, f32 mossiness);

// ── Placement ───────────────────────────────────────────────────────────────

struct PlaceSettings {
    Rotation rotation{Rotation::None};
    Mirror   mirror{Mirror::None};
    BlockPos pivot;
    /// Only blocks inside this box are written. The game places a structure
    /// chunk by chunk, each chunk writing the part of every piece that falls
    /// in it.
    std::optional<BoundingBox> clip;
    std::vector<ProcessorRef>  processors;
    /// Keep a water source that is already there: a waterloggable block
    /// placed into one is waterlogged.
    bool keep_liquids{true};
    /// Palette to use, or `std::nullopt` to draw it from the origin.
    std::optional<u32> palette;
    /// The piece's post-processing random — the chunk's structure-step random.
    /// Every container the template carries (chest, barrel, …) draws one
    /// `nextLong` from it as it is placed, whether or not it has a loot table:
    /// that is what moves the loot seed of the data-marker chests placed after.
    /// Null draws nothing.
    FeatureRandom* random{nullptr};
};

/// A structure block in DATA mode, left for the piece to interpret — the
/// shipwreck's `map_chest`, the ocean ruin's `drowned`.
struct DataMarker {
    BlockPos    pos;
    std::string metadata;
};

struct PlaceResult {
    u32                     written{0};
    u32                     dropped_by_processors{0};
    std::vector<DataMarker> markers;
    /// Blocks whose shape depends on their neighbours (stairs, fences, doors),
    /// to be updated again once the neighbouring chunks are placed.
    std::vector<BlockPos> shaped;
};

/// Whether a state's shape follows its neighbours in the game's
/// neighbour-shape update, as far as this layer reproduces it.
[[nodiscard]] bool needs_shape_update(const registry::BlockRegistry& blocks,
                                      registry::BlockStateId         state) noexcept;

/// Run the neighbour-shape update over some positions: stairs take their
/// shape, fences their connections, a door's lower half its upper half.
void update_shapes(FeatureLevel& level, const registry::BlockRegistry& blocks,
                   std::span<const BlockPos> positions);

/// Write a template into a level.
[[nodiscard]] PlaceResult place_template(StructureLevel& level, const StructureTemplate& tpl,
                                         BlockPos origin, const PlaceSettings& settings,
                                         const registry::BlockRegistry& blocks);

/// The palette `place_template` would draw for this origin.
[[nodiscard]] u32 palette_for(const StructureTemplate& tpl, BlockPos origin) noexcept;

/// A water source, or a block holding one.
[[nodiscard]] bool holds_water_source(const registry::BlockRegistry& blocks,
                                      registry::BlockStateId         state) noexcept;

/// The same state with `waterlogged` set, or the state unchanged when the
/// block has no such property.
[[nodiscard]] registry::BlockStateId with_waterlogged(const registry::BlockRegistry& blocks,
                                                      registry::BlockStateId         state,
                                                      bool value) noexcept;

/// A state with one property set by name; unchanged when the block has no such
/// property or value.
[[nodiscard]] registry::BlockStateId with_value(const registry::BlockRegistry& blocks,
                                                registry::BlockStateId         state,
                                                std::string_view               property,
                                                std::string_view               value) noexcept;

/// The shape a stair takes from the stairs around it — what the game's
/// neighbour-shape update gives a stair once a template is down. The
/// templates store shapes as they were when the template was saved, and a
/// placed ship is full of `outer_right` stairs the game straightens.
[[nodiscard]] registry::BlockStateId stair_shape(const registry::BlockRegistry& blocks,
                                                 const FeatureLevel& level, BlockPos pos,
                                                 registry::BlockStateId state) noexcept;

/// The four sides of a fence, recomputed from its neighbours — the same
/// neighbour-shape update, for fences. The shipwrecks' fences are stored
/// unconnected and the game joins them.
[[nodiscard]] registry::BlockStateId fence_connections(const registry::BlockRegistry& blocks,
                                                       const FeatureLevel& level, BlockPos pos,
                                                       registry::BlockStateId state) noexcept;

}  // namespace ov::worldgen
