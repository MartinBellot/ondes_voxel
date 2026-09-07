// Block states, and the arithmetic that makes them cheap.
//
// 1.20.1 has 1003 blocks and 24135 block states, numbered 0 to 24134 with no
// gaps. Those numbers are Mojang's: the vanilla client hard-codes them and they
// are never sent over the wire, so ours have to be identical or a connected
// client sees the wrong blocks. See docs/ARCHITECTURE.md § 3.3.
//
// The property layout is what makes this fast. A block's states are contiguous,
// and within that range they are a mixed-radix number over the block's
// properties, most significant first. So changing one property is
//
//     state + (new_index - old_index) * stride
//
// — one multiply and one add, no hashing, no allocation. Redstone, doors,
// fences, stairs and fluids all do this constantly; with a hash lookup instead,
// redstone is unplayable.
//
// The trap, and the reason the data is derived rather than read: the property
// order printed in blocks.json is NOT the order that arithmetic uses. Four
// blocks disagree. tools/ov_datagen recovers the real order from the state ids
// themselves and every one of the 24135 states is checked against it.
#pragma once

#include "ov/base/types.hpp"

#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ov::registry {

/// A block state id, as it appears in a chunk palette and on the wire.
///
/// u16 is deliberate: 24135 states fit with room, and a chunk section stores
/// 4096 of them. Id 0 is minecraft:air, so a zeroed section is an empty one.
struct BlockStateTag {};

using BlockStateId = Id<BlockStateTag, u16>;

/// An index into the block registry, not a state.
struct BlockTag {};

using BlockId = Id<BlockTag, u16>;

inline constexpr BlockStateId kAirState{0};

enum class RegistryError {
    FileNotFound,
    /// Not an .ovpack, or truncated.
    Corrupt,
    /// Built by a different version of the emitter.
    VersionMismatch,
};

[[nodiscard]] std::string_view to_string(RegistryError error) noexcept;

/// One property of a block: its name, its possible values, and the stride that
/// moving between them costs in state ids.
struct PropertyView {
    std::string_view name;
    /// Values in declaration order; the index into this is the digit.
    std::span<const std::string_view> values;
    /// How much one step in this property moves the state id.
    u16 stride{0};
};

/// The block registry, loaded from the binary cache.
///
/// Reads a file produced by tools/ov_datagen, which derives it from the
/// official server jar's own reports. Nothing here is hand-maintained: a list
/// of 24135 ids kept by hand would be wrong within a version.
class BlockRegistry {
public:
    [[nodiscard]] static std::expected<BlockRegistry, RegistryError> load(
        const std::filesystem::path& path);

    /// Load from bytes already in memory, for tests.
    [[nodiscard]] static std::expected<BlockRegistry, RegistryError> from_bytes(
        std::vector<u8> data);

    [[nodiscard]] usize block_count() const noexcept;
    [[nodiscard]] usize state_count() const noexcept;

    /// Look up a block by its resource location, e.g. "minecraft:oak_door".
    [[nodiscard]] std::optional<BlockId> find_block(std::string_view name) const noexcept;

    [[nodiscard]] std::string_view block_name(BlockId block) const noexcept;

    /// The state a freshly placed block takes.
    ///
    /// Not always the block's first state: 484 of the 1003 differ, so this is
    /// stored rather than computed.
    [[nodiscard]] BlockStateId default_state(BlockId block) const noexcept;

    /// The state range a block occupies. Contiguous, which is what the
    /// arithmetic depends on.
    [[nodiscard]] BlockStateId first_state(BlockId block) const noexcept;
    [[nodiscard]] u16          state_count(BlockId block) const noexcept;

    /// Which block a state belongs to. O(1): a reverse table is stored.
    [[nodiscard]] BlockId block_of(BlockStateId state) const noexcept;

    [[nodiscard]] std::span<const PropertyView> properties(BlockId block) const noexcept;

    /// The value a state has for one of its block's properties, as an index
    /// into that property's values.
    [[nodiscard]] u16 property_index(BlockStateId        state,
                                     const PropertyView& property) const noexcept;

    /// The value as text, e.g. "north" or "true".
    [[nodiscard]] std::string_view property_value(BlockStateId        state,
                                                  const PropertyView& property) const noexcept;

    /// The state that differs from this one only in the given property.
    ///
    /// One multiply and one add. Returns the input unchanged when the index is
    /// out of range, since a caller asking for a value a property does not have
    /// is a bug rather than a runtime condition.
    [[nodiscard]] BlockStateId with_property(BlockStateId state, const PropertyView& property,
                                             u16 value_index) const noexcept;

    /// Look up a property of a block by name.
    [[nodiscard]] std::optional<PropertyView> find_property(BlockId          block,
                                                            std::string_view name) const noexcept;

    /// The state matching a block and a set of property values, for parsing
    /// blockstate files and Anvil palettes.
    [[nodiscard]] std::optional<BlockStateId> state_for(
        BlockId                                                        block,
        std::span<const std::pair<std::string_view, std::string_view>> properties) const noexcept;

    [[nodiscard]] bool is_valid(BlockStateId state) const noexcept {
        return state.value() < state_count();
    }

private:
    BlockRegistry() = default;

    struct Impl;
    std::vector<u8> data_;

    // Views into data_, built once at load. Rebuilding them per query would
    // undo the point of the format.
    std::vector<std::string_view> strings_;
    std::vector<PropertyView>     properties_;
    std::vector<std::string_view> values_;
    const void*                   header_{nullptr};
};

}  // namespace ov::registry
