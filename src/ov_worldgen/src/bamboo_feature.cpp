#define OV_LOG_CATEGORY "worldgen"

// `bamboo`: a stalk of five to sixteen, sometimes on a disc of podzol.
//
// Draws, in order: the height, then the podzol coin, then — only when the
// coin comes up — the podzol radius. Everything is conditional on the spot
// being empty and on bamboo surviving there, and the stalk stops at the first
// block that is not empty.

#include "overworld_feature.hpp"

#include "ov/base/log.hpp"

namespace ov::worldgen {

namespace {

class BambooFeature final : public Feature {
public:
    BambooFeature(f32 probability, std::vector<u16> plantable, std::vector<u16> dirt,
                  registry::BlockId podzol, registry::BlockStateId trunk,
                  registry::BlockStateId final_large, registry::BlockStateId top_large,
                  registry::BlockStateId top_small)
        : probability_(probability),
          plantable_(std::move(plantable)),
          dirt_(std::move(dirt)),
          podzol_(podzol),
          trunk_(trunk),
          final_large_(final_large),
          top_large_(top_large),
          top_small_(top_small) {}

    [[nodiscard]] std::string_view type_name() const override { return "bamboo"; }

    bool place(const FeatureContext& context, FeatureLevel& level, FeatureRandom& random,
               BlockPos origin) const override {
        const auto& blocks = *context.blocks;
        const auto  empty  = [&](BlockPos p) {
            return blocks.is_air(blocks.block_of(level.block_at(p.x, p.y, p.z)));
        };
        if (!empty(origin)) {
            return false;
        }
        const auto below = blocks.block_of(level.block_at(origin.x, origin.y - 1, origin.z));
        if (holds(plantable_, below)) {
            const i32 height = random.next_int(12) + 5;
            const f32 coin   = random.next_float();
            if (coin < probability_) {
                const i32 radius = random.next_int(4) + 1;
                for (i32 x = origin.x - radius; x <= origin.x + radius; ++x) {
                    for (i32 z = origin.z - radius; z <= origin.z + radius; ++z) {
                        const i32 dx = x - origin.x;
                        const i32 dz = z - origin.z;
                        if (dx * dx + dz * dz > radius * radius) {
                            continue;
                        }
                        const i32 y = level.height(world::HeightmapType::WorldSurface, x, z) - 1;
                        if (!holds(dirt_, blocks.block_of(level.block_at(x, y, z)))) {
                            continue;
                        }
                        (void)level.set_block(x, y, z, blocks.default_state(podzol_));
                    }
                }
            }
            BlockPos cursor = origin;
            for (i32 k = 0; k < height && empty(cursor); ++k) {
                (void)level.set_block(cursor.x, cursor.y, cursor.z, trunk_);
                cursor = cursor.above();
            }
            if (cursor.y - origin.y >= 3) {
                (void)level.set_block(cursor.x, cursor.y, cursor.z, final_large_);
                (void)level.set_block(cursor.x, cursor.y - 1, cursor.z, top_large_);
                (void)level.set_block(cursor.x, cursor.y - 2, cursor.z, top_small_);
            }
        }
        // The game counts the attempt whenever the spot was empty.
        return true;
    }

private:
    f32                    probability_;
    std::vector<u16>       plantable_;
    std::vector<u16>       dirt_;
    registry::BlockId      podzol_;
    registry::BlockStateId trunk_;
    registry::BlockStateId final_large_;
    registry::BlockStateId top_large_;
    registry::BlockStateId top_small_;
};

}  // namespace

ClaimedFeature parse_bamboo_feature(std::string_view kind, Json config,
                                    const registry::BlockRegistry& blocks, const BlockTags& tags) {
    if (kind != "bamboo") {
        return std::nullopt;
    }
    f64 probability = 0.0;
    (void)config.at_key("probability").get(probability);
    auto plantable = tag_members(blocks, tags, "minecraft:bamboo_plantable_on");
    auto dirt      = tag_members(blocks, tags, "minecraft:dirt");
    auto podzol    = named_block(blocks, "minecraft:podzol");
    auto bamboo    = named_block(blocks, "minecraft:bamboo");
    if (!plantable || !dirt || !podzol || !bamboo) {
        return std::unexpected(FeatureError::Missing);
    }
    const auto with = [&](std::string_view leaves, std::string_view stage) {
        auto state = blocks.default_state(*bamboo);
        state      = with_property_value(blocks, state, "age", "1");
        state      = with_property_value(blocks, state, "leaves", leaves);
        state      = with_property_value(blocks, state, "stage", stage);
        return state;
    };
    return std::static_pointer_cast<const Feature>(std::make_shared<const BambooFeature>(
        static_cast<f32>(probability), std::move(*plantable), std::move(*dirt), *podzol,
        with("none", "0"), with("large", "1"), with("large", "0"), with("small", "0")));
}

}  // namespace ov::worldgen
