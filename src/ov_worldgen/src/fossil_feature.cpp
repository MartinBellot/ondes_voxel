#define OV_LOG_CATEGORY "worldgen"

// ── worldgen-3 ── `fossil`: a skull or a spine of bone blocks buried under
// the desert and the swamps, with a thinner overlay of ore laid on the same
// bones.
//
// The two templates come from the server jar (structure_template.cpp reads
// them there at run time — nothing is extracted or committed). What the
// feature adds around them:
//
//   * a rotation and a template index, drawn first;
//   * the footprint's lowest ocean-floor height, from the origin's own height
//     down, and a burial 15 to 24 blocks below it — never closer than ten to
//     the floor of the world;
//   * a refusal when more than `max_empty_corners_allowed` of the eight
//     corners of the box are air or fluid, so a fossil does not hang in a
//     cave;
//   * both templates placed through their processor lists with the
//     *feature's* random: `block_rot` draws once per block in template order,
//     and the palette is drawn too, even from a single palette.
//
// Measured against a probe world of the real game: docs/provenance/features.md,
// « Les fossiles ».

#include "overworld_feature.hpp"

#include "ov/base/log.hpp"
#include "ov/worldgen/structure_template.hpp"

#include <fstream>
#include <iterator>

namespace ov::worldgen {

namespace {

/// A FeatureLevel seen as a StructureLevel. Fossils carry no block entity.
class FeatureAsStructureLevel final : public StructureLevel {
public:
    explicit FeatureAsStructureLevel(FeatureLevel& level) : level_(&level) {}

    [[nodiscard]] registry::BlockStateId block_at(i32 x, i32 y, i32 z) const override {
        return level_->block_at(x, y, z);
    }
    bool set_block(i32 x, i32 y, i32 z, registry::BlockStateId state) override {
        return level_->set_block(x, y, z, state);
    }
    [[nodiscard]] i32 height(world::HeightmapType type, i32 x, i32 z) const override {
        return level_->height(type, x, z);
    }
    [[nodiscard]] std::string_view biome_at(i32 x, i32 y, i32 z) const override {
        return level_->biome_at(x, y, z);
    }
    [[nodiscard]] i32 min_y() const override { return level_->min_y(); }
    [[nodiscard]] i32 world_height() const override { return level_->world_height(); }
    [[nodiscard]] i32 sea_level() const override { return level_->sea_level(); }
    void set_block_entity(i32, i32, i32, nbt::Tag) override {}
    [[nodiscard]] const nbt::Tag* block_entity(i32, i32, i32) const override { return nullptr; }

private:
    FeatureLevel* level_;
};

class FossilFeature final : public Feature {
public:
    FossilFeature(std::vector<std::string> fossils, std::vector<std::string> overlays,
                  std::vector<ProcessorRef> fossil_processors,
                  std::vector<ProcessorRef> overlay_processors, i32 max_empty_corners)
        : fossils_(std::move(fossils)),
          overlays_(std::move(overlays)),
          fossil_processors_(std::move(fossil_processors)),
          overlay_processors_(std::move(overlay_processors)),
          max_empty_corners_(max_empty_corners) {}

    [[nodiscard]] std::string_view type_name() const override { return "fossil"; }

    bool place(const FeatureContext& context, FeatureLevel& level, FeatureRandom& random,
               BlockPos origin) const override {
        if (context.templates == nullptr || context.blocks == nullptr) {
            return false;
        }
        const auto rotation = static_cast<Rotation>(random.next_int(4));
        const auto index    = static_cast<usize>(random.next_int(static_cast<i32>(fossils_.size())));
        const StructureTemplate* fossil  = context.templates->find(fossils_[index]);
        const StructureTemplate* overlay = context.templates->find(overlays_[index]);
        if (fossil == nullptr || overlay == nullptr) {
            return false;
        }

        // What may be written: the origin's chunk and one chunk around it.
        const i32         chunk_x = (origin.x >> 4) * 16;
        const i32         chunk_z = (origin.z >> 4) * 16;
        const BoundingBox clip{chunk_x - 16, level.min_y(),      chunk_z - 16,
                               chunk_x + 31, level.max_y(),  chunk_z + 31};

        const bool quarter = rotation == Rotation::Clockwise90 ||
                             rotation == Rotation::CounterClockwise90;
        const i32 size_x = quarter ? fossil->size.z : fossil->size.x;
        const i32 size_z = quarter ? fossil->size.x : fossil->size.z;
        const BlockPos corner = origin.offset(-size_x / 2, 0, -size_z / 2);

        i32 floor = origin.y;
        for (i32 dx = 0; dx < size_x; ++dx) {
            for (i32 dz = 0; dz < size_z; ++dz) {
                floor = std::min(floor, level.height(world::HeightmapType::OceanFloorWG,
                                                     corner.x + dx, corner.z + dz));
            }
        }
        const i32      depth = std::max(floor - 15 - random.next_int(10), level.min_y() + 10);
        const BlockPos zero  = zero_position({corner.x, depth, corner.z}, fossil->size, rotation);

        const BoundingBox box = fossil->bounding_box(zero, Mirror::None, rotation, BlockPos{});
        if (empty_corners(*context.blocks, level, box) > max_empty_corners_) {
            return false;
        }

        FeatureAsStructureLevel view{level};
        PlaceSettings           settings;
        settings.rotation         = rotation;
        settings.clip             = clip;
        settings.processor_random = &random;
        settings.processors       = fossil_processors_;
        settings.palette = static_cast<u32>(random.next_int(static_cast<i32>(fossil->palettes.size())));
        (void)place_template(view, *fossil, zero, settings, *context.blocks);
        settings.processors = overlay_processors_;
        settings.palette = static_cast<u32>(random.next_int(static_cast<i32>(overlay->palettes.size())));
        (void)place_template(view, *overlay, zero, settings, *context.blocks);
        return true;
    }

private:
    /// `getZeroPositionWithTransform` without a mirror: where the template's
    /// origin goes so that the turned footprint starts at `pos`.
    [[nodiscard]] static BlockPos zero_position(BlockPos pos, BlockPos size, Rotation rotation) {
        const i32 last_x = size.x - 1;
        const i32 last_z = size.z - 1;
        switch (rotation) {
            case Rotation::CounterClockwise90: return pos.offset(0, 0, last_x);
            case Rotation::Clockwise90: return pos.offset(last_z, 0, 0);
            case Rotation::Clockwise180: return pos.offset(last_x, 0, last_z);
            case Rotation::None: break;
        }
        return pos;
    }

    [[nodiscard]] static i32 empty_corners(const registry::BlockRegistry& blocks,
                                           const FeatureLevel& level, const BoundingBox& box) {
        const auto water = blocks.find_block("minecraft:water");
        const auto lava  = blocks.find_block("minecraft:lava");
        i32 empty = 0;
        for (const i32 x : {box.min_x, box.max_x}) {
            for (const i32 y : {box.min_y, box.max_y}) {
                for (const i32 z : {box.min_z, box.max_z}) {
                    const auto block = blocks.block_of(level.block_at(x, y, z));
                    if (blocks.is_air(block) || (water && block == *water) ||
                        (lava && block == *lava)) {
                        ++empty;
                    }
                }
            }
        }
        return empty;
    }

    std::vector<std::string>  fossils_;
    std::vector<std::string>  overlays_;
    std::vector<ProcessorRef> fossil_processors_;
    std::vector<ProcessorRef> overlay_processors_;
    i32                       max_empty_corners_;
};

[[nodiscard]] std::expected<std::vector<std::string>, FeatureError> names(Json config,
                                                                         std::string_view key) {
    simdjson::dom::array list;
    if (config.at_key(key).get(list) != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    std::vector<std::string> out;
    for (auto entry : list) {
        std::string_view name;
        if (entry.get(name) != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        out.emplace_back(name);
    }
    return out;
}

/// A processor list named (a file of `worldgen/processor_list/`) or held inline.
[[nodiscard]] std::expected<std::vector<ProcessorRef>, FeatureError> processors(
    Json config, std::string_view key, const registry::BlockRegistry& blocks, const BlockTags& tags,
    const FeatureResolver& resolve) {
    auto node = config.at_key(key);
    if (node.error() != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    std::string      text;
    std::string_view named;
    if (node.get(named) == simdjson::SUCCESS) {
        const auto path =
            resolve.data_root / "worldgen" / "processor_list" / (strip_namespace(named) + ".json");
        std::ifstream file{path};
        if (!file) {
            OV_LOG_ERROR("worldgen: fossil: processor list {} is not at {}", named, path.string());
            return std::unexpected(FeatureError::Missing);
        }
        text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    } else {
        text = simdjson::minify(node.value());
    }
    std::string detail;
    auto        parsed = ProcessorList::parse_json(text, blocks, &tags, &detail);
    if (!parsed) {
        OV_LOG_ERROR("worldgen: fossil: processor list {}: {}", key, detail);
        return std::unexpected(FeatureError::Unsupported);
    }
    return std::move(parsed->processors);
}

}  // namespace

ClaimedFeature parse_fossil_feature(std::string_view kind, Json config,
                                    const registry::BlockRegistry& blocks, const BlockTags& tags,
                                    const FeatureResolver& resolve) {
    if (kind != "fossil") {
        return std::nullopt;
    }
    auto fossils  = names(config, "fossil_structures");
    auto overlays = names(config, "overlay_structures");
    if (!fossils || !overlays || fossils->empty() || fossils->size() != overlays->size()) {
        return std::unexpected(FeatureError::Malformed);
    }
    auto fossil_processors  = processors(config, "fossil_processors", blocks, tags, resolve);
    auto overlay_processors = processors(config, "overlay_processors", blocks, tags, resolve);
    if (!fossil_processors) {
        return std::unexpected(fossil_processors.error());
    }
    if (!overlay_processors) {
        return std::unexpected(overlay_processors.error());
    }
    i64 corners = 4;
    (void)config.at_key("max_empty_corners_allowed").get(corners);
    return std::static_pointer_cast<const Feature>(std::make_shared<const FossilFeature>(
        std::move(*fossils), std::move(*overlays), std::move(*fossil_processors),
        std::move(*overlay_processors), static_cast<i32>(corners)));
}

}  // namespace ov::worldgen
