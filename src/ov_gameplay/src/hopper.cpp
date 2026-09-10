#include "ov/gameplay/hopper.hpp"

#include <algorithm>

namespace ov::gameplay {
namespace {

/// How much more of `stack` a slot holding `held` could take.
[[nodiscard]] i32 room_in(const ItemContainer& into, const SlotStack& held,
                          const SlotStack& stack) {
    if (held.empty()) {
        return into.max_stack(stack.item);
    }
    if (!held.merges_with(stack)) {
        return 0;
    }
    return std::max(0, into.max_stack(stack.item) - held.count);
}

}  // namespace

i32 HopperRules::slot_for(const ItemContainer& into, const SlotStack& stack, Direction face) {
    if (stack.empty()) {
        return -1;
    }
    // Merging beats filling, and the two passes are separate on purpose.
    // Vanilla scans once for a stack to top up and only then for an empty slot,
    // so a chest with one half-full stack of cobble and four empty slots keeps
    // its cobble in one place. Doing it in a single pass spreads a stack over
    // the container and looks right until a comparator reads it.
    const i32 count = into.slot_count();
    for (i32 index = 0; index < count; ++index) {
        const SlotStack held = into.slot(index);
        if (!held.merges_with(stack)) {
            continue;
        }
        if (room_in(into, held, stack) > 0 && into.can_place_into(index, stack, face)) {
            return index;
        }
    }
    for (i32 index = 0; index < count; ++index) {
        if (into.slot(index).empty() && into.can_place_into(index, stack, face)) {
            return index;
        }
    }
    return -1;
}

i32 HopperRules::insert(ItemContainer& into, SlotStack& stack, Direction face) {
    i32 moved = 0;
    while (!stack.empty()) {
        const i32 index = slot_for(into, stack, face);
        if (index < 0) {
            break;
        }
        const SlotStack held = into.slot(index);
        const i32       room = room_in(into, held, stack);
        if (room <= 0) {
            break;
        }
        const i32 take = std::min(room, stack.count);
        SlotStack put  = held.empty() ? SlotStack{stack.item, take, stack.tag} : held;
        if (!held.empty()) {
            put.count += take;
        }
        into.set_slot(index, put);
        stack.count -= take;
        moved += take;
    }
    if (stack.count <= 0) {
        // A stack that ran out is an empty stack, not a stack of zero cobble:
        // leaving the item id behind makes `merges_with` true for something
        // that is not there.
        stack = SlotStack{};
    }
    return moved;
}

bool HopperRules::push_one(ItemContainer& from, ItemContainer& into, Direction face) {
    const i32 count = from.slot_count();
    for (i32 index = 0; index < count; ++index) {
        const SlotStack held = from.slot(index);
        if (held.empty()) {
            continue;
        }
        // One item, never the stack. A hopper that moved a whole stack would
        // empty a chest in a tick and every filter ever built would stop
        // working, because a filter is a race between two hoppers at one item
        // each.
        SlotStack one{held.item, 1, held.tag};
        if (insert(into, one, face) == 0) {
            continue;
        }
        SlotStack left = held;
        left.count -= 1;
        from.set_slot(index, left.count > 0 ? left : SlotStack{});
        return true;
    }
    return false;
}

bool HopperRules::pull_one(ItemContainer& hopper, ItemContainer& above) {
    const i32 count = above.slot_count();
    for (i32 index = 0; index < count; ++index) {
        const SlotStack held = above.slot(index);
        // A hopper reaching up presents the container's **bottom** face. That
        // is what makes it take a furnace's smelted output and not its fuel.
        if (held.empty() || !above.can_take_from(index, Direction::Down)) {
            continue;
        }
        SlotStack one{held.item, 1, held.tag};
        if (insert(hopper, one, Direction::Up) == 0) {
            continue;
        }
        SlotStack left = held;
        left.count -= 1;
        above.set_slot(index, left.count > 0 ? left : SlotStack{});
        return true;
    }
    return false;
}

HopperRules::TickResult HopperRules::tick(ItemContainer& hopper, ItemContainer* target,
                                          Direction facing, ItemContainer* source) {
    TickResult result;
    // Push before pull, which is vanilla's order and is observable: a hopper
    // that pulled first would move an item all the way through itself in one
    // tick, and a chain of five hoppers would deliver in one tick rather than
    // in five times the cooldown.
    if (target != nullptr) {
        result.pushed = push_one(hopper, *target, opposite(facing));
    }
    if (source != nullptr) {
        result.pulled = pull_one(hopper, *source);
    }
    return result;
}

}  // namespace ov::gameplay
