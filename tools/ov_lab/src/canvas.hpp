// A canvas of chunks, so a test plot reads like what it builds.
//
// The lab world is written block by block from code rather than built by hand
// in a client, because a bench that cannot be regenerated is a bench that
// drifts: someone moves a lever, a measurement changes, and nothing records
// why. `ov-lab` writes the same world every time from the same source.
//
// Coordinates here are **world** coordinates throughout. A builder that took
// chunk-local ones would be faster and would put every plot in the wrong place
// the first time a plot crossed a chunk border — and plots do.
#pragma once

#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/chunk.hpp"

#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ov::lab {

/// Property values for a block state, e.g. {{"facing", "north"}}.
using Props = std::vector<std::pair<std::string_view, std::string_view>>;

class Canvas {
public:
    Canvas(const registry::BlockRegistry& blocks, const registry::Registries* registries);

    /// The y of the topmost solid block of the flat ground, matching the
    /// server's superflat preset. A plot builds upwards from here; a player
    /// stands at `kGroundY + 1`.
    static constexpr i32 kGroundY = -61;

    /// Resolve a name and optional properties to a state.
    ///
    /// A name this version does not have is **recorded and reported**, not
    /// substituted: a bench with a silent hole in it is worse than no bench,
    /// because the measurements that trust it will not know.
    [[nodiscard]] registry::BlockStateId state(std::string_view name, const Props& props = {}) const;

    void set(i32 x, i32 y, i32 z, registry::BlockStateId state);
    void set(i32 x, i32 y, i32 z, std::string_view name, const Props& props = {});

    /// Inclusive on both corners, in any order.
    void fill(i32 x0, i32 y0, i32 z0, i32 x1, i32 y1, i32 z1, std::string_view name,
              const Props& props = {});

    /// A hollow box: the shell only, six faces.
    void shell(i32 x0, i32 y0, i32 z0, i32 x1, i32 y1, i32 z1, std::string_view name,
               const Props& props = {});

    /// A standing sign with up to four lines of front text.
    ///
    /// 1.20 rewrote the sign's NBT — `front_text` / `back_text`, each holding
    /// four JSON text components — and a sign written in the pre-1.20 shape
    /// loads blank rather than failing, which is the worst of both.
    void sign(i32 x, i32 y, i32 z, std::span<const std::string> lines, i32 rotation = 8);

    /// The flat ground every plot sits on, over a chunk-aligned area.
    void lay_ground(i32 chunk_x0, i32 chunk_z0, i32 chunk_x1, i32 chunk_z1);

    [[nodiscard]] world::Chunk& chunk(i32 chunk_x, i32 chunk_z);

    /// The registry the canvas resolves against. The block gallery walks it,
    /// which is the whole reason a plot can be "every block there is" rather
    /// than a list someone has to keep up to date.
    [[nodiscard]] const registry::BlockRegistry& blocks() const noexcept { return *blocks_; }

    [[nodiscard]] const std::map<std::pair<i32, i32>, world::Chunk>& chunks() const noexcept {
        return chunks_;
    }

    /// Names that did not resolve, deduplicated and in the order first asked
    /// for.
    [[nodiscard]] std::span<const std::string> unknown() const noexcept { return unknown_; }

    [[nodiscard]] usize blocks_written() const noexcept { return written_; }

private:
    const registry::BlockRegistry* blocks_{nullptr};
    const registry::Registries*    registries_{nullptr};
    world::AirStates               air_{};
    i32                            sign_type_id_{0};

    std::map<std::pair<i32, i32>, world::Chunk> chunks_;
    mutable std::vector<std::string>            unknown_;
    usize                                       written_{0};
};

}  // namespace ov::lab
