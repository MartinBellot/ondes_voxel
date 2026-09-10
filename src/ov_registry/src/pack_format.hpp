// The on-disk layout of registry.ovpack, shared by everything that reads it.
//
// Private to ov_registry: the format is an implementation detail, and a public
// header exposing it would make every change to the emitter an ABI change for
// callers who have no business knowing the file exists.
//
// Little-endian throughout, unlike the rest of the project. This format is
// ours rather than Mojang's, so matching the host's byte order costs nothing
// and saves a swap on every field.
//
// The single rule: this file and tools/ov_datagen/ovpack.py must agree exactly.
// The static_asserts below catch a padding difference; nothing catches a field
// added on one side only, so they are changed together.
#pragma once

#include "ov/base/types.hpp"
#include "ov/registry/recipe_data.hpp"

#include <vector>

namespace ov::registry {

/// Bumped by hand whenever the layout changes. A stale cache read as if it
/// were current is far worse than no cache: the ids would be plausible and
/// wrong, and nothing would report an error until a vanilla client crashed on
/// an entity type that does not exist.
inline constexpr u32 kFormatVersion = 15;

/// Grew past 128 when the loot tables arrived.
inline constexpr u32 kHeaderSize = 256;

struct PackHeader {
    char magic[4];
    u32  format_version;
    u32  block_count;
    u32  state_count;
    u32  property_count;
    u32  value_count;
    u32  string_bytes;
    u32  strings_offset;
    u32  blocks_offset;
    u32  properties_offset;
    u32  values_offset;
    u32  states_offset;
    u32  registry_count;
    u32  entry_count;
    u32  registries_offset;
    u32  entries_offset;
    u32  tag_count;
    u32  member_count;
    u32  tags_offset;
    u32  members_offset;
    u32  flags_offset;
    u32  stacks_offset;
    u32  item_count;
    /// One bit per block state: does it hold a fluid.
    u32 fluid_offset;
    /// One float per block: how long it takes to break, -1 for never.
    u32 hardness_offset;

    /// The compiled block loot tables. See ov/registry/loot_data.hpp.
    u32 loot_tables_offset;
    u32 loot_pools_offset;
    u32 loot_entries_offset;
    u32 loot_conds_offset;
    u32 loot_funcs_offset;
    u32 loot_floats_offset;
    u32 loot_ints_offset;
    u32 loot_pool_count;
    u32 loot_entry_count;
    u32 loot_cond_count;
    u32 loot_func_count;
    u32 loot_float_count;
    u32 loot_int_count;

    /// Collision shapes: boxes in units of 1/32, one record per shape, and one
    /// shape index per block state.
    u32 boxes_offset;
    u32 shapes_offset;
    u32 state_shapes_offset;
    u32 box_count;
    u32 shape_count;

    /// One byte per state: the light it gives off, 0 to 15.
    u32 emission_offset;

    /// Biome effects, sorted by name. A dynamic registry, so it is not in the
    /// registries report and gets a section of its own.
    u32 biomes_offset;
    u32 biome_count;

    /// Entity types: one record each, in registry order, plus the flat table
    /// of attribute base values their records point into.
    u32 entities_offset;
    u32 entity_attrs_offset;
    u32 entity_count;

    /// The compiled recipes. See ov/registry/recipe_data.hpp.
    u32 recipes_offset;
    u32 recipe_ingredients_offset;
    u32 recipe_choices_offset;
    u32 recipe_count;
    u32 recipe_ingredient_count;
    u32 recipe_choice_count;

    /// Burn times, `kFuelKinds` tables of one u16 per item, in the order
    /// furnace, blast furnace, smoker. Zero means "not a fuel", which here is
    /// a measurement and not a default: every item was tried.
    u32 fuel_offset;

    /// One i32 per item: the item left behind when it is consumed by a recipe,
    /// or -1 for none. Measured, like the burn times.
    u32 remainder_offset;

    /// One float per block: how much of an explosion's energy it eats.
    ///
    /// Measured, like the hardness beside it — see docs/provenance/explosions.md.
    u32 resistance_offset;

    /// ── sound ── One BlockSoundRecord per block, then one EntitySoundRecord
    /// per entity type in registry order. Measured — see docs/provenance/son.md.
    u32 block_sounds_offset;
    u32 entity_sounds_offset;
};

/// What one block sounds like. Event ids index minecraft:sound_event; 0xFFFF
/// is "none".
///
/// `measured`: bit 0 place, bit 1 step, bit 2 fall were **heard on the wire**
/// by another player; bit 3 says break and hit were **inferred** from the
/// family the others name (the client plays both itself, so no packet carries
/// them); bit 4 says open/close were heard. `audience` packs who hears the
/// open (bits 0-1) and the close (bits 2-3): 1 everyone, 2 everyone but the
/// player who did it, 0 not measured.
struct BlockSoundRecord {
    u16 events[7];  // break, step, place, hit, fall, open, close
    u8  measured;
    u8  audience;
    f32 volume;
    f32 pitch;
    f32 open_volume;
    f32 open_pitch_lo;
    f32 open_pitch_hi;
    f32 close_volume;
    f32 close_pitch_lo;
    f32 close_pitch_hi;
};

/// What one entity type sounds like when hurt, killed, or left alone.
/// `measured`: bit 0 hurt heard, bit 1 death heard, bit 2 the ambient event
/// exists by name (not heard: see son.md).
struct EntitySoundRecord {
    u16 hurt;
    u16 death;
    u16 ambient;
    u8  category;  // Mojang's order; 0xFF unmeasured
    u8  measured;
    f32 volume;
    f32 pitch_lo;
    f32 pitch_hi;
};

static_assert(sizeof(BlockSoundRecord) == 48, "layout must match the emitter");
static_assert(sizeof(EntitySoundRecord) == 20, "layout must match the emitter");

/// One entity type, measured on a running 1.20.1 server.
///
/// None of these three numbers appears in any Mojang report: hitboxes, eye
/// heights and attribute base values are Java code. They come from
/// scripts/measure_entities.py, which asks the game itself — see
/// docs/PROVENANCE.md.
struct EntityTypeRecord {
    f32 width;
    f32 height;
    f32 eye_height;
    u16 attribute_first;
    u8  attribute_count;
    /// Bit 0: the hitbox was measured. Bit 1: the eye height was.
    ///
    /// A separate bit rather than "width == 0 means unknown", because four
    /// types genuinely could not be measured — the lightning bolt lives one
    /// tick, the fishing bobber cannot exist without an angler — and a zero
    /// box would make them things nothing can ever hit, silently.
    u8 measured;
};

/// One attribute a type owns, and the base value it starts with.
struct EntityAttributeRecord {
    /// Index into minecraft:attribute, in Mojang's order.
    u8 attribute;
    u8 pad[7];
    /// Base value. f64 because that is what the game prints and what the wire
    /// carries: a movement speed of 0.23 is really 0.23000000417232513, the
    /// double nearest to the float the game holds, and rounding it to f32 here
    /// and back would not round-trip.
    f64 base;
};

static_assert(sizeof(EntityTypeRecord) == 16, "layout must match the emitter");
static_assert(sizeof(EntityAttributeRecord) == 16, "layout must match the emitter");

/// One biome's effects, exactly the fields vanilla sends in Registry Data.
///
/// The grass and foliage colours are absent on purpose for most biomes:
/// vanilla computes them from the climate against a colormap in the resource
/// pack, and that computation belongs where the texture is. What travels is
/// the climate it needs, and the override for the biomes that ignore it.
struct BiomeRecord {
    /// f64 and not f32: the colormap index truncates `(1 - t) * 255`, and a
    /// temperature of 0.6 sits a millionth away from the boundary between two
    /// columns. In double it indexes 102, in float 101, and birch forest's
    /// published grass colour agrees with the double. Measured, not assumed.
    f64 temperature;
    f64 downfall;
    u32 name_offset;
    u32 water_colour;
    u32 water_fog_colour;
    u32 fog_colour;
    u32 sky_colour;
    /// -1 for "no override". Black is a colour a biome could legitimately ask
    /// for, so zero cannot mean absent.
    i32 grass_colour;
    i32 foliage_colour;
    /// 0 none, 1 dark_forest, 2 swamp.
    u8 grass_modifier;
    /// 0 none, 1 frozen.
    u8 temperature_modifier;
    u8 has_precipitation;
    u8 pad;
};

static_assert(sizeof(BiomeRecord) == 48, "layout must match the emitter");

/// One registry: its name, the span of entries it owns, and the numeric id its
/// first entry carries.
struct RegistryRecord {
    u32 name_offset;
    u32 entry_first;
    u32 entry_count;
    /// Zero for every registry except minecraft:mob_effect, which is 1-based.
    /// Storing it rather than special-casing one name keeps the rule in the
    /// data, where the next version can change it.
    u32 first_id;
};

/// One tag: its name, which registry it belongs to, and the span of member ids
/// it resolves to. Members are ids rather than names — the '#' references are
/// flattened at build time, so the game never walks the tag graph.
struct TagRecord {
    u32 name_offset;
    u32 registry_index;
    u32 member_first;
    u32 member_count;
};

static_assert(sizeof(RegistryRecord) == 16, "layout must match the emitter");
static_assert(sizeof(RecipeRecord) == 44, "layout must match the emitter");
static_assert(sizeof(RecipeIngredientRecord) == 8, "layout must match the emitter");
static_assert(sizeof(TagRecord) == 16, "layout must match the emitter");
static_assert(sizeof(PackHeader) <= kHeaderSize, "header must fit its reserved space");

/// A bounds-checked view of `count` records at `offset`, or nullptr.
///
/// Every section offset in the file is attacker-influenced in the sense that
/// matters here: a truncated or corrupted pack must fail to load rather than
/// read past the buffer.
template<typename T>
[[nodiscard]] inline const T* pack_at(const std::vector<u8>& data, usize offset,
                                      usize count) noexcept {
    if (count != 0 && offset + count * sizeof(T) > data.size()) {
        return nullptr;
    }
    if (offset > data.size()) {
        return nullptr;
    }
    return reinterpret_cast<const T*>(data.data() + offset);
}

}  // namespace ov::registry
