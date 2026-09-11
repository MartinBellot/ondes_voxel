// ── mobs-3 ── See entity_storage.hpp.
#define OV_LOG_CATEGORY "server"

#include "entity_storage.hpp"

#include "merchant_session.hpp"  // trade_stack

#include "ov/base/log.hpp"
#include "ov/gameplay/brewing.hpp"
#include "ov/gameplay/enchanting.hpp"
#include "ov/gameplay/mob_logic.hpp"
#include "ov/gameplay/villager.hpp"
#include "ov/nbt/binary.hpp"
#include "ov/nbt/region_writer.hpp"
#include "ov/world/chunk_storage.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <system_error>

namespace ov::server {
namespace {

[[nodiscard]] i64 key_of(ChunkPos pos) noexcept {
    return (static_cast<i64>(pos.x) << 32) | static_cast<i64>(static_cast<u32>(pos.z));
}

[[nodiscard]] ChunkPos pos_of(i64 key) noexcept {
    return ChunkPos{static_cast<i32>(key >> 32), static_cast<i32>(static_cast<u32>(key & 0xFFFFFFFF))};
}

[[nodiscard]] ChunkPos chunk_of(const Vec3d& at) noexcept {
    return ChunkPos{static_cast<i32>(std::floor(at.x)) >> 4, static_cast<i32>(std::floor(at.z)) >> 4};
}

[[nodiscard]] nbt::Tag doubles(f64 a, f64 b, f64 c) {
    nbt::Tag list = nbt::Tag::make_list(nbt::TagType::Double);
    (void)list.push(nbt::Tag{a});
    (void)list.push(nbt::Tag{b});
    (void)list.push(nbt::Tag{c});
    return list;
}

[[nodiscard]] nbt::Tag floats(std::initializer_list<f32> values) {
    nbt::Tag list = nbt::Tag::make_list(nbt::TagType::Float);
    for (const f32 value : values) {
        (void)list.push(nbt::Tag{value});
    }
    return list;
}

[[nodiscard]] nbt::Tag empty_compounds(usize count) {
    nbt::Tag list = nbt::Tag::make_list(nbt::TagType::Compound);
    for (usize i = 0; i < count; ++i) {
        (void)list.push(nbt::Tag::make_compound());
    }
    return list;
}

[[nodiscard]] nbt::Tag uuid_tag(const net::Uuid& uuid) {
    return nbt::Tag{nbt::Tag::IntArray{
        static_cast<i32>(static_cast<u32>(uuid.most_significant >> 32U)),
        static_cast<i32>(static_cast<u32>(uuid.most_significant & 0xFFFFFFFFU)),
        static_cast<i32>(static_cast<u32>(uuid.least_significant >> 32U)),
        static_cast<i32>(static_cast<u32>(uuid.least_significant & 0xFFFFFFFFU))}};
}

[[nodiscard]] net::Uuid uuid_from(const nbt::Tag* tag) {
    net::Uuid uuid;
    if (tag == nullptr) {
        return uuid;
    }
    const auto* ints = tag->get_if<nbt::Tag::IntArray>();
    if (ints == nullptr || ints->size() != 4) {
        return uuid;
    }
    const auto word = [&](usize i) { return static_cast<u64>(static_cast<u32>((*ints)[i])); };
    uuid.most_significant  = (word(0) << 32U) | word(1);
    uuid.least_significant = (word(2) << 32U) | word(3);
    return uuid;
}

[[nodiscard]] f64 element(const nbt::Tag* list, usize index, f64 fallback) {
    if (list == nullptr || list->list() == nullptr || index >= list->list()->size()) {
        return fallback;
    }
    return (*list->list())[index].as_f64(fallback);
}

/// Put `value` under `name` only when the compound does not already carry the
/// name: a default for a mob this server made, never an overwrite of one
/// that came from disk.
void default_to(nbt::Tag& compound, std::string_view name, nbt::Tag value) {
    if (!compound.contains(name)) {
        (void)compound.put(std::string{name}, std::move(value));
    }
}

[[nodiscard]] std::string namespaced(std::string_view name) {
    return name.starts_with("minecraft:") ? std::string{name} : "minecraft:" + std::string{name};
}

template<typename Lookup>
[[nodiscard]] auto from_either(std::string_view name, Lookup lookup) {
    auto found = lookup(name);
    if (!found && name.starts_with("minecraft:")) {
        found = lookup(name.substr(10));
    }
    if (!found && !name.starts_with("minecraft:")) {
        found = lookup(namespaced(name));
    }
    return found;
}

[[nodiscard]] bool is_zombie_family(std::string_view type) noexcept {
    return type == "minecraft:zombie" || type == "minecraft:husk" ||
           type == "minecraft:drowned" || type == "minecraft:zombie_villager";
}

[[nodiscard]] bool is_ageable(std::string_view type) noexcept {
    return gameplay::category_of(type) == gameplay::MobCategory::Creature ||
           type == "minecraft:villager";
}

}  // namespace

EntityStorage::EntityStorage(const registry::Registries& registries,
                             std::filesystem::path directory, bool spawn_mobs)
    : registries_{&registries},
      spawn_mobs_{spawn_mobs},
      types_{registries.find("minecraft:entity_type")},
      items_{registries.find("minecraft:item")},
      directory_{std::move(directory)} {}

bool EntityStorage::is_loaded(ChunkPos chunk) const noexcept {
    return loaded_.contains(key_of(chunk));
}

std::filesystem::path EntityStorage::region_path(i32 region_x, i32 region_z) const {
    return directory_ / fmt::format("r.{}.{}.mca", region_x, region_z);
}

const nbt::RegionFile* EntityStorage::region(i32 region_x, i32 region_z) {
    const auto key = std::pair{region_x, region_z};
    auto       it  = regions_.find(key);
    if (it == regions_.end()) {
        const auto path   = region_path(region_x, region_z);
        auto       opened = nbt::RegionFile::open(path);
        OV_LOG_DEBUG("entities: {} — {}", path.string(),
                     opened ? fmt::format("{} chunks", opened->chunk_count())
                            : std::string{nbt::to_string(opened.error())});
        it = regions_.emplace(key, opened ? std::optional<nbt::RegionFile>{std::move(*opened)}
                                          : std::nullopt)
                 .first;
    }
    return it->second ? &*it->second : nullptr;
}

bool EntityStorage::spawns(std::string_view type) const {
    return gameplay::mob_kind(type) != nullptr ||
           gameplay::category_of(type) != gameplay::MobCategory::Misc;
}

EntityAdopter* EntityStorage::adopter_of(i32 type) const noexcept {
    for (EntityAdopter* adopter : adopters_) {
        if (adopter->owns(type)) {
            return adopter;
        }
    }
    return nullptr;
}

LooseAdopter* EntityStorage::loose_of(std::string_view type) const noexcept {
    for (LooseAdopter* adopter : loose_) {
        if (adopter->owns_type(type)) {
            return adopter;
        }
    }
    return nullptr;
}

bool EntityStorage::saved(const entity::EntityState& state, const EntityStorageHost& host) const {
    if (state.removed || !types_) {
        return false;
    }
    // An adopted entity is not a living one: a minecart has no health. And
    // an adopter's type is saved even where the host calls it transient —
    // the TNT and the arrows, which despawn must leave alone.
    if (adopter_of(state.type) != nullptr) {
        return true;
    }
    if (host.transient && host.transient(state.type)) {
        return false;
    }
    if (host.ignore && host.ignore(state.network_id)) {
        return false;
    }
    if (!spawn_mobs_) {
        return false;
    }
    if (state.health <= 0.0F) {
        return false;
    }
    return spawns(registries_->entry_of(*types_, state.type));
}

std::optional<nbt::Tag> EntityStorage::compound_of(entity::EntityWorld& world,
                                                   entity::EntityHandle handle,
                                                   const MobRecords&     records,
                                                   const EntityStorageHost& host) const {
    const entity::EntityState* state = world.state(handle);
    if (state == nullptr) {
        return std::nullopt;
    }
    if (const EntityAdopter* adopter = adopter_of(state->type)) {
        return adopter->save_entity(world, handle);
    }
    return encode(world, handle, records, host);
}

void EntityStorage::read_before_write(entity::EntityWorld& world, MobRecords& records,
                                      const EntityStorageHost&       host,
                                      const std::unordered_set<i64>* only) {
    std::vector<ChunkPos> unread;
    for (const entity::EntityHandle handle : world.handles()) {
        const entity::EntityState* state = world.state(handle);
        if (state == nullptr || !saved(*state, host)) {
            continue;
        }
        const ChunkPos chunk = chunk_of(state->position);
        const i64      key   = key_of(chunk);
        if (loaded_.contains(key) || (only != nullptr && !only->contains(key)) ||
            std::ranges::find(unread, chunk) != unread.end()) {
            continue;
        }
        unread.push_back(chunk);
    }
    // ── persistence ── and where the loose adopters' entities stand.
    loose_positions_.clear();
    for (const LooseAdopter* adopter : loose_) {
        adopter->positions(loose_positions_);
    }
    for (const Vec3d& at : loose_positions_) {
        const ChunkPos chunk = chunk_of(at);
        const i64      key   = key_of(chunk);
        if (loaded_.contains(key) || (only != nullptr && !only->contains(key)) ||
            std::ranges::find(unread, chunk) != unread.end()) {
            continue;
        }
        unread.push_back(chunk);
    }
    // After the pass: a read spawns, and a spawn invalidates `handles()`.
    for (const ChunkPos chunk : unread) {
        (void)load_chunk(chunk, world, records, host);
    }
}

// ── Villager trades ─────────────────────────────────────────────────────────

namespace {

[[nodiscard]] nbt::Tag item_tag(const registry::Registries& registries,
                                registry::RegistryId items, const gameplay::TradeItem& item) {
    nbt::Tag out = nbt::Tag::make_compound();
    if (item.empty()) {
        (void)out.put("id", nbt::Tag{std::string{"minecraft:air"}});
        (void)out.put("Count", nbt::Tag{i8{0}});
        return out;
    }
    (void)out.put("id", nbt::Tag{std::string{item.item}});
    (void)out.put("Count", nbt::Tag{static_cast<i8>(item.count)});
    const net::ItemStack stack = trade_stack(registries, items, item);
    if (!stack.nbt.empty()) {
        if (auto document = nbt::read(stack.nbt); document && !document->root.empty()) {
            (void)out.put("tag", std::move(document->root));
        }
    }
    return out;
}

[[nodiscard]] gameplay::TradeItem trade_item(const registry::Registries& registries,
                                             registry::RegistryId items, const nbt::Tag* compound) {
    gameplay::TradeItem out;
    if (compound == nullptr) {
        return out;
    }
    const nbt::Tag* id    = compound->find("id");
    const nbt::Tag* count = compound->find("Count");
    if (id == nullptr || count == nullptr || count->as_i64() <= 0 ||
        id->as_string() == "minecraft:air") {
        return out;
    }
    const auto protocol = registries.protocol_id(items, id->as_string());
    if (!protocol) {
        OV_LOG_WARN("entities: a trade names {}, which is not an item — refused",
                    id->as_string());
        return out;
    }
    // The registry's own string: a TradeItem borrows its name.
    out.item  = registries.entry_of(items, static_cast<i32>(*protocol));
    out.count = static_cast<i32>(count->as_i64());
    if (const nbt::Tag* tag = compound->find("tag")) {
        if (tag->contains("StoredEnchantments")) {
            out.enchantments = gameplay::read_enchantments(*tag, true);
            out.stored       = true;
        } else if (tag->contains("Enchantments")) {
            out.enchantments = gameplay::read_enchantments(*tag, false);
        }
        if (const nbt::Tag* potion = tag->find("Potion")) {
            if (const auto known = gameplay::potion_from_name(potion->as_string())) {
                out.potion = gameplay::potion_info(*known).name;
            }
        }
        if (const nbt::Tag* display = tag->find("display")) {
            if (const nbt::Tag* colour = display->find("color")) {
                out.dye_colour = static_cast<i32>(colour->as_i64());
            }
        }
        if (const nbt::Tag* effects = tag->find("Effects");
            effects != nullptr && effects->list() != nullptr && !effects->list()->empty()) {
            const nbt::Tag& first = effects->list()->front();
            if (const nbt::Tag* effect = first.find("EffectId")) {
                out.stew_effect = static_cast<i32>(effect->as_i64());
            }
            if (const nbt::Tag* duration = first.find("EffectDuration")) {
                out.stew_duration = static_cast<i32>(duration->as_i64());
            }
        }
    }
    return out;
}

void put_villager(nbt::Tag& out, const registry::Registries& registries,
                  std::optional<registry::RegistryId> items, const gameplay::VillagerState& v) {
    nbt::Tag data = nbt::Tag::make_compound();
    (void)data.put("profession", nbt::Tag{namespaced(gameplay::profession_name(v.profession))});
    (void)data.put("level", nbt::Tag{v.level});
    (void)data.put("type", nbt::Tag{namespaced(gameplay::villager_type_name(v.type))});
    (void)out.put("VillagerData", std::move(data));
    (void)out.put("Xp", nbt::Tag{v.xp});
    if (v.offers_drawn && items) {
        nbt::Tag recipes = nbt::Tag::make_list(nbt::TagType::Compound);
        for (const gameplay::MerchantOffer& offer : v.offers) {
            nbt::Tag recipe = nbt::Tag::make_compound();
            (void)recipe.put("buy", item_tag(registries, *items, offer.cost_a));
            (void)recipe.put("buyB", item_tag(registries, *items, offer.cost_b));
            (void)recipe.put("sell", item_tag(registries, *items, offer.result));
            (void)recipe.put("uses", nbt::Tag{offer.uses});
            (void)recipe.put("maxUses", nbt::Tag{offer.max_uses});
            (void)recipe.put("xp", nbt::Tag{offer.xp});
            (void)recipe.put("priceMultiplier", nbt::Tag{offer.price_multiplier});
            (void)recipe.put("specialPrice", nbt::Tag{offer.special_price});
            (void)recipe.put("demand", nbt::Tag{offer.demand});
            (void)recipe.put("rewardExp", nbt::Tag::make_bool(offer.reward_exp));
            (void)recipes.push(std::move(recipe));
        }
        nbt::Tag offers = nbt::Tag::make_compound();
        (void)offers.put("Recipes", std::move(recipes));
        (void)out.put("Offers", std::move(offers));
    }
    default_to(out, "Gossips", nbt::Tag::make_list(nbt::TagType::Compound));
}

void read_villager(const nbt::Tag& in, const registry::Registries& registries,
                   std::optional<registry::RegistryId> items, gameplay::VillagerState& v) {
    if (const nbt::Tag* data = in.find("VillagerData")) {
        if (const nbt::Tag* type = data->find("type")) {
            if (const auto known = from_either(type->as_string(), [](std::string_view n) {
                    return gameplay::villager_type_from_name(n);
                })) {
                v.type = *known;
            }
        }
        if (const nbt::Tag* profession = data->find("profession")) {
            if (const auto known = from_either(profession->as_string(), [](std::string_view n) {
                    return gameplay::profession_from_name(n);
                })) {
                v.profession = *known;
            }
        }
        if (const nbt::Tag* level = data->find("level")) {
            v.level = std::clamp(static_cast<i32>(level->as_i64()), 1, 5);
        }
    }
    if (const nbt::Tag* xp = in.find("Xp")) {
        v.xp = static_cast<i32>(xp->as_i64());
    }
    const nbt::Tag* offers = in.find("Offers");
    const nbt::Tag* list   = offers != nullptr ? offers->find("Recipes") : nullptr;
    if (list != nullptr && list->list() != nullptr && items) {
        v.offers.clear();
        for (const nbt::Tag& recipe : *list->list()) {
            gameplay::MerchantOffer offer;
            offer.cost_a = trade_item(registries, *items, recipe.find("buy"));
            offer.cost_b = trade_item(registries, *items, recipe.find("buyB"));
            offer.result = trade_item(registries, *items, recipe.find("sell"));
            if (offer.cost_a.empty() || offer.result.empty()) {
                continue;  // a refused item was named in the log
            }
            const auto number = [&](std::string_view name, i64 fallback) {
                const nbt::Tag* field = recipe.find(name);
                return field != nullptr ? field->as_i64(fallback) : fallback;
            };
            offer.uses          = static_cast<i32>(number("uses", 0));
            offer.max_uses      = static_cast<i32>(number("maxUses", 4));
            offer.xp            = static_cast<i32>(number("xp", 1));
            offer.special_price = static_cast<i32>(number("specialPrice", 0));
            offer.demand        = static_cast<i32>(number("demand", 0));
            offer.reward_exp    = number("rewardExp", 1) != 0;
            if (const nbt::Tag* multiplier = recipe.find("priceMultiplier")) {
                offer.price_multiplier = static_cast<f32>(multiplier->as_f64(0.05));
            }
            v.offers.push_back(std::move(offer));
        }
        v.offers_drawn = true;
    }
    if (const nbt::Tag* restocks = in.find("RestocksToday")) {
        v.restocks_today = static_cast<i32>(restocks->as_i64());
    }
    if (const nbt::Tag* last = in.find("LastRestock")) {
        v.last_restock = last->as_i64();
    }
}

}  // namespace

// ── One mob ─────────────────────────────────────────────────────────────────

nbt::Tag EntityStorage::encode(entity::EntityWorld& world, entity::EntityHandle handle,
                               const MobRecords& records, const EntityStorageHost& host) const {
    const entity::EntityState* state = world.state(handle);
    if (state == nullptr || !types_) {
        return nbt::Tag::make_compound();
    }
    const auto carried = carried_.find(state->network_id);
    nbt::Tag   out = carried != carried_.end() ? carried->second : nbt::Tag::make_compound();
    const std::string_view type = registries_->entry_of(*types_, state->type);

    (void)out.put("id", nbt::Tag{std::string{type}});
    (void)out.put("Pos", doubles(state->position.x, state->position.y, state->position.z));
    (void)out.put("Motion", doubles(state->velocity.x, state->velocity.y, state->velocity.z));
    (void)out.put("Rotation", floats({state->yaw, state->pitch}));
    (void)out.put("UUID", uuid_tag(state->uuid));
    (void)out.put("Health", nbt::Tag{state->health});
    (void)out.put("OnGround", nbt::Tag::make_bool(state->on_ground));
    // What vanilla writes for every mob, when nothing came from disk.
    default_to(out, "FallDistance", nbt::Tag{0.0F});
    default_to(out, "Fire", nbt::Tag{i16{-1}});
    default_to(out, "Air", nbt::Tag{i16{300}});
    default_to(out, "Invulnerable", nbt::Tag::make_bool(false));
    default_to(out, "PortalCooldown", nbt::Tag{i32{0}});
    default_to(out, "AbsorptionAmount", nbt::Tag{0.0F});
    default_to(out, "HurtTime", nbt::Tag{i16{0}});
    default_to(out, "HurtByTimestamp", nbt::Tag{i32{0}});
    default_to(out, "DeathTime", nbt::Tag{i16{0}});
    default_to(out, "FallFlying", nbt::Tag::make_bool(false));
    default_to(out, "CanPickUpLoot", nbt::Tag::make_bool(false));
    default_to(out, "LeftHanded", nbt::Tag::make_bool(false));
    default_to(out, "HandItems", empty_compounds(2));
    default_to(out, "ArmorItems", empty_compounds(4));
    default_to(out, "HandDropChances", floats({0.085F, 0.085F}));
    default_to(out, "ArmorDropChances", floats({0.085F, 0.085F, 0.085F, 0.085F}));
    if (!out.contains("Brain")) {
        nbt::Tag brain = nbt::Tag::make_compound();
        (void)brain.put("memories", nbt::Tag::make_compound());
        (void)out.put("Brain", std::move(brain));
    }

    const MobRecord* record = records.find(state->network_id);
    (void)out.put("PersistenceRequired",
                  nbt::Tag::make_bool(record != nullptr && record->persistence_required));
    if (record != nullptr && !record->custom_name.empty()) {
        (void)out.put("CustomName", nbt::Tag{record->custom_name});
    } else {
        (void)out.erase("CustomName");
    }

    // ── Species ─────────────────────────────────────────────────────────────
    if (const auto* mob = dynamic_cast<const gameplay::Mob*>(world.logic(handle))) {
        const gameplay::MobBrain& brain = mob->brain();
        if (is_ageable(type)) {
            (void)out.put("Age", nbt::Tag{brain.animal.age});
            default_to(out, "ForcedAge", nbt::Tag{i32{0}});
            if (type != "minecraft:villager") {
                (void)out.put("InLove", nbt::Tag{brain.animal.love});
            }
        }
        if (type == "minecraft:sheep") {
            (void)out.put("Color", nbt::Tag{brain.animal.colour});
            (void)out.put("Sheared", nbt::Tag::make_bool(brain.animal.sheared));
        } else if (type == "minecraft:pig") {
            (void)out.put("Saddle", nbt::Tag::make_bool(brain.animal.saddled));
        } else if (type == "minecraft:chicken") {
            (void)out.put("EggLayTime", nbt::Tag{brain.animal.egg_time});
            default_to(out, "IsChickenJockey", nbt::Tag::make_bool(false));
        }
        if (brain.villager.active) {
            put_villager(out, *registries_, items_, brain.villager);
            (void)out.put("RestocksToday", nbt::Tag{brain.villager.restocks_today});
            (void)out.put("LastRestock", nbt::Tag{std::max<i64>(brain.villager.last_restock, 0)});
            default_to(out, "Inventory", nbt::Tag::make_list(nbt::TagType::Compound));
            default_to(out, "FoodLevel", nbt::Tag{i8{0}});
            default_to(out, "LastGossipDecay", nbt::Tag{i64{0}});
        }
    }
    if (is_zombie_family(type)) {
        default_to(out, "IsBaby", nbt::Tag::make_bool(false));
        default_to(out, "CanBreakDoors", nbt::Tag::make_bool(false));
        default_to(out, "DrownedConversionTime", nbt::Tag{i32{-1}});
        default_to(out, "InWaterTime", nbt::Tag{i32{-1}});
    }
    if (type == "minecraft:zombie_villager") {
        if (host.zombie_villager) {
            if (const gameplay::VillagerState* kept = host.zombie_villager(state->network_id)) {
                put_villager(out, *registries_, items_, *kept);
            }
        }
        (void)out.put("ConversionTime",
                      nbt::Tag{host.conversion_time ? host.conversion_time(state->network_id)
                                                    : i32{-1}});
    } else if (type == "minecraft:slime" && host.slime_size) {
        (void)out.put("Size", nbt::Tag{std::max(host.slime_size(state->network_id), 1) - 1});
        default_to(out, "wasOnGround", nbt::Tag::make_bool(state->on_ground));
    } else if (type == "minecraft:creeper") {
        (void)out.put("powered", nbt::Tag::make_bool(host.creeper_powered &&
                                                     host.creeper_powered(state->network_id)));
        default_to(out, "Fuse", nbt::Tag{i16{30}});
        default_to(out, "ExplosionRadius", nbt::Tag{i8{3}});
        default_to(out, "ignited", nbt::Tag::make_bool(false));
    }
    if (host.write_extra) {  // ── persistence ──
        host.write_extra(*state, out);
    }
    return out;
}

std::optional<entity::EntityHandle> EntityStorage::decode(const nbt::Tag& compound,
                                                          entity::EntityWorld& world,
                                                          MobRecords& records,
                                                          const EntityStorageHost& host) {
    const nbt::Tag* id  = compound.find("id");
    const nbt::Tag* pos = compound.find("Pos");
    if (id == nullptr || pos == nullptr || pos->list() == nullptr || pos->list()->size() != 3) {
        return std::nullopt;
    }
    const std::string_view type = id->as_string();
    if (!spawn_mobs_ || !spawns(type)) {
        return std::nullopt;
    }
    const Vec3d at{element(pos, 0, 0.0), element(pos, 1, 0.0), element(pos, 2, 0.0)};
    const auto  spawned = world.spawn(type, at, uuid_from(compound.find("UUID")));
    if (!spawned) {
        OV_LOG_WARN("entities: cannot spawn a {} read from disk: {}", type,
                    entity::to_string(spawned.error()));
        return std::nullopt;
    }
    const entity::EntityHandle handle = *spawned;
    entity::EntityState*       state  = world.mutable_state(handle);
    const nbt::Tag*            motion = compound.find("Motion");
    state->velocity = Vec3d{element(motion, 0, 0.0), element(motion, 1, 0.0), element(motion, 2, 0.0)};
    const nbt::Tag* rotation = compound.find("Rotation");
    state->yaw               = static_cast<f32>(element(rotation, 0, 0.0));
    state->pitch             = static_cast<f32>(element(rotation, 1, 0.0));
    state->head_yaw          = state->yaw;
    if (const nbt::Tag* ground = compound.find("OnGround")) {
        state->on_ground = ground->as_bool();
    }
    state->broadcast_position = state->position;
    state->broadcast_valid    = true;
    if (host.attach) {
        host.attach(handle, type);
    }
    // The type's own maximum, then the stored health within it: a vanilla
    // slime summoned with a size keeps `Health: 20` over a maximum of 4.
    if (const nbt::Tag* health = compound.find("Health")) {
        state->health = std::clamp(static_cast<f32>(health->as_f64()), 0.0F, state->max_health);
    }

    MobRecord& record          = records.at(state->network_id);
    record.persistence_required = compound.contains("PersistenceRequired") &&
                                  compound.find("PersistenceRequired")->as_bool();
    if (const nbt::Tag* name = compound.find("CustomName")) {
        record.custom_name = std::string{name->as_string()};
    }

    if (auto* mob = dynamic_cast<gameplay::Mob*>(world.logic(handle))) {
        gameplay::MobBrain& brain = mob->mutable_brain();
        if (const nbt::Tag* age = compound.find("Age"); age != nullptr && age->as_i64() != 0) {
            mob->set_age(*state, static_cast<i32>(age->as_i64()));
        }
        if (const nbt::Tag* love = compound.find("InLove")) {
            brain.animal.love = static_cast<i32>(love->as_i64());
        }
        if (const nbt::Tag* colour = compound.find("Color")) {
            brain.animal.colour = static_cast<i8>(colour->as_i64());
        }
        if (const nbt::Tag* sheared = compound.find("Sheared")) {
            brain.animal.sheared = sheared->as_bool();
        }
        if (const nbt::Tag* saddle = compound.find("Saddle")) {
            brain.animal.saddled = saddle->as_bool();
        }
        if (const nbt::Tag* egg = compound.find("EggLayTime")) {
            brain.animal.egg_time = static_cast<i32>(egg->as_i64());
        }
        if (brain.villager.active) {
            read_villager(compound, *registries_, items_, brain.villager);
            ++brain.villager.revision;
        }
    }
    if (type == "minecraft:zombie_villager") {
        if (host.zombie_villager) {
            if (gameplay::VillagerState* kept = host.zombie_villager(state->network_id)) {
                read_villager(compound, *registries_, items_, *kept);
            }
        }
        if (const nbt::Tag* conversion = compound.find("ConversionTime");
            conversion != nullptr && host.set_conversion_time) {
            host.set_conversion_time(state->network_id, static_cast<i32>(conversion->as_i64()));
        }
    } else if (type == "minecraft:slime" && host.set_slime_size) {
        const nbt::Tag* size = compound.find("Size");
        host.set_slime_size(*state, static_cast<i32>(size != nullptr ? size->as_i64() : 0) + 1);
    } else if (type == "minecraft:creeper") {
        if (const nbt::Tag* powered = compound.find("powered");
            powered != nullptr && powered->as_bool() && host.charge_creeper) {
            host.charge_creeper(state->network_id);
        }
    }
    if (host.read_extra) {  // ── persistence ──
        host.read_extra(*state, compound);
    }
    carried_[state->network_id] = compound;
    if (host.announce) {
        host.announce(*state);
    }
    return handle;
}

// ── Chunks ──────────────────────────────────────────────────────────────────

EntityStorageStats EntityStorage::load_chunk(ChunkPos chunk, entity::EntityWorld& world,
                                             MobRecords& records, const EntityStorageHost& host) {
    EntityStorageStats stats;
    const i64          key = key_of(chunk);
    if (!loaded_.insert(key).second) {
        return stats;
    }
    const nbt::RegionFile* file = region(chunk.x >> 5, chunk.z >> 5);
    const auto local_x = static_cast<u32>(chunk.x & 31);
    const auto local_z = static_cast<u32>(chunk.z & 31);
    if (file == nullptr || !file->has_chunk(local_x, local_z)) {
        return stats;
    }
    const auto document = file->read_chunk(local_x, local_z);
    if (!document) {
        OV_LOG_WARN("entities: chunk {},{} unreadable: {}", chunk.x, chunk.z,
                    nbt::to_string(document.error()));
        ++stats.refused;
        return stats;
    }
    const nbt::Tag* version = document->root.find("DataVersion");
    if (version == nullptr || version->as_i64() != world::kDataVersion1201) {
        OV_LOG_WARN("entities: chunk {},{} is DataVersion {}, not 1.20.1's {} — refused",
                    chunk.x, chunk.z, version != nullptr ? version->as_i64() : -1,
                    world::kDataVersion1201);
        ++stats.refused;
        return stats;
    }
    ++stats.chunks;
    const nbt::Tag* entities = document->root.find("Entities");
    if (entities == nullptr || entities->list() == nullptr) {
        return stats;
    }
    if (!entities->list()->empty()) {
        on_disk_.insert(key);
    }
    for (const nbt::Tag& compound : *entities->list()) {
        // A type an adopter runs goes to it, and to nothing else.
        const nbt::Tag* id = compound.find("id");
        // ── persistence ── the entities kept outside the entity world
        if (LooseAdopter* loose = id != nullptr ? loose_of(namespaced(id->as_string())) : nullptr) {
            if (loose->adopt_saved(compound)) {
                ++stats.entities;
                ++stats.adopted;
            } else {
                OV_LOG_WARN("entities: a {} in chunk {},{} refused — carried through",
                            id->as_string(), chunk.x, chunk.z);
                foreign_[key].push_back(compound);
                ++stats.carried;
            }
            continue;
        }
        const auto type = id != nullptr && types_
                              ? registries_->protocol_id(*types_, namespaced(id->as_string()))
                              : std::nullopt;
        if (EntityAdopter* adopter = type ? adopter_of(static_cast<i32>(*type)) : nullptr) {
            if (const auto handle = adopter->adopt_saved(world, compound)) {
                ++stats.entities;
                ++stats.adopted;
                if (const entity::EntityState* state = world.state(*handle);
                    state != nullptr && host.announce) {
                    host.announce(*state);
                }
            } else {
                OV_LOG_WARN("entities: a {} in chunk {},{} refused — carried through",
                            id->as_string(), chunk.x, chunk.z);
                foreign_[key].push_back(compound);
                ++stats.carried;
            }
            continue;
        }
        if (decode(compound, world, records, host)) {
            ++stats.entities;
        } else if (compound.contains("id")) {
            foreign_[key].push_back(compound);
            ++stats.carried;
        } else {
            ++stats.refused;
        }
    }
    return stats;
}

void EntityStorage::write(const std::map<i64, std::vector<nbt::Tag>>& chunks) {
    std::map<std::pair<i32, i32>, std::vector<i64>> by_region;
    for (const auto& [key, list] : chunks) {
        const ChunkPos chunk = pos_of(key);
        by_region[{chunk.x >> 5, chunk.z >> 5}].push_back(key);
    }
    std::error_code error;
    std::filesystem::create_directories(directory_, error);
    for (const auto& [region_pos, keys] : by_region) {
        const auto path   = region_path(region_pos.first, region_pos.second);
        auto       writer = nbt::RegionWriter::open_or_empty(path);
        for (const i64 key : keys) {
            const ChunkPos chunk = pos_of(key);
            nbt::Document  document;
            document.root = nbt::Tag::make_compound();
            (void)document.root.put("DataVersion", nbt::Tag{world::kDataVersion1201});
            (void)document.root.put("Position", nbt::Tag{nbt::Tag::IntArray{chunk.x, chunk.z}});
            nbt::Tag list = nbt::Tag::make_list(nbt::TagType::Compound);
            for (const nbt::Tag& entity : chunks.at(key)) {
                (void)list.push(entity);
            }
            if (const auto foreign = foreign_.find(key); foreign != foreign_.end()) {
                for (const nbt::Tag& entity : foreign->second) {
                    (void)list.push(entity);
                }
            }
            if (list.empty()) {
                on_disk_.erase(key);
            } else {
                on_disk_.insert(key);
            }
            (void)document.root.put("Entities", std::move(list));
            writer.set_chunk(static_cast<u32>(chunk.x & 31), static_cast<u32>(chunk.z & 31),
                             document, 0);
        }
        if (!writer.write(path)) {
            OV_LOG_WARN("entities: could not write {}", path.string());
        }
        regions_.erase(region_pos);  // re-read on the next load
    }
}

EntityStorageStats EntityStorage::unload_chunks(std::span<const ChunkPos> chunks,
                                                entity::EntityWorld& world, MobRecords& records,
                                                const EntityStorageHost& host,
                                                std::vector<i32>& removed) {
    EntityStorageStats stats;
    // A chunk leaving before its file was read, with an entity in it: read it
    // now, or the write below would replace what the file held.
    {
        std::unordered_set<i64> asked;
        for (const ChunkPos chunk : chunks) {
            asked.insert(key_of(chunk));
        }
        read_before_write(world, records, host, &asked);
    }
    std::map<i64, std::vector<nbt::Tag>> lists;
    std::unordered_set<i64>              leaving;
    for (const ChunkPos chunk : chunks) {
        const i64 key = key_of(chunk);
        if (loaded_.contains(key)) {
            leaving.insert(key);
            // Written only when there is something to write or to clear.
            if (on_disk_.contains(key) || foreign_.contains(key)) {
                lists[key];
            }
        }
    }
    if (leaving.empty()) {
        return stats;
    }
    std::vector<entity::EntityHandle> taken;
    for (const entity::EntityHandle handle : world.handles()) {
        const entity::EntityState* state = world.state(handle);
        if (state == nullptr || !saved(*state, host)) {
            continue;
        }
        const i64 key = key_of(chunk_of(state->position));
        if (!leaving.contains(key)) {
            continue;
        }
        if (auto compound = compound_of(world, handle, records, host)) {
            lists[key].push_back(std::move(*compound));
        }
        taken.push_back(handle);
    }
    for (const entity::EntityHandle handle : taken) {
        const entity::EntityState* state = world.state(handle);
        const i32                  id    = state->network_id;
        if (EntityAdopter* adopter = adopter_of(state->type)) {
            adopter->release(world, handle);
            ++stats.adopted;
        }
        (void)world.remove(handle);
        removed.push_back(id);
        records.forget(id);
        carried_.erase(id);
        ++stats.entities;
    }
    // ── persistence ── the loose adopters' entities of those chunks
    loose_scratch_.clear();
    const auto is_leaving = [&](ChunkPos chunk) { return leaving.contains(key_of(chunk)); };
    for (LooseAdopter* adopter : loose_) {
        const usize before = loose_scratch_.size();
        adopter->release(is_leaving, loose_scratch_, removed);
        stats.adopted += loose_scratch_.size() - before;
    }
    for (LooseEntity& entity : loose_scratch_) {
        lists[key_of(chunk_of(entity.position))].push_back(std::move(entity.compound));
        ++stats.entities;
    }
    loose_scratch_.clear();
    if (!lists.empty()) {
        write(lists);
    }
    for (const i64 key : leaving) {
        loaded_.erase(key);
        foreign_.erase(key);
    }
    stats.chunks = leaving.size();
    return stats;
}

EntityStorageStats EntityStorage::save_all(entity::EntityWorld& world, MobRecords& records,
                                           const EntityStorageHost& host) {
    EntityStorageStats stats;
    read_before_write(world, records, host, nullptr);
    std::map<i64, std::vector<nbt::Tag>> lists;
    // The chunks whose entry on disk must be cleared or kept (what was there
    // and what is carried through), then every chunk a mob stands in now.
    for (const i64 key : on_disk_) {
        if (loaded_.contains(key)) {
            lists[key];
        }
    }
    for (const auto& [key, carried] : foreign_) {
        lists[key];
    }
    for (const entity::EntityHandle handle : world.handles()) {
        const entity::EntityState* state = world.state(handle);
        if (state == nullptr || !saved(*state, host)) {
            continue;
        }
        // Where it stands now: an entity that crossed a chunk border is not
        // left behind in the chunk it came from, whose entry is rewritten
        // without it (`on_disk_`, above).
        auto compound = compound_of(world, handle, records, host);
        if (!compound) {
            continue;
        }
        lists[key_of(chunk_of(state->position))].push_back(std::move(*compound));
        ++stats.entities;
        if (adopter_of(state->type) != nullptr) {
            ++stats.adopted;
        }
    }
    // ── persistence ── the loose adopters', where each stands now
    loose_scratch_.clear();
    for (const LooseAdopter* adopter : loose_) {
        adopter->save(loose_scratch_);
    }
    for (LooseEntity& entity : loose_scratch_) {
        lists[key_of(chunk_of(entity.position))].push_back(std::move(entity.compound));
        ++stats.entities;
        ++stats.adopted;
    }
    loose_scratch_.clear();
    stats.chunks = lists.size();
    if (!lists.empty()) {
        write(lists);
    }
    return stats;
}

}  // namespace ov::server
