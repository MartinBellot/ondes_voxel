// The numeric ids the vanilla client hard-codes and never receives.
//
// This is the project's least forgiving constraint. In 1.20.1 the client knows
// the id of every entity type, item, sound, particle, block entity, effect and
// menu before it connects; the server never sends them. Ours must therefore be
// Mojang's, in Mojang's order, or the client connects and then behaves as if
// every zombie were a skeleton — with no error anywhere.
//
// So none of this is authored. It is read from the official server jar's own
// reports through tools/ov_datagen, and a test compares all 66 registries back
// against those reports entry by entry, order included.
//
// Six registries are *not* here, because they are the ones the server does
// send, as NBT during login: dimension_type, worldgen/biome, chat_type,
// damage_type, trim_material and trim_pattern. Those ids are ours to choose.
#pragma once

#include "ov/base/types.hpp"
#include "ov/registry/block_states.hpp"

#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ov::registry {

/// A registry's index within the pack. Not a protocol value — it identifies
/// the registry itself, not anything in it.
struct RegistryIdTag {};

using RegistryId = Id<RegistryIdTag, u16>;

/// The id an entry carries on the wire.
using ProtocolId = i32;

/// Every hard-coded registry, keyed by name.
///
/// Loading is a file read and a handful of bounds checks; the pack is laid out
/// so that nothing has to be parsed.
class Registries {
public:
    [[nodiscard]] static std::expected<Registries, RegistryError> load(
        const std::filesystem::path& path);

    /// Load from bytes already in memory, for tests.
    [[nodiscard]] static std::expected<Registries, RegistryError> from_bytes(std::vector<u8> data);

    [[nodiscard]] usize count() const noexcept { return registries_.size(); }

    /// Look up a registry by name, e.g. "minecraft:entity_type".
    [[nodiscard]] std::optional<RegistryId> find(std::string_view name) const noexcept;

    [[nodiscard]] std::string_view name(RegistryId registry) const noexcept;

    /// How many entries the registry holds.
    [[nodiscard]] usize size(RegistryId registry) const noexcept;

    /// The id the registry's first entry carries.
    ///
    /// Zero everywhere except minecraft:mob_effect, which is 1-based. That one
    /// exception is worth a field rather than a special case: an off-by-one in
    /// effect ids means every potion in the game applies the wrong effect.
    [[nodiscard]] ProtocolId first_id(RegistryId registry) const noexcept;

    /// The entries, in the order Mojang lists them — which is the order that
    /// defines their ids.
    [[nodiscard]] std::span<const std::string_view> entries(RegistryId registry) const noexcept;

    /// The wire id of a named entry, e.g. ("minecraft:entity_type", "minecraft:zombie").
    [[nodiscard]] std::optional<ProtocolId> protocol_id(RegistryId       registry,
                                                        std::string_view entry) const noexcept;

    /// The entry a wire id names. Empty when the id is out of range, which is
    /// the case for anything arriving from a peer.
    [[nodiscard]] std::string_view entry_of(RegistryId registry, ProtocolId id) const noexcept;

private:
    Registries() = default;

    struct Entry {
        std::string_view name;
        u32              entry_first;
        u32              entry_count;
        ProtocolId       first_id;
    };

    std::vector<u8>               data_;
    std::vector<Entry>            registries_;
    std::vector<std::string_view> entry_names_;
};

}  // namespace ov::registry
