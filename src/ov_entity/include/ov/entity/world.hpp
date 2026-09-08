// Where entities live: an EnTT registry behind a door that never opens.
//
// EnTT is a PRIVATE_DEP of this module. Nothing about it appears below — no
// entt::registry, no view, no component type — because a template that heavy in
// a public header lands in every translation unit that includes it, and build
// velocity is what actually kills a project this size (risk R5). The cost is
// this file: a hand-written surface instead of a generic one.
//
// Two rules from docs/ARCHITECTURE.md § 5 are load-bearing here:
//
//   * EnTT is not thread-safe, and `view()` / `group()` mutate internal state
//     even when only read. The registry therefore lives on the tick thread and
//     nothing else touches it.
//
//   * Iteration order must be deterministic. An EnTT view iterates *storage*
//     order, and storage is swap-and-pop: removing one entity moves the last
//     one into its slot, so two runs of the same sequence of spawns and
//     removals tick the same entities in different orders. So the tick walks
//     the insertion-ordered handle list below instead, and no view is created
//     on the tick path at all — which happens to make the thread-safety trap
//     unreachable as well.
#pragma once

#include "ov/base/types.hpp"
#include "ov/entity/entity.hpp"
#include "ov/entity/logic.hpp"
#include "ov/protocol/types.hpp"
#include "ov/registry/registries.hpp"

#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace ov::entity {

enum class SpawnError : u8 {
    /// No such name in minecraft:entity_type.
    UnknownType,
    /// The type exists but was never measured, so its box is unknown.
    ///
    /// Four of the game's 124 types are in this state and they are refused by
    /// name rather than given a zero-sized box — a mob nothing can ever hit is
    /// worse than a mob that failed to spawn, because only one of them says so.
    UnmeasuredType,
};

[[nodiscard]] std::string_view to_string(SpawnError error) noexcept;

/// Every entity in one world.
class EntityWorld {
public:
    /// `registries` must outlive the world. `first_network_id` is where wire ids
    /// start: the server hands out ids to players from the same space, so the
    /// two allocators are kept apart by construction rather than by luck.
    explicit EntityWorld(const registry::Registries& registries, i32 first_network_id = 1);
    ~EntityWorld();

    EntityWorld(const EntityWorld&)            = delete;
    EntityWorld& operator=(const EntityWorld&) = delete;
    EntityWorld(EntityWorld&&) noexcept;
    EntityWorld& operator=(EntityWorld&&) noexcept;

    /// Spawn by registry name, e.g. "minecraft:zombie".
    [[nodiscard]] std::expected<EntityHandle, SpawnError> spawn(std::string_view type_name,
                                                                const Vec3d&     position,
                                                                const net::Uuid& uuid);

    /// Spawn by the type's protocol id.
    [[nodiscard]] std::expected<EntityHandle, SpawnError> spawn(i32 type, const Vec3d& position,
                                                                const net::Uuid& uuid);

    /// Remove now. During a tick, set `EntityState::removed` instead.
    bool remove(EntityHandle handle);

    [[nodiscard]] bool  alive(EntityHandle handle) const noexcept;
    [[nodiscard]] usize size() const noexcept;

    /// Null for a handle that no longer resolves — a stale one included.
    [[nodiscard]] const EntityState* state(EntityHandle handle) const noexcept;
    [[nodiscard]] EntityState*       mutable_state(EntityHandle handle) noexcept;

    /// The base value of one attribute, as the game measures it for this type.
    ///
    /// Nullopt when the type does not own the attribute, which is not the same
    /// as owning it at zero: a cow has no attack damage at all.
    [[nodiscard]] std::optional<f64> attribute(EntityHandle handle,
                                               i32          attribute) const noexcept;

    void                        set_logic(EntityHandle handle, std::unique_ptr<IEntityLogic> logic);
    [[nodiscard]] IEntityLogic* logic(EntityHandle handle) noexcept;

    /// Every live entity, in the order they were spawned.
    ///
    /// Insertion order, not storage order — see the note at the top of this
    /// file. Invalidated by any spawn or removal.
    [[nodiscard]] std::span<const EntityHandle> handles() const noexcept;

    /// The entity carrying a given wire id, or kNoEntity.
    [[nodiscard]] EntityHandle find(i32 network_id) const noexcept;

    /// Tick every entity's logic once, then apply the removals it asked for.
    ///
    /// Spawns made during the tick are visible immediately but are not ticked
    /// until the next one, exactly as vanilla does: a mob that spawns another
    /// mob every tick would otherwise recurse until the tick never ended.
    void tick(const TickContext& context);

    /// The entities removed by the last call to `tick`, by wire id.
    ///
    /// The caller needs these to send Remove Entities, and the state they came
    /// from is gone by then. Cleared at the start of each tick.
    [[nodiscard]] std::span<const i32> removed_ids() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ov::entity
