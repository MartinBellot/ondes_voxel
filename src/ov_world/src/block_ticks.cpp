#include "ov/world/block_ticks.hpp"

#include <algorithm>

namespace ov::world {

bool ScheduledTick::runs_before(const ScheduledTick& other) const noexcept {
    if (when != other.when) {
        return when < other.when;
    }
    if (priority != other.priority) {
        // Lower priority value runs earlier: vanilla's ExtremelyHigh is -3.
        return static_cast<i8>(priority) < static_cast<i8>(other.priority);
    }
    return sequence < other.sequence;
}

void BlockTickScheduler::schedule(BlockPos pos, std::string_view what, i64 delay, i64 now,
                                  TickPriority priority) {
    if (is_scheduled(pos, what)) {
        return;
    }
    // A negative delay would mean "already due", which the drain would then run
    // inside the tick that scheduled it. Vanilla clamps rather than refuses.
    const i64 when = now + (delay < 0 ? 0 : delay);
    pending_.push_back(ScheduledTick{pos, std::string{what}, when, priority, next_sequence_++});
}

bool BlockTickScheduler::is_scheduled(BlockPos pos, std::string_view what) const noexcept {
    return std::any_of(pending_.begin(), pending_.end(), [&](const ScheduledTick& tick) {
        return tick.pos == pos && tick.what == what;
    });
}

void BlockTickScheduler::adopt(std::vector<ScheduledTick> ticks) {
    for (ScheduledTick& tick : ticks) {
        if (is_scheduled(tick.pos, tick.what)) {
            continue;
        }
        tick.sequence = next_sequence_++;
        pending_.push_back(std::move(tick));
    }
}

void BlockTickScheduler::collect_due(i64 now, std::vector<ScheduledTick>& out) {
    out.clear();

    // Partition rather than erase-per-element: the due ticks move to `out` and
    // the rest keep their order, so this is one pass and one shuffle instead of
    // a quadratic sequence of vector erases.
    auto first_kept = std::stable_partition(
        pending_.begin(), pending_.end(),
        [now](const ScheduledTick& tick) { return tick.when <= now; });

    out.assign(std::make_move_iterator(pending_.begin()), std::make_move_iterator(first_kept));
    pending_.erase(pending_.begin(), first_kept);

    std::sort(out.begin(), out.end(),
              [](const ScheduledTick& a, const ScheduledTick& b) { return a.runs_before(b); });
}

std::vector<ScheduledTick> BlockTickScheduler::snapshot() const {
    std::vector<ScheduledTick> copy = pending_;
    std::sort(copy.begin(), copy.end(),
              [](const ScheduledTick& a, const ScheduledTick& b) { return a.runs_before(b); });
    return copy;
}

void BlockTickScheduler::forget_chunk(i32 chunk_x, i32 chunk_z) {
    std::erase_if(pending_, [chunk_x, chunk_z](const ScheduledTick& tick) {
        return floor_div(tick.pos.x, kSectionSize) == chunk_x &&
               floor_div(tick.pos.z, kSectionSize) == chunk_z;
    });
}

void BlockTickScheduler::clear() noexcept {
    pending_.clear();
    // The sequence counter is deliberately *not* reset: it only ever has to be
    // increasing, and restarting it after a partial clear would let a new tick
    // sort ahead of an old one that is still queued.
}

nbt::Tag ticks_to_nbt(const std::vector<ScheduledTick>& ticks, i32 chunk_x, i32 chunk_z, i64 now) {
    nbt::Tag list = nbt::Tag::make_list(nbt::TagType::Compound);
    for (const ScheduledTick& tick : ticks) {
        if (floor_div(tick.pos.x, kSectionSize) != chunk_x ||
            floor_div(tick.pos.z, kSectionSize) != chunk_z) {
            continue;
        }
        nbt::Tag entry = nbt::Tag::make_compound();
        entry.put("i", nbt::Tag{tick.what});
        entry.put("p", nbt::Tag{static_cast<i32>(tick.priority)});
        // Relative to the chunk's game time, which is what the format stores.
        entry.put("t", nbt::Tag{static_cast<i32>(tick.when - now)});
        entry.put("x", nbt::Tag{tick.pos.x});
        entry.put("y", nbt::Tag{tick.pos.y});
        entry.put("z", nbt::Tag{tick.pos.z});
        list.push(std::move(entry));
    }
    return list;
}

bool ticks_from_nbt(const nbt::Tag& list, i64 now, std::vector<ScheduledTick>& out) {
    const std::vector<nbt::Tag>* entries = list.list();
    if (entries == nullptr) {
        return false;
    }
    // An empty list is written by vanilla with element type End, so the element
    // type is only checked when there is something to read.
    if (!entries->empty() && list.list_element_type() != nbt::TagType::Compound) {
        return false;
    }

    for (const nbt::Tag& entry : *entries) {
        const nbt::Tag* name = entry.find("i");
        const nbt::Tag* x    = entry.find("x");
        const nbt::Tag* y    = entry.find("y");
        const nbt::Tag* z    = entry.find("z");
        const nbt::Tag* t    = entry.find("t");
        if (name == nullptr || x == nullptr || y == nullptr || z == nullptr || t == nullptr) {
            return false;
        }
        if (name->type() != nbt::TagType::String) {
            return false;
        }

        // `p` is genuinely optional: vanilla omits it on chunks whose ticks are
        // all at normal priority in some versions, and a missing field there
        // means Normal rather than "unreadable".
        const nbt::Tag* p        = entry.find("p");
        const i64       priority = p == nullptr ? 0 : p->as_i64(0);
        if (priority < -3 || priority > 3) {
            return false;
        }

        out.push_back(ScheduledTick{
            BlockPos{static_cast<i32>(x->as_i64()), static_cast<i32>(y->as_i64()),
                     static_cast<i32>(z->as_i64())},
            std::string{name->as_string()}, now + t->as_i64(),
            static_cast<TickPriority>(priority), 0});
    }
    return true;
}

}  // namespace ov::world
