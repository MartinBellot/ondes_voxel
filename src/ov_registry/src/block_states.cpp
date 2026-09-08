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

    if (blocks == nullptr || props == nullptr || values == nullptr || states == nullptr ||
        flags == nullptr || fluids == nullptr || hardness == nullptr) {
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
    return registry;
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

f32 BlockRegistry::hardness(BlockId block) const noexcept {
    // Unknown block: unbreakable, which stops a stray id from being mined
    // through rather than making it free to break.
    return block.value() < hardness_.size() ? hardness_[block.value()] : -1.0F;
}

bool BlockRegistry::requires_correct_tool(BlockId block) const noexcept {
    if (block.value() >= block_flags_.size()) {
        return false;
    }
    return (block_flags_[block.value()] & kNeedsToolBit) != 0;
}

bool BlockRegistry::holds_fluid(BlockStateId state) const noexcept {
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
