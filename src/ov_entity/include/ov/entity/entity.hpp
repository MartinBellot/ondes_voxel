// What an entity is, before anything decides how it behaves.
//
// Minecraft's entities are deeply object-oriented — LivingEntity, Mob,
// PathfinderMob — there are about a hundred types and only a few thousand
// instances at a time. A pure ECS buys almost nothing at that scale and costs a
// great deal in friction, so the split here is the one docs/ARCHITECTURE.md § 5
// settles on: EnTT owns handles, storage and relations; behaviour stays
// polymorphic behind a single component. Decomposing a zombie's AI into forty
// components costs a year and returns nothing.
//
// This header therefore describes *state*. It contains no EnTT, which is a
// PRIVATE_DEP of this module and never reaches a public header — a registry
// template in one would put its instantiation weight into every translation
// unit that so much as spawns a chicken (risk R5).
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/vec.hpp"
#include "ov/protocol/types.hpp"

namespace ov::entity {

/// A reference to one entity, valid only while that entity lives.
///
/// The representation is an index and a generation packed together, so a handle
/// to a removed entity never resolves — and, more importantly, never resolves
/// to whichever entity took its slot. A bare index would do the second thing
/// silently, and the symptom would be damage landing on the wrong mob.
struct EntityHandleTag {};

using EntityHandle = Id<EntityHandleTag, u32>;

/// The handle that refers to nothing.
///
/// Not `EntityHandle{0}`: zero is a perfectly good index, and the first entity
/// spawned would then be indistinguishable from none at all.
inline constexpr EntityHandle kNoEntity{0xFFFFFFFFU};

/// Everything about an entity that both the world and the wire need.
///
/// One struct rather than a component per field. The fields below are read and
/// written together on every tick of every entity — position needs velocity
/// needs the box needs on_ground — so splitting them would buy cache misses and
/// sell nothing.
struct EntityState {
    /// The id this entity carries on the wire. Ours to choose, unlike the type
    /// id: the protocol only requires that it be unique among live entities.
    i32 network_id{0};

    /// The type's id in minecraft:entity_type. **Mojang's number**, never ours:
    /// the vanilla client hard-codes the whole registry and is never sent it, so
    /// a difference of one spawns a skeleton where a zombie should be, with no
    /// error anywhere.
    i32 type{0};

    net::Uuid uuid{};

    Vec3d position{};
    Vec3d velocity{};

    /// Degrees. `head_yaw` is separate because a mob's head turns independently
    /// of its body and the protocol sends it in a packet of its own.
    f32 yaw{0.0F};
    f32 pitch{0.0F};
    f32 head_yaw{0.0F};

    bool on_ground{false};

    /// The bounding box, from the measured table: full width on both horizontal
    /// axes, height upward from the feet. Copied into the entity at spawn
    /// rather than looked up per tick, because a few types change size while
    /// they live — a slime's box is a multiple of its size, and a baby's is
    /// half its adult's.
    f32 width{0.0F};
    f32 height{0.0F};
    f32 eye_height{0.0F};

    f32 health{0.0F};
    f32 max_health{0.0F};

    /// Set by logic that wants this entity gone. Acted on at the end of the
    /// tick, never during it: removing an entity while the tick is iterating
    /// them is how a list gets modified underneath its own loop.
    bool removed{false};
};

/// The half-width of the box, which is what every collision test actually
/// wants. Named so the /2 appears once.
[[nodiscard]] constexpr f64 half_width(const EntityState& state) noexcept {
    return static_cast<f64>(state.width) * 0.5;
}

}  // namespace ov::entity
