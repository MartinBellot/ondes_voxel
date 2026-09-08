// Turning the density function graph into blocks.
//
// The noise stage and nothing else: for every position, the final density
// decides solid or not, and what is not solid is water below the sea level,
// lava below y = -54, and air above. That is `NoiseBasedChunkGenerator`'s
// `fillFromNoise` with the two things that come after it left out — aquifers,
// which put water in caves, and the carvers, which cut the caves in the first
// place.
//
// Leaving them out is deliberate for now and it is measurable: a comparison
// against a world the real game generated shows exactly how much of it they
// account for, which is a better way to decide what to build next than
// guessing.
#pragma once

#include "ov/base/types.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/world/chunk.hpp"
#include "ov/worldgen/biome_source.hpp"
#include "ov/worldgen/carver.hpp"
#include "ov/worldgen/density.hpp"

#include <expected>

namespace ov::worldgen {

class ChunkGenerator {
public:
    ChunkGenerator(const NoiseRouter& router, const BiomeSource& biomes,
                   const registry::BlockRegistry& blocks);

    /// Fill a chunk's blocks and biomes from the noise.
    ///
    /// The chunk must already have the world's shape; only its contents are
    /// written.
    void generate(world::Chunk& chunk) const;

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

    /// Attach the carvers. Optional, and off by default: a generator that is
    /// only asked about the noise — a column dump, the biome comparison —
    /// would otherwise pay 867 carver seedings for a question the carvers do
    /// not answer. Borrowed, not owned; the stage must outlive the generator.
    void set_carvers(const CarverStage* carvers) noexcept { carvers_ = carvers; }

private:
    const NoiseRouter*             router_;
    const BiomeSource*             biomes_;
    const registry::BlockRegistry* blocks_;
    const DensityFunction*         density_{nullptr};
    const CarverStage*             carvers_{nullptr};

    registry::BlockStateId stone_{};
    registry::BlockStateId water_{};
    registry::BlockStateId lava_{};

    i32 sea_level_{63};
};

}  // namespace ov::worldgen
