// The bits of a block that fly off when it is hit or broken.
//
// What the wiki says of them (Particles, Java Edition): produced when blocks
// are broken, textured "from the broken block", squares that "use a random
// part of the textures", that "collide with solid blocks" and "disappear after
// a short animation". What it does not say — how many, how fast, for how long —
// is written here as named constants, and docs/provenance/cassage-bloc.md says
// which were measured against the real client and which were not.
//
// Two ways to make them, as the game has:
//
//   * **destroy** — a block came off. A grid over every box of its shape, at
//     least two cells a side and one per quarter block (four by four by four
//     for a full cube), each flung out from the centre of its box;
//   * **crack** — a tick of hitting a block. One, just outside the face being
//     hit, slower and smaller.
//
// Each lives a few ticks, falls, slows, stops on what it lands on, and is drawn
// as a square facing the camera, cut from a quarter of the block's particle
// sprite at a random offset. The simulation runs at the client's 20 Hz; the
// drawing interpolates between ticks.
//
// Randomness is seeded and explicit. The real client draws these from an
// unseeded source, so no particle here can be bit-identical to one there;
// what can be compared is the counts, the distributions and the lifetimes.
#pragma once

#include "ov/base/types.hpp"
#include "ov/gameplay/collision.hpp"
#include "ov/math/aabb.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/math/random.hpp"
#include "ov/math/vec.hpp"
#include "ov/render/entity_mesh.hpp"

#include <array>
#include <functional>
#include <span>
#include <vector>

namespace ov::client {

/// Where a block's particle sprite sits in the atlas, in normalised
/// coordinates.
struct ParticleSprite {
    f32 u0{0.0F};
    f32 v0{0.0F};
    f32 u1{1.0F};
    f32 v1{1.0F};
};

struct BlockParticle {
    Vec3d position{};
    Vec3d previous{};
    Vec3d velocity{};
    /// Half the width of the square, in blocks.
    f32 size{0.1F};
    f32 gravity{1.0F};
    i32 age{0};
    i32 lifetime{1};
    bool on_ground{false};
    /// Half the width of the box it collides with.
    f64 half_extent{0.1};
    /// The quarter of the sprite it shows: u0, v0, u1, v1.
    std::array<f32, 4> uv{};
    /// Multiplied into the texture: the 0.6 every block particle takes, times
    /// the block's own tint.
    std::array<f32, 3> colour{0.6F, 0.6F, 0.6F};
};

/// The lightmap colour at a point.
using ParticleLight = std::function<std::array<u8, 3>(Vec3f position)>;

class BlockParticles {
public:
    /// The most kept at once. Past it the oldest go first.
    static constexpr usize kMaxParticles = 16384;

    explicit BlockParticles(i64 seed) noexcept : random_{seed} {}

    /// A block came off. `shape` is its boxes in block space (0..1 across the
    /// block); `tint` 0xRRGGBB, white for an untinted block.
    void destroy(BlockPos pos, std::span<const AABB> shape, const ParticleSprite& sprite,
                 u32 tint = 0xFFFFFF);

    /// One tick of hitting `face` of the block at `pos`, whose shape's bounds
    /// in block space are `bounds`.
    void crack(BlockPos pos, Direction face, const AABB& bounds, const ParticleSprite& sprite,
               u32 tint = 0xFFFFFF);

    /// One client tick. `world` may be null: then nothing stops them.
    void tick(const gameplay::CollisionWorld* world);

    /// Append a camera-facing square per particle, four vertices each, in
    /// emit_quad's winding. `partial` is how far into the next tick the frame
    /// is, 0..1.
    void build(Vec3f camera_right, Vec3f camera_up, f32 partial, const ParticleLight& light,
               std::vector<render::EntityVertex>& out) const;

    [[nodiscard]] std::span<const BlockParticle> particles() const noexcept { return live_; }
    [[nodiscard]] usize count() const noexcept { return live_.size(); }
    void clear() noexcept { live_.clear(); }

private:
    BlockParticle& spawn(Vec3d at, Vec3d push, const ParticleSprite& sprite, u32 tint);

    math::LegacyRandomSource   random_;
    std::vector<BlockParticle> live_;
    std::vector<AABB>          boxes_;
};

}  // namespace ov::client
