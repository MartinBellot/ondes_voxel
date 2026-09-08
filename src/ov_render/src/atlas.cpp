#include "ov/render/atlas.hpp"

#include "json.hpp"
#include "png.hpp"

#include "ov/base/resource_location.hpp"
#include "ov/render/asset_path.hpp"
#include "ov/render/model.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <span>
#include <utility>

namespace ov::render {

namespace {

/// A sprite once its file has been read: frame 0 at its source resolution, and
/// the size it will take in the atlas after scaling.
struct LoadedSprite {
    std::string                    name;
    png::Image                     frame;
    u32                            width{0};
    u32                            height{0};
    std::optional<SpriteAnimation> animation;
    /// The texture did not load. Such a sprite is never packed: it borrows the
    /// checkerboard's rect.
    bool missing{false};
};

constexpr std::array<u8, 4> kMissingBlack{0, 0, 0, 255};
constexpr std::array<u8, 4> kMissingMagenta{248, 0, 248, 255};

/// Vanilla's missing texture: 2x2 quadrants of black and magenta, generated
/// rather than read, because the one sprite that reports a broken pack must not
/// itself be breakable by a pack. The magenta is 0xF800F8 — one step off pure,
/// which is the value vanilla ships.
png::Image make_missing_image() {
    const u32 size = AtlasBuilder::kMissingSpriteSize;
    const u32 half = size / 2;

    png::Image image;
    image.width  = size;
    image.height = size;
    image.rgba.resize(static_cast<usize>(size) * size * 4);

    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            const bool  magenta = (x < half) != (y < half);
            const auto& colour  = magenta ? kMissingMagenta : kMissingBlack;
            const usize offset  = image.index(x, y);
            for (usize channel = 0; channel < 4; ++channel) {
                image.rgba[offset + channel] = colour[channel];
            }
        }
    }
    return image;
}

png::Image crop(const png::Image& source, u32 x0, u32 y0, u32 width, u32 height) {
    png::Image out;
    out.width  = width;
    out.height = height;
    out.rgba.resize(static_cast<usize>(width) * height * 4);
    for (u32 y = 0; y < height; ++y) {
        std::memcpy(out.rgba.data() + out.index(0, y),
                    source.rgba.data() + source.index(x0, y0 + y), static_cast<usize>(width) * 4);
    }
    return out;
}

/// A JSON number read as a small count, or the fallback. Written so that a
/// negative, fractional, absurd or NaN value out of a hostile pack takes the
/// fallback instead of wrapping around a cast.
u32 count_field(const json::Value& value, u32 fallback) {
    if (!value.is_number()) {
        return fallback;
    }
    const f64 number = value.as_number(static_cast<f64>(fallback));
    if (!(number >= 0.0) || number > 65535.0) {
        return fallback;
    }
    return static_cast<u32>(number);
}

/// Parse the `animation` object of a `<texture>.png.mcmeta`.
///
/// Frames are cells of a grid read in reading order — in practice a single
/// column, because that is how every vanilla animation is drawn, but the format
/// allows more. A frame size the metadata omits defaults to a square whose side
/// is the texture's smaller dimension, which is what makes a 16x448 strip parse
/// as 28 frames of 16x16 with no `width` or `height` stated at all.
std::optional<SpriteAnimation> parse_animation(const json::Value& root, u32 image_width,
                                               u32 image_height) {
    const auto animation_node = root["animation"];
    if (!animation_node.is_object()) {
        return std::nullopt;
    }

    const auto width_node  = animation_node["width"];
    const auto height_node = animation_node["height"];

    u32 frame_width  = 0;
    u32 frame_height = 0;
    if (width_node.is_number() && height_node.is_number()) {
        frame_width  = count_field(width_node, 0);
        frame_height = count_field(height_node, 0);
    } else {
        const u32 side = std::min(image_width, image_height);
        frame_width    = side;
        frame_height   = side;
    }

    if (frame_width == 0 || frame_height == 0 || frame_width > image_width ||
        frame_height > image_height) {
        return std::nullopt;
    }

    const u32 columns    = image_width / frame_width;
    const u32 rows       = image_height / frame_height;
    const u32 cell_count = columns * rows;
    if (cell_count == 0) {
        return std::nullopt;
    }

    SpriteAnimation animation;
    animation.frame_width        = frame_width;
    animation.frame_height       = frame_height;
    animation.default_frame_time = std::max(1U, count_field(animation_node["frametime"], 1));
    animation.interpolate        = animation_node["interpolate"].as_bool(false);

    if (const auto frames_node = animation_node["frames"]; frames_node.is_array()) {
        for (u32 i = 0; i < frames_node.size(); ++i) {
            const auto entry = frames_node[i];

            u32 index = cell_count;
            u32 time  = animation.default_frame_time;
            if (entry.is_number()) {
                index = count_field(entry, cell_count);
            } else if (entry.is_object()) {
                index = count_field(entry["index"], cell_count);
                time  = std::max(1U, count_field(entry["time"], animation.default_frame_time));
            } else {
                continue;
            }

            // A frame naming a cell the texture does not have is dropped, not
            // fatal: the pack is wrong, the game still runs.
            if (index >= cell_count) {
                continue;
            }
            animation.frames.push_back(AnimationFrame{index, time});
        }
    }

    if (animation.frames.empty()) {
        for (u32 i = 0; i < cell_count; ++i) {
            animation.frames.push_back(AnimationFrame{i, animation.default_frame_time});
        }
    }

    return animation;
}

/// Read one sprite's texture and metadata. Failure at any step is not an error:
/// it is the checkerboard, and `missing` records it.
LoadedSprite load_sprite(const AssetSource& source, const std::string& name) {
    LoadedSprite sprite;
    sprite.name    = name;
    sprite.missing = true;

    const auto location = ResourceLocation::parse(name);
    if (!location) {
        return sprite;
    }

    const std::string path  = texture_asset_path(*location);
    const auto        bytes = source.read(path);
    if (!bytes) {
        return sprite;
    }

    auto image = png::decode_rgba8(*bytes);
    if (!image || image->empty()) {
        return sprite;
    }

    std::optional<SpriteAnimation> animation;
    if (const auto meta = source.read(path + ".mcmeta")) {
        if (const auto document = json::Document::parse(*meta)) {
            animation = parse_animation(document->root(), image->width, image->height);
        }
    }

    if (animation) {
        // Frame 0 of the *sequence*, not cell 0 of the strip: a pack may start
        // its animation anywhere, and the still frame is the one it starts on.
        const u32 columns = image->width / animation->frame_width;
        const u32 index   = animation->frames.front().index;
        sprite.frame = crop(*image, (index % columns) * animation->frame_width,
                            (index / columns) * animation->frame_height, animation->frame_width,
                            animation->frame_height);
    } else {
        sprite.frame = std::move(*image);
    }

    sprite.animation = std::move(animation);
    sprite.missing   = false;
    return sprite;
}

/// Nearest-neighbour blit at an integer scale — pixel replication, so the atlas
/// holds no colour that was not in the source file.
void blit(AtlasMip& target, const png::Image& source, u32 x0, u32 y0, u32 scale) {
    for (u32 sy = 0; sy < source.height; ++sy) {
        for (u32 sx = 0; sx < source.width; ++sx) {
            const usize from = source.index(sx, sy);
            for (u32 ry = 0; ry < scale; ++ry) {
                const usize row =
                    (static_cast<usize>(y0 + sy * scale + ry) * target.width + x0 + sx * scale) * 4;
                for (usize rx = 0; rx < scale; ++rx) {
                    std::memcpy(target.rgba.data() + row + rx * 4, source.rgba.data() + from, 4);
                }
            }
        }
    }
}

/// One box-filtered halving.
///
/// The averaging is **alpha-weighted**, and that is the whole point of this
/// function. A cutout sprite — leaves, grass, glass, iron bars — stores black
/// RGB under its fully transparent texels, because at mip 0 nothing ever
/// samples them. Average the four naively and that black is three quarters of
/// the answer at the first halving: foliage grows a dark halo that thickens
/// with distance, and nothing about mip 0 hints at why.
///
/// So colour is averaged in proportion to coverage, and alpha is averaged flat.
/// A 2x2 with one opaque green texel comes out green at quarter alpha, not
/// quarter-brightness green.
///
/// Not settled here: whether vanilla additionally blends in linear light rather
/// than in sRGB. That difference is a slight darkening across a gradient; the
/// halo is the one that is actually visible, and alpha weighting is what fixes
/// it.
AtlasMip downsample(const AtlasMip& source) {
    AtlasMip target;
    target.width  = source.width / 2;
    target.height = source.height / 2;
    target.rgba.resize(static_cast<usize>(target.width) * target.height * 4);

    for (u32 y = 0; y < target.height; ++y) {
        for (u32 x = 0; x < target.width; ++x) {
            const usize top  = static_cast<usize>(y) * 2 * source.width + static_cast<usize>(x) * 2;
            const usize next = top + source.width;
            const std::array<usize, 4> corners{top * 4, (top + 1) * 4, next * 4, (next + 1) * 4};

            u32 alpha_sum = 0;
            for (const usize corner : corners) {
                alpha_sum += source.rgba[corner + 3];
            }

            const usize out = (static_cast<usize>(y) * target.width + x) * 4;
            if (alpha_sum == 0) {
                // Nothing here is visible, so there is no colour to preserve
                // and any value would do as well as another.
                std::memset(target.rgba.data() + out, 0, 4);
                continue;
            }

            for (usize channel = 0; channel < 3; ++channel) {
                u32 weighted = 0;
                for (const usize corner : corners) {
                    weighted +=
                        static_cast<u32>(source.rgba[corner + channel]) * source.rgba[corner + 3];
                }
                target.rgba[out + channel] =
                    static_cast<u8>((weighted + alpha_sum / 2) / alpha_sum);
            }
            target.rgba[out + 3] = static_cast<u8>((alpha_sum + 2) / 4);
        }
    }
    return target;
}

u32 round_up_pow2(u32 value) noexcept {
    if (value <= 1) {
        return 1;
    }
    if (value > (1U << 30)) {
        return 1U << 30;
    }
    return 1U << (32U - static_cast<u32>(std::countl_zero(value - 1U)));
}

/// Shelf packing, tallest sprite first, into a square of `size`.
///
/// Every width and every shelf height is a multiple of the alignment (see
/// build()), so every position is one too, with no rounding step that could get
/// it wrong. A skyline or quadtree packer would win a few per cent of area on a
/// pathological set and cost exactly that property; on a real pack, where the
/// sprites are all the same power-of-two square, this one wastes nothing.
bool place(std::span<LoadedSprite* const> sprites, u32 size,
           std::vector<std::pair<u32, u32>>& positions) {
    positions.clear();
    positions.reserve(sprites.size());

    u32 cursor_x     = 0;
    u32 cursor_y     = 0;
    u32 shelf_height = 0;

    for (const LoadedSprite* sprite : sprites) {
        if (sprite->width > size) {
            return false;
        }
        if (cursor_x + sprite->width > size) {
            cursor_y += shelf_height;
            cursor_x     = 0;
            shelf_height = 0;
        }
        if (cursor_y + sprite->height > size) {
            return false;
        }
        positions.emplace_back(cursor_x, cursor_y);
        cursor_x += sprite->width;
        shelf_height = std::max(shelf_height, sprite->height);
    }
    return true;
}

}  // namespace

std::string_view to_string(AtlasError error) noexcept {
    switch (error) {
        case AtlasError::TooLarge: return "atlas larger than the limit";
    }
    return "unknown atlas error";
}

const AtlasMip& TextureAtlas::mip(u32 level) const noexcept {
    // A default-constructed atlas has no levels at all. Handing back an empty
    // one beats indexing past the end of an empty vector, which is what the
    // obvious clamp does.
    static constexpr AtlasMip kEmpty{};
    if (mips_.empty()) {
        return kEmpty;
    }
    return mips_[std::min(static_cast<usize>(level), mips_.size() - 1)];
}

const AtlasSprite* TextureAtlas::find(std::string_view name) const noexcept {
    const auto it = by_name_.find(name);
    return it == by_name_.end() ? nullptr : &sprites_[it->second];
}

SpriteUv TextureAtlas::uv(std::string_view name) const noexcept {
    if (const AtlasSprite* sprite = find(name)) {
        return sprite->uv;
    }
    if (const AtlasSprite* fallback = find(kMissingSprite)) {
        return fallback->uv;
    }
    return SpriteUv{};
}

AtlasBuilder::AtlasBuilder(const AssetSource& source) noexcept : source_(&source) {}

AtlasBuilder& AtlasBuilder::set_mip_level(u32 level) noexcept {
    mip_level_ = level;
    return *this;
}

AtlasBuilder& AtlasBuilder::add(std::string_view sprite_name) {
    // Canonical from here on: `block/stone` and `minecraft:block/stone` are one
    // sprite, and stitching both would waste atlas area for nothing. A name
    // that does not parse is kept verbatim, so a caller can still look up what
    // it asked for and get the checkerboard back.
    const auto  location = ResourceLocation::parse(sprite_name);
    std::string name     = location ? location->full() : std::string(sprite_name);

    const auto at = std::lower_bound(names_.begin(), names_.end(), name);
    if (at == names_.end() || *at != name) {
        names_.insert(at, std::move(name));
    }
    return *this;
}

std::expected<TextureAtlas, AtlasError> AtlasBuilder::build() const {
    std::vector<LoadedSprite> loaded;
    loaded.reserve(names_.size() + 1);
    for (const std::string& name : names_) {
        loaded.push_back(load_sprite(*source_, name));
    }

    // The checkerboard becomes a sprite like any other, so that it is scaled,
    // packed and mipped by the same code as the rest. It is generated even if a
    // pack ships a `missingno.png` of its own.
    usize checkerboard = loaded.size();
    for (usize i = 0; i < loaded.size(); ++i) {
        if (loaded[i].name == kMissingSprite) {
            checkerboard = i;
            break;
        }
    }
    if (checkerboard == loaded.size()) {
        loaded.push_back(LoadedSprite{});
    }
    loaded[checkerboard].name      = std::string(kMissingSprite);
    loaded[checkerboard].frame     = make_missing_image();
    loaded[checkerboard].animation = std::nullopt;
    loaded[checkerboard].missing   = false;

    // Pack resolution: the frame width the pack is mostly drawn at.
    //
    // A resource pack is not one resolution. `run/assets` stacks Faithful 32x
    // over the vanilla jar, and the 928 block textures there come out as 923 at
    // 32 wide, three the pack never redrew still at 16 — lightning_rod_on,
    // redstone_dust_overlay, water_overlay — and two at 64: lava_flow and
    // water_flow. Left alone, the 16s would sit at half their neighbours'
    // texel density, which is a mip alignment problem and a visible one: two
    // blocks side by side, one crisp and one soft.
    //
    // So sprites are scaled up. Width and not height, because an animated
    // texture is a tall strip whose height says nothing about resolution. Up
    // and never down, because scaling down throws away the detail the player
    // installed the pack for. By an integer factor, floored, because a filter
    // would invent colours that are in no source file — for pixel art, worse
    // than being one step coarse.
    //
    // And up to the *commonest* width rather than the largest, which is the
    // part that had to be measured. lava_flow and water_flow are 64 wide
    // because a flow texture is two blocks across, not because the pack is a
    // 64x pack: taking the largest reads those two files as the pack's
    // resolution, doubles all 928 sprites, and turns a 1024² atlas into a
    // 2048² one — four times the memory for nothing. A tie goes to the wider,
    // so that a pack split evenly between two resolutions still sharpens.
    std::map<u32, u32> votes;
    for (const LoadedSprite& sprite : loaded) {
        // The checkerboard is drawn at a fixed 16 and would otherwise get a
        // vote in every atlas, which decides small ones on its own.
        if (!sprite.missing && sprite.name != kMissingSprite) {
            ++votes[sprite.frame.width];
        }
    }

    u32 resolution = AtlasBuilder::kMissingSpriteSize;
    u32 best_votes = 0;
    for (const auto& [width, count] : votes) {
        if (count >= best_votes) {
            resolution = width;
            best_votes = count;
        }
    }

    std::vector<LoadedSprite*> packed;
    packed.reserve(loaded.size());
    for (LoadedSprite& sprite : loaded) {
        if (sprite.missing) {
            continue;
        }
        const u32 scale = std::max(1U, resolution / sprite.frame.width);
        sprite.width    = sprite.frame.width * scale;
        sprite.height   = sprite.frame.height * scale;
        packed.push_back(&sprite);
    }

    // Mip level, capped by alignment.
    //
    // At level N one texel covers 2^N texels of level 0, so a sprite whose size
    // is not a multiple of 2^N shares a texel with its neighbour at that level,
    // and the neighbour bleeds across the seam. The cap is the largest N every
    // sprite survives whole. Positions then align for free: sizes are multiples
    // of 2^N, shelves are stacks of those sizes, so every x and y is one too.
    //
    // The alternative, padding each sprite out to the alignment, is worse on
    // both counts: the padding must be filled with something, that something is
    // averaged into the sprite at every level anyway, and it costs area. This
    // costs sharpness at distance, on the one atlas that contains the offending
    // sprite — and costs nothing at all when every sprite is a power of two,
    // which is why 16x16 sprites and a mip level of 4 fit exactly: 16 >> 4 = 1.
    u32 mip_level = mip_level_;
    for (const LoadedSprite* sprite : packed) {
        mip_level = std::min({mip_level, static_cast<u32>(std::countr_zero(sprite->width)),
                              static_cast<u32>(std::countr_zero(sprite->height))});
    }

    std::sort(packed.begin(), packed.end(), [](const LoadedSprite* a, const LoadedSprite* b) {
        if (a->height != b->height) {
            return a->height > b->height;
        }
        if (a->width != b->width) {
            return a->width > b->width;
        }
        // Names are unique, so the order is total and the stitch reproducible.
        return a->name < b->name;
    });

    usize area    = 0;
    u32   longest = 1;
    for (const LoadedSprite* sprite : packed) {
        area += static_cast<usize>(sprite->width) * sprite->height;
        longest = std::max({longest, sprite->width, sprite->height});
    }

    // Start from the smallest square that could hold the total area at all,
    // rather than walking up from 16 and re-packing at every step.
    const auto                       ideal = static_cast<u32>(std::sqrt(static_cast<f64>(area)));
    u32                              size  = std::max(round_up_pow2(longest), round_up_pow2(ideal));
    std::vector<std::pair<u32, u32>> positions;
    while (!place(packed, size, positions)) {
        if (size >= kMaxAtlasSize) {
            return std::unexpected(AtlasError::TooLarge);
        }
        size *= 2;
    }

    AtlasMip level0;
    level0.width  = size;
    level0.height = size;
    // Zero is transparent black, which is what the gaps between shelves must be
    // — an opaque gap would be sampled at the higher mip levels.
    level0.rgba.assign(static_cast<usize>(size) * size * 4, 0);

    TextureAtlas atlas;
    atlas.sprites_.reserve(loaded.size());

    const f32 inverse = 1.0F / static_cast<f32>(size);
    SpriteUv  missing_uv{};
    u32       missing_x      = 0;
    u32       missing_y      = 0;
    u32       missing_width  = 0;
    u32       missing_height = 0;

    for (usize i = 0; i < packed.size(); ++i) {
        LoadedSprite& source = *packed[i];
        const auto [x, y]    = positions[i];

        blit(level0, source.frame, x, y, source.width / source.frame.width);

        AtlasSprite sprite;
        sprite.name      = source.name;
        sprite.x         = x;
        sprite.y         = y;
        sprite.width     = source.width;
        sprite.height    = source.height;
        sprite.uv        = SpriteUv{static_cast<f32>(x) * inverse, static_cast<f32>(y) * inverse,
                                    static_cast<f32>(x + source.width) * inverse,
                                    static_cast<f32>(y + source.height) * inverse};
        sprite.animation = std::move(source.animation);

        if (source.name == kMissingSprite) {
            missing_uv     = sprite.uv;
            missing_x      = x;
            missing_y      = y;
            missing_width  = source.width;
            missing_height = source.height;
        }
        atlas.sprites_.push_back(std::move(sprite));
    }

    // Sprites that failed to load point at the checkerboard's rect rather than
    // getting a copy of it. A pack missing three hundred textures should cost
    // three hundred table entries, not three hundred copies of 16 texels.
    for (const LoadedSprite& source : loaded) {
        if (!source.missing) {
            continue;
        }
        AtlasSprite sprite;
        sprite.name    = source.name;
        sprite.x       = missing_x;
        sprite.y       = missing_y;
        sprite.width   = missing_width;
        sprite.height  = missing_height;
        sprite.uv      = missing_uv;
        sprite.missing = true;
        atlas.sprites_.push_back(std::move(sprite));
    }

    std::sort(atlas.sprites_.begin(), atlas.sprites_.end(),
              [](const AtlasSprite& a, const AtlasSprite& b) { return a.name < b.name; });
    for (u32 i = 0; i < static_cast<u32>(atlas.sprites_.size()); ++i) {
        atlas.by_name_.emplace(atlas.sprites_[i].name, i);
    }

    atlas.mips_.push_back(std::move(level0));
    for (u32 level = 0; level < mip_level; ++level) {
        const AtlasMip& previous = atlas.mips_.back();
        if (previous.width < 2 || previous.height < 2) {
            break;
        }
        atlas.mips_.push_back(downsample(previous));
    }

    return atlas;
}

}  // namespace ov::render
