#include "ov/registry/block_states.hpp"

#include "pack_format.hpp"

#include "ov/io/byte_reader.hpp"
#include "ov/io/file.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace ov::registry {
namespace {

struct BlockRecord {
    u32 name_offset;
    u16 base_state;
    u16 state_count;
    u16 default_state;
    u16 property_first;
    u16 property_count;
    u16 padding;
};

/// A shape: where its boxes start, how many, and which faces are full.
struct ShapeRecord {
    u32 first;
    u16 count;
    u16 sturdy;
};

static_assert(sizeof(ShapeRecord) == 8, "layout must match the emitter");

struct PropertyRecord {
    u32 name_offset;
    u32 value_first;
    u16 value_count;
    u16 stride;
};

static_assert(sizeof(BlockRecord) == 16, "layout must match the emitter");
static_assert(sizeof(PropertyRecord) == 12, "layout must match the emitter");

}  // namespace

std::string_view to_string(RegistryError error) noexcept {
    switch (error) {
        case RegistryError::FileNotFound: return "registry pack not found";
        case RegistryError::Corrupt: return "registry pack is corrupt";
        case RegistryError::VersionMismatch: return "registry pack was built by another version";
    }
    return "unknown registry error";
}

struct BlockRegistry::Impl {};

std::expected<BlockRegistry, RegistryError> BlockRegistry::from_bytes(std::vector<u8> data) {
    if (data.size() < kHeaderSize) {
        return std::unexpected{RegistryError::Corrupt};
    }

    PackHeader header{};
    std::memcpy(&header, data.data(), sizeof(header));

    if (std::memcmp(header.magic, "OVPK", 4) != 0) {
        return std::unexpected{RegistryError::Corrupt};
    }
    if (header.format_version != kFormatVersion) {
        // A stale cache read as if it were current is worse than no cache: the
        // ids would be plausible and wrong.
        return std::unexpected{RegistryError::VersionMismatch};
    }

    const auto* blocks = pack_at<BlockRecord>(data, header.blocks_offset, header.block_count);
    const auto* props =
        pack_at<PropertyRecord>(data, header.properties_offset, header.property_count);
    const auto* values = pack_at<u32>(data, header.values_offset, header.value_count);
    const auto* states = pack_at<u16>(data, header.states_offset, header.state_count);

    // Measured block flags. Bounds-checked like every other section: a pack
    // from an older emitter has no such section, and reading where it would be
    // would hand back whatever follows.
    const auto* flags = pack_at<u8>(data, header.flags_offset, header.block_count);

    // One bit per state, so the length is in bytes, not states.
    const u32   fluid_bytes = (header.state_count + 7) / 8;
    const auto* fluids      = pack_at<u8>(data, header.fluid_offset, fluid_bytes);
    const auto* hardness    = pack_at<f32>(data, header.hardness_offset, header.block_count);
    const auto* resistance  = pack_at<f32>(data, header.resistance_offset, header.block_count);
    const auto* sound_records =
        pack_at<BlockSoundRecord>(data, header.block_sounds_offset, header.block_count);
    const auto* shape_boxes =
        pack_at<BlockRegistry::Box>(data, header.boxes_offset, header.box_count);
    const auto* shape_records =
        pack_at<ShapeRecord>(data, header.shapes_offset, header.shape_count);
    const auto* state_shapes = pack_at<u16>(data, header.state_shapes_offset, header.state_count);
    // One nibble a state would halve it, and the whole table is 24 kB. Bytes
    // until something measures the difference.
    const auto* emission = pack_at<u8>(data, header.emission_offset, header.state_count);
    const auto* biomes   = pack_at<BiomeRecord>(data, header.biomes_offset, header.biome_count);

    if (blocks == nullptr || props == nullptr || values == nullptr || states == nullptr ||
        flags == nullptr || fluids == nullptr || hardness == nullptr || resistance == nullptr ||
        sound_records == nullptr ||
        shape_boxes == nullptr ||
        shape_records == nullptr || state_shapes == nullptr || emission == nullptr ||
        biomes == nullptr) {
        return std::unexpected{RegistryError::Corrupt};
    }
    if (header.strings_offset + header.string_bytes > data.size()) {
        return std::unexpected{RegistryError::Corrupt};
    }

    BlockRegistry registry;
    registry.data_ = std::move(data);

    // Rebound after the move: the pointers above address the buffer that was
    // just moved from, and using one of them later reads freed memory.
    registry.block_flags_ =
        std::span{reinterpret_cast<const u8*>(registry.data_.data() + header.flags_offset),
                  header.block_count};
    registry.fluid_bits_ = std::span{
        reinterpret_cast<const u8*>(registry.data_.data() + header.fluid_offset), fluid_bytes};
    registry.hardness_ =
        std::span{reinterpret_cast<const f32*>(registry.data_.data() + header.hardness_offset),
                  header.block_count};
    registry.resistance_ =
        std::span{reinterpret_cast<const f32*>(registry.data_.data() + header.resistance_offset),
                  header.block_count};
    registry.sounds_ = std::span{registry.data_.data() + header.block_sounds_offset,
                                 static_cast<usize>(header.block_count) * sizeof(BlockSoundRecord)};
    registry.boxes_ = std::span{
        reinterpret_cast<const BlockRegistry::Box*>(registry.data_.data() + header.boxes_offset),
        header.box_count};
    registry.shape_records_ =
        std::span{reinterpret_cast<const u32*>(registry.data_.data() + header.shapes_offset),
                  header.shape_count * 2};
    registry.state_shapes_ =
        std::span{reinterpret_cast<const u16*>(registry.data_.data() + header.state_shapes_offset),
                  header.state_count};

    registry.emission_ =
        std::span{reinterpret_cast<const u8*>(registry.data_.data() + header.emission_offset),
                  header.state_count};

    registry.biomes_      = registry.data_.data() + header.biomes_offset;
    registry.biome_count_ = header.biome_count;

    registry.loot_ = LootData{
        std::span{reinterpret_cast<const LootTableRecord*>(registry.data_.data() +
                                                           header.loot_tables_offset),
                  header.block_count},
        std::span{reinterpret_cast<const LootPoolRecord*>(registry.data_.data() +
                                                          header.loot_pools_offset),
                  header.loot_pool_count},
        std::span{reinterpret_cast<const LootEntryRecord*>(registry.data_.data() +
                                                           header.loot_entries_offset),
                  header.loot_entry_count},
        std::span{reinterpret_cast<const LootConditionRecord*>(registry.data_.data() +
                                                               header.loot_conds_offset),
                  header.loot_cond_count},
        std::span{reinterpret_cast<const LootFunctionRecord*>(registry.data_.data() +
                                                              header.loot_funcs_offset),
                  header.loot_func_count},
        std::span{reinterpret_cast<const f32*>(registry.data_.data() + header.loot_floats_offset),
                  header.loot_float_count},
        std::span{reinterpret_cast<const u32*>(registry.data_.data() + header.loot_ints_offset),
                  header.loot_int_count},
    };

    const auto* string_base =
        reinterpret_cast<const char*>(registry.data_.data() + header.strings_offset);
    registry.strings_blob_   = std::string_view{string_base, header.string_bytes};
    const usize string_bytes = header.string_bytes;

    auto view_at = [&](u32 offset) -> std::string_view {
        if (offset >= string_bytes) {
            return {};
        }
        const char* start  = string_base + offset;
        const usize max    = string_bytes - offset;
        const usize length = ::strnlen(start, max);
        return std::string_view{start, length};
    };

    // Every value's text, resolved once. Doing it per query would undo the
    // point of the format.
    registry.values_.reserve(header.value_count);
    for (u32 i = 0; i < header.value_count; ++i) {
        registry.values_.push_back(view_at(values[i]));
    }

    registry.properties_.reserve(header.property_count);
    for (u32 i = 0; i < header.property_count; ++i) {
        const PropertyRecord& record = props[i];
        if (record.value_first + record.value_count > header.value_count) {
            return std::unexpected{RegistryError::Corrupt};
        }
        registry.properties_.push_back(
            PropertyView{view_at(record.name_offset),
                         std::span<const std::string_view>{
                             registry.values_.data() + record.value_first, record.value_count},
                         record.stride});
    }

    registry.strings_.reserve(header.block_count);
    for (u32 i = 0; i < header.block_count; ++i) {
        registry.strings_.push_back(view_at(blocks[i].name_offset));
    }

    registry.header_ = registry.data_.data();
    registry.build_fluid_table();  // ── implicit water ──
    return registry;
}

// ── implicit water ──────────────────────────────────────────────────────────

namespace {

/// The blocks whose fluid is always a water source, with no property to say
/// so. The Minecraft Wiki (Waterlogging, Seagrass, Kelp, Bubble Column) names
/// them; four were also measured on a real 1.20.1 server, raising
/// MOTION_BLOCKING as only a fluid does (the `fluid` bit of motion.json). The
/// fifth, kelp_plant, is the body of a kelp column and can never be a column's
/// top, so that oracle could not see it. docs/provenance/eau-implicite.md.
constexpr std::string_view kImplicitWater[] = {"minecraft:seagrass", "minecraft:tall_seagrass",
                                               "minecraft:kelp", "minecraft:kelp_plant",
                                               "minecraft:bubble_column"};

constexpr u8 kFluidTypeMask  = 0b11;
constexpr u8 kFluidBlockBit  = 1U << 2;
constexpr u8 kFluidLevelShift = 3;
constexpr u8 kImplicitBit    = 1U << 7;

}  // namespace

void BlockRegistry::build_fluid_table() {
    const usize states = state_count();
    fluid_table_.assign(states, 0);

    const auto set_block = [&](BlockId block, u8 value) {
        const u16 first = first_state(block).value();
        const u16 count = state_count(block);
        for (u16 offset = 0; offset < count && first + offset < states; ++offset) {
            fluid_table_[first + offset] = value;
        }
    };

    // The fluid blocks, at their own level. `level` is 0..15 as the blockstate
    // file spells it, which is also its index.
    for (const auto [name, type] : {std::pair{std::string_view{"minecraft:water"}, FluidType::Water},
                                    std::pair{std::string_view{"minecraft:lava"}, FluidType::Lava}}) {
        const auto block = find_block(name);
        if (!block) {
            continue;
        }
        const auto level = find_property(*block, "level");
        const u16  first = first_state(*block).value();
        for (u16 offset = 0; offset < state_count(*block); ++offset) {
            const BlockStateId state{static_cast<u16>(first + offset)};
            const u16 value = level ? property_index(state, *level) : 0;
            fluid_table_[state.value()] =
                static_cast<u8>(static_cast<u8>(type) | kFluidBlockBit |
                                ((value & 0xFU) << kFluidLevelShift));
        }
    }

    for (const std::string_view name : kImplicitWater) {
        if (const auto block = find_block(name)) {
            set_block(*block, static_cast<u8>(static_cast<u8>(FluidType::Water) | kImplicitBit));
        }
    }

    // waterlogged=true: a water source held by the block.
    for (usize i = 0; i < block_count(); ++i) {
        const BlockId block{static_cast<u16>(i)};
        const auto    property = find_property(block, "waterlogged");
        if (!property) {
            continue;
        }
        const u16 first = first_state(block).value();
        for (u16 offset = 0; offset < state_count(block); ++offset) {
            const BlockStateId state{static_cast<u16>(first + offset)};
            if (property_value(state, *property) == "true") {
                fluid_table_[state.value()] = static_cast<u8>(FluidType::Water);
            }
        }
    }
}

BlockRegistry::StateFluid BlockRegistry::fluid(BlockStateId state) const noexcept {
    if (state.value() >= fluid_table_.size()) {
        return {};
    }
    const u8 packed = fluid_table_[state.value()];
    return StateFluid{static_cast<FluidType>(packed & kFluidTypeMask),
                      static_cast<u8>((packed >> kFluidLevelShift) & 0xFU),
                      (packed & kFluidBlockBit) != 0};
}

bool BlockRegistry::is_implicitly_water(BlockId block) const noexcept {
    const BlockStateId first = first_state(block);
    return first.value() < fluid_table_.size() && (fluid_table_[first.value()] & kImplicitBit) != 0;
}

std::expected<BlockRegistry, RegistryError> BlockRegistry::load(const std::filesystem::path& path) {
    auto data = io::read_file(path);
    if (!data) {
        return std::unexpected{RegistryError::FileNotFound};
    }
    return from_bytes(std::move(*data));
}

namespace {

[[nodiscard]] const PackHeader& header_of(const void* base) noexcept {
    return *reinterpret_cast<const PackHeader*>(base);
}

}  // namespace

usize BlockRegistry::block_count() const noexcept {
    return header_of(header_).block_count;
}

usize BlockRegistry::state_count() const noexcept {
    return header_of(header_).state_count;
}

std::optional<BlockId> BlockRegistry::find_block(std::string_view name) const noexcept {
    // Linear over 1003 entries. A perfect hash belongs here eventually, but
    // this is called when loading a world or parsing a palette, not per block
    // per tick, and measuring before optimising is the rule.
    const auto it = std::ranges::find(strings_, name);
    if (it == strings_.end()) {
        return std::nullopt;
    }
    return BlockId{static_cast<u16>(std::distance(strings_.begin(), it))};
}

BlockRegistry::LightOpacity BlockRegistry::light_opacity(BlockId block) const noexcept {
    if (block.value() >= block_flags_.size()) {
        // Unknown block: opaque, which errs towards a dark room rather than a
        // world with no shadows.
        return LightOpacity::Opaque;
    }
    switch (block_flags_[block.value()] & 0b11) {
        case 0: return LightOpacity::Transparent;
        case 1: return LightOpacity::Attenuating;
        default: return LightOpacity::Opaque;
    }
}

namespace {

/// Bits 2 to 5 of a block's flag byte, above the two the light opacity uses.
constexpr u8 kMotionBit    = 0b000100;
constexpr u8 kLeavesBit    = 0b001000;
constexpr u8 kAirBit       = 0b010000;
constexpr u8 kMeasuredBit  = 0b100000;
constexpr u8 kNeedsToolBit = 0b1000000;

}  // namespace

bool BlockRegistry::blocks_motion(BlockId block) const noexcept {
    if (block.value() >= block_flags_.size()) {
        return false;
    }
    return (block_flags_[block.value()] & kMotionBit) != 0;
}

bool BlockRegistry::is_leaves(BlockId block) const noexcept {
    if (block.value() >= block_flags_.size()) {
        return false;
    }
    return (block_flags_[block.value()] & kLeavesBit) != 0;
}

bool BlockRegistry::is_air(BlockId block) const noexcept {
    if (block.value() >= block_flags_.size()) {
        return false;
    }
    return (block_flags_[block.value()] & kAirBit) != 0;
}

bool BlockRegistry::motion_measured(BlockId block) const noexcept {
    if (block.value() >= block_flags_.size()) {
        return false;
    }
    return (block_flags_[block.value()] & kMeasuredBit) != 0;
}

namespace {

/// The record for a state's shape, or nothing when the pack does not cover it.
[[nodiscard]] const ShapeRecord* shape_of(std::span<const u32> records,
                                          std::span<const u16> state_shapes,
                                          BlockStateId         state) noexcept {
    if (state.value() >= state_shapes.size()) {
        return nullptr;
    }
    const u16 index = state_shapes[state.value()];
    if (static_cast<usize>(index) * 2 >= records.size()) {
        return nullptr;
    }
    return reinterpret_cast<const ShapeRecord*>(records.data()) + index;
}

}  // namespace

std::span<const BlockRegistry::Box> BlockRegistry::collision_boxes(
    BlockStateId state) const noexcept {
    const ShapeRecord* record = shape_of(shape_records_, state_shapes_, state);
    if (record == nullptr || record->first + record->count > boxes_.size()) {
        return {};
    }
    return boxes_.subspan(record->first, record->count);
}

bool BlockRegistry::face_is_sturdy(BlockStateId state, Face face) const noexcept {
    const ShapeRecord* record = shape_of(shape_records_, state_shapes_, state);
    if (record == nullptr) {
        return false;
    }
    return (record->sturdy >> static_cast<u16>(face) & 1U) != 0;
}

u8 BlockRegistry::light_emission(BlockStateId state) const noexcept {
    return state.value() < emission_.size() ? emission_[state.value()] : u8{0};
}

std::optional<u32> BlockRegistry::find_biome(std::string_view name) const noexcept {
    const auto* records = static_cast<const BiomeRecord*>(biomes_);
    if (records == nullptr) {
        return std::nullopt;
    }
    // The emitter sorts by name, so this is a binary search rather than a hash
    // table nobody would ever rebuild.
    u32 low  = 0;
    u32 high = biome_count_;
    while (low < high) {
        const u32  middle = low + (high - low) / 2;
        const auto found  = string_at(records[middle].name_offset);
        if (found < name) {
            low = middle + 1;
        } else if (found > name) {
            high = middle;
        } else {
            return middle;
        }
    }
    return std::nullopt;
}

std::string_view BlockRegistry::biome_name(u32 index) const noexcept {
    const auto* records = static_cast<const BiomeRecord*>(biomes_);
    if (records == nullptr || index >= biome_count_) {
        return {};
    }
    return string_at(records[index].name_offset);
}

BiomeEffects BlockRegistry::biome(u32 index) const noexcept {
    const auto* records = static_cast<const BiomeRecord*>(biomes_);
    if (records == nullptr || index >= biome_count_) {
        // Plains-like rather than empty: an unknown biome should look ordinary
        // rather than paint the world black.
        return BiomeEffects{};
    }
    const BiomeRecord& record = records[index];
    BiomeEffects       effects;
    effects.temperature          = record.temperature;
    effects.downfall             = record.downfall;
    effects.water_colour         = record.water_colour;
    effects.water_fog_colour     = record.water_fog_colour;
    effects.fog_colour           = record.fog_colour;
    effects.sky_colour           = record.sky_colour;
    effects.grass_override       = record.grass_colour;
    effects.foliage_override     = record.foliage_colour;
    effects.grass_modifier       = record.grass_modifier;
    effects.temperature_modifier = record.temperature_modifier;
    effects.has_precipitation    = record.has_precipitation != 0;
    return effects;
}

f32 BlockRegistry::hardness(BlockId block) const noexcept {
    // Unknown block: unbreakable, which stops a stray id from being mined
    // through rather than making it free to break.
    return block.value() < hardness_.size() ? hardness_[block.value()] : -1.0F;
}

std::optional<BlockRegistry::BlockSounds> BlockRegistry::sounds(BlockId block) const noexcept {
    const usize count = sounds_.size() / sizeof(BlockSoundRecord);
    if (block.value() >= count) {
        return std::nullopt;
    }
    BlockSoundRecord record{};
    std::memcpy(&record, sounds_.data() + static_cast<usize>(block.value()) * sizeof(record),
                sizeof(record));
    if (record.measured == 0) {
        return std::nullopt;
    }
    BlockSounds out;
    for (usize i = 0; i < 7; ++i) {
        out.events[i] = record.events[i] == 0xFFFF ? -1 : static_cast<i32>(record.events[i]);
    }
    out.measured       = record.measured;
    out.volume         = record.volume;
    out.pitch          = record.pitch;
    out.open_audience  = static_cast<BlockSounds::Audience>(record.audience & 3U);
    out.close_audience = static_cast<BlockSounds::Audience>((record.audience >> 2U) & 3U);
    out.open_volume    = record.open_volume;
    out.open_pitch_lo  = record.open_pitch_lo;
    out.open_pitch_hi  = record.open_pitch_hi;
    out.close_volume   = record.close_volume;
    out.close_pitch_lo = record.close_pitch_lo;
    out.close_pitch_hi = record.close_pitch_hi;
    return out;
}

f32 BlockRegistry::blast_resistance(BlockId block) const noexcept {
    // -1 rather than 0 for an id the pack does not cover: a caller that treats
    // it as zero would make an unknown block the most fragile in the game, and
    // this project refuses a silent default. The explosion code checks for it.
    return block.value() < resistance_.size() ? resistance_[block.value()] : -1.0F;
}

bool BlockRegistry::requires_correct_tool(BlockId block) const noexcept {
    if (block.value() >= block_flags_.size()) {
        return false;
    }
    return (block_flags_[block.value()] & kNeedsToolBit) != 0;
}

bool BlockRegistry::holds_fluid(BlockStateId state) const noexcept {
    // ── implicit water ── the measured bit, or the fluid table: kelp_plant is
    // water and the heightmap oracle could never see it (see fluid()).
    if (state.value() < fluid_table_.size() && (fluid_table_[state.value()] & kFluidTypeMask) != 0) {
        return true;
    }
    const usize index = state.value() >> 3;
    if (index >= fluid_bits_.size()) {
        return false;
    }
    return (fluid_bits_[index] & (1U << (state.value() & 7))) != 0;
}

std::string_view BlockRegistry::string_at(u32 offset) const noexcept {
    if (offset >= strings_blob_.size()) {
        return {};
    }
    const auto text = strings_blob_.substr(offset);
    return text.substr(0, text.find('\0'));
}

std::string_view BlockRegistry::block_name(BlockId block) const noexcept {
    return block.value() < strings_.size() ? strings_[block.value()] : std::string_view{};
}

namespace {

[[nodiscard]] const BlockRecord* block_record(const void* base, const std::vector<u8>& data,
                                              BlockId block) noexcept {
    const PackHeader& header = header_of(base);
    if (block.value() >= header.block_count) {
        return nullptr;
    }
    const auto* blocks = reinterpret_cast<const BlockRecord*>(data.data() + header.blocks_offset);
    return blocks + block.value();
}

}  // namespace

BlockStateId BlockRegistry::default_state(BlockId block) const noexcept {
    const auto* record = block_record(header_, data_, block);
    return record ? BlockStateId{record->default_state} : kAirState;
}

BlockStateId BlockRegistry::first_state(BlockId block) const noexcept {
    const auto* record = block_record(header_, data_, block);
    return record ? BlockStateId{record->base_state} : kAirState;
}

u16 BlockRegistry::state_count(BlockId block) const noexcept {
    const auto* record = block_record(header_, data_, block);
    return record ? record->state_count : 0;
}

BlockId BlockRegistry::block_of(BlockStateId state) const noexcept {
    const PackHeader& header = header_of(header_);
    if (state.value() >= header.state_count) {
        return BlockId{0};
    }
    const auto* states = reinterpret_cast<const u16*>(data_.data() + header.states_offset);
    return BlockId{states[state.value()]};
}

std::span<const PropertyView> BlockRegistry::properties(BlockId block) const noexcept {
    const auto* record = block_record(header_, data_, block);
    if (record == nullptr) {
        return {};
    }
    return std::span<const PropertyView>{properties_.data() + record->property_first,
                                         record->property_count};
}

u16 BlockRegistry::property_index(BlockStateId state, const PropertyView& property) const noexcept {
    const BlockId block  = block_of(state);
    const auto*   record = block_record(header_, data_, block);
    if (record == nullptr || property.stride == 0) {
        return 0;
    }
    // Mixed-radix digit extraction: divide out everything less significant,
    // then take the remainder against this property's radix.
    const u16 offset = static_cast<u16>(state.value() - record->base_state);
    return static_cast<u16>((offset / property.stride) % property.values.size());
}

std::string_view BlockRegistry::property_value(BlockStateId        state,
                                               const PropertyView& property) const noexcept {
    const u16 index = property_index(state, property);
    return index < property.values.size() ? property.values[index] : std::string_view{};
}

BlockStateId BlockRegistry::with_property(BlockStateId state, const PropertyView& property,
                                          u16 value_index) const noexcept {
    if (value_index >= property.values.size()) {
        return state;
    }
    const u16 current = property_index(state, property);
    const i32 delta   = (static_cast<i32>(value_index) - static_cast<i32>(current)) *
                        static_cast<i32>(property.stride);
    return BlockStateId{static_cast<u16>(static_cast<i32>(state.value()) + delta)};
}

std::optional<PropertyView> BlockRegistry::find_property(BlockId          block,
                                                         std::string_view name) const noexcept {
    for (const PropertyView& property : properties(block)) {
        if (property.name == name) {
            return property;
        }
    }
    return std::nullopt;
}

std::optional<BlockStateId> BlockRegistry::state_for(
    BlockId                                                        block,
    std::span<const std::pair<std::string_view, std::string_view>> requested) const noexcept {
    const auto* record = block_record(header_, data_, block);
    if (record == nullptr) {
        return std::nullopt;
    }

    // Start from the block's first state — all digits zero — and set each
    // requested property. Unspecified properties keep their first value, which
    // is what an Anvil palette entry with a partial property set means.
    BlockStateId state{record->base_state};

    for (const auto& [name, value] : requested) {
        const auto property = find_property(block, name);
        if (!property) {
            return std::nullopt;
        }
        const auto it = std::ranges::find(property->values, value);
        if (it == property->values.end()) {
            return std::nullopt;
        }
        state = with_property(state, *property,
                              static_cast<u16>(std::distance(property->values.begin(), it)));
    }
    return state;
}

}  // namespace ov::registry
