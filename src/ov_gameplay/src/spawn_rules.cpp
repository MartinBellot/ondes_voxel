// ── mobs-2 ── What one mob type asks of its spawn position. See the header.
#include "ov/gameplay/spawn_rules.hpp"

#include "ov/gameplay/spawning.hpp"

#include <array>

namespace ov::gameplay {
namespace {

struct Named {
    std::string_view type;
    TypeSpawnRule    rule;
};

/// The types whose predicate is not the category's default. Everything a biome
/// file lists in the overworld is either here or takes the default, and the
/// default is the game's own (`animals_spawnable_on`, the monster darkness).
constexpr std::array<Named, 11> kRules{{
    {"minecraft:husk", {SpawnRule::MonsterUnderSky, {}}},
    {"minecraft:stray", {SpawnRule::MonsterUnderSky, {}}},
    {"minecraft:slime", {SpawnRule::Slime, {}}},
    {"minecraft:rabbit", {SpawnRule::Animal, "minecraft:rabbits_spawnable_on"}},
    {"minecraft:wolf", {SpawnRule::Animal, "minecraft:wolves_spawnable_on"}},
    {"minecraft:fox", {SpawnRule::Animal, "minecraft:foxes_spawnable_on"}},
    {"minecraft:frog", {SpawnRule::Animal, "minecraft:frogs_spawnable_on"}},
    {"minecraft:goat", {SpawnRule::Animal, "minecraft:goats_spawnable_on"}},
    {"minecraft:mooshroom", {SpawnRule::Animal, "minecraft:mooshrooms_spawnable_on"}},
    {"minecraft:parrot", {SpawnRule::Animal, "minecraft:parrots_spawnable_on"}},
    {"minecraft:axolotl", {SpawnRule::Animal, "minecraft:axolotls_spawnable_on"}},
}};

}  // namespace

bool monster_light_possible(const LightSource& light, BlockPos pos) {
    if (static_cast<i32>(light.block_light(pos)) > kMonsterBlockLightLimit) {
        return false;
    }
    return static_cast<i32>(light.effective_light(pos)) <= kMonsterLightLevelMax;
}

bool monster_dark_enough(const LightSource& light, BlockPos pos, math::LegacyRandomSource& random) {
    // One draw per line, in the documented order. Folding them into one
    // expression would let the compiler pick the order (briefing, trap 2).
    const i32 sky_draw = random.next_int(32);
    if (static_cast<i32>(light.sky_light(pos)) > sky_draw) {
        return false;
    }
    if (static_cast<i32>(light.block_light(pos)) > kMonsterBlockLightLimit) {
        return false;
    }
    const i32 level_draw = random.next_int(kMonsterLightLevelMax + 1);
    return static_cast<i32>(light.effective_light(pos)) <= level_draw;
}

TypeSpawnRule spawn_rule_of(std::string_view type_name, bool creature) noexcept {
    for (const Named& entry : kRules) {
        if (entry.type == type_name) {
            return entry.rule;
        }
    }
    if (creature) {
        return TypeSpawnRule{SpawnRule::Animal, "minecraft:animals_spawnable_on"};
    }
    return TypeSpawnRule{SpawnRule::Monster, {}};
}

bool is_slime_chunk(i64 world_seed, i32 chunk_x, i32 chunk_z) noexcept {
    // The documented expression, with Java's types: the first two products
    // and the last are `int` (they wrap), the third is an `int` widened to
    // `long` before its multiplication, and `^` binds last — it applies to the
    // whole sum. Unsigned arithmetic gives the wrap without undefined
    // behaviour; the casts back give Java's two's-complement values.
    const auto wrap = [](u32 v) { return static_cast<i64>(static_cast<i32>(v)); };
    const u32  x    = static_cast<u32>(chunk_x);
    const u32  z    = static_cast<u32>(chunk_z);
    const i64  a    = wrap(x * x * 0x4c1906U);
    const i64  b    = wrap(x * 0x5ac0dbU);
    const i64  c    = static_cast<i64>(static_cast<u64>(wrap(z * z)) * 0x4307a7ULL);
    const i64  d    = wrap(z * 0x5f24fU);
    const u64  sum  = static_cast<u64>(world_seed) + static_cast<u64>(a) + static_cast<u64>(b) +
                      static_cast<u64>(c) + static_cast<u64>(d);
    math::LegacyRandomSource random{static_cast<i64>(sum ^ 0x3ad8025fULL)};
    return random.next_int(10) == 0;
}

f32 moon_brightness(i64 day_time) noexcept {
    constexpr std::array<f32, 8> kPhase{1.0F, 0.75F, 0.5F, 0.25F, 0.0F, 0.25F, 0.5F, 0.75F};
    const i64                    day   = day_time / 24000;
    const i64                    phase = ((day % 8) + 8) % 8;
    return kPhase[static_cast<usize>(phase)];
}

}  // namespace ov::gameplay
