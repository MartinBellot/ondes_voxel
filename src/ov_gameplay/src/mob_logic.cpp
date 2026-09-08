#include "ov/gameplay/mob_logic.hpp"

namespace ov::gameplay {

void FallingMob::tick(entity::EntityWorld& world, entity::EntityHandle self,
                      const entity::TickContext& context) {
    const MobContext* mob = mob_context(context);
    if (mob == nullptr || mob->world == nullptr) {
        // No world to fall through. Returning rather than falling anyway: an
        // entity that moves without collision walks out of the ground, and a
        // caller that forgot the context should see a mob that does not move
        // rather than one that sinks.
        return;
    }
    entity::EntityState* state = world.mutable_state(self);
    if (state == nullptr) {
        return;
    }
    *state = step_entity(*state, constants_, *mob->world);
}

}  // namespace ov::gameplay
