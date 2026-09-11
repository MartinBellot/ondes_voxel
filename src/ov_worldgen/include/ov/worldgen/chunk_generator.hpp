// Turning the density function graph into blocks, in the order the game does.
//
// The stages, and the order is the whole point of this file:
//
//     noise  ->  biomes  ->  surface rules  ->  carvers  ->  (features)
//
// The noise stage puts down stone, water, lava and air and nothing else. The
// surface rules then turn the top of every stone column into grass, dirt, sand,
// gravel, the desert's sandstone and the badlands' bands. Only then do the
// carvers cut, and they cut through what the surface rules put there — which is
// why the order matters and why it is not a detail:
//
//   * a column whose top a cave has already eaten counts `stone_depth` from the
//     cave's ceiling instead of from the real surface, so carving first gives
//     the rules a terrain the game never showed them;
//   * and the rule that turns the grass above a carved cell into dirt cannot
//     exist at all until the grass has been placed, which is to say until the
//     surface rules have run.
//
// The carving mask itself does not care: it is a pure function of the seed, the
// chunk and the world's height, and it is bit-for-bit identical to the game's
// (1200/1200 chunks, see docs/provenance/carvers.md). What moved is when it is
// applied, not how it is computed.
//
// What fills a block the terrain left empty is the aquifer's answer, in both
// stages that leave blocks empty: the noise stage asks it with the terrain's
// density, the carvers ask it with a density of zero. A carved cell under an
// aquifer's level fills with its fluid, and a carved cell on a barrier is not
// carved at all (aquifer.hpp; docs/provenance/aquiferes.md § 10).
//
// Still missing, and named rather than silently absent: the features, which
// live in the pipeline, and the hand-off of the fluids the aquifer wants woken
// to a level's tick queue — they are collected here, not yet delivered.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/chunk.hpp"
#include "ov/worldgen/aquifer.hpp"
#include "ov/worldgen/biome_source.hpp"
#include "ov/worldgen/carver.hpp"
#include "ov/worldgen/density.hpp"
#include "ov/worldgen/surface_system.hpp"

#include <expected>
#include <memory>
#include <string_view>
#include <vector>

namespace ov::worldgen {

/// Why attaching the carvers to a generator failed.
///
/// Attaching them needs the block tag that says what a carver may replace, and
/// a missing tag is refused here rather than papered over with a hand-written
/// list: a list typed out by hand is a list that is wrong by the next version,
/// and the whole point of resolving the tag is that nobody has to maintain it.
enum class CarverAttachError : u8 {
    /// The pack has no `minecraft:block` registry.
    NoBlockRegistry,
    /// The pack has no `minecraft:overworld_carver_replaceables` tag.
    NoReplaceablesTag,
    /// The tag names a block the block registry does not have. Two packs built
    /// from different versions, and carving would then silently spare blocks it
    /// should cut.
    UnknownMember,
};

[[nodiscard]] std::string_view to_string(CarverAttachError error) noexcept;

class ChunkGenerator {
public:
    ChunkGenerator(const NoiseRouter& router, const BiomeSource& biomes,
                   const registry::BlockRegistry& blocks);

    /// Fill a chunk's blocks and biomes: noise, biomes, surface, carvers.
    ///
    /// The chunk must already have the world's shape; only its contents are
    /// written. The heightmaps are recomputed last, after the carvers, because
    /// a carved cell can be the one the heightmap was pointing at.
    ///
    /// Exactly the four stages below, run in order. Kept as one call because
    /// almost every caller wants all four, and because a caller that has no
    /// notion of a chunk status should not have to learn one.
    ///
    /// `fluid_updates`, when given, receives every fluid block the aquifer
    /// wants woken once the chunk exists — the game's post-processing marks.
    void generate(world::Chunk& chunk, std::vector<BlockPos>* fluid_updates = nullptr) const;

    // ── The stages, individually ────────────────────────────────────────────
    //
    // Split out for the pipeline (`pipeline.hpp`), which drives a chunk one
    // status at a time because the feature stage cannot start until the eight
    // neighbours have finished the carvers. They are the same code `generate()`
    // runs, in the same order, and `test_pipeline.cpp` checks block for block
    // that driving them one by one gives the chunk `generate()` gives.
    //
    // None of the four reads a neighbour: they are pure functions of the seed
    // and the position, which is why the pipeline's radius is zero up to the
    // carvers and one only at the features.

    /// Stone, water, lava, air — and nothing cut. Reads no biome. What fills
    /// an empty block is the aquifer's answer, barrier stone included.
    void generate_noise(world::Chunk& chunk,
                        std::vector<BlockPos>* fluid_updates = nullptr) const;

    /// The 4x4x4 biome grid. Reads no block.
    ///
    /// Independent of the noise stage in both directions, which is why the
    /// game's status order (biomes before noise) and this file's execution
    /// order (noise before biomes) produce the same chunk.
    void generate_biomes(world::Chunk& chunk) const;

    /// Grass, dirt, sand, gravel, sandstone, the badlands' bands, bedrock.
    /// Needs the biomes. A no-op when no surface system is attached.
    void generate_surface(world::Chunk& chunk) const;

    /// The carving mask, applied to what the surface rules left. A no-op when
    /// no carvers are attached, and a no-op in the legacy order, where the
    /// cutting has already happened inside the noise stage.
    void generate_carvers(world::Chunk& chunk,
                          std::vector<BlockPos>* fluid_updates = nullptr) const;

    /// The aquifer this generator asks, or nullptr when it has none. Built
    /// from the router and its seed at construction; exposed for harnesses.
    [[nodiscard]] const Aquifer* aquifer() const noexcept { return aquifer_.get(); }

    /// Switch the aquifer off, so the global fluid rule decides alone and the
    /// carvers cut to air — the behaviour before the aquifer existed.
    ///
    /// A measuring instrument, like `set_carve_before_surface`: the before and
    /// the after have to come out of one binary on one sample. Defaults to on,
    /// or to what `OV_AQUIFER` said at construction (`OV_AQUIFER=0` is off).
    void set_aquifer_enabled(bool enabled) noexcept { use_aquifer_ = enabled; }

    /// Whether the aquifer is actually answering: attached, fully built, on —
    /// and enabled by the dimension's settings (── carvers-3 ── the Nether's
    /// and the End's say no).
    [[nodiscard]] bool aquifer_active() const noexcept {
        return use_aquifer_ && aquifer_ != nullptr && aquifer_->enabled() &&
               router_->aquifers_enabled();
    }

    /// Give the generator the surface rules, so a generated chunk gets grass,
    /// dirt, sand, gravel, snow, the desert's sandstone, the badlands' clay
    /// bands and the bedrock floor rather than bare stone.
    ///
    /// Optional, and the option is not laziness: the noise stage is measured on
    /// its own against the reference world, and a harness that wants that
    /// number needs a generator that stops there. Not owned — the system
    /// outlives the generator.
    void set_surface_system(const SurfaceSystem* surface) noexcept { surface_ = surface; }

    /// Whether the noise says a position is solid. Exposed because a parity
    /// harness wants the decision rather than the block, and because the
    /// decision is the part that is claimed to be exact.
    [[nodiscard]] bool is_solid(i32 x, i32 y, i32 z) const;

    /// The raw density. A parity harness wants the number, not the verdict:
    /// how far a disagreement is from the boundary says whether a constant is
    /// missing or the field is the wrong shape.
    [[nodiscard]] f64 density_at(i32 x, i32 y, i32 z) const;

    /// What fills a position that is not solid: water, lava or air.
    [[nodiscard]] registry::BlockStateId fluid_at(i32 y) const;

    /// The biome at a *block* position, `minecraft:plains` shaped.
    ///
    /// The uncached lookup, deliberately. `BiomeSource`'s cache decides ties by
    /// the order the questions were asked, so a caller that asks in its own
    /// order — a structure placer walking chunks, a harness walking regions —
    /// would get answers that depend on its traversal. The structure layer asks
    /// exactly one question per chunk and must get the same answer whoever asks
    /// it, which is why this exists next to `generate_biomes` rather than
    /// inside it.
    [[nodiscard]] std::string_view biome_name_at(i32 x, i32 y, i32 z) const;

    /// The dimension's sea level, as the noise settings give it.
    ///
    /// Exposed because a `FeatureLevel` has to answer the same question — the
    /// `surface_water_depth_filter` and the springs ask it — and reading it out
    /// of the generator is the only way to be sure the two agree.
    [[nodiscard]] i32 sea_level() const noexcept { return sea_level_; }

    /// ── nether ── The generator's own vertical extent — the noise settings'
    /// `min_y` and `height`. What `above_bottom` / `below_top` resolve against
    /// when a feature is placed: 0 and 128 in the Nether, whose chunks are 256.
    [[nodiscard]] i32 gen_min_y() const noexcept { return router_->min_y(); }
    [[nodiscard]] i32 gen_depth() const noexcept { return router_->height(); }

    /// Attach the carvers, and with them the tag that says what they may cut.
    ///
    /// The two arrive together and cannot be separated, which is deliberate.
    /// Carving is not "remove stone": it is "remove anything in
    /// `minecraft:overworld_carver_replaceables`", and once the surface rules
    /// have run the top of a column is grass, dirt, sand or gravel — every one
    /// of them in that tag. A `set_carvers` that took only the stage would let
    /// a caller carve with the tag missing and quietly leave a crust of dirt
    /// hanging over every cave.
    ///
    /// Optional as a whole, and off by default: a generator that is only asked
    /// about the noise — a column dump, the biome comparison — would otherwise
    /// pay 867 carver seedings for a question the carvers do not answer.
    /// Borrowed, not owned; the stage must outlive the generator.
    [[nodiscard]] std::expected<void, CarverAttachError> set_carvers(
        const CarverStage* carvers, const registry::Registries& registries);

    /// Put the carvers back before the surface rules, cutting stone only —
    /// the order this generator had before the stages were separated.
    ///
    /// A measuring instrument, not a supported way to generate a world. It
    /// exists so that a harness can hold both orders at once and compare them
    /// on the same chunks in the same process: a reordering can make parity
    /// worse, and taking the before from one build and the after from another
    /// is how that goes unnoticed. Defaults to what `OV_CARVE_BEFORE_SURFACE`
    /// said at construction, which is off.
    void set_carve_before_surface(bool legacy) noexcept { carve_before_surface_ = legacy; }

    /// Whether a carver is allowed to replace this block.
    ///
    /// The membership of `minecraft:overworld_carver_replaceables`, resolved
    /// once at attach time into a flat lookup. Exposed so a test can check the
    /// resolution against the tag file rather than against itself.
    [[nodiscard]] bool is_carver_replaceable(registry::BlockId block) const noexcept;

private:
    /// Apply an already-computed carving mask to a chunk whose surface has been
    /// built. Split out because it is the stage this file is about.
    void apply_carving(world::Chunk& chunk, const CarvingMask& mask, i32 lava_level,
                       AquiferSampler* aquifer, std::vector<BlockPos>* fluid_updates) const;

    /// The block an aquifer answer puts down.
    [[nodiscard]] registry::BlockStateId state_of(Substance substance) const noexcept;

    const NoiseRouter*             router_;
    const BiomeSource*             biomes_;
    const registry::BlockRegistry* blocks_;
    const DensityFunction*         density_{nullptr};
    const SurfaceSystem*           surface_{nullptr};
    const CarverStage*             carvers_{nullptr};

    registry::BlockStateId stone_{};
    registry::BlockStateId water_{};
    registry::BlockStateId lava_{};
    registry::BlockStateId dirt_{};

    registry::BlockId grass_block_{};
    bool              has_grass_block_{false};

    /// One bit per block id: is it in the replaceables tag? A flat table rather
    /// than a binary search per cell — a chunk asks this once per carved cell
    /// and a busy chunk carves thousands.
    std::vector<bool> replaceable_;

    /// Run the carvers *before* the surface rules, and let them cut only
    /// stone — the order this generator had before the stages were separated.
    ///
    /// Kept, and switchable through `OV_CARVE_BEFORE_SURFACE`, for exactly one
    /// reason: a reordering can make parity worse, and the only honest way to
    /// say whether it did is to take the before and the after out of the same
    /// binary on the same sample. It is not a supported way to generate a
    /// world; it is a measuring instrument.
    bool carve_before_surface_{false};

    /// Whether a carved cell that currently holds a fluid is carved at all.
    ///
    /// On, because the tag says so — `minecraft:water` is a member. What a
    /// carved cell becomes is no longer decided here: with the aquifer active
    /// it is the aquifer's answer for a density of zero, so carved water under
    /// a sea bed usually comes back as water rather than being drained. The
    /// measurement that first settled this (cave interiors 70,53 % spared,
    /// 73,50 % cut, docs/provenance/ordre-des-etages.md § 5) was taken without
    /// an aquifer, when "carved" meant "air"; it predates
    /// docs/provenance/aquiferes.md § 10.
    ///
    /// `OV_CARVE_FLUIDS=0` puts the old choice back, as an instrument.
    bool carve_fluids_{true};

    /// Seed-level half of the aquifer. Immutable after construction, so one
    /// generator's aquifer is safe wherever the generator itself is; each
    /// stage builds its own per-chunk sampler on top.
    std::unique_ptr<const Aquifer> aquifer_;
    bool                           use_aquifer_{true};

    i32 sea_level_{63};
};

}  // namespace ov::worldgen
