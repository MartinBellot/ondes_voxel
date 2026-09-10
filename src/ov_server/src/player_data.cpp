#include "player_data.hpp"

#include "ov/gameplay/experience.hpp"
#include "ov/io/compression.hpp"
#include "ov/io/file.hpp"
#include "ov/nbt/binary.hpp"
#include "ov/world/chunk_storage.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <system_error>

namespace ov::server {
namespace {

constexpr std::string_view kOverworld = "minecraft:overworld";

/// `Motion` of a player standing still on the ground: one tick of gravity
/// through the drag, stored as a float promoted to double. Measured — the
/// vanilla file of a bot at rest carried exactly this.
constexpr f64 kRestingMotionY = -0.0784000015258789;

/// The order vanilla writes the inventory in, as window slots: the 36 main
/// slots (hotbar first), then the armour from the feet up, then the off hand.
/// Measured: 0, 1, 2, 29, 100, 103, -106.
constexpr std::array<usize, 41> kWriteOrder = [] {
    std::array<usize, 41> order{};
    usize                 n = 0;
    for (usize w = 36; w <= 44; ++w) {
        order[n++] = w;
    }
    for (usize w = 9; w <= 35; ++w) {
        order[n++] = w;
    }
    for (usize w = 8; w >= 5; --w) {
        order[n++] = w;
    }
    order[n] = 45;
    return order;
}();

void put(nbt::Tag& compound, std::string_view name, nbt::Tag value) {
    (void)compound.put(std::string{name}, std::move(value));
}

[[nodiscard]] f64 get_f64(const nbt::Tag& compound, std::string_view name, f64 fallback) {
    const nbt::Tag* tag = compound.find(name);
    return tag != nullptr ? tag->as_f64(fallback) : fallback;
}

[[nodiscard]] i64 get_i64(const nbt::Tag& compound, std::string_view name, i64 fallback) {
    const nbt::Tag* tag = compound.find(name);
    return tag != nullptr ? tag->as_i64(fallback) : fallback;
}

[[nodiscard]] bool get_bool(const nbt::Tag& compound, std::string_view name, bool fallback) {
    const nbt::Tag* tag = compound.find(name);
    return tag != nullptr ? tag->as_bool(fallback) : fallback;
}

[[nodiscard]] nbt::Tag uuid_tag(const net::Uuid& uuid) {
    return nbt::Tag{nbt::Tag::IntArray{
        static_cast<i32>(static_cast<u32>(uuid.most_significant >> 32)),
        static_cast<i32>(static_cast<u32>(uuid.most_significant & 0xFFFFFFFFULL)),
        static_cast<i32>(static_cast<u32>(uuid.least_significant >> 32)),
        static_cast<i32>(static_cast<u32>(uuid.least_significant & 0xFFFFFFFFULL))}};
}

[[nodiscard]] std::optional<net::Uuid> uuid_from(const nbt::Tag* tag) {
    if (tag == nullptr) {
        return std::nullopt;
    }
    const auto* ints = tag->get_if<nbt::Tag::IntArray>();
    if (ints == nullptr || ints->size() != 4) {
        return std::nullopt;
    }
    const auto half = [](i32 high, i32 low) {
        return (static_cast<u64>(static_cast<u32>(high)) << 32) | static_cast<u32>(low);
    };
    return net::Uuid{half((*ints)[0], (*ints)[1]), half((*ints)[2], (*ints)[3])};
}

[[nodiscard]] nbt::Tag doubles(std::initializer_list<f64> values) {
    nbt::Tag list = nbt::Tag::make_list(nbt::TagType::Double);
    for (const f64 value : values) {
        (void)list.push(nbt::Tag{value});
    }
    return list;
}

[[nodiscard]] nbt::Tag floats(std::initializer_list<f32> values) {
    nbt::Tag list = nbt::Tag::make_list(nbt::TagType::Float);
    for (const f32 value : values) {
        (void)list.push(nbt::Tag{value});
    }
    return list;
}

[[nodiscard]] f64 list_f64(const nbt::Tag& compound, std::string_view name, usize index,
                           f64 fallback) {
    const nbt::Tag* tag = compound.find(name);
    if (tag == nullptr || tag->list() == nullptr || tag->list()->size() <= index) {
        return fallback;
    }
    return (*tag->list())[index].as_f64(fallback);
}

[[nodiscard]] i32 cost_of(i32 level) noexcept {
    return gameplay::experience_to_next_level(level, gameplay::ExperienceCurve{});
}

/// Points into a level from the file's progress float. Rounded, not
/// truncated: vanilla's float is 17/112 rounded to 24 bits, which multiplies
/// back to 16.9999…
[[nodiscard]] i32 points_of(f64 progress, i32 level) noexcept {
    const i32 cost = cost_of(level);
    if (cost <= 0) {
        return 0;
    }
    const auto points = static_cast<i32>(std::lround(progress * static_cast<f64>(cost)));
    return std::clamp(points, 0, cost - 1);
}

/// Whether a UUID is the modifier of one of the thirty-three effects. Those are
/// not read from `Attributes`: the effects put them back themselves, with the
/// name and amount their amplifier gives.
[[nodiscard]] std::optional<gameplay::Effect> effect_of_modifier(const net::Uuid& uuid) noexcept {
    for (usize i = 1; i <= gameplay::kEffectCount; ++i) {
        const auto effect = static_cast<gameplay::Effect>(i);
        const auto& info  = gameplay::effect_info(effect);
        if (info.modifier && info.modifier->uuid == uuid) {
            return effect;
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool knows_items(const ItemNames& names) noexcept {
    return names.registries != nullptr && names.item_registry.has_value();
}

/// A file entry this server can place: a slot of the player's window and an
/// item of this registry. Anything else is kept verbatim.
[[nodiscard]] std::optional<std::pair<usize, net::ItemStack>> item_of(const nbt::Tag& entry,
                                                                      const ItemNames& names) {
    const nbt::Tag* slot  = entry.find("Slot");
    const nbt::Tag* id    = entry.find("id");
    const nbt::Tag* count = entry.find("Count");
    if (slot == nullptr || id == nullptr || count == nullptr || !knows_items(names)) {
        return std::nullopt;
    }
    const auto window = window_slot_of_file(static_cast<i32>(slot->as_i64(-1000)));
    const auto item   = names.registries->protocol_id(*names.item_registry, id->as_string());
    if (!window || !item) {
        return std::nullopt;
    }
    net::ItemStack stack{*item, static_cast<i8>(count->as_i64()), {}};
    // Kept as the bytes the wire wants — a whole named document, as in
    // block_container.cpp — and re-parsed only when written back.
    if (const nbt::Tag* tag = entry.find("tag"); tag != nullptr) {
        stack.nbt = nbt::write(nbt::Document{"tag", *tag});
    }
    return std::pair{*window, std::move(stack)};
}

/// The record, as the effect rules see it while its effects are read back.
class RecordTarget final : public gameplay::EffectTarget {
public:
    explicit RecordTarget(PlayerRecord& record) noexcept : record_{record} {}

    [[nodiscard]] f32 health() const noexcept override { return record_.health; }

    [[nodiscard]] f32 max_health() const noexcept override {
        return static_cast<f32>(
            record_.attributes.value(gameplay::Attribute::MaxHealth).value_or(20.0));
    }

    void heal(f32 amount) override { record_.health = std::min(max_health(), record_.health + amount); }

    void hurt(gameplay::DamageKind /*kind*/, f32 /*amount*/,
              const gameplay::DamageConstants& /*constants*/) override {}

    [[nodiscard]] f32 absorption() const noexcept override { return record_.absorption; }
    void set_absorption(f32 amount) override { record_.absorption = std::max(amount, 0.0F); }

    [[nodiscard]] gameplay::AttributeMap* attributes() noexcept override {
        return &record_.attributes;
    }

    void clamp_health() override { record_.health = std::min(record_.health, max_health()); }

private:
    PlayerRecord& record_;
};

/// What vanilla writes for a player who has never done anything, for the keys
/// this server does not own. Measured on a fresh bot: `XpSeed` 0, `Fire` -20,
/// the ender chest an empty list. Brain, recipe book and warden tracker are
/// left out — vanilla builds its own when they are absent.
[[nodiscard]] nbt::Tag fresh_root() {
    nbt::Tag root = nbt::Tag::make_compound();
    put(root, "HurtByTimestamp", nbt::Tag{i32{0}});
    put(root, "SleepTimer", nbt::Tag{i16{0}});
    put(root, "Invulnerable", nbt::Tag::make_bool(false));
    put(root, "FallFlying", nbt::Tag::make_bool(false));
    put(root, "PortalCooldown", nbt::Tag{i32{0}});
    put(root, "XpSeed", nbt::Tag{i32{0}});
    put(root, "seenCredits", nbt::Tag::make_bool(false));
    put(root, "Score", nbt::Tag{i32{0}});
    put(root, "Fire", nbt::Tag{i16{-20}});
    put(root, "EnderItems", nbt::Tag::make_list(nbt::TagType::End));
    return root;
}

/// One `Attributes` entry, merged onto the one it came from when there is one.
[[nodiscard]] nbt::Tag attribute_entry(const gameplay::AttributeInstance& instance,
                                       const nbt::Tag* before, const PlayerRecord& record) {
    nbt::Tag entry = before != nullptr ? *before : nbt::Tag::make_compound();
    put(entry, "Base", nbt::Tag{instance.base()});
    if (instance.modifiers().empty()) {
        (void)entry.erase("Modifiers");  // measured: absent, not an empty list
    } else {
        const nbt::Tag* old_list = before != nullptr ? before->find("Modifiers") : nullptr;
        nbt::Tag        list     = nbt::Tag::make_list(nbt::TagType::Compound);
        for (const gameplay::AttributeModifier& modifier : instance.modifiers()) {
            std::string name{modifier.name};
            if (const auto effect = effect_of_modifier(modifier.uuid)) {
                // The name carries the amplifier — "effect.minecraft.speed 3",
                // measured — so it is rebuilt from the effect that holds it.
                std::array<char, 64> buffer{};
                if (const gameplay::EffectInstance* active = record.effects.get(*effect)) {
                    const usize n = gameplay::modifier_name(*effect, active->amplifier,
                                                            buffer.data(), buffer.size());
                    name.assign(buffer.data(), n);
                }
            }
            if (name.empty() && old_list != nullptr && old_list->list() != nullptr) {
                // A modifier read from the file carries no static name; its
                // own entry still has it.
                for (const nbt::Tag& old : *old_list->list()) {
                    if (uuid_from(old.find("UUID")) == modifier.uuid) {
                        name = std::string{old.find("Name") ? old.find("Name")->as_string() : ""};
                    }
                }
            }
            nbt::Tag out = nbt::Tag::make_compound();
            put(out, "Amount", nbt::Tag{modifier.amount});
            put(out, "Operation", nbt::Tag{static_cast<i32>(modifier.operation)});
            put(out, "UUID", uuid_tag(modifier.uuid));
            put(out, "Name", nbt::Tag{std::move(name)});
            (void)list.push(std::move(out));
        }
        put(entry, "Modifiers", std::move(list));
    }
    put(entry, "Name", nbt::Tag{std::string{gameplay::attribute_info(instance.attribute()).name}});
    return entry;
}

[[nodiscard]] PlayerDataError refuse(PlayerDataErrorKind kind, std::string message) {
    return PlayerDataError{kind, std::move(message)};
}

}  // namespace

// ── Slots ───────────────────────────────────────────────────────────────────

std::optional<i8> file_slot_of_window(usize window) noexcept {
    if (window >= 36 && window <= 44) {
        return static_cast<i8>(window - 36);
    }
    if (window >= 9 && window <= 35) {
        return static_cast<i8>(window);
    }
    if (window >= 5 && window <= 8) {
        return static_cast<i8>(103 - static_cast<i32>(window - 5));
    }
    if (window == 45) {
        return static_cast<i8>(-106);
    }
    return std::nullopt;
}

std::optional<usize> window_slot_of_file(i32 slot) noexcept {
    if (slot >= 0 && slot <= 8) {
        return static_cast<usize>(36 + slot);
    }
    if (slot >= 9 && slot <= 35) {
        return static_cast<usize>(slot);
    }
    if (slot >= 100 && slot <= 103) {
        return static_cast<usize>(5 + (103 - slot));
    }
    if (slot == -106) {
        return usize{45};
    }
    return std::nullopt;
}

Abilities Abilities::for_game_type(i32 game_type, bool was_flying) noexcept {
    Abilities out;
    switch (game_type) {
        case 1:  // creative
            out.may_fly      = true;
            out.instabuild   = true;
            out.invulnerable = true;
            break;
        case 2:  // adventure
            out.may_build = false;
            break;
        case 3:  // spectator
            out.may_fly      = true;
            out.invulnerable = true;
            out.may_build    = false;
            was_flying       = true;
            break;
        default:
            break;
    }
    out.flying = out.may_fly && was_flying;
    return out;
}

// ── Reading ─────────────────────────────────────────────────────────────────

std::optional<net::Uuid> uuid_of(const nbt::Tag& root) { return uuid_from(root.find("UUID")); }

std::expected<LoadedPlayer, PlayerDataError> read_player(const nbt::Tag& root,
                                                         const net::Uuid* expected_uuid,
                                                         const ItemNames& names,
                                                         std::string_view where) {
    if (root.compound() == nullptr) {
        return std::unexpected(refuse(PlayerDataErrorKind::NotNbt,
                                      fmt::format("{}: the root is not a compound", where)));
    }
    const nbt::Tag* version = root.find("DataVersion");
    if (version == nullptr) {
        return std::unexpected(refuse(
            PlayerDataErrorKind::NoDataVersion,
            fmt::format("{} declares no DataVersion; this is {} (Minecraft 1.20.1). Refusing to "
                        "read it, and leaving it untouched",
                        where, world::kDataVersion1201)));
    }
    if (version->as_i64() != world::kDataVersion1201) {
        return std::unexpected(refuse(
            PlayerDataErrorKind::WrongDataVersion,
            fmt::format("{} was written by data version {}, and this is {} (Minecraft 1.20.1). "
                        "There is no converter here: refusing to read it, and leaving it "
                        "untouched",
                        where, version->as_i64(), world::kDataVersion1201)));
    }
    if (expected_uuid != nullptr) {
        if (const auto stored = uuid_of(root); stored && *stored != *expected_uuid) {
            return std::unexpected(refuse(
                PlayerDataErrorKind::WrongUuid,
                fmt::format("{} belongs to {}, not to {}", where, stored->to_string(),
                            expected_uuid->to_string())));
        }
    }
    const nbt::Tag* dimension = root.find("Dimension");
    if (dimension != nullptr && dimension->as_string() != kOverworld) {
        return std::unexpected(refuse(
            PlayerDataErrorKind::UnsupportedDimension,
            fmt::format("{} stands in {}, and this server has only {}. Refusing the player rather "
                        "than moving them",
                        where, dimension->as_string(), kOverworld)));
    }

    LoadedPlayer  out;
    out.original         = root;
    PlayerRecord& record = out.record;

    record.x         = list_f64(root, "Pos", 0, record.x);
    record.y         = list_f64(root, "Pos", 1, record.y);
    record.z         = list_f64(root, "Pos", 2, record.z);
    record.motion_x  = list_f64(root, "Motion", 0, 0.0);
    record.motion_y  = list_f64(root, "Motion", 1, 0.0);
    record.motion_z  = list_f64(root, "Motion", 2, 0.0);
    record.yaw       = static_cast<f32>(list_f64(root, "Rotation", 0, 0.0));
    record.pitch     = static_cast<f32>(list_f64(root, "Rotation", 1, 0.0));
    record.on_ground = get_bool(root, "OnGround", true);
    record.fall_distance = static_cast<f32>(get_f64(root, "FallDistance", 0.0));

    record.game_type          = static_cast<i32>(get_i64(root, "playerGameType", 0));
    record.previous_game_type = static_cast<i32>(get_i64(root, "previousPlayerGameType", -1));
    if (const nbt::Tag* abilities = root.find("abilities"); abilities != nullptr) {
        record.abilities.flying       = get_bool(*abilities, "flying", false);
        record.abilities.may_fly      = get_bool(*abilities, "mayfly", false);
        record.abilities.instabuild   = get_bool(*abilities, "instabuild", false);
        record.abilities.invulnerable = get_bool(*abilities, "invulnerable", false);
        record.abilities.may_build    = get_bool(*abilities, "mayBuild", true);
        record.abilities.fly_speed    = static_cast<f32>(get_f64(*abilities, "flySpeed", 0.05));
        record.abilities.walk_speed   = static_cast<f32>(get_f64(*abilities, "walkSpeed", 0.1));
    }

    record.hurt_time  = static_cast<i16>(get_i64(root, "HurtTime", 0));
    record.death_time = static_cast<i16>(get_i64(root, "DeathTime", 0));
    record.air        = static_cast<i16>(get_i64(root, "Air", 300));

    record.food.food       = static_cast<i32>(get_i64(root, "foodLevel", 20));
    record.food.saturation = static_cast<f32>(get_f64(root, "foodSaturationLevel", 5.0));
    record.food.exhaustion = static_cast<f32>(get_f64(root, "foodExhaustionLevel", 0.0));
    record.food.tick_timer = static_cast<i32>(get_i64(root, "foodTickTimer", 0));

    record.xp_level  = static_cast<i32>(get_i64(root, "XpLevel", 0));
    record.xp_total  = static_cast<i32>(get_i64(root, "XpTotal", 0));
    record.xp_points = points_of(get_f64(root, "XpP", 0.0), record.xp_level);

    record.selected_slot = std::clamp(static_cast<i32>(get_i64(root, "SelectedItemSlot", 0)), 0, 8);

    if (const nbt::Tag* items = root.find("Inventory"); items != nullptr && items->list() != nullptr) {
        for (const nbt::Tag& entry : *items->list()) {
            if (auto placed = item_of(entry, names)) {
                record.inventory[placed->first] = std::move(placed->second);
            } else {
                ++out.unknown_items;  // stays in `original`, written back as is
            }
        }
    }
    if (const nbt::Tag* ender = root.find("EnderItems"); ender != nullptr && ender->list() != nullptr) {
        for (const nbt::Tag& entry : *ender->list()) {
            const nbt::Tag* id = entry.find("id");
            if (id == nullptr || !knows_items(names) ||
                !names.registries->protocol_id(*names.item_registry, id->as_string())) {
                ++out.unknown_items;
            }
        }
    }

    // Attributes before effects: a base set by `/attribute` is the player's
    // own, and an effect's modifier is put back by the effect itself.
    if (const nbt::Tag* list = root.find("Attributes"); list != nullptr && list->list() != nullptr) {
        for (const nbt::Tag& entry : *list->list()) {
            const nbt::Tag* name = entry.find("Name");
            const auto attribute =
                name != nullptr ? gameplay::attribute_from_name(name->as_string()) : std::nullopt;
            gameplay::AttributeInstance* instance =
                attribute ? record.attributes.get(*attribute) : nullptr;
            if (instance == nullptr) {
                continue;  // not one a player owns here: kept in `original`
            }
            if (const nbt::Tag* base = entry.find("Base")) {
                instance->set_base(base->as_f64(instance->base()));
            }
            const nbt::Tag* modifiers = entry.find("Modifiers");
            if (modifiers == nullptr || modifiers->list() == nullptr) {
                continue;
            }
            for (const nbt::Tag& modifier : *modifiers->list()) {
                const auto uuid = uuid_from(modifier.find("UUID"));
                if (!uuid || effect_of_modifier(*uuid)) {
                    continue;
                }
                const auto operation = std::clamp<i64>(get_i64(modifier, "Operation", 0), 0, 2);
                (void)instance->add_modifier(gameplay::AttributeModifier{
                    *uuid, {}, get_f64(modifier, "Amount", 0.0),
                    static_cast<gameplay::AttributeOperation>(operation)});
            }
        }
    }

    if (const nbt::Tag* list = root.find("ActiveEffects"); list != nullptr) {
        RecordTarget target{record};
        (void)gameplay::load_effects(record.effects, *list, target);
        if (list->list() != nullptr) {
            for (const nbt::Tag& entry : *list->list()) {
                if (!gameplay::effect_from_id(static_cast<i32>(get_i64(entry, "Id", -1)))) {
                    ++out.unknown_effects;
                }
            }
        }
    }

    // Last, over whatever reading the effects did to them: the file's own
    // numbers are the truth. Absorption in particular was granted again by
    // re-adding the effect, and the file says how much of it is left.
    record.absorption = static_cast<f32>(get_f64(root, "AbsorptionAmount", 0.0));
    record.health     = static_cast<f32>(get_f64(root, "Health", 20.0));
    return out;
}

// ── Writing ─────────────────────────────────────────────────────────────────

nbt::Tag write_player(const PlayerRecord& record, const nbt::Tag* original, const net::Uuid& uuid,
                      const ItemNames& names) {
    const bool merge = original != nullptr && original->compound() != nullptr;
    nbt::Tag   root  = merge ? *original : fresh_root();

    // Read what the merge needs before anything is overwritten.
    i32 previous = record.previous_game_type;
    if (const nbt::Tag* before = root.find("playerGameType")) {
        const auto was = static_cast<i32>(before->as_i64());
        if (was != record.game_type) {
            previous = was;  // the mode changed here: vanilla keeps the old one
        } else if (previous == -1) {
            previous = static_cast<i32>(get_i64(root, "previousPlayerGameType", -1));
        }
    }
    bool keep_progress = false;
    if (const nbt::Tag* progress = root.find("XpP");
        progress != nullptr && progress->type() == nbt::TagType::Float) {
        // An unchanged level keeps vanilla's own float, bit for bit: it is a
        // sum of float divisions, and recomputing it as one division can move
        // the last bit.
        keep_progress = get_i64(root, "XpLevel", -1) == record.xp_level &&
                        points_of(progress->as_f64(), record.xp_level) == record.xp_points;
    }
    const nbt::Tag* old_abilities = root.find("abilities");
    const bool      was_flying =
        old_abilities != nullptr && get_bool(*old_abilities, "flying", false);

    put(root, "AbsorptionAmount", nbt::Tag{record.absorption});
    {
        nbt::Tag abilities = old_abilities != nullptr && old_abilities->compound() != nullptr
                                 ? *old_abilities
                                 : nbt::Tag::make_compound();
        put(abilities, "invulnerable", nbt::Tag::make_bool(record.abilities.invulnerable));
        put(abilities, "mayfly", nbt::Tag::make_bool(record.abilities.may_fly));
        put(abilities, "instabuild", nbt::Tag::make_bool(record.abilities.instabuild));
        put(abilities, "walkSpeed", nbt::Tag{record.abilities.walk_speed});
        put(abilities, "mayBuild", nbt::Tag::make_bool(record.abilities.may_build));
        // The client says when it starts flying and this server does not
        // listen yet, so a flight vanilla recorded is kept while flying is
        // still allowed.
        put(abilities, "flying", nbt::Tag::make_bool(record.abilities.may_fly &&
                                                     (record.abilities.flying || was_flying)));
        put(abilities, "flySpeed", nbt::Tag{record.abilities.fly_speed});
        put(root, "abilities", std::move(abilities));
    }
    put(root, "FallDistance", nbt::Tag{record.fall_distance});
    put(root, "DeathTime", nbt::Tag{record.death_time});
    put(root, "XpTotal", nbt::Tag{record.xp_total});
    put(root, "UUID", uuid_tag(uuid));
    put(root, "playerGameType", nbt::Tag{record.game_type});
    if (previous != -1) {
        put(root, "previousPlayerGameType", nbt::Tag{previous});
    } else {
        (void)root.erase("previousPlayerGameType");  // measured: absent when none
    }
    put(root, "Motion", doubles({record.motion_x, record.motion_y, record.motion_z}));
    put(root, "Health", nbt::Tag{record.health});
    put(root, "foodSaturationLevel", nbt::Tag{record.food.saturation});
    put(root, "Air", nbt::Tag{record.air});
    put(root, "OnGround", nbt::Tag::make_bool(record.on_ground));
    put(root, "Dimension", nbt::Tag{record.dimension});
    put(root, "Rotation", floats({record.yaw, record.pitch}));
    put(root, "XpLevel", nbt::Tag{record.xp_level});
    put(root, "Pos", doubles({record.x, record.y, record.z}));
    if (!keep_progress) {
        const i32 cost = cost_of(record.xp_level);
        put(root, "XpP", nbt::Tag{cost > 0 ? static_cast<f32>(record.xp_points) /
                                                 static_cast<f32>(cost)
                                           : 0.0F});
    }
    put(root, "DataVersion", nbt::Tag{world::kDataVersion1201});
    put(root, "foodLevel", nbt::Tag{record.food.food});
    put(root, "foodExhaustionLevel", nbt::Tag{record.food.exhaustion});
    put(root, "HurtTime", nbt::Tag{record.hurt_time});
    put(root, "SelectedItemSlot", nbt::Tag{record.selected_slot});

    // ── Effects: ours, then any the file had that this registry does not ────
    {
        nbt::Tag list = gameplay::save_effects(record.effects)
                            .value_or(nbt::Tag::make_list(nbt::TagType::Compound));
        if (const nbt::Tag* before = root.find("ActiveEffects");
            before != nullptr && before->list() != nullptr) {
            for (const nbt::Tag& entry : *before->list()) {
                if (!gameplay::effect_from_id(static_cast<i32>(get_i64(entry, "Id", -1)))) {
                    (void)list.push(entry);
                }
            }
        }
        if (list.empty()) {
            (void)root.erase("ActiveEffects");  // measured: absent, not empty
        } else {
            put(root, "ActiveEffects", std::move(list));
        }
    }

    // ── Inventory ───────────────────────────────────────────────────────────
    {
        nbt::Tag              items = nbt::Tag::make_list(nbt::TagType::Compound);
        std::array<bool, 256> taken{};
        for (const usize window : kWriteOrder) {
            const net::ItemStack& stack = record.inventory[window];
            if (stack.empty() || !knows_items(names)) {
                continue;
            }
            const std::string_view name = names.registries->entry_of(*names.item_registry,
                                                                     stack.item_id);
            const auto slot = file_slot_of_window(window);
            if (name.empty() || !slot) {
                continue;
            }
            nbt::Tag entry = nbt::Tag::make_compound();
            put(entry, "Slot", nbt::Tag{*slot});
            put(entry, "id", nbt::Tag{std::string{name}});
            put(entry, "Count", nbt::Tag{stack.count});
            if (!stack.nbt.empty()) {
                if (const auto document = nbt::read(stack.nbt)) {
                    put(entry, "tag", document->root);
                }
            }
            taken[static_cast<u8>(*slot)] = true;
            (void)items.push(std::move(entry));
        }
        // What the file held that this server could not place: an item of
        // another registry, a slot of another inventory. Kept, unless one of
        // ours now sits in the same slot.
        if (const nbt::Tag* before = root.find("Inventory");
            before != nullptr && before->list() != nullptr) {
            for (const nbt::Tag& entry : *before->list()) {
                if (item_of(entry, names)) {
                    continue;
                }
                const auto slot = static_cast<u8>(static_cast<i8>(get_i64(entry, "Slot", 0)));
                if (!taken[slot]) {
                    taken[slot] = true;
                    (void)items.push(entry);
                }
            }
        }
        put(root, "Inventory",
            items.empty() ? nbt::Tag::make_list(nbt::TagType::End) : std::move(items));
    }
    put(root, "foodTickTimer", nbt::Tag{record.food.tick_timer});

    // ── Attributes: merged entry by entry ───────────────────────────────────
    {
        const gameplay::AttributeMap defaults = gameplay::AttributeMap::player();
        std::array<bool, gameplay::kAttributeCount> written{};
        nbt::Tag list = nbt::Tag::make_list(nbt::TagType::Compound);
        if (const nbt::Tag* before = root.find("Attributes");
            before != nullptr && before->list() != nullptr) {
            for (const nbt::Tag& entry : *before->list()) {
                const nbt::Tag* name = entry.find("Name");
                const auto attribute = name != nullptr
                                           ? gameplay::attribute_from_name(name->as_string())
                                           : std::nullopt;
                const gameplay::AttributeInstance* instance =
                    attribute ? record.attributes.get(*attribute) : nullptr;
                if (instance == nullptr) {
                    (void)list.push(entry);
                    continue;
                }
                (void)list.push(attribute_entry(*instance, &entry, record));
                written[static_cast<usize>(*attribute)] = true;
            }
        }
        // Vanilla lists only the attributes something has touched; so does
        // this — a base that moved, or a modifier.
        for (usize a = 0; a < gameplay::kAttributeCount; ++a) {
            const auto                         attribute = static_cast<gameplay::Attribute>(a);
            const gameplay::AttributeInstance* instance  = record.attributes.get(attribute);
            if (instance == nullptr || written[a]) {
                continue;
            }
            const gameplay::AttributeInstance* fresh = defaults.get(attribute);
            if (!instance->modifiers().empty() ||
                (fresh != nullptr && fresh->base() != instance->base())) {
                (void)list.push(attribute_entry(*instance, nullptr, record));
            }
        }
        if (!list.empty() || root.contains("Attributes")) {
            put(root, "Attributes", std::move(list));
        }
    }
    return root;
}

std::vector<u8> encode_player_file(const nbt::Tag& root) {
    auto compressed = io::gzip_compress(nbt::write(nbt::Document{"", root}));
    return compressed ? std::move(*compressed) : std::vector<u8>{};
}

std::expected<nbt::Tag, PlayerDataError> decode_player_file(std::span<const u8> bytes,
                                                            std::string_view where) {
    const auto plain = io::gzip_decompress(bytes);
    if (!plain) {
        return std::unexpected(refuse(
            PlayerDataErrorKind::Unreadable,
            fmt::format("{} is not a gzip file; refusing it, and leaving it untouched", where)));
    }
    auto document = nbt::read(*plain);
    if (!document) {
        return std::unexpected(refuse(
            PlayerDataErrorKind::NotNbt,
            fmt::format("{} does not parse as NBT ({}); refusing it, and leaving it untouched",
                        where, nbt::to_string(document.error()))));
    }
    return std::move(document->root);
}

// ── The live state ──────────────────────────────────────────────────────────

PlayerRecord capture_player(const PlayerPose& pose, i32 game_type,
                            std::span<const net::ItemStack> inventory,
                            const net::ItemStack& carried, i16 held_slot,
                            const SurvivalSession& survival, const EffectSession& effects,
                            usize* overflow) {
    PlayerRecord record;
    record.x         = pose.x;
    record.y         = pose.y;
    record.z         = pose.z;
    record.yaw       = pose.yaw;
    record.pitch     = pose.pitch;
    record.on_ground = pose.on_ground;
    // The server does not integrate a player's velocity — the client does —
    // so what is written is the resting value on the ground and nothing in
    // the air. Stated rather than invented.
    record.motion_y      = pose.on_ground ? kRestingMotionY : 0.0;
    record.fall_distance = survival.health.fall_distance;

    record.game_type = game_type;
    record.abilities = Abilities::for_game_type(game_type, false);

    record.health     = survival.health.health;
    record.absorption = survival.health.absorption;
    record.hurt_time  = static_cast<i16>(std::clamp(survival.health.hurt_ticks, 0, 32767));
    record.air        = static_cast<i16>(std::clamp(survival.health.air, -32768, 32767));
    record.food       = survival.food;
    record.xp_level   = survival.experience_level;
    record.xp_points  = survival.experience_points;
    record.xp_total   = survival.experience_total;

    const usize count = std::min(inventory.size(), kPlayerSlots);
    for (usize w = 5; w < count; ++w) {
        record.inventory[w] = inventory[w];
    }
    // The 2x2 grid and the cursor go back into the inventory, where vanilla
    // puts them when the screen closes: first free slot, hotbar first.
    const auto give_back = [&](const net::ItemStack& stack) {
        if (stack.empty()) {
            return;
        }
        for (const usize w : kWriteOrder) {
            if (w >= 5 && w <= 8) {
                break;  // armour and off hand are not free slots
            }
            if (record.inventory[w].empty()) {
                record.inventory[w] = stack;
                return;
            }
        }
        if (overflow != nullptr) {
            ++*overflow;
        }
    };
    for (usize w = 1; w <= 4 && w < count; ++w) {
        give_back(inventory[w]);
    }
    give_back(carried);
    record.selected_slot = std::clamp<i32>(held_slot, 0, 8);

    record.effects    = effects.effects;
    record.attributes = effects.attributes;
    record.effects.clear_events();
    return record;
}

void restore_player(const PlayerRecord& record, PlayerPose& pose, std::span<net::ItemStack> inventory,
                    i16& held_slot, SurvivalSession& survival, EffectSession& effects) {
    pose.x         = record.x;
    pose.y         = record.y;
    pose.z         = record.z;
    pose.yaw       = record.yaw;
    pose.pitch     = record.pitch;
    pose.on_ground = record.on_ground;

    for (usize w = 0; w < inventory.size(); ++w) {
        inventory[w] = w < kPlayerSlots && w >= 5 ? record.inventory[w] : net::ItemStack{};
    }
    held_slot = static_cast<i16>(record.selected_slot);

    // Effects and attributes first: the maximum health is theirs.
    effects.effects    = record.effects;
    effects.attributes = record.attributes;
    survival.health.max_health = static_cast<f32>(
        record.attributes.value(gameplay::Attribute::MaxHealth).value_or(20.0));
    survival.mitigation.resistance = record.effects.amplifier(gameplay::Effect::Resistance);

    survival.health.health        = record.health;
    survival.health.absorption    = record.absorption;
    survival.health.hurt_ticks    = record.hurt_time;
    survival.health.air           = record.air;
    survival.health.fall_distance = record.fall_distance;
    // Saved dead: the first Set Health says zero, the client shows the death
    // screen, and the respawn button works as it would have.
    survival.health.dead      = record.health <= 0.0F;
    survival.awaiting_respawn = survival.health.dead;

    survival.food              = record.food;
    survival.experience_level  = record.xp_level;
    survival.experience_points = record.xp_points;
    survival.experience_total  = record.xp_total;
    survival.needs_first_update = true;
}

// ── The store ───────────────────────────────────────────────────────────────

PlayerDataStore::PlayerDataStore(std::filesystem::path world_dir, ItemNames names, std::string host)
    : directory_{std::move(world_dir) / "playerdata"}, names_{names}, host_{std::move(host)} {}

std::filesystem::path PlayerDataStore::file_of(const net::Uuid& uuid) const {
    return directory_ / (uuid.to_string() + ".dat");
}

void PlayerDataStore::load_level_player(const std::filesystem::path& level_dat) {
    const auto bytes = io::read_file(level_dat);
    if (!bytes) {
        return;
    }
    const auto root = decode_player_file(*bytes, level_dat.string());
    if (!root) {
        return;  // level.dat's own reader names a broken file
    }
    if (const nbt::Tag* data = root->find("Data"); data != nullptr) {
        if (const nbt::Tag* player = data->find("Player"); player != nullptr) {
            const std::scoped_lock lock{mutex_};
            level_player_ = *player;
        }
    }
}

std::expected<std::optional<LoadedPlayer>, PlayerDataError> PlayerDataStore::load(
    const net::Uuid& uuid, std::string_view name) {
    const std::scoped_lock lock{mutex_};
    const std::string      key = uuid.to_string();

    const auto finish = [&](std::expected<LoadedPlayer, PlayerDataError> loaded)
        -> std::expected<std::optional<LoadedPlayer>, PlayerDataError> {
        if (!loaded) {
            refused_.insert(key);
            return std::unexpected(std::move(loaded.error()));
        }
        refused_.erase(key);
        originals_[key] = loaded->original;
        return std::optional<LoadedPlayer>{std::move(*loaded)};
    };

    if (!host_.empty() && name == host_ && level_player_) {
        return finish(read_player(*level_player_, nullptr, names_, "level.dat Data.Player"));
    }

    const auto            path = file_of(uuid);
    std::error_code       error;
    if (!std::filesystem::exists(path, error)) {
        refused_.erase(key);
        return std::optional<LoadedPlayer>{};
    }
    const auto bytes = io::read_file(path);
    if (!bytes) {
        refused_.insert(key);
        return std::unexpected(refuse(
            PlayerDataErrorKind::Unreadable,
            fmt::format("{} could not be read ({}); refusing it, and leaving it untouched",
                        path.string(), io::to_string(bytes.error()))));
    }
    auto root = decode_player_file(*bytes, path.string());
    if (!root) {
        refused_.insert(key);
        return std::unexpected(std::move(root.error()));
    }
    return finish(read_player(*root, &uuid, names_, path.string()));
}

bool PlayerDataStore::save(const net::Uuid& uuid, std::string_view name,
                           const PlayerRecord& record) {
    const std::scoped_lock lock{mutex_};
    const std::string      key = uuid.to_string();
    if (refused_.contains(key)) {
        return false;  // never write over a file that was refused
    }
    const auto      found    = originals_.find(key);
    const nbt::Tag* original = found != originals_.end() ? &found->second : nullptr;
    const bool      host     = !host_.empty() && name == host_;
    // The host's first save onto a vanilla singleplayer world merges onto the
    // record vanilla kept in level.dat, if the playerdata file had nothing.
    if (original == nullptr && host && level_player_) {
        original = &*level_player_;
    }
    nbt::Tag   root  = write_player(record, original, uuid, names_);
    const auto bytes = encode_player_file(root);
    if (bytes.empty()) {
        return false;
    }

    std::error_code error;
    std::filesystem::create_directories(directory_, error);
    const auto path = file_of(uuid);
    // Vanilla's rule, measured: after a save, `.dat_old` holds the save
    // before it. A copy rather than a rename, so `.dat` never goes missing.
    if (std::filesystem::exists(path, error)) {
        auto old = path;
        old += "_old";
        std::filesystem::copy_file(path, old, std::filesystem::copy_options::overwrite_existing,
                                   error);
    }
    if (!io::write_file_atomic(path, bytes)) {
        return false;
    }
    if (host) {
        nbt::Tag in_level = root;
        // level.dat's player is the vanilla account; keep its UUID when it
        // had one, so vanilla does not see a stranger in its own seat.
        if (level_player_) {
            if (const nbt::Tag* account = level_player_->find("UUID")) {
                put(in_level, "UUID", *account);
            }
        }
        level_player_ = std::move(in_level);
    }
    originals_[key] = std::move(root);
    return true;
}

std::vector<u8> PlayerDataStore::encode_level_dat(const world::LevelSettings& settings) {
    nbt::Document document = world::make_level_dat(settings);
    {
        const std::scoped_lock lock{mutex_};
        if (level_player_) {
            if (nbt::Tag* data = document.root.find("Data")) {
                put(*data, "Player", *level_player_);
            }
        }
    }
    auto compressed = io::gzip_compress(nbt::write(document));
    return compressed ? std::move(*compressed) : std::vector<u8>{};
}

}  // namespace ov::server
