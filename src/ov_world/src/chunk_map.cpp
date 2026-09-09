#define OV_LOG_CATEGORY "world"

#include "ov/world/chunk_map.hpp"

#include <algorithm>
#include <utility>

namespace ov::world {

void ChunkMap::set_ticket(TicketType type, u64 owner, ChunkPos pos, i32 level) {
    tickets_[ticket_key(type, owner)] = Ticket{type, owner, pos, level};
}

void ChunkMap::remove_ticket(TicketType type, u64 owner) {
    tickets_.erase(ticket_key(type, owner));
}

void ChunkMap::refresh(LevelChanges& changes) {
    changes.wanted.clear();
    changes.unwanted.clear();

    // Rebuilt rather than patched. A ticket's reach is bounded — `kUnloaded`
    // minus its level — so the whole map is one pass over a few hundred
    // squares, and a from-scratch answer cannot drift the way an incremental
    // one does when a removal is missed.
    std::unordered_map<u64, i32> next;
    next.reserve(levels_.size());

    for (const auto& [key, ticket] : tickets_) {
        const i32 reach = LoadLevel::kUnloaded - 1 - ticket.level;
        if (reach < 0) {
            continue;
        }
        for (i32 dz = -reach; dz <= reach; ++dz) {
            for (i32 dx = -reach; dx <= reach; ++dx) {
                const i32 distance = std::max(std::abs(dx), std::abs(dz));
                const i32 level    = ticket.level + distance;
                if (level > LoadLevel::kLoaded) {
                    continue;
                }
                const u64  packed = ChunkPos{ticket.pos.x + dx, ticket.pos.z + dz}.packed();
                const auto it     = next.find(packed);
                if (it == next.end()) {
                    next.emplace(packed, level);
                } else {
                    it->second = std::min(it->second, level);
                }
            }
        }
    }

    for (const auto& [packed, level] : next) {
        if (!levels_.contains(packed)) {
            changes.wanted.push_back(ChunkPos::from_packed(packed));
        }
    }
    for (const auto& [packed, level] : levels_) {
        if (!next.contains(packed)) {
            changes.unwanted.push_back(ChunkPos::from_packed(packed));
        }
    }

    levels_ = std::move(next);
}

i32 ChunkMap::level_of(ChunkPos pos) const {
    const auto it = levels_.find(pos.packed());
    return it == levels_.end() ? LoadLevel::kUnloaded : it->second;
}

void ChunkMap::wanted_chunks(std::vector<ChunkPos>& out) const {
    out.clear();
    out.reserve(levels_.size());
    for (const auto& [packed, level] : levels_) {
        out.push_back(ChunkPos::from_packed(packed));
    }
    // By level first: a chunk the player is standing on has the lowest level
    // any ticket gave it, so this is "nearest first" without the map having to
    // know where anybody is. The packed key breaks ties so the order is total
    // and does not depend on the hash table's layout — an unstable order here
    // would make two runs of the same server send chunks in different
    // sequences, which is the kind of nondeterminism that hides in a diff.
    std::ranges::sort(out, [this](ChunkPos a, ChunkPos b) {
        const i32 la = level_of(a);
        const i32 lb = level_of(b);
        if (la != lb) {
            return la < lb;
        }
        return a.packed() < b.packed();
    });
}

void ChunkMap::publish(ChunkPos pos, Chunk chunk) {
    const u64 packed = pos.packed();
    in_flight_.erase(packed);
    if (const auto it = chunks_.find(packed); it != chunks_.end()) {
        it->second = std::move(chunk);
        return;
    }
    chunks_.emplace(packed, std::move(chunk));
}

Chunk* ChunkMap::find(ChunkPos pos) {
    const auto it = chunks_.find(pos.packed());
    return it == chunks_.end() ? nullptr : &it->second;
}

const Chunk* ChunkMap::find(ChunkPos pos) const {
    const auto it = chunks_.find(pos.packed());
    return it == chunks_.end() ? nullptr : &it->second;
}

std::optional<Chunk> ChunkMap::evict(ChunkPos pos) {
    const auto it = chunks_.find(pos.packed());
    if (it == chunks_.end()) {
        return std::nullopt;
    }
    std::optional<Chunk> taken{std::move(it->second)};
    chunks_.erase(it);
    return taken;
}

bool ChunkMap::mark_in_flight(ChunkPos pos) { return in_flight_.insert(pos.packed()).second; }

}  // namespace ov::world
