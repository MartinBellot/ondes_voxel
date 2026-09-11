// Right-clicking: what the block does, and what the item does when it doesn't.
//
// One packet — Use Item On — carries a door being opened, a bucket being
// emptied, a hoe tilling dirt and a block being placed, and the server decides
// between them in a fixed order:
//
//   1. Unless the player is sneaking *with something in hand*, the **block**
//      gets first refusal. A chest clicked with a diamond in hand opens; the
//      same chest clicked while sneaking takes the diamond's place instead.
//   2. If the block passed, the **item** acts.
//
// Getting that order backwards is not a subtle bug: it makes every container in
// the game impossible to open while holding anything, and makes it impossible
// to place a block against a door.
//
// What each interaction *does* was measured rather than recalled. A bot on a
// real 1.20.1 server was made to click each of thirty-eight arrangements once,
// and the resulting block states were read off the **Block Update packets** the
// server sent — the state id itself, not a guess checked against a list. The
// obvious readback, `/data get block`, answers only for blocks that have a
// block entity and reports "no block entity" for farmland, a lit candle and
// every other case that matters here.
//
// Layer 9: everything takes a `LevelWriter&`, never a server. A door that
// needed a `ServerLevel` could not be predicted by a client, and prediction is
// the whole reason this interface exists.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/level.hpp"

#include <optional>
#include <string_view>

namespace ov::gameplay {

class FireRules;  // ── fire ── fire.hpp

/// How an interaction ended. Vanilla's own four, and they are four rather than
/// two because the animation and the cooldown follow from which one it was.
enum class UseResult : u8 {
    /// Nothing here reacted. The caller moves on to the next rule — which for
    /// a block means "now let the item try".
    Pass,
    /// It worked. Swing the arm.
    Success,
    /// It worked, but do not swing — eating, drinking, filling a bucket.
    Consume,
    /// It was refused. Not the same as Pass: a Fail stops the chain, which is
    /// what keeps an iron door from being placed through.
    Fail,
};

/// A screen only the caller can open.
///
/// Containers live above this layer — they need a window id, a slot list and a
/// player — so a block whose interaction *is* a screen names the screen and
/// stops. That is the honest boundary: this module knows a barrel opens a
/// nine-by-three, and knows it cannot open one.
enum class ScreenKind : u8 {
    None,
    Chest,
    TrappedChest,
    EnderChest,
    Barrel,
    ShulkerBox,
    Hopper,
    Dropper,
    Dispenser,
    Furnace,
    BlastFurnace,
    Smoker,
    CraftingTable,
    Anvil,
    EnchantingTable,
    Beacon,
    BrewingStand,
    Grindstone,
    Loom,
    CartographyTable,
    Stonecutter,
    SmithingTable,
    Lectern,
};

/// What the caller is holding and how it is standing.
struct UseContext {
    /// The block that was clicked.
    BlockPos position;

    /// 0..5 as the protocol orders them: -Y, +Y, -Z, +Z, -X, +X.
    i32 face{1};

    /// Where on the face, 0..1 on each axis.
    f32 cursor_x{0.5F};
    f32 cursor_y{0.5F};
    f32 cursor_z{0.5F};

    /// The held item's registry name, empty for a bare hand.
    std::string_view item;

    /// The client's own sneak flag. It is what decides rule 1 above.
    bool sneaking{false};

    /// Whether the held stack is the offhand's. Only used to keep an offhand
    /// use from consuming the main hand.
    bool off_hand{false};
};

/// Everything one interaction asks the caller to do.
struct UseOutcome {
    UseResult result{UseResult::Pass};

    /// Durability the held item should lose.
    i32 item_damage{0};

    /// The held stack shrinks by one — bone meal, a bucket emptied.
    bool consume_one{false};

    /// The held item turns into this. A bucket filled from water becomes a
    /// water bucket; emptied, it becomes a bucket. Empty means unchanged.
    std::string_view replace_with{};

    /// A screen the caller must open, and where.
    ScreenKind screen{ScreenKind::None};
    BlockPos   screen_position{};

    /// A primed TNT entity the caller must spawn. Entities are layer 8 and this
    /// is layer 9, but spawning one needs a world this module has no reference
    /// to, so it is named and handed back instead.
    bool     spawn_primed_tnt{false};
    BlockPos tnt_position{};

    /// Something recognised and not finished. Never silent: a bone meal on a
    /// sapling needs a tree to be placed, which is worldgen and is above this
    /// layer, so it says so rather than doing nothing and looking correct.
    std::string_view unsupported{};
};

/// The interaction rules, with the registry lookups they need resolved once.
class ItemUse {
public:
    ItemUse(const registry::BlockRegistry& blocks, const registry::Registries& registries);

    /// The whole rule, in the game's own order: the block first unless the
    /// player is sneaking with something in hand, then the item.
    [[nodiscard]] UseOutcome use_on(world::LevelWriter& level, const UseContext& context) const;

    /// The block half alone. Pass when this block has no interaction.
    [[nodiscard]] UseOutcome interact_block(world::LevelWriter& level,
                                            const UseContext&   context) const;

    /// The item half alone. Pass when the item does nothing to a block.
    [[nodiscard]] UseOutcome use_item_on(world::LevelWriter& level,
                                         const UseContext&   context) const;

    /// The block one step along `context.face` from the clicked one.
    [[nodiscard]] static BlockPos offset_by_face(BlockPos position, i32 face) noexcept;

    // ── fire ──
    /// The rules a flint and steel's fire obeys: `FireRules::placement` decides
    /// whether a fire may stand in the cell and in which shape (soul fire over
    /// soul soil, face flags against flammable walls). Not owned; null keeps
    /// the older rule — fire[age=0] into any empty cell.
    void set_fire_rules(const FireRules* fire) noexcept { fire_ = fire; }

private:
    [[nodiscard]] std::string_view name_of(registry::BlockStateId state) const noexcept;
    [[nodiscard]] std::optional<registry::BlockStateId> toggled(registry::BlockStateId state,
                                                                std::string_view property) const;
    [[nodiscard]] std::optional<registry::BlockStateId> with(registry::BlockStateId state,
                                                             std::string_view       property,
                                                             std::string_view       value) const;
    [[nodiscard]] std::string_view value_of(registry::BlockStateId state,
                                            std::string_view       property) const;
    [[nodiscard]] std::optional<registry::BlockStateId> default_of(std::string_view name) const;

    const registry::BlockRegistry* blocks_{nullptr};
    const FireRules*               fire_{nullptr};  // ── fire ──
};

// ── Using an item on nothing in particular ──────────────────────────────────

/// How long the hand is held up before an item's use finishes.
///
/// Thirty-two ticks for nearly every food, and the exceptions are what make this
/// a function rather than a constant: dried kelp takes 16 and a honey bottle 40.
/// The 32 is the food table's own measured `default_use_duration`, so a food
/// added to that table is automatically right here too.
///
/// **The exceptions are not measured.** The 16, the 40, and the 32 given to a
/// potion and to milk are carried over rather than timed; the campaign that
/// would time them sends Use Item and watches which tick `foodLevel` moves on.
/// They are named one per line in the implementation for that reason.
[[nodiscard]] i32 use_duration_ticks(std::string_view item) noexcept;

/// The state of a use in progress.
///
/// Eating in Minecraft is not instantaneous and is not a timer either: it is a
/// count that goes *down* every tick the button is still held, and releasing
/// the button abandons it with nothing gained. Both halves matter — a player
/// who lets go at tick 31 has eaten nothing at all.
struct UseInProgress {
    /// Ticks left. Zero when nothing is being used.
    i32 remaining{0};

    /// What is being used. Empty when nothing is.
    std::string_view item;

    [[nodiscard]] bool active() const noexcept { return remaining > 0; }
};

/// Begin using an item. Returns false when the item is not usable that way —
/// or, for food, when the player is not hungry enough to eat it.
[[nodiscard]] bool begin_use(UseInProgress& use, std::string_view item, i32 food,
                             i32 max_food) noexcept;

/// One tick of a use in progress. True on the tick it completes.
///
/// The caller applies the effect: this module says *when*, not *what*, because
/// what a finished use does — nutrition, a potion effect, a bucket of milk
/// clearing effects — reaches into three other systems.
[[nodiscard]] bool tick_use(UseInProgress& use) noexcept;

/// Give up on a use in progress.
void cancel_use(UseInProgress& use) noexcept;

}  // namespace ov::gameplay
