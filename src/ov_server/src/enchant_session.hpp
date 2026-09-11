// The enchanting table, the anvil and the grindstone, as windows.
//
// The rules are in ov_gameplay (enchanting.hpp) and know nothing of slots or
// packets. What is here is the window around them: which slot is which, what
// each admits, what taking the output costs, and the three packets that are
// not clicks — Click Container Button for the table's three offers, Rename
// Item for the anvil's text box, and Container Property for everything the
// client draws but does not compute.
//
// The same shape as workbench.{hpp,cpp}: callbacks rather than a reference to
// the server, so the whole file can be driven from a test.
#pragma once

#include "ov/gameplay/enchanting.hpp"
#include "ov/math/random.hpp"
#include "ov/protocol/play.hpp"
#include "ov/registry/registries.hpp"

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ov::server {

class SurvivalSession;

/// Serverbound packets this file owns, protocol 763. Both checked against the
/// frozen archive and against the real server, which answers each.
inline constexpr i32 kClickContainerButton = 0x0A;
inline constexpr i32 kRenameItem           = 0x23;

enum class EnchantScreen : u8 { Table, Anvil, Grindstone };

/// The screen a block opens. The three anvil stages all open the anvil.
[[nodiscard]] std::optional<EnchantScreen> enchant_screen_of_block(std::string_view block);

/// One open screen.
struct EnchantWindow {
    EnchantScreen kind{EnchantScreen::Table};
    u8            window_id{0};
    i32           x{0};
    i32           y{0};
    i32           z{0};

    /// The table: 0 the item, 1 the lapis. The anvil: 0 left, 1 right, 2 the
    /// output. The grindstone: 0 top, 1 bottom, 2 the output. The output is
    /// recomputed from the inputs after every change, never stored for itself.
    std::array<net::ItemStack, 3> slots{};

    gameplay::TableOffers      offers{};
    gameplay::AnvilResult      anvil{};
    gameplay::GrindstoneResult grind{};

    /// What the anvil's text box last said. Nullopt until a Rename Item
    /// arrives, which vanilla treats as blank.
    std::optional<std::string> rename;

    /// The last value of each property sent; the first send is all of them.
    std::array<i32, 10> sent{};
    bool                sent_any{false};
};

struct EnchantContext {
    const registry::Registries* registries{nullptr};
    registry::RegistryId        item_registry{};
    registry::RegistryId        menu_registry{};
    registry::RegistryId        block_registry{};
};

/// Everything a window reaches outside itself for.
struct EnchantHost {
    std::function<void(i32, std::vector<u8>)>  send;
    std::function<void(const net::ItemStack&)> drop;
    std::function<std::string_view(i32, i32, i32)> block_name;
    /// Put another block of the same shape here — an anvil one stage more
    /// damaged, facing the same way — or air when `block` is empty.
    std::function<void(i32, i32, i32, std::string_view)> replace_block;
    /// The name an item shows: its custom name, else its default name.
    std::function<std::string(const net::ItemStack&)> hover_name;
    std::function<i32()>      level;
    std::function<void(i32)>  take_levels;
    std::function<bool()>     creative;
    std::function<i32()>      xp_seed;
    std::function<void(i32)>  set_xp_seed;
    /// Experience paid out at a point, as orbs.
    std::function<void(i32, f64, f64, f64)> spawn_experience;
    /// A level event (1029 anvil destroyed, 1030 anvil used, 1042 grindstone
    /// used) at a block. May be empty.
    std::function<void(i32, i32, i32, i32)> level_event;
    /// The player's own generator: the anvil's 12 %, the grindstone's refund,
    /// the next XpSeed.
    math::LegacyRandomSource* random{nullptr};
};

/// Open the screen for the block at a position. False — and `out` untouched —
/// for any block that is not one of the three.
[[nodiscard]] bool open_enchant_screen(const EnchantContext& context, const EnchantHost& host,
                                       i32 x, i32 y, i32 z, u8 window_id,
                                       std::span<const net::ItemStack> inventory,
                                       std::optional<EnchantWindow>&   out);

/// One Click Container on an open screen.
void enchant_click(const EnchantContext& context, const EnchantHost& host, EnchantWindow& window,
                   const net::ContainerClick& click, std::span<net::ItemStack> inventory,
                   net::ItemStack& carried, std::optional<EnchantWindow>& holder);

/// Click Container Button: one of the table's three offers.
void enchant_button(const EnchantContext& context, const EnchantHost& host, EnchantWindow& window,
                    i32 button, std::span<const net::ItemStack> inventory,
                    const net::ItemStack& carried);

/// Rename Item: the anvil's text box changed.
void enchant_rename(const EnchantContext& context, const EnchantHost& host, EnchantWindow& window,
                    std::string_view name, std::span<const net::ItemStack> inventory,
                    const net::ItemStack& carried);

/// The screen closed. The inputs go back to the player, or to the ground.
void close_enchant_screen(const EnchantContext& context, const EnchantHost& host,
                          EnchantWindow& window, std::span<net::ItemStack> inventory);

/// Recompute what the inputs make: offers, anvil result, grindstone result.
void refresh_enchant_window(const EnchantContext& context, const EnchantHost& host,
                            EnchantWindow& window);

/// The window as `Set Container Content` lists it.
[[nodiscard]] std::vector<net::ItemStack> enchant_window_contents(
    const EnchantWindow& window, std::span<const net::ItemStack> inventory);

/// Decode a stack the way the anvil and the grindstone read it. The item name
/// points into the registry and outlives the stack.
[[nodiscard]] gameplay::EnchantStack enchant_stack_of(const EnchantContext& context,
                                                      const net::ItemStack& stack);

/// The anvil's output stack for a result, built from the left input.
[[nodiscard]] net::ItemStack anvil_output(const EnchantContext& context,
                                          const net::ItemStack& left,
                                          const gameplay::AnvilResult& result,
                                          std::string_view             rename);

/// The grindstone's output stack, built from the input it copies.
[[nodiscard]] net::ItemStack grindstone_output(const EnchantContext& context,
                                               const net::ItemStack& source,
                                               const gameplay::GrindstoneResult& result);

[[nodiscard]] std::optional<std::pair<u8, i32>> parse_click_button(std::span<const u8> payload);
[[nodiscard]] std::optional<std::string>        parse_rename_item(std::span<const u8> payload);

/// `giveExperienceLevels(-n)`: the level drops, the bar keeps its fraction.
void take_levels(SurvivalSession& survival, i32 levels);

/// The seed of a player's own generator — the one that draws the next
/// `XpSeed`. From the UUID, mixed: an earlier `entity_id * 0x5DEECE66D` was
/// cancelled exactly by `setSeed`'s own XOR for entity 1, left the generator at
/// state 0, and gave every "new" seed as 0 — the table offered the same seed-0
/// enchantments after every enchant.
[[nodiscard]] i64 enchant_random_seed(const net::Uuid& player);

// ── Effects on a player ─────────────────────────────────────────────────────

/// The four worn pieces' enchantments, from the protocol's slots 5..8.
[[nodiscard]] std::array<gameplay::EnchantmentList, 4> worn_enchantments(
    std::span<const net::ItemStack> inventory);

/// A hit after the worn enchantments' EPF.
[[nodiscard]] f32 after_worn_protection(std::span<const net::ItemStack> inventory,
                                        gameplay::DamageKind kind, f32 amount);

/// The enchantment level on a stack's `Enchantments`.
[[nodiscard]] i32 stack_enchantment(const net::ItemStack& stack, gameplay::Enchantment which);

/// Mending: spend an orb's value on the damaged Mending items among the held,
/// off-hand and worn stacks, one at random at a time as vanilla does. Returns
/// the experience left for the player. Stacks are rewritten in place;
/// `changed` receives each protocol slot whose stack changed.
[[nodiscard]] i32 apply_mending(std::span<net::ItemStack> inventory, i16 held_slot, i32 value,
                                math::LegacyRandomSource& random,
                                const std::function<void(usize)>& changed);

/// Smite, Bane of Arthropods and Impaling on the held stack, against an
/// entity type. Sharpness is not here: combat already counts it.
[[nodiscard]] f32 target_enchantment_bonus(const net::ItemStack& held, std::string_view entity_type);

/// Curse of Vanishing: the stack disappears on death instead of dropping.
[[nodiscard]] bool vanishes_on_death(const net::ItemStack& stack);

}  // namespace ov::server
