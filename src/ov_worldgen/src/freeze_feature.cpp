#define OV_LOG_CATEGORY "worldgen"

// ── worldgen-3 ── `freeze_top_layer`: the last feature of every chunk. For
// each of its 256 columns, the top of the motion-blocking heightmap:
//
//   * the block under it becomes ice when it is a water source in a biome cold
//     enough to snow there (`Biome.shouldFreeze`, not at an edge);
//   * the block over it becomes a snow layer when it is air, the biome is cold
//     enough at *that* height, and a snow layer would stand there — and the
//     block under the snow is then marked `snowy` if it has the property.
//
// "Cold enough" is the biome's temperature after its modifier (the frozen
// oceans' warm patches) and after the cooling above y = 80, below 0.15 —
// climate_noise.cpp. The order is the game's: the water freezes first, so no
// snow is ever laid on the ice made in the same column, snow not standing on
// ice.
//
// Named approximations: the block light a generating chunk has (none, so the
// "< 10" test always passes), and the biome read at the raw 4x4x4 cell rather
// than through the game's fuzzy zoom — which moves a biome's edge by up to two
// blocks. Measured in docs/provenance/features.md, « La couche gelée ».

#include "overworld_feature.hpp"

#include "climate_noise.hpp"

#include "ov/base/log.hpp"

namespace ov::worldgen {

namespace {

class FreezeTopLayerFeature final : public Feature {
public:
    struct Vocabulary {
        registry::BlockId      water{0};
        registry::BlockId      snow{0};
        registry::BlockStateId ice{};
        registry::BlockStateId snow_layer{};
        std::vector<u16>       snow_cannot_stand_on;
        std::vector<u16>       snow_can_stand_on;
    };

    explicit FreezeTopLayerFeature(Vocabulary words) : words_(std::move(words)) {}

    [[nodiscard]] std::string_view type_name() const override { return "freeze_top_layer"; }

    bool place(const FeatureContext& context, FeatureLevel& level, FeatureRandom&,
               BlockPos origin) const override {
        const auto& blocks = *context.blocks;
        const i32   min_x  = (origin.x >> 4) * 16;
        const i32   min_z  = (origin.z >> 4) * 16;
        for (i32 dx = 0; dx < 16; ++dx) {
            for (i32 dz = 0; dz < 16; ++dz) {
                const i32 x   = min_x + dx;
                const i32 z   = min_z + dz;
                const i32 top = level.height(world::HeightmapType::MotionBlocking, x, z);
                const i32 below_y = top - 1;
                const auto biome  = blocks.find_biome(context.biome_at(level, x, top, z));
                if (!biome) {
                    continue;
                }
                const auto& effects  = blocks.biome(*biome);
                const auto  modifier = effects.temperature_modifier == 1
                                           ? TemperatureModifier::Frozen
                                           : TemperatureModifier::None;
                const auto  base     = static_cast<f32>(effects.temperature);
                const auto  cold = [&](i32 y) {
                    return height_adjusted_temperature(base, modifier, x, y, z) < 0.15F;
                };

                if (cold(below_y) && inside(level, below_y) &&
                    is_water_source(blocks, level.block_at(x, below_y, z))) {
                    (void)level.set_block(x, below_y, z, words_.ice);
                }
                if (cold(top) && inside(level, top)) {
                    const auto here  = level.block_at(x, top, z);
                    const auto block = blocks.block_of(here);
                    if ((here == registry::kAirState || blocks.is_air(block) || block == words_.snow) &&
                        snow_stands_on(blocks, level.block_at(x, below_y, z))) {
                        (void)level.set_block(x, top, z, words_.snow_layer);
                        const auto under = level.block_at(x, below_y, z);
                        if (blocks.find_property(blocks.block_of(under), "snowy")) {
                            (void)level.set_block(
                                x, below_y, z, with_property_value(blocks, under, "snowy", "true"));
                        }
                    }
                }
            }
        }
        return true;
    }

private:
    [[nodiscard]] static bool inside(const FeatureLevel& level, i32 y) {
        return y >= level.min_y() && y < level.min_y() + level.world_height();
    }

    /// The fluid state is water, not flowing, and the block is the fluid
    /// itself rather than something holding it.
    [[nodiscard]] bool is_water_source(const registry::BlockRegistry& blocks,
                                       registry::BlockStateId          state) const {
        const auto block = blocks.block_of(state);
        if (block != words_.water) {
            return false;
        }
        const auto level = blocks.find_property(block, "level");
        return !level || blocks.property_value(state, *level) == "0";
    }

    /// `SnowLayerBlock.canSurvive` on the block below.
    [[nodiscard]] bool snow_stands_on(const registry::BlockRegistry& blocks,
                                      registry::BlockStateId          below) const {
        const auto block = blocks.block_of(below);
        if (holds(words_.snow_cannot_stand_on, block)) {
            return false;
        }
        if (holds(words_.snow_can_stand_on, block)) {
            return true;
        }
        if (block == words_.snow) {
            const auto layers = blocks.find_property(block, "layers");
            return layers && blocks.property_value(below, *layers) == "8";
        }
        // A collision box covering the whole top face.
        for (const auto& box : blocks.collision_boxes(below)) {
            if (box.max_y == 32 && box.min_x == 0 && box.max_x == 32 && box.min_z == 0 &&
                box.max_z == 32) {
                return true;
            }
        }
        return false;
    }

    Vocabulary words_;
};

}  // namespace

ClaimedFeature parse_freeze_feature(std::string_view kind, const registry::BlockRegistry& blocks,
                                    const BlockTags& tags) {
    if (kind != "freeze_top_layer") {
        return std::nullopt;
    }
    FreezeTopLayerFeature::Vocabulary words;
    auto water = named_block(blocks, "minecraft:water");
    auto snow  = named_block(blocks, "minecraft:snow");
    auto ice   = named_block(blocks, "minecraft:ice");
    auto cannot = tag_members(blocks, tags, "minecraft:snow_layer_cannot_survive_on");
    auto can    = tag_members(blocks, tags, "minecraft:snow_layer_can_survive_on");
    if (!water || !snow || !ice || !cannot || !can) {
        return std::unexpected(FeatureError::Missing);
    }
    words.water                = *water;
    words.snow                 = *snow;
    words.ice                  = blocks.default_state(*ice);
    words.snow_layer           = blocks.default_state(*snow);
    words.snow_cannot_stand_on = std::move(*cannot);
    words.snow_can_stand_on    = std::move(*can);
    return std::static_pointer_cast<const Feature>(
        std::make_shared<const FreezeTopLayerFeature>(std::move(words)));
}

}  // namespace ov::worldgen
