#define OV_LOG_CATEGORY "server"

#include "brewing_session.hpp"

#include "block_container.hpp"

#include "ov/base/log.hpp"
#include "ov/nbt/binary.hpp"
#include "ov/nbt/tag.hpp"
#include "ov/protocol/chat.hpp"
#include "ov/protocol/entity.hpp"
#include "ov/protocol/recipe_packets.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace ov::server {
namespace {

using gameplay::Bottle;
using gameplay::Potion;
using gameplay::PotionEffect;
using gameplay::PotionForm;

/// The window's slots: the stand's five, then the player's 36 (main, hotbar).
constexpr i16 kStandSlots     = 5;
constexpr i16 kIngredientSlot = 3;
constexpr i16 kFuelSlot       = 4;
constexpr i16 kWindowSlots    = kStandSlots + 36;
constexpr i16 kHotbarStart    = kStandSlots + 27;

constexpr std::string_view kStandType   = "minecraft:brewing_stand";
constexpr std::string_view kPowder      = "minecraft:blaze_powder";
constexpr std::string_view kGlassBottle = "minecraft:glass_bottle";

/// The two numbers the screen draws its bars from, in the protocol's order:
/// 0 the brew time, 1 the fuel. Protocol archive; checked by the `window`
/// campaign.
constexpr i16 kBrewTimeProperty = 0;
constexpr i16 kFuelProperty     = 1;

/// An area effect cloud's metadata: 8 the radius (float), 9 the colour
/// (VarInt), 10 "waiting" (Boolean). Protocol archive's entity metadata;
/// checked by the `lingering` campaign.
constexpr u8 kCloudRadius  = 8;
constexpr u8 kCloudColor   = 9;
constexpr u8 kCloudWaiting = 10;

/// The World Event a breaking potion sends, with its colour as the data:
/// 2002 for a splash, 2007 when it holds an instant effect. From the protocol
/// archive's World Event table; not captured here.
constexpr i32 kSplashEvent        = 2002;
constexpr i32 kInstantSplashEvent = 2007;

/// A player's box, for a splash and a cloud.
constexpr f64 kPlayerHalfWidth = 0.3;
constexpr f64 kPlayerHeight    = 1.8;

[[nodiscard]] u64 key_of(BlockPos p) noexcept {
    return (static_cast<u64>(static_cast<u32>(p.x) & 0x3FFFFFFU) << 38U) |
           (static_cast<u64>(static_cast<u32>(p.z) & 0x3FFFFFFU) << 12U) |
           static_cast<u64>(static_cast<u32>(p.y) & 0xFFFU);
}

[[nodiscard]] u64 fnv(u64 hash, std::string_view text) noexcept {
    for (const char c : text) {
        hash ^= static_cast<u8>(c);
        hash *= 0x100000001B3ULL;
    }
    return hash;
}

[[nodiscard]] std::optional<nbt::Document> read_tag(const net::ItemStack& stack) {
    if (stack.nbt.empty()) {
        return std::nullopt;
    }
    auto document = nbt::read(stack.nbt);
    if (!document || document->root.type() != nbt::TagType::Compound) {
        return std::nullopt;
    }
    return std::move(*document);
}

[[nodiscard]] i32 read_int(const nbt::Tag& data, std::string_view name) noexcept {
    const nbt::Tag* tag = data.find(name);
    return tag == nullptr ? 0 : static_cast<i32>(tag->as_i64());
}

/// Overwrite a number in place. `put` would build a key string for every
/// write, and a stand writes `BrewTime` every tick of a brew.
void set_number(nbt::Tag& data, std::string_view name, nbt::Tag value) {
    if (nbt::Tag* tag = data.find(name); tag != nullptr) {
        *tag = std::move(value);
    } else {
        (void)data.put(std::string{name}, std::move(value));
    }
}

/// Read a stand's five slots out of its block entity, without copying: every
/// string in the view points into the NBT. `signature`, when given, gets one
/// number per slot that changes whenever the slot does.
void read_stand(const nbt::Tag& data, gameplay::StandView& view, std::array<u64, 5>* signature) {
    view = {};
    if (signature != nullptr) {
        signature->fill(0);
    }
    const nbt::Tag* items = data.find("Items");
    if (items == nullptr || items->list() == nullptr) {
        return;
    }
    for (const nbt::Tag& entry : *items->list()) {
        const nbt::Tag* slot  = entry.find("Slot");
        const nbt::Tag* id    = entry.find("id");
        const nbt::Tag* count = entry.find("Count");
        if (slot == nullptr || id == nullptr || count == nullptr || count->as_i64() <= 0) {
            continue;
        }
        const i64              index = slot->as_i64();
        const std::string_view name  = id->as_string();
        std::string_view       potion;
        if (const nbt::Tag* tag = entry.find("tag"); tag != nullptr) {
            if (const nbt::Tag* named = tag->find("Potion"); named != nullptr) {
                potion = named->as_string();
            }
        }
        if (index < 0 || index > 4) {
            continue;
        }
        if (signature != nullptr) {
            u64 hash = fnv(0xCBF29CE484222325ULL, name);
            hash ^= static_cast<u64>(count->as_i64()) * 0x9E3779B97F4A7C15ULL;
            hash = fnv(hash, potion);
            // Any other tag (a name, custom effects) moves it too.
            if (const nbt::Tag* tag = entry.find("tag"); tag != nullptr) {
                hash ^= static_cast<u64>(tag->size()) << 56U;
            }
            (*signature)[static_cast<usize>(index)] = hash == 0 ? 1 : hash;
        }
        if (index < 3) {
            view.occupied[static_cast<usize>(index)] = true;
            if (const auto form = gameplay::potion_form(name)) {
                // No tag, or a name 1.20.1 does not have, is `minecraft:empty`
                // — the uncraftable potion — as it is in vanilla.
                view.bottles[static_cast<usize>(index)] =
                    Bottle{*form, gameplay::potion_from_name(potion).value_or(Potion::Empty)};
            }
        } else if (index == kIngredientSlot) {
            view.ingredient = name;
        } else {
            view.powder = name == kPowder;
        }
    }
}

[[nodiscard]] net::Uuid cloud_uuid(i32 id) noexcept {
    const auto mix = [](u64 z) {
        z += 0x9E3779B97F4A7C15ULL;
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    };
    const auto seed = static_cast<u64>(static_cast<u32>(id)) ^ 0xC10D'0000'0000ULL;
    return net::Uuid{mix(seed), mix(seed ^ 0x5A5A5A5A5A5A5A5AULL)};
}

constexpr std::array<std::string_view, 9> kGaps{
    "water bottles thrown or lingering put out no fire and hurt no enderman: named, the fire is "
    "another agent's",
    "splash potions, clouds and tipped arrows reach players only; mobs carry no effects on this "
    "server",
    "a thrown potion carries no item metadata, so the client draws the default bottle",
    "a stand reloaded mid-brew keeps brewing here; vanilla forgets the ingredient it started "
    "with (not measured)",
    "tipped arrows are not crafted (lingering potion + 8 arrows is a special recipe)",
    "the cloud's owner, its particle and a custom colour are not modelled",
    "mushroom stew, rabbit stew and beetroot soup do not give their bowl back (only suspicious "
    "stew was measured)",
    "a player who joins after a cloud appeared does not see it until the next one",
    "drag and double-click in the brewing window are refused and the window resent",
};

}  // namespace

// ── Potion items ────────────────────────────────────────────────────────────

PotionContents potion_contents(const net::ItemStack& stack) {
    PotionContents out;
    const auto document = read_tag(stack);
    if (document) {
        if (const nbt::Tag* name = document->root.find("Potion"); name != nullptr) {
            if (const auto potion = gameplay::potion_from_name(name->as_string())) {
                out.potion = *potion;
            } else {
                out.unknown_name = true;
            }
        }
    }
    const auto own = gameplay::potion_info(out.potion).effects;
    out.effects.assign(own.begin(), own.end());
    if (!document) {
        return out;
    }
    const nbt::Tag* custom = document->root.find("CustomPotionEffects");
    if (custom == nullptr || custom->list() == nullptr) {
        return out;
    }
    for (const nbt::Tag& entry : *custom->list()) {
        const nbt::Tag* id = entry.find("Id");
        if (id == nullptr) {
            continue;
        }
        const auto effect = gameplay::effect_from_id(static_cast<i32>(id->as_i64()));
        if (!effect) {
            continue;  // an id this version does not have: skipped, not defaulted
        }
        const nbt::Tag* amplifier = entry.find("Amplifier");
        const nbt::Tag* duration  = entry.find("Duration");
        out.effects.push_back(PotionEffect{
            *effect, duration != nullptr ? static_cast<i32>(duration->as_i64()) : 0,
            amplifier != nullptr ? static_cast<u8>(amplifier->as_i64()) : u8{0}});
    }
    return out;
}

std::vector<u8> potion_tag(Potion potion) {
    nbt::Document document;
    document.root = nbt::Tag::make_compound();
    (void)document.root.put("Potion", nbt::Tag{std::string{gameplay::potion_info(potion).name}});
    return nbt::write(document);
}

std::vector<PotionEffect> stew_effects(const net::ItemStack& stack) {
    std::vector<PotionEffect> out;
    const auto                document = read_tag(stack);
    if (!document) {
        return out;
    }
    const nbt::Tag* list = document->root.find("Effects");
    if (list == nullptr || list->list() == nullptr) {
        return out;
    }
    for (const nbt::Tag& entry : *list->list()) {
        const nbt::Tag* id = entry.find("EffectId");
        if (id == nullptr) {
            continue;
        }
        const auto effect = gameplay::effect_from_id(static_cast<i32>(id->as_i64()));
        if (!effect) {
            continue;
        }
        const nbt::Tag* duration = entry.find("EffectDuration");
        out.push_back(PotionEffect{*effect,
                                   duration != nullptr ? static_cast<i32>(duration->as_i64())
                                                       : gameplay::kStewDefaultTicks,
                                   0});
    }
    return out;
}

DrinkOutcome drink_potion(const registry::Registries& registries, net::ItemStack& held,
                          bool creative, EffectSession& effects, SurvivalSession& survival,
                          const EffectIo& io, const EffectBearer& bearer) {
    const auto items = registries.find("minecraft:item");
    if (!items || held.count <= 0 || registries.entry_of(*items, held.item_id) != "minecraft:potion") {
        return {};
    }
    const PotionContents contents = potion_contents(held);
    effects.with_target(survival, io, bearer,
                        [&](gameplay::ActiveEffects& active, gameplay::EffectTarget& target) {
                            gameplay::drink_effects(contents.effects, active, target);
                        });
    DrinkOutcome out;
    out.drunk = true;
    if (creative) {
        return out;  // the potion stays in a creative hand, no bottle (`drink`)
    }
    const i32 bottle = registries.protocol_id(*items, kGlassBottle).value_or(0);
    if (held.count <= 1) {
        held = net::ItemStack{bottle, 1, {}};
    } else {
        held.count      = static_cast<i8>(held.count - 1);
        out.give_bottle = true;
    }
    return out;
}

bool eat_stew(const net::ItemStack& held, EffectSession& effects, SurvivalSession& survival,
              const EffectIo& io, const EffectBearer& bearer) {
    const std::vector<PotionEffect> list = stew_effects(held);
    if (list.empty()) {
        return false;
    }
    effects.with_target(survival, io, bearer,
                        [&](gameplay::ActiveEffects& active, gameplay::EffectTarget& target) {
                            gameplay::drink_effects(list, active, target);
                        });
    return true;
}

// ── Brewing ─────────────────────────────────────────────────────────────────

Brewing::Brewing(const registry::Registries& registries) : registries_{&registries} {
    item_registry_ = registries.find("minecraft:item");
    if (const auto menus = registries.find("minecraft:menu")) {
        menu_ = registries.protocol_id(*menus, kStandType).value_or(-1);
    }
    if (const auto types = registries.find("minecraft:entity_type")) {
        cloud_type_ = registries.protocol_id(*types, "minecraft:area_effect_cloud").value_or(-1);
    }
    for (const std::string_view gap : gaps()) {
        OV_LOG_DEBUG("brewing: {}", gap);
    }
    stands_.reserve(64);
    clouds_.reserve(16);
    players_.reserve(16);
}

std::span<const std::string_view> Brewing::gaps() noexcept { return kGaps; }

std::string_view Brewing::item_name(i32 id) const {
    if (!item_registry_ || id == 0) {
        return {};
    }
    return registries_->entry_of(*item_registry_, id);
}

i32 Brewing::item_id(std::string_view name) const {
    if (!item_registry_) {
        return 0;
    }
    return registries_->protocol_id(*item_registry_, name).value_or(0);
}

BrewingStats Brewing::take_stats() noexcept {
    const BrewingStats out = stats_;
    stats_                 = {};
    return out;
}

BrewingStats Brewing::tick_stands(const StandHost& host, std::span<const ChunkPos> loaded,
                                  i64 now) {
    BrewingStats stats;
    if (indexed_at_ < 0 || now < indexed_at_ || now - indexed_at_ >= 20) {
        indexed_at_ = now;
        stands_.clear();
        for (const ChunkPos& pos : loaded) {
            world::Chunk* chunk = host.chunk(pos.x, pos.z);
            if (chunk == nullptr) {
                continue;
            }
            for (const world::BlockEntity& entity : chunk->block_entities()) {
                if (entity.type == kStandType) {
                    // Local to the chunk on disk, world coordinates here.
                    stands_.push_back(BlockPos{pos.x * 16 + static_cast<i32>(entity.x), entity.y,
                                               pos.z * 16 + static_cast<i32>(entity.z)});
                }
            }
        }
        std::erase_if(cache_, [&](const auto& entry) {
            return std::ranges::none_of(stands_,
                                        [&](BlockPos at) { return key_of(at) == entry.first; });
        });
    }

    const ContainerSpec* spec = container_spec_for_block(kStandType);
    for (const BlockPos& at : stands_) {
        world::Chunk* chunk = host.chunk(at.x >> 4, at.z >> 4);
        if (chunk == nullptr) {
            continue;
        }
        world::BlockEntity* entity = chunk->block_entity_at(static_cast<usize>(at.x & 15), at.y,
                                                            static_cast<usize>(at.z & 15));
        if (entity == nullptr || entity->type != kStandType) {
            continue;  // changed under the index, which is a second old by design
        }
        ++stats.stands;
        gameplay::StandView view;
        read_stand(entity->data, view, nullptr);
        StandCache&          cache = cache_[key_of(at)];
        gameplay::StandState state{read_int(entity->data, "BrewTime"),
                                   read_int(entity->data, "Fuel"), cache.brewing};
        const i32            time_before = state.brew_time;
        const i32            fuel_before = state.fuel;
        const gameplay::StandTick step   = gameplay::brewing_stand_tick(view, state);
        cache.brewing                    = state.brewing;
        if (state.brew_time > 0) {
            ++stats.brewing;
        }

        bool dirty = false;
        if ((step.refuelled || step.brewed) && spec != nullptr) {
            // The view points into the NBT this rewrites: keep what is needed.
            const std::string ingredient{view.ingredient};
            BlockInventory    inventory{*spec, registries_, item_registry_};
            inventory.load(entity->data);
            std::span<net::ItemStack> slots = inventory.stacks();
            const auto                shrink = [](net::ItemStack& stack) {
                stack.count = static_cast<i8>(stack.count - 1);
                if (stack.count <= 0) {
                    stack = {};
                }
            };
            if (step.refuelled) {
                shrink(slots[kFuelSlot]);
                ++stats.refuelled;
            }
            if (step.brewed) {
                ++stats.brewed;
                for (usize i = 0; i < 3; ++i) {
                    if (const auto& made = step.results[i]) {
                        // A new stack, as vanilla makes one: a brewed potion
                        // keeps nothing of the bottle it was but its bottle.
                        slots[i] = net::ItemStack{item_id(gameplay::potion_form_item(made->form)), 1,
                                                  potion_tag(made->potion)};
                    }
                }
                shrink(slots[kIngredientSlot]);
                // Dragon's breath leaves its glass bottle in the slot when it
                // was the last one, and on the ground when it was not (wiki;
                // checked by the `timing` campaign).
                if (const std::string_view left = gameplay::brewing_remainder(ingredient);
                    !left.empty()) {
                    const net::ItemStack bottle{item_id(left), 1, {}};
                    if (slots[kIngredientSlot].empty()) {
                        slots[kIngredientSlot] = bottle;
                    } else if (host.drop) {
                        host.drop(at, bottle);
                    }
                }
            }
            inventory.store(entity->data);
            dirty = true;
        }
        if (state.brew_time != time_before) {
            set_number(entity->data, "BrewTime", nbt::Tag{static_cast<i16>(state.brew_time)});
            dirty = true;
        }
        if (state.fuel != fuel_before) {
            set_number(entity->data, "Fuel", nbt::Tag{static_cast<i8>(state.fuel)});
            dirty = true;
        }
        if (dirty && host.mark_dirty) {
            host.mark_dirty(at.x >> 4, at.z >> 4);
        }

        // The block shows which bottle slots are occupied.
        gameplay::StandView after;
        read_stand(entity->data, after, nullptr);
        const auto bits = static_cast<u8>((after.occupied[0] ? 1U : 0U) |
                                          (after.occupied[1] ? 2U : 0U) |
                                          (after.occupied[2] ? 4U : 0U));
        if (bits != cache.bottles) {
            cache.bottles = bits;
            if (host.set_bottles) {
                host.set_bottles(at, after.occupied);
            }
        }
    }
    stats_.stands += stats.stands;
    stats_.brewing += stats.brewing;
    stats_.brewed += stats.brewed;
    stats_.refuelled += stats.refuelled;
    return stats;
}

// ── The window ──────────────────────────────────────────────────────────────

bool Brewing::open(const StandHost& host, BlockPos pos, const PacketSink& send,
                   std::span<const net::ItemStack> inventory,
                   std::optional<BrewingWindow>& out) const {
    if (menu_ < 0) {
        return false;
    }
    world::Chunk* chunk = host.chunk(pos.x >> 4, pos.z >> 4);
    if (chunk == nullptr) {
        return false;
    }
    const world::BlockEntity* entity = chunk->block_entity_at(
        static_cast<usize>(pos.x & 15), pos.y, static_cast<usize>(pos.z & 15));
    if (entity == nullptr || entity->type != kStandType) {
        return false;
    }
    BrewingWindow window;
    window.pos = pos;
    window.sent_slots.fill(~0ULL);
    send(net::clientbound::kOpenScreen,
         net::encode_open_screen(window.window_id, menu_, "Brewing Stand"));
    out = window;
    (void)refresh(host, *out, inventory, {}, send);
    return true;
}

bool Brewing::refresh(const StandHost& host, BrewingWindow& window,
                      std::span<const net::ItemStack> inventory, const net::ItemStack& carried,
                      const PacketSink& send) const {
    world::Chunk* chunk = host.chunk(window.pos.x >> 4, window.pos.z >> 4);
    if (chunk == nullptr) {
        return false;
    }
    const world::BlockEntity* entity =
        chunk->block_entity_at(static_cast<usize>(window.pos.x & 15), window.pos.y,
                               static_cast<usize>(window.pos.z & 15));
    if (entity == nullptr || entity->type != kStandType) {
        return false;
    }
    gameplay::StandView view;
    std::array<u64, 5>  signature{};
    read_stand(entity->data, view, &signature);
    const ContainerSpec* spec = container_spec_for_block(kStandType);
    if (signature != window.sent_slots && spec != nullptr) {
        window.sent_slots = signature;
        BlockInventory stand{*spec, registries_, item_registry_};
        stand.load(entity->data);
        std::vector<net::ItemStack> slots;
        slots.reserve(static_cast<usize>(kWindowSlots));
        slots.insert(slots.end(), stand.stacks().begin(), stand.stacks().end());
        for (usize i = 9; i < 45 && i < inventory.size(); ++i) {
            slots.push_back(inventory[i]);
        }
        send(net::clientbound::kContainerContent,
             net::encode_container_content(window.window_id, window.state_id, slots, carried));
    }
    const std::array<i16, 2> now{
        static_cast<i16>(std::clamp(read_int(entity->data, "BrewTime"), 0, 32767)),
        static_cast<i16>(std::clamp(read_int(entity->data, "Fuel"), 0, 32767))};
    const std::array<i16, 2> index{kBrewTimeProperty, kFuelProperty};
    for (usize i = 0; i < now.size(); ++i) {
        if (now[i] == window.sent_properties[i]) {
            continue;
        }
        window.sent_properties[i] = now[i];
        send(net::clientbound::kContainerProperty,
             net::encode_container_property(window.window_id, index[i], now[i]));
    }
    return true;
}

void Brewing::click(const StandHost& host, BrewingWindow& window, const net::ContainerClick& click,
                    std::span<net::ItemStack> inventory, net::ItemStack& carried,
                    const PacketSink& send,
                    const std::function<void(const net::ItemStack&)>& drop) const {
    world::Chunk* chunk = host.chunk(window.pos.x >> 4, window.pos.z >> 4);
    world::BlockEntity* entity =
        chunk == nullptr ? nullptr
                         : chunk->block_entity_at(static_cast<usize>(window.pos.x & 15),
                                                  window.pos.y,
                                                  static_cast<usize>(window.pos.z & 15));
    const ContainerSpec* spec = container_spec_for_block(kStandType);
    if (entity == nullptr || entity->type != kStandType || spec == nullptr) {
        return;
    }
    BlockInventory stand{*spec, registries_, item_registry_};
    stand.load(entity->data);
    std::span<net::ItemStack> slots = stand.stacks();

    const auto ref = [&](i16 index) -> net::ItemStack* {
        if (index >= 0 && index < kStandSlots) {
            return &slots[static_cast<usize>(index)];
        }
        if (index >= kStandSlots && index < kWindowSlots) {
            const auto into = static_cast<usize>(index - kStandSlots + 9);
            return into < inventory.size() ? &inventory[into] : nullptr;
        }
        return nullptr;
    };
    // The three admission rules of the screen: a bottle slot takes a potion of
    // any form or a glass bottle, one at a time; the ingredient slot takes one
    // of the seventeen; the fuel slot takes blaze powder.
    const auto admits = [&](i16 index, const net::ItemStack& stack) {
        if (index >= kStandSlots || stack.empty()) {
            return true;
        }
        const std::string_view name = item_name(stack.item_id);
        if (index < kIngredientSlot) {
            return gameplay::potion_form(name).has_value() || name == kGlassBottle;
        }
        if (index == kIngredientSlot) {
            return gameplay::is_brewing_ingredient(name);
        }
        return name == kPowder;
    };
    const auto limit = [&](i16 index, const net::ItemStack& stack) -> i8 {
        if (index >= 0 && index < kIngredientSlot) {
            return 1;
        }
        const i8 most = registries_->max_stack_size(stack.item_id);
        return most > 0 ? most : i8{64};
    };
    const auto same = [](const net::ItemStack& a, const net::ItemStack& b) {
        return a.item_id == b.item_id && a.nbt == b.nbt;
    };
    const auto settle = [](net::ItemStack& stack) {
        if (stack.count <= 0) {
            stack = {};
        }
    };
    // Merge into matching stacks first, then fill empty slots — the order that
    // makes a shift-click one move rather than a scatter.
    const auto deposit = [&](net::ItemStack& from, i16 first, i16 last, bool reverse) {
        for (int pass = 0; pass < 2 && !from.empty(); ++pass) {
            for (i16 step = 0; step < last - first && !from.empty(); ++step) {
                const i16       index = reverse ? static_cast<i16>(last - 1 - step)
                                                : static_cast<i16>(first + step);
                net::ItemStack* into  = ref(index);
                if (into == nullptr || !admits(index, from)) {
                    continue;
                }
                const i8 most = limit(index, from);
                if (pass == 0 && !into->empty() && same(*into, from) && into->count < most) {
                    const i8 moved = std::min<i8>(static_cast<i8>(most - into->count), from.count);
                    into->count    = static_cast<i8>(into->count + moved);
                    from.count     = static_cast<i8>(from.count - moved);
                } else if (pass == 1 && into->empty()) {
                    const i8 moved = std::min<i8>(most, from.count);
                    *into          = from;
                    into->count    = moved;
                    from.count     = static_cast<i8>(from.count - moved);
                }
                settle(from);
            }
        }
    };

    bool touched = false;
    switch (click.mode) {
        case 0: {
            if (click.slot == -999) {
                if (!carried.empty()) {
                    net::ItemStack thrown = carried;
                    thrown.count          = click.button == 0 ? carried.count : i8{1};
                    carried.count         = static_cast<i8>(carried.count - thrown.count);
                    settle(carried);
                    drop(thrown);
                }
                break;
            }
            net::ItemStack* slot = ref(click.slot);
            if (slot == nullptr) {
                break;
            }
            touched      = click.slot < kStandSlots;
            const i8 most = limit(click.slot, carried.empty() ? *slot : carried);
            if (click.button == 0) {
                if (carried.empty()) {
                    carried = *slot;
                    *slot   = {};
                } else if (slot->empty()) {
                    if (admits(click.slot, carried)) {
                        const i8 moved = std::min(most, carried.count);
                        *slot          = carried;
                        slot->count    = moved;
                        carried.count  = static_cast<i8>(carried.count - moved);
                        settle(carried);
                    }
                } else if (same(*slot, carried)) {
                    const i8 moved = std::min<i8>(static_cast<i8>(std::max(0, most - slot->count)),
                                                  carried.count);
                    slot->count   = static_cast<i8>(slot->count + moved);
                    carried.count = static_cast<i8>(carried.count - moved);
                    settle(carried);
                } else if (admits(click.slot, carried) && carried.count <= most) {
                    std::swap(*slot, carried);
                }
            } else if (click.button == 1) {
                if (carried.empty()) {
                    if (!slot->empty()) {
                        const auto half = static_cast<i8>((slot->count + 1) / 2);
                        carried         = *slot;
                        carried.count   = half;
                        slot->count     = static_cast<i8>(slot->count - half);
                        settle(*slot);
                    }
                } else if (slot->empty()) {
                    if (admits(click.slot, carried)) {
                        *slot         = carried;
                        slot->count   = 1;
                        carried.count = static_cast<i8>(carried.count - 1);
                        settle(carried);
                    }
                } else if (same(*slot, carried)) {
                    if (slot->count < most) {
                        slot->count   = static_cast<i8>(slot->count + 1);
                        carried.count = static_cast<i8>(carried.count - 1);
                        settle(carried);
                    }
                } else if (admits(click.slot, carried) && carried.count <= most) {
                    std::swap(*slot, carried);
                }
            }
            break;
        }
        case 1: {
            net::ItemStack* slot = ref(click.slot);
            if (slot == nullptr || slot->empty()) {
                break;
            }
            if (click.slot < kStandSlots) {
                // Out of the stand: into the player, hotbar end first.
                deposit(*slot, kStandSlots, kWindowSlots, true);
                touched = true;
                break;
            }
            // Into the stand, by what the item is: blaze powder to the fuel
            // then the ingredient slot, an ingredient to its slot, a single
            // bottle to the bottle slots; anything else between the main
            // inventory and the hotbar. From the wiki's Brewing Stand article;
            // a client-driven rule, not measured here.
            const std::string_view name = item_name(slot->item_id);
            if (name == kPowder) {
                deposit(*slot, kFuelSlot, kFuelSlot + 1, false);
                deposit(*slot, kIngredientSlot, kIngredientSlot + 1, false);
                touched = true;
            } else if (gameplay::is_brewing_ingredient(name)) {
                deposit(*slot, kIngredientSlot, kIngredientSlot + 1, false);
                touched = true;
            } else if ((gameplay::potion_form(name) || name == kGlassBottle) && slot->count == 1) {
                deposit(*slot, 0, kIngredientSlot, false);
                touched = true;
            } else if (click.slot < kHotbarStart) {
                deposit(*slot, kHotbarStart, kWindowSlots, false);
            } else {
                deposit(*slot, kStandSlots, kHotbarStart, false);
            }
            break;
        }
        case 2: {
            if (click.button < 0 || click.button > 8) {
                break;
            }
            net::ItemStack* slot   = ref(click.slot);
            net::ItemStack& hotbar = inventory[36 + static_cast<usize>(click.button)];
            if (slot == nullptr || slot == &hotbar) {
                break;
            }
            if (click.slot < kStandSlots && !hotbar.empty() &&
                (!admits(click.slot, hotbar) || hotbar.count > limit(click.slot, hotbar))) {
                break;
            }
            std::swap(*slot, hotbar);
            touched = click.slot < kStandSlots;
            break;
        }
        case 4: {
            net::ItemStack* slot = ref(click.slot);
            if (slot == nullptr || slot->empty()) {
                break;
            }
            net::ItemStack thrown = *slot;
            thrown.count          = click.button == 1 ? slot->count : i8{1};
            slot->count           = static_cast<i8>(slot->count - thrown.count);
            settle(*slot);
            drop(thrown);
            touched = click.slot < kStandSlots;
            break;
        }
        default:
            // Drag (5), double-click (6) and pick-block (3): refused, and the
            // window resent below so the client's guess is rolled back.
            OV_LOG_DEBUG("brewing window: click mode {} is refused", click.mode);
            break;
    }
    if (touched) {
        stand.store(entity->data);
        if (host.mark_dirty) {
            host.mark_dirty(window.pos.x >> 4, window.pos.z >> 4);
        }
    }
    // Always resent: the client has already applied its own guess.
    window.state_id = click.state_id + 1;
    window.sent_slots.fill(~0ULL);
    (void)refresh(host, window, inventory, carried, send);
}

// ── Potions ─────────────────────────────────────────────────────────────────

void Brewing::potion_broke(const PotionHost& host, Vec3d at, const net::ItemStack& potion,
                           i32 direct, bool direct_is_player) {
    const std::string_view name = item_name(potion.item_id);
    const auto             form = gameplay::potion_form(name);
    if (!form || *form == PotionForm::Drink) {
        return;
    }
    PotionContents contents = potion_contents(potion);
    ++stats_.splashes;
    const bool instant = std::ranges::any_of(contents.effects, [](const PotionEffect& effect) {
        return gameplay::effect_info(effect.effect).instantaneous;
    });
    if (host.broadcast) {
        host.broadcast(net::clientbound::kWorldEvent,
                       net::encode_world_event(
                           instant ? kInstantSplashEvent : kSplashEvent,
                           net::WirePosition{static_cast<i32>(std::floor(at.x)),
                                             static_cast<i32>(std::floor(at.y)),
                                             static_cast<i32>(std::floor(at.z))},
                           static_cast<i32>(gameplay::potion_color(contents.effects)), false));
    }
    if (contents.potion == Potion::Water && contents.effects.empty()) {
        OV_LOG_DEBUG("brewing: a water bottle broke; it puts out nothing here (named)");
        return;
    }
    if (*form == PotionForm::Lingering) {
        spawn_cloud(host, at, std::move(contents));
        return;
    }
    if (contents.effects.empty() || !host.players || !host.affect) {
        return;
    }
    players_.clear();
    host.players(players_);
    for (const PotionPlayer& player : players_) {
        if (!player.alive) {
            continue;
        }
        const f64 dx = player.feet.x - at.x;
        const f64 dy = player.feet.y - at.y;
        const f64 dz = player.feet.z - at.z;
        // The potion's box, grown by 4 across and 2 up and down, must meet the
        // player's; then the distance to the feet decides.
        constexpr f64 kReach = gameplay::kSplashRadius + 0.125 + kPlayerHalfWidth;
        if (std::abs(dx) > kReach || std::abs(dz) > kReach ||
            dy > 0.25 + gameplay::kSplashHalfHeight ||
            dy + kPlayerHeight < -gameplay::kSplashHalfHeight) {
            continue;
        }
        const bool hit_directly = direct_is_player && player.entity_id == direct;
        const f64  factor       = gameplay::splash_factor(dx * dx + dy * dy + dz * dz, hit_directly);
        if (factor <= 0.0) {
            continue;
        }
        host.affect(player.entity_id,
                    [&](gameplay::ActiveEffects& active, gameplay::EffectTarget& target) {
                        gameplay::splash_effects(contents.effects, factor, active, target);
                    });
        ++stats_.applied;
    }
}

void Brewing::arrow_hit(const PotionHost& host, const net::ItemStack& arrow, i32 target,
                        bool target_is_player) {
    if (!target_is_player || !host.affect) {
        return;  // mobs carry no effects on this server — named in `gaps`
    }
    const std::string_view name = item_name(arrow.item_id);
    if (name == "minecraft:spectral_arrow") {
        host.affect(target, [](gameplay::ActiveEffects& active, gameplay::EffectTarget& victim) {
            gameplay::EffectInstance glow;
            glow.effect   = gameplay::Effect::Glowing;
            glow.duration = gameplay::kSpectralGlowTicks;
            (void)active.add(glow, victim);
        });
        ++stats_.applied;
        return;
    }
    if (name != "minecraft:tipped_arrow") {
        return;
    }
    const PotionContents contents = potion_contents(arrow);
    if (contents.effects.empty()) {
        return;
    }
    host.affect(target, [&](gameplay::ActiveEffects& active, gameplay::EffectTarget& victim) {
        gameplay::arrow_effects(contents.effects, active, victim);
    });
    ++stats_.applied;
}

void Brewing::spawn_cloud(const PotionHost& host, Vec3d at, PotionContents contents) {
    if (cloud_type_ < 0 || !host.next_entity_id) {
        return;
    }
    CloudEntity cloud;
    cloud.id      = host.next_entity_id();
    cloud.at      = at;
    cloud.cloud   = gameplay::lingering_cloud();
    cloud.color   = gameplay::potion_color(contents.effects);
    cloud.effects = std::move(contents.effects);
    clouds_.push_back(std::move(cloud));
    ++stats_.clouds;
    if (host.broadcast) {
        const CloudEntity& made = clouds_.back();
        net::SpawnEntity   spawn;
        spawn.entity_id = made.id;
        spawn.uuid      = cloud_uuid(made.id);
        spawn.type      = cloud_type_;
        spawn.x         = made.at.x;
        spawn.y         = made.at.y;
        spawn.z         = made.at.z;
        host.broadcast(net::clientbound::kSpawnEntity, net::encode_spawn_entity(spawn));
        net::MetadataWriter fields;
        fields.float_value(kCloudRadius, made.cloud.radius);
        fields.varint_value(kCloudColor, static_cast<i32>(made.color));
        fields.boolean_value(kCloudWaiting, true);
        host.broadcast(net::clientbound::kEntityMetadata,
                       net::encode_entity_metadata(made.id, fields.take()));
    }
}

void Brewing::cloud_packets(const PacketSink& send) const {
    for (const CloudEntity& cloud : clouds_) {
        net::SpawnEntity spawn;
        spawn.entity_id = cloud.id;
        spawn.uuid      = cloud_uuid(cloud.id);
        spawn.type      = cloud_type_;
        spawn.x         = cloud.at.x;
        spawn.y         = cloud.at.y;
        spawn.z         = cloud.at.z;
        send(net::clientbound::kSpawnEntity, net::encode_spawn_entity(spawn));
        net::MetadataWriter fields;
        fields.float_value(kCloudRadius, cloud.cloud.radius);
        fields.varint_value(kCloudColor, static_cast<i32>(cloud.color));
        fields.boolean_value(kCloudWaiting, gameplay::cloud_waiting(cloud.cloud));
        send(net::clientbound::kEntityMetadata, net::encode_entity_metadata(cloud.id, fields.take()));
    }
}

void Brewing::tick_clouds(const PotionHost& host) {
    for (usize i = 0; i < clouds_.size();) {
        CloudEntity& cloud = clouds_[i];
        bool         alive = gameplay::cloud_tick(cloud.cloud);
        const bool   waiting = gameplay::cloud_waiting(cloud.cloud);
        bool         radius_moved = alive && !waiting && cloud.cloud.radius_per_tick != 0.0F;
        if (alive && gameplay::cloud_scans(cloud.cloud) && host.players && host.affect) {
            std::erase_if(cloud.victims,
                          [&](const auto& victim) { return cloud.cloud.age >= victim.second; });
            if (!cloud.effects.empty()) {
                players_.clear();
                host.players(players_);
                for (const PotionPlayer& player : players_) {
                    if (!player.alive || cloud.victims.contains(player.entity_id)) {
                        continue;
                    }
                    if (!gameplay::cloud_reaches(cloud.cloud, player.feet.x - cloud.at.x,
                                                 player.feet.y - cloud.at.y,
                                                 player.feet.z - cloud.at.z, kPlayerHeight)) {
                        continue;
                    }
                    cloud.victims[player.entity_id] =
                        cloud.cloud.age + cloud.cloud.reapplication_delay;
                    host.affect(player.entity_id,
                                [&](gameplay::ActiveEffects& active, gameplay::EffectTarget& target) {
                                    gameplay::cloud_effects(cloud.effects, active, target);
                                });
                    ++stats_.applied;
                    radius_moved = true;
                    if (!gameplay::cloud_used(cloud.cloud)) {
                        alive = false;
                        break;
                    }
                }
            }
        }
        if (!alive) {
            if (host.broadcast) {
                host.broadcast(net::clientbound::kRemoveEntities,
                               net::encode_remove_entity(cloud.id));
            }
            clouds_.erase(clouds_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        net::MetadataWriter fields;
        if (waiting != cloud.sent_waiting) {
            fields.boolean_value(kCloudWaiting, waiting);
            cloud.sent_waiting = waiting;
        }
        if (radius_moved) {
            fields.float_value(kCloudRadius, cloud.cloud.radius);
        }
        if (!fields.empty() && host.broadcast) {
            host.broadcast(net::clientbound::kEntityMetadata,
                           net::encode_entity_metadata(cloud.id, fields.take()));
        }
        ++i;
    }
}

}  // namespace ov::server
