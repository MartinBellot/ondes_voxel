#include "merchant_session.hpp"

#include "ov/base/log.hpp"
#include "ov/gameplay/breeding.hpp"
#include "ov/gameplay/enchanting.hpp"
#include "ov/gameplay/trading.hpp"
#include "ov/io/byte_writer.hpp"
#include "ov/nbt/binary.hpp"
#include "ov/nbt/tag.hpp"
#include "ov/protocol/varint.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace ov::server {
namespace {

/// The protocol's interaction reach, from the eyes to the entity's box. Not
/// measured here: the probe stood two to three blocks away.
constexpr f64 kReach = 6.0;
/// A trading screen closes when its player is this far from the villager.
/// Not measured; named in villageois.md.
constexpr f64 kLeaveDistance = 16.0;

/// Hostile types and how close each must come before a villager runs. The
/// wiki's "Villager" table; the zombie's 8 is inside what the flee campaign
/// saw (runs at 5 and 7, nothing clear beyond).
struct Hostile {
    std::string_view name;
    f32              distance;
};
constexpr std::array<Hostile, 11> kHostiles{{
    {"minecraft:zombie", 8.0F},
    {"minecraft:husk", 8.0F},
    {"minecraft:drowned", 8.0F},
    {"minecraft:zombie_villager", 8.0F},
    {"minecraft:vex", 8.0F},
    {"minecraft:vindicator", 10.0F},
    {"minecraft:zoglin", 10.0F},
    {"minecraft:evoker", 12.0F},
    {"minecraft:illusioner", 12.0F},
    {"minecraft:ravager", 12.0F},
    {"minecraft:pillager", 15.0F},
}};

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

[[nodiscard]] bool same_kind(const net::ItemStack& a, const net::ItemStack& b) {
    return a.item_id == b.item_id && a.nbt == b.nbt;
}

void normalise(net::ItemStack& stack) {
    if (stack.count <= 0) {
        stack = {};
    }
}

/// Merge into matching stacks, then fill empties. Returns what did not fit.
[[nodiscard]] net::ItemStack deposit(std::span<net::ItemStack> into, net::ItemStack stack,
                                     i32 limit, bool reverse) {
    const usize n = into.size();
    for (int pass = 0; pass < 2 && !stack.empty(); ++pass) {
        for (usize step = 0; step < n && !stack.empty(); ++step) {
            net::ItemStack& slot = into[reverse ? n - 1 - step : step];
            if (pass == 0) {
                if (slot.empty() || !same_kind(slot, stack) || slot.count >= limit) {
                    continue;
                }
                const i32 moved = std::min<i32>(limit - slot.count, stack.count);
                slot.count      = static_cast<i8>(slot.count + moved);
                stack.count     = static_cast<i8>(stack.count - moved);
            } else if (slot.empty()) {
                slot        = stack;
                slot.count  = static_cast<i8>(std::min<i32>(limit, stack.count));
                stack.count = static_cast<i8>(stack.count - slot.count);
            }
        }
    }
    normalise(stack);
    return stack;
}

/// Room for all of `stack` in `into`?
[[nodiscard]] bool fits(std::span<const net::ItemStack> into, const net::ItemStack& stack,
                        i32 limit) {
    i32 room = 0;
    for (const net::ItemStack& slot : into) {
        if (slot.empty()) {
            room += limit;
        } else if (same_kind(slot, stack)) {
            room += std::max(0, limit - slot.count);
        }
    }
    return room >= stack.count;
}

void put_tag(net::ItemStack& stack, const nbt::Tag& tag) {
    if (tag.empty()) {
        stack.nbt.clear();
        return;
    }
    // An unnamed root, as the real server writes one: 0a 00 00 …
    stack.nbt = nbt::write(nbt::Document{"", tag});
}

}  // namespace

// ── Stacks and packets ──────────────────────────────────────────────────────

net::ItemStack trade_stack(const registry::Registries& registries,
                           registry::RegistryId item_registry, const gameplay::TradeItem& item) {
    net::ItemStack out;
    if (item.empty()) {
        return out;
    }
    const auto id = registries.protocol_id(item_registry, item.item);
    if (!id) {
        OV_LOG_WARN("trade: {} is not an item", item.item);
        return out;
    }
    out.item_id  = static_cast<i32>(*id);
    out.count    = static_cast<i8>(item.count);
    nbt::Tag tag = nbt::Tag::make_compound();
    if (gameplay::enchant_max_damage(item.item).has_value()) {
        tag.put("Damage", nbt::Tag{i32{0}});
    }
    if (!item.enchantments.empty()) {
        gameplay::write_enchantments(tag, item.enchantments, item.stored);
    }
    if (item.stew_effect != 0) {
        nbt::Tag effect = nbt::Tag::make_compound();
        effect.put("EffectId", nbt::Tag{item.stew_effect});
        effect.put("EffectDuration", nbt::Tag{item.stew_duration});
        nbt::Tag effects = nbt::Tag::make_list(nbt::TagType::Compound);
        (void)effects.push(std::move(effect));
        tag.put("Effects", std::move(effects));
    }
    if (item.dye_colour >= 0) {
        nbt::Tag display = nbt::Tag::make_compound();
        display.put("color", nbt::Tag{item.dye_colour});
        tag.put("display", std::move(display));
    }
    if (!item.potion.empty()) {
        tag.put("Potion", nbt::Tag{std::string{item.potion}});
    }
    put_tag(out, tag);
    return out;
}

std::vector<u8> encode_merchant_offers(const registry::Registries& registries,
                                       registry::RegistryId item_registry, u8 window,
                                       const gameplay::VillagerState& v) {
    io::ByteWriter w;
    net::write_varint(w, window);
    net::write_varint(w, static_cast<i32>(v.offers.size()));
    for (const gameplay::MerchantOffer& offer : v.offers) {
        net::write_slot(w, trade_stack(registries, item_registry, offer.cost_a));
        net::write_slot(w, trade_stack(registries, item_registry, offer.result));
        net::write_slot(w, trade_stack(registries, item_registry, offer.cost_b));
        w.write_u8(offer.out_of_stock() ? 1 : 0);
        w.write_i32(offer.uses);
        w.write_i32(offer.max_uses);
        w.write_i32(offer.xp);
        w.write_i32(offer.special_price);
        w.write_f32(offer.price_multiplier);
        w.write_i32(offer.demand);
    }
    net::write_varint(w, v.level);
    net::write_varint(w, v.xp);
    // A villager shows the level bar and restocks; a wandering trader does
    // neither (── brains ──; the trader's two bytes are not captured, named).
    w.write_u8(v.wandering ? 0 : 1);
    w.write_u8(v.wandering ? 0 : 1);
    return w.take();
}

void apply_special_prices(gameplay::VillagerState& v, const net::Uuid& player,
                          i32 hero_amplifier) {
    const i32 reputation = v.gossips.reputation(player);
    for (gameplay::MerchantOffer& offer : v.offers) {
        i32 special = 0;
        if (reputation != 0) {
            special += gameplay::brain::reputation_price_diff(reputation, offer.price_multiplier);
        }
        if (hero_amplifier >= 0) {
            special += gameplay::brain::hero_price_diff(hero_amplifier, offer.cost_a.count);
        }
        offer.special_price = special;
    }
}

std::optional<i32> parse_select_trade(std::span<const u8> payload) {
    u32 value = 0;
    for (usize i = 0; i < payload.size() && i < 5; ++i) {
        value |= static_cast<u32>(payload[i] & 0x7FU) << (7U * i);
        if ((payload[i] & 0x80U) == 0) {
            return i + 1 == payload.size() ? std::optional<i32>{static_cast<i32>(value)}
                                           : std::nullopt;
        }
    }
    return std::nullopt;
}

std::string merchant_title(const gameplay::VillagerState& v, const net::Uuid& uuid) {
    std::string_view profession = gameplay::profession_name(v.profession);
    if (const auto colon = profession.find(':'); colon != std::string_view::npos) {
        profession = profession.substr(colon + 1);
    }
    // ── brains ── a wandering trader is named after its type
    const std::string key  = v.wandering ? std::string{"entity.minecraft.wandering_trader"}
                                         : "entity.minecraft.villager." + std::string{profession};
    const std::string type = v.wandering ? "minecraft:wandering_trader" : "minecraft:villager";
    const std::string id   = uuid.to_string();
    return "{\"insertion\":\"" + id +
           "\",\"hoverEvent\":{\"action\":\"show_entity\",\"contents\":{\"type\":\"" + type +
           "\",\"id\":\"" + id + "\",\"name\":{\"translate\":\"" + key +
           "\"}}},\"translate\":\"" + key + "\"}";
}

// ── Villagers ───────────────────────────────────────────────────────────────

Villagers::Villagers(const registry::Registries& registries, const registry::BlockRegistry& blocks)
    : registries_{&registries}, blocks_{&blocks} {
    if (const auto items = registries.find("minecraft:item")) {
        item_registry_ = *items;
    }
    if (const auto menus = registries.find("minecraft:menu")) {
        menu_registry_ = *menus;
        merchant_menu_ = static_cast<i32>(
            registries.protocol_id(menu_registry_, "minecraft:merchant").value_or(-1));
    }
    if (const auto types = registries.find("minecraft:entity_type")) {
        for (const Hostile& hostile : kHostiles) {
            if (const auto id = registries.protocol_id(*types, hostile.name)) {
                hostiles_.push_back(gameplay::HostileSight{static_cast<i32>(*id), hostile.distance});
            }
        }
    }
    pending_.reserve(16);
    working_.reserve(16);
    windows_.reserve(8);
}

std::string_view Villagers::item_name(i32 id) const {
    return registries_->entry_of(item_registry_, id);
}

i32 Villagers::item_id(std::string_view name) const {
    return static_cast<i32>(registries_->protocol_id(item_registry_, name).value_or(-1));
}

i32 Villagers::stack_limit(i32 item) const { return registries_->max_stack_size(item); }

i32 Villagers::cost_a(const gameplay::MerchantOffer& offer) const {
    const i32 id = item_id(offer.cost_a.item);
    return gameplay::cost_a_count(offer, id >= 0 ? stack_limit(id) : 64);
}

bool Villagers::pays(const gameplay::MerchantOffer& offer, const net::ItemStack& a,
                     const net::ItemStack& b) const {
    const i32 id = item_id(offer.cost_a.item);
    return gameplay::satisfied_by(offer, id >= 0 ? stack_limit(id) : 64,
                                  a.empty() ? std::string_view{} : item_name(a.item_id),
                                  a.empty() ? 0 : a.count,
                                  b.empty() ? std::string_view{} : item_name(b.item_id),
                                  b.empty() ? 0 : b.count);
}

gameplay::VillagerState* Villagers::villager_of(entity::EntityWorld& world, i32 network_id) const {
    const entity::EntityHandle handle = world.find(network_id);
    const entity::EntityState* state  = world.state(handle);
    if (state == nullptr || state->removed) {
        return nullptr;
    }
    gameplay::MobBrain* brain = gameplay::mob_brain_of(world, handle);
    return brain != nullptr && brain->villager.active ? &brain->villager : nullptr;
}

void Villagers::queue_interact(i32 player, i32 entity, net::Hand hand, bool sneaking) {
    const std::scoped_lock lock{mutex_};
    Action a;
    a.kind     = ActionKind::Interact;
    a.player   = player;
    a.entity   = entity;
    a.hand     = hand;
    a.sneaking = sneaking;
    pending_.push_back(std::move(a));
}

bool Villagers::queue_select(i32 player, i32 index) {
    const std::scoped_lock lock{mutex_};
    if (!open_.contains(player)) {
        return false;
    }
    Action a;
    a.kind   = ActionKind::Select;
    a.player = player;
    a.index  = index;
    pending_.push_back(std::move(a));
    return true;
}

bool Villagers::queue_click(i32 player, const net::ContainerClick& click) {
    const std::scoped_lock lock{mutex_};
    const auto it = open_.find(player);
    if (it == open_.end() || it->second != click.window_id) {
        return false;
    }
    Action a;
    a.kind   = ActionKind::Click;
    a.player = player;
    a.click  = click;
    pending_.push_back(std::move(a));
    return true;
}

bool Villagers::queue_close(i32 player, u8 window) {
    const std::scoped_lock lock{mutex_};
    const auto it = open_.find(player);
    if (it == open_.end() || it->second != window) {
        return false;
    }
    Action a;
    a.kind   = ActionKind::Close;
    a.player = player;
    a.window = window;
    pending_.push_back(std::move(a));
    return true;
}

VillagerStats Villagers::before_entity_tick(entity::EntityWorld& world,
                                            gameplay::MobContext& context,
                                            const VillagerHost& host,
                                            const VillagerDeliver& deliver, i64 day_time,
                                            i64 game_time) {
    VillagerStats stats;
    world_.day_time  = day_time;
    world_.game_time = game_time;
    world_.hostiles  = hostiles_;
    context.villagers = &world_;

    {
        const std::scoped_lock lock{mutex_};
        working_.swap(pending_);
    }
    for (const Action& a : working_) {
        if (a.kind == ActionKind::Interact) {
            interact(world, a, host, deliver, stats);
            continue;
        }
        const auto found = std::ranges::find_if(windows_, [&](const Window& w) {
            return w.player == a.player;
        });
        if (found == windows_.end() || !host.with_player) {
            continue;
        }
        Window&                  w = *found;
        gameplay::VillagerState* v = villager_of(world, w.villager);
        bool                     closed = false;
        (void)host.with_player(a.player, [&](MerchantPlayer& p) {
            if (a.kind == ActionKind::Close || v == nullptr) {
                close(world, w, &p);
                closed = true;
                return;
            }
            if (a.kind == ActionKind::Select) {
                select(*v, w, a.index, p);
            } else {
                click(world, *v, w, a.click, p, host, stats);
            }
        });
        if (closed) {
            windows_.erase(found);
        }
    }
    working_.clear();

    // A screen whose villager went, whose player went, or whose player walked
    // away, closes.
    for (usize i = windows_.size(); i-- > 0;) {
        Window&                    w      = windows_[i];
        const entity::EntityHandle handle = world.find(w.villager);
        const entity::EntityState* state  = world.state(handle);
        bool                       stay   = false;
        const bool present = host.with_player && host.with_player(w.player, [&](MerchantPlayer& p) {
            if (state != nullptr && !state->removed && state->health > 0.0F &&
                box_distance_sq(*state, p.eyes) < kLeaveDistance * kLeaveDistance) {
                stay = true;
                return;
            }
            close(world, w, &p);
            if (p.send) {
                p.send(net::clientbound::kCloseContainer,
                       net::encode_close_container(kMerchantWindow));
            }
        });
        if (!present) {
            close(world, w, nullptr);
        }
        if (!stay) {
            windows_.erase(windows_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    (void)deliver;
    return stats;
}

void Villagers::interact(entity::EntityWorld& world, const Action& a, const VillagerHost& host,
                         const VillagerDeliver& deliver, VillagerStats& stats) {
    const entity::EntityHandle handle = world.find(a.entity);
    const entity::EntityState* state  = world.state(handle);
    gameplay::Mob*             mob    = mob_of(world, handle);
    if (state == nullptr || state->removed || mob == nullptr || !host.with_player) {
        return;
    }
    gameplay::VillagerState& v = mob->mutable_brain().villager;
    if (!v.active) {
        return;  // not a villager: husbandry's click, not ours
    }
    // The client sends the main hand first and stops at the first answer; the
    // off hand's click only ever arrives when the main hand did nothing, and a
    // villager always answers the main hand.
    if (a.hand != net::Hand::Main) {
        return;
    }
    const bool found = host.with_player(a.player, [&](MerchantPlayer& p) {
        if (box_distance_sq(*state, p.eyes) >= kReach * kReach) {
            OV_LOG_DEBUG("villager {}: out of reach of player {} ({:.1f} blocks)",
                         state->network_id, a.player, std::sqrt(box_distance_sq(*state, p.eyes)));
            return;
        }
        // Vanilla's order: trading, asleep or sneaking — nothing; a baby —
        // shake; no offers — shake; else trade.
        if (v.trading_player >= 0 || v.sleeping || a.sneaking) {
            OV_LOG_DEBUG("villager {}: busy (trading with {}, asleep {}, sneaking {})",
                         state->network_id, v.trading_player, v.sleeping, a.sneaking);
            return;
        }
        const auto shake = [&] {
            v.unhappy = gameplay::kUnhappyTicks;
            net::MetadataWriter fields;
            fields.varint_value(villager_metadata::kUnhappy, gameplay::kUnhappyTicks);
            deliver(net::clientbound::kEntityMetadata,
                    net::encode_entity_metadata(state->network_id, fields.take()));
            ++stats.refused;
        };
        if (mob->brain().animal.baby()) {
            shake();
            return;
        }
        gameplay::ensure_offers(v);
        if (v.offers.empty()) {
            shake();
            return;
        }
        v.trading_player = a.player;
        v.trading_with   = p.eyes;
        {
            const std::scoped_lock lock{mutex_};
            open_[a.player] = kMerchantWindow;
        }
        // ── brains ── priced for this player until the screen closes
        apply_special_prices(v, p.uuid, p.hero_amplifier);
        Window w;
        w.player      = a.player;
        w.villager    = state->network_id;
        w.player_uuid = p.uuid;  // ── brains ──
        // Captured order: Open Screen, Set Container Content, Merchant Offers.
        if (p.send) {
            p.send(net::clientbound::kOpenScreen,
                   net::encode_open_screen(kMerchantWindow, merchant_menu_,
                                           merchant_title(v, state->uuid)));
            send_contents(w, p);
            p.send(kMerchantOffers,
                   encode_merchant_offers(*registries_, item_registry_, kMerchantWindow, v));
        }
        windows_.push_back(std::move(w));
        ++stats.opened;
        OV_LOG_DEBUG("villager {}: trading with player {}, {} offers", state->network_id,
                     a.player, v.offers.size());
    });
    if (!found) {
        OV_LOG_DEBUG("villager {}: player {} is not connected", state->network_id, a.player);
    }
}

void Villagers::send_contents(Window& w, MerchantPlayer& p) const {
    if (!p.send || p.carried == nullptr) {
        return;
    }
    std::array<net::ItemStack, static_cast<usize>(kMerchantSlots)> all{};
    for (usize i = 0; i < 3; ++i) {
        all[i] = w.slots[i];
    }
    for (usize i = 0; i < 36 && 9 + i < p.inventory.size(); ++i) {
        all[3 + i] = p.inventory[9 + i];
    }
    ++w.state_id;
    p.send(net::clientbound::kContainerContent,
           net::encode_container_content(kMerchantWindow, w.state_id, all, *p.carried));
}

void Villagers::refresh(gameplay::VillagerState& v, Window& w) const {
    const net::ItemStack* a = &w.slots[0];
    const net::ItemStack* b = &w.slots[1];
    static const net::ItemStack kNothing{};
    if (a->empty()) {
        a = b;
        b = &kNothing;
    }
    w.active   = -1;
    w.slots[2] = {};
    if (a->empty() || v.offers.empty()) {
        return;
    }
    const auto size   = static_cast<i32>(v.offers.size());
    const auto recipe = [&](const net::ItemStack& x, const net::ItemStack& y) -> i32 {
        // A hint above 0 asks for that offer only; 0 means "any".
        if (w.hint > 0 && w.hint < size) {
            return pays(v.offers[static_cast<usize>(w.hint)], x, y) ? w.hint : -1;
        }
        for (i32 i = 0; i < size; ++i) {
            if (pays(v.offers[static_cast<usize>(i)], x, y)) {
                return i;
            }
        }
        return -1;
    };
    i32 found = recipe(*a, *b);
    if ((found < 0 || v.offers[static_cast<usize>(found)].out_of_stock()) && !b->empty()) {
        found = recipe(*b, *a);
    }
    if (found >= 0 && !v.offers[static_cast<usize>(found)].out_of_stock()) {
        w.active   = found;
        w.slots[2] = trade_stack(*registries_, item_registry_,
                                 v.offers[static_cast<usize>(found)].result);
    }
}

void Villagers::select(gameplay::VillagerState& v, Window& w, i32 index, MerchantPlayer& p) {
    if (index < 0 || index >= static_cast<i32>(v.offers.size())) {
        return;
    }
    const std::span<net::ItemStack> inventory = p.inventory.subspan(9, 36);
    // What is in the payment slots goes back first, from the end of the
    // hotbar; if it does not all fit, nothing else happens.
    bool cleared = true;
    for (usize i = 0; i < 2; ++i) {
        net::ItemStack& slot = w.slots[i];
        if (!slot.empty()) {
            slot    = deposit(inventory, slot, stack_limit(slot.item_id), true);
            cleared = cleared && slot.empty();
        }
    }
    if (cleared) {
        const gameplay::MerchantOffer& offer = v.offers[static_cast<usize>(index)];
        // Then each cost is fetched from the inventory, main slots first, as
        // many as a stack holds — measured: 64 paper for a 24-paper trade.
        const auto fetch = [&](usize payment, const gameplay::TradeItem& cost) {
            if (cost.empty()) {
                return;
            }
            const i32 id    = item_id(cost.item);
            const i32 limit = id >= 0 ? stack_limit(id) : 64;
            for (net::ItemStack& from : inventory) {
                net::ItemStack& into = w.slots[payment];
                if (from.empty() || from.item_id != id || !from.nbt.empty()) {
                    continue;
                }
                const i32 have  = into.empty() ? 0 : into.count;
                const i32 moved = std::min<i32>(limit - have, from.count);
                if (moved <= 0) {
                    break;
                }
                if (into.empty()) {
                    into       = from;
                    into.count = 0;
                }
                into.count = static_cast<i8>(have + moved);
                from.count = static_cast<i8>(from.count - moved);
                normalise(from);
                if (into.count >= limit) {
                    break;
                }
            }
        };
        fetch(0, offer.cost_a);
        fetch(1, offer.cost_b);
    }
    w.hint = index;
    refresh(v, w);
    send_contents(w, p);
}

bool Villagers::take(entity::EntityWorld& world, gameplay::VillagerState& v, Window& w,
                     const VillagerHost& host, VillagerStats& stats) {
    if (w.active < 0 || w.active >= static_cast<i32>(v.offers.size())) {
        return false;
    }
    gameplay::MerchantOffer& offer = v.offers[static_cast<usize>(w.active)];
    net::ItemStack&          a     = w.slots[0];
    net::ItemStack&          b     = w.slots[1];
    const auto               shrink = [](net::ItemStack& s, i32 n) {
        s.count = static_cast<i8>(s.count - n);
        normalise(s);
    };
    const i32 price = cost_a(offer);
    const i32 extra = offer.cost_b.empty() ? 0 : offer.cost_b.count;
    if (pays(offer, a, b)) {
        shrink(a, price);
        if (extra > 0) {
            shrink(b, extra);
        }
    } else if (pays(offer, b, a)) {
        shrink(b, price);
        if (extra > 0) {
            shrink(a, extra);
        }
    } else {
        return false;
    }
    const gameplay::TradeOutcome out = gameplay::record_trade(v, offer, v.random);
    // ── brains ── measured: trading +2 a trade (2, then 4)
    v.gossips.add_event(w.player_uuid, gameplay::brain::ReputationEvent::Trade);
    if (out.orb > 0 && host.spawn_orb) {
        if (const entity::EntityState* s = world.state(world.find(w.villager))) {
            // Measured: one orb per trade, at the villager, half a block up.
            host.spawn_orb(s->position + Vec3d{0.0, 0.5, 0.0}, out.orb);
        }
    }
    ++stats.trades;
    return true;
}

void Villagers::click(entity::EntityWorld& world, gameplay::VillagerState& v, Window& w,
                      const net::ContainerClick& c, MerchantPlayer& p, const VillagerHost& host,
                      VillagerStats& stats) {
    if (p.carried == nullptr) {
        return;
    }
    net::ItemStack& carried = *p.carried;
    const std::span<net::ItemStack> inventory = p.inventory.subspan(9, 36);
    const auto ref = [&](i16 s) -> net::ItemStack* {
        if (s >= 0 && s < 3) {
            return &w.slots[static_cast<usize>(s)];
        }
        if (s >= 3 && s < kMerchantSlots) {
            return &inventory[static_cast<usize>(s - 3)];
        }
        return nullptr;
    };
    const auto limit_of = [&](const net::ItemStack& s) { return stack_limit(s.item_id); };

    switch (c.mode) {
        case 0: {
            if (c.slot == -999) {
                if (!carried.empty() && p.drop) {
                    net::ItemStack thrown = carried;
                    if (c.button == 1) {
                        thrown.count = 1;
                    }
                    carried.count = static_cast<i8>(carried.count - thrown.count);
                    normalise(carried);
                    p.drop(thrown);
                }
                break;
            }
            if (c.slot == 2) {
                const net::ItemStack out = w.slots[2];
                if (out.empty()) {
                    break;
                }
                if (carried.empty()) {
                    if (take(world, v, w, host, stats)) {
                        carried = out;
                    }
                } else if (same_kind(carried, out) && carried.count + out.count <= limit_of(out)) {
                    if (take(world, v, w, host, stats)) {
                        carried.count = static_cast<i8>(carried.count + out.count);
                    }
                }
                break;
            }
            net::ItemStack* slot = ref(c.slot);
            if (slot == nullptr) {
                break;
            }
            if (c.button == 0) {
                if (carried.empty() || slot->empty() || !same_kind(*slot, carried)) {
                    if (slot->empty() && !carried.empty() && carried.count > limit_of(carried)) {
                        break;
                    }
                    std::swap(carried, *slot);
                } else {
                    const i32 moved = std::min<i32>(limit_of(*slot) - slot->count, carried.count);
                    slot->count     = static_cast<i8>(slot->count + moved);
                    carried.count   = static_cast<i8>(carried.count - moved);
                    normalise(carried);
                }
            } else if (c.button == 1) {
                if (carried.empty()) {
                    if (!slot->empty()) {
                        const i8 half = static_cast<i8>((slot->count + 1) / 2);
                        carried       = *slot;
                        carried.count = half;
                        slot->count   = static_cast<i8>(slot->count - half);
                        normalise(*slot);
                    }
                } else if (slot->empty()) {
                    *slot         = carried;
                    slot->count   = 1;
                    carried.count = static_cast<i8>(carried.count - 1);
                    normalise(carried);
                } else if (same_kind(*slot, carried)) {
                    if (slot->count < limit_of(*slot)) {
                        slot->count   = static_cast<i8>(slot->count + 1);
                        carried.count = static_cast<i8>(carried.count - 1);
                        normalise(carried);
                    }
                } else {
                    std::swap(carried, *slot);
                }
            }
            break;
        }
        case 1: {
            if (c.slot == 2) {
                // Shift on the result trades again and again, while the result
                // stays the same item and the inventory has room for it.
                const net::ItemStack first = w.slots[2];
                for (int guard = 0; guard < 64; ++guard) {
                    const net::ItemStack out = w.slots[2];
                    if (out.empty() || !same_kind(out, first) ||
                        !fits(inventory, out, limit_of(out)) || !take(world, v, w, host, stats)) {
                        break;
                    }
                    (void)deposit(inventory, out, limit_of(out), true);
                    refresh(v, w);
                }
                break;
            }
            net::ItemStack* slot = ref(c.slot);
            if (slot == nullptr || slot->empty()) {
                break;
            }
            if (c.slot < 2) {
                *slot = deposit(inventory, *slot, limit_of(*slot), false);
            } else if (c.slot < 30) {
                *slot = deposit(inventory.subspan(27, 9), *slot, limit_of(*slot), false);
            } else {
                *slot = deposit(inventory.subspan(0, 27), *slot, limit_of(*slot), false);
            }
            break;
        }
        case 2: {
            if (c.button < 0 || c.button >= 9) {
                OV_LOG_DEBUG("merchant: swap with button {} is not carried out", c.button);
                break;
            }
            net::ItemStack& hotbar = inventory[static_cast<usize>(27 + c.button)];
            if (c.slot == 2) {
                const net::ItemStack out = w.slots[2];
                if (!out.empty() && hotbar.empty() && take(world, v, w, host, stats)) {
                    hotbar = out;
                }
                break;
            }
            if (net::ItemStack* slot = ref(c.slot); slot != nullptr && slot != &hotbar) {
                std::swap(*slot, hotbar);
            }
            break;
        }
        case 4: {
            if (c.slot == 2) {
                const net::ItemStack out = w.slots[2];
                if (!out.empty() && take(world, v, w, host, stats) && p.drop) {
                    p.drop(out);
                }
                break;
            }
            if (net::ItemStack* slot = ref(c.slot); slot != nullptr && !slot->empty()) {
                net::ItemStack thrown = *slot;
                thrown.count          = c.button == 1 ? slot->count : i8{1};
                slot->count           = static_cast<i8>(slot->count - thrown.count);
                normalise(*slot);
                if (p.drop) {
                    p.drop(thrown);
                }
            }
            break;
        }
        default:
            // Clone (3), drag (5), double-click (6): named, not carried out;
            // the full resend below puts the client back where it was.
            OV_LOG_DEBUG("merchant: click mode {} is not carried out", c.mode);
            break;
    }
    refresh(v, w);
    send_contents(w, p);
}

void Villagers::close(entity::EntityWorld& world, Window& w, MerchantPlayer* p) {
    if (gameplay::VillagerState* v = villager_of(world, w.villager)) {
        v->trading_player = -1;
        // ── brains ── the prices go back to their own (measured: 0 after the
        // close on 17 villagers of 18; the 18th was read before its close)
        for (gameplay::MerchantOffer& offer : v->offers) {
            offer.special_price = 0;
        }
    }
    for (usize i = 0; i < 2; ++i) {
        net::ItemStack& slot = w.slots[i];
        if (slot.empty()) {
            continue;
        }
        if (p != nullptr) {
            slot = deposit(p->inventory.subspan(9, 36), slot, stack_limit(slot.item_id), false);
            if (!slot.empty() && p->drop) {
                p->drop(slot);
            }
        }
        // A player who is gone loses what was in the slots. Named: vanilla
        // drops it at their feet, and there are no feet to drop it at.
        slot = {};
    }
    w.slots[2] = {};
    if (p != nullptr && p->carried != nullptr && !p->carried->empty()) {
        *p->carried = deposit(p->inventory.subspan(9, 36), *p->carried,
                              stack_limit(p->carried->item_id), false);
        if (!p->carried->empty() && p->drop) {
            p->drop(*p->carried);
        }
        *p->carried = {};
    }
    {
        const std::scoped_lock lock{mutex_};
        open_.erase(w.player);
    }
    if (p != nullptr && p->send && p->carried != nullptr) {
        // The player's own window, whole: the payments came back into it.
        p->send(net::clientbound::kContainerContent,
                net::encode_container_content(0, 0, p->inventory, *p->carried));
    }
}

void Villagers::send_data(const entity::EntityState& state, const gameplay::VillagerState& v,
                          const VillagerDeliver& deliver) const {
    net::MetadataWriter fields;
    if (!v.wandering) {  // ── brains ── a trader has no VillagerData
        fields.villager_data_value(villager_metadata::kData, static_cast<i32>(v.type),
                                   static_cast<i32>(v.profession), v.level);
    }
    if (v.sleeping && v.claims.home) {
        const BlockPos bed = *v.claims.home;
        fields.pose_value(villager_metadata::kPose, villager_metadata::kPoseSleeping);
        fields.optional_block_pos_value(villager_metadata::kSleepingPos,
                                        net::WirePosition{bed.x, bed.y, bed.z});
    } else {
        fields.pose_value(villager_metadata::kPose, villager_metadata::kPoseStanding);
        fields.optional_block_pos_value(villager_metadata::kSleepingPos, std::nullopt);
    }
    deliver(net::clientbound::kEntityMetadata,
            net::encode_entity_metadata(state.network_id, fields.take()));
}

VillagerStats Villagers::after_entity_tick(entity::EntityWorld& world,
                                           const VillagerDeliver& deliver) {
    VillagerStats stats;
    for (const entity::EntityHandle handle : world.handles()) {
        gameplay::MobBrain*        brain = gameplay::mob_brain_of(world, handle);
        const entity::EntityState* state = world.state(handle);
        if (brain == nullptr || state == nullptr || !brain->villager.active) {
            continue;
        }
        // A hit: the health went down since the last tick.
        const auto [health, first] = last_health_.try_emplace(state->network_id, state->health);
        if (!first && state->health < health->second) {
            brain->villager.hurt_ticks =
                std::max(brain->villager.hurt_ticks, gameplay::kHurtPanicTicks);
        }
        health->second = state->health;

        const auto [it, fresh] = sent_revision_.try_emplace(state->network_id,
                                                            brain->villager.revision);
        if (fresh) {
            continue;  // announced by its spawn, with these very fields
        }
        if (it->second != brain->villager.revision) {
            it->second = brain->villager.revision;
            send_data(*state, brain->villager, deliver);
            ++stats.metadata;
        }
    }
    for (const i32 gone : world.removed_ids()) {
        sent_revision_.erase(gone);
        last_health_.erase(gone);
    }
    return stats;
}

void Villagers::spawn_metadata(entity::EntityWorld& world, const entity::EntityState& state,
                               net::MetadataWriter& fields) const {
    const gameplay::MobBrain* brain = gameplay::mob_brain_of(world, world.find(state.network_id));
    if (brain == nullptr || !brain->villager.active) {
        return;
    }
    const gameplay::VillagerState& v = brain->villager;
    if (v.wandering) {  // ── brains ── a trader has no VillagerData
        return;
    }
    // Captured: a villager's spawn carries index 18 even at plains / none / 1.
    fields.villager_data_value(villager_metadata::kData, static_cast<i32>(v.type),
                               static_cast<i32>(v.profession), v.level);
    if (v.sleeping && v.claims.home) {
        fields.pose_value(villager_metadata::kPose, villager_metadata::kPoseSleeping);
        fields.optional_block_pos_value(
            villager_metadata::kSleepingPos,
            net::WirePosition{v.claims.home->x, v.claims.home->y, v.claims.home->z});
    }
}

}  // namespace ov::server
