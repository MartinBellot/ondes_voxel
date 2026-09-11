#include "ov/render/texture_animation.hpp"

#include <algorithm>

namespace ov::render {

AnimationPhase animation_phase(const SpriteAnimation& animation, u64 tick) noexcept {
    AnimationPhase phase;
    if (animation.frames.empty()) {
        return phase;
    }
    u64 cycle = 0;
    for (const AnimationFrame& frame : animation.frames) {
        cycle += std::max(1U, frame.time);
    }
    u64 left = tick % cycle;
    const auto count = static_cast<u32>(animation.frames.size());
    for (u32 i = 0; i < count; ++i) {
        const u32 time = std::max(1U, animation.frames[i].time);
        if (left < time) {
            phase.frame      = i;
            phase.next       = (i + 1) % count;
            phase.sub_tick   = static_cast<u32>(left);
            phase.frame_time = time;
            return phase;
        }
        left -= time;
    }
    return phase;
}

TextureAnimator::TextureAnimator(const TextureAtlas& atlas)
    : atlas_(&atlas), levels_(std::max<usize>(1, atlas.mips().size())) {
    last_key_.assign(atlas.animations().size(), -1);
    for (const AtlasAnimation& animation : atlas.animations()) {
        u32 width  = animation.width;
        u32 height = animation.height;
        for (usize level = 0; level < levels_ && width > 0 && height > 0; ++level) {
            max_bytes_ += static_cast<usize>(width) * height * 4;
            width /= 2;
            height /= 2;
        }
    }
    patches_.reserve(atlas.animations().size() * levels_);
    bytes_.reserve(max_bytes_);
}

void TextureAnimator::push(u32 mip, u32 x, u32 y, const AtlasMip& level) {
    patches_.push_back(AtlasPatch{mip, x, y, level.width, level.height, bytes_.size()});
    bytes_.insert(bytes_.end(), level.rgba.begin(), level.rgba.end());
}

usize TextureAnimator::tick(u64 tick) {
    patches_.clear();
    bytes_.clear();
    usize changed = 0;

    const auto& animations = atlas_->animations();
    for (usize i = 0; i < animations.size(); ++i) {
        const AtlasAnimation& animation = animations[i];
        const auto&           frames    = animation.animation.frames;
        if (frames.empty() || animation.cells.empty()) {
            continue;
        }
        const AnimationPhase phase = animation_phase(animation.animation, tick);
        const bool blend = animation.animation.interpolate && phase.sub_tick > 0;
        const i64  key   = (static_cast<i64>(phase.frame) << 32) | (blend ? phase.sub_tick : 0);
        if (key == last_key_[i]) {
            continue;
        }
        last_key_[i] = key;

        const u32 from_cell = std::min<u32>(frames[phase.frame].index,
                                            static_cast<u32>(animation.cells.size() - 1));
        const std::vector<u8>& from = animation.cells[from_cell];
        level_.width  = animation.width;
        level_.height = animation.height;
        if (blend) {
            // Linear in the stored values, colour only: alpha is the current
            // frame's. The share of the next frame grows by 1/frametime a
            // tick. Unverified against the game beyond the eye — named in
            // docs/provenance/rendu-parite.md.
            const u32 to_cell = std::min<u32>(frames[phase.next].index,
                                              static_cast<u32>(animation.cells.size() - 1));
            const std::vector<u8>& to = animation.cells[to_cell];
            const f64 share = static_cast<f64>(phase.sub_tick) / static_cast<f64>(phase.frame_time);
            level_.rgba.resize(from.size());
            for (usize k = 0; k < from.size(); ++k) {
                if (k % 4 == 3) {
                    level_.rgba[k] = from[k];
                    continue;
                }
                level_.rgba[k] = static_cast<u8>(static_cast<f64>(from[k]) * (1.0 - share) +
                                                 static_cast<f64>(to[k]) * share);
            }
        } else {
            level_.rgba = from;
        }

        ++changed;
        push(0, animation.x, animation.y, level_);
        for (u32 mip = 1; mip < levels_; ++mip) {
            if (level_.width < 2 || level_.height < 2) {
                break;
            }
            level_ = halve(level_);
            push(mip, animation.x >> mip, animation.y >> mip, level_);
        }
    }
    return changed;
}

}  // namespace ov::render
