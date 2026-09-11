// ── persistence ── The pieces of the 1.20.1 entity format every saved entity
// shares, for the modules that write their own entities into entities/ through
// the entity storage (entity_storage.hpp): the base keys, a UUID, an item
// stack, a block state.
//
// The keys and their types are the ones the real 1.20.1 server writes, read
// with `data get entity` and in the region files it saved
// (scripts/measure_persistence.py, docs/provenance/persistance-entites.md):
// `Pos`/`Motion` lists of doubles, `Rotation` two floats, `UUID` four ints,
// `OnGround`/`Invulnerable` bytes, `Air`/`Fire` shorts, `FallDistance` a
// float, `PortalCooldown` an int. `Fire` is -1 for an entity that does not
// burn over time (an item, an orb, a TNT) and 0 for one that does (an arrow,
// a falling block, a crystal, a cloud): both measured.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/vec.hpp"
#include "ov/nbt/tag.hpp"
#include "ov/protocol/play.hpp"
#include "ov/protocol/types.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"

#include <optional>
#include <string_view>

namespace ov::server {

/// `id`, `Pos`, `Motion`, `Rotation`, `UUID`, `OnGround` from the entity's
/// own state, then the base keys vanilla writes — `FallDistance`, `Fire`,
/// `Air`, `Invulnerable`, `PortalCooldown` — only where `out` does not already
/// carry them (a compound read from disk keeps its own).
void put_entity_base(nbt::Tag& out, std::string_view id, Vec3d position, Vec3d motion, f32 yaw,
                     f32 pitch, const net::Uuid& uuid, bool on_ground, i16 fire);

/// Put `value` under `name` only when the compound has no such key.
void default_to(nbt::Tag& compound, std::string_view name, nbt::Tag value);

[[nodiscard]] nbt::Tag uuid_tag(const net::Uuid& uuid);
/// Nullopt for anything but four ints.
[[nodiscard]] std::optional<net::Uuid> uuid_from(const nbt::Tag* tag);

/// `Pos` (or any list of doubles), element `index`; `fallback` when absent.
[[nodiscard]] f64 list_f64(const nbt::Tag& compound, std::string_view name, usize index,
                           f64 fallback = 0.0);
[[nodiscard]] Vec3d list_vec3(const nbt::Tag& compound, std::string_view name);

/// A short, int, byte… under `name`, or `fallback`.
[[nodiscard]] i64 get_i64(const nbt::Tag& compound, std::string_view name, i64 fallback);
[[nodiscard]] f64 get_f64(const nbt::Tag& compound, std::string_view name, f64 fallback);
[[nodiscard]] bool get_bool(const nbt::Tag& compound, std::string_view name, bool fallback);

/// `{id, Count, tag}` as an item entity, an arrow's trident or a thrown
/// potion carries it. An empty stack or an unknown id gives nullopt.
[[nodiscard]] std::optional<nbt::Tag> item_stack_tag(const registry::Registries& registries,
                                                     registry::RegistryId        items,
                                                     const net::ItemStack&       stack);
/// The stack a `{id, Count, tag}` compound names. Nullopt for an item this
/// registry does not have (named in the log by the caller) or a count of 0.
[[nodiscard]] std::optional<net::ItemStack> item_stack_from(const registry::Registries& registries,
                                                            registry::RegistryId        items,
                                                            const nbt::Tag*             compound);

/// `{Name, Properties}` as a falling block's `BlockState` and an arrow's
/// `inBlockState` hold it.
[[nodiscard]] nbt::Tag block_state_tag(const registry::BlockRegistry& blocks,
                                       registry::BlockStateId          state);
/// Nullopt for a block this registry does not know. Properties the compound
/// leaves out keep the block's default, one at a time (piège 8).
[[nodiscard]] std::optional<registry::BlockStateId> block_state_from(
    const registry::BlockRegistry& blocks, const nbt::Tag* compound);

}  // namespace ov::server
