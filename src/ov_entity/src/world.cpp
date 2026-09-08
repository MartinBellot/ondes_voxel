#include "ov/entity/world.hpp"

#include "ov/base/assert.hpp"

#include <entt/entity/registry.hpp>

#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ov::entity {
namespace {

/// The behaviour component. A separate type so that an entity without one —
/// a dropped stack, a painting — costs nothing at all.
struct Behaviour {
    std::unique_ptr<IEntityLogic> logic;
};

/// The attributes a type owns, copied in at spawn.
///
/// Copied rather than looked up per query because a modifier will eventually
/// change them per entity: a zombie that picked up armour has a different
/// armour value from the type's, and the type table must stay the type's.
struct Attributes {
    std::vector<registry::Registries::EntityAttribute> values;
};

/// EnTT's handle type is a u32 carrying an index and a version. Ours is the
/// same u32, so the conversion is a bit pattern and not a lookup — and the
/// version travels with it, which is the entire point of a generation.
[[nodiscard]] constexpr entt::entity to_entt(EntityHandle handle) noexcept {
    return static_cast<entt::entity>(handle.value());
}

[[nodiscard]] constexpr EntityHandle from_entt(entt::entity entity) noexcept {
    return EntityHandle{static_cast<u32>(entity)};
}

static_assert(std::is_same_v<std::underlying_type_t<entt::entity>, u32>,
              "EntityHandle mirrors entt::entity exactly; a wider entt::entity would "
              "silently truncate the generation and make stale handles resolve");

}  // namespace

std::string_view to_string(SpawnError error) noexcept {
    switch (error) {
    case SpawnError::UnknownType:
        return "unknown entity type";
    case SpawnError::UnmeasuredType:
        return "entity type has no measured hitbox";
    }
    return "unknown error";
}

struct EntityWorld::Impl {
    const registry::Registries* registries{nullptr};
    registry::RegistryId        entity_type_registry{};
    bool                        has_entity_registry{false};

    entt::registry storage;

    /// Insertion order. The tick walks this, not a view: see world.hpp.
    std::vector<EntityHandle> handles;
    /// Wire id to handle. The server looks entities up by the id a client sent.
    std::unordered_map<i32, EntityHandle> by_network_id;

    i32 next_network_id{1};

    /// minecraft:generic.max_health, resolved once. Not a constant: it is
    /// index 0 today, and the whole reason this project reads a registry rather
    /// than writing numbers down is that "today" is a version.
    i32 max_health_attribute{-1};

    /// Scratch, reused every tick so the steady state allocates nothing.
    std::vector<EntityHandle> tick_order;
    std::vector<i32>          removed_ids;
};

EntityWorld::EntityWorld(const registry::Registries& registries, i32 first_network_id)
    : impl_{std::make_unique<Impl>()} {
    impl_->registries      = &registries;
    impl_->next_network_id = first_network_id;
    if (const auto id = registries.find("minecraft:entity_type")) {
        impl_->entity_type_registry = *id;
        impl_->has_entity_registry  = true;
    }
    if (const auto attributes = registries.find("minecraft:attribute")) {
        if (const auto max_health =
                registries.protocol_id(*attributes, "minecraft:generic.max_health")) {
            impl_->max_health_attribute = *max_health;
        }
    }
}

EntityWorld::~EntityWorld()                            = default;
EntityWorld::EntityWorld(EntityWorld&&) noexcept       = default;
EntityWorld& EntityWorld::operator=(EntityWorld&&) noexcept = default;

std::expected<EntityHandle, SpawnError> EntityWorld::spawn(std::string_view type_name,
                                                           const Vec3d&     position,
                                                           const net::Uuid& uuid) {
    if (!impl_->has_entity_registry) {
        return std::unexpected{SpawnError::UnknownType};
    }
    const auto type = impl_->registries->protocol_id(impl_->entity_type_registry, type_name);
    if (!type) {
        return std::unexpected{SpawnError::UnknownType};
    }
    return spawn(*type, position, uuid);
}

std::expected<EntityHandle, SpawnError> EntityWorld::spawn(i32 type, const Vec3d& position,
                                                           const net::Uuid& uuid) {
    const auto info = impl_->registries->entity_type(type);
    if (!info) {
        // Either the id is out of range, or the type is one of the four the
        // measurement could not reach. Both are refusals, and both are better
        // than a mob with no box.
        return std::unexpected{SpawnError::UnmeasuredType};
    }

    const entt::entity entity = impl_->storage.create();
    const EntityHandle handle = from_entt(entity);

    EntityState state{};
    state.network_id = impl_->next_network_id++;
    state.type       = type;
    state.uuid       = uuid;
    state.position   = position;
    state.width      = info->width;
    state.height     = info->height;
    state.eye_height = info->eye_height;

    Attributes attributes{impl_->registries->entity_attributes(type)};

    // Health starts at the type's maximum. The pair travels together because
    // one without the other is a mob that is either unkillable or already dead.
    for (const auto& value : attributes.values) {
        if (value.attribute == impl_->max_health_attribute) {
            state.max_health = static_cast<f32>(value.base);
            state.health     = state.max_health;
        }
    }

    impl_->storage.emplace<EntityState>(entity, state);
    if (!attributes.values.empty()) {
        impl_->storage.emplace<Attributes>(entity, std::move(attributes));
    }

    impl_->handles.push_back(handle);
    impl_->by_network_id.emplace(state.network_id, handle);
    return handle;
}

bool EntityWorld::remove(EntityHandle handle) {
    const entt::entity entity = to_entt(handle);
    if (!impl_->storage.valid(entity)) {
        return false;
    }
    const EntityState& state = impl_->storage.get<EntityState>(entity);
    impl_->by_network_id.erase(state.network_id);
    impl_->storage.destroy(entity);
    // erase, not swap-and-pop: the order of this vector is the order the tick
    // runs in, and reordering it would make the simulation depend on which
    // entity happened to die.
    const auto it = std::ranges::find(impl_->handles, handle);
    if (it != impl_->handles.end()) {
        impl_->handles.erase(it);
    }
    return true;
}

bool EntityWorld::alive(EntityHandle handle) const noexcept {
    return impl_->storage.valid(to_entt(handle));
}

usize EntityWorld::size() const noexcept { return impl_->handles.size(); }

const EntityState* EntityWorld::state(EntityHandle handle) const noexcept {
    const entt::entity entity = to_entt(handle);
    if (!impl_->storage.valid(entity)) {
        return nullptr;
    }
    return impl_->storage.try_get<EntityState>(entity);
}

EntityState* EntityWorld::mutable_state(EntityHandle handle) noexcept {
    const entt::entity entity = to_entt(handle);
    if (!impl_->storage.valid(entity)) {
        return nullptr;
    }
    return impl_->storage.try_get<EntityState>(entity);
}

std::optional<f64> EntityWorld::attribute(EntityHandle handle, i32 attribute) const noexcept {
    const entt::entity entity = to_entt(handle);
    if (!impl_->storage.valid(entity)) {
        return std::nullopt;
    }
    const auto* attributes = impl_->storage.try_get<Attributes>(entity);
    if (attributes == nullptr) {
        return std::nullopt;
    }
    for (const auto& value : attributes->values) {
        if (value.attribute == attribute) {
            return value.base;
        }
    }
    return std::nullopt;
}

void EntityWorld::set_logic(EntityHandle handle, std::unique_ptr<IEntityLogic> logic) {
    const entt::entity entity = to_entt(handle);
    if (!impl_->storage.valid(entity)) {
        return;
    }
    impl_->storage.emplace_or_replace<Behaviour>(entity, std::move(logic));
}

IEntityLogic* EntityWorld::logic(EntityHandle handle) noexcept {
    const entt::entity entity = to_entt(handle);
    if (!impl_->storage.valid(entity)) {
        return nullptr;
    }
    auto* behaviour = impl_->storage.try_get<Behaviour>(entity);
    return behaviour == nullptr ? nullptr : behaviour->logic.get();
}

std::span<const EntityHandle> EntityWorld::handles() const noexcept { return impl_->handles; }

EntityHandle EntityWorld::find(i32 network_id) const noexcept {
    const auto it = impl_->by_network_id.find(network_id);
    return it == impl_->by_network_id.end() ? kNoEntity : it->second;
}

void EntityWorld::tick(const TickContext& context) {
    impl_->removed_ids.clear();

    // A snapshot of who exists now. Logic may spawn, and a spawn during the
    // tick appends to `handles` — iterating it directly would tick the newborn
    // in the same tick that made it, and a mob that spawns one every tick would
    // never let the loop end.
    impl_->tick_order.assign(impl_->handles.begin(), impl_->handles.end());

    for (const EntityHandle handle : impl_->tick_order) {
        const entt::entity entity = to_entt(handle);
        if (!impl_->storage.valid(entity)) {
            continue;  // removed by something that ran earlier this tick
        }
        auto* behaviour = impl_->storage.try_get<Behaviour>(entity);
        if (behaviour == nullptr || behaviour->logic == nullptr) {
            continue;
        }
        behaviour->logic->tick(*this, handle, context);
    }

    // Removals are applied once, at the end, so nothing is destroyed while the
    // loop above still holds a reference into its storage.
    for (const EntityHandle handle : impl_->tick_order) {
        const entt::entity entity = to_entt(handle);
        if (!impl_->storage.valid(entity)) {
            continue;
        }
        const auto* state = impl_->storage.try_get<EntityState>(entity);
        if (state == nullptr || !state->removed) {
            continue;
        }
        impl_->removed_ids.push_back(state->network_id);
        remove(handle);
    }
}

std::span<const i32> EntityWorld::removed_ids() const noexcept { return impl_->removed_ids; }

}  // namespace ov::entity
