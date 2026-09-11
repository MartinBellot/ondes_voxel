// ── mobs-2 ── What one mob *type* asks of the place it spawns.
//
// `NaturalSpawner` asks the category's questions — loaded, far from players,
// room for a body. The game then asks the type's own: an animal wants its own
// floor (a rabbit sand or snow, a wolf snow, a cow grass), a husk or a stray
// wants the open sky, a slime wants a swamp at the right height or a slime
// chunk, and a monster wants darkness — which is a *draw*, not a threshold.
//
// ── Sources ─────────────────────────────────────────────────────────────────
//
// * The floors are the data generator's block tags
//   (`tags/blocks/<type>s_spawnable_on.json`), read through the registry.
// * The darkness test is the dimension type's `monster_spawn_light_level`
//   (uniform 0..7) and `monster_spawn_block_light_limit` (0) from
//   `dimension_type/overworld.json`, applied as minecraft.wiki *Mob spawning*
//   documents it: the stored sky light must not exceed a draw in 0..31, the
//   block light must not exceed the limit, and the light level must not exceed
//   a draw from the provider. docs/provenance/mobs-2.md § 3 says why the
//   previous "light 0 exactly" rule could not produce a surface monster at
//   night, and what the real server does.
// * The slime rules are minecraft.wiki *Slime* § Spawning: a swamp between
//   y 51 and 69, a moon-phase chance and a light draw; or a slime chunk below
//   y 40, one attempt in ten.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/math/random.hpp"

#include <string_view>

namespace ov::gameplay {

class LightSource;

/// The overworld's dimension-type numbers. Data-generator values, not ours.
inline constexpr i32 kMonsterBlockLightLimit = 0;
inline constexpr i32 kMonsterLightLevelMax   = 7;

/// A monster's darkness test, with its draws in the game's order: sky light
/// against 0..31, then the block light limit, then the light level against
/// 0..7. Three draws at most; the first failure returns.
[[nodiscard]] bool monster_dark_enough(const LightSource& light, BlockPos pos,
                                       math::LegacyRandomSource& random);

/// The part of the darkness test no draw can rescue: block light over the
/// limit, or a light level above the provider's maximum. Asked before a type
/// is drawn, so a lit position costs no draw.
[[nodiscard]] bool monster_light_possible(const LightSource& light, BlockPos pos);

/// What a type's spawn predicate needs beyond the category's.
enum class SpawnRule : u8 {
    /// Darkness draw, then the category's floor and headroom.
    Monster,
    /// Darkness, and the open sky above — husk and stray.
    MonsterUnderSky,
    /// The slime's own: swamp surface or slime chunk. No darkness draw.
    Slime,
    /// A floor from a block tag, and a raw light level above 8.
    Animal,
};

/// The rule, and for an animal the tag naming its floor.
struct TypeSpawnRule {
    SpawnRule        rule{SpawnRule::Monster};
    std::string_view floor_tag{};
};

/// The rule for a type. A creature not listed gets `animals_spawnable_on`,
/// which is the game's default; a monster not listed gets `Monster`.
[[nodiscard]] TypeSpawnRule spawn_rule_of(std::string_view type_name, bool creature) noexcept;

/// Is this chunk a slime chunk? minecraft.wiki *Slime*: a Java `Random`
/// seeded from the world seed and the chunk position, `nextInt(10) == 0`.
[[nodiscard]] bool is_slime_chunk(i64 world_seed, i32 chunk_x, i32 chunk_z) noexcept;

/// The moon's brightness for a day time, by phase: full moon 1.0 down to new
/// moon 0.0 and back. minecraft.wiki *Moon*.
[[nodiscard]] f32 moon_brightness(i64 day_time) noexcept;

}  // namespace ov::gameplay
