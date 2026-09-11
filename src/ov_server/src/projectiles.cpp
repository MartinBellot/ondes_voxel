#define OV_LOG_CATEGORY "server"

#include "projectiles.hpp"

#include "brewing_session.hpp"  // ── brewing ──
#include "entity_nbt.hpp"       // ── persistence ──
#include "survival_session.hpp"

#include "ov/base/log.hpp"
#include "ov/gameplay/combat.hpp"
#include "ov/gameplay/entity_physics.hpp"
#include "ov/gameplay/mob_attack.hpp"  // ── mobs-3 ──
#include "ov/nbt/binary.hpp"
#include "ov/nbt/tag.hpp"
#include "ov/protocol/entity.hpp"
#include "ov/protocol/interaction.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ov::server {
namespace {

using gameplay::ProjectileKind;

/// Metadata indices, each measured by summoning one arrow or trident with one
/// NBT field set and reading which index moved (measure_projectiles.py
/// metadata): `crit:1b` moves 8 to 0x01, `ShotFromCrossbow:1b` moves 8 to
/// 0x04, `PierceLevel:3b` moves 9, `Color` and `Potion` move 10; a trident's
/// Loyalty III moves its own 10. A crossbow shot sends 8 = 5, both bits.
constexpr u8 kArrowFlags     = 8;
constexpr u8 kArrowPierce    = 9;
constexpr i8 kArrowCritical  = 0x01;
constexpr i8 kArrowCrossbow  = 0x04;
constexpr u8 kArrowColor     = 10;  // ── brewing ── `Potion:"poison"` → 8889187

/// Ticks a stuck arrow waits before it can be picked up. From the wiki's
/// Arrow article (the "shake"); not measured — the campaign walked onto a
/// long-landed arrow.
constexpr i32 kPickupShakeTicks = 7;

/// The most wear these three take. From the wiki's item articles, not
/// measured: only the tick an item breaks depends on them.
[[nodiscard]] std::optional<i32> launcher_max_damage(std::string_view item) noexcept {
    if (item == "minecraft:bow") {
        return 384;
    }
    if (item == "minecraft:crossbow") {
        return 465;
    }
    if (item == "minecraft:trident") {
        return 250;
    }
    return std::nullopt;
}

[[nodiscard]] net::Uuid uuid_for(i32 network_id) noexcept {
    const auto mix = [](u64 z) {
        z += 0x9E3779B97F4A7C15ULL;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    };
    const auto id = static_cast<u64>(static_cast<u32>(network_id)) ^ 0xA770'0000'0000ULL;
    return net::Uuid{mix(id), mix(id ^ 0x5A5A5A5A5A5A5A5AULL)};
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

[[nodiscard]] u8 enchantment(const net::ItemStack& stack, std::string_view name) {
    const auto document = read_tag(stack);
    if (!document) {
        return 0;
    }
    const nbt::Tag* list = document->root.find("Enchantments");
    if (list == nullptr || list->list() == nullptr) {
        return 0;
    }
    for (const nbt::Tag& entry : *list->list()) {
        const nbt::Tag* id  = entry.find("id");
        const nbt::Tag* lvl = entry.find("lvl");
        if (id != nullptr && lvl != nullptr && id->as_string() == name) {
            return static_cast<u8>(std::clamp<i64>(lvl->as_i64(), 0, 255));
        }
    }
    return 0;
}

/// Measured on a crossbow loaded by a real server:
/// `{ChargedProjectiles: [{id: "minecraft:arrow", Count: 1b}], Damage: 0, Charged: 1b}`,
/// and after the shot `ChargedProjectiles: [], Charged: 0b`.
[[nodiscard]] bool crossbow_charged(const net::ItemStack& stack) {
    const auto document = read_tag(stack);
    if (!document) {
        return false;
    }
    const nbt::Tag* charged = document->root.find("Charged");
    return charged != nullptr && charged->as_i64() != 0;
}

/// The projectile a loaded crossbow holds, or empty.
[[nodiscard]] std::string crossbow_projectile(const net::ItemStack& stack) {
    const auto document = read_tag(stack);
    if (!document) {
        return {};
    }
    const nbt::Tag* list = document->root.find("ChargedProjectiles");
    if (list == nullptr || list->list() == nullptr || list->list()->empty()) {
        return {};
    }
    const nbt::Tag* id = list->list()->front().find("id");
    return id == nullptr ? std::string{} : std::string{id->as_string()};
}

void set_crossbow(net::ItemStack& stack, std::string_view projectile) {
    nbt::Document document;
    if (auto parsed = read_tag(stack)) {
        document = std::move(*parsed);
    } else {
        document.root = nbt::Tag::make_compound();
    }
    nbt::Tag list = nbt::Tag::make_list(nbt::TagType::Compound);
    if (!projectile.empty()) {
        nbt::Tag entry = nbt::Tag::make_compound();
        (void)entry.put("id", nbt::Tag{std::string{projectile}});
        (void)entry.put("Count", nbt::Tag{static_cast<i8>(1)});
        (void)list.push(std::move(entry));
    }
    (void)document.root.put("ChargedProjectiles", std::move(list));
    (void)document.root.put("Charged", nbt::Tag{static_cast<i8>(projectile.empty() ? 0 : 1)});
    stack.nbt = nbt::write(document);
}

[[nodiscard]] i32 item_damage(const net::ItemStack& stack) {
    const auto document = read_tag(stack);
    if (!document) {
        return 0;
    }
    const nbt::Tag* damage = document->root.find("Damage");
    return damage == nullptr ? 0 : static_cast<i32>(damage->as_i64());
}

void set_item_damage(net::ItemStack& stack, i32 damage) {
    nbt::Document document;
    if (auto parsed = read_tag(stack)) {
        document = std::move(*parsed);
    } else {
        document.root = nbt::Tag::make_compound();
    }
    (void)document.root.put("Damage", nbt::Tag{damage});
    stack.nbt = nbt::write(document);
}

/// The eyes of a standing player. Sneaking lowers them; this server does not
/// pass the pose here, and a sneaking archer shoots from standing height.
constexpr f64 kPlayerEye = static_cast<f64>(1.62F);

/// Measured: an arrow shot by a player standing at y = -60 starts at
/// -58.479999996721745, which is `-60 + 1.62F - 0.1F` to the last digit.
[[nodiscard]] Vec3d shot_start(const Vec3d& feet) noexcept {
    return Vec3d{feet.x, feet.y + kPlayerEye - static_cast<f64>(0.1F), feet.z};
}

/// The rotation a projectile is drawn with, from its velocity.
void face_along(entity::EntityState& state) noexcept {
    const Vec3d& v = state.velocity;
    const f64    h = std::sqrt(v.x * v.x + v.z * v.z);
    state.yaw      = static_cast<f32>(std::atan2(v.x, v.z) * 180.0 / std::numbers::pi);
    state.pitch    = static_cast<f32>(std::atan2(v.y, h) * 180.0 / std::numbers::pi);
    state.head_yaw = state.yaw;
}

[[nodiscard]] std::optional<ProjectileKind> thrown_kind(std::string_view item) noexcept {
    if (item == "minecraft:snowball") {
        return ProjectileKind::Snowball;
    }
    if (item == "minecraft:egg") {
        return ProjectileKind::Egg;
    }
    if (item == "minecraft:ender_pearl") {
        return ProjectileKind::EnderPearl;
    }
    if (item == "minecraft:experience_bottle") {
        return ProjectileKind::ExperienceBottle;
    }
    if (item == "minecraft:splash_potion" || item == "minecraft:lingering_potion") {
        return ProjectileKind::Potion;
    }
    return std::nullopt;
}

[[nodiscard]] bool is_ammo(std::string_view item) noexcept {
    return item == "minecraft:arrow" || item == "minecraft:spectral_arrow" ||
           item == "minecraft:tipped_arrow";
}

}  // namespace

Projectiles::Projectiles(const registry::Registries& registries,
                         const registry::BlockRegistry& blocks, MobCombat* mob_combat)
    : registries_{&registries}, blocks_{&blocks}, mob_combat_{mob_combat} {
    item_registry_ = registries.find("minecraft:item");
    if (const auto types = registries.find("minecraft:entity_type")) {
        for (usize i = 0; i < types_.size(); ++i) {
            types_[i] = registries
                            .protocol_id(*types, gameplay::projectile_type_name(
                                                     static_cast<ProjectileKind>(i)))
                            .value_or(-1);
        }
        skeleton_type_ = registries.protocol_id(*types, "minecraft:skeleton").value_or(-1);
        stray_type_    = registries.protocol_id(*types, "minecraft:stray").value_or(-1);
        blaze_type_    = registries.protocol_id(*types, "minecraft:blaze").value_or(-1);
    }
    world_.water = blocks.find_block("minecraft:water").value_or(registry::BlockId{0});
    for (const std::string_view gap : gameplay::projectile_gaps()) {
        OV_LOG_DEBUG("projectiles: {}", gap);
    }
    world_.targets.reserve(256);
    world_.events.events.reserve(64);
    pending_.reserve(32);
    spawning_.reserve(32);
    players_.reserve(16);
    doomed_.reserve(32);
    // ── mobs-3 ── A stray's arrow carries Slowness 600 as a *custom* effect,
    // which brewing's `arrow_effects` applies whole (a potion's own is ÷ 8).
    if (const i32 tipped = item_id("minecraft:tipped_arrow"); tipped != 0) {
        nbt::Document document;
        document.root     = nbt::Tag::make_compound();
        nbt::Tag effects  = nbt::Tag::make_list(nbt::TagType::Compound);
        nbt::Tag slowness = nbt::Tag::make_compound();
        (void)slowness.put("Id", nbt::Tag{static_cast<i32>(gameplay::Effect::Slowness)});
        (void)slowness.put("Duration", nbt::Tag{i32{600}});
        (void)slowness.put("Amplifier", nbt::Tag{i8{0}});
        (void)effects.push(std::move(slowness));
        (void)document.root.put("CustomPotionEffects", std::move(effects));
        stray_arrow_ = net::ItemStack{tipped, 1, nbt::write(document)};
    }
}

i32 Projectiles::item_id(std::string_view name) const {
    if (!item_registry_) {
        return 0;
    }
    return registries_->protocol_id(*item_registry_, name).value_or(0);
}

std::string_view Projectiles::item_name(i32 id) const {
    if (!item_registry_ || id == 0) {
        return {};
    }
    return registries_->entry_of(*item_registry_, id);
}

i32 Projectiles::type_of(ProjectileKind kind) const noexcept {
    return types_[static_cast<usize>(kind)];
}

bool Projectiles::owns(i32 type) const noexcept {
    return type >= 0 && std::find(types_.begin(), types_.end(), type) != types_.end();
}

std::optional<usize> Projectiles::find_ammo(std::span<const net::ItemStack> inventory) const {
    // The game's order: the offhand, then the hotbar, then the main inventory.
    const auto holds = [&](usize slot) {
        return slot < inventory.size() && inventory[slot].count > 0 &&
               is_ammo(item_name(inventory[slot].item_id));
    };
    if (holds(45)) {
        return 45;
    }
    for (usize slot = 36; slot < 45; ++slot) {
        if (holds(slot)) {
            return slot;
        }
    }
    for (usize slot = 9; slot < 36; ++slot) {
        if (holds(slot)) {
            return slot;
        }
    }
    return std::nullopt;
}

void Projectiles::wear_held(const Shooter& shooter, std::string_view item) const {
    // Measured: a bow's Damage goes up by one per shot, a crossbow's by one
    // per shot (four shots, Damage 4). Unbreaking is not read here.
    net::ItemStack& held   = shooter.inventory[shooter.held_slot];
    const i32       damage = item_damage(held) + 1;
    const auto      most   = launcher_max_damage(item);
    if (most && damage >= *most) {
        held = net::ItemStack{};
        if (shooter.held_broke) {
            shooter.held_broke();
        }
    } else {
        set_item_damage(held, damage);
    }
    if (shooter.send_slot) {
        shooter.send_slot(shooter.held_slot);
    }
}

void Projectiles::queue(Shot shot) {
    const std::scoped_lock lock{mutex_};
    pending_.push_back(std::move(shot));
}

// ── The network thread ──────────────────────────────────────────────────────

bool Projectiles::on_use_item(const Shooter& shooter, i64 tick) {
    if (shooter.held_slot >= shooter.inventory.size()) {
        return false;
    }
    net::ItemStack&        held = shooter.inventory[shooter.held_slot];
    const std::string_view item = item_name(held.count > 0 ? held.item_id : 0);
    if (item.empty()) {
        return false;
    }

    if (item == "minecraft:bow" || item == "minecraft:trident" ||
        (item == "minecraft:crossbow" && !crossbow_charged(held))) {
        // Measured: a survival player with a bow and no arrow does not shoot
        // at all; a creative one shoots anyway. A trident needs nothing.
        if (item != "minecraft:trident" && !shooter.creative && !find_ammo(shooter.inventory)) {
            return true;
        }
        const std::scoped_lock lock{mutex_};
        draws_[shooter.entity_id] = Draw{item, tick};
        return true;
    }

    if (item == "minecraft:crossbow") {
        // Loaded: this press fires it. Measured: speed 3.15, always critical,
        // `ShotFromCrossbow`, pickup 1 in survival, one point of wear.
        const std::string projectile = crossbow_projectile(held);
        Shot              shot;
        shot.data.kind            = projectile == "minecraft:spectral_arrow"
                                        ? ProjectileKind::SpectralArrow
                                        : ProjectileKind::Arrow;
        shot.data.owner           = shooter.entity_id;
        shot.data.owner_is_player = true;
        shot.data.critical        = true;
        shot.data.from_crossbow   = true;
        shot.data.pickup          = shooter.creative ? 2 : 1;
        shot.data.pierce          = enchantment(held, "minecraft:piercing");
        shot.start                = shot_start(shooter.feet);
        shot.yaw                  = shooter.yaw;
        shot.pitch                = shooter.pitch;
        shot.speed                = gameplay::kCrossbowSpeed;
        shot.inaccuracy           = gameplay::kCrossbowInaccuracy;
        shot.item                 = net::ItemStack{
            item_id(projectile.empty() ? "minecraft:arrow" : projectile), 1, {}};
        if (enchantment(held, "minecraft:multishot") > 0) {
            OV_LOG_DEBUG("crossbow: multishot fires one arrow here, not three");
        }
        set_crossbow(held, {});
        if (!shooter.creative) {
            wear_held(shooter, item);
        } else if (shooter.send_slot) {
            shooter.send_slot(shooter.held_slot);
        }
        queue(std::move(shot));
        return true;
    }

    if (const auto kind = thrown_kind(item)) {
        Shot shot;
        shot.data.kind            = *kind;
        shot.data.owner           = shooter.entity_id;
        shot.data.owner_is_player = true;
        shot.start                = shot_start(shooter.feet);
        shot.yaw                  = shooter.yaw;
        shot.pitch                = shooter.pitch;
        shot.speed                = gameplay::kThrowSpeed;
        shot.inaccuracy           = gameplay::kThrowInaccuracy;
        if (*kind == ProjectileKind::ExperienceBottle) {
            shot.speed = gameplay::kBottleSpeed;
            shot.pitch = shooter.pitch + gameplay::kBottlePitchOffset;
        }
        if (*kind == ProjectileKind::Potion) {
            shot.speed = 0.5F;
            shot.pitch = shooter.pitch + gameplay::kBottlePitchOffset;
            shot.item  = held;  // ── brewing ── what it will splash
            shot.item.count = 1;
        }
        if (*kind == ProjectileKind::EnderPearl && shooter.send) {
            // Twenty ticks of grey on the client's pearls. From the wiki; the
            // server itself does not refuse a pearl thrown during it.
            const auto cooldown = net::encode_set_cooldown(held.item_id, 20);
            shooter.send(net::clientbound::kSetCooldown, cooldown);
        }
        if (!shooter.creative) {
            held.count = static_cast<i8>(held.count - 1);
            if (held.count <= 0) {
                held = net::ItemStack{};
            }
            if (shooter.send_slot) {
                shooter.send_slot(shooter.held_slot);
            }
        }
        queue(std::move(shot));
        return true;
    }
    return false;
}

bool Projectiles::on_release(const Shooter& shooter, i64 tick) {
    Draw draw;
    {
        const std::scoped_lock lock{mutex_};
        const auto             it = draws_.find(shooter.entity_id);
        if (it == draws_.end()) {
            return false;
        }
        draw = it->second;
        draws_.erase(it);
    }
    if (shooter.held_slot >= shooter.inventory.size()) {
        return true;
    }
    net::ItemStack&        held  = shooter.inventory[shooter.held_slot];
    const std::string_view item  = item_name(held.count > 0 ? held.item_id : 0);
    if (item != draw.item) {
        // The hand changed under the draw. Nothing is shot.
        return true;
    }
    const i32 ticks = static_cast<i32>(std::max<i64>(0, tick - draw.started));

    if (item == "minecraft:bow") {
        // Measured: every shot's speed is `3 × bow_power(t)` for the ticks
        // drawn, to the spread; two ticks does not shoot.
        const f32 power = gameplay::bow_power(ticks);
        if (power < gameplay::kBowMinimumPower) {
            return true;
        }
        const auto ammo = find_ammo(shooter.inventory);
        if (!ammo && !shooter.creative) {
            return true;
        }
        const std::string_view ammo_name =
            ammo ? item_name(shooter.inventory[*ammo].item_id) : std::string_view{"minecraft:arrow"};
        const bool infinity = enchantment(held, "minecraft:infinity") > 0;
        const bool plain    = ammo_name == "minecraft:arrow";

        Shot shot;
        shot.data.kind = ammo_name == "minecraft:spectral_arrow" ? ProjectileKind::SpectralArrow
                                                                 : ProjectileKind::Arrow;
        shot.data.owner           = shooter.entity_id;
        shot.data.owner_is_player = true;
        shot.data.critical        = power >= 1.0F;
        // Measured: pickup 1 in survival, 2 ("creative only") for a creative
        // player and for an Infinity bow.
        shot.data.pickup = (shooter.creative || (infinity && plain)) ? 2 : 1;
        // Power adds 0.5 per level and 0.5 — from the wiki, not measured.
        if (const u8 level = enchantment(held, "minecraft:power"); level > 0) {
            shot.data.base_damage += 0.5 * static_cast<f64>(level) + 0.5;
        }
        shot.data.punch   = enchantment(held, "minecraft:punch");
        shot.data.flaming = enchantment(held, "minecraft:flame") > 0;
        shot.start        = shot_start(shooter.feet);
        shot.yaw          = shooter.yaw;
        shot.pitch        = shooter.pitch;
        shot.speed        = power * gameplay::kBowSpeed;
        shot.inaccuracy   = gameplay::kBowInaccuracy;
        shot.item         = net::ItemStack{item_id(ammo_name), 1,
                                           ammo ? shooter.inventory[*ammo].nbt : std::vector<u8>{}};
        // Measured: survival spends one arrow per shot; Infinity and creative
        // spend none (five arrows before, five after).
        if (ammo && !shooter.creative && !(infinity && plain)) {
            net::ItemStack& stack = shooter.inventory[*ammo];
            stack.count           = static_cast<i8>(stack.count - 1);
            if (stack.count <= 0) {
                stack = net::ItemStack{};
            }
            if (shooter.send_slot) {
                shooter.send_slot(*ammo);
            }
        }
        if (!shooter.creative) {
            wear_held(shooter, item);
        }
        queue(std::move(shot));
        return true;
    }

    if (item == "minecraft:crossbow") {
        // Measured: released after 24 ticks it is not loaded, after 25 it is.
        if (ticks < gameplay::crossbow_charge_ticks(enchantment(held, "minecraft:quick_charge"))) {
            return true;
        }
        const auto ammo = find_ammo(shooter.inventory);
        if (!ammo && !shooter.creative) {
            return true;
        }
        const std::string projectile{ammo ? item_name(shooter.inventory[*ammo].item_id)
                                          : std::string_view{"minecraft:arrow"}};
        if (ammo && !shooter.creative) {
            net::ItemStack& stack = shooter.inventory[*ammo];
            stack.count           = static_cast<i8>(stack.count - 1);
            if (stack.count <= 0) {
                stack = net::ItemStack{};
            }
            if (shooter.send_slot) {
                shooter.send_slot(*ammo);
            }
        }
        set_crossbow(held, projectile);
        if (shooter.send_slot) {
            shooter.send_slot(shooter.held_slot);
        }
        return true;
    }

    if (item == "minecraft:trident") {
        if (ticks < gameplay::kTridentMinimumTicks) {
            return true;
        }
        if (enchantment(held, "minecraft:riptide") > 0) {
            OV_LOG_DEBUG("trident: riptide is not carried out; thrown instead");
        }
        Shot shot;
        shot.data.kind            = ProjectileKind::Trident;
        shot.data.owner           = shooter.entity_id;
        shot.data.owner_is_player = true;
        shot.data.pickup          = shooter.creative ? 2 : 1;
        shot.start                = shot_start(shooter.feet);
        shot.yaw                  = shooter.yaw;
        shot.pitch                = shooter.pitch;
        shot.speed                = gameplay::kTridentSpeed;
        shot.inaccuracy           = gameplay::kBowInaccuracy;
        if (!shooter.creative) {
            wear_held(shooter, item);
            // The trident itself flies: it leaves the hand.
            shot.item = shooter.inventory[shooter.held_slot];
            shooter.inventory[shooter.held_slot] = net::ItemStack{};
            if (shooter.send_slot) {
                shooter.send_slot(shooter.held_slot);
            }
        } else {
            shot.item = held;
        }
        if (shot.item.count <= 0) {
            // It broke on the throw.
            return true;
        }
        queue(std::move(shot));
        return true;
    }
    return true;
}

void Projectiles::cancel(i32 player) {
    const std::scoped_lock lock{mutex_};
    draws_.erase(player);
}

// ── The tick thread ─────────────────────────────────────────────────────────

bool Projectiles::has_pending() const {
    const std::scoped_lock lock{mutex_};
    return !pending_.empty();
}

void Projectiles::spawn_pending(entity::EntityWorld& world, const ProjectileDeliver& deliver) {
    {
        const std::scoped_lock lock{mutex_};
        spawning_.swap(pending_);
    }
    for (const Shot& shot : spawning_) {
        const Vec3d velocity = gameplay::shoot_velocity(
            gameplay::look_direction(shot.yaw, shot.pitch), shot.speed, shot.inaccuracy, random_);
        spawn_shot(world, shot, velocity, deliver);
    }
    spawning_.clear();
}

void Projectiles::spawn_shot(entity::EntityWorld& world, const Shot& shot, Vec3d velocity,
                             const ProjectileDeliver& deliver) {
    const i32 type = type_of(shot.data.kind);
    if (type < 0) {
        return;
    }
    const auto handle = world.spawn(type, shot.start, net::Uuid{});
    if (!handle) {
        OV_LOG_WARN("cannot spawn a {}: {}", gameplay::projectile_type_name(shot.data.kind),
                    entity::to_string(handle.error()));
        return;
    }
    entity::EntityState* state = world.mutable_state(*handle);
    state->uuid                = uuid_for(state->network_id);
    state->velocity            = velocity;
    state->broadcast_position  = state->position;
    state->broadcast_valid     = true;
    face_along(*state);
    world.set_logic(*handle, std::make_unique<gameplay::ProjectileLogic>(shot.data, world_));
    if (gameplay::is_arrow_like(shot.data.kind) && shot.item.item_id != 0) {
        pickup_items_[state->network_id] = shot.item;
    }
    if (shot.data.kind == ProjectileKind::Potion && shot.item.item_id != 0) {  // ── brewing ──
        potion_items_[state->network_id] = shot.item;
    }
    spawn_packets(world, *state, deliver);
}

void Projectiles::spawn_packets(entity::EntityWorld& world, const entity::EntityState& state,
                                const ProjectileDeliver& deliver) const {
    const auto* logic =
        dynamic_cast<const gameplay::ProjectileLogic*>(world.logic(world.find(state.network_id)));
    net::SpawnEntity spawn;
    spawn.entity_id = state.network_id;
    spawn.uuid      = state.uuid;
    spawn.type      = state.type;
    const Vec3d anchor = state.broadcast_valid ? state.broadcast_position : state.position;
    spawn.x          = anchor.x;
    spawn.y          = anchor.y;
    spawn.z          = anchor.z;
    spawn.yaw        = state.yaw;
    spawn.pitch      = state.pitch;
    spawn.head_yaw   = state.head_yaw;
    spawn.velocity_x = state.velocity.x;
    spawn.velocity_y = state.velocity.y;
    spawn.velocity_z = state.velocity.z;
    // Measured: the shooter's own entity id — 1 for the first player, 37 for
    // a later one — and 0 for an arrow nobody shot. **Not** id + 1.
    spawn.data = logic != nullptr ? logic->data().owner : 0;
    deliver(net::clientbound::kSpawnEntity, net::encode_spawn_entity(spawn));

    if (logic == nullptr) {
        return;
    }
    const gameplay::ProjectileData& data = logic->data();
    net::MetadataWriter             fields;
    if (gameplay::is_arrow_like(data.kind) && data.kind != ProjectileKind::Trident) {
        const i8 flags = static_cast<i8>((data.critical ? kArrowCritical : 0) |
                                         (data.from_crossbow ? kArrowCrossbow : 0));
        if (flags != 0) {
            fields.byte_value(kArrowFlags, flags);
        }
        if (data.pierce > 0) {
            fields.byte_value(kArrowPierce, static_cast<i8>(data.pierce));
        }
        // ── brewing ── a tipped arrow is drawn in its potion's colour
        if (const auto it = pickup_items_.find(state.network_id);
            it != pickup_items_.end() && item_name(it->second.item_id) == "minecraft:tipped_arrow") {
            fields.varint_value(kArrowColor, static_cast<i32>(gameplay::potion_color(
                                                 potion_contents(it->second).effects)));
        }
    }
    if (!fields.empty()) {
        deliver(net::clientbound::kEntityMetadata,
                net::encode_entity_metadata(state.network_id, fields.take()));
    }
}

void Projectiles::before_entity_tick(entity::EntityWorld& world, const ProjectileHost& host) {
    world_.targets.clear();
    world_.events.events.clear();
    // Built every tick, projectile or not: the skeletons read their players
    // from here, and a skeleton that waits for an arrow to exist before it
    // looks for a target never shoots the first one — which is exactly what
    // the end-to-end check caught.
    for (const entity::EntityHandle handle : world.handles()) {
        const entity::EntityState* state = world.state(handle);
        // Living things only: a primed TNT, a falling block and another
        // projectile have no health and are flown through.
        if (state == nullptr || state->removed || state->max_health <= 0.0F || owns(state->type)) {
            continue;
        }
        world_.targets.push_back(gameplay::ProjectileTarget{
            state->network_id, gameplay::entity_box(*state), false, false});
    }
    players_.clear();
    if (host.players) {
        host.players(players_);
    }
    for (const ProjectilePlayer& player : players_) {
        if (!player.alive) {
            continue;
        }
        world_.targets.push_back(gameplay::ProjectileTarget{
            player.entity_id, gameplay::player_box(player.feet), true, false});
    }
}

void Projectiles::tick_skeletons(entity::EntityWorld& world,
                                 const gameplay::CollisionWorld& collisions, i32 difficulty,
                                 const ProjectileDeliver& deliver) {
    difficulty_ = difficulty;  // ── mobs-3 ──
    if (skeleton_type_ < 0 || difficulty <= 0) {
        return;
    }
    // Measured: one arrow every 60 ticks on easy and normal, every 40 on hard
    // — a 20-tick draw and a cooldown of 40 or 20. The range (15) and the
    // first draw's 20 ticks are the wiki's, not measured.
    constexpr f64 kRange = 15.0;
    const i32     interval = difficulty >= 3 ? 40 : 60;
    constexpr i32 kDraw    = 20;

    std::erase_if(skeletons_, [&](const auto& entry) {
        return world.find(entry.first) == entity::kNoEntity;
    });

    bool any = false;
    for (const entity::EntityHandle handle : world.handles()) {
        const entity::EntityState* state = world.state(handle);
        if (state != nullptr && (state->type == skeleton_type_ || state->type == stray_type_)) {
            any = true;
            break;
        }
    }
    if (!any) {
        return;
    }
    players_.clear();
    for (const gameplay::ProjectileTarget& target : world_.targets) {
        if (target.player) {
            players_.push_back(ProjectilePlayer{target.network_id,
                                                Vec3d{target.box.centre().x, target.box.min.y,
                                                      target.box.centre().z},
                                                false, true});
        }
    }

    doomed_.clear();
    for (const entity::EntityHandle handle : world.handles()) {
        const entity::EntityState* state = world.state(handle);
        if (state == nullptr || state->removed ||
            (state->type != skeleton_type_ && state->type != stray_type_)) {
            continue;
        }
        const Vec3d eye{state->position.x, state->position.y + static_cast<f64>(state->eye_height),
                        state->position.z};
        const ProjectilePlayer* target = nullptr;
        f64                     best   = kRange;
        for (const ProjectilePlayer& player : players_) {
            const Vec3d their_eye{player.feet.x, player.feet.y + kPlayerEye, player.feet.z};
            const f64   d = (their_eye - eye).length();
            if (d <= best && !gameplay::clip_blocks(collisions, eye, their_eye)) {
                best   = d;
                target = &player;
            }
        }
        auto [it, fresh] = skeletons_.try_emplace(state->network_id, kDraw);
        if (target == nullptr) {
            // Out of sight: it lowers the bow, and starts a fresh draw when it
            // sees someone again.
            it->second = std::max(it->second, kDraw);
            continue;
        }
        if (--it->second > 0) {
            continue;
        }
        it->second = interval;
        Shot shot;
        shot.data.kind  = ProjectileKind::Arrow;
        shot.data.owner = state->network_id;
        // A mob's arrow is never picked up.
        shot.data.pickup = 0;
        if (state->type == stray_type_) {  // ── mobs-3 ── the Slowness it carries
            shot.item = stray_arrow_;
        }
        shot.start       = Vec3d{eye.x, eye.y - static_cast<f64>(0.1F), eye.z};
        const Vec3d velocity =
            gameplay::skeleton_aim(shot.start, target->feet, 1.8, difficulty, random_);
        doomed_.push_back(state->network_id);  // reused as "who shot" for the log
        spawn_shot(world, shot, velocity, deliver);
    }
    if (!doomed_.empty()) {
        OV_LOG_DEBUG("{} skeleton arrows", doomed_.size());
    }
}

void Projectiles::hit_mob(entity::EntityWorld& world, entity::EntityState& projectile,
                          gameplay::ProjectileData& data, const gameplay::ProjectileEvent& event,
                          const ProjectileHost& host, const ProjectileDeliver& deliver,
                          bool& landed) {
    landed                            = false;
    const entity::EntityHandle handle = world.find(event.target);
    entity::EntityState* victim = handle == entity::kNoEntity ? nullptr : world.mutable_state(handle);
    if (victim == nullptr || victim->removed || mob_combat_ == nullptr) {
        return;
    }
    const bool trident = data.kind == ProjectileKind::Trident;
    const f32  amount =
        trident ? gameplay::kTridentDamage
                : static_cast<f32>(gameplay::arrow_damage(event.velocity, data.base_damage,
                                                          data.critical, random_));
    const MobHurt hurt = mob_combat_->hurt(*victim, amount, damage_constants_);
    if (!hurt.applied) {
        return;
    }
    landed = true;
    // ── fire ── a Flame arrow is a burning arrow, and a burning arrow sets
    // what it hits alight for five seconds (the wiki's Flame article).
    if (data.flaming && host.set_on_fire) {
        host.set_on_fire(victim->network_id, 5);
    }
    deliver(net::clientbound::kDamageEvent,
            net::encode_damage_event(
                victim->network_id,
                damage_type_id(trident ? gameplay::DamageKind::Trident : gameplay::DamageKind::Arrow),
                data.owner != 0 ? std::optional<i32>{data.owner} : std::nullopt,
                projectile.network_id));
    net::MetadataWriter fields;
    fields.float_value(net::metadata::kHealth, victim->health);
    deliver(net::clientbound::kEntityMetadata,
            net::encode_entity_metadata(victim->network_id, fields.take()));

    // The push: along the arrow's own horizontal travel. The strength is the
    // melee hit's 0.4, not measured for an arrow; Punch adds 0.6 a level
    // along the same line (the wiki's).
    const gameplay::CombatConstants constants{};
    victim->velocity = gameplay::apply_knockback(victim->velocity, victim->on_ground,
                                                 constants.base_knockback, event.velocity.x,
                                                 event.velocity.z, 0.0F, constants);
    if (data.punch > 0) {
        const f64 h = std::sqrt(event.velocity.x * event.velocity.x +
                                event.velocity.z * event.velocity.z);
        if (h > 0.0) {
            const f64 push = 0.6 * static_cast<f64>(data.punch) / h;
            victim->velocity.x += event.velocity.x * push;
            victim->velocity.y += 0.1;
            victim->velocity.z += event.velocity.z * push;
        }
    }
    deliver(net::clientbound::kEntityVelocity,
            net::encode_entity_velocity(victim->network_id, victim->velocity.x, victim->velocity.y,
                                        victim->velocity.z));

    if (!hurt.killed) {
        return;
    }
    deliver(net::clientbound::kEntityEvent, net::encode_entity_event(victim->network_id, 3));
    std::vector<gameplay::Drop> drops;
    (void)mob_combat_->loot(*victim, data.owner_is_player, 0, loot_random_, drops);
    if (host.drop_item) {
        for (const gameplay::Drop& drop : drops) {
            host.drop_item(Vec3d{victim->position.x,
                                 victim->position.y + static_cast<f64>(victim->height) * 0.5,
                                 victim->position.z},
                           net::ItemStack{drop.item, static_cast<i8>(std::min(drop.count, 64)), {}});
        }
    }
    victim->removed = true;
    mob_combat_->forget(victim->network_id);
}

ProjectileStats Projectiles::after_entity_tick(entity::EntityWorld& world,
                                               const ProjectileHost& host,
                                               const ProjectileDeliver& deliver) {
    ProjectileStats stats;
    for (const i32 gone : world.removed_ids()) {
        pickup_items_.erase(gone);
        saved_.erase(gone);  // ── persistence ──
    }

    doomed_.clear();
    for (const gameplay::ProjectileEvent& event : world_.events.events) {
        switch (event.kind) {
            case gameplay::ProjectileEvent::Kind::HitEntity: {
                ++stats.hits;
                const entity::EntityHandle handle = world.find(event.projectile);
                entity::EntityState* state =
                    handle == entity::kNoEntity ? nullptr : world.mutable_state(handle);
                auto* logic = handle == entity::kNoEntity
                                  ? nullptr
                                  : dynamic_cast<gameplay::ProjectileLogic*>(world.logic(handle));
                if (state == nullptr || logic == nullptr) {
                    break;
                }
                gameplay::ProjectileData& data = logic->data();
                bool                      landed = false;
                if (event.target_is_player) {
                    f32 amount =
                        data.kind == ProjectileKind::Trident
                            ? gameplay::kTridentDamage
                            : static_cast<f32>(gameplay::arrow_damage(
                                  event.velocity, data.base_damage, data.critical, random_));
                    // ── mobs-3 ── A mob's arrow on a player scales with the
                    // difficulty (`arrow`: when_caused_by_living_non_player).
                    if (!data.owner_is_player && data.owner != 0) {
                        amount = gameplay::scale_for_difficulty(
                            amount, static_cast<gameplay::Difficulty>(difficulty_));
                    }
                    landed = host.hurt_player &&
                             host.hurt_player(event.target, amount,
                                              data.kind == ProjectileKind::Trident
                                                  ? gameplay::DamageKind::Trident
                                                  : gameplay::DamageKind::Arrow);
                } else {
                    hit_mob(world, *state, data, event, host, deliver, landed);
                }
                gameplay::resolve_entity_hit(*state, data, landed);
                // ── brewing ── a tipped or spectral arrow's effect, on a hit that landed
                if (landed && host.arrow_hit && data.kind != ProjectileKind::Trident) {
                    if (const auto it = pickup_items_.find(state->network_id);
                        it != pickup_items_.end()) {
                        host.arrow_hit(it->second, event.target, event.target_is_player);
                    }
                }
                if (state->removed) {
                    doomed_.push_back(state->network_id);
                } else {
                    // It bounced, or a trident fell away: tell the clients.
                    deliver(net::clientbound::kEntityVelocity,
                            net::encode_entity_velocity(state->network_id, state->velocity.x,
                                                        state->velocity.y, state->velocity.z));
                }
                break;
            }
            case gameplay::ProjectileEvent::Kind::Stuck: {
                ++stats.stuck;
                // The critical trail stops when it lands.
                net::MetadataWriter fields;
                const entity::EntityHandle handle = world.find(event.projectile);
                const auto* logic = handle == entity::kNoEntity
                                        ? nullptr
                                        : dynamic_cast<gameplay::ProjectileLogic*>(world.logic(handle));
                if (logic != nullptr && event.projectile_kind != ProjectileKind::Trident) {
                    fields.byte_value(kArrowFlags,
                                      logic->data().from_crossbow ? kArrowCrossbow : i8{0});
                    deliver(net::clientbound::kEntityMetadata,
                            net::encode_entity_metadata(event.projectile, fields.take()));
                }
                break;
            }
            case gameplay::ProjectileEvent::Kind::Expired:
                ++stats.expired;
                pickup_items_.erase(event.projectile);
                break;
            case gameplay::ProjectileEvent::Kind::Broke: {
                ++stats.broke;
                switch (event.projectile_kind) {
                    case ProjectileKind::Snowball: {
                        // Measured: 3 to a blaze, 0 to a cow — and the cow did
                        // not move, so a zero hit is no hit at all.
                        const entity::EntityHandle handle = world.find(event.target);
                        entity::EntityState*       victim =
                            event.target == 0 || event.target_is_player || handle == entity::kNoEntity
                                ? nullptr
                                : world.mutable_state(handle);
                        if (victim != nullptr && victim->type == blaze_type_ && mob_combat_ != nullptr) {
                            const MobHurt hurt =
                                mob_combat_->hurt(*victim, gameplay::snowball_damage(true),
                                                  damage_constants_);
                            if (hurt.applied) {
                                net::MetadataWriter fields;
                                fields.float_value(net::metadata::kHealth, victim->health);
                                deliver(net::clientbound::kEntityMetadata,
                                        net::encode_entity_metadata(victim->network_id,
                                                                    fields.take()));
                            }
                        }
                        break;
                    }
                    case ProjectileKind::Egg: {
                        // Measured on 6400 eggs: 776 one chicken, 22 four.
                        const i32 chickens = gameplay::egg_chickens(random_);
                        for (i32 i = 0; i < chickens && host.spawn_mob; ++i) {
                            host.spawn_mob("minecraft:chicken", event.point);
                        }
                        break;
                    }
                    case ProjectileKind::EnderPearl:
                        // Measured: the thrower lands where the pearl broke and
                        // loses exactly 5 (20 → 15, three throws out of three).
                        if (event.owner_is_player && event.owner != 0) {
                            if (host.teleport) {
                                host.teleport(event.owner, event.point);
                            }
                            if (host.hurt_player) {
                                (void)host.hurt_player(event.owner, gameplay::kPearlFallDamage,
                                                       gameplay::DamageKind::Fall);
                            }
                        }
                        break;
                    case ProjectileKind::Potion:  // ── brewing ──
                        if (const auto it = potion_items_.find(event.projectile);
                            it != potion_items_.end()) {
                            if (host.potion_broke) {
                                // From the start of the breaking tick, as
                                // measured — not the impact point.
                                host.potion_broke(event.from, it->second, event.target,
                                                  event.target_is_player);
                            }
                            potion_items_.erase(it);
                        }
                        break;
                    case ProjectileKind::ExperienceBottle:
                        // 3 + 0..4 + 0..4, from the wiki; not measured, and the
                        // game splits it into several orbs where this drops one.
                        if (host.spawn_orb) {
                            const i32 a = random_.next_int(5);
                            const i32 b = random_.next_int(5);
                            host.spawn_orb(event.point, 3 + a + b);
                        }
                        break;
                    default:
                        break;
                }
                break;
            }
        }
    }
    world_.events.events.clear();
    // ── brewing ── a potion that vanished without breaking holds nothing
    std::erase_if(potion_items_,
                  [&](const auto& entry) { return world.find(entry.first) == entity::kNoEntity; });

    // Pickups: a stuck arrow or trident that a player walks over.
    for (const entity::EntityHandle handle : world.handles()) {
        const entity::EntityState* state = world.state(handle);
        if (state == nullptr || state->removed || !owns(state->type)) {
            continue;
        }
        const auto* logic = dynamic_cast<const gameplay::ProjectileLogic*>(world.logic(handle));
        if (logic == nullptr) {
            continue;
        }
        const gameplay::ProjectileData& data = logic->data();
        if (!data.in_ground || data.life < kPickupShakeTicks || data.pickup == 0 ||
            !gameplay::is_arrow_like(data.kind)) {
            continue;
        }
        const AABB box = AABB::from_entity(state->position, 0.5, 0.5);
        for (const ProjectilePlayer& player : players_) {
            if (!player.alive) {
                continue;
            }
            const AABB reach = gameplay::player_box(player.feet);
            const AABB grown{Vec3d{reach.min.x - 1.0, reach.min.y - 0.5, reach.min.z - 1.0},
                             Vec3d{reach.max.x + 1.0, reach.max.y + 0.5, reach.max.z + 1.0}};
            if (!grown.intersects(box)) {
                continue;
            }
            bool taken = false;
            if (data.pickup == 2) {
                // "Creative only": a creative player takes it and gets nothing.
                taken = player.creative;
            } else if (const auto it = pickup_items_.find(state->network_id);
                       it != pickup_items_.end() && host.give) {
                taken = host.give(player.entity_id, it->second) > 0;
            }
            if (!taken) {
                continue;
            }
            ++stats.picked_up;
            // Measured: Take Item Entity names the arrow, the player, and 1.
            deliver(net::clientbound::kTakeItem,
                    net::encode_take_item(state->network_id, player.entity_id, 1));
            doomed_.push_back(state->network_id);
            break;
        }
    }
    for (const i32 id : doomed_) {
        const entity::EntityHandle handle = world.find(id);
        if (handle != entity::kNoEntity) {
            (void)world.remove(handle);
            deliver(net::clientbound::kRemoveEntities, net::encode_remove_entity(id));
        }
        pickup_items_.erase(id);
        saved_.erase(id);  // ── persistence ──
    }
    doomed_.clear();
    return stats;
}

// ── persistence ─────────────────────────────────────────────────────────────

std::optional<entity::EntityHandle> Projectiles::adopt_saved(entity::EntityWorld& world,
                                                             const nbt::Tag&      compound) {
    const nbt::Tag* id   = compound.find("id");
    const auto      kind = id != nullptr ? gameplay::projectile_kind(id->as_string()) : std::nullopt;
    if (!kind || type_of(*kind) < 0) {
        return std::nullopt;
    }
    // What a pickup gives back, or what the bottle holds: from the keys
    // 1.20.1 keeps them under.
    net::ItemStack item{};
    if (*kind == ProjectileKind::Trident) {
        if (item_registry_) {
            item = item_stack_from(*registries_, *item_registry_, compound.find("Trident"))
                       .value_or(net::ItemStack{item_id("minecraft:trident"), 1, {}});
        }
    } else if (*kind == ProjectileKind::SpectralArrow) {
        item = net::ItemStack{item_id("minecraft:spectral_arrow"), 1, {}};
    } else if (*kind == ProjectileKind::Arrow) {
        const nbt::Tag* potion = compound.find("Potion");
        const nbt::Tag* custom = compound.find("CustomPotionEffects");
        if (potion != nullptr || custom != nullptr) {
            nbt::Tag tag = nbt::Tag::make_compound();
            if (potion != nullptr) {
                (void)tag.put("Potion", *potion);
            }
            if (custom != nullptr) {
                (void)tag.put("CustomPotionEffects", *custom);
            }
            item = net::ItemStack{item_id("minecraft:tipped_arrow"), 1,
                                  nbt::write(nbt::Document{"tag", std::move(tag)})};
        } else {
            item = net::ItemStack{item_id("minecraft:arrow"), 1, {}};
        }
    } else if (*kind == ProjectileKind::Potion) {
        if (item_registry_) {
            item = item_stack_from(*registries_, *item_registry_, compound.find("Item"))
                       .value_or(net::ItemStack{});
        }
        if (item.empty()) {
            OV_LOG_WARN("entities: a thrown potion with no potion in it — refused");
            return std::nullopt;
        }
    }

    const auto handle = world.spawn(type_of(*kind), list_vec3(compound, "Pos"),
                                    uuid_from(compound.find("UUID")).value_or(net::Uuid{}));
    if (!handle) {
        OV_LOG_WARN("entities: cannot spawn a {} read from disk: {}", id->as_string(),
                    entity::to_string(handle.error()));
        return std::nullopt;
    }
    entity::EntityState* state = world.mutable_state(*handle);
    if (state->uuid == net::Uuid{}) {
        state->uuid = uuid_for(state->network_id);
    }
    state->velocity           = list_vec3(compound, "Motion");
    state->yaw                = static_cast<f32>(list_f64(compound, "Rotation", 0));
    state->pitch              = static_cast<f32>(list_f64(compound, "Rotation", 1));
    state->head_yaw           = state->yaw;
    state->broadcast_position = state->position;
    state->broadcast_valid    = true;

    gameplay::ProjectileData data;
    data.kind = *kind;
    // Whoever shot it is a UUID on disk and a wire id here: not resolved. It
    // stays in the compound and goes back out.
    data.owner         = 0;
    data.left_owner    = true;
    data.age           = 1000;
    data.base_damage   = get_f64(compound, "damage", 2.0);
    data.critical      = get_bool(compound, "crit", false);
    data.pickup        = static_cast<u8>(std::clamp<i64>(get_i64(compound, "pickup", 0), 0, 2));
    data.pierce        = static_cast<u8>(std::clamp<i64>(get_i64(compound, "PierceLevel", 0), 0, 127));
    data.from_crossbow = get_bool(compound, "ShotFromCrossbow", false);
    data.dealt_damage  = get_bool(compound, "DealtDamage", false);
    data.in_ground     = gameplay::is_arrow_like(*kind) && get_bool(compound, "inGround", false);
    data.life          = static_cast<i32>(get_i64(compound, "life", 0));
    if (data.in_ground) {
        // The tip is in the block, 0.05 along the way it came (measured on
        // the stuck arrow, projectile.cpp); `Motion` still points that way.
        const Vec3d  v   = state->velocity;
        const f64    len = v.length();
        const Vec3d  tip = len > 1e-9 ? state->position + v * (0.1 / len) : state->position;
        data.stuck       = BlockPos{static_cast<i32>(std::floor(tip.x)),
                                    static_cast<i32>(std::floor(tip.y)),
                                    static_cast<i32>(std::floor(tip.z))};
        data.stuck_state = block_state_from(*blocks_, compound.find("inBlockState"))
                               .value_or(registry::BlockStateId{});
    }
    world.set_logic(*handle, std::make_unique<gameplay::ProjectileLogic>(data, world_));
    if (gameplay::is_arrow_like(*kind) && item.item_id != 0) {
        pickup_items_[state->network_id] = item;
    }
    if (*kind == ProjectileKind::Potion) {
        potion_items_[state->network_id] = item;
    }
    saved_[state->network_id] = compound;
    return *handle;
}

std::optional<nbt::Tag> Projectiles::save_entity(entity::EntityWorld& world,
                                                 entity::EntityHandle handle) const {
    const entity::EntityState* state = world.state(handle);
    const auto* logic = dynamic_cast<const gameplay::ProjectileLogic*>(world.logic(handle));
    if (state == nullptr || logic == nullptr || state->removed) {
        return std::nullopt;
    }
    const gameplay::ProjectileData& data = logic->data();
    const auto                      kept = saved_.find(state->network_id);
    nbt::Tag out = kept != saved_.end() ? kept->second : nbt::Tag::make_compound();
    put_entity_base(out, gameplay::projectile_type_name(data.kind), state->position,
                    state->velocity, state->yaw, state->pitch, state->uuid, state->on_ground,
                    i16{0});
    default_to(out, "HasBeenShot", nbt::Tag::make_bool(true));
    if (data.left_owner) {
        (void)out.put("LeftOwner", nbt::Tag::make_bool(true));
    }
    if (data.owner != 0 && owner_uuid_) {
        if (const auto owner = owner_uuid_(data.owner)) {
            (void)out.put("Owner", uuid_tag(*owner));
        }
    }
    const auto pickup = pickup_items_.find(state->network_id);
    if (gameplay::is_arrow_like(data.kind)) {
        (void)out.put("life", nbt::Tag{static_cast<i16>(std::clamp(data.life, 0, 32767))});
        default_to(out, "shake", nbt::Tag{i8{0}});
        (void)out.put("inGround", nbt::Tag::make_bool(data.in_ground));
        if (data.in_ground && data.stuck_state != registry::BlockStateId{}) {
            (void)out.put("inBlockState", block_state_tag(*blocks_, data.stuck_state));
        } else if (!data.in_ground) {
            (void)out.erase("inBlockState");
        }
        (void)out.put("pickup", nbt::Tag{static_cast<i8>(data.pickup)});
        (void)out.put("damage", nbt::Tag{data.base_damage});
        (void)out.put("crit", nbt::Tag::make_bool(data.critical));
        (void)out.put("ShotFromCrossbow", nbt::Tag::make_bool(data.from_crossbow));
        (void)out.put("PierceLevel", nbt::Tag{static_cast<i8>(data.pierce)});
        default_to(out, "SoundEvent", nbt::Tag{std::string{"minecraft:entity.arrow.hit"}});
        if (data.kind == ProjectileKind::SpectralArrow) {
            default_to(out, "Duration", nbt::Tag{i32{200}});
        } else if (data.kind == ProjectileKind::Trident) {
            (void)out.put("DealtDamage", nbt::Tag::make_bool(data.dealt_damage));
            if (pickup != pickup_items_.end() && item_registry_) {
                if (auto trident = item_stack_tag(*registries_, *item_registry_, pickup->second)) {
                    (void)out.put("Trident", std::move(*trident));
                }
            }
        } else if (pickup != pickup_items_.end() &&
                   item_name(pickup->second.item_id) == "minecraft:tipped_arrow") {
            if (const auto tag = read_tag(pickup->second)) {
                if (const nbt::Tag* potion = tag->root.find("Potion")) {
                    (void)out.put("Potion", *potion);
                }
                if (const nbt::Tag* custom = tag->root.find("CustomPotionEffects")) {
                    (void)out.put("CustomPotionEffects", *custom);
                }
            }
        }
    } else if (data.kind == ProjectileKind::Potion) {
        // Measured: a thrown potion always carries its `Item`; a snowball,
        // an egg, a pearl, a bottle o' enchanting do not, even when given one.
        if (const auto potion = potion_items_.find(state->network_id);
            potion != potion_items_.end() && item_registry_) {
            if (auto stack = item_stack_tag(*registries_, *item_registry_, potion->second)) {
                (void)out.put("Item", std::move(*stack));
            }
        }
    }
    return out;
}

void Projectiles::release(entity::EntityWorld& world, entity::EntityHandle handle) {
    if (const entity::EntityState* state = world.state(handle)) {
        pickup_items_.erase(state->network_id);
        potion_items_.erase(state->network_id);
        saved_.erase(state->network_id);
    }
}

}  // namespace ov::server
