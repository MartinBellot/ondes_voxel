#define OV_LOG_CATEGORY "render"

#include "ov/render/block_models.hpp"

#include "ov/base/log.hpp"
#include "ov/render/asset_path.hpp"
#include "ov/render/block_state_model.hpp"
#include "ov/render/model.hpp"

#include <algorithm>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ov::render {

namespace {

/// How much of a sprite is see-through, which is what decides its layer.
struct AlphaClass {
    bool any_cut{false};      ///< At least one fully transparent texel.
    bool any_partial{false};  ///< At least one texel that is neither 0 nor 255.
};

[[nodiscard]] AlphaClass classify(const TextureAtlas& atlas, const AtlasSprite& sprite) {
    AlphaClass  result;
    const auto& level = atlas.mip(0);
    for (u32 y = 0; y < sprite.height; ++y) {
        const usize row = (static_cast<usize>(sprite.y + y) * level.width + sprite.x) * 4;
        for (u32 x = 0; x < sprite.width; ++x) {
            const u8 alpha = level.rgba[row + static_cast<usize>(x) * 4 + 3];
            if (alpha == 0) {
                result.any_cut = true;
            } else if (alpha != 255) {
                result.any_partial = true;
            }
        }
    }
    return result;
}

/// The geometry vanilla does not keep in a model file.
///
/// `models/block/water.json` declares a particle texture and nothing else: the
/// game draws fluids with a renderer of its own, which reads the level of the
/// eight columns around each corner and tilts the surface towards the flow. A
/// box of the right height is the honest first approximation — it puts the
/// ocean back, which is most of what was missing — and the corner heights and
/// flow direction are recorded as not done rather than faked.
[[nodiscard]] BakedModel fluid_model(std::string_view sprite, u8 level) {
    // Vanilla's still surface sits at 14/16 of a block, which is why you stand
    // slightly below the waterline. A falling fluid — level 8 and up — fills
    // its cube.
    const f32 height = level >= 8 ? 16.0F : 14.0F;

    Element element;
    element.from = {0.0F, 0.0F, 0.0F};
    element.to   = {16.0F, height, 16.0F};
    for (u8 i = 0; i < kDirectionCount; ++i) {
        FaceDefinition face;
        face.sprite = std::string(sprite);
        // Every face carries a cullface, so a fluid against a solid neighbour
        // or against more of itself draws nothing.
        face.cullface = static_cast<Direction>(i);
        // Fluids take the biome tint on every face, not on a tintindex.
        face.tint_index  = 0;
        element.faces[i] = face;
    }

    Model model;
    // Flat lighting: a fluid surface has no corners to shade, and vanilla does
    // not give it smooth lighting either.
    model.ambient_occlusion = false;
    model.elements.push_back(element);
    return bake(model, ModelVariant{.model = ResourceLocation::parse("block/fluid").value()});
}

}  // namespace

namespace {

/// Which blocks the game nudges off their centre, and how. Nothing in the
/// assets or the data generator says it; every entry is printed by the real
/// client (the oracle's `offsets` directive walks every block and gives the
/// range of BlockState.getOffset; docs/provenance/rendu-parite.md).
struct OffsetEntry {
    std::string_view name;
    OffsetType       type;
    f32              max_horizontal;
};

constexpr OffsetEntry kOffsets[] = {
    {"minecraft:grass", OffsetType::XYZ, 0.25F},
};

}  // namespace

TintChannel tint_channel_for(std::string_view block_name) noexcept {
    // Vanilla decides this in Java, block by block, so there is no file to read
    // it from. The list is the 1.20.1 blocks whose models declare a tintindex,
    // grouped by which colormap they sample.
    constexpr std::string_view kGrass[]   = {"grass_block", "grass",           "tall_grass",
                                             "fern",        "large_fern",      "potted_fern",
                                             "sugar_cane",  "grass_block_snow"};
    constexpr std::string_view kFoliage[] = {"oak_leaves",      "jungle_leaves", "acacia_leaves",
                                             "dark_oak_leaves", "mangrove_leaves", "vine"};
    // These two ignore the biome and take a constant, which is why they are not
    // in the list above.
    constexpr std::string_view kEvergreen[] = {"spruce_leaves", "potted_spruce_sapling"};
    constexpr std::string_view kBirch[]     = {"birch_leaves"};
    constexpr std::string_view kWater[]   = {"water", "bubble_column", "water_cauldron"};

    const auto bare = block_name.starts_with("minecraft:") ? block_name.substr(10) : block_name;

    if (std::ranges::find(kGrass, bare) != std::ranges::end(kGrass)) {
        return TintChannel::Grass;
    }
    if (std::ranges::find(kFoliage, bare) != std::ranges::end(kFoliage)) {
        return TintChannel::Foliage;
    }
    if (std::ranges::find(kWater, bare) != std::ranges::end(kWater)) {
        return TintChannel::Water;
    }
    if (std::ranges::find(kEvergreen, bare) != std::ranges::end(kEvergreen)) {
        return TintChannel::EvergreenFoliage;
    }
    if (std::ranges::find(kBirch, bare) != std::ranges::end(kBirch)) {
        return TintChannel::BirchFoliage;
    }
    return TintChannel::None;
}

struct BlockModelCache::Impl {
    const AssetSource*             source{nullptr};
    const registry::BlockRegistry* blocks{nullptr};
    ModelLoader                    loader;

    std::unordered_map<u16, BlockRender>            states;
    std::unordered_map<std::string, BlockStateFile> files;
    /// Names in insertion order, deduplicated, for the atlas builder.
    std::vector<std::string>        sprite_names;
    std::unordered_set<std::string> sprite_seen;
    usize                           missing{0};

    explicit Impl(const AssetSource& asset_source) : source(&asset_source), loader(asset_source) {}

    [[nodiscard]] const BlockStateFile* blockstate_of(std::string_view block_name) {
        const auto key = std::string(block_name);
        if (const auto it = files.find(key); it != files.end()) {
            return &it->second;
        }

        const auto location = ResourceLocation::parse(block_name);
        if (!location) {
            return nullptr;
        }
        const auto bytes = source->read(blockstate_asset_path(*location));
        if (!bytes) {
            return nullptr;
        }
        auto parsed = BlockStateFile::parse(*bytes);
        if (!parsed) {
            return nullptr;
        }
        const auto [it, _] = files.emplace(key, std::move(*parsed));
        return &it->second;
    }

    void remember_sprites(const BakedModel& model) {
        for (const auto& quad : model.quads) {
            if (sprite_seen.insert(quad.sprite).second) {
                sprite_names.push_back(quad.sprite);
            }
        }
    }
};

BlockModelCache::BlockModelCache(const AssetSource& source, const registry::BlockRegistry& blocks)
    : impl_(std::make_unique<Impl>(source)) {
    impl_->blocks = &blocks;
}

BlockModelCache::BlockModelCache(BlockModelCache&&) noexcept            = default;
BlockModelCache& BlockModelCache::operator=(BlockModelCache&&) noexcept = default;
BlockModelCache::~BlockModelCache()                                     = default;

const std::vector<std::string>& BlockModelCache::sprites() const noexcept {
    return impl_->sprite_names;
}

usize BlockModelCache::resolved_count() const noexcept {
    return impl_->states.size();
}

usize BlockModelCache::missing_count() const noexcept {
    return impl_->missing;
}

const BlockRender& BlockModelCache::resolve(registry::BlockStateId state) {
    static const BlockRender kNothing{};

    const auto key = state.value();
    if (const auto it = impl_->states.find(key); it != impl_->states.end()) {
        return it->second;
    }

    BlockRender render;
    const auto& blocks = *impl_->blocks;
    if (!blocks.is_valid(state)) {
        return kNothing;
    }

    const auto block = blocks.block_of(state);
    const auto name  = blocks.block_name(block);
    render.tint      = tint_channel_for(name);
    for (const OffsetEntry& entry : kOffsets) {  // ── render-parity ──
        if (entry.name == name) {
            render.offset     = entry.type;
            render.max_offset = entry.max_horizontal;
        }
    }

    if (blocks.is_air(block)) {
        const auto [it, _] = impl_->states.emplace(key, std::move(render));
        return it->second;
    }

    // Fluids first: they have a blockstate and a model file, and the model file
    // is empty on purpose.
    if (name == "minecraft:water" || name == "minecraft:lava") {
        const bool is_water = name == "minecraft:water";
        u8         level    = 0;
        if (const auto property = blocks.find_property(block, "level")) {
            level = static_cast<u8>(blocks.property_index(state, *property));
        }
        render.model = fluid_model(
            is_water ? "minecraft:block/water_still" : "minecraft:block/lava_still", level);
        render.layer    = is_water ? RenderLayer::Translucent : RenderLayer::Solid;
        render.tint     = is_water ? TintChannel::Water : TintChannel::None;
        render.fluid    = block.value() + 1u;
        render.drawable = true;
        impl_->remember_sprites(render.model);
        const auto [it, _] = impl_->states.emplace(key, std::move(render));
        return it->second;
    }

    const BlockStateFile* file = impl_->blockstate_of(name);
    if (file == nullptr) {
        ++impl_->missing;
        const auto [it, _] = impl_->states.emplace(key, std::move(render));
        return it->second;
    }

    // The state's properties, read back out of the mixed-radix id. This is the
    // exact inverse of how the registry packed them, so a stair's `facing` and
    // `shape` come back the way the blockstate file spells them.
    std::vector<std::pair<std::string_view, std::string_view>> properties;
    for (const auto& property : blocks.properties(block)) {
        properties.emplace_back(property.name, blocks.property_value(state, property));
    }

    // Multipart yields one group per matching piece, and they all draw. A
    // fence is its post plus a side for each connection, and concatenating
    // them is what a fence *is*.
    const auto groups = file->select(properties);
    for (const auto& group : groups) {
        if (group.alternatives.empty()) {
            continue;
        }
        // ── render-parity ── a single group of weighted alternatives — the
        // shape of every randomised blockstate in the game — keeps them all;
        // the mesher picks one per position. (Multipart with weighted pieces
        // would need a pick per piece; none of the terrain has it, and it
        // still takes the first.)
        if (groups.size() == 1 && group.alternatives.size() > 1) {
            for (const auto& alternative : group.alternatives) {
                const auto loaded = impl_->loader.load(alternative.model);
                if (!loaded) {
                    render.alternatives.clear();
                    render.weights.clear();
                    break;
                }
                render.alternatives.push_back(bake(**loaded, alternative));
                render.weights.push_back(alternative.weight);
                impl_->remember_sprites(render.alternatives.back());
            }
        }
        // The first alternative is `model`, for everything that does not care
        // where the block is.
        const auto& variant = group.alternatives.front();
        const auto  model   = impl_->loader.load(variant.model);
        if (!model) {
            continue;
        }
        auto baked = bake(**model, variant);
        render.model.ambient_occlusion =
            render.model.quads.empty() ? baked.ambient_occlusion
                                       : render.model.ambient_occlusion && baked.ambient_occlusion;
        render.model.quads.insert(render.model.quads.end(),
                                  std::make_move_iterator(baked.quads.begin()),
                                  std::make_move_iterator(baked.quads.end()));
    }

    render.drawable = !render.model.quads.empty();
    if (!render.drawable) {
        ++impl_->missing;
    }
    impl_->remember_sprites(render.model);

    const auto [it, _] = impl_->states.emplace(key, std::move(render));
    return it->second;
}

void BlockModelCache::classify_layers(const TextureAtlas& atlas) {
    // One pass over the atlas first: a sprite is shared by hundreds of states,
    // and scanning its pixels once per state would scan the same rect
    // thousands of times.
    std::unordered_map<std::string, AlphaClass> by_sprite;
    for (const auto& sprite : atlas.sprites()) {
        by_sprite.emplace(sprite.name, classify(atlas, sprite));
    }

    for (auto& [state, render] : impl_->states) {
        (void)state;
        if (render.fluid != 0) {
            // A fluid's layer is decided by what it is, not by its texture:
            // water_still is fully opaque as a texture and would classify as
            // solid, which is exactly backwards.
            continue;
        }
        bool any_cut     = false;
        bool any_partial = false;
        for (const auto& quad : render.model.quads) {
            const auto it = by_sprite.find(quad.sprite);
            if (it == by_sprite.end()) {
                continue;
            }
            any_cut     = any_cut || it->second.any_cut;
            any_partial = any_partial || it->second.any_partial;
        }

        if (any_partial) {
            render.layer = RenderLayer::Translucent;
        } else if (any_cut) {
            // Everything hard-edged goes to the mipped cutout. Vanilla splits
            // this in two — glass and iron bars use `cutout` so that mipping
            // does not eat their one-pixel frames — and which block is in which
            // list is a rendering decision in Java, not a property of the
            // texture. Recorded as a known gap rather than guessed at.
            render.layer = RenderLayer::CutoutMipped;
        } else {
            render.layer = RenderLayer::Solid;
        }
    }
}

}  // namespace ov::render
