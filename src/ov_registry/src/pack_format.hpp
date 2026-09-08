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

#include <vector>

namespace ov::registry {

/// Bumped by hand whenever the layout changes. A stale cache read as if it
/// were current is far worse than no cache: the ids would be plausible and
/// wrong, and nothing would report an error until a vanilla client crashed on
/// an entity type that does not exist.
inline constexpr u32 kFormatVersion = 6;

inline constexpr u32 kHeaderSize = 128;

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
    u32 reserved;
};

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
