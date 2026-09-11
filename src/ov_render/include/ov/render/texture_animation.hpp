// Playing the `.mcmeta` animations: water, lava, fire, the Nether portal,
// magma, prismarine, sea lanterns, kelp...
//
// The format is documented (a `frametime` in ticks, an optional `frames` list
// of cells and per-frame times, and `interpolate`), and so is the clock it runs
// on: one step per client tick, twenty a second. What the atlas holds at level
// 0 is each animation's first frame; this class says, tick by tick, which
// sprites have changed and hands back their new pixels for every mip level —
// a rectangle each, to be copied into the atlas image where the sprite already
// is. Nothing is re-stitched, and a tick on which no frame changes costs a
// comparison per animation.
//
// No GPU here: the frame arithmetic and the blend are unit tests, and the
// client owns the copy (apps/ov_voxel, "render-parity" blocks).
#pragma once

#include "ov/base/types.hpp"
#include "ov/render/atlas.hpp"

#include <span>
#include <vector>

namespace ov::render {

/// Where an animation stands on a given tick.
struct AnimationPhase {
    /// Index into SpriteAnimation::frames, and the one after it (wrapping).
    u32 frame{0};
    u32 next{0};
    /// Ticks already spent on `frame`, and how many it lasts.
    u32 sub_tick{0};
    u32 frame_time{1};
};

/// The phase at `tick`, counted from the tick the animation started on.
[[nodiscard]] AnimationPhase animation_phase(const SpriteAnimation& animation, u64 tick) noexcept;

/// One rectangle of one mip level to copy into the atlas image.
struct AtlasPatch {
    u32 mip{0};
    u32 x{0};
    u32 y{0};
    u32 width{0};
    u32 height{0};
    /// Where its rows — width * 4 bytes each, tightly packed — start in
    /// TextureAnimator::bytes().
    usize offset{0};
};

class TextureAnimator {
public:
    explicit TextureAnimator(const TextureAtlas& atlas);

    /// Compose every animation whose picture differs from the one last
    /// composed. Returns how many sprites changed; their patches and bytes
    /// replace the previous call's.
    usize tick(u64 tick);

    [[nodiscard]] std::span<const AtlasPatch> patches() const noexcept { return patches_; }
    [[nodiscard]] std::span<const u8>         bytes() const noexcept { return bytes_; }

    [[nodiscard]] usize animations() const noexcept { return last_key_.size(); }
    /// The most bytes() can hold after one tick: every animation changing at
    /// once, at every level. What a staging buffer is sized to.
    [[nodiscard]] usize max_bytes() const noexcept { return max_bytes_; }

private:
    void push(u32 mip, u32 x, u32 y, const AtlasMip& level);

    const TextureAtlas*     atlas_;
    usize                   levels_{1};
    std::vector<AtlasPatch> patches_;
    std::vector<u8>         bytes_;
    /// Per animation: the frame and blend step last composed, or -1.
    std::vector<i64> last_key_;
    usize            max_bytes_{0};
    AtlasMip         level_;
};

}  // namespace ov::render
