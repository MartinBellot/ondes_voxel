// A live world, streamed from a server, meshed as it arrives.
//
// This is what turns the viewer into a client. The disk loader read a fixed
// square of chunks once and meshed all of it before the window opened; a
// server sends chunks over time, changes blocks under you, and expects to be
// told where you are twenty times a second.
//
// Three rules shape it:
//
//   * The level is written by this thread and no other. The network thread
//     hands over finished chunks through a queue and never touches what is
//     already here.
//   * Meshing is budgeted per frame. A hundred chunks arriving at once must
//     not produce a hundred-millisecond stall — the frame time percentile is
//     the whole point, and a spike passes a test of the mean.
//   * A block change remeshes the section it lands in *and the neighbours it
//     touches*, because a face is drawn or hidden according to what is on the
//     other side of it.
#pragma once

#include "ov/base/types.hpp"
#include "ov/client/terrain_renderer.hpp"
#include "ov/gameplay/collision.hpp"
#include "ov/gameplay/physics.hpp"
#include "ov/netclient/client.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/world/light_engine.hpp"  // ── light ──
#include "ov/render/atlas.hpp"
#include "ov/render/biome_colours.hpp"
#include "ov/render/block_models.hpp"
#include "ov/gameplay/block_motion.hpp"
#include "ov/render/chunk_mesher.hpp"
#include "ov/world/chunk.hpp"

#include <map>
#include <set>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace ov::demo {

/// One section's slot in the renderer, per layer.
struct SectionSlots {
    std::array<u32, static_cast<usize>(render::RenderLayer::Count)> slots{};
    bool live{false};
};

class Session {
public:
    Session(const registry::BlockRegistry& blocks, render::BlockModelCache& models,
            const render::TextureAtlas& atlas, const render::BiomeTints& tints,
            client::TerrainRenderer& terrain);

    /// Take what the network produced and fold it into the level.
    ///
    /// Meshing is not done here: a chunk is marked dirty and the work is done
    /// under a budget, so that a burst of arrivals costs several frames rather
    /// than one long one.
    void apply(netclient::ClientEvents& events);

    /// Throw the whole level away — chunks, meshes, pending work, light — for
    /// a new one: a Respawn into another dimension. `rules` are the new
    /// dimension's light.
    void clear_level(world::LightRules rules);

    /// Mesh up to `budget_ms` worth of what is waiting, **nearest to `eye`
    /// first**, and return how many sections were rebuilt.
    ///
    /// Nearest first is what the game does — it compiles the sections closest
    /// to the camera before the horizon — and what a player expects: the
    /// ground under them before the far hills. This used to take the most
    /// recent arrival first, and since the server sends nearest first, that
    /// drew the horizon before the ground.
    usize mesh_pending(f64 budget_ms, const Vec3d& eye);

    [[nodiscard]] const world::Chunk* chunk_at(i32 chunk_x, i32 chunk_z) const;

    /// The block state at a world position, or air outside what has arrived.
    [[nodiscard]] registry::BlockStateId block_at(i32 x, i32 y, i32 z) const;

    /// A collision view over the level, for the player's physics.
    [[nodiscard]] gameplay::CollisionWorld collision() const;

    /// A fluid view over the same level. Separate from the collision one
    /// because water stops nothing and changes everything: it has no boxes and
    /// its own movement rules.
    [[nodiscard]] gameplay::FluidWorld fluids() const;

    /// What fluid stands at a position, and how tall.
    [[nodiscard]] gameplay::FluidSample fluid_at(i32 x, i32 y, i32 z) const;

    /// Can a click hit this block?
    ///
    /// Air cannot, and neither can water or lava: a ray aimed across a pond
    /// must reach the bottom rather than stop at the surface. A waterlogged
    /// fence still can — it is a fence that happens to be wet.
    [[nodiscard]] bool is_interaction_target(i32 x, i32 y, i32 z) const;

    [[nodiscard]] usize chunk_count() const noexcept { return chunks_.size(); }
    [[nodiscard]] usize pending_sections() const noexcept { return dirty_.size(); }
    /// Sections that have geometry and are resident on the GPU.
    [[nodiscard]] usize resident_sections() const noexcept { return resident_; }

    /// The biome under a position, for the fog and the sky.
    [[nodiscard]] u32 biome_at(i32 x, i32 y, i32 z) const;

private:
    /// A section, addressed the way everything here addresses one.
    struct SectionKey {
        i32 chunk_x{0};
        i32 chunk_z{0};
        i32 section{0};

        friend constexpr bool operator==(const SectionKey&, const SectionKey&) noexcept = default;
        friend constexpr bool operator<(const SectionKey& a, const SectionKey& b) noexcept {
            if (a.chunk_x != b.chunk_x) {
                return a.chunk_x < b.chunk_x;
            }
            if (a.chunk_z != b.chunk_z) {
                return a.chunk_z < b.chunk_z;
            }
            return a.section < b.section;
        }
    };

    void mark_dirty(i32 chunk_x, i32 chunk_z, i32 section);
    void mark_column_dirty(i32 chunk_x, i32 chunk_z);
    void mesh_one(const SectionKey& key);
    void release(const SectionKey& key);

    [[nodiscard]] render::ChunkNeighbours neighbours_of(i32 chunk_x, i32 chunk_z) const;

    const registry::BlockRegistry* blocks_;
    /// ── movement physics ── ice, ladders, slime, cobwebs, bubble columns:
    /// resolved once from the registry, read by the player's physics.
    gameplay::BlockMotionTable     motion_;
    render::BlockModelCache*       models_;
    const render::TextureAtlas*    atlas_;
    const render::BiomeTints*      tints_;
    client::TerrainRenderer*       terrain_;

    std::map<std::pair<i32, i32>, std::unique_ptr<world::Chunk>> chunks_;
    std::map<SectionKey, SectionSlots>                           slots_;
    /// Sections waiting to be meshed. Sorted furthest-first from the focus
    /// by `mesh_pending`, so the back is the nearest.
    std::vector<SectionKey> dirty_;
    /// The same sections, for membership: a section changed twice before it
    /// is drawn is meshed once. A linear search of `dirty_` did this before,
    /// and with a few thousand sections waiting while 289 chunks arrive —
    /// each marking its own column and its four neighbours' — it was tens
    /// of millions of comparisons per join.
    std::set<SectionKey> dirty_set_;
    /// Whether `dirty_` is still in order for the focus below.
    bool dirty_sorted_{false};
    i32  focus_x_{0};
    i32  focus_z_{0};
    i32  focus_section_{0};

    /// The two fluid blocks, resolved once. Looking them up by name per cell
    /// would put a string comparison in the physics tick.
    registry::BlockId water_block_{};
    registry::BlockId lava_block_{};
    bool              fluids_known_{false};

    usize resident_{0};
    /// Scratch, kept between calls so meshing allocates nothing per section.
    render::MeshBuffers scratch_;

    /// ── light ── The server sends no light after an edit — the game's own
    /// client lights its edits itself, and so does this one: a Block Update is
    /// noted here and the light around it repaired once per `apply`, with the
    /// same engine the server runs, with the dimension's rules — no sky light
    /// in the Nether or the End (`clear_level`).
    std::unique_ptr<world::LightEngine> light_;
};

}  // namespace ov::demo
