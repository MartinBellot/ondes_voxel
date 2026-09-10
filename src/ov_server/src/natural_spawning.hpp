// Natural spawning, joined to the server's chunks.
//
// `NaturalSpawner` has been complete and measured since M2 and nothing called
// it: a world only ever held the mobs `--mobs=` put there by hand. What was
// missing is one thing the spawner cannot supply itself — **light** — plus the
// bookkeeping that turns a `ChunkMap` into the three spans `SpawnEnvironment`
// asks for.
//
// Light is the interesting half. The spawn rule compares a number that does not
// exist anywhere in the world: `max(block_light, sky_light - sky_darken)`. The
// server already computes both arrays (`relight_chunk` and `relight_blocks`)
// and stores them per section, so this is an adapter over storage that is
// already right, not a second light engine. Getting it wrong in the safe
// direction is not safe here — a spawner that reads zero everywhere fills a lit
// room with zombies, and one that reads fifteen everywhere spawns nothing at
// all and looks exactly like a spawner that is not wired up.
//
// ── Where the mob lists come from ──────────────────────────────────────────
//
// A biome's `spawners` block is datapack data, and this project does not parse
// datapacks yet. Rather than invent a distribution — which `spawning.hpp` says
// in as many words it will not accept — the lists are read at start-up straight
// out of the data generator's own output, `data/vanilla/1.20.1/generated/data/
// minecraft/worldgen/biome/<name>.json`. Nothing is committed; if the generated
// data is absent the server says so and spawns nothing, which is a stated gap
// rather than an empty world nobody can explain.
//
// The **stated limitation**: one biome's list is used for the whole world,
// because the chunk's biome array is not yet consulted per spawn attempt. The
// biome is named in the log line at start-up so that what is running is never a
// guess.
#pragma once

#include "ov/gameplay/spawning.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/level.hpp"

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ov::server {

/// The two light arrays the spawn rule reads, and the time of day.
///
/// A callback pair rather than a chunk map, for the reason every adapter in
/// this directory is: the light lives inside the server's lock and only the
/// server knows how to take it.
struct LightHooks {
    std::function<u8(BlockPos)> block_light;
    std::function<u8(BlockPos)> sky_light;
    /// 0 at noon, 11 at midnight. The level does not know what time it is, so
    /// the caller says.
    std::function<u8()> sky_darken;
};

class ChunkLight final : public gameplay::LightSource {
public:
    explicit ChunkLight(LightHooks hooks);

    [[nodiscard]] u8 block_light(BlockPos pos) const override;
    [[nodiscard]] u8 sky_light(BlockPos pos) const override;
    [[nodiscard]] u8 sky_darken() const override;

private:
    LightHooks hooks_;
};

/// How much the sky is dimmed at a given time of day.
///
/// Vanilla's `Level.getSkyDarken`: a cosine of the day fraction, clamped, times
/// eleven. The result is what turns a sky-lit surface into a spawnable one at
/// dusk, so it is the whole reason monsters appear at night rather than never.
[[nodiscard]] u8 sky_darken_for(i64 day_time) noexcept;

/// The spawner entries for one biome, read from the data generator's output.
///
/// Returns false, having changed nothing, when the file is missing or does not
/// carry a `spawners` block — refused rather than defaulted, because a spawner
/// configured with an empty list is indistinguishable from one that is not
/// wired up.
[[nodiscard]] bool load_biome_spawners(const std::filesystem::path& generated_root,
                                       std::string_view biome_name, gameplay::NaturalSpawner& into,
                                       std::vector<std::string>& name_storage);

// ── mobs-2 ──────────────────────────────────────────────────────────────────
//
// The limitation named above is lifted: every biome's lists are loaded, keyed
// by the index the chunk stores (the registry codec's order), and the spawner
// asks the biome **of each position** through `ChunkBiomes`.

/// The biome at a position, over the server's chunks. One callback, built
/// once — constructing a std::function inside the tick would allocate there.
class ChunkBiomes final : public gameplay::BiomeLookup {
public:
    explicit ChunkBiomes(std::function<u16(BlockPos)> at) : at_{std::move(at)} {}
    [[nodiscard]] u16 biome_at(BlockPos pos) const override { return at_ ? at_(pos) : u16{0}; }

private:
    std::function<u16(BlockPos)> at_;
};

/// Every biome's `spawners`, by codec index, and the surface-slime tag.
///
/// Returns how many biomes carried a list. Biomes whose file is missing get no
/// list and spawn nothing — refused, never given another biome's list.
[[nodiscard]] usize load_all_biome_spawners(const std::filesystem::path&      generated_root,
                                            std::span<const std::string_view> biome_names,
                                            gameplay::NaturalSpawner&         into,
                                            std::vector<std::string>&         name_storage);
// ── end mobs-2 ──

}  // namespace ov::server
