#include "workbench.hpp"

#include <algorithm>
#include <array>

namespace ov::server {
namespace {

using gameplay::FurnaceKind;
using gameplay::RecipeStack;

/// Slot numbering inside each screen. The container's own slots come first and
/// the player's 36 follow, which is the protocol's order and not a choice.
constexpr i16 kCraftingResult = 0;
constexpr i16 kCraftingGrid   = 1;   // 1..9
constexpr i16 kCraftingPlayer = 10;  // 10..45

constexpr i16 kFurnaceInput  = 0;
constexpr i16 kFurnaceFuel   = 1;
constexpr i16 kFurnaceOutput = 2;
constexpr i16 kFurnacePlayer = 3;  // 3..38

[[nodiscard]] i16 container_slots(WorkbenchKind kind) {
    return kind == WorkbenchKind::CraftingTable ? kCraftingPlayer : kFurnacePlayer;
}

[[nodiscard]] RecipeStack to_recipe(const net::ItemStack& stack) {
    return RecipeStack{stack.item_id, stack.count};
}

[[nodiscard]] net::ItemStack to_wire(const RecipeStack& stack) {
    if (stack.empty()) {
        return {};
    }
    return net::ItemStack{stack.item, static_cast<i8>(std::min<i32>(stack.count, 127)), {}};
}

[[nodiscard]] i8 stack_limit(const WorkbenchContext& context, i32 item) {
    return context.registries != nullptr ? context.registries->max_stack_size(item) : i8{64};
}

/// The grid as ov_gameplay wants it.
[[nodiscard]] gameplay::CraftingGrid grid_of(const Workbench& bench) {
    gameplay::CraftingGrid grid;
    grid.width = grid.height = 3;
    for (usize cell = 0; cell < grid.cells.size(); ++cell) {
        grid.cells[cell] = to_recipe(bench.grid[cell]);
    }
    return grid;
}

void write_grid(Workbench& bench, const gameplay::CraftingGrid& grid) {
    for (usize cell = 0; cell < grid.cells.size(); ++cell) {
        bench.grid[cell] = to_wire(grid.cells[cell]);
    }
}

/// What the result slot currently shows.
[[nodiscard]] net::ItemStack crafting_result(const WorkbenchContext& context,
                                             const Workbench&        bench) {
    if (context.book == nullptr) {
        return {};
    }
    const auto made = gameplay::match_crafting(*context.book, grid_of(bench));
    return made ? to_wire(made->result) : net::ItemStack{};
}

/// Put a stack into a range of slots, merging first and only then filling
/// empties. That order is what makes a shift-click feel like one move rather
/// than scattering a stack across every free slot.
///
/// Returns what would not fit.
[[nodiscard]] net::ItemStack deposit(const WorkbenchContext&   context,
                                     std::span<net::ItemStack> slots, net::ItemStack stack,
                                     bool reverse = false) {
    if (stack.empty()) {
        return {};
    }
    const i8    limit = stack_limit(context, stack.item_id);
    const usize count = slots.size();

    for (usize step = 0; step < count && !stack.empty(); ++step) {
        net::ItemStack& into = slots[reverse ? count - 1 - step : step];
        if (into.empty() || into.item_id != stack.item_id || into.count >= limit) {
            continue;
        }
        const i8 moved = std::min<i8>(static_cast<i8>(limit - into.count), stack.count);
        into.count     = static_cast<i8>(into.count + moved);
        stack.count    = static_cast<i8>(stack.count - moved);
    }
    for (usize step = 0; step < count && !stack.empty(); ++step) {
        net::ItemStack& into = slots[reverse ? count - 1 - step : step];
        if (!into.empty()) {
            continue;
        }
        const i8 moved = std::min<i8>(limit, stack.count);
        into           = stack;
        into.count     = moved;
        stack.count    = static_cast<i8>(stack.count - moved);
    }
    return stack.empty() ? net::ItemStack{} : stack;
}

/// Would this furnace slot accept that stack from a shift-click?
[[nodiscard]] bool furnace_accepts(const WorkbenchContext& context, FurnaceKind kind, i16 slot,
                                   const net::ItemStack& stack) {
    if (context.book == nullptr || stack.empty()) {
        return false;
    }
    if (slot == kFurnaceInput) {
        return gameplay::match_cooking(*context.book, kind, stack.item_id).has_value();
    }
    if (slot == kFurnaceFuel) {
        return gameplay::burn_ticks(*context.book, kind, stack.item_id) > 0;
    }
    return false;
}

}  // namespace

std::optional<WorkbenchKind> workbench_of_block(std::string_view block_name) {
    if (block_name == "minecraft:crafting_table") {
        return WorkbenchKind::CraftingTable;
    }
    if (block_name == "minecraft:furnace") {
        return WorkbenchKind::Furnace;
    }
    if (block_name == "minecraft:blast_furnace") {
        return WorkbenchKind::BlastFurnace;
    }
    if (block_name == "minecraft:smoker") {
        return WorkbenchKind::Smoker;
    }
    return std::nullopt;
}

std::optional<FurnaceKind> furnace_of(WorkbenchKind kind) {
    switch (kind) {
        case WorkbenchKind::Furnace:
            return FurnaceKind::Furnace;
        case WorkbenchKind::BlastFurnace:
            return FurnaceKind::BlastFurnace;
        case WorkbenchKind::Smoker:
            return FurnaceKind::Smoker;
        case WorkbenchKind::CraftingTable:
            break;
    }
    return std::nullopt;
}

std::string_view menu_name(WorkbenchKind kind) {
    switch (kind) {
        case WorkbenchKind::CraftingTable:
            return "minecraft:crafting";
        case WorkbenchKind::Furnace:
            return "minecraft:furnace";
        case WorkbenchKind::BlastFurnace:
            return "minecraft:blast_furnace";
        case WorkbenchKind::Smoker:
            return "minecraft:smoker";
    }
    return "minecraft:crafting";
}

std::string_view screen_title(WorkbenchKind kind) {
    switch (kind) {
        case WorkbenchKind::CraftingTable:
            return "Crafting";
        case WorkbenchKind::Furnace:
            return "Furnace";
        case WorkbenchKind::BlastFurnace:
            return "Blast Furnace";
        case WorkbenchKind::Smoker:
            return "Smoker";
    }
    return "Crafting";
}

usize window_slot_count(WorkbenchKind kind) {
    return static_cast<usize>(container_slots(kind)) + 36;
}

std::vector<net::ItemStack> window_contents(const WorkbenchContext& context,
                                            const Workbench&        bench,
                                            std::span<const net::ItemStack> player_inventory) {
    std::vector<net::ItemStack> slots;
    slots.reserve(window_slot_count(bench.kind));
    if (bench.kind == WorkbenchKind::CraftingTable) {
        slots.push_back(crafting_result(context, bench));
        slots.insert(slots.end(), bench.grid.begin(), bench.grid.end());
    } else {
        slots.push_back(to_wire(bench.furnace_slots.input));
        slots.push_back(to_wire(bench.furnace_slots.fuel));
        slots.push_back(to_wire(bench.furnace_slots.output));
    }
    for (usize i = 9; i < 45 && i < player_inventory.size(); ++i) {
        slots.push_back(player_inventory[i]);
    }
    return slots;
}

std::array<i16, 4> furnace_properties(const Workbench& bench) {
    // 0 fuel left, 1 what it started at, 2 cooking progress, 3 how long the
    // recipe takes. The client draws both bars from these four and computes
    // nothing; sending 1 as zero makes the flame vanish while the furnace burns.
    return {static_cast<i16>(std::min(bench.furnace_state.lit_time, 32767)),
            static_cast<i16>(std::min(bench.furnace_state.lit_duration, 32767)),
            static_cast<i16>(std::min(bench.furnace_state.cook_time, 32767)),
            static_cast<i16>(std::min(bench.furnace_state.cook_total, 32767))};
}

gameplay::FurnaceTick tick_furnace(const WorkbenchContext& context, Workbench& bench) {
    const auto kind = furnace_of(bench.kind);
    if (!kind || context.book == nullptr) {
        return {};
    }
    return gameplay::furnace_tick(*context.book, *kind, bench.furnace_slots,
                                  bench.furnace_state);
}

// ── The click ───────────────────────────────────────────────────────────────

WorkbenchOutcome apply_click(const WorkbenchContext& context, Workbench& bench,
                             const net::ContainerClick& click,
                             std::span<net::ItemStack> player_inventory,
                             net::ItemStack&           carried) {
    WorkbenchOutcome outcome;
    outcome.handled = true;

    const i16  first_player = container_slots(bench.kind);
    const auto total        = static_cast<i16>(window_slot_count(bench.kind));

    // The player's own slots are 9..44, so the mapping is not the identity and
    // getting it wrong moves items between the wrong two places.
    const auto slot_ref = [&](i16 index) -> net::ItemStack* {
        if (index >= first_player && index < total) {
            const auto into = static_cast<usize>(index - first_player + 9);
            return into < player_inventory.size() ? &player_inventory[into] : nullptr;
        }
        if (bench.kind == WorkbenchKind::CraftingTable) {
            if (index >= kCraftingGrid && index < kCraftingPlayer) {
                return &bench.grid[static_cast<usize>(index - kCraftingGrid)];
            }
            return nullptr;  // the result slot is not storage
        }
        // A furnace's own three slots are not here: they live in ov_gameplay's
        // stack type and get their own view below, so that a click does not
        // copy them in and out on every path.
        return nullptr;
    };

    // The furnace's three slots live in ov_gameplay's stack type, so they get a
    // view of their own rather than being copied in and out around every click.
    std::array<net::ItemStack, 3> furnace_view{to_wire(bench.furnace_slots.input),
                                               to_wire(bench.furnace_slots.fuel),
                                               to_wire(bench.furnace_slots.output)};
    const auto                    furnace_ref = [&](i16 index) -> net::ItemStack* {
        if (bench.kind == WorkbenchKind::CraftingTable) {
            return nullptr;
        }
        if (index >= kFurnaceInput && index <= kFurnaceOutput) {
            return &furnace_view[static_cast<usize>(index)];
        }
        return nullptr;
    };
    const auto any_ref = [&](i16 index) -> net::ItemStack* {
        net::ItemStack* direct = slot_ref(index);
        return direct != nullptr ? direct : furnace_ref(index);
    };
    const auto commit_furnace = [&]() {
        bench.furnace_slots.input  = to_recipe(furnace_view[0]);
        bench.furnace_slots.fuel   = to_recipe(furnace_view[1]);
        bench.furnace_slots.output = to_recipe(furnace_view[2]);
        outcome.save_block_entity  = true;
    };

    const bool result_slot =
        bench.kind == WorkbenchKind::CraftingTable && click.slot == kCraftingResult;
    const bool furnace_output = bench.kind != WorkbenchKind::CraftingTable &&
                                click.slot == kFurnaceOutput;

    // ── Taking a crafted result ─────────────────────────────────────────────
    //
    // Not an ordinary move: the result slot has nothing in it until the grid
    // says so, and taking from it consumes the grid.
    if (result_slot && context.book != nullptr) {
        const gameplay::CraftingGrid grid = grid_of(bench);
        const auto                   made = gameplay::match_crafting(*context.book, grid);
        if (!made) {
            return outcome;
        }

        if (click.mode == 1) {
            // Shift-click: craft in a loop until the grid runs out or the
            // player does. The loop is bounded by the grid, which shrinks every
            // pass — but it is bounded explicitly as well, because a recipe
            // whose ingredients came back as remainders would otherwise match
            // for ever. A cake's three milk buckets do exactly that.
            gameplay::CraftingGrid current = grid;
            for (int pass = 0; pass < 64 * 9; ++pass) {
                const auto again = gameplay::match_crafting(*context.book, current);
                if (!again || again->result.item != made->result.item) {
                    break;
                }
                const net::ItemStack product = to_wire(again->result);
                // Vanilla fills the hotbar first on a shift-click out of a
                // result slot, then the main inventory.
                net::ItemStack left =
                    deposit(context, player_inventory.subspan(36, 9), product);
                if (!left.empty()) {
                    left = deposit(context, player_inventory.subspan(9, 27), left);
                }
                if (!left.empty()) {
                    break;  // nowhere to put it: stop before consuming the grid
                }
                const gameplay::CraftConsumption after =
                    gameplay::consume_craft(*context.book, current, *again);
                current = after.grid;
                for (usize i = 0; i < after.overflow_count; ++i) {
                    outcome.overflow.push_back(to_wire(after.overflow[i]));
                }
            }
            write_grid(bench, current);
            return outcome;
        }

        if (click.mode == 0) {
            // An ordinary click takes one result onto the cursor, and only if
            // the cursor can hold it.
            const net::ItemStack product = to_wire(made->result);
            if (carried.empty()) {
                carried = product;
            } else if (carried.item_id == product.item_id &&
                       carried.count + product.count <= stack_limit(context, product.item_id)) {
                carried.count = static_cast<i8>(carried.count + product.count);
            } else {
                return outcome;
            }
            const gameplay::CraftConsumption after =
                gameplay::consume_craft(*context.book, grid, *made);
            write_grid(bench, after.grid);
            for (usize i = 0; i < after.overflow_count; ++i) {
                outcome.overflow.push_back(to_wire(after.overflow[i]));
            }
        }
        return outcome;
    }

    // ── Emptying a furnace's output ─────────────────────────────────────────
    if (furnace_output && (click.mode == 0 || click.mode == 1)) {
        net::ItemStack& out = furnace_view[static_cast<usize>(kFurnaceOutput)];
        if (out.empty()) {
            return outcome;
        }
        if (click.mode == 1) {
            const net::ItemStack left =
                deposit(context, player_inventory.subspan(9, 36), out, true);
            out = left;
        } else if (carried.empty()) {
            carried = out;
            out     = {};
        } else if (carried.item_id == out.item_id &&
                   carried.count + out.count <= stack_limit(context, out.item_id)) {
            carried.count = static_cast<i8>(carried.count + out.count);
            out           = {};
        } else {
            return outcome;
        }
        // The furnace hands over everything it has been holding, all at once —
        // which is why a furnace left running all night pays out in one go.
        outcome.experience              = bench.furnace_state.stored_experience;
        bench.furnace_state.stored_experience = 0.0F;
        commit_furnace();
        return outcome;
    }

    // ── Shift-click, everywhere else ────────────────────────────────────────
    if (click.mode == 1 && click.slot >= 0 && click.slot < total) {
        net::ItemStack* from = any_ref(click.slot);
        if (from == nullptr || from->empty()) {
            return outcome;
        }
        const bool from_player = click.slot >= first_player;
        if (!from_player) {
            // Out of the container and into the player, back to front: vanilla
            // fills the hotbar first when a container gives something up.
            *from = deposit(context, player_inventory.subspan(9, 36), *from, true);
            if (bench.kind != WorkbenchKind::CraftingTable) {
                commit_furnace();
            }
            return outcome;
        }

        if (bench.kind == WorkbenchKind::CraftingTable) {
            *from = deposit(context, std::span{bench.grid}, *from);
            return outcome;
        }

        // Into a furnace, the slot is chosen by what the item is: a fuel goes
        // to the fuel slot and an ore to the input. Vanilla decides the same
        // way, and an implementation that always picks slot 0 makes coal
        // impossible to load without dragging.
        const auto kind = furnace_of(bench.kind);
        if (kind) {
            for (const i16 target : {kFurnaceInput, kFurnaceFuel}) {
                if (!furnace_accepts(context, *kind, target, *from)) {
                    continue;
                }
                *from = deposit(context, std::span{furnace_view}.subspan(
                                             static_cast<usize>(target), 1),
                                *from);
                commit_furnace();
                break;
            }
        }
        return outcome;
    }

    // ── The five ordinary modes ─────────────────────────────────────────────
    if (click.mode == 2 && click.button >= 0 && click.button < 9) {
        // A number key must not push anything into a furnace's output: it is a
        // slot the game fills and the player only empties.
        if (furnace_output) {
            return outcome;
        }
        net::ItemStack* slot   = any_ref(click.slot);
        net::ItemStack* hotbar = &player_inventory[36 + static_cast<usize>(click.button)];
        if (slot != nullptr && slot != hotbar) {
            std::swap(*slot, *hotbar);
            if (bench.kind != WorkbenchKind::CraftingTable) {
                commit_furnace();
            }
        }
        return outcome;
    }

    if (click.mode == 4) {
        net::ItemStack* slot = click.slot == -999 ? &carried : any_ref(click.slot);
        if (slot != nullptr && !slot->empty()) {
            const i8       amount = click.button == 1 ? slot->count : static_cast<i8>(1);
            net::ItemStack thrown = *slot;
            thrown.count          = amount;
            slot->count           = static_cast<i8>(slot->count - amount);
            if (slot->count <= 0) {
                *slot = {};
            }
            outcome.overflow.push_back(thrown);
            if (bench.kind != WorkbenchKind::CraftingTable) {
                commit_furnace();
            }
        }
        return outcome;
    }

    if (click.mode == 0 && click.slot >= 0 && click.slot < total) {
        net::ItemStack* slot = any_ref(click.slot);
        if (slot == nullptr) {
            return outcome;
        }
        if (click.button == 0) {
            const i8 limit = stack_limit(context, carried.item_id);
            if (!carried.empty() && !slot->empty() && slot->item_id == carried.item_id &&
                slot->count < limit) {
                const i8 moved = std::min<i8>(static_cast<i8>(limit - slot->count), carried.count);
                slot->count    = static_cast<i8>(slot->count + moved);
                carried.count  = static_cast<i8>(carried.count - moved);
                if (carried.count <= 0) {
                    carried = {};
                }
            } else {
                std::swap(*slot, carried);
            }
        } else if (click.button == 1) {
            if (carried.empty()) {
                if (!slot->empty()) {
                    // Rounding up on the half is what vanilla does; rounding
                    // down loses an item on odd stacks.
                    const i8 half = static_cast<i8>((slot->count + 1) / 2);
                    carried       = *slot;
                    carried.count = half;
                    slot->count   = static_cast<i8>(slot->count - half);
                    if (slot->count <= 0) {
                        *slot = {};
                    }
                }
            } else {
                const i8   limit = stack_limit(context, carried.item_id);
                const bool same  = !slot->empty() && slot->item_id == carried.item_id;
                if (slot->empty() || (same && slot->count < limit)) {
                    if (slot->empty()) {
                        *slot       = carried;
                        slot->count = 1;
                    } else {
                        slot->count = static_cast<i8>(slot->count + 1);
                    }
                    carried.count = static_cast<i8>(carried.count - 1);
                    if (carried.count <= 0) {
                        carried = {};
                    }
                }
            }
        }
        if (bench.kind != WorkbenchKind::CraftingTable) {
            commit_furnace();
        }
        return outcome;
    }

    return outcome;
}

// ── The furnace's block entity ──────────────────────────────────────────────

namespace {

[[nodiscard]] RecipeStack read_item(const WorkbenchContext& context, const nbt::Tag& entry) {
    const nbt::Tag* id    = entry.find("id");
    const nbt::Tag* count = entry.find("Count");
    if (id == nullptr || count == nullptr || context.registries == nullptr) {
        return {};
    }
    const auto item = context.registries->protocol_id(context.item_registry, id->as_string());
    if (!item) {
        return {};
    }
    return RecipeStack{*item, static_cast<i32>(count->as_i64())};
}

}  // namespace

void load_furnace(const WorkbenchContext& context, const nbt::Tag& data, Workbench& bench) {
    bench.furnace_slots = {};
    // A furnace's `Items` list omits empty slots and is not indexed by slot, so
    // its length says nothing: the `Slot` field of each entry is the only thing
    // that says where a stack goes.
    const nbt::Tag* items = data.find("Items");
    if (items != nullptr && items->list() != nullptr) {
        for (const nbt::Tag& entry : *items->list()) {
            const nbt::Tag* slot = entry.find("Slot");
            if (slot == nullptr) {
                continue;
            }
            switch (slot->as_i64()) {
                case 0:
                    bench.furnace_slots.input = read_item(context, entry);
                    break;
                case 1:
                    bench.furnace_slots.fuel = read_item(context, entry);
                    break;
                case 2:
                    bench.furnace_slots.output = read_item(context, entry);
                    break;
                default:
                    break;
            }
        }
    }

    bench.furnace_state.lit_time   = static_cast<i32>(data.find("BurnTime") != nullptr
                                                          ? data.find("BurnTime")->as_i64()
                                                          : 0);
    bench.furnace_state.cook_time  = static_cast<i32>(
        data.find("CookTime") != nullptr ? data.find("CookTime")->as_i64() : 0);
    bench.furnace_state.cook_total = static_cast<i32>(
        data.find("CookTimeTotal") != nullptr ? data.find("CookTimeTotal")->as_i64() : 0);
    // Vanilla does not store what the burn started at, and recomputes it from
    // what is left — so a furnace reloaded mid-burn shows a full flame there
    // too. Matching that is cheaper than being subtly different.
    bench.furnace_state.lit_duration = bench.furnace_state.lit_time;

    const nbt::Tag* experience = data.find("ovExperience");
    bench.furnace_state.stored_experience =
        experience != nullptr ? static_cast<f32>(experience->as_f64()) : 0.0F;
}

void store_furnace(const WorkbenchContext& context, nbt::Tag& data, const Workbench& bench) {
    nbt::Tag items = nbt::Tag::make_list(nbt::TagType::Compound);
    const std::array<const RecipeStack*, 3> slots{&bench.furnace_slots.input,
                                                  &bench.furnace_slots.fuel,
                                                  &bench.furnace_slots.output};
    for (usize index = 0; index < slots.size(); ++index) {
        if (slots[index]->empty() || context.registries == nullptr) {
            continue;  // empty slots are omitted, not stored as air
        }
        const std::string_view name =
            context.registries->entry_of(context.item_registry, slots[index]->item);
        if (name.empty()) {
            continue;
        }
        nbt::Tag entry = nbt::Tag::make_compound();
        entry.put("Slot", nbt::Tag{static_cast<i8>(index)});
        entry.put("id", nbt::Tag{std::string{name}});
        entry.put("Count", nbt::Tag{static_cast<i8>(std::min(slots[index]->count, 127))});
        (void)items.push(std::move(entry));
    }
    (void)data.put("Items", std::move(items));
    (void)data.put("BurnTime", nbt::Tag{static_cast<i16>(bench.furnace_state.lit_time)});
    (void)data.put("CookTime", nbt::Tag{static_cast<i16>(bench.furnace_state.cook_time)});
    (void)data.put("CookTimeTotal", nbt::Tag{static_cast<i16>(bench.furnace_state.cook_total)});
    // Vanilla keeps a per-recipe tally in `RecipesUsed` and turns it into
    // experience when the output is taken. A single total is enough for what
    // this server does with it, and the name is prefixed so that nothing
    // mistakes it for a vanilla field.
    (void)data.put("ovExperience", nbt::Tag{bench.furnace_state.stored_experience});
}

// ── The window, joined up ───────────────────────────────────────────────────

namespace {

/// Resend the whole window.
///
/// Whether the click was handled or not: the client has already applied its own
/// guess, and anything the server did differently would otherwise stay on
/// screen as an item that does not exist.
void resend(const WorkbenchContext& context, const WorkbenchHost& host, const Workbench& bench,
            std::span<const net::ItemStack> inventory, const net::ItemStack& carried,
            i32 state_id) {
    host.send(net::clientbound::kContainerContent,
              net::encode_container_content(bench.window_id, state_id,
                                            window_contents(context, bench, inventory), carried));
}

/// Send the four numbers whose value has changed since last time.
void send_properties(const WorkbenchHost& host, Workbench& bench) {
    const std::array<i16, 4> now = furnace_properties(bench);
    for (i16 index = 0; index < 4; ++index) {
        const auto slot = static_cast<usize>(index);
        if (now[slot] == bench.sent_properties[slot]) {
            continue;
        }
        bench.sent_properties[slot] = now[slot];
        host.send(net::clientbound::kContainerProperty,
                  net::encode_container_property(bench.window_id, index, now[slot]));
    }
}

}  // namespace

bool open_workbench(const WorkbenchContext& context, const WorkbenchHost& host, i32 x, i32 y,
                    i32 z, u8 window_id, std::span<const net::ItemStack> inventory,
                    std::optional<Workbench>& out) {
    if (context.registries == nullptr || context.book == nullptr) {
        return false;
    }
    const auto kind = workbench_of_block(host.block_name(x, y, z));
    if (!kind) {
        return false;
    }
    // The menu id is one the client hard-codes and never receives, so it comes
    // out of the registry rather than being written down: a furnace opened as
    // a crafting table shows three slots and swallows what goes into them.
    const auto menu = context.registries->protocol_id(context.menu_registry, menu_name(*kind));
    if (!menu) {
        return false;
    }

    Workbench bench;
    bench.kind      = *kind;
    bench.window_id = window_id;
    bench.x         = x;
    bench.y         = y;
    bench.z         = z;
    if (furnace_of(*kind)) {
        const nbt::Tag* data = host.block_entity(x, y, z);
        if (data != nullptr) {
            load_furnace(context, *data, bench);
        }
    }

    host.send(net::clientbound::kOpenScreen,
              net::encode_open_screen(window_id, *menu, screen_title(*kind)));
    resend(context, host, bench, inventory, {}, 1);
    if (furnace_of(*kind)) {
        send_properties(host, bench);
    }
    out = std::move(bench);
    return true;
}

void handle_click(const WorkbenchContext& context, const WorkbenchHost& host, Workbench& bench,
                  const net::ContainerClick& click, std::span<net::ItemStack> inventory,
                  net::ItemStack& carried, std::optional<Workbench>& holder) {
    // The block may have been broken while the screen was open. Vanilla closes
    // the screen; carrying on would write a furnace's contents into a hole.
    if (workbench_of_block(host.block_name(bench.x, bench.y, bench.z)) != bench.kind) {
        close_workbench(host, bench);
        host.send(net::clientbound::kCloseContainer, net::encode_close_container(bench.window_id));
        holder.reset();
        return;
    }

    const WorkbenchOutcome outcome = apply_click(context, bench, click, inventory, carried);

    if (outcome.save_block_entity) {
        if (nbt::Tag* data = host.block_entity(bench.x, bench.y, bench.z); data != nullptr) {
            store_furnace(context, *data, bench);
            host.mark_dirty(bench.x, bench.z);
        }
    }
    for (const net::ItemStack& stack : outcome.overflow) {
        host.drop(stack);
    }
    if (outcome.experience > 0.0F) {
        host.award_experience(outcome.experience);
    }

    resend(context, host, bench, inventory, carried, click.state_id + 1);
    if (furnace_of(bench.kind)) {
        send_properties(host, bench);
    }
}

void tick_open_workbench(const WorkbenchContext& context, const WorkbenchHost& host,
                         Workbench& bench, std::span<const net::ItemStack> inventory) {
    if (!furnace_of(bench.kind)) {
        return;
    }
    const gameplay::FurnaceTick step = tick_furnace(context, bench);
    if (step.lit_changed) {
        host.set_lit(bench.x, bench.y, bench.z, bench.furnace_state.lit());
    }
    if (step.slots_changed) {
        if (nbt::Tag* data = host.block_entity(bench.x, bench.y, bench.z); data != nullptr) {
            store_furnace(context, *data, bench);
            host.mark_dirty(bench.x, bench.z);
        }
        // The state id is not advanced here: the client is not waiting on an
        // acknowledgement, it is being told what changed on its own.
        resend(context, host, bench, inventory, {}, 0);
    }
    send_properties(host, bench);
}

void send_recipe_book(const WorkbenchContext&                          context,
                      const std::function<void(i32, std::vector<u8>)>& send) {
    if (context.book == nullptr) {
        return;
    }
    const gameplay::RecipeBook& book = *context.book;

    // The choice lists are spans into the pack, so nothing here copies an
    // ingredient — but the `WireIngredient` wrappers and the recipe list do
    // have to exist while the packet is written, which is why they are
    // reserved up front rather than grown per recipe.
    std::vector<net::WireIngredient> ingredients;
    std::vector<net::WireRecipe>     wire;
    ingredients.reserve(book.size() * 4);
    wire.reserve(book.size());

    // Two passes: the first counts, so that reserving is exact and no
    // reallocation invalidates the spans the second pass hands out.
    usize total_ingredients = 0;
    for (gameplay::RecipeIndex index = 0; index < book.size(); ++index) {
        total_ingredients += book.ingredients(index).size();
    }
    ingredients.reserve(total_ingredients);

    for (gameplay::RecipeIndex index = 0; index < book.size(); ++index) {
        const registry::RecipeRecord& record = book.recipe(index);
        const usize                   first  = ingredients.size();
        for (u32 slot = 0; slot < record.ingredient_count; ++slot) {
            ingredients.push_back(net::WireIngredient{book.choices(record.ingredient_first + slot)});
        }

        net::WireRecipe one;
        one.identifier = book.name(index);
        one.type       = book.type_name(index);
        one.group      = book.group(index);
        one.category   = record.category;
        one.width      = record.width;
        one.height     = record.height;
        one.show_notification =
            (record.flags & registry::kFlagShowNotification) != 0;
        one.experience   = record.experience;
        one.cooking_time = static_cast<i32>(record.cook_time);
        one.ingredients =
            std::span{ingredients}.subspan(first, ingredients.size() - first);
        if (const auto result = book.result(index)) {
            one.result = to_wire(*result);
        }
        wire.push_back(one);
    }

    send(net::clientbound::kUpdateRecipes, net::encode_update_recipes(wire));

    std::vector<std::string_view> names;
    names.reserve(book.size());
    for (gameplay::RecipeIndex index = 0; index < book.size(); ++index) {
        names.push_back(book.name(index));
    }
    net::RecipeBookState state;
    state.action  = 0;
    state.recipes = names;
    send(net::clientbound::kUpdateRecipeBook, net::encode_update_recipe_book(state));
}

void close_workbench(const WorkbenchHost& host, Workbench& bench) {
    if (bench.kind != WorkbenchKind::CraftingTable) {
        return;
    }
    for (net::ItemStack& stack : bench.grid) {
        if (!stack.empty()) {
            host.drop(stack);
            stack = {};
        }
    }
}

}  // namespace ov::server
