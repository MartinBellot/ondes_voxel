// ── mobs-2 ── Zombies and husks that drown. See the header.
#include "drowning.hpp"

#include <algorithm>
#include <cmath>

namespace ov::server {

Drowning::Drowning(const registry::Registries& registries) {
    if (const auto types = registries.find("minecraft:entity_type")) {
        zombie_ = registries.protocol_id(*types, "minecraft:zombie").value_or(-1);
        husk_   = registries.protocol_id(*types, "minecraft:husk").value_or(-1);
    }
    seen_.reserve(64);
}

void Drowning::tick(const entity::EntityWorld& world, WaterAt water, const void* context,
                    std::vector<Conversion>& out) {
    seen_.clear();
    for (const entity::EntityHandle handle : world.handles()) {
        const entity::EntityState* state = world.state(handle);
        if (state == nullptr || state->removed || state->health <= 0.0F ||
            (state->type != zombie_ && state->type != husk_) || state->type < 0) {
            continue;
        }
        seen_.push_back(state->network_id);
        const BlockPos eyes{
            static_cast<i32>(std::floor(state->position.x)),
            static_cast<i32>(std::floor(state->position.y + static_cast<f64>(state->eye_height))),
            static_cast<i32>(std::floor(state->position.z))};
        gameplay::DrowningState& drowning = states_[state->network_id];
        if (gameplay::drowning_tick(drowning, water(context, eyes)) ==
            gameplay::DrowningEvent::Converted) {
            out.push_back(Conversion{handle, state->type == husk_
                                                 ? std::string_view{"minecraft:zombie"}
                                                 : std::string_view{"minecraft:drowned"}});
        }
    }
    // Forget the mobs that are gone, so the table does not grow forever.
    std::erase_if(states_, [&](const auto& entry) {
        return std::find(seen_.begin(), seen_.end(), entry.first) == seen_.end();
    });
}

}  // namespace ov::server
