#define OV_LOG_CATEGORY "worldgen"

// The lush caves and every cave's walls: `vegetation_patch`,
// `waterlogged_vegetation_patch`, `multiface_growth`.
//
// Two of them walk a `java.util.HashSet<BlockPos>` and draw once per element,
// so, exactly as for the tree decorators, the set's iteration order is part
// of the seed and comes from `java_hash_order`.

#include "overworld_feature.hpp"

#include "ov/base/log.hpp"
#include "ov/worldgen/tree_feature.hpp"

#include <array>

namespace ov::worldgen {

namespace {

using Face = registry::BlockRegistry::Face;

/// The six directions in `Direction` order, with the face that looks back.
struct Dir {
    std::array<i32, 3> offset;
    Face               face;      ///< this direction as a face of the block
    Face               opposite;  ///< the face of the neighbour that looks back
    std::string_view   property;  ///< the multiface property for this face
    u8                 axis;      ///< 0 x, 1 y, 2 z
};

constexpr std::array<Dir, 6> kDirs{{
    {{0, -1, 0}, Face::Down, Face::Up, "down", 1},
    {{0, 1, 0}, Face::Up, Face::Down, "up", 1},
    {{0, 0, -1}, Face::North, Face::South, "north", 2},
    {{0, 0, 1}, Face::South, Face::North, "south", 2},
    {{-1, 0, 0}, Face::West, Face::East, "west", 0},
    {{1, 0, 0}, Face::East, Face::West, "east", 0},
}};
constexpr usize kDown  = 0;
constexpr usize kUp    = 1;
constexpr usize kNorth = 2;
constexpr usize kSouth = 3;
constexpr usize kWest  = 4;
constexpr usize kEast  = 5;

[[nodiscard]] constexpr usize opposite(usize dir) {
    return dir ^ 1U;
}

[[nodiscard]] BlockPos step(BlockPos pos, usize dir, i32 times = 1) {
    return pos.offset(kDirs[dir].offset[0] * times, kDirs[dir].offset[1] * times,
                      kDirs[dir].offset[2] * times);
}

/// `Util.shuffle` over a small list, one `nextInt(i)` per step from the top.
template<typename T>
void shuffle(std::vector<T>& list, FeatureRandom& random) {
    for (usize i = list.size(); i > 1; --i) {
        const auto j = static_cast<usize>(random.next_int(static_cast<i32>(i)));
        std::swap(list[i - 1], list[j]);
    }
}

bool run_placed(const PlacedFeature& placed, const FeatureContext& context, FeatureLevel& level,
                FeatureRandom& random, BlockPos at) {
    bool wrote = false;
    expand(placed.placement, context, level, random, at, [&](BlockPos where) {
        wrote |= placed.feature->place(context, level, random, where);
    });
    return wrote;
}

// ── vegetation_patch, waterlogged_vegetation_patch ──────────────────────────

struct PatchConfig {
    std::vector<u16>                     replaceable;
    StateProviderRef                     ground;
    std::shared_ptr<const PlacedFeature> vegetation;
    bool                                 ceiling{false};
    IntProviderRef                       depth;
    f32                                  extra_bottom_block_chance{0.0F};
    i32                                  vertical_range{5};
    f32                                  vegetation_chance{0.0F};
    IntProviderRef                       xz_radius;
    f32                                  extra_edge_column_chance{0.0F};
    registry::BlockId                    water{0};
};

class VegetationPatchFeature final : public Feature {
public:
    VegetationPatchFeature(PatchConfig c, bool waterlogged)
        : c_(std::move(c)), waterlogged_(waterlogged) {}

    [[nodiscard]] std::string_view type_name() const override {
        return waterlogged_ ? "waterlogged_vegetation_patch" : "vegetation_patch";
    }

    bool place(const FeatureContext& context, FeatureLevel& level, FeatureRandom& random,
               BlockPos origin) const override {
        const i32 radius_x = c_.xz_radius->sample(random) + 1;
        const i32 radius_z = c_.xz_radius->sample(random) + 1;
        auto      ground   = ground_patch(context, level, random, origin, radius_x, radius_z);
        if (waterlogged_) {
            ground = flood(context, level, ground);
        }
        for (const BlockPos& at : java_hash_order(ground)) {
            if (!(c_.vegetation_chance > 0.0F)) {
                continue;
            }
            const f32 roll = random.next_float();
            if (!(roll < c_.vegetation_chance)) {
                continue;
            }
            place_vegetation(context, level, random, at);
        }
        return !ground.empty();
    }

private:
    /// Floor patches look down for the ground; ceiling patches look up.
    [[nodiscard]] usize surface() const { return c_.ceiling ? kUp : kDown; }

    std::vector<BlockPos> ground_patch(const FeatureContext& context, FeatureLevel& level,
                                       FeatureRandom& random, BlockPos origin, i32 radius_x,
                                       i32 radius_z) const {
        const auto& blocks  = *context.blocks;
        const usize towards = surface();
        const usize away    = opposite(towards);
        std::vector<BlockPos> set;
        for (i32 i = -radius_x; i <= radius_x; ++i) {
            const bool x_edge = i == -radius_x || i == radius_x;
            for (i32 j = -radius_z; j <= radius_z; ++j) {
                const bool z_edge = j == -radius_z || j == radius_z;
                const bool edge   = x_edge || z_edge;
                const bool corner = x_edge && z_edge;
                const bool side   = edge && !corner;
                if (corner) {
                    continue;
                }
                if (side) {
                    if (c_.extra_edge_column_chance == 0.0F) {
                        continue;
                    }
                    const f32 roll = random.next_float();
                    if (roll > c_.extra_edge_column_chance) {
                        continue;
                    }
                }
                BlockPos cursor = origin.offset(i, 0, j);
                for (i32 k = 0; k < c_.vertical_range && air(blocks, level, cursor); ++k) {
                    cursor = step(cursor, towards);
                }
                for (i32 k = 0; k < c_.vertical_range && !air(blocks, level, cursor); ++k) {
                    cursor = step(cursor, away);
                }
                BlockPos floor = step(cursor, towards);
                if (!air(blocks, level, cursor) ||
                    !blocks.face_is_sturdy(level.block_at(floor.x, floor.y, floor.z),
                                           kDirs[away].face)) {
                    continue;
                }
                const i32 base  = c_.depth->sample(random);
                i32       extra = 0;
                if (c_.extra_bottom_block_chance > 0.0F) {
                    const f32 roll = random.next_float();
                    extra          = roll < c_.extra_bottom_block_chance ? 1 : 0;
                }
                const BlockPos top = floor;
                if (place_ground(context, level, random, floor, base + extra)) {
                    set.push_back(top);
                }
            }
        }
        return set;
    }

    /// Replace downward (or upward) with the ground block, `depth` times. A
    /// block that already *is* the ground block is stepped over without the
    /// cursor moving — the game's own loop, reproduced as it is.
    bool place_ground(const FeatureContext& context, FeatureLevel& level, FeatureRandom& random,
                      BlockPos& cursor, i32 depth) const {
        const auto& blocks = *context.blocks;
        for (i32 i = 0; i < depth; ++i) {
            const auto state = c_.ground->state(level, random, cursor);
            const auto here  = blocks.block_of(level.block_at(cursor.x, cursor.y, cursor.z));
            if (blocks.block_of(state) == here) {
                continue;
            }
            if (!holds(c_.replaceable, here)) {
                return i != 0;
            }
            (void)level.set_block(cursor.x, cursor.y, cursor.z, state);
            cursor = step(cursor, surface());
        }
        return true;
    }

    /// The waterlogged patch keeps only the ground cells closed on the four
    /// sides and below, and fills them with water.
    std::vector<BlockPos> flood(const FeatureContext& context, FeatureLevel& level,
                                const std::vector<BlockPos>& ground) const {
        const auto&           blocks = *context.blocks;
        std::vector<BlockPos> kept;
        for (const BlockPos& at : java_hash_order(ground)) {
            bool exposed = false;
            for (const usize dir : {kNorth, kEast, kSouth, kWest, kDown}) {
                const BlockPos next = step(at, dir);
                if (!blocks.face_is_sturdy(level.block_at(next.x, next.y, next.z),
                                           kDirs[dir].opposite)) {
                    exposed = true;
                    break;
                }
            }
            if (!exposed) {
                kept.push_back(at);
            }
        }
        for (const BlockPos& at : java_hash_order(kept)) {
            (void)level.set_block(at.x, at.y, at.z, blocks.default_state(c_.water));
        }
        return kept;
    }

    void place_vegetation(const FeatureContext& context, FeatureLevel& level,
                          FeatureRandom& random, BlockPos ground) const {
        const auto& blocks = *context.blocks;
        if (!waterlogged_) {
            (void)run_placed(*c_.vegetation, context, level, random, step(ground, opposite(surface())));
            return;
        }
        // One further down, so that the plant goes *into* the water cell.
        const BlockPos below = ground.below();
        if (run_placed(*c_.vegetation, context, level, random, step(below, opposite(surface())))) {
            const auto state    = level.block_at(ground.x, ground.y, ground.z);
            const auto property = blocks.find_property(blocks.block_of(state), "waterlogged");
            if (property && blocks.property_value(state, *property) == "false") {
                (void)level.set_block(ground.x, ground.y, ground.z,
                                      with_property_value(blocks, state, "waterlogged", "true"));
            }
        }
    }

    [[nodiscard]] static bool air(const registry::BlockRegistry& blocks, const FeatureLevel& level,
                                  BlockPos at) {
        return blocks.is_air(blocks.block_of(level.block_at(at.x, at.y, at.z)));
    }

    PatchConfig c_;
    bool        waterlogged_;
};

// ── multiface_growth ────────────────────────────────────────────────────────

struct GrowthConfig {
    registry::BlockId  block{0};
    std::vector<u16>   can_be_placed_on;
    std::vector<usize> valid_directions;  ///< up, down, then the four walls
    i32                search_range{10};
    f32                chance_of_spreading{0.5F};
    registry::BlockId  water{0};
};

class MultifaceGrowthFeature final : public Feature {
public:
    explicit MultifaceGrowthFeature(GrowthConfig c) : c_(std::move(c)) {}

    [[nodiscard]] std::string_view type_name() const override { return "multiface_growth"; }

    bool place(const FeatureContext& context, FeatureLevel& level, FeatureRandom& random,
               BlockPos origin) const override {
        const auto& blocks = *context.blocks;
        if (!air_or_water(blocks, level.block_at(origin.x, origin.y, origin.z))) {
            return false;
        }
        std::vector<usize> order = c_.valid_directions;
        shuffle(order, random);
        if (grow(blocks, level, random, origin, order)) {
            return true;
        }
        for (const usize dir : order) {
            BlockPos           cursor = origin;
            std::vector<usize> others;
            for (const usize d : c_.valid_directions) {
                if (d != opposite(dir)) {
                    others.push_back(d);
                }
            }
            shuffle(others, random);
            for (i32 i = 0; i < c_.search_range; ++i) {
                cursor           = step(cursor, dir);
                const auto state = level.block_at(cursor.x, cursor.y, cursor.z);
                if (!air_or_water(blocks, state) && blocks.block_of(state) != c_.block) {
                    break;
                }
                if (grow(blocks, level, random, cursor, others)) {
                    return true;
                }
            }
        }
        return false;
    }

private:
    [[nodiscard]] bool air_or_water(const registry::BlockRegistry& blocks,
                                    registry::BlockStateId          state) const {
        const auto block = blocks.block_of(state);
        return blocks.is_air(block) || block == c_.water;
    }

    [[nodiscard]] bool is_water_source(const registry::BlockRegistry& blocks,
                                       registry::BlockStateId          state) const {
        const auto block = blocks.block_of(state);
        if (block == c_.water) {
            const auto level = blocks.find_property(block, "level");
            return !level || blocks.property_value(state, *level) == "0";
        }
        const auto property = blocks.find_property(block, "waterlogged");
        return property && blocks.property_value(state, *property) == "true";
    }

    [[nodiscard]] bool has_face(const registry::BlockRegistry& blocks, registry::BlockStateId state,
                                usize dir) const {
        const auto block = blocks.block_of(state);
        if (block != c_.block) {
            return false;
        }
        const auto property = blocks.find_property(block, kDirs[dir].property);
        return property && blocks.property_value(state, *property) == "true";
    }

    /// `MultifaceBlock.canAttachTo`: the neighbour shows a full face back.
    [[nodiscard]] static bool can_attach(const registry::BlockRegistry& blocks,
                                         const FeatureLevel& level, BlockPos pos, usize dir) {
        const BlockPos next = step(pos, dir);
        return blocks.face_is_sturdy(level.block_at(next.x, next.y, next.z), kDirs[dir].opposite);
    }

    /// `getStateForPlacement`, or nothing.
    [[nodiscard]] std::optional<registry::BlockStateId> state_for(
        const registry::BlockRegistry& blocks, const FeatureLevel& level,
        registry::BlockStateId current, BlockPos pos, usize dir) const {
        if (has_face(blocks, current, dir) || !can_attach(blocks, level, pos, dir)) {
            return std::nullopt;
        }
        registry::BlockStateId base = current;
        if (blocks.block_of(current) != c_.block) {
            base = blocks.default_state(c_.block);
            if (is_water_source(blocks, current)) {
                base = with_property_value(blocks, base, "waterlogged", "true");
            }
        }
        return with_property_value(blocks, base, kDirs[dir].property, "true");
    }

    bool grow(const registry::BlockRegistry& blocks, FeatureLevel& level, FeatureRandom& random,
              BlockPos pos, const std::vector<usize>& directions) const {
        const auto current = level.block_at(pos.x, pos.y, pos.z);
        for (const usize dir : directions) {
            const BlockPos next = step(pos, dir);
            if (!holds(c_.can_be_placed_on, blocks.block_of(level.block_at(next.x, next.y, next.z)))) {
                continue;
            }
            const auto state = state_for(blocks, level, current, pos, dir);
            if (!state) {
                return false;
            }
            (void)level.set_block(pos.x, pos.y, pos.z, *state);
            const f32 roll = random.next_float();
            if (roll < c_.chance_of_spreading) {
                spread(blocks, level, random, *state, pos, dir);
            }
            return true;
        }
        return false;
    }

    /// `MultifaceSpreader.spreadFromFaceTowardRandomDirection`: all six
    /// directions shuffled, the first that yields a spread wins; for each, the
    /// three spread types in order — same block, same plane, wrap around.
    void spread(const registry::BlockRegistry& blocks, FeatureLevel& level, FeatureRandom& random,
                registry::BlockStateId state, BlockPos pos, usize from) const {
        std::vector<usize> all{kDown, kUp, kNorth, kSouth, kWest, kEast};
        shuffle(all, random);
        for (const usize to : all) {
            if (to == from || kDirs[to].axis == kDirs[from].axis) {
                continue;
            }
            if (!(has_face(blocks, state, from) && !has_face(blocks, state, to))) {
                continue;
            }
            const std::array<std::pair<BlockPos, usize>, 3> candidates{{
                {pos, to},
                {step(pos, to), from},
                {step(step(pos, to), from), opposite(to)},
            }};
            for (const auto& [target, face] : candidates) {
                const auto there = level.block_at(target.x, target.y, target.z);
                const auto block = blocks.block_of(there);
                const bool replaceable =
                    blocks.is_air(block) || block == c_.block || is_water_source(blocks, there);
                if (!replaceable) {
                    continue;
                }
                const auto placed = state_for(blocks, level, there, target, face);
                if (!placed) {
                    continue;
                }
                (void)level.set_block(target.x, target.y, target.z, *placed);
                return;
            }
        }
    }

    GrowthConfig c_;
};

[[nodiscard]] f32 float_field(Json node, std::string_view key, f32 fallback) {
    f64 value = static_cast<f64>(fallback);
    if (node.at_key(key).get(value) == simdjson::SUCCESS) {
        return static_cast<f32>(value);
    }
    i64 whole = 0;
    if (node.at_key(key).get(whole) == simdjson::SUCCESS) {
        return static_cast<f32>(whole);
    }
    return fallback;
}

[[nodiscard]] std::expected<std::vector<u16>, FeatureError> block_set(
    Json node, const registry::BlockRegistry& blocks, const BlockTags& tags) {
    std::string_view single;
    if (node.get(single) == simdjson::SUCCESS) {
        if (single.starts_with('#')) {
            return tag_members(blocks, tags, single.substr(1));
        }
        auto block = named_block(blocks, qualify(single));
        if (!block) return std::unexpected(block.error());
        return std::vector<u16>{block->value()};
    }
    simdjson::dom::array list;
    if (node.get(list) != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    std::vector<u16> out;
    for (auto value : list) {
        std::string_view name;
        if (value.get(name) != simdjson::SUCCESS) return std::unexpected(FeatureError::Malformed);
        auto block = named_block(blocks, qualify(name));
        if (!block) return std::unexpected(block.error());
        out.push_back(block->value());
    }
    std::ranges::sort(out);
    return out;
}

}  // namespace

ClaimedFeature parse_lush_feature(std::string_view kind, Json config,
                                  const registry::BlockRegistry& blocks, const BlockTags& tags,
                                  const FeatureResolver& resolve) {
    if (kind == "vegetation_patch" || kind == "waterlogged_vegetation_patch") {
        PatchConfig c;
        auto replaceable = config.at_key("replaceable");
        auto ground      = config.at_key("ground_state");
        auto depth       = config.at_key("depth");
        auto radius      = config.at_key("xz_radius");
        auto vegetation  = config.at_key("vegetation_feature");
        if (replaceable.error() != simdjson::SUCCESS || ground.error() != simdjson::SUCCESS ||
            depth.error() != simdjson::SUCCESS || radius.error() != simdjson::SUCCESS ||
            vegetation.error() != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        auto set = block_set(replaceable.value(), blocks, tags);
        if (!set) return std::unexpected(set.error());
        c.replaceable = std::move(*set);
        auto provider = parse_state_provider(ground.value(), blocks, tags);
        if (!provider) return std::unexpected(provider.error());
        c.ground = *provider;
        auto d = parse_int_provider(depth.value());
        auto r = parse_int_provider(radius.value());
        if (!d || !r) return std::unexpected(FeatureError::Malformed);
        c.depth     = *d;
        c.xz_radius = *r;
        auto inner  = parse_inline_placed_feature(vegetation.value(), blocks, tags, resolve);
        if (!inner) return std::unexpected(inner.error());
        c.vegetation = *inner;
        std::string_view surface = "floor";
        (void)config.at_key("surface").get(surface);
        c.ceiling                   = surface == "ceiling";
        c.extra_bottom_block_chance = float_field(config, "extra_bottom_block_chance", 0.0F);
        c.vertical_range   = static_cast<i32>(float_field(config, "vertical_range", 5.0F));
        c.vegetation_chance = float_field(config, "vegetation_chance", 0.0F);
        c.extra_edge_column_chance = float_field(config, "extra_edge_column_chance", 0.0F);
        auto water = named_block(blocks, "minecraft:water");
        if (!water) return std::unexpected(water.error());
        c.water = *water;
        return std::static_pointer_cast<const Feature>(std::make_shared<const VegetationPatchFeature>(
            std::move(c), kind == "waterlogged_vegetation_patch"));
    }

    if (kind == "multiface_growth") {
        GrowthConfig     c;
        std::string_view name;
        if (config.at_key("block").get(name) != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        auto block = named_block(blocks, qualify(name));
        if (!block) return std::unexpected(block.error());
        c.block = *block;
        if (blocks.block_name(c.block) != "minecraft:glow_lichen") {
            // The sculk vein grows through the sculk spreader, a different
            // machine; only the glow lichen's default spreader is written.
            OV_LOG_ERROR("worldgen: multiface_growth of {} needs its own spreader, not written",
                         blocks.block_name(c.block));
            return std::unexpected(FeatureError::Unsupported);
        }
        auto on = config.at_key("can_be_placed_on");
        if (on.error() != simdjson::SUCCESS) return std::unexpected(FeatureError::Malformed);
        auto set = block_set(on.value(), blocks, tags);
        if (!set) return std::unexpected(set.error());
        c.can_be_placed_on = std::move(*set);
        bool ceiling = false;
        bool floor   = false;
        bool wall    = false;
        (void)config.at_key("can_place_on_ceiling").get(ceiling);
        (void)config.at_key("can_place_on_floor").get(floor);
        (void)config.at_key("can_place_on_wall").get(wall);
        if (ceiling) c.valid_directions.push_back(kUp);
        if (floor) c.valid_directions.push_back(kDown);
        if (wall) {
            // `Direction.Plane.HORIZONTAL`: north, east, south, west.
            for (const usize d : {kNorth, kEast, kSouth, kWest}) c.valid_directions.push_back(d);
        }
        c.search_range        = static_cast<i32>(float_field(config, "search_range", 10.0F));
        c.chance_of_spreading = float_field(config, "chance_of_spreading", 0.5F);
        auto water = named_block(blocks, "minecraft:water");
        if (!water) return std::unexpected(water.error());
        c.water = *water;
        return std::static_pointer_cast<const Feature>(
            std::make_shared<const MultifaceGrowthFeature>(std::move(c)));
    }
    return std::nullopt;
}

}  // namespace ov::worldgen
