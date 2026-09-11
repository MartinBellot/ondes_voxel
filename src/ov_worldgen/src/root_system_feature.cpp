#define OV_LOG_CATEGORY "worldgen"

// `root_system`: the rooted azalea tree, the one sign on the surface that a
// lush cave lies underneath.
//
// From a point in the cave, climb until there is room for a tree on solid
// ground; grow the tree there; then fill the column between the cave and the
// tree with rooted dirt, and hang roots from the cave's ceiling. The tree is a
// nested placed feature — `azalea_tree` — run at the position the climb found.

#include "overworld_feature.hpp"

#include "ov/base/log.hpp"

namespace ov::worldgen {

namespace {

struct RootConfig {
    std::shared_ptr<const PlacedFeature> tree;
    BlockPredicateRef                    allowed_tree_position;
    i32                                  required_vertical_space_for_tree{3};
    i32                                  allowed_vertical_water_for_tree{2};
    i32                                  root_column_max_height{100};
    i32                                  root_radius{3};
    i32                                  root_placement_attempts{20};
    std::vector<u16>                     root_replaceable;
    StateProviderRef                     root_state;
    i32                                  hanging_root_radius{3};
    i32                                  hanging_roots_vertical_span{2};
    i32                                  hanging_root_placement_attempts{20};
    StateProviderRef                     hanging_root_state;
    registry::BlockId                    water{0};
    registry::BlockId                    lava{0};
};

class RootSystemFeature final : public Feature {
public:
    explicit RootSystemFeature(RootConfig c) : c_(std::move(c)) {}

    [[nodiscard]] std::string_view type_name() const override { return "root_system"; }

    bool place(const FeatureContext& context, FeatureLevel& level, FeatureRandom& random,
               BlockPos origin) const override {
        const auto& blocks = *context.blocks;
        if (!blocks.is_air(blocks.block_of(level.block_at(origin.x, origin.y, origin.z)))) {
            return false;
        }
        if (grow_tree(context, level, random, origin)) {
            hang_roots(blocks, level, random, origin);
        }
        return true;
    }

private:
    [[nodiscard]] bool is_water(const registry::BlockRegistry& blocks,
                                registry::BlockStateId          state) const {
        const auto block = blocks.block_of(state);
        if (block == c_.water) {
            return true;
        }
        const auto property = blocks.find_property(block, "waterlogged");
        return property && blocks.property_value(state, *property) == "true";
    }

    /// Air, or water no higher than the allowance above the tree's base.
    [[nodiscard]] bool room_for_tree(const registry::BlockRegistry& blocks,
                                     const FeatureLevel& level, BlockPos at) const {
        for (i32 i = 1; i <= c_.required_vertical_space_for_tree; ++i) {
            const auto state = level.block_at(at.x, at.y + i, at.z);
            if (blocks.is_air(blocks.block_of(state))) {
                continue;
            }
            if (i + 1 <= c_.allowed_vertical_water_for_tree && is_water(blocks, state)) {
                continue;
            }
            return false;
        }
        return true;
    }

    bool grow_tree(const FeatureContext& context, FeatureLevel& level, FeatureRandom& random,
                   BlockPos origin) const {
        const auto& blocks = *context.blocks;
        BlockPos    cursor = origin;
        for (i32 i = 0; i < c_.root_column_max_height; ++i) {
            cursor = cursor.above();
            if (!c_.allowed_tree_position->test(level, cursor) ||
                !room_for_tree(blocks, level, cursor)) {
                continue;
            }
            const auto below = level.block_at(cursor.x, cursor.y - 1, cursor.z);
            if (blocks.block_of(below) == c_.lava || !is_solid(blocks, below)) {
                return false;
            }
            bool wrote = false;
            expand(c_.tree->placement, context, level, random, cursor, [&](BlockPos where) {
                wrote |= c_.tree->feature->place(context, level, random, where);
            });
            if (!wrote) {
                continue;
            }
            // Rooted dirt up the column the climb went through, below the tree.
            for (i32 y = origin.y; y < origin.y + i; ++y) {
                root_layer(blocks, level, random, {origin.x, y, origin.z});
            }
            return true;
        }
        return false;
    }

    /// A handful of tries around the column at one height. Four draws for
    /// the offset, in order, whatever the block turns out to be.
    void root_layer(const registry::BlockRegistry& blocks, FeatureLevel& level,
                    FeatureRandom& random, BlockPos centre) const {
        const i32 r = c_.root_radius;
        for (i32 j = 0; j < c_.root_placement_attempts; ++j) {
            const i32 x_hi = random.next_int(r);
            const i32 x_lo = random.next_int(r);
            const i32 z_hi = random.next_int(r);
            const i32 z_lo = random.next_int(r);
            const BlockPos at = centre.offset(x_hi - x_lo, 0, z_hi - z_lo);
            if (!holds(c_.root_replaceable, blocks.block_of(level.block_at(at.x, at.y, at.z)))) {
                continue;
            }
            (void)level.set_block(at.x, at.y, at.z, c_.root_state->state(level, random, at));
        }
    }

    /// Hanging roots in the cave: an empty cell whose ceiling is sturdy.
    void hang_roots(const registry::BlockRegistry& blocks, FeatureLevel& level,
                    FeatureRandom& random, BlockPos origin) const {
        const i32 r    = c_.hanging_root_radius;
        const i32 span = c_.hanging_roots_vertical_span;
        for (i32 k = 0; k < c_.hanging_root_placement_attempts; ++k) {
            const i32 x_hi = random.next_int(r);
            const i32 x_lo = random.next_int(r);
            const i32 y_hi = random.next_int(span);
            const i32 y_lo = random.next_int(span);
            const i32 z_hi = random.next_int(r);
            const i32 z_lo = random.next_int(r);
            const BlockPos at = origin.offset(x_hi - x_lo, y_hi - y_lo, z_hi - z_lo);
            if (!blocks.is_air(blocks.block_of(level.block_at(at.x, at.y, at.z)))) {
                continue;
            }
            const auto state   = c_.hanging_root_state->state(level, random, at);
            const auto ceiling = level.block_at(at.x, at.y + 1, at.z);
            // Its survival and the placement test are the same question.
            if (!blocks.face_is_sturdy(ceiling, registry::BlockRegistry::Face::Down)) {
                continue;
            }
            (void)level.set_block(at.x, at.y, at.z, state);
        }
    }

    RootConfig c_;
};

[[nodiscard]] i32 int_field(Json node, std::string_view key, i32 fallback) {
    i64 value = fallback;
    (void)node.at_key(key).get(value);
    return static_cast<i32>(value);
}

}  // namespace

ClaimedFeature parse_root_system_feature(std::string_view kind, Json config,
                                         const registry::BlockRegistry& blocks,
                                         const BlockTags& tags, const FeatureResolver& resolve) {
    if (kind != "root_system") {
        return std::nullopt;
    }
    RootConfig c;
    auto tree     = config.at_key("feature");
    auto allowed  = config.at_key("allowed_tree_position");
    auto root     = config.at_key("root_state_provider");
    auto hanging  = config.at_key("hanging_root_state_provider");
    std::string_view replaceable;
    if (tree.error() != simdjson::SUCCESS || allowed.error() != simdjson::SUCCESS ||
        root.error() != simdjson::SUCCESS || hanging.error() != simdjson::SUCCESS ||
        config.at_key("root_replaceable").get(replaceable) != simdjson::SUCCESS ||
        !replaceable.starts_with('#')) {
        return std::unexpected(FeatureError::Malformed);
    }
    auto placed = parse_inline_placed_feature(tree.value(), blocks, tags, resolve);
    if (!placed) return std::unexpected(placed.error());
    c.tree = *placed;
    auto predicate = parse_block_predicate(allowed.value(), blocks, tags);
    if (!predicate) return std::unexpected(predicate.error());
    c.allowed_tree_position = *predicate;
    auto root_state = parse_state_provider(root.value(), blocks, tags);
    if (!root_state) return std::unexpected(root_state.error());
    c.root_state = *root_state;
    auto hanging_state = parse_state_provider(hanging.value(), blocks, tags);
    if (!hanging_state) return std::unexpected(hanging_state.error());
    c.hanging_root_state = *hanging_state;
    auto set = tag_members(blocks, tags, replaceable.substr(1));
    if (!set) return std::unexpected(set.error());
    c.root_replaceable = std::move(*set);

    c.required_vertical_space_for_tree = int_field(config, "required_vertical_space_for_tree", 3);
    c.allowed_vertical_water_for_tree  = int_field(config, "allowed_vertical_water_for_tree", 2);
    c.root_column_max_height           = int_field(config, "root_column_max_height", 100);
    c.root_radius                      = int_field(config, "root_radius", 3);
    c.root_placement_attempts          = int_field(config, "root_placement_attempts", 20);
    c.hanging_root_radius              = int_field(config, "hanging_root_radius", 3);
    c.hanging_roots_vertical_span      = int_field(config, "hanging_roots_vertical_span", 2);
    c.hanging_root_placement_attempts  = int_field(config, "hanging_root_placement_attempts", 20);

    auto water = named_block(blocks, "minecraft:water");
    auto lava  = named_block(blocks, "minecraft:lava");
    if (!water || !lava) return std::unexpected(FeatureError::Missing);
    c.water = *water;
    c.lava  = *lava;
    return std::static_pointer_cast<const Feature>(
        std::make_shared<const RootSystemFeature>(std::move(c)));
}

}  // namespace ov::worldgen
