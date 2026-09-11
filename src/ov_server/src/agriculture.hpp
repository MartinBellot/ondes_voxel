// The random tick, and the plants that live on it, wired to the server.
//
// `gameplay::Plants` knows what a crop does when it is picked. What it cannot
// know is *which* positions are picked, because that depends on where the
// players are: vanilla gives random ticks only to a chunk whose centre is
// within 128 blocks of a player who is not a spectator — the same gate natural
// spawning uses. A world with nobody in it grows nothing, and measuring that
// on a real server is how a whole campaign reads "no growth at any speed".
//
// Three pieces live here, each for the reason `ov_gameplay` cannot hold it:
//
//   * `RandomTicks` — the picking. `randomTickSpeed` positions per non-empty
//     16x16x16 section per tick, with replacement, from one explicit seeded
//     generator. It walks the chunks' own sections, so it never asks the chunk
//     map for a chunk per pick.
//   * `TreeGrower` — a sapling becoming a tree. The placers are worldgen,
//     layer 10; the server is layer 12 and may use them.
//   * `ServerPlantEnvironment` — the light, the drops and the trees a plant
//     asks for, over callbacks, so this file never learns what a chunk mutex
//     or a player is.
#pragma once

#include "world_ticks.hpp"

#include "ov/gameplay/plants.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/math/vec.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/world/chunk_map.hpp"
#include "ov/world/level.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace ov::server {

/// Sapling to tree, through the configured features the datapack defines.
///
/// Which feature each sapling grows is from the wiki's Sapling page, and the
/// tree itself is the same interpreter the world generator uses — the one
/// measured tree by tree against the reference world in
/// docs/provenance/features.md.
class TreeGrower {
public:
    /// Read the configured features. Nothing when they cannot be read, and the
    /// caller then says saplings do not grow rather than growing nothing.
    [[nodiscard]] static std::unique_ptr<TreeGrower> load(const std::filesystem::path& data_root,
                                                          const registry::BlockRegistry& blocks);

    ~TreeGrower();
    TreeGrower(const TreeGrower&)            = delete;
    TreeGrower& operator=(const TreeGrower&) = delete;

    /// Grow the sapling at `pos` into a tree. False — and the sapling left
    /// where it was — when the tree does not fit or this sapling is not one
    /// this file grows.
    bool grow(world::LevelWriter& level, BlockPos pos, registry::BlockStateId sapling,
              gameplay::PlantRandom& random) const;

private:
    struct Impl;
    explicit TreeGrower(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

/// What the plant environment reaches outside itself for.
struct PlantHooks {
    std::function<u8(BlockPos)> block_light;
    std::function<u8(BlockPos)> sky_light;
    std::function<u8()>         sky_darken;
    /// Spawn the loot of a block a rule broke. Called before the block is
    /// replaced, because the loot table reads the state.
    std::function<void(BlockPos, registry::BlockStateId)> drop_block;
};

/// `gameplay::PlantEnvironment` over callbacks.
class ServerPlantEnvironment final : public gameplay::PlantEnvironment {
public:
    ServerPlantEnvironment(PlantHooks hooks, const TreeGrower* trees);

    [[nodiscard]] u8 block_light(BlockPos pos) const override;
    [[nodiscard]] u8 sky_light(BlockPos pos) const override;
    [[nodiscard]] u8 sky_darken() const override;

    /// This server has no weather: it never rains, and farmland that would
    /// have been kept wet by rain dries. Said here rather than returned
    /// silently from a callback nobody reads.
    [[nodiscard]] bool is_raining_at(BlockPos) const override { return false; }

    void drop_block(BlockPos pos, registry::BlockStateId state) override;
    bool grow_tree(world::LevelWriter& level, BlockPos pos, registry::BlockStateId sapling,
                   gameplay::PlantRandom& random) override;

    /// Trees asked for and refused because no grower was loaded.
    [[nodiscard]] u64 trees_refused() const noexcept { return trees_refused_; }

private:
    PlantHooks        hooks_;
    const TreeGrower* trees_{nullptr};
    u64               trees_refused_{0};
};

/// What one random-tick pass did. Instrumentation.
struct RandomTickStats {
    usize chunks{0};
    usize sections{0};
    usize picks{0};
    /// Picks that landed on something that answers a random tick.
    usize ticked{0};
};

// ── fire ─────────────────────────────────────────────────────────────────────
/// A second answer to the random tick, owned outside this file: lava lighting
/// fire (fire_session.hpp). Asked at the same pick, when the plants pass —
/// vanilla ticks a block and then its fluid, and lava is no plant.
class RandomTickExtension {
public:
    RandomTickExtension()                                      = default;
    RandomTickExtension(const RandomTickExtension&)            = delete;
    RandomTickExtension& operator=(const RandomTickExtension&) = delete;
    RandomTickExtension(RandomTickExtension&&)                 = delete;
    RandomTickExtension& operator=(RandomTickExtension&&)      = delete;
    virtual ~RandomTickExtension()                             = default;

    [[nodiscard]] virtual bool ticks_randomly(registry::BlockStateId state) const noexcept = 0;
    virtual void random_tick(world::LevelWriter& level, BlockPos pos,
                             registry::BlockStateId state) = 0;
};
// ── end fire ─────────────────────────────────────────────────────────────────

/// The picking: which positions a tick reaches.
class RandomTicks {
public:
    /// The seed is fixed by the caller, never drawn from a clock (principle 5).
    explicit RandomTicks(u64 seed);

    /// `randomTickSpeed`. Three by default, as in vanilla; zero stops every
    /// plant. The setter a `/gamerule` command calls.
    void               set_speed(i32 speed) noexcept { speed_ = speed < 0 ? 0 : speed; }
    [[nodiscard]] i32  speed() const noexcept { return speed_; }

    /// The horizontal distance, in blocks, from a chunk's centre to the nearest
    /// non-spectator player within which the chunk gets random ticks.
    static constexpr f64 kPlayerRange = 128.0;

    /// Decide which chunks are ticked: ticking by their ticket level **and**
    /// within `kPlayerRange` of one of `players`. Sorted, so the order does not
    /// depend on a hash map. Allocates only when the set outgrows its reserve.
    void select(const world::ChunkMap& chunks, std::span<const Vec3d> players);

    [[nodiscard]] std::span<const ChunkPos> selected() const noexcept { return chunks_; }

    /// One tick's worth of picks over the selected chunks.
    ///
    /// Walks each chunk's sections directly. A chunk whose eight neighbours
    /// are not all resident is skipped for the tick: a stem setting a melon, or
    /// grass spreading, one block over the edge would otherwise write into a
    /// chunk that is not there yet, and on this server writing there generates
    /// it on the tick thread.
    RandomTickStats run(world::LevelWriter& level, world::ChunkMap& chunks,
                        const gameplay::Plants& plants, gameplay::PlantEnvironment& env);

    /// One section's picks, for a caller that holds sections of its own — the
    /// statistical test drives exactly this, so the measurement passes through
    /// the same code the server runs.
    void tick_section(world::LevelWriter& level, const world::ChunkSection& section, BlockPos origin,
                      const gameplay::Plants& plants, gameplay::PlantEnvironment& env,
                      RandomTickStats& stats);

    // ── fire ──
    /// Hand the picks the plants pass to a second engine. Not owned.
    void set_extension(RandomTickExtension* extension) noexcept { extension_ = extension; }

private:
    gameplay::PlantRandom random_;
    i32                   speed_{3};
    std::vector<ChunkPos> chunks_;
    RandomTickExtension*  extension_{nullptr};  // ── fire ──
};

}  // namespace ov::server
