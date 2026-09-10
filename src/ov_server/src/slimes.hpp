// ── mobs-2 ── Slimes: a size per slime, and the division on death.
//
// The rules are ov_gameplay's (slime.hpp); this file keeps the one number a
// slime carries that `EntityState` has no field for — its size — keyed by wire
// id, the way MobCombat keeps its damage windows, and answers the server's
// three questions: what does a new slime look like, what does its metadata
// say, and what does it leave when it dies.
#pragma once

#include "ov/base/types.hpp"
#include "ov/entity/entity.hpp"
#include "ov/gameplay/slime.hpp"
#include "ov/math/random.hpp"
#include "ov/protocol/entity.hpp"
#include "ov/registry/registries.hpp"

#include <unordered_map>
#include <vector>

namespace ov::server {

/// Slime, an int: its size (1, 2, 4). Index 16 on a slime — the first field
/// after Mob's flags. Not measured on the wire by this wave; named.
inline constexpr u8 kSlimeSize = 16;

class Slimes {
public:
    explicit Slimes(const registry::Registries& registries);

    [[nodiscard]] bool owns(i32 type) const noexcept { return type == slime_type_ && type >= 0; }

    /// Give a slime its size: box, health. Remembered by wire id.
    void set_size(entity::EntityState& state, i32 size);

    /// A natural or summoned slime: draw the size, then `set_size`.
    void on_spawn(entity::EntityState& state, math::LegacyRandomSource& random,
                  f32 special_multiplier);

    [[nodiscard]] i32 size_of(i32 network_id) const noexcept;

    /// The size field, for the spawn packets. Nothing for a non-slime.
    void spawn_metadata(const entity::EntityState& state, net::MetadataWriter& fields) const;

    /// The slimes a dead one leaves, as absolute positions and sizes. Also
    /// forgets the dead one.
    void on_death(const entity::EntityState& state, math::LegacyRandomSource& random,
                  std::vector<gameplay::SlimeChild>& out);

private:
    i32                          slime_type_{-1};
    std::unordered_map<i32, i32> sizes_;
};

}  // namespace ov::server
