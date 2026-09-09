#include "ov/world/block_ticks.hpp"

#include <algorithm>
#include <string>

namespace ov::world {

usize BlockTickScheduler::KeyHash::operator()(const Key& key) const noexcept {
    // Coordinates are small and correlated, so a plain xor collides on whole
    // planes. Multiply each by a distinct odd 64-bit constant first.
    u64 h = static_cast<u64>(static_cast<u32>(key.pos.x)) * 0x9E3779B97F4A7C15ull;
    h ^= static_cast<u64>(static_cast<u32>(key.pos.y)) * 0xC2B2AE3D27D4EB4Full;
    h ^= static_cast<u64>(static_cast<u32>(key.pos.z)) * 0x165667B19E3779F9ull;
    h ^= static_cast<u64>(key.block) * 0x27D4EB2F165667C5ull;
    h ^= h >> 29;
    return static_cast<usize>(h);
}

bool BlockTickScheduler::schedule(BlockPos pos, registry::BlockId block, i64 now, i32 delay,
                                  TickPriority priority) {
    // A negative delay is a caller bug, not a request to run in the past.
    const i32 clamped = delay < 0 ? 0 : delay;
    return schedule_at(pos, block, now + clamped, priority);
}

bool BlockTickScheduler::schedule_at(BlockPos pos, registry::BlockId block, i64 trigger_tick,
                                     TickPriority priority) {
    const Key key{pos, block.value()};
    if (!pending_.insert(key).second) {
        return false;
    }
    entries_.push_back(ScheduledTick{pos, block, trigger_tick, priority, next_order_++});
    return true;
}

bool BlockTickScheduler::is_scheduled(BlockPos pos, registry::BlockId block) const noexcept {
    return pending_.contains(Key{pos, block.value()});
}

bool BlockTickScheduler::cancel(BlockPos pos, registry::BlockId block) {
    const Key key{pos, block.value()};
    if (pending_.erase(key) == 0) {
        return false;
    }
    const auto it = std::ranges::find_if(entries_, [&](const ScheduledTick& t) {
        return t.pos == pos && t.block == block;
    });
    if (it != entries_.end()) {
        *it = entries_.back();
        entries_.pop_back();
    }
    return true;
}

usize BlockTickScheduler::cancel_all_at(BlockPos pos) {
    const usize before = entries_.size();
    std::erase_if(entries_, [&](const ScheduledTick& t) {
        if (t.pos != pos) {
            return false;
        }
        pending_.erase(Key{pos, t.block.value()});
        return true;
    });
    return before - entries_.size();
}

void BlockTickScheduler::drain_due(i64 now, std::vector<ScheduledTick>& out, usize limit) {
    // Partition rather than sort the whole queue: the pending set is the whole
    // loaded world's worth of fluids, and only a handful of them are due.
    std::vector<ScheduledTick> due;
    for (const ScheduledTick& tick : entries_) {
        if (tick.trigger_tick <= now) {
            due.push_back(tick);
        }
    }
    std::ranges::sort(due, tick_order);
    if (due.size() > limit) {
        due.resize(limit);
    }

    for (const ScheduledTick& tick : due) {
        pending_.erase(Key{tick.pos, tick.block.value()});
    }
    // Remove exactly the drained ones. Matching on the order number rather than
    // on the position keeps a tick that was re-scheduled during the same drain
    // — impossible here, since nothing runs yet, but the invariant is cheap.
    std::vector<u64> drained;
    drained.reserve(due.size());
    for (const ScheduledTick& tick : due) {
        drained.push_back(tick.order);
    }
    std::ranges::sort(drained);
    std::erase_if(entries_, [&](const ScheduledTick& t) {
        return std::ranges::binary_search(drained, t.order);
    });

    out.insert(out.end(), due.begin(), due.end());
}

void BlockTickScheduler::snapshot(std::vector<ScheduledTick>& out) const {
    out.insert(out.end(), entries_.begin(), entries_.end());
    std::ranges::sort(out, tick_order);
}

void BlockTickScheduler::clear() noexcept {
    pending_.clear();
    entries_.clear();
}

nbt::Tag BlockTickScheduler::save_chunk(ChunkPos chunk, i64 game_time,
                                        const registry::BlockRegistry& blocks) const {
    std::vector<ScheduledTick> mine;
    for (const ScheduledTick& tick : entries_) {
        if (floor_div(tick.pos.x, 16) == chunk.x && floor_div(tick.pos.z, 16) == chunk.z) {
            mine.push_back(tick);
        }
    }
    std::ranges::sort(mine, tick_order);

    nbt::Tag list = nbt::Tag::make_list(nbt::TagType::Compound);
    for (const ScheduledTick& tick : mine) {
        nbt::Tag entry = nbt::Tag::make_compound();
        auto*    body  = entry.compound();
        body->push_back(nbt::CompoundEntry{"i", nbt::Tag{std::string{blocks.block_name(tick.block)}}});
        body->push_back(nbt::CompoundEntry{"p", nbt::Tag{static_cast<i32>(tick.priority)}});
        body->push_back(
            nbt::CompoundEntry{"t", nbt::Tag{static_cast<i32>(tick.trigger_tick - game_time)}});
        body->push_back(nbt::CompoundEntry{"x", nbt::Tag{tick.pos.x}});
        body->push_back(nbt::CompoundEntry{"y", nbt::Tag{tick.pos.y}});
        body->push_back(nbt::CompoundEntry{"z", nbt::Tag{tick.pos.z}});
        list.list()->push_back(std::move(entry));
    }
    return list;
}

usize BlockTickScheduler::load_chunk(const nbt::Tag& list, i64 game_time,
                                     const registry::BlockRegistry& blocks, usize& skipped) {
    skipped = 0;
    if (list.list() == nullptr) {
        return 0;
    }
    usize loaded = 0;
    for (const nbt::Tag& entry : *list.list()) {
        const nbt::Tag* name = entry.find("i");
        const nbt::Tag* x    = entry.find("x");
        const nbt::Tag* y    = entry.find("y");
        const nbt::Tag* z    = entry.find("z");
        if (name == nullptr || x == nullptr || y == nullptr || z == nullptr) {
            ++skipped;
            continue;
        }
        const auto block = blocks.find_block(name->as_string());
        if (!block.has_value()) {
            // Named, not swallowed: a tick for a block we do not have is a
            // world from another version or another implementation.
            ++skipped;
            continue;
        }
        const nbt::Tag* p     = entry.find("p");
        const nbt::Tag* t     = entry.find("t");
        const auto      raw_p = p != nullptr ? p->as_i64() : 0;
        const auto      delay = t != nullptr ? t->as_i64() : 0;
        const auto      clamped_p =
            static_cast<i8>(std::clamp<i64>(raw_p, kMinTickPriority, kMaxTickPriority));
        schedule_at(BlockPos{static_cast<i32>(x->as_i64()), static_cast<i32>(y->as_i64()),
                             static_cast<i32>(z->as_i64())},
                    *block, game_time + delay, static_cast<TickPriority>(clamped_p));
        ++loaded;
    }
    return loaded;
}

}  // namespace ov::world
