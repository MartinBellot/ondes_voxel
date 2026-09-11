#define OV_LOG_CATEGORY "worldgen"

#include "overworld_feature.hpp"

#include "ov/base/log.hpp"

namespace ov::worldgen {

ClaimedFeature parse_overworld_feature(std::string_view kind, Json config,
                                       const registry::BlockRegistry& blocks,
                                       const BlockTags& tags, const FeatureResolver& resolve) {
    if (auto claimed = parse_huge_mushroom_feature(kind, config, blocks, tags)) {
        return claimed;
    }
    if (auto claimed = parse_ocean_feature(kind, config, blocks, tags)) {
        return claimed;
    }
    if (auto claimed = parse_ice_feature(kind, config, blocks, tags)) {
        return claimed;
    }
    if (auto claimed = parse_cave_feature(kind, config, blocks, tags, resolve)) {
        return claimed;
    }
    if (auto claimed = parse_terrain_feature(kind, config, blocks, tags)) {
        return claimed;
    }
    return std::nullopt;
}

namespace {

class ReplaceablePredicate final : public BlockPredicate {
public:
    ReplaceablePredicate(std::vector<u16> members, BlockPos offset,
                         const registry::BlockRegistry& blocks)
        : members_(std::move(members)), offset_(offset), blocks_(&blocks) {}

    [[nodiscard]] bool test(const FeatureLevel& level, BlockPos at) const override {
        const auto state = level.block_at(at.x + offset_.x, at.y + offset_.y, at.z + offset_.z);
        return holds(members_, blocks_->block_of(state));
    }

private:
    std::vector<u16>               members_;
    BlockPos                       offset_;
    const registry::BlockRegistry* blocks_;
};

class SolidPredicate final : public BlockPredicate {
public:
    SolidPredicate(BlockPos offset, const registry::BlockRegistry& blocks)
        : offset_(offset), blocks_(&blocks) {}

    [[nodiscard]] bool test(const FeatureLevel& level, BlockPos at) const override {
        return is_solid(*blocks_,
                        level.block_at(at.x + offset_.x, at.y + offset_.y, at.z + offset_.z));
    }

    [[nodiscard]] static bool is_solid(const registry::BlockRegistry& blocks,
                                       registry::BlockStateId          state) {
        const auto boxes = blocks.collision_boxes(state);
        if (boxes.empty()) {
            return false;
        }
        i32 lo_x = 127, lo_y = 127, lo_z = 127;
        i32 hi_x = -128, hi_y = -128, hi_z = -128;
        for (const auto& box : boxes) {
            lo_x = std::min<i32>(lo_x, box.min_x);
            lo_y = std::min<i32>(lo_y, box.min_y);
            lo_z = std::min<i32>(lo_z, box.min_z);
            hi_x = std::max<i32>(hi_x, box.max_x);
            hi_y = std::max<i32>(hi_y, box.max_y);
            hi_z = std::max<i32>(hi_z, box.max_z);
        }
        // Thirty-seconds of a block. `AABB.getSize()` is the mean of the three
        // extents.
        const f64 x = static_cast<f64>(hi_x - lo_x) / 32.0;
        const f64 y = static_cast<f64>(hi_y - lo_y) / 32.0;
        const f64 z = static_cast<f64>(hi_z - lo_z) / 32.0;
        if ((x + y + z) / 3.0 >= 0.7291666666666666) {
            return true;
        }
        return y >= 1.0;
    }

private:
    BlockPos                       offset_;
    const registry::BlockRegistry* blocks_;
};

/// `matching_fluids`: the *fluid state* at a position, which is not its block.
///
/// Water at level 0 and any waterlogged block hold `minecraft:water`, the
/// source; water at any other level holds `minecraft:flowing_water`; lava the
/// same without the waterlogged case; everything else holds
/// `minecraft:empty`.
class MatchingFluidsPredicate final : public BlockPredicate {
public:
    enum Fluid : u8 { kWater = 1, kFlowingWater = 2, kLava = 4, kFlowingLava = 8, kEmpty = 16 };

    MatchingFluidsPredicate(u8 wanted, BlockPos offset, const registry::BlockRegistry& blocks,
                            registry::BlockId water, registry::BlockId lava)
        : wanted_(wanted), offset_(offset), blocks_(&blocks), water_(water), lava_(lava) {}

    [[nodiscard]] bool test(const FeatureLevel& level, BlockPos at) const override {
        const auto state = level.block_at(at.x + offset_.x, at.y + offset_.y, at.z + offset_.z);
        return (fluid_of(state) & wanted_) != 0;
    }

private:
    [[nodiscard]] u8 fluid_of(registry::BlockStateId state) const {
        const auto block = blocks_->block_of(state);
        if (block == water_ || block == lava_) {
            const auto level  = blocks_->find_property(block, "level");
            const bool source = !level || blocks_->property_value(state, *level) == "0";
            if (block == water_) {
                return source ? kWater : kFlowingWater;
            }
            return source ? kLava : kFlowingLava;
        }
        if (const auto waterlogged = blocks_->find_property(block, "waterlogged");
            waterlogged && blocks_->property_value(state, *waterlogged) == "true") {
            return kWater;
        }
        return kEmpty;
    }

    u8                             wanted_;
    BlockPos                       offset_;
    const registry::BlockRegistry* blocks_;
    registry::BlockId              water_;
    registry::BlockId              lava_;
};

[[nodiscard]] BlockPos predicate_offset(Json node) {
    simdjson::dom::array values;
    if (node.at_key("offset").get(values) != simdjson::SUCCESS) {
        return {};
    }
    std::array<i32, 3> parts{};
    usize              index = 0;
    for (auto value : values) {
        i64 number = 0;
        if (index < parts.size() && value.get(number) == simdjson::SUCCESS) {
            parts[index] = static_cast<i32>(number);
        }
        ++index;
    }
    return {parts[0], parts[1], parts[2]};
}

}  // namespace

std::optional<std::expected<BlockPredicateRef, FeatureError>> parse_shape_predicate(
    std::string_view kind, Json node, const registry::BlockRegistry& blocks,
    const BlockTags& tags) {
    if (kind == "replaceable") {
        auto members = tag_members(blocks, tags, "minecraft:replaceable");
        if (!members) {
            return std::unexpected(members.error());
        }
        return std::static_pointer_cast<const BlockPredicate>(std::make_shared<const ReplaceablePredicate>(
            std::move(*members), predicate_offset(node), blocks));
    }
    if (kind == "solid") {
        return std::static_pointer_cast<const BlockPredicate>(
            std::make_shared<const SolidPredicate>(predicate_offset(node), blocks));
    }
    if (kind == "matching_fluids") {
        std::vector<std::string_view> names;
        auto                          field = node.at_key("fluids");
        std::string_view              single;
        simdjson::dom::array          list;
        if (field.get(single) == simdjson::SUCCESS) {
            names.push_back(single);
        } else if (field.get(list) == simdjson::SUCCESS) {
            for (auto value : list) {
                std::string_view name;
                if (value.get(name) != simdjson::SUCCESS) {
                    return std::unexpected(FeatureError::Malformed);
                }
                names.push_back(name);
            }
        } else {
            return std::unexpected(FeatureError::Malformed);
        }
        u8 wanted = 0;
        for (const std::string_view raw : names) {
            const std::string name = qualify(raw);
            if (name == "minecraft:water") wanted |= MatchingFluidsPredicate::kWater;
            else if (name == "minecraft:flowing_water") wanted |= MatchingFluidsPredicate::kFlowingWater;
            else if (name == "minecraft:lava") wanted |= MatchingFluidsPredicate::kLava;
            else if (name == "minecraft:flowing_lava") wanted |= MatchingFluidsPredicate::kFlowingLava;
            else if (name == "minecraft:empty") wanted |= MatchingFluidsPredicate::kEmpty;
            else {
                OV_LOG_ERROR("worldgen: matching_fluids names unknown fluid {}", name);
                return std::unexpected(FeatureError::Malformed);
            }
        }
        auto water = named_block(blocks, "minecraft:water");
        auto lava  = named_block(blocks, "minecraft:lava");
        if (!water || !lava) {
            return std::unexpected(FeatureError::Missing);
        }
        return std::static_pointer_cast<const BlockPredicate>(
            std::make_shared<const MatchingFluidsPredicate>(wanted, predicate_offset(node), blocks,
                                                            *water, *lava));
    }
    return std::nullopt;
}

bool is_solid(const registry::BlockRegistry& blocks, registry::BlockStateId state) noexcept {
    return SolidPredicate::is_solid(blocks, state);
}

bool is_solid_render(const registry::BlockRegistry& blocks,
                     registry::BlockStateId          state) noexcept {
    const auto block = blocks.block_of(state);
    if (!blocks.blocks_sky_light(block)) {
        return false;
    }
    using Face = registry::BlockRegistry::Face;
    for (const Face face :
         {Face::Down, Face::Up, Face::North, Face::South, Face::West, Face::East}) {
        if (!blocks.face_is_sturdy(state, face)) {
            return false;
        }
    }
    return true;
}

std::expected<std::vector<u16>, FeatureError> tag_members(const registry::BlockRegistry& blocks,
                                                         const BlockTags& tags,
                                                         std::string_view tag) {
    if (!tags.known(tag)) {
        OV_LOG_ERROR("worldgen: a feature needs tag {}, which was not exported", tag);
        return std::unexpected(FeatureError::Missing);
    }
    std::vector<u16> out;
    for (usize index = 0; index < blocks.block_count(); ++index) {
        const registry::BlockId block{static_cast<u16>(index)};
        if (tags.contains(tag, block)) {
            out.push_back(block.value());
        }
    }
    std::ranges::sort(out);
    return out;
}

std::expected<registry::BlockId, FeatureError> named_block(const registry::BlockRegistry& blocks,
                                                          std::string_view name) {
    if (const auto block = blocks.find_block(name)) {
        return *block;
    }
    OV_LOG_ERROR("worldgen: block {} is missing from the registry", name);
    return std::unexpected(FeatureError::Missing);
}

registry::BlockStateId with_property_value(const registry::BlockRegistry& blocks,
                                           registry::BlockStateId state, std::string_view name,
                                           std::string_view value) noexcept {
    const auto property = blocks.find_property(blocks.block_of(state), name);
    if (!property) {
        return state;
    }
    for (usize index = 0; index < property->values.size(); ++index) {
        if (property->values[index] == value) {
            return blocks.with_property(state, *property, static_cast<u16>(index));
        }
    }
    return state;
}

}  // namespace ov::worldgen
