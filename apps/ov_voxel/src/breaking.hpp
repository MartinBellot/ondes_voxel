// Breaking a block, as the player sees and hears it.
//
// The glue between the rule (gameplay::DigController, which decides what to
// send and when), the wire (netclient), the ears (client::SoundDirector) and
// the eyes (the cracks, the particles, the outline). Everything that decides a
// number lives below this file and is tested there; this one only wires.
//
// What it draws:
//
//   * the cracks — this player's own, counted here, and every other player's,
//     from Set Block Destroy Stage — on the block's real model;
//   * the particles — a destroy burst when a block comes off (this player's, or
//     another's through World Event 2001) and a chip a tick while one is hit;
//   * the outline of the aimed block, along its shape rather than a cube.
//
// What it does not draw, named: the first-person arm and held item. The swing
// is counted (its duration follows Haste and Mining Fatigue) and sent, but this
// client has no first-person hand to swing. docs/provenance/cassage-bloc.md.
#pragma once

#include "ov/base/types.hpp"
#include "ov/client/block_particles.hpp"
#include "ov/gameplay/breaking.hpp"
#include "ov/gameplay/collision.hpp"
#include "ov/gameplay/dig_controller.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/math/raycast.hpp"
#include "ov/netclient/client.hpp"
#include "ov/protocol/play.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"
#include "ov/render/atlas.hpp"
#include "ov/render/block_models.hpp"
#include "ov/render/entity_mesh.hpp"

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace ov::client {
class SoundDirector;
}

namespace ov::demo {

/// What one client tick of breaking needs to know.
struct BreakingTick {
    bool                  pressed{false};
    bool                  held{false};
    std::optional<RayHit> aimed;
    bool                  creative{false};
    bool                  on_ground{true};
    bool                  head_in_water{false};
    /// Window 0: armour at 5..8, the hotbar at 36..44.
    const std::vector<net::ItemStack>* inventory{nullptr};
    i32                                selected{0};
};

/// The block at a position, as this client knows it.
using BlockAt = std::function<registry::BlockStateId(BlockPos)>;

class Breaking {
public:
    /// Ticks a crack nobody updates is kept before it is dropped.
    static constexpr i32 kStaleCrackTicks = 400;

    Breaking(const registry::BlockRegistry& blocks, const registry::Registries* registries,
             render::BlockModelCache& models, const render::TextureAtlas& atlas,
             u32 foliage_tint, i64 seed);

    /// What the server said this poll: other players' cracks, the blocks they
    /// broke, this player's id and its effects.
    void on_events(const netclient::ClientEvents& events);

    /// One client tick, 20 Hz: the dig, its packets, its sounds, its particles.
    void tick(const BreakingTick& input, const BlockAt& block_at, netclient::Client& client,
              client::SoundDirector* sounds, const gameplay::CollisionWorld* world);

    /// The crack quads of every drawn stage, filed by stage (0..9).
    void build_cracks(const BlockAt& block_at,
                      std::array<std::vector<render::EntityVertex>, 10>& out);

    /// The particles, camera-facing.
    void build_particles(Vec3f camera_right, Vec3f camera_up, f32 partial,
                         const client::ParticleLight& light,
                         std::vector<render::EntityVertex>& out) const;

    /// The outline of the block at `pos`, edges in world space.
    void outline(BlockPos pos, registry::BlockStateId state,
                 std::vector<std::array<Vec3d, 2>>& out);

    /// Show `stage` on a block as a crack of nobody's — for a scripted
    /// screenshot (--crack). A stage outside 0..9 removes it.
    void force_stage(BlockPos pos, i32 stage);

    /// Seconds since the swing began over its duration, 0..1, or nothing when
    /// the arm is still. For the day this client draws a hand.
    [[nodiscard]] std::optional<f32> swing_progress(f32 partial) const noexcept;
    /// The swing's length in ticks: 6, less one a Haste level, plus two a
    /// Mining Fatigue level.
    [[nodiscard]] i32 swing_duration() const noexcept;

    [[nodiscard]] const gameplay::DigController& dig() const noexcept { return dig_; }
    [[nodiscard]] const client::BlockParticles& particles() const noexcept { return particles_; }
    [[nodiscard]] u64 ticks() const noexcept { return ticks_; }

    /// What happened, for the scripted checks and the end-of-run report.
    struct Counters {
        u64 starts{0};
        u64 aborts{0};
        u64 finishes{0};
        u64 broken{0};
        u64 swings{0};
        u64 hits{0};
        u64 others_stages{0};
        u64 others_broken{0};
        /// Particles not made because the sprite is not in the atlas.
        u64 no_sprite{0};
        /// Frames a crack was held on a block with no model of its own (a
        /// sign, a chest: block entities), so nothing could be drawn.
        u64 cracks_unmodelled{0};
    };
    [[nodiscard]] const Counters& counters() const noexcept { return counters_; }

private:
    struct Crack {
        BlockPos pos{};
        i32      stage{-1};
        u64      updated{0};
    };

    [[nodiscard]] gameplay::Held    held(const BreakingTick& input) const;
    [[nodiscard]] gameplay::Stance  stance(const BreakingTick& input) const;
    [[nodiscard]] bool              creative_can_attack(const BreakingTick& input) const;
    [[nodiscard]] i32               held_identity(const BreakingTick& input) const;
    [[nodiscard]] std::optional<client::ParticleSprite> sprite_of(registry::BlockStateId state);
    [[nodiscard]] u32               tint_of(registry::BlockStateId state);
    void shape_of(registry::BlockStateId state, std::vector<AABB>& out);
    void burst(BlockPos pos, registry::BlockStateId state);
    void chip(BlockPos pos, Direction face, registry::BlockStateId state);
    void swing(netclient::Client& client);
    void set_crack(i32 key, BlockPos pos, i32 stage);

    const registry::BlockRegistry* blocks_;
    const registry::Registries*    registries_;
    render::BlockModelCache*       models_;
    const render::TextureAtlas*    atlas_;
    u32                            foliage_tint_;
    std::optional<gameplay::BreakRules> rules_;
    std::optional<registry::RegistryId> item_registry_;

    gameplay::DigController dig_;
    client::BlockParticles  particles_;

    std::optional<i32> own_id_;
    /// Effect wire id to amplifier, this player's.
    std::map<i32, i32> effects_;
    /// Cracks by breaker: entity id, or the two keys below.
    std::map<i32, Crack> cracks_;

    bool swinging_{false};
    i32  swing_time_{0};
    u64  ticks_{0};
    u64  start_tick_{0};

    std::vector<AABB>                     boxes_;
    std::vector<std::array<Vec3d, 2>>     edges_;
    std::vector<std::pair<BlockPos, i32>> deepest_;
    /// The last block a crack could not be drawn on, so it is named once.
    std::optional<BlockPos>               last_unmodelled_;
    Counters                              counters_;
};

}  // namespace ov::demo
