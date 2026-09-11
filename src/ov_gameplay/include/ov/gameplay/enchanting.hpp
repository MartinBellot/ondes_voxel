// Enchantments: what they are, what the table offers, what the anvil and the
// grindstone make of them, and the few effects that are arithmetic.
//
// The thirty-nine enchantments are Java classes in 1.20.1, not data. Their
// weights, level windows, categories and conflicts come from the wiki's
// enchanting pages (revisions before the 1.21 rewrite, see
// docs/provenance/enchantement.md); every rule here that the real server can be
// asked about was then asked — scripts/measure_enchanting.py — and the tests
// replay its answers. The table in particular is checked **exactly**: the same
// `XpSeed`, bookshelf count and item give the same three costs, the same three
// clues and the same enchantments as vanilla, draw for draw.
//
// Layer 9: nothing here takes a level, a server or a socket. The table counts
// bookshelves from booleans the caller read, the anvil works on stacks the
// caller decoded. That is what lets a client predict the same offers.
#pragma once

#include "ov/base/types.hpp"
#include "ov/gameplay/damage.hpp"
#include "ov/math/random.hpp"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ov::nbt {
class Tag;
}

namespace ov::gameplay {

/// The thirty-nine, in `minecraft:enchantment` registry order — which is the
/// network id the client hard-codes, and the order the table iterates when it
/// builds its list of candidates. Both facts make the order load-bearing: a
/// swap of two entries moves every clue the table shows and changes which
/// enchantment a given random draw lands on.
enum class Enchantment : u8 {
    Protection,
    FireProtection,
    FeatherFalling,
    BlastProtection,
    ProjectileProtection,
    Respiration,
    AquaAffinity,
    Thorns,
    DepthStrider,
    FrostWalker,
    BindingCurse,
    SoulSpeed,
    SwiftSneak,
    Sharpness,
    Smite,
    BaneOfArthropods,
    Knockback,
    FireAspect,
    Looting,
    Sweeping,
    Efficiency,
    SilkTouch,
    Unbreaking,
    Fortune,
    Power,
    Punch,
    Flame,
    Infinity,
    LuckOfTheSea,
    Lure,
    Loyalty,
    Impaling,
    Riptide,
    Channeling,
    Multishot,
    QuickCharge,
    Piercing,
    Mending,
    VanishingCurse,
};

inline constexpr usize kEnchantmentCount = 39;

/// What kind of item an enchantment belongs on. The table asks the category
/// and nothing else; the anvil asks the enchantment, which widens a few of
/// them (Sharpness on an axe, Efficiency on shears, Thorns on any armour).
enum class EnchantCategory : u8 {
    Armor,
    ArmorFeet,
    ArmorLegs,
    ArmorChest,
    ArmorHead,
    Weapon,
    Digger,
    FishingRod,
    Trident,
    Breakable,
    Bow,
    Wearable,
    Crossbow,
    Vanishable,
};

/// How the top of an enchantment level's cost window is computed.
enum class MaxCost : u8 {
    /// `min_cost(level) + value`.
    MinPlus,
    /// A constant.
    Constant,
    /// `value + max_per_level * (level - 1)` — Thorns only: 60, 70, 80.
    Linear,
};

struct EnchantmentInfo {
    /// Registry name, "minecraft:…".
    std::string_view name;
    u8               max_level;
    /// The weight the table draws with: 10, 5, 2 or 1.
    u8              weight;
    EnchantCategory category;
    /// Never offered by the table (Mending, Frost Walker, the curses, Soul
    /// Speed, Swift Sneak).
    bool treasure;
    bool curse;
    /// False for Soul Speed and Swift Sneak, which not even loot's treasure
    /// draw may pick: they come only from their structures' chests.
    bool discoverable;
    /// `min_cost(level) = min_base + min_per_level * (level - 1)`.
    i16     min_base;
    i16     min_per_level;
    MaxCost max_kind;
    i16     max_value;
    i16     max_per_level;
};

[[nodiscard]] const EnchantmentInfo& enchantment_info(Enchantment enchantment) noexcept;

/// Look up by registry name, with or without the `minecraft:` prefix.
[[nodiscard]] std::optional<Enchantment> enchantment_from_name(std::string_view name) noexcept;

/// The network id the client hard-codes.
[[nodiscard]] constexpr i32 network_id(Enchantment enchantment) noexcept {
    return static_cast<i32>(enchantment);
}

/// The *modified* level window in which the table picks this level.
[[nodiscard]] i32 min_cost(Enchantment enchantment, i32 level) noexcept;
[[nodiscard]] i32 max_cost(Enchantment enchantment, i32 level) noexcept;

/// Two enchantments may share an item. Symmetric, and false for an enchantment
/// and itself.
[[nodiscard]] bool compatible(Enchantment a, Enchantment b) noexcept;

/// What one level of it costs at the anvil: 1, 2, 4 or 8 by weight, halved
/// (never below one) when it comes from a book.
[[nodiscard]] i32 anvil_multiplier(Enchantment enchantment, bool from_book) noexcept;

// ── What an item is, as far as enchanting cares ─────────────────────────────

/// The item's enchantability; 0 for an item the table refuses. Books, bows,
/// crossbows, tridents and fishing rods are 1.
[[nodiscard]] i32 enchantability(std::string_view item) noexcept;

/// The item's maximum damage for the anvil and the grindstone, including the
/// armour, elytra, shield and the rest the tool table does not carry. Nullopt
/// for an item that does not wear out.
[[nodiscard]] std::optional<i32> enchant_max_damage(std::string_view item) noexcept;

/// The table's question: does this category take this item?
[[nodiscard]] bool category_accepts(EnchantCategory category, std::string_view item) noexcept;

/// The anvil's question: may this enchantment go on this item?
[[nodiscard]] bool can_enchant(Enchantment enchantment, std::string_view item) noexcept;

/// The item a unit of `material` repairs at the anvil, e.g. a diamond for any
/// diamond tool or armour.
[[nodiscard]] bool is_repair_material(std::string_view item, std::string_view material) noexcept;

// ── Enchantments on a stack ─────────────────────────────────────────────────

struct EnchantmentLevel {
    Enchantment enchantment{Enchantment::Protection};
    i32         level{0};

    friend constexpr bool operator==(const EnchantmentLevel&, const EnchantmentLevel&) = default;
};

/// An ordered set, without allocation. The order is the stack's own, and it is
/// the order the anvil and the table walk.
class EnchantmentList {
public:
    [[nodiscard]] usize size() const noexcept { return size_; }
    [[nodiscard]] bool  empty() const noexcept { return size_ == 0; }

    [[nodiscard]] const EnchantmentLevel* begin() const noexcept { return entries_.data(); }
    [[nodiscard]] const EnchantmentLevel* end() const noexcept { return entries_.data() + size_; }
    [[nodiscard]] const EnchantmentLevel& operator[](usize i) const noexcept { return entries_[i]; }

    /// Level of an enchantment, 0 when absent.
    [[nodiscard]] i32 level(Enchantment enchantment) const noexcept;

    /// Append, or overwrite the level of one already present in place.
    void set(Enchantment enchantment, i32 level) noexcept;

    /// Append even if present — the table's own list may carry anything once.
    void push(EnchantmentLevel entry) noexcept;

    void erase_at(usize index) noexcept;

    /// Remove every entry for which `keep` is false.
    void keep_curses() noexcept;

    friend bool operator==(const EnchantmentList& a, const EnchantmentList& b) noexcept;

private:
    std::array<EnchantmentLevel, kEnchantmentCount> entries_{};
    usize                                            size_{0};
};

/// Read `Enchantments` (or `StoredEnchantments` for a book) from an item's tag
/// compound. An id this server does not know is skipped, and a second entry
/// for the same enchantment is ignored: vanilla reads the first.
[[nodiscard]] EnchantmentList read_enchantments(const nbt::Tag& tag, bool stored);

/// Write them back in vanilla's shape: `{id:"minecraft:…", lvl:<short>}`. An
/// empty list removes the key, as vanilla does.
void write_enchantments(nbt::Tag& tag, const EnchantmentList& list, bool stored);

// ── The enchanting table ────────────────────────────────────────────────────

struct BlockOffset {
    i32 dx;
    i32 dy;
    i32 dz;
};

/// The 32 positions a bookshelf may stand at around the table: two blocks out
/// on each side, at the table's height and one above.
[[nodiscard]] std::span<const BlockOffset, 32> bookshelf_offsets() noexcept;

/// The block the path from the table to a shelf runs through: half the offset,
/// truncated towards zero, at the shelf's height. Measured to be this one block
/// and not the one below or above it.
[[nodiscard]] BlockOffset bookshelf_gap(BlockOffset shelf) noexcept;

/// What the caller saw at one offset.
struct ShelfProbe {
    /// The block at the offset is in #enchantment_power_provider.
    bool shelf{false};
    /// The gap block is in #enchantment_power_transmitter (#replaceable).
    bool clear{false};
};

/// How many shelves count. Not capped: the cost formula caps at 15 itself.
[[nodiscard]] i32 count_bookshelves(std::span<const ShelfProbe, 32> probes) noexcept;

/// The ten numbers of the table's `Container Property`, in property order.
struct TableOffers {
    std::array<i32, 3> costs{0, 0, 0};
    std::array<i32, 3> clue_enchantment{-1, -1, -1};
    std::array<i32, 3> clue_level{-1, -1, -1};
};

/// The table can enchant this stack at all: something with durability, or a
/// single book, not already enchanted, of non-zero enchantability.
[[nodiscard]] bool table_accepts(std::string_view item, i32 count, bool enchanted) noexcept;

/// One slot's cost. Draws two numbers from `random`, as vanilla does in order
/// for slots 0, 1, 2.
[[nodiscard]] i32 table_slot_cost(math::LegacyRandomSource& random, i32 slot, i32 bookshelves,
                                  i32 enchantability) noexcept;

/// The three offers for a stack the table accepts.
[[nodiscard]] TableOffers table_offers(i32 xp_seed, i32 bookshelves, std::string_view item);

/// Pick enchantments at a *base* level — the table's step one, two and three.
/// Treasure is allowed only for loot, never for the table.
[[nodiscard]] EnchantmentList select_enchantments(math::LegacyRandomSource& random,
                                                  std::string_view item, i32 level,
                                                  bool treasure);

/// What clicking slot `slot` at `cost` puts on the item: the selection seeded
/// by `xp_seed + slot`, less one random entry when it is a book.
[[nodiscard]] EnchantmentList table_enchantments(i32 xp_seed, i32 slot, i32 cost,
                                                 std::string_view item);

/// Property 3: the seed as the client is allowed to see it.
[[nodiscard]] constexpr i32 table_seed_property(i32 xp_seed) noexcept { return xp_seed & -16; }

// ── The anvil ───────────────────────────────────────────────────────────────

/// A stack as the anvil and the grindstone read it.
struct EnchantStack {
    std::string_view item;
    i32              count{1};
    i32              damage{0};
    i32              repair_cost{0};
    /// `Enchantments`, or `StoredEnchantments` for an enchanted book.
    EnchantmentList enchantments;
    bool            has_custom_name{false};
};

enum class NameChange : u8 { Keep, Set, Reset };

struct AnvilRequest {
    EnchantStack                left;
    std::optional<EnchantStack> right;
    /// What the player typed. Nullopt until a Rename Item arrived.
    std::optional<std::string_view> rename;
    /// The left item's current hover name — its custom name, or its default
    /// name — against which a rename is compared.
    std::string_view hover_name;
    bool             creative{false};
};

struct AnvilResult {
    /// There is something in the output slot.
    bool valid{false};
    /// Property 0: the level cost, shown even when too expensive.
    i32             cost{0};
    i32             damage{0};
    i32             repair_cost{0};
    EnchantmentList enchantments;
    NameChange      name{NameChange::Keep};
    /// How many of the right stack are used up: all of it, or — for a unit
    /// repair — the units spent.
    i32 right_consumed{0};
};

[[nodiscard]] AnvilResult anvil_result(const AnvilRequest& request);

/// The chance an anvil drops a stage on a use, taken in survival only.
inline constexpr f32 kAnvilDamageChance = 0.12F;

/// The penalty after one more anvil use: `2 * cost + 1`.
[[nodiscard]] constexpr i32 increased_repair_cost(i32 cost) noexcept { return cost * 2 + 1; }

/// The next stage of an anvil block, or empty when it breaks.
[[nodiscard]] std::string_view damaged_anvil(std::string_view block) noexcept;

// ── The grindstone ──────────────────────────────────────────────────────────

struct GrindstoneResult {
    bool             valid{false};
    std::string_view item;
    i32              count{1};
    i32              damage{0};
    i32              repair_cost{0};
    /// The curses, which the grindstone cannot remove.
    EnchantmentList enchantments;
    /// The output is a plain book made from an enchanted one.
    bool into_book{false};
    /// Sum over the removed enchantments of their minimum cost. What the
    /// experience is drawn from.
    i32 experience_base{0};
};

[[nodiscard]] GrindstoneResult grindstone_result(const std::optional<EnchantStack>& top,
                                                 const std::optional<EnchantStack>& bottom);

/// The experience a take pays: `ceil(base / 2) + nextInt(ceil(base / 2))`.
[[nodiscard]] i32 grindstone_experience(i32 base, math::LegacyRandomSource& random) noexcept;

// ── Effects that are arithmetic ─────────────────────────────────────────────

/// The Enchantment Protection Factor one enchantment level gives against a
/// damage kind. Zero for a type in #bypasses_enchantments… and see the notes
/// in enchanting.cpp for what was measured to bypass.
[[nodiscard]] i32 protection_epf(Enchantment enchantment, i32 level, DamageKind kind) noexcept;

/// Sum of `protection_epf` over worn pieces. Uncapped; the cap is the
/// reduction's.
[[nodiscard]] i32 total_epf(std::span<const EnchantmentList> worn, DamageKind kind) noexcept;

/// `amount * (1 - clamp(epf, 0, 20) / 25)`, in float as vanilla.
[[nodiscard]] f32 after_protection(f32 amount, i32 epf) noexcept;

/// The mob groups that change a damage enchantment's bonus.
enum class MobGroup : u8 { Default, Undead, Arthropod, Illager, Water };

[[nodiscard]] MobGroup mob_group(std::string_view entity_type) noexcept;

/// The melee bonus of Sharpness, Smite, Bane of Arthropods and Impaling
/// against a target of this group, before the charge scales it.
[[nodiscard]] f32 damage_bonus(const EnchantmentList& weapon, MobGroup target) noexcept;

/// Mending: an orb of `value` meets an item with `damage`. What comes off the
/// damage, and what experience is left for the next item or the player.
struct MendingOutcome {
    i32 repaired{0};
    i32 left{0};
};

[[nodiscard]] MendingOutcome mend(i32 value, i32 damage) noexcept;

/// Respiration: a tick on which the air that would go down does not.
[[nodiscard]] bool respiration_saves_air(i32 level, math::LegacyRandomSource& random) noexcept;

/// Thorns on a hit taken: whether it answers, and for how much.
[[nodiscard]] bool thorns_triggers(i32 level, math::LegacyRandomSource& random) noexcept;
[[nodiscard]] i32  thorns_damage(i32 level, math::LegacyRandomSource& random) noexcept;

// ── Where each effect lives ─────────────────────────────────────────────────

enum class EffectHost : u8 {
    /// Implemented and wired, in this module or the system named.
    Wired,
    /// The client does it; the server has nothing to compute.
    ClientSide,
    /// The system the effect acts on does not exist in this server yet.
    NoHost,
};

struct EffectStatus {
    EffectHost       host;
    /// Where, or what is missing.
    std::string_view note;
};

/// For every enchantment, where its effect is — or, named, why it is not.
[[nodiscard]] const EffectStatus& effect_status(Enchantment enchantment) noexcept;

}  // namespace ov::gameplay
