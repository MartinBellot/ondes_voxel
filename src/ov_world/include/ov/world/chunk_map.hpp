// Which chunks are loaded, why, and what may be done with them.
//
// Before this existed the server loaded a chunk because somebody happened to
// ask for it and unloaded it never. That is workable while a world is sixteen
// flat chunks and untenable the moment generating one costs real time: the
// question "should this chunk exist right now" had no owner, so the answer was
// always yes.
//
// The model is the game's, and it has two halves that are easy to conflate:
//
//   * A **ticket** is a reason. A player standing somewhere is a reason; a
//     forceload is a reason; a chunk a feature has to reach into is a reason.
//     Tickets are added and dropped by whoever owns the reason.
//
//   * A **level** is what follows from the reasons. Every ticket names a level
//     at its own chunk, and that level rises by one for each chunk of Chebyshev
//     distance outward; a chunk's level is the lowest any ticket gives it.
//     Lower means more loaded. Past `kUnloaded` nothing wants the chunk and it
//     goes away.
//
// The level, not the ticket, decides what happens to a chunk: ticked, merely
// resident and sendable, or gone. That indirection is the point — a chunk two
// steps beyond a player is loaded because the player's ticket reaches it, and
// it stops being loaded when the player walks away without anyone having to
// remember they were the one who asked.
//
// ── Threading ───────────────────────────────────────────────────────────────
//
// `ChunkMap` is not thread-safe and does not try to be. It is meant to be owned
// by one thread — the tick thread, in the server — which is the only writer of
// every chunk it holds (CLAUDE.md § 2, principle 3). Work that takes too long
// for the tick happens elsewhere on a chunk that nothing else can see yet, and
// arrives here through `publish`, by move, once. There is no mutex on the world
// in this file and there is not meant to be one.
//
// ── The numbers ─────────────────────────────────────────────────────────────
//
// The three thresholds below are **ours**. They are shaped like the game's — a
// level that propagates outward one per chunk, with a cut-off past which
// nothing is loaded — because that shape is what makes a view distance
// expressible as a single number. We have not measured the game's constants,
// so this file does not claim them. Nothing on the wire depends on the choice.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/world/chunk.hpp"

#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ov::world {

/// Why a chunk is loaded. The type is carried so that a reason can be dropped
/// without knowing what else was asked for at the same place.
enum class TicketType : u8 {
    /// A connected player. Dropped when they disconnect or move.
    Player = 0,
    /// A `/forceload`-style pin, or the world spawn.
    Forced = 1,
    /// Something transient asked for this chunk and will let go.
    Transient = 2,
};

/// Load levels. Lower is more loaded.
struct LoadLevel {
    /// The chunk is simulated: block ticks, entities, fluids.
    static constexpr i32 kTicking = 31;

    /// The chunk is resident and may be sent to a client, but is not ticked.
    /// The outermost ring of a player's view distance sits exactly here.
    static constexpr i32 kLoaded = 33;

    /// No ticket reaches this chunk. It may be written out and forgotten.
    static constexpr i32 kUnloaded = 34;

    /// The centre level a view-distance ticket needs so that its outermost
    /// ring lands on `kLoaded` and the ring beyond it on `kUnloaded`.
    [[nodiscard]] static constexpr i32 for_view_distance(i32 chunks) noexcept {
        return kLoaded - chunks;
    }
};

/// One reason, at one place.
struct Ticket {
    TicketType type{TicketType::Player};
    /// Identifies the holder within its type. A player's entity id, a
    /// forceload's index — the map only ever compares it.
    u64      owner{0};
    ChunkPos pos{};
    /// The level this ticket gives its own chunk. See `LoadLevel`.
    i32 level{LoadLevel::kLoaded};
};

/// What a refresh changed.
struct LevelChanges {
    /// Chunks that are wanted now and were not before. Nearest-first is not
    /// promised here; the caller orders them by whatever it is optimising.
    std::vector<ChunkPos> wanted;

    /// Chunks that no ticket reaches any more. Still resident until the caller
    /// calls `evict`; handing them over rather than dropping them is what lets
    /// the server write them to disk and tell clients to unload them.
    std::vector<ChunkPos> unwanted;
};

/// The loaded chunks, the tickets that keep them loaded, and nothing else.
class ChunkMap {
public:
    ChunkMap() = default;

    ChunkMap(const ChunkMap&)            = delete;
    ChunkMap& operator=(const ChunkMap&) = delete;
    ChunkMap(ChunkMap&&)                 = default;
    ChunkMap& operator=(ChunkMap&&)      = default;
    ~ChunkMap()                          = default;

    // ── Tickets ─────────────────────────────────────────────────────────────

    /// Add a ticket, or move the one this holder already has.
    ///
    /// A holder has at most one ticket per type, which is what makes a player
    /// walking a *move* rather than a leak: the previous position stops being
    /// a reason the moment the new one is set.
    void set_ticket(TicketType type, u64 owner, ChunkPos pos, i32 level);

    /// Drop a holder's ticket of that type. Silent if there was none.
    void remove_ticket(TicketType type, u64 owner);

    [[nodiscard]] usize ticket_count() const noexcept { return tickets_.size(); }

    /// Recompute every level from the tickets and report what moved.
    ///
    /// Cheap enough to call whenever a ticket changes — one player at view
    /// distance eight is 289 squares — and deliberately *not* incremental: an
    /// incremental level propagator is where this kind of code goes wrong, and
    /// the cost only becomes interesting at a player count this server does not
    /// have yet. When it does, this is the function to replace, and its tests
    /// are the specification.
    void refresh(LevelChanges& changes);

    /// The level of a chunk, `kUnloaded` when no ticket reaches it.
    [[nodiscard]] i32 level_of(ChunkPos pos) const;

    /// Some ticket reaches this chunk: it should be resident.
    [[nodiscard]] bool is_wanted(ChunkPos pos) const { return level_of(pos) <= LoadLevel::kLoaded; }

    /// The chunk should be simulated, not merely present.
    [[nodiscard]] bool is_ticking(ChunkPos pos) const {
        return level_of(pos) <= LoadLevel::kTicking;
    }

    /// Every chunk a ticket reaches, in ascending (level, distance) order so
    /// that a caller draining a budget serves the middle of the world first.
    void wanted_chunks(std::vector<ChunkPos>& out) const;

    // ── Storage ─────────────────────────────────────────────────────────────
    //
    // The single writer. Everything below is called from the owning thread and
    // from nowhere else; that is the whole of the concurrency design.

    /// Install a finished chunk. Replaces whatever was there.
    void publish(ChunkPos pos, Chunk chunk);

    [[nodiscard]] Chunk* find(ChunkPos pos);

    [[nodiscard]] const Chunk* find(ChunkPos pos) const;

    [[nodiscard]] bool contains(ChunkPos pos) const { return find(pos) != nullptr; }

    /// Remove a chunk and hand it over. Empty if it was not there.
    [[nodiscard]] std::optional<Chunk> evict(ChunkPos pos);

    [[nodiscard]] usize resident() const noexcept { return chunks_.size(); }

    /// Every resident chunk, for a save pass.
    template <typename Fn>
    void for_each(Fn&& fn) const {
        for (const auto& [key, chunk] : chunks_) {
            fn(ChunkPos::from_packed(key), chunk);
        }
    }

    // ── Requests in flight ──────────────────────────────────────────────────
    //
    // A chunk that has been handed to a generator is neither absent nor
    // resident, and forgetting that is how the same chunk gets generated eight
    // times while the queue drains.

    /// Mark a chunk as being generated. False if it already was.
    bool mark_in_flight(ChunkPos pos);

    [[nodiscard]] bool is_in_flight(ChunkPos pos) const { return in_flight_.contains(pos.packed()); }

    void clear_in_flight(ChunkPos pos) { in_flight_.erase(pos.packed()); }

    [[nodiscard]] usize in_flight_count() const noexcept { return in_flight_.size(); }

private:
    [[nodiscard]] static u64 ticket_key(TicketType type, u64 owner) noexcept {
        return (static_cast<u64>(type) << 56) ^ owner;
    }

    std::unordered_map<u64, Ticket> tickets_;
    std::unordered_map<u64, i32>    levels_;
    std::unordered_map<u64, Chunk>  chunks_;
    std::unordered_set<u64>         in_flight_;
};

}  // namespace ov::world
