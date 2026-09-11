#define OV_LOG_CATEGORY "worldgen"

// `huge_brown_mushroom` and `huge_red_mushroom`.
//
// They matter far beyond their own few blocks: `dark_forest_vegetation` names
// both, and a selector that cannot build one of its candidates does not load at
// all — which is why every dark forest in the world had no trees. Measured
// against a probe world of nothing but these; see
// docs/provenance/features.md, "Les features de l'Overworld".
//
// The shape, as the game draws it:
//
//   * a height of `nextInt(3) + 4`, doubled when `nextInt(12)` is zero —
//     always two draws;
//   * a column check under the cap: the ground must be `#dirt` or
//     `#mushroom_grow_block`, and every cell up to the height must be air or
//     leaves over the width the cap claims at that level;
//   * the cap, then the stem, each written only where the world is not a
//     solid cube already.

#include "overworld_feature.hpp"

#include "ov/base/log.hpp"

namespace ov::worldgen {

namespace {

class HugeMushroomFeature final : public Feature {
public:
    enum class Kind : u8 { Brown, Red };

    HugeMushroomFeature(Kind kind, StateProviderRef cap, StateProviderRef stem, i32 radius,
                        std::vector<u16> dirt, std::vector<u16> grow_block,
                        std::vector<u16> leaves)
        : kind_(kind),
          cap_(std::move(cap)),
          stem_(std::move(stem)),
          radius_(radius),
          dirt_(std::move(dirt)),
          grow_block_(std::move(grow_block)),
          leaves_(std::move(leaves)) {}

    [[nodiscard]] std::string_view type_name() const override {
        return kind_ == Kind::Brown ? "huge_brown_mushroom" : "huge_red_mushroom";
    }

    bool place(const FeatureContext& context, FeatureLevel& level, FeatureRandom& random,
               BlockPos at) const override {
        const auto& blocks = *context.blocks;
        // Two draws, always, in this order.
        const i32 base    = random.next_int(3) + 4;
        const i32 doubled = random.next_int(12);
        const i32 height  = doubled == 0 ? base * 2 : base;

        if (!valid_position(blocks, level, at, height)) {
            return false;
        }
        make_cap(blocks, level, random, at, height);
        place_stem(blocks, level, random, at, height);
        return true;
    }

private:
    /// How wide the cap is at one level above the base, for the clearance
    /// check. The game asks it with the tree height given as -1, so for the red
    /// mushroom — whose answer depends on the height — this is zero at every
    /// level and only the stem's own column is checked.
    [[nodiscard]] i32 radius_for_check(i32 y) const {
        if (kind_ == Kind::Brown) {
            return y <= 3 ? 0 : radius_;
        }
        constexpr i32 kHeight = -1;
        if (y < kHeight && y >= kHeight - 3) {
            return radius_;
        }
        return y == kHeight ? radius_ : 0;
    }

    [[nodiscard]] bool valid_position(const registry::BlockRegistry& blocks,
                                      const FeatureLevel& level, BlockPos at, i32 height) const {
        if (at.y < level.min_y() + 1 || at.y + height + 1 >= level.min_y() + level.world_height()) {
            return false;
        }
        const auto below = blocks.block_of(level.block_at(at.x, at.y - 1, at.z));
        if (!holds(dirt_, below) && !holds(grow_block_, below)) {
            return false;
        }
        for (i32 y = 0; y <= height; ++y) {
            const i32 reach = radius_for_check(y);
            for (i32 dx = -reach; dx <= reach; ++dx) {
                for (i32 dz = -reach; dz <= reach; ++dz) {
                    const auto block =
                        blocks.block_of(level.block_at(at.x + dx, at.y + y, at.z + dz));
                    if (!blocks.is_air(block) && !holds(leaves_, block)) {
                        return false;
                    }
                }
            }
        }
        return true;
    }

    void place_stem(const registry::BlockRegistry& blocks, FeatureLevel& level,
                    FeatureRandom& random, BlockPos at, i32 height) const {
        for (i32 y = 0; y < height; ++y) {
            if (is_solid_render(blocks, level.block_at(at.x, at.y + y, at.z))) {
                continue;
            }
            // The provider is asked at the feature's origin, not at the block.
            (void)level.set_block(at.x, at.y + y, at.z, stem_->state(level, random, at));
        }
    }

    void make_cap(const registry::BlockRegistry& blocks, FeatureLevel& level,
                  FeatureRandom& random, BlockPos at, i32 height) const {
        if (kind_ == Kind::Brown) {
            make_brown_cap(blocks, level, random, at, height);
        } else {
            make_red_cap(blocks, level, random, at, height);
        }
    }

    /// One flat square at the top with its four corners cut, each face
    /// flagged on the rim so the texture shows.
    void make_brown_cap(const registry::BlockRegistry& blocks, FeatureLevel& level,
                        FeatureRandom& random, BlockPos at, i32 height) const {
        const i32 r = radius_;
        for (i32 dx = -r; dx <= r; ++dx) {
            for (i32 dz = -r; dz <= r; ++dz) {
                const bool west_rim  = dx == -r;
                const bool east_rim  = dx == r;
                const bool north_rim = dz == -r;
                const bool south_rim = dz == r;
                const bool x_rim     = west_rim || east_rim;
                const bool z_rim     = north_rim || south_rim;
                if (x_rim && z_rim) {
                    continue;
                }
                const BlockPos here{at.x + dx, at.y + height, at.z + dz};
                if (is_solid_render(blocks, level.block_at(here.x, here.y, here.z))) {
                    continue;
                }
                const bool west  = west_rim || (z_rim && dx == 1 - r);
                const bool east  = east_rim || (z_rim && dx == r - 1);
                const bool north = north_rim || (x_rim && dz == 1 - r);
                const bool south = south_rim || (x_rim && dz == r - 1);
                auto state = cap_->state(level, random, at);
                state      = faces(blocks, state, std::nullopt, west, east, north, south);
                (void)level.set_block(here.x, here.y, here.z, state);
            }
        }
    }

    /// Four rings: the three below the top are hollow squares, the top is a
    /// full square one narrower.
    void make_red_cap(const registry::BlockRegistry& blocks, FeatureLevel& level,
                      FeatureRandom& random, BlockPos at, i32 height) const {
        for (i32 y = height - 3; y <= height; ++y) {
            const i32 r     = y < height ? radius_ : radius_ - 1;
            const i32 inner = radius_ - 2;
            for (i32 dx = -r; dx <= r; ++dx) {
                for (i32 dz = -r; dz <= r; ++dz) {
                    const bool x_rim = dx == -r || dx == r;
                    const bool z_rim = dz == -r || dz == r;
                    if (!(y >= height || x_rim != z_rim)) {
                        continue;
                    }
                    const BlockPos here{at.x + dx, at.y + y, at.z + dz};
                    if (is_solid_render(blocks, level.block_at(here.x, here.y, here.z))) {
                        continue;
                    }
                    auto state = cap_->state(level, random, at);
                    state      = faces(blocks, state, y >= height - 1, dx < -inner, dx > inner,
                                       dz < -inner, dz > inner);
                    (void)level.set_block(here.x, here.y, here.z, state);
                }
            }
        }
    }

    /// The five face flags a huge mushroom block carries, set only when the
    /// block has them — the game checks all four horizontal properties exist
    /// before touching any.
    [[nodiscard]] static registry::BlockStateId faces(const registry::BlockRegistry& blocks,
                                                      registry::BlockStateId state,
                                                      std::optional<bool> up, bool west,
                                                      bool east, bool north, bool south) {
        const auto block = blocks.block_of(state);
        if (!blocks.find_property(block, "west") || !blocks.find_property(block, "east") ||
            !blocks.find_property(block, "north") || !blocks.find_property(block, "south")) {
            return state;
        }
        const auto flag = [](bool value) { return value ? "true" : "false"; };
        if (up) {
            state = with_property_value(blocks, state, "up", flag(*up));
        }
        state = with_property_value(blocks, state, "west", flag(west));
        state = with_property_value(blocks, state, "east", flag(east));
        state = with_property_value(blocks, state, "north", flag(north));
        state = with_property_value(blocks, state, "south", flag(south));
        return state;
    }

    Kind             kind_;
    StateProviderRef cap_;
    StateProviderRef stem_;
    i32              radius_;
    std::vector<u16> dirt_;
    std::vector<u16> grow_block_;
    std::vector<u16> leaves_;
};

}  // namespace

ClaimedFeature parse_huge_mushroom_feature(std::string_view kind, Json config,
                                           const registry::BlockRegistry& blocks,
                                           const BlockTags&               tags) {
    if (kind != "huge_brown_mushroom" && kind != "huge_red_mushroom") {
        return std::nullopt;
    }
    auto cap_field  = config.at_key("cap_provider");
    auto stem_field = config.at_key("stem_provider");
    if (cap_field.error() != simdjson::SUCCESS || stem_field.error() != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    auto cap = parse_state_provider(cap_field.value(), blocks, tags);
    if (!cap) return std::unexpected(cap.error());
    auto stem = parse_state_provider(stem_field.value(), blocks, tags);
    if (!stem) return std::unexpected(stem.error());
    i64 radius = 2;
    (void)config.at_key("foliage_radius").get(radius);

    auto dirt = tag_members(blocks, tags, "minecraft:dirt");
    if (!dirt) return std::unexpected(dirt.error());
    auto grow = tag_members(blocks, tags, "minecraft:mushroom_grow_block");
    if (!grow) return std::unexpected(grow.error());
    auto leaves = tag_members(blocks, tags, "minecraft:leaves");
    if (!leaves) return std::unexpected(leaves.error());

    const auto which = kind == "huge_brown_mushroom" ? HugeMushroomFeature::Kind::Brown
                                                     : HugeMushroomFeature::Kind::Red;
    return std::static_pointer_cast<const Feature>(std::make_shared<const HugeMushroomFeature>(
        which, *cap, *stem, static_cast<i32>(radius), std::move(*dirt), std::move(*grow),
        std::move(*leaves)));
}

}  // namespace ov::worldgen
