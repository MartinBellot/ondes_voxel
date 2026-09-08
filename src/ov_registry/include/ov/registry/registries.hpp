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

/// A tag's index within the pack.
struct TagIdTag {};

using TagId = Id<TagIdTag, u16>;

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

    /// How many of an item fit in one slot.
    ///
    /// Not in Mojang's reports either — it is Java code — so this was measured
    /// by giving a real 1.20.1 server 65 of every item and reading back how it
    /// split them. 1032 items stack to 64, 45 to 16, and 177 to a single one.
    ///
    /// Defaults to 64 for an id the pack does not cover, which is the majority
    /// answer and the safe direction: over-stacking a tool is visible, while
    /// under-stacking blocks would make every chest behave oddly.
    [[nodiscard]] i8 max_stack_size(ProtocolId item) const noexcept;

    // ── Tags ────────────────────────────────────────────────────────────────
    //
    // Tags are how the game asks "is this one of the logs?" without listing
    // forty blocks. They form a graph — a tag may include another with a '#'
    // prefix — and that graph is flattened once, at build time, into sorted id
    // lists. Resolving at every lookup would be the obvious implementation and
    // would put a graph walk inside the block-breaking path.
    //
    // Tags for the six dynamic registries are deliberately absent: their ids
    // only exist once the server has built them, so they cannot be numbers here.

    [[nodiscard]] usize tag_count() const noexcept { return tags_.size(); }

    /// Find a tag by registry and name, e.g. ("minecraft:block", "minecraft:logs").
    [[nodiscard]] std::optional<TagId> find_tag(RegistryId       registry,
                                                std::string_view name) const noexcept;

    [[nodiscard]] std::string_view tag_name(TagId tag) const noexcept;

    /// The tag's members, as wire ids, sorted ascending.
    [[nodiscard]] std::span<const ProtocolId> tag_members(TagId tag) const noexcept;

    /// Whether an id is in the tag. A binary search over a sorted span — this
    /// is the call the game actually makes, millions of times.
    [[nodiscard]] bool tag_contains(TagId tag, ProtocolId id) const noexcept;

private:
    Registries() = default;

    struct Entry {
        std::string_view name;
        u32              entry_first;
        u32              entry_count;
        ProtocolId       first_id;
    };

    struct Tag {
        std::string_view name;
        u16              registry_index;
        u32              member_first;
        u32              member_count;
    };

    std::vector<u8>               data_;
    std::vector<Entry>            registries_;
    std::vector<std::string_view> entry_names_;
    std::vector<Tag>              tags_;
    std::span<const u8>           stack_sizes_;
    std::span<const ProtocolId>   members_;
};

}  // namespace ov::registry
