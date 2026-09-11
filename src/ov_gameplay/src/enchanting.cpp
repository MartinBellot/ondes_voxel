#include "ov/gameplay/enchanting.hpp"

#include "ov/gameplay/durability.hpp"
#include "ov/nbt/tag.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <string>

namespace ov::gameplay {
namespace {

using E = Enchantment;
using C = EnchantCategory;

// name, max level, weight, category, treasure, curse, discoverable,
// min base, min per level, max kind, max value, max per level.
//
// Weights and the level windows are the wiki's (pre-1.21 revisions of
// "Enchanting mechanics" and "Enchanting/Levels"; see
// docs/provenance/enchantement.md). The table campaign checks every weight and
// every window a table can reach; the windows above a modified level of about
// 50 cannot be reached by a table and are the documentation's alone.
constexpr std::array<EnchantmentInfo, kEnchantmentCount> kInfo{{
    {"minecraft:protection", 4, 10, C::Armor, false, false, true, 1, 11, MaxCost::MinPlus, 11, 0},
    {"minecraft:fire_protection", 4, 5, C::Armor, false, false, true, 10, 8, MaxCost::MinPlus, 8, 0},
    {"minecraft:feather_falling", 4, 5, C::ArmorFeet, false, false, true, 5, 6, MaxCost::MinPlus, 6, 0},
    {"minecraft:blast_protection", 4, 2, C::Armor, false, false, true, 5, 8, MaxCost::MinPlus, 8, 0},
    {"minecraft:projectile_protection", 4, 5, C::Armor, false, false, true, 3, 6, MaxCost::MinPlus, 6, 0},
    {"minecraft:respiration", 3, 2, C::ArmorHead, false, false, true, 10, 10, MaxCost::MinPlus, 30, 0},
    {"minecraft:aqua_affinity", 1, 2, C::ArmorHead, false, false, true, 1, 0, MaxCost::MinPlus, 40, 0},
    {"minecraft:thorns", 3, 1, C::ArmorChest, false, false, true, 10, 20, MaxCost::Linear, 60, 10},
    {"minecraft:depth_strider", 3, 2, C::ArmorFeet, false, false, true, 10, 10, MaxCost::MinPlus, 15, 0},
    {"minecraft:frost_walker", 2, 2, C::ArmorFeet, true, false, true, 10, 10, MaxCost::MinPlus, 15, 0},
    {"minecraft:binding_curse", 1, 1, C::Wearable, true, true, true, 25, 0, MaxCost::Constant, 50, 0},
    {"minecraft:soul_speed", 3, 1, C::ArmorFeet, true, false, false, 10, 10, MaxCost::MinPlus, 15, 0},
    {"minecraft:swift_sneak", 3, 1, C::ArmorLegs, true, false, false, 25, 25, MaxCost::MinPlus, 50, 0},
    {"minecraft:sharpness", 5, 10, C::Weapon, false, false, true, 1, 11, MaxCost::MinPlus, 20, 0},
    {"minecraft:smite", 5, 5, C::Weapon, false, false, true, 5, 8, MaxCost::MinPlus, 20, 0},
    {"minecraft:bane_of_arthropods", 5, 5, C::Weapon, false, false, true, 5, 8, MaxCost::MinPlus, 20, 0},
    {"minecraft:knockback", 2, 5, C::Weapon, false, false, true, 5, 20, MaxCost::MinPlus, 50, 0},
    {"minecraft:fire_aspect", 2, 2, C::Weapon, false, false, true, 10, 20, MaxCost::MinPlus, 50, 0},
    {"minecraft:looting", 3, 2, C::Weapon, false, false, true, 15, 9, MaxCost::MinPlus, 50, 0},
    {"minecraft:sweeping", 3, 2, C::Weapon, false, false, true, 5, 9, MaxCost::MinPlus, 15, 0},
    {"minecraft:efficiency", 5, 10, C::Digger, false, false, true, 1, 10, MaxCost::MinPlus, 50, 0},
    {"minecraft:silk_touch", 1, 1, C::Digger, false, false, true, 15, 0, MaxCost::MinPlus, 50, 0},
    {"minecraft:unbreaking", 3, 5, C::Breakable, false, false, true, 5, 8, MaxCost::MinPlus, 50, 0},
    {"minecraft:fortune", 3, 2, C::Digger, false, false, true, 15, 9, MaxCost::MinPlus, 50, 0},
    {"minecraft:power", 5, 10, C::Bow, false, false, true, 1, 10, MaxCost::MinPlus, 15, 0},
    {"minecraft:punch", 2, 2, C::Bow, false, false, true, 12, 20, MaxCost::MinPlus, 25, 0},
    {"minecraft:flame", 1, 2, C::Bow, false, false, true, 20, 0, MaxCost::Constant, 50, 0},
    {"minecraft:infinity", 1, 1, C::Bow, false, false, true, 20, 0, MaxCost::Constant, 50, 0},
    {"minecraft:luck_of_the_sea", 3, 2, C::FishingRod, false, false, true, 15, 9, MaxCost::MinPlus, 50, 0},
    {"minecraft:lure", 3, 2, C::FishingRod, false, false, true, 15, 9, MaxCost::MinPlus, 50, 0},
    {"minecraft:loyalty", 3, 5, C::Trident, false, false, true, 12, 7, MaxCost::Constant, 50, 0},
    {"minecraft:impaling", 5, 2, C::Trident, false, false, true, 1, 8, MaxCost::MinPlus, 20, 0},
    {"minecraft:riptide", 3, 2, C::Trident, false, false, true, 17, 7, MaxCost::Constant, 50, 0},
    {"minecraft:channeling", 1, 1, C::Trident, false, false, true, 25, 0, MaxCost::Constant, 50, 0},
    {"minecraft:multishot", 1, 2, C::Crossbow, false, false, true, 20, 0, MaxCost::Constant, 50, 0},
    {"minecraft:quick_charge", 3, 5, C::Crossbow, false, false, true, 12, 20, MaxCost::Constant, 50, 0},
    {"minecraft:piercing", 4, 10, C::Crossbow, false, false, true, 1, 10, MaxCost::Constant, 50, 0},
    {"minecraft:mending", 1, 2, C::Breakable, true, false, true, 25, 0, MaxCost::MinPlus, 50, 0},
    {"minecraft:vanishing_curse", 1, 1, C::Vanishable, true, true, true, 25, 0, MaxCost::Constant, 50, 0},
}};

[[nodiscard]] std::string_view bare(std::string_view name) noexcept {
    constexpr std::string_view kPrefix = "minecraft:";
    return name.starts_with(kPrefix) ? name.substr(kPrefix.size()) : name;
}

enum class Piece : u8 { None, Helmet, Chestplate, Leggings, Boots };

[[nodiscard]] Piece armour_piece(std::string_view item) noexcept {
    const std::string_view b = bare(item);
    if (b == "turtle_helmet") {
        return Piece::Helmet;
    }
    for (const std::string_view material :
         {"leather_", "chainmail_", "iron_", "golden_", "diamond_", "netherite_"}) {
        if (!b.starts_with(material)) {
            continue;
        }
        const std::string_view rest = b.substr(material.size());
        if (rest == "helmet") {
            return Piece::Helmet;
        }
        if (rest == "chestplate") {
            return Piece::Chestplate;
        }
        if (rest == "leggings") {
            return Piece::Leggings;
        }
        if (rest == "boots") {
            return Piece::Boots;
        }
    }
    return Piece::None;
}

[[nodiscard]] bool is_tool_of(std::string_view item, std::string_view kind) noexcept {
    const std::string_view b = bare(item);
    for (const std::string_view tier :
         {"wooden_", "stone_", "iron_", "golden_", "diamond_", "netherite_"}) {
        if (b.starts_with(tier) && b.substr(tier.size()) == kind) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool is_sword(std::string_view item) noexcept { return is_tool_of(item, "sword"); }
[[nodiscard]] bool is_axe(std::string_view item) noexcept { return is_tool_of(item, "axe"); }
[[nodiscard]] bool is_digger(std::string_view item) noexcept {
    return is_tool_of(item, "pickaxe") || is_axe(item) || is_tool_of(item, "shovel") ||
           is_tool_of(item, "hoe");
}

[[nodiscard]] bool is_head_block(std::string_view b) noexcept {
    return b == "carved_pumpkin" || b == "player_head" || b == "zombie_head" ||
           b == "creeper_head" || b == "dragon_head" || b == "piglin_head" ||
           b == "skeleton_skull" || b == "wither_skeleton_skull";
}

[[nodiscard]] bool is_book(std::string_view item) noexcept { return bare(item) == "book"; }
[[nodiscard]] bool is_enchanted_book(std::string_view item) noexcept {
    return bare(item) == "enchanted_book";
}

/// `Math.round(float)`: half rounds up, computed without the double rounding
/// that `floor(x + 0.5f)` suffers just below one half.
[[nodiscard]] i32 java_round(f32 x) noexcept {
    const f32 down = std::floor(x);
    const f32 up   = (x - down >= 0.5F) ? down + 1.0F : down;
    if (up >= static_cast<f32>(INT_MAX)) {
        return INT_MAX;
    }
    return static_cast<i32>(up);
}

/// Java's `int` addition: wraps.
[[nodiscard]] i32 wrap_add(i32 a, i32 b) noexcept {
    return static_cast<i32>(static_cast<u32>(a) + static_cast<u32>(b));
}

/// One protection enchantment's own compatibility test, before the symmetric
/// check `compatible` makes of it.
[[nodiscard]] bool one_way(E self, E other) noexcept {
    if (self == other) {
        return false;
    }
    const auto protection = [](E e) {
        return e == E::Protection || e == E::FireProtection || e == E::FeatherFalling ||
               e == E::BlastProtection || e == E::ProjectileProtection;
    };
    if (protection(self) && protection(other)) {
        // Two protections exclude each other unless one is the fall kind.
        return self == E::FeatherFalling || other == E::FeatherFalling;
    }
    const auto damage = [](E e) {
        return e == E::Sharpness || e == E::Smite || e == E::BaneOfArthropods;
    };
    if (damage(self) && damage(other)) {
        return false;
    }
    const auto loot = [](E e) {
        return e == E::Fortune || e == E::Looting || e == E::LuckOfTheSea;
    };
    if (self == E::SilkTouch && other == E::Fortune) {
        return false;
    }
    if (loot(self) && other == E::SilkTouch) {
        return false;
    }
    if ((self == E::DepthStrider && other == E::FrostWalker) ||
        (self == E::FrostWalker && other == E::DepthStrider)) {
        return false;
    }
    if (self == E::Infinity && other == E::Mending) {
        return false;
    }
    if (self == E::Riptide && (other == E::Loyalty || other == E::Channeling)) {
        return false;
    }
    if ((self == E::Multishot && other == E::Piercing) ||
        (self == E::Piercing && other == E::Multishot)) {
        return false;
    }
    return true;
}

/// Step two: every enchantment the item may take at this modified level, at
/// the highest level whose window holds it, in registry order.
[[nodiscard]] EnchantmentList candidates(std::string_view item, i32 level, bool treasure) {
    EnchantmentList out;
    const bool      book = is_book(item);
    for (usize i = 0; i < kEnchantmentCount; ++i) {
        const auto             e    = static_cast<E>(i);
        const EnchantmentInfo& info = kInfo[i];
        if ((info.treasure && !treasure) || !info.discoverable) {
            continue;
        }
        if (!book && !category_accepts(info.category, item)) {
            continue;
        }
        for (i32 l = info.max_level; l >= 1; --l) {
            if (level >= min_cost(e, l) && level <= max_cost(e, l)) {
                out.push({e, l});
                break;
            }
        }
    }
    return out;
}

/// `WeightedRandom.getRandomItem`: one draw of `nextInt(total)`.
[[nodiscard]] std::optional<EnchantmentLevel> weighted(math::LegacyRandomSource& random,
                                                       const EnchantmentList&     list) {
    i32 total = 0;
    for (const EnchantmentLevel& e : list) {
        total += enchantment_info(e.enchantment).weight;
    }
    if (total <= 0) {
        return std::nullopt;
    }
    i32 pick = random.next_int(total);
    for (const EnchantmentLevel& e : list) {
        pick -= enchantment_info(e.enchantment).weight;
        if (pick < 0) {
            return e;
        }
    }
    return std::nullopt;
}

/// `getEnchantmentList`: reseed at `seed + slot`, select, and drop one at random
/// from a book's list. Leaves `random` where the table's clue draw expects it.
[[nodiscard]] EnchantmentList list_for(math::LegacyRandomSource& random, i32 seed, i32 slot,
                                       i32 cost, std::string_view item) {
    random.set_seed(static_cast<i64>(wrap_add(seed, slot)));
    EnchantmentList list = select_enchantments(random, item, cost, false);
    if (is_book(item) && list.size() > 1) {
        list.erase_at(static_cast<usize>(random.next_int(static_cast<i32>(list.size()))));
    }
    return list;
}

[[nodiscard]] bool blank(std::string_view s) noexcept {
    return std::all_of(s.begin(), s.end(), [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    });
}

}  // namespace

// ── The enchantments themselves ─────────────────────────────────────────────

const EnchantmentInfo& enchantment_info(Enchantment enchantment) noexcept {
    const auto index = static_cast<usize>(enchantment);
    return kInfo[index < kEnchantmentCount ? index : 0];
}

std::optional<Enchantment> enchantment_from_name(std::string_view name) noexcept {
    const std::string_view wanted = bare(name);
    for (usize i = 0; i < kEnchantmentCount; ++i) {
        if (bare(kInfo[i].name) == wanted) {
            return static_cast<Enchantment>(i);
        }
    }
    return std::nullopt;
}

i32 min_cost(Enchantment enchantment, i32 level) noexcept {
    const EnchantmentInfo& info = enchantment_info(enchantment);
    return info.min_base + info.min_per_level * (level - 1);
}

i32 max_cost(Enchantment enchantment, i32 level) noexcept {
    const EnchantmentInfo& info = enchantment_info(enchantment);
    switch (info.max_kind) {
        case MaxCost::MinPlus:
            return min_cost(enchantment, level) + info.max_value;
        case MaxCost::Constant:
            return info.max_value;
        case MaxCost::Linear:
            return info.max_value + info.max_per_level * (level - 1);
    }
    return info.max_value;
}

bool compatible(Enchantment a, Enchantment b) noexcept { return one_way(a, b) && one_way(b, a); }

i32 anvil_multiplier(Enchantment enchantment, bool from_book) noexcept {
    i32 m = 1;
    switch (enchantment_info(enchantment).weight) {
        case 10:
            m = 1;
            break;
        case 5:
            m = 2;
            break;
        case 2:
            m = 4;
            break;
        default:
            m = 8;
            break;
    }
    return from_book ? std::max(1, m / 2) : m;
}

// ── Items ───────────────────────────────────────────────────────────────────

i32 enchantability(std::string_view item) noexcept {
    const std::string_view b = bare(item);
    if (b == "book" || b == "bow" || b == "crossbow" || b == "trident" || b == "fishing_rod") {
        return 1;
    }
    if (b == "turtle_helmet") {
        return 9;
    }
    if (armour_piece(item) != Piece::None) {
        if (b.starts_with("leather_")) {
            return 15;
        }
        if (b.starts_with("chainmail_")) {
            return 12;
        }
        if (b.starts_with("iron_")) {
            return 9;
        }
        if (b.starts_with("golden_")) {
            return 25;
        }
        if (b.starts_with("diamond_")) {
            return 10;
        }
        if (b.starts_with("netherite_")) {
            return 15;
        }
    }
    if (is_sword(item) || is_digger(item)) {
        if (b.starts_with("wooden_")) {
            return 15;
        }
        if (b.starts_with("stone_")) {
            return 5;
        }
        if (b.starts_with("iron_")) {
            return 14;
        }
        if (b.starts_with("golden_")) {
            return 22;
        }
        if (b.starts_with("diamond_")) {
            return 10;
        }
        if (b.starts_with("netherite_")) {
            return 15;
        }
    }
    return 0;
}

std::optional<i32> enchant_max_damage(std::string_view item) noexcept {
    std::string full{"minecraft:"};
    full += bare(item);
    if (const auto tool = max_damage(full)) {
        return tool;
    }
    const std::string_view b     = bare(item);
    const Piece            piece = armour_piece(item);
    if (piece != Piece::None) {
        // Base durability per slot times the material's multiplier — the
        // wiki's armour table; each value is re-derived by the anvil campaign.
        i32 base = 0;
        switch (piece) {
            case Piece::Helmet:
                base = 11;
                break;
            case Piece::Chestplate:
                base = 16;
                break;
            case Piece::Leggings:
                base = 15;
                break;
            case Piece::Boots:
                base = 13;
                break;
            case Piece::None:
                break;
        }
        i32 multiplier = 0;
        if (b == "turtle_helmet") {
            multiplier = 25;
        } else if (b.starts_with("leather_")) {
            multiplier = 5;
        } else if (b.starts_with("chainmail_") || b.starts_with("iron_")) {
            multiplier = 15;
        } else if (b.starts_with("golden_")) {
            multiplier = 7;
        } else if (b.starts_with("diamond_")) {
            multiplier = 33;
        } else if (b.starts_with("netherite_")) {
            multiplier = 37;
        }
        return base * multiplier;
    }
    struct Row {
        std::string_view name;
        i32              max;
    };
    constexpr Row kOther[] = {
        {"elytra", 432},   {"shield", 336},        {"bow", 384},
        {"crossbow", 465}, {"trident", 250},       {"fishing_rod", 64},
        {"brush", 64},     {"flint_and_steel", 64}, {"carrot_on_a_stick", 25},
        {"warped_fungus_on_a_stick", 100},
    };
    for (const Row& row : kOther) {
        if (row.name == b) {
            return row.max;
        }
    }
    return std::nullopt;
}

bool category_accepts(EnchantCategory category, std::string_view item) noexcept {
    const std::string_view b     = bare(item);
    const Piece            piece = armour_piece(item);
    switch (category) {
        case C::Armor:
            return piece != Piece::None;
        case C::ArmorFeet:
            return piece == Piece::Boots;
        case C::ArmorLegs:
            return piece == Piece::Leggings;
        case C::ArmorChest:
            return piece == Piece::Chestplate;
        case C::ArmorHead:
            return piece == Piece::Helmet;
        case C::Weapon:
            return is_sword(item);
        case C::Digger:
            return is_digger(item);
        case C::FishingRod:
            return b == "fishing_rod";
        case C::Trident:
            return b == "trident";
        case C::Breakable:
            return enchant_max_damage(item).has_value();
        case C::Bow:
            return b == "bow";
        case C::Crossbow:
            return b == "crossbow";
        case C::Wearable:
            return piece != Piece::None || b == "elytra" || is_head_block(b);
        case C::Vanishable:
            return enchant_max_damage(item).has_value() || b == "compass" ||
                   b == "recovery_compass" || is_head_block(b);
    }
    return false;
}

bool can_enchant(Enchantment enchantment, std::string_view item) noexcept {
    if (category_accepts(enchantment_info(enchantment).category, item)) {
        return true;
    }
    switch (enchantment) {
        case E::Sharpness:
        case E::Smite:
        case E::BaneOfArthropods:
            return is_axe(item);
        case E::Efficiency:
            return bare(item) == "shears";
        case E::Thorns:
            return armour_piece(item) != Piece::None;
        default:
            return false;
    }
}

bool is_repair_material(std::string_view item, std::string_view material) noexcept {
    const std::string_view b = bare(item);
    const std::string_view m = bare(material);
    const auto             tiered = is_sword(item) || is_digger(item);
    const Piece            piece  = armour_piece(item);
    if (tiered) {
        if (b.starts_with("wooden_")) {
            return m.ends_with("_planks");
        }
        if (b.starts_with("stone_")) {
            return m == "cobblestone" || m == "cobbled_deepslate" || m == "blackstone";
        }
        if (b.starts_with("iron_")) {
            return m == "iron_ingot";
        }
        if (b.starts_with("golden_")) {
            return m == "gold_ingot";
        }
        if (b.starts_with("diamond_")) {
            return m == "diamond";
        }
        if (b.starts_with("netherite_")) {
            return m == "netherite_ingot";
        }
    }
    if (b == "turtle_helmet") {
        return m == "scute";
    }
    if (piece != Piece::None) {
        if (b.starts_with("leather_")) {
            return m == "leather";
        }
        if (b.starts_with("chainmail_") || b.starts_with("iron_")) {
            return m == "iron_ingot";
        }
        if (b.starts_with("golden_")) {
            return m == "gold_ingot";
        }
        if (b.starts_with("diamond_")) {
            return m == "diamond";
        }
        if (b.starts_with("netherite_")) {
            return m == "netherite_ingot";
        }
    }
    if (b == "elytra") {
        return m == "phantom_membrane";
    }
    if (b == "shield") {
        return m.ends_with("_planks");
    }
    return false;
}

// ── The list ────────────────────────────────────────────────────────────────

i32 EnchantmentList::level(Enchantment enchantment) const noexcept {
    for (usize i = 0; i < size_; ++i) {
        if (entries_[i].enchantment == enchantment) {
            return entries_[i].level;
        }
    }
    return 0;
}

void EnchantmentList::set(Enchantment enchantment, i32 level) noexcept {
    for (usize i = 0; i < size_; ++i) {
        if (entries_[i].enchantment == enchantment) {
            entries_[i].level = level;
            return;
        }
    }
    push({enchantment, level});
}

void EnchantmentList::push(EnchantmentLevel entry) noexcept {
    if (size_ < entries_.size()) {
        entries_[size_++] = entry;
    }
}

void EnchantmentList::erase_at(usize index) noexcept {
    if (index >= size_) {
        return;
    }
    for (usize i = index + 1; i < size_; ++i) {
        entries_[i - 1] = entries_[i];
    }
    --size_;
}

void EnchantmentList::keep_curses() noexcept {
    usize kept = 0;
    for (usize i = 0; i < size_; ++i) {
        if (enchantment_info(entries_[i].enchantment).curse) {
            entries_[kept++] = entries_[i];
        }
    }
    size_ = kept;
}

bool operator==(const EnchantmentList& a, const EnchantmentList& b) noexcept {
    return a.size_ == b.size_ && std::equal(a.begin(), a.end(), b.begin());
}

EnchantmentList read_enchantments(const nbt::Tag& tag, bool stored) {
    EnchantmentList out;
    const nbt::Tag* list = tag.find(stored ? "StoredEnchantments" : "Enchantments");
    if (list == nullptr || list->list() == nullptr) {
        return out;
    }
    for (const nbt::Tag& entry : *list->list()) {
        const nbt::Tag* id  = entry.find("id");
        const nbt::Tag* lvl = entry.find("lvl");
        if (id == nullptr || lvl == nullptr) {
            continue;
        }
        const auto e = enchantment_from_name(id->as_string());
        if (!e || out.level(*e) != 0) {
            continue;
        }
        // Vanilla clamps the stored short to 0..255 when it reads a level.
        out.push({*e, static_cast<i32>(std::clamp<i64>(lvl->as_i64(), 0, 255))});
    }
    return out;
}

void write_enchantments(nbt::Tag& tag, const EnchantmentList& list, bool stored) {
    const char* key = stored ? "StoredEnchantments" : "Enchantments";
    if (list.empty()) {
        (void)tag.erase(key);
        return;
    }
    nbt::Tag out = nbt::Tag::make_list(nbt::TagType::Compound);
    for (const EnchantmentLevel& e : list) {
        nbt::Tag entry = nbt::Tag::make_compound();
        (void)entry.put("id", nbt::Tag{std::string{enchantment_info(e.enchantment).name}});
        (void)entry.put("lvl", nbt::Tag{static_cast<i16>(e.level)});
        (void)out.push(std::move(entry));
    }
    (void)tag.put(key, std::move(out));
}

// ── The table ───────────────────────────────────────────────────────────────

std::span<const BlockOffset, 32> bookshelf_offsets() noexcept {
    static constexpr std::array<BlockOffset, 32> kOffsets = [] {
        std::array<BlockOffset, 32> out{};
        usize                       n = 0;
        for (i32 dy = 0; dy <= 1; ++dy) {
            for (i32 dx = -2; dx <= 2; ++dx) {
                for (i32 dz = -2; dz <= 2; ++dz) {
                    if (dx == -2 || dx == 2 || dz == -2 || dz == 2) {
                        out[n++] = BlockOffset{dx, dy, dz};
                    }
                }
            }
        }
        return out;
    }();
    return kOffsets;
}

BlockOffset bookshelf_gap(BlockOffset shelf) noexcept {
    // Integer division truncates towards zero in C++ as in Java.
    return BlockOffset{shelf.dx / 2, shelf.dy, shelf.dz / 2};
}

i32 count_bookshelves(std::span<const ShelfProbe, 32> probes) noexcept {
    i32 n = 0;
    for (const ShelfProbe& p : probes) {
        n += (p.shelf && p.clear) ? 1 : 0;
    }
    return n;
}

bool table_accepts(std::string_view item, i32 count, bool enchanted) noexcept {
    if (enchanted || count != 1) {
        return false;
    }
    return is_book(item) || enchant_max_damage(item).has_value();
}

i32 table_slot_cost(math::LegacyRandomSource& random, i32 slot, i32 bookshelves,
                    i32 enchantability) noexcept {
    if (enchantability <= 0) {
        return 0;
    }
    const i32 shelves = std::min(bookshelves, 15);
    // Two draws, in this order. Never one expression: C++ may evaluate the
    // operands of `+` in either order.
    const i32 first  = random.next_int(8);
    const i32 second = random.next_int(shelves + 1);
    const i32 base   = first + 1 + (shelves >> 1) + second;
    if (slot == 0) {
        return std::max(base / 3, 1);
    }
    if (slot == 1) {
        return base * 2 / 3 + 1;
    }
    return std::max(base, shelves * 2);
}

TableOffers table_offers(i32 xp_seed, i32 bookshelves, std::string_view item) {
    TableOffers              offers;
    math::LegacyRandomSource random{static_cast<i64>(xp_seed)};
    const i32                value = enchantability(item);
    for (i32 k = 0; k < 3; ++k) {
        offers.costs[static_cast<usize>(k)] = table_slot_cost(random, k, bookshelves, value);
        if (offers.costs[static_cast<usize>(k)] < k + 1) {
            offers.costs[static_cast<usize>(k)] = 0;
        }
    }
    for (i32 k = 0; k < 3; ++k) {
        const auto slot = static_cast<usize>(k);
        if (offers.costs[slot] <= 0) {
            continue;
        }
        const EnchantmentList list = list_for(random, xp_seed, k, offers.costs[slot], item);
        if (list.empty()) {
            continue;
        }
        const EnchantmentLevel clue =
            list[static_cast<usize>(random.next_int(static_cast<i32>(list.size())))];
        offers.clue_enchantment[slot] = network_id(clue.enchantment);
        offers.clue_level[slot]       = clue.level;
    }
    return offers;
}

EnchantmentList select_enchantments(math::LegacyRandomSource& random, std::string_view item,
                                    i32 level, bool treasure) {
    EnchantmentList out;
    const i32       value = enchantability(item);
    if (value <= 0) {
        return out;
    }
    const i32 spread = value / 4 + 1;
    const i32 a      = random.next_int(spread);
    const i32 b      = random.next_int(spread);
    level += 1 + a + b;
    const f32 f1    = random.next_float();
    const f32 f2    = random.next_float();
    const f32 bonus = (f1 + f2 - 1.0F) * 0.15F;
    const f32 as_f  = static_cast<f32>(level);
    level           = std::max(java_round(as_f + as_f * bonus), 1);

    EnchantmentList pool = candidates(item, level, treasure);
    if (pool.empty()) {
        return out;
    }
    if (const auto first = weighted(random, pool)) {
        out.push(*first);
    }
    while (random.next_int(50) <= level) {
        if (!out.empty()) {
            const Enchantment last = out[out.size() - 1].enchantment;
            for (usize i = pool.size(); i-- > 0;) {
                if (!compatible(last, pool[i].enchantment)) {
                    pool.erase_at(i);
                }
            }
        }
        if (pool.empty()) {
            break;
        }
        if (const auto next = weighted(random, pool)) {
            out.push(*next);
        }
        level /= 2;
    }
    return out;
}

EnchantmentList table_enchantments(i32 xp_seed, i32 slot, i32 cost, std::string_view item) {
    math::LegacyRandomSource random{0};
    return list_for(random, xp_seed, slot, cost, item);
}

// ── The anvil ───────────────────────────────────────────────────────────────

AnvilResult anvil_result(const AnvilRequest& request) {
    AnvilResult         result;
    const EnchantStack& left = request.left;
    if (left.item.empty() || left.count <= 0) {
        return result;
    }
    result.damage       = left.damage;
    result.enchantments = left.enchantments;

    const i32 base = left.repair_cost + (request.right ? request.right->repair_cost : 0);
    i32       work = 0;
    const auto left_max = enchant_max_damage(left.item);

    if (request.right) {
        const EnchantStack& right   = *request.right;
        const bool          is_book = is_enchanted_book(right.item) && !right.enchantments.empty();
        if (left_max && is_repair_material(left.item, right.item)) {
            // A unit repair: a quarter of the maximum per unit, as many units
            // as it takes or the stack holds.
            i32 step = std::min(result.damage, *left_max / 4);
            if (step <= 0) {
                return AnvilResult{};
            }
            i32 units = 0;
            for (; step > 0 && units < right.count; ++units) {
                result.damage -= step;
                ++work;
                step = std::min(result.damage, *left_max / 4);
            }
            result.right_consumed = units;
        } else {
            if (!is_book && (left.item != right.item || !left_max)) {
                return AnvilResult{};
            }
            if (left_max && !is_book) {
                const i32 remaining_left  = *left_max - left.damage;
                const i32 remaining_right = *left_max - right.damage;
                const i32 gained          = remaining_right + *left_max * 12 / 100;
                const i32 total           = remaining_left + gained;
                const i32 damage          = std::max(*left_max - total, 0);
                if (damage < result.damage) {
                    result.damage = damage;
                    work += 2;
                }
            }
            bool applied = false;
            bool refused = false;
            for (const EnchantmentLevel& offered : right.enchantments) {
                const i32 have = result.enchantments.level(offered.enchantment);
                i32 level = have == offered.level ? offered.level + 1 : std::max(offered.level, have);
                bool fits = can_enchant(offered.enchantment, left.item) || request.creative ||
                            is_enchanted_book(left.item);
                for (const EnchantmentLevel& present : result.enchantments) {
                    if (present.enchantment != offered.enchantment &&
                        !compatible(offered.enchantment, present.enchantment)) {
                        fits = false;
                        ++work;
                    }
                }
                if (!fits) {
                    refused = true;
                    continue;
                }
                applied = true;
                level   = std::min<i32>(level, enchantment_info(offered.enchantment).max_level);
                result.enchantments.set(offered.enchantment, level);
                work += anvil_multiplier(offered.enchantment, is_book) * level;
                if (left.count > 1) {
                    work = 40;
                }
            }
            if (refused && !applied) {
                return AnvilResult{};
            }
            result.right_consumed = right.count;
        }
    }

    i32 rename = 0;
    if (!request.rename || blank(*request.rename)) {
        if (left.has_custom_name) {
            rename      = 1;
            work       += 1;
            result.name = NameChange::Reset;
        }
    } else if (*request.rename != request.hover_name) {
        rename      = 1;
        work       += 1;
        result.name = NameChange::Set;
    }

    result.cost  = base + work;
    result.valid = work > 0;
    if (rename == work && rename > 0 && result.cost >= 40) {
        result.cost = 39;
    }
    if (result.cost >= 40 && !request.creative) {
        result.valid = false;
    }
    if (result.valid) {
        i32 penalty = left.repair_cost;
        if (request.right && penalty < request.right->repair_cost) {
            penalty = request.right->repair_cost;
        }
        if (rename != work || rename == 0) {
            penalty = increased_repair_cost(penalty);
        }
        result.repair_cost = penalty;
    }
    return result;
}

std::string_view damaged_anvil(std::string_view block) noexcept {
    const std::string_view b = bare(block);
    if (b == "anvil") {
        return "minecraft:chipped_anvil";
    }
    if (b == "chipped_anvil") {
        return "minecraft:damaged_anvil";
    }
    return {};
}

// ── The grindstone ──────────────────────────────────────────────────────────

GrindstoneResult grindstone_result(const std::optional<EnchantStack>& top,
                                   const std::optional<EnchantStack>& bottom) {
    GrindstoneResult out;
    if (!top && !bottom) {
        return out;
    }
    const bool both  = top && bottom;
    const auto plain = [](const std::optional<EnchantStack>& s) {
        return s && !is_enchanted_book(s->item) && s->enchantments.empty();
    };
    if ((top && top->count > 1) || (bottom && bottom->count > 1) ||
        (!both && (plain(top) || plain(bottom)))) {
        return out;
    }

    const auto removed_cost = [](const std::optional<EnchantStack>& s) {
        i32 sum = 0;
        if (s) {
            for (const EnchantmentLevel& e : s->enchantments) {
                if (!enchantment_info(e.enchantment).curse) {
                    sum += min_cost(e.enchantment, e.level);
                }
            }
        }
        return sum;
    };

    EnchantmentList kept;
    if (both) {
        if (top->item != bottom->item) {
            return out;
        }
        const i32 max       = enchant_max_damage(top->item).value_or(0);
        const i32 remaining = (max - top->damage) + (max - bottom->damage) + max * 5 / 100;
        out.damage          = std::max(max - remaining, 0);
        out.item            = top->item;
        kept                = top->enchantments;
        for (const EnchantmentLevel& e : bottom->enchantments) {
            if (!enchantment_info(e.enchantment).curse || kept.level(e.enchantment) == 0) {
                kept.set(e.enchantment, e.level);
            }
        }
        if (max == 0) {
            // Two of an item that does not wear out: only identical stacks,
            // and then both go through.
            const bool same = top->count == bottom->count && top->damage == bottom->damage &&
                              top->repair_cost == bottom->repair_cost &&
                              top->enchantments == bottom->enchantments &&
                              top->has_custom_name == bottom->has_custom_name;
            if (!same) {
                return out;
            }
            out.count = 2;
        }
    } else {
        const EnchantStack& only = top ? *top : *bottom;
        out.item                 = only.item;
        out.damage               = only.damage;
        kept                     = only.enchantments;
    }
    kept.keep_curses();
    out.enchantments = kept;
    out.repair_cost  = 0;
    for (usize i = 0; i < kept.size(); ++i) {
        out.repair_cost = increased_repair_cost(out.repair_cost);
    }
    if (is_enchanted_book(out.item) && kept.empty()) {
        out.item      = "minecraft:book";
        out.into_book = true;
    }
    out.experience_base = removed_cost(top) + removed_cost(bottom);
    out.valid           = true;
    return out;
}

i32 grindstone_experience(i32 base, math::LegacyRandomSource& random) noexcept {
    if (base <= 0) {
        return 0;
    }
    const i32 half = (base + 1) / 2;
    return half + random.next_int(half);
}

// ── Effects ─────────────────────────────────────────────────────────────────

i32 protection_epf(Enchantment enchantment, i32 level, DamageKind kind) noexcept {
    const DamageFlags flags = damage_type(kind).flags;
    if (level <= 0 || has(flags, DamageFlags::BypassesInvulnerability)) {
        return 0;
    }
    switch (enchantment) {
        case E::Protection:
            return level;
        case E::FireProtection:
            return has(flags, DamageFlags::IsFire) ? level * 2 : 0;
        case E::FeatherFalling:
            return has(flags, DamageFlags::IsFall) ? level * 3 : 0;
        case E::BlastProtection:
            return has(flags, DamageFlags::IsExplosion) ? level * 2 : 0;
        case E::ProjectileProtection:
            return has(flags, DamageFlags::IsProjectile) ? level * 2 : 0;
        default:
            return 0;
    }
}

i32 total_epf(std::span<const EnchantmentList> worn, DamageKind kind) noexcept {
    if (has(damage_type(kind).flags, DamageFlags::BypassesEnchantments)) {
        return 0;
    }
    i32 sum = 0;
    for (const EnchantmentList& piece : worn) {
        for (const EnchantmentLevel& e : piece) {
            sum += protection_epf(e.enchantment, e.level, kind);
        }
    }
    return sum;
}

f32 after_protection(f32 amount, i32 epf) noexcept {
    const f32 capped = std::clamp(static_cast<f32>(epf), 0.0F, 20.0F);
    return amount * (1.0F - capped / 25.0F);
}

MobGroup mob_group(std::string_view entity_type) noexcept {
    const std::string_view b = bare(entity_type);
    for (const std::string_view undead :
         {"zombie", "zombie_villager", "husk", "drowned", "skeleton", "stray", "wither_skeleton",
          "wither", "zombified_piglin", "zoglin", "phantom", "skeleton_horse", "zombie_horse"}) {
        if (b == undead) {
            return MobGroup::Undead;
        }
    }
    for (const std::string_view bug : {"spider", "cave_spider", "bee", "silverfish", "endermite"}) {
        if (b == bug) {
            return MobGroup::Arthropod;
        }
    }
    for (const std::string_view illager : {"pillager", "vindicator", "evoker", "illusioner"}) {
        if (b == illager) {
            return MobGroup::Illager;
        }
    }
    for (const std::string_view water :
         {"guardian", "elder_guardian", "squid", "glow_squid", "dolphin", "cod", "salmon",
          "pufferfish", "tropical_fish", "tadpole", "turtle", "axolotl"}) {
        if (b == water) {
            return MobGroup::Water;
        }
    }
    return MobGroup::Default;
}

f32 damage_bonus(const EnchantmentList& weapon, MobGroup target) noexcept {
    f32 bonus = 0.0F;
    for (const EnchantmentLevel& e : weapon) {
        if (e.level <= 0) {
            continue;
        }
        const f32 level = static_cast<f32>(e.level);
        switch (e.enchantment) {
            case E::Sharpness:
                bonus += 1.0F + static_cast<f32>(std::max(0, e.level - 1)) * 0.5F;
                break;
            case E::Smite:
                bonus += target == MobGroup::Undead ? level * 2.5F : 0.0F;
                break;
            case E::BaneOfArthropods:
                bonus += target == MobGroup::Arthropod ? level * 2.5F : 0.0F;
                break;
            case E::Impaling:
                bonus += target == MobGroup::Water ? level * 2.5F : 0.0F;
                break;
            default:
                break;
        }
    }
    return bonus;
}

MendingOutcome mend(i32 value, i32 damage) noexcept {
    MendingOutcome out;
    out.repaired = std::min(value * 2, std::max(damage, 0));
    out.left     = value - out.repaired / 2;
    return out;
}

bool respiration_saves_air(i32 level, math::LegacyRandomSource& random) noexcept {
    return level > 0 && random.next_int(level + 1) > 0;
}

bool thorns_triggers(i32 level, math::LegacyRandomSource& random) noexcept {
    return level > 0 && random.next_float() < 0.15F * static_cast<f32>(level);
}

i32 thorns_damage(i32 level, math::LegacyRandomSource& random) noexcept {
    return level > 10 ? level - 10 : 1 + random.next_int(4);
}

// ── Where each effect lives ─────────────────────────────────────────────────

const EffectStatus& effect_status(Enchantment enchantment) noexcept {
    using H = EffectHost;
    static constexpr std::array<EffectStatus, kEnchantmentCount> kStatus{{
        {H::Wired, "EPF in the player hurt path (enchant_session.cpp), measured"},
        {H::Wired, "EPF against #is_fire, measured"},
        {H::Wired, "EPF against #is_fall, measured"},
        {H::Wired, "EPF against #is_explosion, measured"},
        {H::Wired, "EPF against #is_projectile, measured"},
        {H::Wired, "a submerged tick keeps its air with chance level/(level+1) (survival_session)"},
        {H::Wired, "breaking stance: the helmet's level, breaking.cpp"},
        {H::NoHost, "no mob hits a player in this server: nothing to answer"},
        {H::ClientSide, "player movement is the client's; no mob wears boots here"},
        {H::NoHost, "frosted ice and its own ticks are not modelled"},
        {H::NoHost, "the player's own screen does not refuse taking the piece off yet"},
        {H::ClientSide, "speed is the client's; the boots' wear on soul sand is not modelled"},
        {H::ClientSide, "sneaking speed is the client's"},
        {H::Wired, "combat: +1 + 0.5(level-1) before the charge"},
        {H::Wired, "combat: +2.5 per level against undead, measured"},
        {H::Wired, "combat: +2.5 per level against arthropods, measured"},
        {H::Wired, "combat.cpp, measured in the combat campaign"},
        {H::Wired, "combat.cpp computes the ticks; burning belongs to the fire system"},
        {H::Wired, "entity loot tables read the level (mob_combat.cpp)"},
        {H::Wired, "combat.cpp sweep ratio"},
        {H::Wired, "breaking.cpp: level^2 + 1 on a correct tool"},
        {H::Wired, "block loot tables (loot.cpp)"},
        {H::Wired, "durability.cpp for tools; armour takes no wear in this server"},
        {H::Wired, "block loot tables (loot.cpp)"},
        {H::Wired, "projectiles.cpp"},
        {H::Wired, "projectiles.cpp"},
        {H::Wired, "projectiles.cpp sets the arrow alight"},
        {H::Wired, "projectiles.cpp"},
        {H::NoHost, "there is no fishing"},
        {H::NoHost, "there is no fishing"},
        {H::NoHost, "a thrown trident does not come back (projectile.cpp says so)"},
        {H::Wired, "melee only: +2.5 per level against water mobs, measured; not on a thrown trident"},
        {H::Wired, "projectiles.cpp"},
        {H::NoHost, "needs a thunderstorm and a lightning bolt from the trident"},
        {H::Wired, "projectiles.cpp"},
        {H::Wired, "projectiles.cpp: crossbow charge time"},
        {H::Wired, "projectiles.cpp"},
        {H::Wired, "orb pickup repairs the held and worn items (enchant_session.cpp), measured"},
        {H::Wired, "vanishes instead of dropping on death"},
    }};
    const auto index = static_cast<usize>(enchantment);
    return kStatus[index < kEnchantmentCount ? index : 0];
}

}  // namespace ov::gameplay
