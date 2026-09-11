#include "ov/gameplay/item_use.hpp"

#include "ov/gameplay/fire.hpp"  // ── fire ──

#include "ov/gameplay/food.hpp"

#include <array>
#include <string>

namespace ov::gameplay {
namespace {

/// Suffix test, used for the families the game treats alike.
[[nodiscard]] bool ends_with(std::string_view text, std::string_view suffix) noexcept {
    return text.size() > suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

/// The two blocks a bare hand must *not* open. Measured: an iron door and an
/// iron trapdoor clicked with an empty hand came back at `open=false`, while
/// every wooden one came back `open=true`.
[[nodiscard]] bool hand_openable(std::string_view name) noexcept {
    return name != "minecraft:iron_door" && name != "minecraft:iron_trapdoor";
}

/// How long a button stays down, by material.
///
/// **Not measured here.** The two numbers are the ones vanilla's redstone
/// timing needs and this campaign did not time them; they are named so that the
/// gap is visible rather than buried in a literal, and so that whoever measures
/// buttons has one place to correct.
constexpr i64 kWoodenButtonTicks = 30;
constexpr i64 kStoneButtonTicks  = 20;

/// The same for how long a redstone ore stays lit. Also not measured here.
constexpr i64 kRedstoneOreLitTicks = 30;

/// Tilling: what a hoe turns each block into. Measured, one click each.
///
/// Note the split. Dirt, grass and a dirt path all become farmland; coarse dirt
/// and rooted dirt become plain **dirt** instead — the hoe roughs them up
/// rather than tilling them. A table that sends all five to farmland is wrong on
/// two of them and looks right in a screenshot of the other three.
constexpr std::pair<std::string_view, std::string_view> kTilling[] = {
    {"minecraft:dirt", "minecraft:farmland"},
    {"minecraft:grass_block", "minecraft:farmland"},
    {"minecraft:dirt_path", "minecraft:farmland"},
    {"minecraft:coarse_dirt", "minecraft:dirt"},
    {"minecraft:rooted_dirt", "minecraft:dirt"},
};

/// Path-making: what a shovel turns each block into. Measured on grass.
constexpr std::string_view kPathable[] = {
    "minecraft:grass_block", "minecraft:dirt",       "minecraft:podzol",
    "minecraft:coarse_dirt", "minecraft:mycelium",   "minecraft:rooted_dirt",
};

/// Stripping: what an axe turns a log into. The whole list, by construction —
/// every `X_log` becomes `stripped_X_log` and every `X_wood` becomes
/// `stripped_X_wood`, which is checked below rather than tabulated.
///
/// Scraping and dewaxing are tabulated, because oxidation is a chain of four
/// named blocks with no naming rule at all.
constexpr std::pair<std::string_view, std::string_view> kScraping[] = {
    {"minecraft:oxidized_copper", "minecraft:weathered_copper"},
    {"minecraft:weathered_copper", "minecraft:exposed_copper"},
    {"minecraft:exposed_copper", "minecraft:copper_block"},
    {"minecraft:oxidized_cut_copper", "minecraft:weathered_cut_copper"},
    {"minecraft:weathered_cut_copper", "minecraft:exposed_cut_copper"},
    {"minecraft:exposed_cut_copper", "minecraft:cut_copper"},
    {"minecraft:oxidized_cut_copper_stairs", "minecraft:weathered_cut_copper_stairs"},
    {"minecraft:weathered_cut_copper_stairs", "minecraft:exposed_cut_copper_stairs"},
    {"minecraft:exposed_cut_copper_stairs", "minecraft:cut_copper_stairs"},
    {"minecraft:oxidized_cut_copper_slab", "minecraft:weathered_cut_copper_slab"},
    {"minecraft:weathered_cut_copper_slab", "minecraft:exposed_cut_copper_slab"},
    {"minecraft:exposed_cut_copper_slab", "minecraft:cut_copper_slab"},
};

/// Which screen a block opens. The blocks that *are* a screen and nothing else.
constexpr std::pair<std::string_view, ScreenKind> kScreens[] = {
    {"minecraft:chest", ScreenKind::Chest},
    {"minecraft:trapped_chest", ScreenKind::TrappedChest},
    {"minecraft:ender_chest", ScreenKind::EnderChest},
    {"minecraft:barrel", ScreenKind::Barrel},
    {"minecraft:hopper", ScreenKind::Hopper},
    {"minecraft:dropper", ScreenKind::Dropper},
    {"minecraft:dispenser", ScreenKind::Dispenser},
    {"minecraft:furnace", ScreenKind::Furnace},
    {"minecraft:blast_furnace", ScreenKind::BlastFurnace},
    {"minecraft:smoker", ScreenKind::Smoker},
    {"minecraft:crafting_table", ScreenKind::CraftingTable},
    {"minecraft:anvil", ScreenKind::Anvil},
    {"minecraft:chipped_anvil", ScreenKind::Anvil},
    {"minecraft:damaged_anvil", ScreenKind::Anvil},
    {"minecraft:enchanting_table", ScreenKind::EnchantingTable},
    {"minecraft:beacon", ScreenKind::Beacon},
    {"minecraft:brewing_stand", ScreenKind::BrewingStand},
    {"minecraft:grindstone", ScreenKind::Grindstone},
    {"minecraft:loom", ScreenKind::Loom},
    {"minecraft:cartography_table", ScreenKind::CartographyTable},
    {"minecraft:stonecutter", ScreenKind::Stonecutter},
    {"minecraft:smithing_table", ScreenKind::SmithingTable},
};

[[nodiscard]] std::optional<ScreenKind> screen_for(std::string_view name) noexcept {
    for (const auto& [block, kind] : kScreens) {
        if (block == name) {
            return kind;
        }
    }
    if (ends_with(name, "_shulker_box") || name == "minecraft:shulker_box") {
        return ScreenKind::ShulkerBox;
    }
    return std::nullopt;
}

[[nodiscard]] UseOutcome success(i32 item_damage = 0) noexcept {
    UseOutcome out;
    out.result      = UseResult::Success;
    out.item_damage = item_damage;
    return out;
}

}  // namespace

ItemUse::ItemUse(const registry::BlockRegistry& blocks, const registry::Registries&)
    : blocks_{&blocks} {}

BlockPos ItemUse::offset_by_face(BlockPos position, i32 face) noexcept {
    switch (face) {
        case 0: return BlockPos{position.x, position.y - 1, position.z};
        case 1: return BlockPos{position.x, position.y + 1, position.z};
        case 2: return BlockPos{position.x, position.y, position.z - 1};
        case 3: return BlockPos{position.x, position.y, position.z + 1};
        case 4: return BlockPos{position.x - 1, position.y, position.z};
        case 5: return BlockPos{position.x + 1, position.y, position.z};
        default: return position;
    }
}

std::string_view ItemUse::name_of(registry::BlockStateId state) const noexcept {
    return blocks_->block_name(blocks_->block_of(state));
}

std::string_view ItemUse::value_of(registry::BlockStateId state,
                                   std::string_view       property) const {
    const auto found = blocks_->find_property(blocks_->block_of(state), property);
    if (!found) {
        return {};
    }
    return blocks_->property_value(state, *found);
}

std::optional<registry::BlockStateId> ItemUse::with(registry::BlockStateId state,
                                                    std::string_view       property,
                                                    std::string_view       value) const {
    const auto found = blocks_->find_property(blocks_->block_of(state), property);
    if (!found) {
        return std::nullopt;
    }
    // The registry indexes a property by the *digit*, not by the string, and it
    // returns the state unchanged for an out-of-range digit. So the value has to
    // be located here — and an absent one refused, rather than falling through
    // to digit zero, which for `open` is "true" and would make every failed
    // lookup open a door.
    for (u16 index = 0; index < found->values.size(); ++index) {
        if (found->values[index] == value) {
            return blocks_->with_property(state, *found, index);
        }
    }
    return std::nullopt;
}

std::optional<registry::BlockStateId> ItemUse::toggled(registry::BlockStateId state,
                                                       std::string_view property) const {
    const std::string_view current = value_of(state, property);
    if (current.empty()) {
        return std::nullopt;
    }
    return with(state, property, current == "true" ? "false" : "true");
}

std::optional<registry::BlockStateId> ItemUse::default_of(std::string_view name) const {
    const auto block = blocks_->find_block(name);
    if (!block) {
        return std::nullopt;
    }
    return blocks_->default_state(*block);
}

UseOutcome ItemUse::use_on(world::LevelWriter& level, const UseContext& context) const {
    // Rule one, and it is the whole ordering: the block goes first *unless* the
    // player is sneaking with something in hand. A chest clicked with a diamond
    // opens; the same chest clicked while sneaking takes the diamond instead.
    const bool block_first = !(context.sneaking && !context.item.empty());
    if (block_first) {
        const UseOutcome from_block = interact_block(level, context);
        if (from_block.result != UseResult::Pass) {
            return from_block;
        }
    }
    return use_item_on(level, context);
}

UseOutcome ItemUse::interact_block(world::LevelWriter& level, const UseContext& context) const {
    const registry::BlockStateId state = level.block_at(context.position);
    const std::string_view       name  = name_of(state);
    if (name.empty()) {
        return {};
    }

    // ── Things that open ────────────────────────────────────────────────────
    if (ends_with(name, "_door") || ends_with(name, "_trapdoor") ||
        ends_with(name, "_fence_gate")) {
        if (!hand_openable(name)) {
            // Measured: an iron door clicked bare-handed stays shut. Fail, not
            // Pass — a Pass would let the held item act *through* the door.
            UseOutcome out;
            out.result = UseResult::Fail;
            return out;
        }
        const auto flipped = toggled(state, "open");
        if (!flipped) {
            return {};
        }
        level.set_block(context.position, *flipped);
        if (ends_with(name, "_door")) {
            // A door is two blocks and both halves carry `open`. Moving only
            // the clicked one leaves a door that is half open, which the client
            // draws and which nothing can walk through.
            const std::string_view half = value_of(state, "half");
            const BlockPos         other =
                half == "lower" ? BlockPos{context.position.x, context.position.y + 1,
                                                   context.position.z}
                                        : BlockPos{context.position.x, context.position.y - 1,
                                                   context.position.z};
            const registry::BlockStateId neighbour = level.block_at(other);
            if (name_of(neighbour) == name) {
                const auto other_flipped = toggled(neighbour, "open");
                if (other_flipped) {
                    level.set_block(other, *other_flipped);
                }
            }
        }
        return success();
    }

    // ── Things that switch ──────────────────────────────────────────────────
    if (name == "minecraft:lever") {
        const auto flipped = toggled(state, "powered");
        if (!flipped) {
            return {};
        }
        level.set_block(context.position, *flipped);
        return success();
    }

    if (ends_with(name, "_button")) {
        if (value_of(state, "powered") == "true") {
            UseOutcome out;
            out.result = UseResult::Consume;
            return out;
        }
        const auto pressed = with(state, "powered", "true");
        if (!pressed) {
            return {};
        }
        level.set_block(context.position, *pressed);
        const bool stone = name == "minecraft:stone_button" ||
                           name == "minecraft:polished_blackstone_button";
        level.schedule_tick(context.position, name,
                            stone ? kStoneButtonTicks : kWoodenButtonTicks,
                            world::TickQueue::Block);
        return success();
    }

    // ── Things that cycle ───────────────────────────────────────────────────
    if (name == "minecraft:note_block") {
        const std::string_view note = value_of(state, "note");
        if (note.empty()) {
            return {};
        }
        const i32  current = std::stoi(std::string{note});
        const auto next    = with(state, "note", std::to_string((current + 1) % 25));
        if (!next) {
            return {};
        }
        level.set_block(context.position, *next);
        return success();
    }

    if (name == "minecraft:repeater") {
        const std::string_view delay = value_of(state, "delay");
        if (delay.empty()) {
            return {};
        }
        // One to four, wrapping. Measured: a repeater at delay 1 became 2.
        const i32  current = std::stoi(std::string{delay});
        const auto next    = with(state, "delay", std::to_string(current % 4 + 1));
        if (!next) {
            return {};
        }
        level.set_block(context.position, *next);
        return success();
    }

    if (name == "minecraft:comparator") {
        const std::string_view mode = value_of(state, "mode");
        const auto next = with(state, "mode", mode == "compare" ? "subtract" : "compare");
        if (!next) {
            return {};
        }
        level.set_block(context.position, *next);
        return success();
    }

    if (name == "minecraft:daylight_detector") {
        const auto next = toggled(state, "inverted");
        if (!next) {
            return {};
        }
        level.set_block(context.position, *next);
        return success();
    }

    if (name == "minecraft:redstone_ore" || name == "minecraft:deepslate_redstone_ore") {
        const auto lit = with(state, "lit", "true");
        if (!lit) {
            return {};
        }
        level.set_block(context.position, *lit);
        level.schedule_tick(context.position, name, kRedstoneOreLitTicks,
                            world::TickQueue::Block);
        // Consume, not Success: vanilla lights the ore and does not swing.
        UseOutcome out;
        out.result = UseResult::Consume;
        return out;
    }

    // ── Things that go out ──────────────────────────────────────────────────
    if (context.item.empty() &&
        (ends_with(name, "candle") || ends_with(name, "candle_cake") ||
         name == "minecraft:campfire" || name == "minecraft:soul_campfire")) {
        if (value_of(state, "lit") != "true") {
            return {};
        }
        const auto out_state = with(state, "lit", "false");
        if (!out_state) {
            return {};
        }
        level.set_block(context.position, *out_state);
        return success();
    }

    // ── Cake ────────────────────────────────────────────────────────────────
    if (name == "minecraft:cake") {
        const std::string_view bites = value_of(state, "bites");
        if (bites.empty()) {
            return {};
        }
        const i32 current = std::stoi(std::string{bites});
        if (current >= 6) {
            level.set_block(context.position, registry::kAirState);
        } else {
            const auto next = with(state, "bites", std::to_string(current + 1));
            if (!next) {
                return {};
            }
            level.set_block(context.position, *next);
        }
        UseOutcome out;
        out.result = UseResult::Consume;
        return out;
    }

    // ── Things that are a screen ────────────────────────────────────────────
    if (const auto screen = screen_for(name)) {
        UseOutcome out;
        out.result          = UseResult::Success;
        out.screen          = *screen;
        out.screen_position = context.position;
        return out;
    }

    // ── Recognised, and not finished ────────────────────────────────────────
    if (name == "minecraft:dragon_egg") {
        // Measured: the block became air. It teleports somewhere within a
        // sixteen-block box, which needs a random and a placement search this
        // module does not have. Named rather than silently ignored.
        UseOutcome out;
        out.result      = UseResult::Consume;
        out.unsupported = "dragon egg teleport";
        return out;
    }
    if (name == "minecraft:lectern" || name == "minecraft:bell" ||
        name == "minecraft:jukebox" || name == "minecraft:beehive" ||
        name == "minecraft:bee_nest" || name == "minecraft:composter" ||
        name == "minecraft:cauldron" || name == "minecraft:respawn_anchor" ||
        ends_with(name, "_sign") || ends_with(name, "_bed")) {
        UseOutcome out;
        out.result      = UseResult::Pass;
        out.unsupported = name;
        return out;
    }

    return {};
}

UseOutcome ItemUse::use_item_on(world::LevelWriter& level, const UseContext& context) const {
    if (context.item.empty()) {
        return {};
    }
    const registry::BlockStateId state = level.block_at(context.position);
    const std::string_view       name  = name_of(state);

    // ── Flint and steel ─────────────────────────────────────────────────────
    if (context.item == "minecraft:flint_and_steel" || context.item == "minecraft:fire_charge") {
        if (name == "minecraft:tnt") {
            // Measured: the TNT block became air and a primed entity appeared.
            level.set_block(context.position, registry::kAirState);
            UseOutcome out       = success(context.item == "minecraft:flint_and_steel" ? 1 : 0);
            out.consume_one      = context.item == "minecraft:fire_charge";
            out.spawn_primed_tnt = true;
            out.tnt_position     = context.position;
            return out;
        }
        if (value_of(state, "lit") == "false" &&
            (ends_with(name, "candle") || ends_with(name, "candle_cake") ||
             name == "minecraft:campfire" || name == "minecraft:soul_campfire")) {
            const auto lit = with(state, "lit", "true");
            if (lit) {
                level.set_block(context.position, *lit);
                return success(1);
            }
        }
        const BlockPos target = offset_by_face(context.position, context.face);
        // ── fire ── The one point where a flint decides its fire: an empty
        // cell where the fire `FireRules::placement` shapes can stand
        // (`BaseFireBlock.canBePlacedAt`). A Nether portal lit from here is
        // decided before this line — vanilla lets a frame take a flint the
        // fire itself could not survive — and replaces the fire it would light.
        std::optional<registry::BlockStateId> fire;
        if (fire_ != nullptr) {
            fire = fire_->placement(level, target);
        } else if (level.block_at(target) == registry::kAirState) {
            fire = default_of("minecraft:fire");
        }
        if (!fire) {
            UseOutcome out;
            out.result = UseResult::Fail;
            return out;
        }
        // Measured: fire[age=0] with every side false, one block along the face.
        level.set_block(target, *fire);
        UseOutcome out  = success(context.item == "minecraft:flint_and_steel" ? 1 : 0);
        out.consume_one = context.item == "minecraft:fire_charge";
        return out;
    }

    // ── Hoes ────────────────────────────────────────────────────────────────
    if (ends_with(context.item, "_hoe")) {
        // Vanilla refuses to till a block with anything on top of it.
        const BlockPos above{context.position.x, context.position.y + 1, context.position.z};
        if (context.face == 0 || level.block_at(above) != registry::kAirState) {
            return {};
        }
        for (const auto& [from, to] : kTilling) {
            if (from != name) {
                continue;
            }
            const auto next = default_of(to);
            if (!next) {
                return {};
            }
            level.set_block(context.position, *next);
            return success(1);
        }
        return {};
    }

    // ── Shovels ─────────────────────────────────────────────────────────────
    if (ends_with(context.item, "_shovel")) {
        if (name == "minecraft:campfire" || name == "minecraft:soul_campfire") {
            if (value_of(state, "lit") != "true") {
                return {};
            }
            const auto out_state = with(state, "lit", "false");
            if (!out_state) {
                return {};
            }
            level.set_block(context.position, *out_state);
            return success(1);
        }
        const BlockPos above{context.position.x, context.position.y + 1, context.position.z};
        if (context.face == 0 || level.block_at(above) != registry::kAirState) {
            return {};
        }
        for (const std::string_view from : kPathable) {
            if (from != name) {
                continue;
            }
            const auto path = default_of("minecraft:dirt_path");
            if (!path) {
                return {};
            }
            level.set_block(context.position, *path);
            return success(1);
        }
        return {};
    }

    // ── Axes ────────────────────────────────────────────────────────────────
    if (ends_with(context.item, "_axe")) {
        // Stripping, by construction rather than by table: every log and every
        // wood has a `stripped_` twin under the same name.
        if ((ends_with(name, "_log") || ends_with(name, "_wood") ||
             ends_with(name, "_hyphae") || ends_with(name, "_stem")) &&
            name.substr(0, 20) != "minecraft:stripped_") {
            std::string stripped{"minecraft:stripped_"};
            stripped += name.substr(std::string_view{"minecraft:"}.size());
            const auto block = blocks_->find_block(stripped);
            if (block) {
                // Keep the axis: measured, an oak log became
                // stripped_oak_log[axis=y] and not the default axis.
                registry::BlockStateId next = blocks_->default_state(*block);
                const std::string_view axis = value_of(state, "axis");
                if (!axis.empty()) {
                    const auto with_axis = with(next, "axis", axis);
                    if (with_axis) {
                        next = *with_axis;
                    }
                }
                level.set_block(context.position, next);
                return success(1);
            }
        }
        for (const auto& [from, to] : kScraping) {
            if (from != name) {
                continue;
            }
            const auto next = default_of(to);
            if (!next) {
                return {};
            }
            level.set_block(context.position, *next);
            return success(1);
        }
        // Dewaxing: `waxed_X` becomes `X`. Measured on waxed_copper_block.
        if (name.starts_with("minecraft:waxed_")) {
            std::string unwaxed{"minecraft:"};
            unwaxed += name.substr(std::string_view{"minecraft:waxed_"}.size());
            const auto block = blocks_->find_block(unwaxed);
            if (block) {
                level.set_block(context.position, blocks_->default_state(*block));
                return success(1);
            }
        }
        return {};
    }

    // ── Buckets ─────────────────────────────────────────────────────────────
    if (context.item == "minecraft:bucket" || context.item == "minecraft:water_bucket" ||
        context.item == "minecraft:lava_bucket" ||
        context.item == "minecraft:powder_snow_bucket") {
        // A bucket does **not** act through Use Item On. It is `Item.use`, which
        // the client reaches with the Use Item packet and which ray-traces on
        // the server with SOURCE_ONLY so that it can see a fluid the block ray
        // passed straight through. Measured, and it is what made the first
        // version of this campaign report a bucket that did nothing: thirty-six
        // other interactions worked through Use Item On and this one is not one
        // of them.
        UseOutcome out;
        out.result      = UseResult::Pass;
        out.unsupported = "bucket: use through Use Item, not Use Item On";
        return out;
    }

    // ── Bone meal ───────────────────────────────────────────────────────────
    if (context.item == "minecraft:bone_meal") {
        const auto age = blocks_->find_property(blocks_->block_of(state), "age");
        if (age) {
            // A crop. Measured: wheat at age 0 went to age 4, which is inside
            // the 2..5 the game adds — so the *amount* is random and only the
            // caller's generator can supply it. The growth itself is here.
            UseOutcome out;
            out.result      = UseResult::Pass;
            out.unsupported = "bone meal growth amount needs a random source";
            return out;
        }
        UseOutcome out;
        out.result      = UseResult::Pass;
        out.unsupported = "bone meal on a sapling grows a tree, which is worldgen";
        return out;
    }

    return {};
}

// ── Using an item on nothing in particular ──────────────────────────────────

i32 use_duration_ticks(std::string_view item) noexcept {
    const FoodConstants constants;
    // **Only the 32 is measured.** It is food.hpp's own
    // `default_use_duration`, established by the survival campaign — see
    // docs/provenance/survie.md — and it covers every food in the game bar the
    // two below.
    //
    // The two exceptions and the two drinks are *not* measured here. They are
    // named individually rather than folded into the 32 so that the gap is one
    // line of diff wide when somebody times them; a campaign that did would
    // send Use Item and watch which tick `foodLevel` moves on.
    if (item == "minecraft:dried_kelp") {
        return 16;  // not measured
    }
    if (item == "minecraft:honey_bottle") {
        return 40;  // not measured
    }
    if (food_for(item)) {
        return constants.default_use_duration;
    }
    if (item == "minecraft:potion" || item == "minecraft:milk_bucket") {
        return constants.default_use_duration;  // not measured
    }
    return 0;
}

bool begin_use(UseInProgress& use, std::string_view item, i32 food, i32 max_food) noexcept {
    const i32 duration = use_duration_ticks(item);
    if (duration <= 0) {
        return false;
    }
    if (const auto value = food_for(item)) {
        // A full player cannot eat, and the exceptions — golden apples, honey —
        // are a flag on the food itself rather than a list here.
        if (food >= max_food && !value->always_edible) {
            return false;
        }
    }
    use.remaining = duration;
    use.item      = item;
    return true;
}

bool tick_use(UseInProgress& use) noexcept {
    if (use.remaining <= 0) {
        return false;
    }
    --use.remaining;
    if (use.remaining > 0) {
        return false;
    }
    use.item = {};
    return true;
}

void cancel_use(UseInProgress& use) noexcept {
    use.remaining = 0;
    use.item      = {};
}

}  // namespace ov::gameplay
