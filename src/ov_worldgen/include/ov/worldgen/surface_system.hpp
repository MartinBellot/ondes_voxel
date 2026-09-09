// The surface stage: everything the seed decides about what covers the stone.
//
// The rule tree in surface_rules.hpp is pure — it reads a context and names a
// block. This is the part that is not: the seeded noises the rules sample, the
// positional generators the bedrock floor draws from, the 192 clay bands of
// the badlands, and the walk down each column that fills in the context.
//
// The column walk is where the subtle state lives. Going down from the top of a
// column the stage tracks three things: how deep into the stone it is, how deep
// it would be counting from below, and where the water above it starts. All
// three are reset by air, and by water — a sea bed is one block deep, not
// forty — and the rules read all three constantly. `stone_depth` alone is 122
// of the overworld's conditions.
//
// What this reproduces and what it does not:
//
//   * Every rule and condition type the vanilla overworld, nether and end use.
//   * The bedrock floor and the deepslate transition, both positional and both
//     checkable one block at a time.
//   * The badlands clay bands.
//   * NOT the two extensions that are not rules at all: the eroded badlands
//     pillars and the frozen ocean icebergs are separate passes in the same
//     stage, and they are named here rather than silently absent. See
//     docs/provenance/surface-rules.md for what that costs, measured.
#pragma once

#include "ov/base/types.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/world/chunk.hpp"
#include "ov/worldgen/biome_source.hpp"
#include "ov/worldgen/density.hpp"
#include "ov/worldgen/surface_rules.hpp"

#include <expected>
#include <filesystem>
#include <memory>
#include <span>

namespace ov::worldgen {

class SurfaceSystem {
public:
    /// Read the settings file's `surface_rule` and seed everything it needs.
    ///
    /// The seed is the world seed, not a derived one: this forks its own
    /// positional factory from it exactly as the density router does, because
    /// vanilla's surface noises come from the same factory the density noises
    /// do and sharing a name would otherwise give them a different stream.
    [[nodiscard]] static std::expected<SurfaceSystem, SurfaceError> load(
        const std::filesystem::path& data_root, std::string_view settings, i64 seed,
        const registry::BlockRegistry& blocks);

    SurfaceSystem(SurfaceSystem&&) noexcept;
    SurfaceSystem& operator=(SurfaceSystem&&) noexcept;
    ~SurfaceSystem();

    /// Run the rules over a whole chunk the noise stage has already filled.
    ///
    /// The biomes must already be set: the rules ask about them constantly and
    /// a chunk whose biome array is still the default would put plains grass on
    /// a desert. That trap has a name in this repo — see the parity harness,
    /// which skips chunks the game left at a partial status for the same
    /// reason.
    void build(world::Chunk& chunk, const NoiseRouter& router, const BiomeSource& biomes,
               const registry::BlockRegistry& blocks) const;

    /// Run the rules over one column, in place.
    ///
    /// `column[0]` is at `min_y`. Exposed because it is the only way to check
    /// this stage on its own: hand it the terrain the real game wrote and the
    /// disagreements that come back are this stage's, not the noise's.
    void build_column(std::span<registry::BlockStateId> column, i32 world_x, i32 world_z,
                      const SurfaceQueries& queries, const registry::BlockRegistry& blocks) const;

    /// The soft-layer thickness this column was given. Per column and cheap to
    /// ask about, which is what makes the "hole" condition checkable.
    [[nodiscard]] i32 surface_depth(i32 x, i32 z) const;

    /// The badlands band at a height, for a test that compares the table
    /// against the real game rather than against itself.
    [[nodiscard]] registry::BlockStateId clay_band(i32 x, i32 y, i32 z) const;

    [[nodiscard]] i32 min_y() const noexcept;
    [[nodiscard]] i32 height() const noexcept;
    [[nodiscard]] i32 sea_level() const noexcept;

    /// The block the rules are allowed to replace. Everything else in a column
    /// — water, a carver's air, a band already placed — is left alone.
    [[nodiscard]] registry::BlockStateId default_block() const noexcept;

private:
    struct Impl;

    SurfaceSystem();

    std::unique_ptr<Impl> impl_;
};

}  // namespace ov::worldgen
