#include "enchant_session.hpp"

#include "survival_session.hpp"

#include "ov/gameplay/experience.hpp"
#include "ov/nbt/binary.hpp"
#include "ov/nbt/tag.hpp"
#include "ov/protocol/recipe_packets.hpp"

#include <algorithm>
#include <string>

namespace ov::server {
namespace {

using gameplay::EnchantStack;

constexpr usize kPlayerFirst = 9;   // the protocol's slot 9 is the main inventory
constexpr usize kPlayerCount = 36;  // 27 + 9

[[nodiscard]] i16 container_slots(EnchantScreen kind) {
    return kind == EnchantScreen::Table ? i16{2} : i16{3};
}

[[nodiscard]] std::string_view menu_of(EnchantScreen kind) {
    switch (kind) {
        case EnchantScreen::Table:
            return "minecraft:enchantment";
        case EnchantScreen::Anvil:
            return "minecraft:anvil";
        case EnchantScreen::Grindstone:
            return "minecraft:grindstone";
    }
    return "minecraft:enchantment";
}

[[nodiscard]] std::string_view title_of(EnchantScreen kind) {
    switch (kind) {
        case EnchantScreen::Table:
            return "Enchant";
        case EnchantScreen::Anvil:
            return "Repair & Name";
        case EnchantScreen::Grindstone:
            return "Repair & Disenchant";
    }
    return "Enchant";
}

[[nodiscard]] std::string_view name_of(const EnchantContext& context, i32 item) {
    if (context.registries == nullptr || item == 0) {
        return {};
    }
    return context.registries->entry_of(context.item_registry, item);
}

[[nodiscard]] std::optional<i32> id_of(const EnchantContext& context, std::string_view name) {
    if (context.registries == nullptr) {
        return std::nullopt;
    }
    const auto id = context.registries->protocol_id(context.item_registry, name);
    if (!id) {
        return std::nullopt;
    }
    return static_cast<i32>(*id);
}

[[nodiscard]] i8 stack_limit(const EnchantContext& context, i32 item) {
    return context.registries != nullptr ? context.registries->max_stack_size(item) : i8{64};
}

/// The item's tag compound, or an empty compound.
[[nodiscard]] nbt::Tag tag_of(const net::ItemStack& stack) {
    if (!stack.nbt.empty()) {
        if (auto document = nbt::read(stack.nbt); document && document->root.compound() != nullptr) {
            return std::move(document->root);
        }
    }
    return nbt::Tag::make_compound();
}

void set_tag(net::ItemStack& stack, const nbt::Tag& tag) {
    if (tag.compound() == nullptr || tag.empty()) {
        stack.nbt.clear();
        return;
    }
    stack.nbt = nbt::write(nbt::Document{"tag", tag});
}

[[nodiscard]] bool same_kind(const net::ItemStack& a, const net::ItemStack& b) {
    return a.item_id == b.item_id && a.nbt == b.nbt;
}

[[nodiscard]] bool enchanted(const net::ItemStack& stack) {
    if (stack.nbt.empty()) {
        return false;
    }
    const nbt::Tag  tag  = tag_of(stack);
    const nbt::Tag* list = tag.find("Enchantments");
    return list != nullptr && list->list() != nullptr && !list->list()->empty();
}

/// What a slot of this screen admits.
[[nodiscard]] bool admits(const EnchantContext& context, EnchantScreen kind, i16 slot,
                          const net::ItemStack& stack) {
    if (stack.empty()) {
        return true;
    }
    const std::string_view name = name_of(context, stack.item_id);
    switch (kind) {
        case EnchantScreen::Table:
            return slot == 0 || (slot == 1 && name == "minecraft:lapis_lazuli");
        case EnchantScreen::Anvil:
            return slot == 0 || slot == 1;
        case EnchantScreen::Grindstone:
            return (slot == 0 || slot == 1) &&
                   (gameplay::enchant_max_damage(name).has_value() ||
                    name == "minecraft:enchanted_book" || enchanted(stack));
    }
    return false;
}

/// How many of a stack a slot takes: the table's item slot takes one.
[[nodiscard]] i8 slot_limit(const EnchantContext& context, EnchantScreen kind, i16 slot,
                            i32 item) {
    if (kind == EnchantScreen::Table && slot == 0) {
        return 1;
    }
    return stack_limit(context, item);
}

/// Merge into matching stacks, then fill empties. Returns what did not fit.
[[nodiscard]] net::ItemStack deposit(const EnchantContext& context, std::span<net::ItemStack> into,
                                     net::ItemStack stack, bool reverse) {
    if (stack.empty()) {
        return {};
    }
    const i8    limit = stack_limit(context, stack.item_id);
    const usize n     = into.size();
    for (int pass = 0; pass < 2 && !stack.empty(); ++pass) {
        for (usize step = 0; step < n && !stack.empty(); ++step) {
            net::ItemStack& slot = into[reverse ? n - 1 - step : step];
            if (pass == 0) {
                if (slot.empty() || !same_kind(slot, stack) || slot.count >= limit) {
                    continue;
                }
                const i8 moved = std::min<i8>(static_cast<i8>(limit - slot.count), stack.count);
                slot.count     = static_cast<i8>(slot.count + moved);
                stack.count    = static_cast<i8>(stack.count - moved);
            } else if (slot.empty()) {
                slot         = stack;
                slot.count   = std::min<i8>(limit, stack.count);
                stack.count  = static_cast<i8>(stack.count - slot.count);
            }
        }
    }
    return stack.empty() ? net::ItemStack{} : stack;
}

/// The protocol's hotbar then main inventory, the order vanilla gives things
/// back in.
void give_back(const EnchantContext& context, const EnchantHost& host,
               std::span<net::ItemStack> inventory, net::ItemStack stack) {
    if (stack.empty()) {
        return;
    }
    if (inventory.size() >= 45) {
        stack = deposit(context, inventory.subspan(36, 9), stack, false);
        stack = deposit(context, inventory.subspan(9, 27), stack, false);
    }
    if (!stack.empty() && host.drop) {
        host.drop(stack);
    }
}

[[nodiscard]] i32 count_shelves(const EnchantContext& context, const EnchantHost& host,
                                const EnchantWindow& window) {
    if (context.registries == nullptr || !host.block_name) {
        return 0;
    }
    const registry::Registries& regs     = *context.registries;
    const auto                  provider = regs.find_tag(context.block_registry,
                                                         "minecraft:enchantment_power_provider");
    const auto transmitter = regs.find_tag(context.block_registry,
                                           "minecraft:enchantment_power_transmitter");
    if (!provider || !transmitter) {
        return 0;
    }
    const auto in_tag = [&](registry::TagId tag, i32 x, i32 y, i32 z) {
        const std::string_view name = host.block_name(x, y, z);
        if (name.empty()) {
            return false;
        }
        const auto id = regs.protocol_id(context.block_registry, name);
        return id && regs.tag_contains(tag, *id);
    };
    std::array<gameplay::ShelfProbe, 32> probes{};
    const auto                           offsets = gameplay::bookshelf_offsets();
    for (usize i = 0; i < offsets.size(); ++i) {
        const gameplay::BlockOffset o   = offsets[i];
        const gameplay::BlockOffset gap = gameplay::bookshelf_gap(o);
        probes[i].shelf = in_tag(*provider, window.x + o.dx, window.y + o.dy, window.z + o.dz);
        probes[i].clear = probes[i].shelf &&
                          in_tag(*transmitter, window.x + gap.dx, window.y + gap.dy,
                                 window.z + gap.dz);
    }
    return gameplay::count_bookshelves(probes);
}

[[nodiscard]] std::array<i32, 10> properties(const EnchantWindow& window) {
    std::array<i32, 10> out{};
    out.fill(0);
    if (window.kind == EnchantScreen::Table) {
        out[0] = window.offers.costs[0];
        out[1] = window.offers.costs[1];
        out[2] = window.offers.costs[2];
        out[4] = window.offers.clue_enchantment[0];
        out[5] = window.offers.clue_enchantment[1];
        out[6] = window.offers.clue_enchantment[2];
        out[7] = window.offers.clue_level[0];
        out[8] = window.offers.clue_level[1];
        out[9] = window.offers.clue_level[2];
    } else if (window.kind == EnchantScreen::Anvil) {
        out[0] = window.anvil.cost;
    }
    return out;
}

void send_properties(const EnchantHost& host, EnchantWindow& window, i32 seed) {
    std::array<i32, 10> now = properties(window);
    usize               count = 0;
    if (window.kind == EnchantScreen::Table) {
        now[3] = gameplay::table_seed_property(seed);
        count  = 10;
    } else if (window.kind == EnchantScreen::Anvil) {
        count = 1;
    }
    for (usize i = 0; i < count; ++i) {
        if (window.sent_any && window.sent[i] == now[i]) {
            continue;
        }
        window.sent[i] = now[i];
        // Every Container Property is a short on the wire; the seed keeps
        // bits 4..15 of itself, which is all the client ever sees.
        host.send(net::clientbound::kContainerProperty,
                  net::encode_container_property(window.window_id, static_cast<i16>(i),
                                                 static_cast<i16>(now[i])));
    }
    window.sent_any = true;
}

void resend(const EnchantHost& host, const EnchantWindow& window,
            std::span<const net::ItemStack> inventory, const net::ItemStack& carried,
            i32 state_id) {
    host.send(net::clientbound::kContainerContent,
              net::encode_container_content(window.window_id, state_id,
                                            enchant_window_contents(window, inventory), carried));
}

[[nodiscard]] i32 current_seed(const EnchantHost& host) { return host.xp_seed ? host.xp_seed() : 0; }

[[nodiscard]] bool creative(const EnchantHost& host) { return host.creative && host.creative(); }

[[nodiscard]] bool output_takeable(const EnchantHost& host, const EnchantWindow& window) {
    if (window.slots[2].empty()) {
        return false;
    }
    if (window.kind == EnchantScreen::Anvil) {
        const i32 level = host.level ? host.level() : 0;
        return (creative(host) || level >= window.anvil.cost) && window.anvil.cost > 0;
    }
    return window.kind == EnchantScreen::Grindstone;
}

/// What taking the output does to everything else.
void on_take(const EnchantHost& host, EnchantWindow& window) {
    if (window.kind == EnchantScreen::Anvil) {
        if (!creative(host) && host.take_levels) {
            host.take_levels(window.anvil.cost);
        }
        window.slots[0] = {};
        net::ItemStack& right = window.slots[1];
        if (window.anvil.right_consumed > 0 && right.count > window.anvil.right_consumed) {
            right.count = static_cast<i8>(right.count - window.anvil.right_consumed);
        } else {
            right = {};
        }
        bool broke = false;
        if (!creative(host) && host.random != nullptr &&
            host.random->next_float() < gameplay::kAnvilDamageChance && host.block_name) {
            const std::string_view next =
                gameplay::damaged_anvil(host.block_name(window.x, window.y, window.z));
            if (host.replace_block) {
                host.replace_block(window.x, window.y, window.z, next);
            }
            broke = next.empty();
        }
        if (host.level_event) {
            host.level_event(broke ? 1029 : 1030, window.x, window.y, window.z);
        }
        return;
    }
    if (window.kind == EnchantScreen::Grindstone) {
        if (host.random != nullptr && host.spawn_experience) {
            const i32 xp = gameplay::grindstone_experience(window.grind.experience_base, *host.random);
            if (xp > 0) {
                host.spawn_experience(xp, window.x + 0.5, window.y + 0.5, window.z + 0.5);
            }
        }
        if (host.level_event) {
            host.level_event(1042, window.x, window.y, window.z);
        }
        window.slots[0] = {};
        window.slots[1] = {};
    }
}

}  // namespace

// ── Stacks ──────────────────────────────────────────────────────────────────

std::optional<EnchantScreen> enchant_screen_of_block(std::string_view block) {
    if (block == "minecraft:enchanting_table") {
        return EnchantScreen::Table;
    }
    if (block == "minecraft:anvil" || block == "minecraft:chipped_anvil" ||
        block == "minecraft:damaged_anvil") {
        return EnchantScreen::Anvil;
    }
    if (block == "minecraft:grindstone") {
        return EnchantScreen::Grindstone;
    }
    return std::nullopt;
}

EnchantStack enchant_stack_of(const EnchantContext& context, const net::ItemStack& stack) {
    EnchantStack out;
    out.item  = name_of(context, stack.item_id);
    out.count = stack.count;
    if (stack.nbt.empty()) {
        return out;
    }
    const nbt::Tag tag = tag_of(stack);
    if (const nbt::Tag* damage = tag.find("Damage")) {
        out.damage = static_cast<i32>(damage->as_i64());
    }
    if (const nbt::Tag* cost = tag.find("RepairCost")) {
        out.repair_cost = static_cast<i32>(cost->as_i64());
    }
    out.enchantments = gameplay::read_enchantments(tag, out.item == "minecraft:enchanted_book");
    if (const nbt::Tag* display = tag.find("display")) {
        out.has_custom_name = display->find("Name") != nullptr;
    }
    return out;
}

net::ItemStack anvil_output(const EnchantContext& context, const net::ItemStack& left,
                            const gameplay::AnvilResult& result, std::string_view rename) {
    if (!result.valid) {
        return {};
    }
    net::ItemStack   out  = left;
    nbt::Tag         tag  = tag_of(left);
    const EnchantStack before = enchant_stack_of(context, left);
    if (result.damage != before.damage) {
        (void)tag.put("Damage", nbt::Tag{std::max(result.damage, 0)});
    }
    const bool book = before.item == "minecraft:enchanted_book";
    (void)tag.erase("Enchantments");
    (void)tag.erase("StoredEnchantments");
    gameplay::write_enchantments(tag, result.enchantments, book);
    if (result.name == gameplay::NameChange::Set) {
        nbt::Tag* display = tag.find("display");
        if (display == nullptr) {
            (void)tag.put("display", nbt::Tag::make_compound());
            display = tag.find("display");
        }
        std::string json = "{\"text\":\"";
        for (const char c : rename) {
            if (c == '"' || c == '\\') {
                json += '\\';
            }
            json += c;
        }
        json += "\"}";
        (void)display->put("Name", nbt::Tag{std::move(json)});
    } else if (result.name == gameplay::NameChange::Reset) {
        if (nbt::Tag* display = tag.find("display")) {
            (void)display->erase("Name");
            if (display->empty()) {
                (void)tag.erase("display");
            }
        }
    }
    (void)tag.put("RepairCost", nbt::Tag{result.repair_cost});
    set_tag(out, tag);
    return out;
}

net::ItemStack grindstone_output(const EnchantContext& context, const net::ItemStack& source,
                                 const gameplay::GrindstoneResult& result) {
    if (!result.valid) {
        return {};
    }
    net::ItemStack out;
    out.count = static_cast<i8>(result.count);
    nbt::Tag tag = tag_of(source);
    if (result.into_book) {
        // A fresh book, carrying over only a custom name.
        nbt::Tag fresh = nbt::Tag::make_compound();
        if (const nbt::Tag* display = tag.find("display"); display && display->find("Name")) {
            nbt::Tag name_only = nbt::Tag::make_compound();
            (void)name_only.put("Name", *display->find("Name"));
            (void)fresh.put("display", std::move(name_only));
        }
        tag = std::move(fresh);
    } else {
        (void)tag.erase("Enchantments");
        (void)tag.erase("StoredEnchantments");
        if (result.damage > 0) {
            (void)tag.put("Damage", nbt::Tag{result.damage});
        } else {
            (void)tag.erase("Damage");
        }
        gameplay::write_enchantments(tag, result.enchantments,
                                     result.item == "minecraft:enchanted_book");
    }
    (void)tag.put("RepairCost", nbt::Tag{result.repair_cost});
    if (result.into_book) {
        (void)tag.erase("RepairCost");
    }
    const auto id = id_of(context, result.item);
    if (!id) {
        return {};
    }
    out.item_id = *id;
    set_tag(out, tag);
    return out;
}

// ── The window ──────────────────────────────────────────────────────────────

std::vector<net::ItemStack> enchant_window_contents(const EnchantWindow& window,
                                                    std::span<const net::ItemStack> inventory) {
    std::vector<net::ItemStack> out;
    const i16                   own = container_slots(window.kind);
    out.reserve(static_cast<usize>(own) + kPlayerCount);
    for (i16 i = 0; i < own; ++i) {
        out.push_back(window.slots[static_cast<usize>(i)]);
    }
    for (usize i = kPlayerFirst; i < kPlayerFirst + kPlayerCount && i < inventory.size(); ++i) {
        out.push_back(inventory[i]);
    }
    return out;
}

void refresh_enchant_window(const EnchantContext& context, const EnchantHost& host,
                            EnchantWindow& window) {
    switch (window.kind) {
        case EnchantScreen::Table: {
            window.offers              = {};
            const net::ItemStack& item = window.slots[0];
            const std::string_view name = name_of(context, item.item_id);
            if (!item.empty() && gameplay::table_accepts(name, item.count, enchanted(item))) {
                window.offers =
                    gameplay::table_offers(current_seed(host), count_shelves(context, host, window),
                                           name);
            }
            break;
        }
        case EnchantScreen::Anvil: {
            window.anvil = {};
            window.slots[2] = {};
            if (window.slots[0].empty()) {
                break;
            }
            gameplay::AnvilRequest request;
            request.left = enchant_stack_of(context, window.slots[0]);
            if (!window.slots[1].empty()) {
                request.right = enchant_stack_of(context, window.slots[1]);
            }
            if (window.rename) {
                request.rename = std::string_view{*window.rename};
            }
            const std::string hover = host.hover_name ? host.hover_name(window.slots[0]) : std::string{};
            request.hover_name = hover;
            request.creative   = creative(host);
            window.anvil       = gameplay::anvil_result(request);
            window.slots[2]    = anvil_output(context, window.slots[0], window.anvil,
                                              window.rename ? std::string_view{*window.rename}
                                                            : std::string_view{});
            break;
        }
        case EnchantScreen::Grindstone: {
            std::optional<EnchantStack> top;
            std::optional<EnchantStack> bottom;
            if (!window.slots[0].empty()) {
                top = enchant_stack_of(context, window.slots[0]);
            }
            if (!window.slots[1].empty()) {
                bottom = enchant_stack_of(context, window.slots[1]);
            }
            window.grind    = gameplay::grindstone_result(top, bottom);
            window.slots[2] = grindstone_output(
                context, !window.slots[0].empty() ? window.slots[0] : window.slots[1], window.grind);
            break;
        }
    }
}

bool open_enchant_screen(const EnchantContext& context, const EnchantHost& host, i32 x, i32 y,
                         i32 z, u8 window_id, std::span<const net::ItemStack> inventory,
                         std::optional<EnchantWindow>& out) {
    if (context.registries == nullptr || !host.block_name) {
        return false;
    }
    const auto kind = enchant_screen_of_block(host.block_name(x, y, z));
    if (!kind) {
        return false;
    }
    const auto menu = context.registries->protocol_id(context.menu_registry, menu_of(*kind));
    if (!menu) {
        return false;
    }
    EnchantWindow window;
    window.kind      = *kind;
    window.window_id = window_id;
    window.x         = x;
    window.y         = y;
    window.z         = z;
    host.send(net::clientbound::kOpenScreen,
              net::encode_open_screen(window_id, static_cast<i32>(*menu), title_of(*kind)));
    resend(host, window, inventory, {}, 1);
    send_properties(host, window, current_seed(host));
    out = std::move(window);
    return true;
}

void enchant_click(const EnchantContext& context, const EnchantHost& host, EnchantWindow& window,
                   const net::ContainerClick& click, std::span<net::ItemStack> inventory,
                   net::ItemStack& carried, std::optional<EnchantWindow>& holder) {
    // The block went while the screen was open: vanilla closes it.
    if (!host.block_name ||
        enchant_screen_of_block(host.block_name(window.x, window.y, window.z)) != window.kind) {
        close_enchant_screen(context, host, window, inventory);
        host.send(net::clientbound::kCloseContainer, net::encode_close_container(window.window_id));
        holder.reset();
        return;
    }

    const i16  own    = container_slots(window.kind);
    const auto total  = static_cast<i16>(own + static_cast<i16>(kPlayerCount));
    const bool output = window.kind != EnchantScreen::Table && click.slot == 2;
    const auto ref    = [&](i16 index) -> net::ItemStack* {
        if (index >= 0 && index < own) {
            return &window.slots[static_cast<usize>(index)];
        }
        if (index >= own && index < total) {
            const auto into = static_cast<usize>(index - own) + kPlayerFirst;
            return into < inventory.size() ? &inventory[into] : nullptr;
        }
        return nullptr;
    };
    const auto player_slots = [&]() { return inventory.subspan(kPlayerFirst, kPlayerCount); };

    if (output) {
        net::ItemStack& out = window.slots[2];
        if (output_takeable(host, window)) {
            bool taken = false;
            if (click.mode == 0) {
                if (carried.empty()) {
                    carried = out;
                    taken   = true;
                } else if (same_kind(carried, out) &&
                           carried.count + out.count <= stack_limit(context, out.item_id)) {
                    carried.count = static_cast<i8>(carried.count + out.count);
                    taken         = true;
                }
            } else if (click.mode == 1) {
                const net::ItemStack left = deposit(context, player_slots(), out, true);
                taken                     = left.empty();
            } else if (click.mode == 2 && click.button >= 0 && click.button < 9) {
                net::ItemStack& hotbar = inventory[36 + static_cast<usize>(click.button)];
                if (hotbar.empty()) {
                    hotbar = out;
                    taken  = true;
                }
            } else if (click.mode == 4) {
                if (host.drop) {
                    host.drop(out);
                }
                taken = true;
            }
            if (taken) {
                out = {};
                on_take(host, window);
            }
        }
    } else if (click.mode == 1 && click.slot >= 0 && click.slot < total) {
        net::ItemStack* from = ref(click.slot);
        if (from != nullptr && !from->empty()) {
            if (click.slot < own) {
                *from = deposit(context, player_slots(), *from, true);
            } else if (window.kind == EnchantScreen::Table) {
                // Lapis to its slot; anything else, one of it, to the item slot.
                if (admits(context, window.kind, 1, *from) &&
                    name_of(context, from->item_id) == "minecraft:lapis_lazuli") {
                    *from = deposit(context, std::span{window.slots}.subspan(1, 1), *from, false);
                } else if (window.slots[0].empty()) {
                    window.slots[0]       = *from;
                    window.slots[0].count = 1;
                    from->count           = static_cast<i8>(from->count - 1);
                    if (from->count <= 0) {
                        *from = {};
                    }
                }
            } else {
                for (i16 slot = 0; slot < 2 && !from->empty(); ++slot) {
                    net::ItemStack& into = window.slots[static_cast<usize>(slot)];
                    if (!admits(context, window.kind, slot, *from)) {
                        continue;
                    }
                    if (window.kind == EnchantScreen::Grindstone) {
                        if (into.empty()) {
                            into       = *from;
                            into.count = 1;
                            from->count = static_cast<i8>(from->count - 1);
                            if (from->count <= 0) {
                                *from = {};
                            }
                        }
                        continue;
                    }
                    *from = deposit(context, std::span{window.slots}.subspan(
                                                 static_cast<usize>(slot), 1),
                                    *from, false);
                }
            }
        }
    } else if (click.mode == 2 && click.button >= 0 && click.button < 9) {
        net::ItemStack* slot   = ref(click.slot);
        net::ItemStack* hotbar = &inventory[36 + static_cast<usize>(click.button)];
        if (slot != nullptr && slot != hotbar) {
            const bool into_container = click.slot < own;
            const i8   limit =
                into_container ? slot_limit(context, window.kind, click.slot, hotbar->item_id) : i8{0};
            if (!into_container) {
                std::swap(*slot, *hotbar);
            } else if (admits(context, window.kind, click.slot, *hotbar)) {
                if (hotbar->count > limit && slot->empty()) {
                    // More than the slot holds: vanilla splits what fits off
                    // the hotbar stack rather than refusing the key.
                    *slot         = *hotbar;
                    slot->count   = limit;
                    hotbar->count = static_cast<i8>(hotbar->count - limit);
                } else if (hotbar->count <= limit) {
                    std::swap(*slot, *hotbar);
                }
            }
        }
    } else if (click.mode == 4) {
        net::ItemStack* slot = click.slot == -999 ? &carried : ref(click.slot);
        if (slot != nullptr && !slot->empty()) {
            const i8       amount = click.button == 1 ? slot->count : i8{1};
            net::ItemStack thrown = *slot;
            thrown.count          = amount;
            slot->count           = static_cast<i8>(slot->count - amount);
            if (slot->count <= 0) {
                *slot = {};
            }
            if (host.drop) {
                host.drop(thrown);
            }
        }
    } else if (click.mode == 0 && click.slot == -999) {
        if (!carried.empty() && host.drop) {
            net::ItemStack thrown = carried;
            if (click.button == 1) {
                thrown.count  = 1;
                carried.count = static_cast<i8>(carried.count - 1);
                if (carried.count <= 0) {
                    carried = {};
                }
            } else {
                carried = {};
            }
            host.drop(thrown);
        }
    } else if (click.mode == 0 && click.slot >= 0 && click.slot < total) {
        net::ItemStack* slot = ref(click.slot);
        if (slot != nullptr) {
            const bool into_container = click.slot < own;
            const bool fits = !into_container || admits(context, window.kind, click.slot, carried);
            const i8   limit = into_container
                                   ? slot_limit(context, window.kind, click.slot, carried.item_id)
                                   : stack_limit(context, carried.item_id);
            if (click.button == 0) {
                if (!carried.empty() && !slot->empty() && same_kind(*slot, carried)) {
                    if (fits && slot->count < limit) {
                        const i8 moved =
                            std::min<i8>(static_cast<i8>(limit - slot->count), carried.count);
                        slot->count   = static_cast<i8>(slot->count + moved);
                        carried.count = static_cast<i8>(carried.count - moved);
                        if (carried.count <= 0) {
                            carried = {};
                        }
                    }
                } else if (carried.empty()) {
                    carried = *slot;
                    *slot   = {};
                } else if (fits) {
                    if (slot->empty() && carried.count > limit) {
                        *slot         = carried;
                        slot->count   = limit;
                        carried.count = static_cast<i8>(carried.count - limit);
                    } else if (carried.count <= limit) {
                        std::swap(*slot, carried);
                    }
                }
            } else if (click.button == 1) {
                if (carried.empty()) {
                    if (!slot->empty()) {
                        const i8 half = static_cast<i8>((slot->count + 1) / 2);
                        carried       = *slot;
                        carried.count = half;
                        slot->count   = static_cast<i8>(slot->count - half);
                        if (slot->count <= 0) {
                            *slot = {};
                        }
                    }
                } else if (fits && (slot->empty() || (same_kind(*slot, carried) &&
                                                      slot->count < limit))) {
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
    }
    // Drags (5) and double-clicks (6) are not carried out: the resend below
    // puts the client back where the server is.

    refresh_enchant_window(context, host, window);
    resend(host, window, inventory, carried, click.state_id + 1);
    send_properties(host, window, current_seed(host));
}

void enchant_button(const EnchantContext& context, const EnchantHost& host, EnchantWindow& window,
                    i32 button, std::span<const net::ItemStack> inventory,
                    const net::ItemStack& carried) {
    if (window.kind != EnchantScreen::Table || button < 0 || button > 2) {
        return;
    }
    net::ItemStack& item  = window.slots[0];
    net::ItemStack& lapis = window.slots[1];
    const i32       need  = button + 1;
    const i32       cost  = window.offers.costs[static_cast<usize>(button)];
    const i32       level = host.level ? host.level() : 0;
    const bool      free  = creative(host);
    if ((lapis.empty() || lapis.count < need) && !free) {
        return;
    }
    if (cost <= 0 || item.empty() || ((level < need || level < cost) && !free)) {
        return;
    }
    const std::string_view name = name_of(context, item.item_id);
    const gameplay::EnchantmentList list =
        gameplay::table_enchantments(current_seed(host), button, cost, name);
    if (list.empty()) {
        return;
    }
    // Levels: the button's index plus one, not the cost shown. Then a new seed
    // from the player's own generator.
    if (host.take_levels && !free) {
        host.take_levels(need);
    }
    if (host.set_xp_seed && host.random != nullptr) {
        host.set_xp_seed(host.random->next_int());
    }
    nbt::Tag   tag  = tag_of(item);
    const bool book = name == "minecraft:book";
    if (book) {
        if (const auto enchanted_book = id_of(context, "minecraft:enchanted_book")) {
            item.item_id = *enchanted_book;
        }
    }
    gameplay::EnchantmentList current = gameplay::read_enchantments(tag, book);
    for (const gameplay::EnchantmentLevel& e : list) {
        if (book) {
            // A book keeps one entry per enchantment, at the higher level.
            current.set(e.enchantment, std::max(current.level(e.enchantment), e.level));
        } else {
            current.push(e);
        }
    }
    gameplay::write_enchantments(tag, current, book);
    set_tag(item, tag);
    if (!free) {
        lapis.count = static_cast<i8>(lapis.count - need);
        if (lapis.count <= 0) {
            lapis = {};
        }
    }
    refresh_enchant_window(context, host, window);
    resend(host, window, inventory, carried, 0);
    send_properties(host, window, current_seed(host));
}

void enchant_rename(const EnchantContext& context, const EnchantHost& host, EnchantWindow& window,
                    std::string_view name, std::span<const net::ItemStack> inventory,
                    const net::ItemStack& carried) {
    if (window.kind != EnchantScreen::Anvil) {
        return;
    }
    // Longer than fifty characters is ignored, as vanilla does — counted in
    // UTF-16 units there, in code points here, which agree below U+10000.
    usize code_points = 0;
    for (const char c : name) {
        code_points += (static_cast<unsigned char>(c) & 0xC0U) != 0x80U ? 1 : 0;
    }
    if (code_points > 50) {
        return;
    }
    window.rename = std::string{name};
    refresh_enchant_window(context, host, window);
    resend(host, window, inventory, carried, 0);
    send_properties(host, window, current_seed(host));
}

void close_enchant_screen(const EnchantContext& context, const EnchantHost& host,
                          EnchantWindow& window, std::span<net::ItemStack> inventory) {
    // The inputs are the player's; the output is only a preview of them.
    for (usize i = 0; i < 2; ++i) {
        give_back(context, host, inventory, window.slots[i]);
        window.slots[i] = {};
    }
    window.slots[2] = {};
}

std::optional<std::pair<u8, i32>> parse_click_button(std::span<const u8> payload) {
    if (payload.size() != 2) {
        return std::nullopt;
    }
    return std::pair<u8, i32>{payload[0], static_cast<i32>(static_cast<i8>(payload[1]))};
}

std::optional<std::string> parse_rename_item(std::span<const u8> payload) {
    u32   length = 0;
    usize i      = 0;
    for (int shift = 0; shift < 35; shift += 7) {
        if (i >= payload.size()) {
            return std::nullopt;
        }
        const u8 byte = payload[i++];
        length |= static_cast<u32>(byte & 0x7FU) << static_cast<u32>(shift);
        if ((byte & 0x80U) == 0) {
            break;
        }
    }
    if (length > 32767U * 4U || i + length != payload.size()) {
        return std::nullopt;
    }
    return std::string{reinterpret_cast<const char*>(payload.data() + i), length};
}

void take_levels(SurvivalSession& survival, i32 levels) {
    if (levels <= 0) {
        return;
    }
    const i32 old_cost = gameplay::experience_to_next_level(survival.experience_level, survival.curve);
    const f32 fraction = old_cost > 0 ? static_cast<f32>(survival.experience_points) /
                                            static_cast<f32>(old_cost)
                                      : 0.0F;
    survival.experience_level -= levels;
    if (survival.experience_level < 0) {
        survival.experience_level  = 0;
        survival.experience_points = 0;
        survival.experience_total  = 0;
        return;
    }
    const i32 new_cost = gameplay::experience_to_next_level(survival.experience_level, survival.curve);
    survival.experience_points = static_cast<i32>(fraction * static_cast<f32>(new_cost));
}

// ── Effects ─────────────────────────────────────────────────────────────────

std::array<gameplay::EnchantmentList, 4> worn_enchantments(std::span<const net::ItemStack> inventory) {
    std::array<gameplay::EnchantmentList, 4> out{};
    for (usize i = 0; i < 4 && 5 + i < inventory.size(); ++i) {
        const net::ItemStack& piece = inventory[5 + i];
        if (!piece.empty() && !piece.nbt.empty()) {
            out[i] = gameplay::read_enchantments(tag_of(piece), false);
        }
    }
    return out;
}

f32 after_worn_protection(std::span<const net::ItemStack> inventory, gameplay::DamageKind kind,
                          f32 amount) {
    const auto worn = worn_enchantments(inventory);
    return gameplay::after_protection(amount, gameplay::total_epf(worn, kind));
}

i32 stack_enchantment(const net::ItemStack& stack, gameplay::Enchantment which) {
    if (stack.empty() || stack.nbt.empty()) {
        return 0;
    }
    return gameplay::read_enchantments(tag_of(stack), false).level(which);
}

i32 apply_mending(std::span<net::ItemStack> inventory, i16 held_slot, i32 value,
                  math::LegacyRandomSource& random, const std::function<void(usize)>& changed) {
    // Main hand, off hand, then the four worn pieces: the equipment slots a
    // Mending item may sit in.
    std::array<usize, 6> slots{36 + static_cast<usize>(std::clamp<i16>(held_slot, 0, 8)), 45, 5, 6,
                               7, 8};
    while (value > 0) {
        std::array<usize, 6> candidates{};
        usize                n = 0;
        for (const usize slot : slots) {
            if (slot >= inventory.size() || inventory[slot].empty()) {
                continue;
            }
            const nbt::Tag tag = tag_of(inventory[slot]);
            const nbt::Tag* damage = tag.find("Damage");
            if (damage != nullptr && damage->as_i64() > 0 &&
                gameplay::read_enchantments(tag, false).level(gameplay::Enchantment::Mending) > 0) {
                candidates[n++] = slot;
            }
        }
        if (n == 0) {
            break;
        }
        const usize slot = candidates[static_cast<usize>(random.next_int(static_cast<i32>(n)))];
        nbt::Tag    tag  = tag_of(inventory[slot]);
        const auto  have = static_cast<i32>(tag.find("Damage")->as_i64());
        const gameplay::MendingOutcome outcome = gameplay::mend(value, have);
        (void)tag.put("Damage", nbt::Tag{have - outcome.repaired});
        set_tag(inventory[slot], tag);
        if (changed) {
            changed(slot);
        }
        value = outcome.left;
    }
    return value;
}

f32 target_enchantment_bonus(const net::ItemStack& held, std::string_view entity_type) {
    if (held.empty() || held.nbt.empty()) {
        return 0.0F;
    }
    const gameplay::EnchantmentList all = gameplay::read_enchantments(tag_of(held), false);
    gameplay::EnchantmentList       only;
    for (const gameplay::EnchantmentLevel& e : all) {
        if (e.enchantment == gameplay::Enchantment::Smite ||
            e.enchantment == gameplay::Enchantment::BaneOfArthropods ||
            e.enchantment == gameplay::Enchantment::Impaling) {
            only.push(e);
        }
    }
    return gameplay::damage_bonus(only, gameplay::mob_group(entity_type));
}

bool vanishes_on_death(const net::ItemStack& stack) {
    if (stack.empty() || stack.nbt.empty()) {
        return false;
    }
    return gameplay::read_enchantments(tag_of(stack), false).level(
               gameplay::Enchantment::VanishingCurse) > 0;
}

}  // namespace ov::server
