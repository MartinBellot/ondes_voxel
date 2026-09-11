#define OV_LOG_CATEGORY "worldgen"

// The sea floor: `seagrass`, `kelp`, `sea_pickle`, `coral_tree`, `coral_claw`,
// `coral_mushroom`, `underwater_magma`.
//
// The corals choose their blocks by drawing an index into a tag's *contents*,
// so the order of a tag's entries is part of the seed. BlockTags keeps sets,
// not lists; the three coral tags are therefore read here in file order, with
// nested `#tag` references expanded in place, and a tag that cannot be read
// that way refuses the feature by name.

#include "overworld_feature.hpp"

#include "ov/base/log.hpp"

#include <array>

namespace ov::worldgen {

namespace {

constexpr std::array<std::array<i32, 2>, 4> kHorizontal{{{0, -1}, {1, 0}, {0, 1}, {-1, 0}}};
constexpr std::array<std::string_view, 4>   kHorizontalNames{"north", "east", "south", "west"};

/// What the ocean features ask of a block, resolved once.
struct Sea {
    const registry::BlockRegistry* blocks{nullptr};
    registry::BlockId              water{0};
    registry::BlockId              magma{0};
    registry::BlockId              seagrass{0};
    registry::BlockId              tall_seagrass{0};
    registry::BlockId              kelp{0};
    registry::BlockId              kelp_plant{0};
    registry::BlockId              sea_pickle{0};
    std::vector<u16>               corals;  ///< #corals, for "may be replaced by a coral block"

    [[nodiscard]] registry::BlockId at(const FeatureLevel& level, BlockPos pos) const {
        return blocks->block_of(level.block_at(pos.x, pos.y, pos.z));
    }
    [[nodiscard]] bool is_water(const FeatureLevel& level, BlockPos pos) const {
        return at(level, pos) == water;
    }
    [[nodiscard]] registry::BlockStateId state_of(registry::BlockId block) const {
        return blocks->default_state(block);
    }
    /// A sturdy top face below that is not magma — the seagrass's `mayPlaceOn`.
    [[nodiscard]] bool seagrass_ground(const FeatureLevel& level, BlockPos pos) const {
        const auto below = level.block_at(pos.x, pos.y - 1, pos.z);
        return blocks->block_of(below) != magma &&
               blocks->face_is_sturdy(below, registry::BlockRegistry::Face::Up);
    }
    /// `SeaPickleBlock.mayPlaceOn`: a sturdy top face, or any collision shape
    /// that reaches the top of its block. The second half is read off the
    /// collision boxes — a face shape the registry does not store as such.
    [[nodiscard]] bool pickle_ground(const FeatureLevel& level, BlockPos pos) const {
        const auto below = level.block_at(pos.x, pos.y - 1, pos.z);
        if (blocks->face_is_sturdy(below, registry::BlockRegistry::Face::Up)) {
            return true;
        }
        for (const auto& box : blocks->collision_boxes(below)) {
            if (box.max_y == 32 && box.max_x > box.min_x && box.max_z > box.min_z) {
                return true;
            }
        }
        return false;
    }
    /// Kelp: not on magma, and on kelp or a sturdy top face.
    [[nodiscard]] bool kelp_ground(const FeatureLevel& level, BlockPos pos) const {
        const auto below       = level.block_at(pos.x, pos.y - 1, pos.z);
        const auto below_block = blocks->block_of(below);
        if (below_block == magma) {
            return false;
        }
        return below_block == kelp || below_block == kelp_plant ||
               blocks->face_is_sturdy(below, registry::BlockRegistry::Face::Up);
    }
};

[[nodiscard]] std::expected<Sea, FeatureError> sea_vocabulary(const registry::BlockRegistry& blocks,
                                                             const BlockTags&               tags) {
    Sea sea;
    sea.blocks = &blocks;
    for (const auto& [name, into] : std::array<std::pair<std::string_view, registry::BlockId*>, 7>{{
             {"minecraft:water", &sea.water},
             {"minecraft:magma_block", &sea.magma},
             {"minecraft:seagrass", &sea.seagrass},
             {"minecraft:tall_seagrass", &sea.tall_seagrass},
             {"minecraft:kelp", &sea.kelp},
             {"minecraft:kelp_plant", &sea.kelp_plant},
             {"minecraft:sea_pickle", &sea.sea_pickle},
         }}) {
        auto block = named_block(blocks, name);
        if (!block) return std::unexpected(block.error());
        *into = *block;
    }
    auto corals = tag_members(blocks, tags, "minecraft:corals");
    if (!corals) return std::unexpected(corals.error());
    sea.corals = std::move(*corals);
    return sea;
}

// ── seagrass ────────────────────────────────────────────────────────────────

class SeagrassFeature final : public Feature {
public:
    SeagrassFeature(Sea sea, f32 probability) : sea_(std::move(sea)), probability_(probability) {}

    [[nodiscard]] std::string_view type_name() const override { return "seagrass"; }

    bool place(const FeatureContext&, FeatureLevel& level, FeatureRandom& random,
               BlockPos origin) const override {
        const i32 x_hi = random.next_int(8);
        const i32 x_lo = random.next_int(8);
        const i32 z_hi = random.next_int(8);
        const i32 z_lo = random.next_int(8);
        const i32 x    = origin.x + x_hi - x_lo;
        const i32 z    = origin.z + z_hi - z_lo;
        const BlockPos at{x, level.height(world::HeightmapType::OceanFloor, x, z), z};
        if (!sea_.is_water(level, at)) {
            return false;
        }
        const f64  roll = random.next_double();
        const bool tall = roll < static_cast<f64>(probability_);
        // Both heights need water here and a sturdy, non-magma floor; the tall
        // one also needs a full water block here, which "is water" already is.
        if (!sea_.seagrass_ground(level, at)) {
            return false;
        }
        if (tall) {
            const BlockPos above = at.above();
            if (sea_.is_water(level, above)) {
                const auto lower = with_property_value(*sea_.blocks, sea_.state_of(sea_.tall_seagrass),
                                                       "half", "lower");
                const auto upper = with_property_value(*sea_.blocks, lower, "half", "upper");
                (void)level.set_block(at.x, at.y, at.z, lower);
                (void)level.set_block(above.x, above.y, above.z, upper);
            }
        } else {
            (void)level.set_block(at.x, at.y, at.z, sea_.state_of(sea_.seagrass));
        }
        return true;
    }

private:
    Sea sea_;
    f32 probability_;
};

// ── kelp ────────────────────────────────────────────────────────────────────

class KelpFeature final : public Feature {
public:
    explicit KelpFeature(Sea sea) : sea_(std::move(sea)) {}

    [[nodiscard]] std::string_view type_name() const override { return "kelp"; }

    bool place(const FeatureContext&, FeatureLevel& level, FeatureRandom& random,
               BlockPos origin) const override {
        BlockPos at{origin.x, level.height(world::HeightmapType::OceanFloor, origin.x, origin.z),
                    origin.z};
        if (!sea_.is_water(level, at)) {
            return false;
        }
        i32       placed = 0;
        const i32 height = 1 + random.next_int(10);
        for (i32 step = 0; step <= height; ++step) {
            if (sea_.is_water(level, at) && sea_.is_water(level, at.above()) &&
                sea_.kelp_ground(level, at)) {
                if (step == height) {
                    const i32 age = random.next_int(4) + 20;
                    (void)level.set_block(at.x, at.y, at.z, head(age));
                    ++placed;
                } else {
                    (void)level.set_block(at.x, at.y, at.z, sea_.state_of(sea_.kelp_plant));
                }
            } else if (step > 0) {
                const BlockPos below = at.below();
                if (sea_.kelp_ground(level, below) && sea_.at(level, below.below()) != sea_.kelp) {
                    const i32 age = random.next_int(4) + 20;
                    (void)level.set_block(below.x, below.y, below.z, head(age));
                    ++placed;
                }
                break;
            }
            at = at.above();
        }
        return placed > 0;
    }

private:
    [[nodiscard]] registry::BlockStateId head(i32 age) const {
        return with_property_value(*sea_.blocks, sea_.state_of(sea_.kelp), "age",
                                   std::to_string(age));
    }

    Sea sea_;
};

// ── sea_pickle ──────────────────────────────────────────────────────────────

class SeaPickleFeature final : public Feature {
public:
    SeaPickleFeature(Sea sea, IntProviderRef count) : sea_(std::move(sea)), count_(std::move(count)) {}

    [[nodiscard]] std::string_view type_name() const override { return "sea_pickle"; }

    bool place(const FeatureContext&, FeatureLevel& level, FeatureRandom& random,
               BlockPos origin) const override {
        i32       placed = 0;
        const i32 count  = count_->sample(random);
        for (i32 k = 0; k < count; ++k) {
            const i32 x_hi = random.next_int(8);
            const i32 x_lo = random.next_int(8);
            const i32 z_hi = random.next_int(8);
            const i32 z_lo = random.next_int(8);
            const i32 x    = origin.x + x_hi - x_lo;
            const i32 z    = origin.z + z_hi - z_lo;
            const BlockPos at{x, level.height(world::HeightmapType::OceanFloor, x, z), z};
            const i32      pickles = random.next_int(4) + 1;
            if (!sea_.is_water(level, at) || !sea_.pickle_ground(level, at)) {
                continue;
            }
            (void)level.set_block(at.x, at.y, at.z, pickle(pickles));
            ++placed;
        }
        return placed > 0;
    }

private:
    [[nodiscard]] registry::BlockStateId pickle(i32 count) const {
        return with_property_value(*sea_.blocks, sea_.state_of(sea_.sea_pickle), "pickles",
                                   std::to_string(count));
    }

    Sea            sea_;
    IntProviderRef count_;
};

// ── The corals ──────────────────────────────────────────────────────────────

struct Coral {
    Sea                            sea;
    std::vector<registry::BlockId> blocks;       ///< #coral_blocks, in order
    std::vector<registry::BlockId> plants;       ///< #corals, in order
    std::vector<registry::BlockId> wall_fans;    ///< #wall_corals, in order

    [[nodiscard]] static registry::BlockId pick(const std::vector<registry::BlockId>& list,
                                                FeatureRandom&                         random) {
        return list[static_cast<usize>(random.next_int(static_cast<i32>(list.size())))];
    }

    /// `CoralFeature.placeCoralBlock`: the block, then maybe a plant or a
    /// pickle on top, then maybe a fan on each side.
    bool place_block(FeatureLevel& level, FeatureRandom& random, BlockPos pos,
                     registry::BlockStateId state) const {
        const auto&    b     = *sea.blocks;
        const BlockPos above = pos.above();
        const auto     here  = sea.at(level, pos);
        if ((here != sea.water && !holds(sea.corals, here)) || !sea.is_water(level, above)) {
            return false;
        }
        (void)level.set_block(pos.x, pos.y, pos.z, state);
        const f32 plant_roll = random.next_float();
        if (plant_roll < 0.25F) {
            const auto plant = pick(plants, random);
            (void)level.set_block(above.x, above.y, above.z, b.default_state(plant));
        } else {
            const f32 pickle_roll = random.next_float();
            if (pickle_roll < 0.05F) {
                const i32 pickles = random.next_int(4) + 1;
                (void)level.set_block(above.x, above.y, above.z,
                                      with_property_value(b, b.default_state(sea.sea_pickle),
                                                          "pickles", std::to_string(pickles)));
            }
        }
        for (usize side = 0; side < kHorizontal.size(); ++side) {
            const f32 fan_roll = random.next_float();
            if (fan_roll >= 0.2F) {
                continue;
            }
            const BlockPos beside = pos.offset(kHorizontal[side][0], 0, kHorizontal[side][1]);
            if (!sea.is_water(level, beside)) {
                continue;
            }
            const auto fan = pick(wall_fans, random);
            (void)level.set_block(beside.x, beside.y, beside.z,
                                  with_property_value(b, b.default_state(fan), "facing",
                                                      kHorizontalNames[side]));
        }
        return true;
    }
};

class CoralFeature final : public Feature {
public:
    enum class Shape : u8 { Tree, Claw, Mushroom };

    CoralFeature(Shape shape, Coral coral) : shape_(shape), coral_(std::move(coral)) {}

    [[nodiscard]] std::string_view type_name() const override {
        return shape_ == Shape::Tree ? "coral_tree"
               : shape_ == Shape::Claw ? "coral_claw"
                                       : "coral_mushroom";
    }

    bool place(const FeatureContext&, FeatureLevel& level, FeatureRandom& random,
               BlockPos origin) const override {
        const auto block = Coral::pick(coral_.blocks, random);
        const auto state = coral_.sea.blocks->default_state(block);
        switch (shape_) {
            case Shape::Tree:
                return tree(level, random, origin, state);
            case Shape::Claw:
                return claw(level, random, origin, state);
            case Shape::Mushroom:
                return mushroom(level, random, origin, state);
        }
        return false;
    }

private:
    static void shuffle(std::vector<usize>& list, FeatureRandom& random) {
        for (usize i = list.size(); i > 1; --i) {
            const auto j = static_cast<usize>(random.next_int(static_cast<i32>(i)));
            std::swap(list[i - 1], list[j]);
        }
    }

    bool tree(FeatureLevel& level, FeatureRandom& random, BlockPos pos,
              registry::BlockStateId state) const {
        BlockPos  cursor = pos;
        const i32 trunk  = random.next_int(3) + 1;
        for (i32 j = 0; j < trunk; ++j) {
            if (!coral_.place_block(level, random, cursor, state)) {
                return true;
            }
            cursor = cursor.above();
        }
        const BlockPos top      = cursor;
        const i32      branches = random.next_int(3) + 2;
        std::vector<usize> sides{0, 1, 2, 3};
        shuffle(sides, random);
        for (i32 b = 0; b < branches; ++b) {
            const auto dir = kHorizontal[sides[static_cast<usize>(b)]];
            cursor         = top.offset(dir[0], 0, dir[1]);
            const i32 length = random.next_int(5) + 2;
            i32       run    = 0;
            for (i32 n = 0; n < length && coral_.place_block(level, random, cursor, state); ++n) {
                ++run;
                cursor = cursor.above();
                if (n != 0 && run >= 2) {
                    const f32 roll = random.next_float();
                    if (roll < 0.25F) {
                        cursor = cursor.offset(dir[0], 0, dir[1]);
                        run    = 0;
                    }
                    continue;
                }
                if (n == 0) {
                    cursor = cursor.offset(dir[0], 0, dir[1]);
                    run    = 0;
                }
            }
        }
        return true;
    }

    bool claw(FeatureLevel& level, FeatureRandom& random, BlockPos pos,
              registry::BlockStateId state) const {
        if (!coral_.place_block(level, random, pos, state)) {
            return false;
        }
        const i32 main = random.next_int(4);
        const i32 arms = random.next_int(2) + 2;
        // The main direction, its clockwise and its counter-clockwise
        // neighbours, shuffled.
        std::vector<usize> order{static_cast<usize>(main), static_cast<usize>((main + 1) % 4),
                                 static_cast<usize>((main + 3) % 4)};
        shuffle(order, random);
        const auto forward = kHorizontal[static_cast<usize>(main)];
        for (i32 a = 0; a < arms; ++a) {
            const usize side    = order[static_cast<usize>(a)];
            const auto  dir     = kHorizontal[side];
            BlockPos    cursor  = pos;
            const i32   base    = random.next_int(2) + 1;
            cursor              = cursor.offset(dir[0], 0, dir[1]);
            std::array<i32, 3> step{};
            i32                 length = 0;
            if (side == static_cast<usize>(main)) {
                step   = {dir[0], 0, dir[1]};
                length = random.next_int(3) + 2;
            } else {
                cursor         = cursor.above();
                const bool up  = random.next_int(2) == 1;
                step           = up ? std::array<i32, 3>{0, 1, 0}
                                    : std::array<i32, 3>{dir[0], 0, dir[1]};
                length         = random.next_int(3) + 3;
            }
            for (i32 l = 0; l < base && coral_.place_block(level, random, cursor, state); ++l) {
                cursor = cursor.offset(step[0], step[1], step[2]);
            }
            cursor = cursor.offset(-step[0], -step[1], -step[2]);
            cursor = cursor.above();
            bool broken = false;
            for (i32 l = 0; l < length && !broken; ++l) {
                cursor = cursor.offset(forward[0], 0, forward[1]);
                if (!coral_.place_block(level, random, cursor, state)) {
                    broken = true;
                    continue;
                }
                const f32 roll = random.next_float();
                if (roll < 0.25F) {
                    cursor = cursor.above();
                }
            }
        }
        return true;
    }

    bool mushroom(FeatureLevel& level, FeatureRandom& random, BlockPos pos,
                  registry::BlockStateId state) const {
        const i32 size_y = random.next_int(3) + 3;
        const i32 size_x = random.next_int(3) + 3;
        const i32 size_z = random.next_int(3) + 3;
        const i32 sink   = random.next_int(3) + 1;
        for (i32 m = 0; m <= size_x; ++m) {
            for (i32 n = 0; n <= size_y; ++n) {
                for (i32 o = 0; o <= size_z; ++o) {
                    const bool x_edge = m == 0 || m == size_x;
                    const bool y_edge = n == 0 || n == size_y;
                    const bool z_edge = o == 0 || o == size_z;
                    // Not an edge of the box, and on its surface.
                    if ((x_edge && y_edge) || (z_edge && y_edge) || (x_edge && z_edge)) {
                        continue;
                    }
                    if (!(x_edge || y_edge || z_edge)) {
                        continue;
                    }
                    const f32 roll = random.next_float();
                    if (roll < 0.1F) {
                        continue;
                    }
                    (void)coral_.place_block(level, random,
                                             {pos.x + m, pos.y + n - sink, pos.z + o}, state);
                }
            }
        }
        return true;
    }

    Shape shape_;
    Coral coral_;
};

// ── underwater_magma ────────────────────────────────────────────────────────

class UnderwaterMagmaFeature final : public Feature {
public:
    UnderwaterMagmaFeature(Sea sea, i32 search, i32 radius, f32 chance)
        : sea_(std::move(sea)), search_(search), radius_(radius), chance_(chance) {}

    [[nodiscard]] std::string_view type_name() const override { return "underwater_magma"; }

    bool place(const FeatureContext&, FeatureLevel& level, FeatureRandom& random,
               BlockPos origin) const override {
        if (!sea_.is_water(level, origin)) {
            return false;
        }
        // `Column.scan` down through water; the edge must not be water.
        BlockPos cursor = origin;
        for (i32 i = 1; i < search_ && sea_.is_water(level, cursor); ++i) {
            cursor = cursor.below();
        }
        if (sea_.is_water(level, cursor)) {
            return false;
        }
        const BlockPos floor{origin.x, cursor.y, origin.z};
        i32            placed = 0;
        for (i32 z = floor.z - radius_; z <= floor.z + radius_; ++z) {
            for (i32 y = floor.y - radius_; y <= floor.y + radius_; ++y) {
                for (i32 x = floor.x - radius_; x <= floor.x + radius_; ++x) {
                    const f32 roll = random.next_float();
                    if (roll >= chance_) {
                        continue;
                    }
                    const BlockPos at{x, y, z};
                    if (!valid(level, at)) {
                        continue;
                    }
                    (void)level.set_block(x, y, z, sea_.state_of(sea_.magma));
                    ++placed;
                }
            }
        }
        return placed > 0;
    }

private:
    [[nodiscard]] bool water_or_air(const FeatureLevel& level, BlockPos pos) const {
        const auto block = sea_.at(level, pos);
        return block == sea_.water || sea_.blocks->is_air(block);
    }

    [[nodiscard]] bool valid(const FeatureLevel& level, BlockPos pos) const {
        if (water_or_air(level, pos) || water_or_air(level, pos.below())) {
            return false;
        }
        for (const auto& side : kHorizontal) {
            if (water_or_air(level, pos.offset(side[0], 0, side[1]))) {
                return false;
            }
        }
        return true;
    }

    Sea sea_;
    i32 search_;
    i32 radius_;
    f32 chance_;
};

[[nodiscard]] f64 number_field(Json node, std::string_view key, f64 fallback) {
    f64 value = fallback;
    if (node.at_key(key).get(value) == simdjson::SUCCESS) {
        return value;
    }
    i64 whole = 0;
    if (node.at_key(key).get(whole) == simdjson::SUCCESS) {
        return static_cast<f64>(whole);
    }
    return fallback;
}

}  // namespace

ClaimedFeature parse_ocean_feature(std::string_view kind, Json config,
                                   const registry::BlockRegistry& blocks, const BlockTags& tags) {
    const bool coral = kind == "coral_tree" || kind == "coral_claw" || kind == "coral_mushroom";
    if (kind != "seagrass" && kind != "kelp" && kind != "sea_pickle" &&
        kind != "underwater_magma" && !coral) {
        return std::nullopt;
    }
    auto sea = sea_vocabulary(blocks, tags);
    if (!sea) return std::unexpected(sea.error());

    if (kind == "seagrass") {
        return std::static_pointer_cast<const Feature>(std::make_shared<const SeagrassFeature>(
            std::move(*sea), static_cast<f32>(number_field(config, "probability", 0.0))));
    }
    if (kind == "kelp") {
        return std::static_pointer_cast<const Feature>(
            std::make_shared<const KelpFeature>(std::move(*sea)));
    }
    if (kind == "sea_pickle") {
        auto field = config.at_key("count");
        if (field.error() != simdjson::SUCCESS) return std::unexpected(FeatureError::Malformed);
        auto count = parse_int_provider(field.value());
        if (!count) return std::unexpected(count.error());
        return std::static_pointer_cast<const Feature>(
            std::make_shared<const SeaPickleFeature>(std::move(*sea), *count));
    }
    if (kind == "underwater_magma") {
        return std::static_pointer_cast<const Feature>(std::make_shared<const UnderwaterMagmaFeature>(
            std::move(*sea), static_cast<i32>(number_field(config, "floor_search_range", 5)),
            static_cast<i32>(number_field(config, "placement_radius_around_floor", 1)),
            static_cast<f32>(number_field(config, "placement_probability_per_valid_position", 0.5))));
    }

    // In file order, from the tags already loaded: the index a coral draws is
    // into this order. (An earlier form re-read the tag files through a path
    // relative to the working directory, and failed under ctest.)
    auto coral_blocks = tags.ordered("minecraft:coral_blocks");
    auto corals       = tags.ordered("minecraft:corals");
    auto wall_corals  = tags.ordered("minecraft:wall_corals");
    if (coral_blocks.empty() || corals.empty() || wall_corals.empty()) {
        OV_LOG_ERROR("worldgen: a coral tag is empty or unknown, so {} is refused", kind);
        return std::unexpected(FeatureError::Missing);
    }
    Coral c{std::move(*sea), std::move(coral_blocks), std::move(corals), std::move(wall_corals)};
    const auto shape = kind == "coral_tree"   ? CoralFeature::Shape::Tree
                       : kind == "coral_claw" ? CoralFeature::Shape::Claw
                                              : CoralFeature::Shape::Mushroom;
    return std::static_pointer_cast<const Feature>(
        std::make_shared<const CoralFeature>(shape, std::move(c)));
}

}  // namespace ov::worldgen
