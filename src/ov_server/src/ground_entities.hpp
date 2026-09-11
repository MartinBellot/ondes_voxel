// ── persistence ── The items and experience orbs on the ground, to and from
// `entities/`.
//
// Neither lives in the entity world: the server keeps them in two flat lists
// (they have no physics here), under the players' lock. This adopter reads
// and writes those lists for the entity storage of one dimension — the only
// writer of that dimension's `entities/` — as the rails session does its
// carts:
//
//   * `minecraft:item`: `Item` {id, Count, tag}, `Age` (short, counts up; the
//     item goes at 6000 — this server's `born` is `now - Age`), `PickupDelay`
//     (short), `Health` (short, 5 for a dropped stack; measured 0 for one
//     summoned without it), and `Owner`/`Thrower` carried as they came;
//   * `minecraft:experience_orb`: `Value` (short), `Count` (int, how many
//     orbs of that value the entity stands for), `Age` (short), `Health`
//     (short, 5).
//
// Both with the base keys every entity has, `Fire` -1 (measured). Fields this
// server does not model are carried through from the compound an entity was
// read with. Sources: minecraft.wiki "Entity format"; checked key by key and
// type by type against the real 1.20.1 server (scripts/measure_persistence.py,
// docs/provenance/persistance-entites.md).
//
// Threads: every call is made with the players' lock held — the lists'.
#pragma once

#include "entity_storage.hpp"
#include "nether_travel.hpp"  // DimensionId

#include "ov/nbt/tag.hpp"
#include "ov/protocol/play.hpp"
#include "ov/registry/registries.hpp"

#include <functional>
#include <optional>
#include <vector>

namespace ov::server {

/// A stack lying on the ground, waiting to be walked into.
///
/// Kept in a flat list on the tick thread. There are a handful at a time, and
/// an index would cost more than the scan it saves.
struct ItemEntity {
    i32            entity_id{0};
    net::Uuid      uuid{};
    /// ── nether ── The level it lies in: only players there see it and pick
    /// it up. Items have no physics here, so nothing else needs to know.
    DimensionId dimension{DimensionId::Overworld};
    f64            x{0.0};
    f64            y{0.0};
    f64            z{0.0};
    net::ItemStack stack{};
    /// The tick it appeared, for the five minutes vanilla gives it.
    i64 born{0};
    /// Ticks before anyone may pick it up. Vanilla gives a dropped stack half a
    /// second so the player who broke the block does not instantly re-absorb a
    /// block they meant to place.
    i32 pickup_delay{10};
    /// ── persistence ── The compound it was read with (End when none): its
    /// `Owner`, `Thrower`, `Health`, go back out through it.
    nbt::Tag saved{};
};

/// One experience orb lying in the world.
///
/// Its own type rather than an ItemEntity with a special item: an orb carries a
/// *value* and no stack, it is attracted to a player instead of waiting to be
/// walked into, and two of them can become one. None of that is true of a
/// dropped stack.
struct GroundOrb {
    i32 entity_id{0};
    f64 x{0.0};
    f64 y{0.0};
    f64 z{0.0};
    i32 value{1};
    /// The tick it appeared, for the five minutes vanilla gives it.
    i64 born{0};
    /// Ticks before anyone may pick it up.
    i32 delay{0};
    /// ── persistence ── The level it lies in, and the UUID it was read with
    /// (zero: the one the server derives from its wire id).
    DimensionId dimension{DimensionId::Overworld};
    net::Uuid   uuid{};
};

/// What the adopter reaches outside itself for.
struct GroundHost {
    /// The clock `born` is counted in.
    std::function<i64()> now;
    /// A fresh wire id.
    std::function<i32()> next_entity_id;
    /// The UUID the server gives an entity that has none of its own.
    std::function<net::Uuid(i32 id)> uuid_for;
    /// Tell the players of the item's (the orb's) level about it.
    std::function<void(const ItemEntity&)> announce_item;
    std::function<void(const GroundOrb&)>  announce_orb;
};

class GroundEntities final : public LooseAdopter {
public:
    GroundEntities(DimensionId dimension, const registry::Registries& registries,
                   std::vector<ItemEntity>& items, std::vector<GroundOrb>& orbs, GroundHost host);

    [[nodiscard]] bool owns_type(std::string_view type) const noexcept override;
    bool adopt_saved(const nbt::Tag& compound) override;
    void positions(std::vector<Vec3d>& out) const override;
    void save(std::vector<LooseEntity>& out) const override;
    void release(const std::function<bool(ChunkPos)>& leaving, std::vector<LooseEntity>& out,
                 std::vector<i32>& removed) override;

    /// One item's compound at tick `now`, as vanilla writes it. Nullopt for a
    /// stack of an item the registry cannot name.
    [[nodiscard]] std::optional<nbt::Tag> item_nbt(const ItemEntity& item, i64 now) const;
    /// One orb's compounds — more than one only for a value past a short's
    /// 32767, split into orbs a short can hold.
    void orb_nbt(const GroundOrb& orb, i64 now, std::vector<LooseEntity>& out) const;

private:
    DimensionId                         dimension_;
    const registry::Registries*         registries_;
    std::optional<registry::RegistryId> items_;
    std::vector<ItemEntity>*            items_list_;
    std::vector<GroundOrb>*             orbs_;
    GroundHost                          host_;
};

}  // namespace ov::server
