// Mob behaviour, as the entity world sees it.
//
// ov_entity holds state and a `tick` it knows nothing about; what an entity
// actually *does* is a rule, and rules live here. The bridge is
// `TickContext::user`, an opaque pointer ov_entity cannot name — the same idiom
// CollisionWorld already uses for the world it reads. That is what lets layer 8
// tick behaviour written at layer 9 without either knowing about the other.
#pragma once

#include "ov/entity/logic.hpp"
#include "ov/entity/world.hpp"
#include "ov/gameplay/collision.hpp"
#include "ov/gameplay/entity_physics.hpp"

#include <string_view>

namespace ov::gameplay {

/// What mob behaviour is handed each tick, through TickContext::user.
///
/// The caller builds one on the stack per tick and points the context at it.
/// Nothing here is owned: the collision world is a view over blocks the tick
/// thread already holds.
struct MobContext {
    const CollisionWorld* world{nullptr};
};

/// Recover the context, or null if the caller did not provide one.
///
/// A free function rather than a cast at every call site, so that the one
/// unchecked conversion in the design has exactly one home.
[[nodiscard]] inline const MobContext* mob_context(const entity::TickContext& context) noexcept {
    return static_cast<const MobContext*>(context.user);
}

/// The behaviour every entity has before it has any other: it falls.
///
/// Deliberately the whole of it. Vanilla's Mob is a tower of goals over a
/// LivingEntity that already knows how to fall, and this is that floor — a
/// zombie that chases a player will do it by adding horizontal velocity on top
/// of this, not by replacing it.
class FallingMob final : public entity::IEntityLogic {
public:
    explicit FallingMob(EntityMotionConstants constants = {}) noexcept
        : constants_{constants} {}

    void tick(entity::EntityWorld& world, entity::EntityHandle self,
              const entity::TickContext& context) override;

    [[nodiscard]] std::string_view name() const noexcept override { return "falling"; }

private:
    EntityMotionConstants constants_;
};

}  // namespace ov::gameplay
