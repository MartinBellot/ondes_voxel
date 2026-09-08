// How an entity behaves: one virtual call, not forty components.
//
// See docs/ARCHITECTURE.md § 5. The reason behaviour is polymorphic rather than
// data-oriented is that Minecraft's own model is: a zombie is a Monster is a
// PathfinderMob is a Mob is a LivingEntity, and every level of that adds
// behaviour rather than data. Expressing it as component churn means an
// archetype change on every state transition, for a few thousand instances that
// were never the bottleneck.
#pragma once

#include "ov/base/types.hpp"
#include "ov/entity/entity.hpp"

#include <string_view>

namespace ov::entity {

class EntityWorld;

/// What a tick knows about itself.
struct TickContext {
    /// The world's age, in ticks. The only clock logic may read: a wall clock
    /// here would make the simulation depend on how fast the machine is, and
    /// determinism (CLAUDE.md principle 5) would be gone.
    i64 tick{0};

    /// Whatever the caller's layer needs its own logic to reach — a level, a
    /// collision world, a server.
    ///
    /// Opaque on purpose. ov_entity is layer 8 and ov_gameplay is layer 9, so
    /// this module cannot name a level even if it wanted to; mob behaviour that
    /// needs blocks is written up there and recovers its own world from here.
    /// It is the same idiom gameplay::CollisionWorld already uses, which takes
    /// a lookup function and a context pointer for exactly this reason.
    void* user{nullptr};
};

/// One entity's behaviour.
///
/// Held in a `std::unique_ptr` component, so an entity with no behaviour — a
/// dropped item, a painting — costs one null pointer and no virtual call.
class IEntityLogic {
public:
    IEntityLogic()                               = default;
    IEntityLogic(const IEntityLogic&)            = delete;
    IEntityLogic& operator=(const IEntityLogic&) = delete;
    IEntityLogic(IEntityLogic&&)                 = delete;
    IEntityLogic& operator=(IEntityLogic&&)      = delete;
    virtual ~IEntityLogic()                      = default;

    /// One tick. `self` is guaranteed alive for the duration of the call.
    ///
    /// Spawning and removing during a tick is allowed: both are deferred to the
    /// end of it by the world, so the set being iterated never changes under
    /// the loop.
    virtual void tick(EntityWorld& world, EntityHandle self, const TickContext& context) = 0;

    /// For logs and tests. Not an identity — two zombies share a name.
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
};

}  // namespace ov::entity
