// ── mobs-2 ── Slimes: size and division, over the server's entities.
#include "slimes.hpp"

namespace ov::server {

Slimes::Slimes(const registry::Registries& registries, std::string_view type) {
    if (const auto types = registries.find("minecraft:entity_type")) {
        slime_type_ = registries.protocol_id(*types, type).value_or(-1);
    }
}

void Slimes::set_size(entity::EntityState& state, i32 size) {
    const i32 clamped        = size < 1 ? 1 : size;
    sizes_[state.network_id] = clamped;
    const f32 side           = gameplay::slime_side(clamped);
    state.width              = side;
    state.height             = side;
    // Eye height scales with the box: 0.325125 measured at size 1
    // (normalized/entities.json), which is 0.625 of the side.
    state.eye_height = side * 0.625F;
    state.max_health = gameplay::slime_health(clamped);
    state.health     = state.max_health;
}

void Slimes::on_spawn(entity::EntityState& state, math::LegacyRandomSource& random,
                      f32 special_multiplier) {
    set_size(state, gameplay::draw_slime_size(random, special_multiplier));
}

i32 Slimes::size_of(i32 network_id) const noexcept {
    const auto found = sizes_.find(network_id);
    return found == sizes_.end() ? 1 : found->second;
}

void Slimes::spawn_metadata(const entity::EntityState& state, net::MetadataWriter& fields) const {
    if (!owns(state.type)) {
        return;
    }
    fields.varint_value(kSlimeSize, size_of(state.network_id));
}

void Slimes::on_death(const entity::EntityState& state, math::LegacyRandomSource& random,
                      std::vector<gameplay::SlimeChild>& out) {
    out.clear();
    if (!owns(state.type)) {
        return;
    }
    gameplay::slime_children(size_of(state.network_id), random, out);
    for (gameplay::SlimeChild& child : out) {
        child.offset = state.position + child.offset;
    }
    sizes_.erase(state.network_id);
}

}  // namespace ov::server
