// The furnace: what burns, how long, and what comes out.
//
// A furnace is three slots and four counters, and the counters are the whole
// mechanism. `Set Container Property` sends them to the client every tick the
// screen is open, which is why the two fire bars and the arrow move at all —
// the client draws them from these numbers and computes nothing.
//
// Two things here are code rather than data in vanilla, so both were measured
// on a real 1.20.1 server rather than copied:
//
//   * how long one unit of each item burns, per furnace kind. The blast
//     furnace is not simply half the furnace: integer division turns a dried
//     kelp block's 4001 into 2000, and a divisor applied here would round the
//     other way for exactly the items where it shows.
//   * the tick on which a furnace lights. Fuel is taken and `BurnTime` set on
//     the first block-entity tick after the furnace has both fuel and
//     something to cook, and that value is **not** decremented on that same
//     tick. One tick out here and every fuel in the game burns wrong.
//
// See docs/provenance/crafting-and-smelting.md.
#pragma once

#include "ov/gameplay/recipe.hpp"

#include <optional>
#include <vector>

namespace ov::gameplay {

/// Which furnace. The three differ in what they accept, how fast they cook and
/// how long a given fuel lasts in them.
enum class FurnaceKind : u8 { Furnace, BlastFurnace, Smoker };

/// The recipe kind each furnace consults.
[[nodiscard]] registry::RecipeKind cooking_kind(FurnaceKind kind) noexcept;

/// The three slots, in the order the window numbers them.
struct FurnaceSlots {
    RecipeStack input{};
    RecipeStack fuel{};
    RecipeStack output{};
};

/// The four counters the client draws its bars from, plus the experience the
/// furnace is holding for whoever empties it.
struct FurnaceState {
    /// Ticks of fuel left. The block is `lit` exactly while this is above zero.
    i32 lit_time{0};
    /// What `lit_time` started at, so the flame can be drawn as a fraction.
    /// Not stored by vanilla's NBT, which recomputes it from `lit_time` on
    /// load — so a furnace reloaded mid-burn shows a full flame in vanilla too.
    i32 lit_duration{0};
    i32 cook_time{0};
    i32 cook_total{0};

    /// Experience earned and not yet collected.
    ///
    /// Vanilla keeps a count per recipe and awards it when the output is
    /// taken, so that a furnace left running hands over everything at once.
    /// Held as a float because a single iron ingot is worth 0.7.
    f32 stored_experience{0.0F};

    [[nodiscard]] bool lit() const noexcept { return lit_time > 0; }
};

/// What one tick did, so a caller knows what to send and what to save.
struct FurnaceTick {
    /// The lit state changed, so the block state has to change with it.
    bool lit_changed{false};
    /// Any slot changed.
    bool slots_changed{false};
    /// One item finished cooking this tick.
    bool produced{false};
};

/// Advance a furnace by one tick.
///
/// The order inside matters and is the order the measurement pinned down:
/// burn down first, then light if dark, then cook. Lighting after burning down
/// is what makes a fuel last exactly its measured number of ticks rather than
/// one fewer.
///
/// `cook_total` is **not** read again here: an item finishes when `cook_time`
/// reaches it exactly, and it is recomputed only after an item and by
/// `furnace_input_changed`. Measured: a furnace placed without a total counts
/// `CookTime` up to 1202 and never finishes anything.
[[nodiscard]] FurnaceTick furnace_tick(const RecipeBook& book, FurnaceKind kind,
                                       FurnaceSlots& slots, FurnaceState& state);

/// The input slot now holds something else than it did — another item, other
/// tags, or nothing — because someone other than the furnace put it there.
/// The progress is lost and the total is the new input's recipe's, as vanilla
/// does when its input slot is set.
void furnace_input_changed(const RecipeBook& book, FurnaceKind kind, const FurnaceSlots& slots,
                           FurnaceState& state);

/// How long one of this item keeps that furnace alight, in ticks. Zero when it
/// is not a fuel.
[[nodiscard]] i32 burn_ticks(const RecipeBook& book, FurnaceKind kind,
                             registry::ProtocolId item) noexcept;

/// The recipe this furnace would use for this input, if any.
[[nodiscard]] std::optional<RecipeIndex> match_cooking(const RecipeBook& book, FurnaceKind kind,
                                                       registry::ProtocolId input);

/// The recipe a stonecutter would use for this input and this chosen result.
///
/// A stonecutter offers several results for one input — stone gives stairs,
/// slabs, bricks and more — so the client picks one by index into the offered
/// list. That list is `stonecutting_options`.
[[nodiscard]] std::vector<RecipeIndex> stonecutting_options(const RecipeBook&    book,
                                                            registry::ProtocolId input);

/// The smithing recipe that transforms these three, if any.
[[nodiscard]] std::optional<RecipeIndex> match_smithing(const RecipeBook&    book,
                                                        registry::ProtocolId tmpl,
                                                        registry::ProtocolId base,
                                                        registry::ProtocolId addition);

}  // namespace ov::gameplay
