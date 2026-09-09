// The crafting table and the three furnaces, as windows.
//
// A container screen is the same six click modes everywhere — pick up, split,
// shift-move, number key, throw, drag — over a list of slots that differ only
// in what they are allowed to hold and what happens when something lands in
// them. That common part is `Clicks` below, and it is written once here rather
// than a second time per screen.
//
// Two screens are not ordinary containers, and both are here for that reason:
//
//   * the crafting table's slot 0 is not storage. It shows what the grid
//     currently makes, cannot be put into, and taking from it consumes the
//     grid. Shift-clicking it crafts in a **loop** until the grid runs out,
//     which is the one click in the game that does an unbounded amount of work
//     and the classic place to write an infinite one.
//   * a furnace has three slots with three different admission rules, a
//     progress bar the client draws from four numbers the server sends, and
//     experience it hands over when its output is taken.
//
// The rules themselves live in ov_gameplay and are testable without a server;
// what is here is the window: which slot is which, and what to send back.
#pragma once

#include "ov/gameplay/crafting.hpp"
#include "ov/gameplay/smelting.hpp"
#include "ov/nbt/tag.hpp"
#include "ov/protocol/play.hpp"
#include "ov/protocol/recipe_packets.hpp"
#include "ov/registry/registries.hpp"

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ov::server {

/// Which of the screens this file owns a player has open.
enum class WorkbenchKind : u8 { CraftingTable, Furnace, BlastFurnace, Smoker };

/// The block each screen belongs to, and back.
[[nodiscard]] std::optional<WorkbenchKind> workbench_of_block(std::string_view block_name);

/// The furnace kind, for a screen that is one. Nullopt for the crafting table.
[[nodiscard]] std::optional<gameplay::FurnaceKind> furnace_of(WorkbenchKind kind);

/// The client's `minecraft:menu` entry for this screen. A wire id the client
/// hard-codes: a furnace opened as a crafting table shows three slots and
/// swallows everything put in them.
[[nodiscard]] std::string_view menu_name(WorkbenchKind kind);

[[nodiscard]] std::string_view screen_title(WorkbenchKind kind);

/// One open screen, and the state that belongs to it rather than to the world.
///
/// A crafting grid is not saved anywhere: vanilla drops it on the floor when
/// the screen closes, because a crafting table is a block with no block entity
/// and nowhere to put it.
struct Workbench {
    WorkbenchKind kind{WorkbenchKind::CraftingTable};
    u8            window_id{0};
    i32           x{0};
    i32           y{0};
    i32           z{0};

    /// The 3x3 grid of a crafting table. Empty for a furnace.
    std::array<net::ItemStack, 9> grid{};

    /// The furnace's three slots and four counters. Loaded from the block
    /// entity when the screen opens and written back on every change, so that
    /// a second player looking at the same furnace sees the same thing.
    gameplay::FurnaceSlots furnace_slots{};
    gameplay::FurnaceState furnace_state{};

    /// The last counters sent, so that `Set Container Property` goes out only
    /// when a number actually moved rather than four times a tick.
    std::array<i16, 4> sent_properties{-1, -1, -1, -1};
};

/// What a click did, for the caller to carry out.
///
/// Returned rather than performed: this file has no socket, no chunk and no
/// player list, which is what lets the whole thing be exercised from a test.
struct WorkbenchOutcome {
    bool handled{false};
    /// The screen must close — the block is gone, or was never the right one.
    bool close{false};
    /// The furnace's slots changed and the block entity has to be written.
    bool save_block_entity{false};
    /// Stacks the player could not fit, to be dropped at their feet.
    std::vector<net::ItemStack> overflow;
    /// Experience the player has just earned by emptying a furnace.
    f32 experience{0.0F};
};

/// Everything the window needs from the game that is not in the window.
struct WorkbenchContext {
    const registry::Registries* registries{nullptr};
    const gameplay::RecipeBook* book{nullptr};
    registry::RegistryId        item_registry{};
    registry::RegistryId        menu_registry{};
};

/// The four things a window has to reach outside itself for.
///
/// Callbacks rather than a reference to the server: this file must not know
/// what a connection, a chunk or a player list is, and the server's own helpers
/// are lambdas over its locals. The cost is one indirect call per window
/// operation, none of which happen more than a few times a tick.
struct WorkbenchHost {
    /// Send a packet to the player whose window this is.
    std::function<void(i32, std::vector<u8>)> send;
    /// Drop a stack at the player's feet, because nothing else would hold it.
    std::function<void(const net::ItemStack&)> drop;
    /// The block entity NBT at a position, or nullptr when there is none.
    std::function<nbt::Tag*(i32, i32, i32)> block_entity;
    /// The block's registry name at a position, empty when out of the world.
    std::function<std::string_view(i32, i32, i32)> block_name;
    /// Turn a furnace's `lit` property on or off, and tell everyone.
    std::function<void(i32, i32, i32, bool)> set_lit;
    /// The chunk holding this position has changed and must be saved.
    std::function<void(i32, i32)> mark_dirty;
    /// Award experience to the player.
    std::function<void(f32)> award_experience;
};

/// Open a screen on the block the player clicked, if it is one this file owns.
///
/// Returns false and leaves `out` alone for anything else — a chest, a stone
/// block — so the caller can go on to whatever it would have done.
[[nodiscard]] bool open_workbench(const WorkbenchContext& context, const WorkbenchHost& host,
                                  i32 x, i32 y, i32 z, u8 window_id,
                                  std::span<const net::ItemStack> inventory,
                                  std::optional<Workbench>&       out);

/// Handle one click and send everything it implies.
void handle_click(const WorkbenchContext& context, const WorkbenchHost& host, Workbench& bench,
                  const net::ContainerClick& click, std::span<net::ItemStack> inventory,
                  net::ItemStack& carried, std::optional<Workbench>& holder);

/// One tick of an open furnace, including the property packets the bars need.
void tick_open_workbench(const WorkbenchContext& context, const WorkbenchHost& host,
                         Workbench& bench, std::span<const net::ItemStack> inventory);

/// The screen closed. A crafting grid is not stored anywhere, so vanilla gives
/// it back rather than eating it — and so does this.
void close_workbench(const WorkbenchHost& host, Workbench& bench);

/// Tell a joining client every recipe, and unlock all of them.
///
/// Vanilla sends this once, right after the join sequence. A client that never
/// receives it still plays — the server computes the results and the client only
/// draws them — but its recipe book stays empty and the button that fills a grid
/// from the book does nothing.
///
/// Everything is unlocked rather than tracked per player: unlocking is a
/// progression system of its own, and pretending to have it would be worse than
/// saying it is not here.
void send_recipe_book(const WorkbenchContext& context,
                      const std::function<void(i32, std::vector<u8>)>& send);

/// The whole window: the screen's own slots, then the player's 36.
///
/// The order is the protocol's and not a choice — a window shows its container
/// first and the player second, and getting the boundary wrong moves items
/// between the wrong two halves without any error.
[[nodiscard]] usize window_slot_count(WorkbenchKind kind);

/// Lay the window out as one list of stacks, for `Set Container Content`.
///
/// The context is needed for the crafting table's slot 0, which holds no item
/// of its own: it shows what the grid currently makes, so it has to be matched
/// rather than read.
[[nodiscard]] std::vector<net::ItemStack> window_contents(
    const WorkbenchContext& context, const Workbench& bench,
    std::span<const net::ItemStack> player_inventory);

/// The four numbers a furnace screen draws its bars from, in the order the
/// protocol numbers them.
[[nodiscard]] std::array<i16, 4> furnace_properties(const Workbench& bench);

/// Apply one click to an open screen.
///
/// `player_inventory` is the player's 46 slots as the protocol numbers them, so
/// that slot 36 is the first hotbar slot here as it is everywhere else.
[[nodiscard]] WorkbenchOutcome apply_click(const WorkbenchContext& context, Workbench& bench,
                                           const net::ContainerClick& click,
                                           std::span<net::ItemStack>  player_inventory,
                                           net::ItemStack&            carried);

/// Advance a furnace one tick. Does nothing for a crafting table.
[[nodiscard]] gameplay::FurnaceTick tick_furnace(const WorkbenchContext& context,
                                                 Workbench&              bench);

/// Read a furnace's slots and counters out of its block entity NBT.
void load_furnace(const WorkbenchContext& context, const nbt::Tag& data, Workbench& bench);

/// Write them back, in the shape vanilla reads.
void store_furnace(const WorkbenchContext& context, nbt::Tag& data, const Workbench& bench);

}  // namespace ov::server
