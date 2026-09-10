#include "husbandry.hpp"

#include "ov/base/log.hpp"
#include "ov/gameplay/breeding.hpp"
#include "ov/gameplay/durability.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace ov::server {
namespace {

/// Vanilla checks a click against the distance from the player's eyes to the
/// entity's box; six blocks is the protocol's interaction reach. Not measured
/// here — the probe always stood within two blocks.
constexpr f64 kReach = 6.0;

/// Entity Event status of a sheep lowering its head to graze. Captured: 54
/// events over the regrow campaign, every one of them 10.
constexpr i8 kGrazeEventStatus = 10;

[[nodiscard]] f64 box_distance_sq(const entity::EntityState& state, Vec3d point) noexcept {
    const f64 half = static_cast<f64>(state.width) * 0.5;
    const auto axis = [](f64 v, f64 lo, f64 hi) {
        return v < lo ? lo - v : (v > hi ? v - hi : 0.0);
    };
    const f64 dx = axis(point.x, state.position.x - half, state.position.x + half);
    const f64 dy =
        axis(point.y, state.position.y, state.position.y + static_cast<f64>(state.height));
    const f64 dz = axis(point.z, state.position.z - half, state.position.z + half);
    return dx * dx + dy * dy + dz * dz;
}

[[nodiscard]] gameplay::Mob* mob_of(entity::EntityWorld& world, entity::EntityHandle handle) {
    return dynamic_cast<gameplay::Mob*>(world.logic(handle));
}

}  // namespace

Husbandry::Husbandry(const registry::Registries& registries,
                     const registry::BlockRegistry& blocks)
    : registries_{&registries}, blocks_{&blocks} {
    item_registry_   = registries.find("minecraft:item");
    entity_registry_ = registries.find("minecraft:entity_type");
    pending_.reserve(16);
    working_.reserve(16);
    tempters_.reserve(16);
    events_.reserve(64);
}

i32 Husbandry::item_id(std::string_view name) const {
    if (!item_registry_) {
        return -1;
    }
    return registries_->protocol_id(*item_registry_, name).value_or(-1);
}

std::string_view Husbandry::item_name(i32 id) const {
    return item_registry_ ? registries_->entry_of(*item_registry_, id) : std::string_view{};
}

std::string_view Husbandry::type_name(i32 type) const {
    return entity_registry_ ? registries_->entry_of(*entity_registry_, type) : std::string_view{};
}

void Husbandry::queue_interact(i32 player, i32 entity, net::Hand hand) {
    const std::scoped_lock lock{mutex_};
    pending_.push_back(Click{player, entity, hand});
}

bool Husbandry::has_pending() const {
    const std::scoped_lock lock{mutex_};
    return !pending_.empty();
}

void Husbandry::spawn_metadata(entity::EntityWorld& world, const entity::EntityState& state,
                               net::MetadataWriter& fields) const {
    const gameplay::MobBrain* brain = gameplay::mob_brain_of(world, world.find(state.network_id));
    if (brain == nullptr) {
        return;
    }
    const gameplay::AnimalState& animal = brain->animal;
    // Only what differs from the default: a real server's spawn of an adult
    // cow carries no index 16 at all (captured).
    if (animal.baby()) {
        fields.boolean_value(net::metadata::kAgeableBaby, true);
    }
    const std::string_view type = type_name(state.type);
    if (type == "minecraft:sheep" && (animal.colour != 0 || animal.sheared)) {
        fields.byte_value(net::metadata::kSheepFleece,
                          static_cast<i8>(animal.colour | (animal.sheared ? net::metadata::kSheepSheared
                                                                          : 0)));
    }
    if (type == "minecraft:pig" && animal.saddled) {
        fields.boolean_value(net::metadata::kPigSaddle, true);
    }
}

void Husbandry::send_metadata(const entity::EntityState& state,
                              const gameplay::AnimalState& animal,
                              const HusbandryDeliver& deliver) const {
    // An update carries its fields even at their defaults: a calf that grew
    // up has to be told `false`, not left to keep its last `true`.
    net::MetadataWriter fields;
    fields.boolean_value(net::metadata::kAgeableBaby, animal.baby());
    const std::string_view type = type_name(state.type);
    if (type == "minecraft:sheep") {
        fields.byte_value(net::metadata::kSheepFleece,
                          static_cast<i8>(animal.colour | (animal.sheared ? net::metadata::kSheepSheared
                                                                          : 0)));
    } else if (type == "minecraft:pig") {
        fields.boolean_value(net::metadata::kPigSaddle, animal.saddled);
    }
    deliver(net::clientbound::kEntityMetadata,
            net::encode_entity_metadata(state.network_id, fields.take()));
}

void Husbandry::make_baby(entity::EntityWorld& world, entity::EntityHandle handle) const {
    entity::EntityState* state = world.mutable_state(handle);
    gameplay::Mob*       mob   = mob_of(world, handle);
    if (state == nullptr || mob == nullptr ||
        gameplay::animal_kind(type_name(state->type)) == nullptr) {
        return;
    }
    mob->make_baby(*state);
}

HusbandryStats Husbandry::before_entity_tick(entity::EntityWorld& world,
                                             gameplay::MobContext& context,
                                             const HusbandryHost& host,
                                             const HusbandryDeliver& deliver) {
    HusbandryStats stats;
    {
        const std::scoped_lock lock{mutex_};
        working_.swap(pending_);
    }
    for (const Click& click : working_) {
        interact(world, click, host, deliver, stats);
    }
    working_.clear();

    tempters_.clear();
    if (host.tempters) {
        host.tempters(tempters_);
    }
    events_.clear();
    context.tempters      = tempters_;
    context.animal_events = &events_;
    return stats;
}

void Husbandry::interact(entity::EntityWorld& world, const Click& click,
                         const HusbandryHost& host, const HusbandryDeliver& deliver,
                         HusbandryStats& stats) {
    const entity::EntityHandle handle = world.find(click.entity);
    entity::EntityState*       state  = world.mutable_state(handle);
    gameplay::Mob*             mob    = mob_of(world, handle);
    if (state == nullptr || state->removed || mob == nullptr || !host.with_hand) {
        return;
    }
    const std::string_view        type = type_name(state->type);
    const gameplay::AnimalKind*   kind = gameplay::animal_kind(type);
    if (kind == nullptr) {
        // Right-clicking a villager, a horse, a wolf: named, not treated as
        // nothing happening for a reason.
        OV_LOG_DEBUG("interact: {} has no right-click behaviour here", type);
        return;
    }
    gameplay::AnimalState& animal = mob->mutable_brain().animal;

    (void)host.with_hand(click.player, click.hand, [&](HusbandryHand& hand) {
        if (box_distance_sq(*state, hand.eyes) >= kReach * kReach) {
            return;
        }
        net::ItemStack& held = hand.inventory[hand.slot];
        if (held.item_id == 0 || held.count <= 0) {
            return;  // an empty hand does nothing to a farm animal
        }
        const std::string_view item = item_name(held.item_id);
        const auto consume = [&] {
            if (hand.creative) {
                return;
            }
            held.count = static_cast<i8>(held.count - 1);
            if (held.count <= 0) {
                held = net::ItemStack{};
            }
            hand.send_slot(hand.slot);
        };

        // Milk. Measured: one bucket becomes a milk bucket in the hand; of a
        // stack of two, one stays and the milk goes to the inventory; a calf
        // gives nothing.
        if (kind->milkable && item == "minecraft:bucket" && !animal.baby()) {
            const net::ItemStack milk{item_id("minecraft:milk_bucket"), 1, {}};
            if (hand.creative) {
                // Not measured: the game's creative rule keeps the bucket and
                // adds milk only if none is there yet. Said, and done so.
                const bool has = std::ranges::any_of(hand.inventory, [&](const net::ItemStack& s) {
                    return s.item_id == milk.item_id && s.count > 0;
                });
                if (!has && hand.give) {
                    (void)hand.give(milk);
                }
            } else if (held.count == 1) {
                held = milk;
                hand.send_slot(hand.slot);
            } else {
                consume();
                if (!hand.give || hand.give(milk) == 0) {
                    if (host.drop_item) {
                        host.drop_item(hand.eyes, milk);
                    }
                }
            }
            ++stats.milked;
            return;
        }

        // Shear. Measured: an adult unshorn sheep gives 1 to 3 wool of its
        // colour, the shears take one point of wear; a lamb and a shorn sheep
        // give nothing and cost nothing.
        if (kind->shearable && item == "minecraft:shears") {
            if (animal.baby() || animal.sheared) {
                return;
            }
            animal.sheared = true;
            const i32 count = gameplay::wool_count(random_);
            const std::string wool = "minecraft:" +
                                     std::string{gameplay::colour_names()[static_cast<usize>(
                                         std::clamp<i8>(animal.colour, 0, 15))]} +
                                     "_wool";
            const i32 wool_id = item_id(wool);
            for (i32 i = 0; i < count && host.drop_item && wool_id >= 0; ++i) {
                // One stack per wool, as the game drops them (each arrived as
                // its own item entity in the capture), a block above the feet.
                host.drop_item(state->position + Vec3d{0.0, 1.0, 0.0},
                               net::ItemStack{wool_id, 1, {}});
            }
            if (!hand.creative && hand.wear) {
                hand.wear(1);
            }
            send_metadata(*state, animal, deliver);
            ++stats.shorn;
            return;
        }

        // Dye. Measured: an unshorn sheep takes a new colour, lamb included,
        // for one dye; the same colour, or a shorn sheep, takes nothing.
        if (kind->shearable) {
            if (const auto colour = gameplay::dye_colour(item)) {
                if (animal.sheared || animal.colour == *colour) {
                    return;
                }
                animal.colour = *colour;
                consume();
                send_metadata(*state, animal, deliver);
                ++stats.dyed;
                return;
            }
        }

        // Saddle. Measured: an adult pig takes it, a piglet does not. Riding
        // a saddled pig is not implemented — named in elevage.md.
        if (kind->saddleable && item == "minecraft:saddle") {
            if (animal.baby() || animal.saddled) {
                return;
            }
            animal.saddled = true;
            consume();
            send_metadata(*state, animal, deliver);
            ++stats.saddled;
            return;
        }

        // Food.
        switch (gameplay::feed(animal, *kind, item, click.player)) {
        case gameplay::FeedResult::NotFood:
        case gameplay::FeedResult::Refused:
            return;
        case gameplay::FeedResult::Love:
            consume();
            // The hearts: captured, status 18 on the animal fed.
            deliver(net::clientbound::kEntityEvent,
                    net::encode_entity_event(state->network_id, gameplay::kLoveEventStatus));
            ++stats.fed;
            return;
        case gameplay::FeedResult::Grew:
            consume();
            ++stats.fed;
            return;
        }
    });
}

HusbandryStats Husbandry::after_entity_tick(entity::EntityWorld& world, world::LevelWriter* level,
                                            const HusbandryHost& host,
                                            const HusbandryDeliver& deliver) {
    HusbandryStats stats;
    for (const gameplay::AnimalEvent& event : events_) {
        entity::EntityState* self = world.mutable_state(event.self);
        gameplay::MobBrain*  brain = gameplay::mob_brain_of(world, event.self);
        if (self == nullptr || brain == nullptr) {
            continue;
        }
        switch (event.kind) {
        case gameplay::AnimalEventKind::Birth: {
            const std::string_view type = type_name(self->type);
            if (!host.create_mob) {
                break;
            }
            const entity::EntityHandle calf = host.create_mob(type, event.at);
            entity::EntityState*       calf_state = world.mutable_state(calf);
            gameplay::Mob*             calf_mob   = mob_of(world, calf);
            if (calf_state == nullptr || calf_mob == nullptr) {
                break;
            }
            calf_mob->make_baby(*calf_state);
            if (type == "minecraft:sheep") {
                const gameplay::MobBrain* other = gameplay::mob_brain_of(world, event.other);
                const i8 mother = brain->animal.colour;
                const i8 father = other != nullptr ? other->animal.colour : mother;
                calf_mob->mutable_brain().animal.colour =
                    gameplay::offspring_colour(mother, father, random_);
            }
            if (host.announce) {
                host.announce(*calf_state);
            }
            // The capture shows a third status 18 per birth, on one parent.
            deliver(net::clientbound::kEntityEvent,
                    net::encode_entity_event(self->network_id, gameplay::kLoveEventStatus));
            if (host.spawn_orb) {
                host.spawn_orb(event.at, gameplay::breeding_xp(random_));
            }
            // The `animals_bred` statistic (measured: +1 per birth to the
            // player who fed) has nowhere to go: this server keeps no
            // statistics. Named in elevage.md.
            ++stats.births;
            break;
        }
        case gameplay::AnimalEventKind::GrewUp:
            send_metadata(*self, brain->animal, deliver);
            ++stats.grown;
            break;
        case gameplay::AnimalEventKind::LaidEgg:
            if (host.drop_item) {
                host.drop_item(event.at, net::ItemStack{item_id("minecraft:egg"), 1, {}});
            }
            ++stats.eggs;
            break;
        case gameplay::AnimalEventKind::GrazeStart:
            deliver(net::clientbound::kEntityEvent,
                    net::encode_entity_event(self->network_id, kGrazeEventStatus));
            break;
        case gameplay::AnimalEventKind::AteGrass:
            if (level != nullptr) {
                // Measured: the grass under a grazing sheep is dirt afterwards.
                // Assumes mobGriefing, the default and the measured case.
                if (event.grass_block) {
                    if (const auto dirt = blocks_->find_block("minecraft:dirt")) {
                        level->set_block(event.block, blocks_->default_state(*dirt));
                    }
                } else {
                    level->set_block(event.block, registry::kAirState);
                }
            }
            send_metadata(*self, brain->animal, deliver);
            ++stats.grazed;
            break;
        }
    }
    events_.clear();
    return stats;
}

}  // namespace ov::server
