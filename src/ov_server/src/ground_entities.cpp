// ── persistence ── See ground_entities.hpp.
#define OV_LOG_CATEGORY "server"

#include "ground_entities.hpp"

#include "entity_nbt.hpp"

#include "ov/base/log.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace ov::server {
namespace {

[[nodiscard]] ChunkPos chunk_at(f64 x, f64 z) noexcept {
    return ChunkPos{static_cast<i32>(std::floor(x)) >> 4, static_cast<i32>(std::floor(z)) >> 4};
}

/// A short the way vanilla's own clamps would leave it.
[[nodiscard]] i16 to_short(i64 value) noexcept {
    return static_cast<i16>(std::clamp<i64>(value, std::numeric_limits<i16>::min(),
                                            std::numeric_limits<i16>::max()));
}

/// `Age` -32768 is "never ages" (minecraft.wiki); `PickupDelay` 32767 is
/// "never picked up". Both are kept by pushing the count far out of reach, so
/// the clamp above writes them back as they came.
constexpr i64 kFrozenAge    = std::numeric_limits<i16>::min();
constexpr i64 kFarAway      = i64{1} << 40;
constexpr i32 kNeverPickup  = std::numeric_limits<i16>::max();

}  // namespace

GroundEntities::GroundEntities(DimensionId dimension, const registry::Registries& registries,
                               std::vector<ItemEntity>& items, std::vector<GroundOrb>& orbs,
                               GroundHost host)
    : dimension_{dimension},
      registries_{&registries},
      items_{registries.find("minecraft:item")},
      items_list_{&items},
      orbs_{&orbs},
      host_{std::move(host)} {}

bool GroundEntities::owns_type(std::string_view type) const noexcept {
    return type == "minecraft:item" || type == "minecraft:experience_orb";
}

bool GroundEntities::adopt_saved(const nbt::Tag& compound) {
    const nbt::Tag* id = compound.find("id");
    if (id == nullptr || !host_.now || !host_.next_entity_id) {
        return false;
    }
    const i64   now = host_.now();
    const Vec3d at  = list_vec3(compound, "Pos");
    const i64   age = get_i64(compound, "Age", 0);
    const i64   born = age == kFrozenAge ? now + kFarAway : now - age;
    if (id->as_string() == "minecraft:experience_orb") {
        const i64 value = get_i64(compound, "Value", 0);
        const i64 count = std::max<i64>(get_i64(compound, "Count", 1), 1);
        if (value <= 0) {
            return false;
        }
        GroundOrb orb;
        orb.entity_id = host_.next_entity_id();
        orb.x         = at.x;
        orb.y         = at.y;
        orb.z         = at.z;
        // `Count` orbs of `Value` each, which vanilla gives one pickup at a
        // time; this server's orb is one pickup of the whole.
        orb.value     = static_cast<i32>(std::min<i64>(value * count, std::numeric_limits<i32>::max()));
        orb.born      = born;
        orb.delay     = 0;
        orb.dimension = dimension_;
        orb.uuid      = uuid_from(compound.find("UUID")).value_or(net::Uuid{});
        orbs_->push_back(orb);
        if (host_.announce_orb) {
            host_.announce_orb(orbs_->back());
        }
        return true;
    }
    if (!items_) {
        return false;
    }
    const auto stack = item_stack_from(*registries_, *items_, compound.find("Item"));
    if (!stack) {
        const nbt::Tag* item = compound.find("Item");
        const nbt::Tag* name = item != nullptr ? item->find("id") : nullptr;
        OV_LOG_WARN("entities: an item of {} this server does not know",
                    name != nullptr ? name->as_string() : std::string_view{"nothing"});
        return false;
    }
    ItemEntity item;
    item.entity_id    = host_.next_entity_id();
    item.uuid         = uuid_from(compound.find("UUID"))
                            .value_or(host_.uuid_for ? host_.uuid_for(item.entity_id) : net::Uuid{});
    item.dimension    = dimension_;
    item.x            = at.x;
    item.y            = at.y;
    item.z            = at.z;
    item.stack        = *stack;
    item.born         = born;
    const i64 delay   = get_i64(compound, "PickupDelay", 0);
    item.pickup_delay = delay >= kNeverPickup ? std::numeric_limits<i32>::max() / 2
                                              : static_cast<i32>(std::max<i64>(delay, 0));
    item.saved        = compound;
    items_list_->push_back(std::move(item));
    if (host_.announce_item) {
        host_.announce_item(items_list_->back());
    }
    return true;
}

std::optional<nbt::Tag> GroundEntities::item_nbt(const ItemEntity& item, i64 now) const {
    if (!items_) {
        return std::nullopt;
    }
    auto stack = item_stack_tag(*registries_, *items_, item.stack);
    if (!stack) {
        return std::nullopt;
    }
    nbt::Tag out = item.saved.compound() != nullptr ? item.saved : nbt::Tag::make_compound();
    // No physics here: an item lies still, and says so.
    put_entity_base(out, "minecraft:item", Vec3d{item.x, item.y, item.z}, Vec3d{}, 0.0F, 0.0F,
                    item.uuid, true, i16{-1});
    (void)out.put("Age", nbt::Tag{to_short(now - item.born)});
    (void)out.put("PickupDelay", nbt::Tag{to_short(item.pickup_delay)});
    default_to(out, "Health", nbt::Tag{i16{5}});
    (void)out.put("Item", std::move(*stack));
    return out;
}

void GroundEntities::orb_nbt(const GroundOrb& orb, i64 now, std::vector<LooseEntity>& out) const {
    const net::Uuid uuid =
        orb.uuid != net::Uuid{} ? orb.uuid : (host_.uuid_for ? host_.uuid_for(orb.entity_id) : net::Uuid{});
    i32 left  = orb.value;
    u64 split = 0;
    while (left > 0) {
        const i32 value = std::min<i32>(left, std::numeric_limits<i16>::max());
        left -= value;
        nbt::Tag compound = nbt::Tag::make_compound();
        // A split orb past the first gets a UUID of its own.
        const net::Uuid mine{uuid.most_significant, uuid.least_significant + split++};
        put_entity_base(compound, "minecraft:experience_orb", Vec3d{orb.x, orb.y, orb.z}, Vec3d{},
                        0.0F, 0.0F, mine, true, i16{-1});
        (void)compound.put("Age", nbt::Tag{to_short(now - orb.born)});
        (void)compound.put("Health", nbt::Tag{i16{5}});
        (void)compound.put("Value", nbt::Tag{static_cast<i16>(value)});
        (void)compound.put("Count", nbt::Tag{i32{1}});
        out.push_back(LooseEntity{Vec3d{orb.x, orb.y, orb.z}, std::move(compound)});
    }
}

void GroundEntities::positions(std::vector<Vec3d>& out) const {
    for (const ItemEntity& item : *items_list_) {
        if (item.dimension == dimension_) {
            out.emplace_back(item.x, item.y, item.z);
        }
    }
    for (const GroundOrb& orb : *orbs_) {
        if (orb.dimension == dimension_) {
            out.emplace_back(orb.x, orb.y, orb.z);
        }
    }
}

void GroundEntities::save(std::vector<LooseEntity>& out) const {
    const i64 now = host_.now ? host_.now() : 0;
    for (const ItemEntity& item : *items_list_) {
        if (item.dimension != dimension_) {
            continue;
        }
        if (auto compound = item_nbt(item, now)) {
            out.push_back(LooseEntity{Vec3d{item.x, item.y, item.z}, std::move(*compound)});
        }
    }
    for (const GroundOrb& orb : *orbs_) {
        if (orb.dimension == dimension_) {
            orb_nbt(orb, now, out);
        }
    }
}

void GroundEntities::release(const std::function<bool(ChunkPos)>& leaving,
                             std::vector<LooseEntity>& out, std::vector<i32>& removed) {
    const i64 now = host_.now ? host_.now() : 0;
    std::erase_if(*items_list_, [&](const ItemEntity& item) {
        if (item.dimension != dimension_ || !leaving(chunk_at(item.x, item.z))) {
            return false;
        }
        if (auto compound = item_nbt(item, now)) {
            out.push_back(LooseEntity{Vec3d{item.x, item.y, item.z}, std::move(*compound)});
        }
        removed.push_back(item.entity_id);
        return true;
    });
    std::erase_if(*orbs_, [&](const GroundOrb& orb) {
        if (orb.dimension != dimension_ || !leaving(chunk_at(orb.x, orb.z))) {
            return false;
        }
        orb_nbt(orb, now, out);
        removed.push_back(orb.entity_id);
        return true;
    });
}

}  // namespace ov::server
