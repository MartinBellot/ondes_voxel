// Stitching: from a set of sprite names, to one texture and a table of rects.
//
// The mesher emits `minecraft:block/oak_planks` and 0..16 sprite units; the GPU
// wants one bound texture and normalised coordinates. The atlas is the whole of
// that translation, and it is decided once at resource-pack load.
//
// Three things here are not obvious, and each of them is a visible bug when it
// is got wrong:
//
//   • Mixed resolutions. `run/assets` stacks Faithful 32x over the vanilla 16x
//     jar, so a pack is not one resolution — it is mostly one resolution with
//     holes, plus a couple of textures that are legitimately bigger because
//     they span more than one block. Sprites are scaled up to the pack's
//     commonest resolution by integer pixel replication, never resampled: a
//     filter would invent colours that are not in the source, which for pixel
//     art is the one unforgivable thing. See AtlasBuilder::build() for why
//     "commonest" and not "largest", and what it costs to get that wrong.
//
//   • Mip alignment. At mip level N a texel covers a 2^N block of the atlas,
//     so a sprite whose size or position is not a multiple of 2^N gets its
//     neighbours averaged into its edge. The fix is not padding, it is
//     alignment plus a cap on N — see TextureAtlas::mip_level().
//
//   • Alpha-weighted averaging. Leaves, grass and glass are cutouts: fully
//     transparent texels carry black RGB, and a naive box filter drags that
//     black into the visible colour. Distant foliage goes grey-black and the
//     cause is invisible at mip 0. See mip_level() and the tests.
//
// Animation is parsed but not played: see SpriteAnimation.
#pragma once

#include "ov/base/types.hpp"
#include "ov/render/asset_source.hpp"

#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ov::render {

/// A sprite's rectangle in the atlas, normalised to 0..1, v downward — the same
/// sense as the model format, so a face's 0..16 uv maps in with a lerp and no
/// flip.
///
/// The rect is exact: it runs from the sprite's first texel edge to its last,
/// with no inset. Any half-texel shrink a sampler needs depends on the mip
/// level it is sampling, so it belongs at sampling time, not baked in here.
struct SpriteUv {
    f32 u0{0.0F};
    f32 v0{0.0F};
    f32 u1{1.0F};
    f32 v1{1.0F};
};

/// One entry of an animation's frame sequence.
struct AnimationFrame {
    /// Which cell of the source texture's frame grid, in reading order.
    u32 index{0};
    /// How many ticks it shows for. 20 ticks to the second.
    u32 time{1};
};

/// A texture's `.mcmeta` animation, parsed.
///
/// ⚠️ Recorded, not played. The atlas holds frame 0 and nothing else: playing
/// an animation means re-uploading its rect every few ticks, which is a job for
/// the upload path in ov_rhi and does not exist yet. Everything needed to drive
/// it is kept here so that adding it later does not mean re-reading the pack.
struct SpriteAnimation {
    /// One frame's size in *source* texels, before the atlas scales the sprite.
    u32 frame_width{0};
    u32 frame_height{0};
    /// `frametime`: the duration of any frame that does not state its own.
    u32 default_frame_time{1};
    /// Blend between consecutive frames rather than cutting.
    bool interpolate{false};
    /// In play order. Never empty: a texture with an `animation` object but no
    /// usable `frames` list gets every cell of its grid, in reading order.
    std::vector<AnimationFrame> frames;
};

/// One sprite's place in the atlas.
struct AtlasSprite {
    /// Canonical, e.g. `minecraft:block/stone`.
    std::string name;
    /// Top-left in mip level 0 texels. Always a multiple of 2^mip_level.
    u32 x{0};
    u32 y{0};
    /// Size in mip level 0 texels, after scaling. Also a multiple of
    /// 2^mip_level, which is what makes the alignment above hold.
    u32      width{0};
    u32      height{0};
    SpriteUv uv{};

    std::optional<SpriteAnimation> animation;

    /// The texture did not load, and this entry points at the checkerboard.
    /// Several missing sprites share one rect, so `x`/`y` are not unique when
    /// this is set. Worth logging: a missing texture that is visible gets
    /// fixed, and one that is not gets reported as "the block does not render".
    bool missing{false};
};

/// One mip level's pixels: RGBA8, row-major from the top-left.
struct AtlasMip {
    u32             width{0};
    u32             height{0};
    std::vector<u8> rgba;
};

/// A stitched atlas: the pixels, and where every sprite went.
class TextureAtlas {
public:
    [[nodiscard]] u32 width() const noexcept { return mips_.empty() ? 0 : mips_.front().width; }

    [[nodiscard]] u32 height() const noexcept { return mips_.empty() ? 0 : mips_.front().height; }

    /// The number of halvings below level 0, so `mips()` holds one more image
    /// than this.
    ///
    /// It may be lower than what the builder was asked for. A sprite whose
    /// scaled size is not a multiple of 2^N cannot survive N halvings with
    /// whole texels of its own, and vanilla's answer to that is to lower N for
    /// the whole atlas rather than to pad the sprite. Padding is the obvious
    /// alternative and it is worse: the padding has to be filled with
    /// *something*, that something is averaged into the sprite at every level
    /// anyway, and it costs atlas area. Lowering N costs sharpness at distance
    /// on one atlas, and costs nothing when every sprite is a power of two —
    /// which is why 16x16 sprites and a mip level of 4 are exactly compatible:
    /// 16 >> 4 is 1.
    [[nodiscard]] u32 mip_level() const noexcept {
        return mips_.empty() ? 0 : static_cast<u32>(mips_.size() - 1);
    }

    /// Level 0 first. Never empty for an atlas that was built.
    [[nodiscard]] const std::vector<AtlasMip>& mips() const noexcept { return mips_; }

    /// Levels above mip_level() are clamped to it, so a caller that asks for
    /// more than the atlas has gets the smallest image rather than a crash.
    [[nodiscard]] const AtlasMip& mip(u32 level) const noexcept;

    /// Sorted by name, which makes a stitch reproducible and a diff readable.
    [[nodiscard]] const std::vector<AtlasSprite>& sprites() const noexcept { return sprites_; }

    /// Null when the name was never given to the builder. A name that *was*
    /// given but failed to load is present, with `missing` set.
    ///
    /// Looked up by canonical name — `minecraft:block/stone`, never
    /// `block/stone`. That is what AtlasBuilder::add() stores and what a baked
    /// quad carries, so the two always meet.
    [[nodiscard]] const AtlasSprite* find(std::string_view name) const noexcept;

    /// The rect to texture a face with. Falls back to the checkerboard, so a
    /// mesher never has to decide what to do about a name it cannot resolve.
    [[nodiscard]] SpriteUv uv(std::string_view name) const noexcept;

private:
    friend class AtlasBuilder;

    std::vector<AtlasMip>    mips_;
    std::vector<AtlasSprite> sprites_;
    /// Name to index into sprites_. std::map, not unordered: it looks up by
    /// string_view without a transparent hash in a public header, and stitching
    /// happens once at load, never in a frame.
    std::map<std::string, u32, std::less<>> by_name_;
};

enum class AtlasError : u8 {
    /// The sprites do not fit within kMaxAtlasSize. Reachable only with a pack
    /// far beyond anything vanilla ships: the 928 block textures of `run/assets`
    /// at 32x stitch into 1024², at 91% occupancy.
    TooLarge,
};

[[nodiscard]] std::string_view to_string(AtlasError error) noexcept;

/// Collects sprite names, loads them, and stitches.
///
/// Deliberately not incremental: a resource pack reload rebuilds the atlas from
/// scratch, because the packing depends on the whole set and a sprite that
/// changed size moves everything after it.
class AtlasBuilder {
public:
    /// Vanilla's default, and the value its 16x16 sprites are built for.
    static constexpr u32 kDefaultMipLevel = 4;

    /// A hard stop, not a target. Above this a pack has stopped being a texture
    /// pack and started being a denial of service.
    static constexpr u32 kMaxAtlasSize = 16384;

    /// The generated checkerboard is drawn at this size and then scaled up with
    /// the rest, so it is as coarse in a 32x pack as vanilla's own is.
    static constexpr u32 kMissingSpriteSize = 16;

    explicit AtlasBuilder(const AssetSource& source) noexcept;

    /// Halvings below level 0. The build may use fewer — see
    /// TextureAtlas::mip_level(). Zero means no mips at all.
    AtlasBuilder& set_mip_level(u32 level) noexcept;

    /// Add a sprite by name, e.g. `minecraft:block/stone` or `block/stone`.
    /// Models name the same sprite thousands of times, so repeats are expected
    /// and free. A name that is not a valid resource location is kept, and
    /// resolves to the checkerboard.
    AtlasBuilder& add(std::string_view sprite_name);

    /// Distinct names added so far, not counting the checkerboard the build
    /// always adds.
    [[nodiscard]] usize requested_sprites() const noexcept { return names_.size(); }

    /// Stitch. An empty builder still yields an atlas: it holds the
    /// checkerboard, which is what every unresolved name maps to.
    [[nodiscard]] std::expected<TextureAtlas, AtlasError> build() const;

private:
    const AssetSource*       source_;
    std::vector<std::string> names_;
    u32                      mip_level_{kDefaultMipLevel};
};

}  // namespace ov::render
