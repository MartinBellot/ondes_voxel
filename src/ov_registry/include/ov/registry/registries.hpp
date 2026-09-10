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
#include "ov/registry/recipe_data.hpp"

#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
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

    // ── Entity types ────────────────────────────────────────────────────────
    //
    // A hitbox, an eye height and a set of attribute base values. None of the
    // three is in any Mojang report — they are Java code — so all of them were
    // measured on a running 1.20.1 server by scripts/measure_entities.py, which
    // asks the game rather than a formula. See docs/PROVENANCE.md.

    /// How big an entity of this type is, and where it looks from.
    ///
    /// `width` is the full width: the box runs from x - width/2 to x + width/2
    /// on both horizontal axes, and from y to y + height vertically. Vanilla's
    /// boxes are square in plan — there is no separate depth — which is why one
    /// number covers both.
    struct EntityTypeInfo {
        f32 width{0.0F};
        f32 height{0.0F};
        f32 eye_height{0.0F};
        /// False when the eye height could not be measured. Three types are in
        /// that state: a marker has no body to look out of, a painting is moved
        /// by the game when it is placed, and an eye of ender drifts away.
        bool eye_measured{false};
    };

    /// The measured size of an entity type, or nullopt when it was never
    /// measured.
    ///
    /// Four of the 124 types answer nullopt, and they are named rather than
    /// rounded to zero: the lightning bolt exists for a single tick, evoker
    /// fangs for about twenty, a fishing bobber cannot exist without an angler,
    /// and a player cannot be summoned at all. A zero-sized box would make each
    /// of them something nothing can ever hit, and nothing would say so.
    [[nodiscard]] std::optional<EntityTypeInfo> entity_type(ProtocolId type) const noexcept;

    /// What a creature sounds like, measured on a real 1.20.1 server: hurt
    /// three times by /damage and then killed, and every Sound Effect a second
    /// player heard written down. See docs/provenance/son.md.
    struct EntitySounds {
        /// Indices into minecraft:sound_event, -1 for none.
        i32 hurt{-1};
        i32 death{-1};
        /// Named by the registry (`entity.<type>.ambient`), not heard: an
        /// ambient sound comes when it likes, and three hurts and a death do
        /// not wait for one.
        i32 ambient{-1};
        /// Mojang's sound category, as the packet carries it; -1 unmeasured.
        i32 category{-1};
        f32 volume{-1.0F};
        /// The pitch range seen over the samples — a sample, not a bound.
        f32 pitch_lo{-1.0F};
        f32 pitch_hi{-1.0F};
        /// Bit 0 hurt heard, bit 1 death heard, bit 2 ambient named.
        u8 measured{0};
    };

    /// Empty for a type nothing about was measured.
    [[nodiscard]] std::optional<EntitySounds> entity_sounds(ProtocolId type) const noexcept;

    /// One attribute a type owns, as an index into minecraft:attribute.
    struct EntityAttribute {
        ProtocolId attribute{0};
        f64        base{0.0};
    };

    /// Every attribute this type owns, in the registry's order.
    ///
    /// Empty for a type that has none — an arrow, a boat, a painting — which is
    /// different from a type whose attributes are all zero.
    [[nodiscard]] std::vector<EntityAttribute> entity_attributes(ProtocolId type) const;

    /// The base value of one attribute on one type, or nullopt when the type
    /// does not have it.
    ///
    /// Nullopt and not zero: a cow has no attack damage, and answering 0.0
    /// would make "does not attack" indistinguishable from "hits for nothing".
    [[nodiscard]] std::optional<f64> attribute_base(ProtocolId type,
                                                    ProtocolId attribute) const noexcept;

    // ── Recipes ─────────────────────────────────────────────────────────────

    /// The compiled recipes, as plain arrays rather than as a matcher.
    ///
    /// Same split as the loot tables: reading them is gameplay, and gameplay
    /// lives above this module. What is here is the file and the bounds checks.
    [[nodiscard]] RecipeData recipes() const noexcept { return recipes_; }

    /// The recipe's identifier, e.g. "minecraft:wooden_pickaxe".
    [[nodiscard]] std::string_view recipe_name(usize index) const noexcept;

    /// The recipe's group, or empty. Groups are what makes the client's recipe
    /// book show one entry for six colours of bed.
    [[nodiscard]] std::string_view recipe_group(usize index) const noexcept;

    /// The recipe's type as it travels on the wire, e.g.
    /// "minecraft:crafting_shaped".
    [[nodiscard]] std::string_view recipe_type(usize index) const noexcept;

    /// How long one unit of this item keeps a furnace of that kind alight, in
    /// ticks. Zero means it is not a fuel — measured, not assumed: every item
    /// in the registry was put in a furnace and watched.
    [[nodiscard]] u16 burn_ticks(ProtocolId item, FuelKind kind) const noexcept;

    /// What a recipe leaves in the slot when it consumes this item — the empty
    /// bucket, the glass bottle — or nullopt when it leaves nothing.
    ///
    /// Nullopt rather than the item itself: "nothing comes back" and "the same
    /// item comes back" are different, and only one of them empties the slot.
    [[nodiscard]] std::optional<ProtocolId> crafting_remainder(
        ProtocolId item) const noexcept;

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

    /// One entry per entity type, mirroring the pack record. Kept in a plain
    /// struct rather than as a span over the file so that the public header
    /// need not describe the on-disk layout.
    struct EntityRecord {
        f32 width;
        f32 height;
        f32 eye_height;
        u32 attribute_first;
        u8  attribute_count;
        u8  measured;
    };

    std::vector<u8>                data_;
    std::vector<Entry>             registries_;
    std::vector<std::string_view>  entry_names_;

    // ── name → id, because this stopped being a load-time path ──────────────
    //
    // `protocol_id` was a linear scan, and its own comment said why that was
    // fine: "resolving a datapack, not a hot loop". `NaturalSpawner` made it a
    // hot loop. Profiled on the lab world with one player connected
    // (`scripts/bench_tick.py`, macos-release, `sample` over 20 s): **19 of the
    // 50 samples inside the whole spawn pass were `memcmp` under this one
    // call**, because the spawner resolves a mob type by name once per spawn
    // attempt and there are a couple of thousand attempts per tick.
    //
    // One map per registry rather than one keyed by (registry, name): entry
    // names repeat across registries — "minecraft:stone" is both a block and an
    // item — and a single map would need a composite key for no gain.
    // Built at load, from the same views as `entry_names_`, so it lives exactly
    // as long as `data_` does and carries no strings of its own.
    std::vector<std::unordered_map<std::string_view, ProtocolId>> entry_index_;

    std::vector<Tag>               tags_;
    std::span<const u8>            stack_sizes_;
    std::span<const ProtocolId>    members_;
    std::vector<EntityRecord>      entity_types_;
    std::vector<EntityAttribute>   entity_attributes_;
    std::vector<EntitySounds>      entity_sounds_;
    RecipeData                     recipes_;
    /// The pack's string blob, for the three recipe name accessors. Held as
    /// offset and length rather than as resolved views: 1174 recipes carry
    /// three names each, and resolving them all at load would cost more than
    /// the lookups ever will.
    u32 strings_offset_{0};
    u32 strings_bytes_{0};
};

}  // namespace ov::registry
