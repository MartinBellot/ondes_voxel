#define OV_LOG_CATEGORY "worldgen"

#include "ov/worldgen/structure_pieces.hpp"

#include "ov/base/log.hpp"
#include "ov/math/random.hpp"
#include "ov/worldgen/structure_set.hpp"
#include "buried_treasure.hpp"  // ── treasure ──
#include "ruined_portal.hpp"    // ── portals ──
#include "scattered.hpp"      // ── temples ──

#include <simdjson.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <string>
#include <utility>

namespace ov::worldgen {

namespace {

/// The template families this file places. Loaded once, eagerly.
constexpr std::array<std::string_view, 10> kFamilies{
    "igloo/", "shipwreck/", "underwater_ruin/", "ruined_portal/",
    "nether_fossils/",  // ── nether-2 ──
    // ── jigsaw ── the pools' templates
    "village/", "pillager_outpost/", "bastion/", "ancient_city/", "trail_ruins/"};

/// ── nether-2 ── The Nether fossil's search: from a drawn height down to the
/// sea level, the first air above something solid, in the base column.
constexpr i32 kNetherSeaLevel     = 32;
constexpr i32 kNetherFossilLowest = 32;   // `height`'s min_inclusive, absolute
constexpr i32 kNetherFossilTop    = 125;  // below_top 2 of a 128-deep generator
constexpr i32 kNetherFossils      = 14;

// ── Template lists, in the game's order ─────────────────────────────────────
//
// The order is the draw: a start picks `list[nextInt(size)]`. It is not in any
// data file, so it was read back off the game's own starts — for each start,
// the draw our random makes and the template the game stored. See
// docs/provenance/structures.md § 13 for which slots were observed and which
// follow from the pattern.

constexpr std::array<std::string_view, 20> kShipwrecks{"with_mast",
                                                       "upsidedown_full",
                                                       "upsidedown_fronthalf",
                                                       "upsidedown_backhalf",
                                                       "sideways_full",
                                                       "sideways_fronthalf",
                                                       "sideways_backhalf",
                                                       "rightsideup_full",
                                                       "rightsideup_fronthalf",
                                                       "rightsideup_backhalf",
                                                       "with_mast_degraded",
                                                       "upsidedown_full_degraded",
                                                       "upsidedown_fronthalf_degraded",
                                                       "upsidedown_backhalf_degraded",
                                                       "sideways_full_degraded",
                                                       "sideways_fronthalf_degraded",
                                                       "sideways_backhalf_degraded",
                                                       "rightsideup_full_degraded",
                                                       "rightsideup_fronthalf_degraded",
                                                       "rightsideup_backhalf_degraded"};

/// A beached ship is never upside down.
constexpr std::array<std::string_view, 11> kBeachedShipwrecks{"with_mast",
                                                              "sideways_full",
                                                              "sideways_fronthalf",
                                                              "sideways_backhalf",
                                                              "rightsideup_full",
                                                              "rightsideup_fronthalf",
                                                              "rightsideup_backhalf",
                                                              "with_mast_degraded",
                                                              "rightsideup_full_degraded",
                                                              "rightsideup_fronthalf_degraded",
                                                              "rightsideup_backhalf_degraded"};

constexpr std::array<i32, 4> kBigColdRuins{1, 2, 3, 8};
constexpr std::array<i32, 4> kBigWarmRuins{4, 5, 6, 7};

constexpr BlockPos kShipwreckPivot{4, 0, 15};

/// The igloo's three pieces each turn about their own pivot, chosen so that
/// the three pivots stack on one column — which is why their offsets do not
/// rotate with the igloo.
constexpr BlockPos kIglooTopPivot{3, 5, 5};
constexpr BlockPos kIglooMiddlePivot{1, 3, 1};
constexpr BlockPos kIglooBottomPivot{3, 6, 7};

/// Every structure of this file starts its pieces at this height, before the
/// terrain is consulted.
constexpr i32 kPlaceholderY = 90;

[[nodiscard]] Rotation random_rotation(math::LegacyRandomSource& random) {
    return static_cast<Rotation>(random.next_int(4));
}

[[nodiscard]] std::string strip_namespace(std::string_view name) {
    const auto colon = name.find(':');
    return std::string{colon == std::string_view::npos ? name : name.substr(colon + 1)};
}

/// Who a data marker's chest belongs to.
[[nodiscard]] std::string_view shipwreck_loot(std::string_view marker) {
    if (marker == "map_chest") {
        return "minecraft:chests/shipwreck_map";
    }
    if (marker == "treasure_chest") {
        return "minecraft:chests/shipwreck_treasure";
    }
    if (marker == "supply_chest") {
        return "minecraft:chests/shipwreck_supply";
    }
    return {};
}

}  // namespace

i32 step_ordinal(std::string_view step) noexcept {
    constexpr std::array<std::string_view, 11> kSteps{
        "raw_generation",        "lakes",
        "local_modifications",   "underground_structures",
        "surface_structures",    "strongholds",
        "underground_ores",      "underground_decoration",
        "fluid_springs",         "vegetal_decoration",
        "top_layer_modification"};
    for (usize index = 0; index < kSteps.size(); ++index) {
        if (kSteps[index] == step) {
            return static_cast<i32>(index);
        }
    }
    return -1;
}

i32 structure_step_index(const StructurePlacer& placer, std::string_view structure) noexcept {
    const StructureDefinition* self = placer.find(structure);
    if (self == nullptr) {
        return -1;
    }
    i32 index = 0;
    for (const StructureDefinition& other : placer.structures()) {
        if (other.step == self->step && other.name < self->name) {
            ++index;
        }
    }
    return index;
}

std::string_view to_string(PieceKind kind) noexcept {
    switch (kind) {
        case PieceKind::Igloo: return "igloo";
        case PieceKind::Shipwreck: return "shipwreck";
        case PieceKind::OceanRuin: return "ocean_ruin";
        case PieceKind::RuinedPortal: return "ruined_portal";
        case PieceKind::BuriedTreasure: return "buried_treasure";
        case PieceKind::NetherFossil: return "nether_fossil";  // ── nether-2 ──
        case PieceKind::Scattered: return "scattered";         // ── temples ──
        case PieceKind::Jigsaw: return "jigsaw";               // ── jigsaw ──
    }
    return "?";
}

/// One entry of a ruined portal's `setups`.
struct PortalSetup {
    std::string placement;
    f32         air_pocket_probability{0.0F};
    f32         mossiness{0.0F};
    bool        overgrown{false};
    bool        vines{false};
    bool        can_be_cold{false};
    bool        replace_with_blackstone{false};
    f32         weight{1.0F};
};

struct StructureBuilder::Impl {
    const registry::BlockRegistry*                               blocks{nullptr};
    const BlockTags*                                             tags{nullptr};
    std::optional<TemplateLibrary>                               library;
    std::map<std::string, std::vector<PortalSetup>, std::less<>> portal_setups;

    std::vector<std::string_view> ignore_structure_and_air{"minecraft:structure_block",
                                                           "minecraft:air"};
    ProcessorRef                  structure_and_air;
    ProcessorRef                  jigsaw_replacement;
    /// ── treasure ── The states a buried treasure's chest rests on.
    std::vector<registry::BlockStateId> treasure_support;

    registry::BlockStateId chest_default{};
    registry::BlockStateId water_default{};

    /// ── nether-2 ── `#has_structure/nether_fossil`: the biomes a fossil may
    /// stand in, tested at the point its search found.
    std::vector<std::string> fossil_biomes;

    /// ── jigsaw ── The pools, over the same templates.
    std::optional<JigsawLibrary> jigsaw;

    [[nodiscard]] const StructureTemplate* find(std::string_view name) const {
        return library->find(name);
    }

    /// Make the block entity at `pos` carry a loot table, as the game's
    /// `setLootTable(level, random, pos, table)` does: the seed is the next
    /// long of the chunk's structure random.
    bool set_loot(StructureLevel& level, BlockPos pos, std::string_view table,
                  FeatureRandom& random) const {
        const nbt::Tag* existing = level.block_entity(pos.x, pos.y, pos.z);
        nbt::Tag        data     = existing != nullptr ? *existing : nbt::Tag::make_compound();
        if (data.find("id") == nullptr) {
            data.put("id", nbt::Tag{std::string{"minecraft:chest"}});
        }
        data.erase("Items");
        data.put("LootTable", nbt::Tag{std::string{table}});
        data.put("LootTableSeed", nbt::Tag{random.next_long()});
        level.set_block_entity(pos.x, pos.y, pos.z, std::move(data));
        return true;
    }
};

StructureBuilder::StructureBuilder() : impl_(std::make_unique<Impl>()) {}

StructureBuilder::StructureBuilder(StructureBuilder&&) noexcept            = default;
StructureBuilder& StructureBuilder::operator=(StructureBuilder&&) noexcept = default;
StructureBuilder::~StructureBuilder()                                      = default;

std::expected<StructureBuilder, TemplateError> StructureBuilder::load(
    const std::filesystem::path& server_jar, const std::filesystem::path& data_root,
    const registry::BlockRegistry& blocks, const BlockTags& tags, std::string* detail) {
    auto library = TemplateLibrary::open(server_jar, blocks, kFamilies, detail);
    if (!library) {
        return std::unexpected(library.error());
    }
    StructureBuilder builder;

    // The ruined portals' setups: which placement, how mossy, how likely an
    // air pocket. Data, so read rather than spelled.
    const auto      structure_dir = data_root / "worldgen" / "structure";
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(structure_dir, error)) {
        const std::string stem = entry.path().stem().string();
        if (!stem.starts_with("ruined_portal") || entry.path().extension() != ".json") {
            continue;
        }
        simdjson::dom::parser  parser;
        simdjson::dom::element document;
        if (parser.load(entry.path().string()).get(document) != simdjson::SUCCESS) {
            if (detail != nullptr) {
                *detail = entry.path().string() + " is not JSON";
            }
            return std::unexpected(TemplateError::Malformed);
        }
        simdjson::dom::array setups;
        if (document["setups"].get(setups) != simdjson::SUCCESS) {
            continue;
        }
        std::vector<PortalSetup> parsed;
        for (const simdjson::dom::element node : setups) {
            PortalSetup      setup;
            std::string_view placement;
            double           number = 0.0;
            bool             flag   = false;
            if (node["placement"].get(placement) == simdjson::SUCCESS) {
                setup.placement = std::string{placement};
            }
            if (node["air_pocket_probability"].get(number) == simdjson::SUCCESS) {
                setup.air_pocket_probability = static_cast<f32>(number);
            }
            if (node["mossiness"].get(number) == simdjson::SUCCESS) {
                setup.mossiness = static_cast<f32>(number);
            }
            if (node["weight"].get(number) == simdjson::SUCCESS) {
                setup.weight = static_cast<f32>(number);
            }
            if (node["overgrown"].get(flag) == simdjson::SUCCESS) {
                setup.overgrown = flag;
            }
            if (node["vines"].get(flag) == simdjson::SUCCESS) {
                setup.vines = flag;
            }
            if (node["can_be_cold"].get(flag) == simdjson::SUCCESS) {
                setup.can_be_cold = flag;
            }
            if (node["replace_with_blackstone"].get(flag) == simdjson::SUCCESS) {
                setup.replace_with_blackstone = flag;
            }
            parsed.push_back(std::move(setup));
        }
        builder.impl_->portal_setups.emplace("minecraft:" + stem, std::move(parsed));
    }

    // ── treasure ── the support blocks, resolved once
    for (const std::string_view name : kTreasureSupport) {
        if (const auto block = blocks.find_block(name)) {
            builder.impl_->treasure_support.push_back(blocks.default_state(*block));
        }
    }

    // ── nether-2 ── the fossil's biome tag, read like every other tag
    if (auto biome_tags = BiomeTags::load(data_root)) {
        for (const std::string_view biome : biome_tags->members("minecraft:has_structure/nether_fossil")) {
            builder.impl_->fossil_biomes.emplace_back(biome);
        }
    }
    builder.impl_->blocks  = &blocks;
    builder.impl_->tags    = &tags;
    builder.impl_->library = std::move(*library);
    // ── jigsaw ──
    auto pools = JigsawLibrary::load(data_root, *builder.impl_->library, blocks, tags, detail);
    if (!pools) {
        return std::unexpected(pools.error());
    }
    builder.impl_->jigsaw.emplace(std::move(*pools));
    builder.impl_->structure_and_air =
        make_block_ignore(blocks, builder.impl_->ignore_structure_and_air);
    std::string ignored;
    auto        jigsaw = ProcessorList::parse_json(
        R"({"processors":[{"processor_type":"minecraft:jigsaw_replacement"}]})", blocks, &tags,
        &ignored);
    if (jigsaw) {
        builder.impl_->jigsaw_replacement = jigsaw->processors.front();
    }
    if (const auto chest = blocks.find_block("minecraft:chest")) {
        builder.impl_->chest_default = blocks.default_state(*chest);
    }
    if (const auto water = blocks.find_block("minecraft:water")) {
        builder.impl_->water_default = blocks.default_state(*water);
    }
    return builder;
}

const TemplateLibrary& StructureBuilder::templates() const noexcept {
    return *impl_->library;
}

// ── jigsaw ──
const JigsawLibrary* StructureBuilder::jigsaw() const noexcept {
    return impl_->jigsaw ? &*impl_->jigsaw : nullptr;
}

// ── Generation ──────────────────────────────────────────────────────────────

std::expected<StructureStart, std::string> StructureBuilder::generate(
    const StructureDefinition& definition, i64 level_seed, i32 chunk_x, i32 chunk_z,
    const StructureWorldSampler* sampler) const {  // ── nether-2 ── the fossil reads it
    StructureStart start;
    start.structure = definition.name;
    start.chunk_x   = chunk_x;
    start.chunk_z   = chunk_z;

    // `WorldgenRandom.setLargeFeatureSeed` over a legacy core. Every draw
    // below is in the order the game's stored pieces confirmed.
    math::LegacyRandomSource random{large_feature_seed(level_seed, chunk_x, chunk_z)};
    const BlockPos           corner{chunk_x * 16, kPlaceholderY, chunk_z * 16};

    const auto add = [&](StructurePiece piece) -> std::expected<void, std::string> {
        if (!piece.template_name.empty()) {
            const StructureTemplate* tpl = impl_->find(piece.template_name);
            if (tpl == nullptr) {
                return std::unexpected("template " + piece.template_name + " is not in the jar");
            }
            piece.box = tpl->bounding_box(piece.origin, piece.mirror, piece.rotation, piece.pivot);
        }
        piece.generated_origin = piece.origin;  // ── structures ──
        start.pieces.push_back(std::move(piece));
        return {};
    };

    switch (definition.kind) {
        case StructureKind::Igloo: {
            const Rotation rotation = random_rotation(random);
            // Half the igloos have a basement, reached down a shaft whose
            // length is one less than the draw.
            if (random.next_double() < 0.5) {
                const i32      depth = random.next_int(8) + 4;
                StructurePiece bottom;
                bottom.kind          = PieceKind::Igloo;
                bottom.template_name = "minecraft:igloo/bottom";
                bottom.rotation      = rotation;
                bottom.pivot         = kIglooBottomPivot;
                bottom.origin =
                    corner.offset(kIglooTopPivot.x - kIglooBottomPivot.x, -3 * depth - 3,
                                  kIglooTopPivot.z - kIglooBottomPivot.z);
                if (auto added = add(std::move(bottom)); !added) {
                    return std::unexpected(added.error());
                }
                for (i32 index = 0; index < depth - 1; ++index) {
                    StructurePiece middle;
                    middle.kind          = PieceKind::Igloo;
                    middle.template_name = "minecraft:igloo/middle";
                    middle.rotation      = rotation;
                    middle.pivot         = kIglooMiddlePivot;
                    middle.origin =
                        corner.offset(kIglooTopPivot.x - kIglooMiddlePivot.x, -3 - 3 * index,
                                      kIglooTopPivot.z - kIglooMiddlePivot.z);
                    if (auto added = add(std::move(middle)); !added) {
                        return std::unexpected(added.error());
                    }
                }
            }
            StructurePiece top;
            top.kind          = PieceKind::Igloo;
            top.template_name = "minecraft:igloo/top";
            top.rotation      = rotation;
            top.pivot         = kIglooTopPivot;
            top.origin        = corner;
            if (auto added = add(std::move(top)); !added) {
                return std::unexpected(added.error());
            }
            break;
        }
        case StructureKind::Shipwreck: {
            const bool             beached  = definition.name.ends_with("_beached");
            const Rotation         rotation = random_rotation(random);
            const std::string_view name =
                beached ? kBeachedShipwrecks[static_cast<usize>(
                              random.next_int(static_cast<i32>(kBeachedShipwrecks.size())))]
                        : kShipwrecks[static_cast<usize>(
                              random.next_int(static_cast<i32>(kShipwrecks.size())))];
            StructurePiece piece;
            piece.kind          = PieceKind::Shipwreck;
            piece.template_name = "minecraft:shipwreck/" + std::string{name};
            piece.rotation      = rotation;
            piece.pivot         = kShipwreckPivot;
            piece.origin        = corner;
            piece.beached       = beached;
            if (auto added = add(std::move(piece)); !added) {
                return std::unexpected(added.error());
            }
            break;
        }
        case StructureKind::OceanRuin: {
            const bool     warm     = definition.name.ends_with("_warm");
            const Rotation rotation = random_rotation(random);
            // `large_probability` and `cluster_probability` are 0.3 and 0.9
            // for both ocean ruins in 1.20.1; the draw is `<=`, not `<`.
            const bool large = random.next_float() <= 0.3F;
            const i32  index = random.next_int(large ? 4 : 8);
            const i32  number =
                large ? (warm ? kBigWarmRuins : kBigColdRuins)[static_cast<usize>(index)]
                      : index + 1;
            const auto make = [&](std::string_view family, f32 integrity) {
                StructurePiece piece;
                piece.kind          = PieceKind::OceanRuin;
                piece.template_name = "minecraft:underwater_ruin/" +
                                      std::string{large ? "big_" : ""} + std::string{family} + "_" +
                                      std::to_string(number);
                piece.rotation      = rotation;
                piece.origin        = corner;
                piece.integrity     = integrity;
                piece.large         = large;
                piece.warm          = warm;
                return piece;
            };
            if (warm) {
                if (auto added = add(make("warm", large ? 0.9F : 0.8F)); !added) {
                    return std::unexpected(added.error());
                }
            } else {
                // Three layers on one origin: whatever the mossy layer keeps,
                // the cracked and brick layers keep too, because the three
                // draws at a position are the same draw.
                for (const auto& [family, integrity] :
                     {std::pair<std::string_view, f32>{"brick", large ? 0.9F : 0.8F},
                      std::pair<std::string_view, f32>{"cracked", 0.7F},
                      std::pair<std::string_view, f32>{"mossy", 0.5F}}) {
                    if (auto added = add(make(family, integrity)); !added) {
                        return std::unexpected(added.error());
                    }
                }
            }
            if (large && random.next_float() <= 0.9F) {
                start.incomplete =
                    "ocean ruin cluster: the small ruins around the large one are not generated";
            }
            break;
        }
        case StructureKind::BuriedTreasure: {
            StructurePiece piece;
            piece.kind   = PieceKind::BuriedTreasure;
            piece.origin = corner.offset(9, 0, 9);
            piece.box    = {piece.origin.x, piece.origin.y, piece.origin.z,
                            piece.origin.x, piece.origin.y, piece.origin.z};
            piece.generated_origin = piece.origin;  // ── structures ──
            start.pieces.push_back(std::move(piece));
            break;
        }
        case StructureKind::RuinedPortal: {
            const auto found = impl_->portal_setups.find(definition.name);
            if (found == impl_->portal_setups.end() || found->second.empty()) {
                return std::unexpected(definition.name + ": no setups were read");
            }
            const auto& setups = found->second;
            // A weighted choice, drawn only when there is a choice to make.
            const PortalSetup* setup = &setups.front();
            if (setups.size() > 1) {
                f32 total = 0.0F;
                for (const PortalSetup& candidate : setups) {
                    total += candidate.weight;
                }
                f32 roll = random.next_float() * total;
                setup    = &setups.back();
                for (const PortalSetup& candidate : setups) {
                    roll -= candidate.weight;
                    if (roll < 0.0F) {
                        setup = &candidate;
                        break;
                    }
                }
            }
            // A probability of exactly 0 or 1 does not draw.
            const f32  pocket = setup->air_pocket_probability;
            const bool air_pocket =
                pocket >= 1.0F ? true : (pocket <= 0.0F ? false : random.next_float() < pocket);
            const bool     giant    = random.next_float() < 0.05F;
            const i32      index    = random.next_int(giant ? 3 : 10);
            const Rotation rotation = random_rotation(random);
            const Mirror   mirror   = random.next_float() < 0.5F ? Mirror::None : Mirror::FrontBack;

            StructurePiece piece;
            piece.kind              = PieceKind::RuinedPortal;
            piece.template_name     = std::string{giant ? "minecraft:ruined_portal/giant_portal_"
                                                        : "minecraft:ruined_portal/portal_"} +
                                      std::to_string(index + 1);
            piece.rotation          = rotation;
            piece.mirror            = mirror;
            piece.origin            = {chunk_x * 16, 0, chunk_z * 16};
            piece.portal.placement  = setup->placement;
            piece.portal.air_pocket = air_pocket;
            piece.portal.mossiness  = setup->mossiness;
            piece.portal.overgrown  = setup->overgrown;
            piece.portal.vines      = setup->vines;
            piece.portal.replace_with_blackstone = setup->replace_with_blackstone;
            const StructureTemplate* tpl         = impl_->find(piece.template_name);
            if (tpl == nullptr) {
                return std::unexpected("template " + piece.template_name + " is not in the jar");
            }
            piece.pivot = {tpl->size.x / 2, 0, tpl->size.z / 2};
            if (auto added = add(std::move(piece)); !added) {
                return std::unexpected(added.error());
            }
            // ── portals ── The height and the cold test, from the noise, here:
            // the game's stored starts carry both before any block exists.
            StructurePiece& placed = start.pieces.back();
            if (sampler == nullptr) {
                start.incomplete = "ruined portal: no sampler, the height is not searched";
                break;
            }
            // The dimension's bottom: a Nether portal is the only one there.
            const i32  bottom = placed.portal.placement == "in_nether" ? 0 : -64;
            const auto y      = ruined_portal_height(*sampler, placed.portal.placement,
                                                     placed.portal.air_pocket, placed.box, bottom,
                                                     random);
            if (!y) {
                return std::unexpected(y.error());
            }
            placed.origin.y           = *y;
            placed.generated_origin.y = *y;
            placed.box.move(0, *y - placed.box.min_y, 0);
            placed.height_settled = true;
            if (setup->can_be_cold) {
                const auto cold = ruined_portal_cold(*sampler, placed.origin);
                if (!cold) {
                    return std::unexpected(cold.error());
                }
                placed.portal.cold = *cold;
            }
            start.incomplete = "ruined portal: the netherrack spread is not implemented";
            break;
        }
        case StructureKind::NetherFossil: {  // ── nether-2 ──
            // A column of the chunk, a height between 32 and the generator's
            // top less two, then down to the first air over something solid in
            // the base column — the noise alone (minecraft.wiki "Nether
            // Fossil"; the order of the draws is the one the game's 185 stored
            // starts confirm, nether-2.md § 2.1).
            if (sampler == nullptr) {
                return std::unexpected(std::string{"nether fossil: no sampler for the base column"});
            }
            const i32 x = chunk_x * 16 + random.next_int(16);
            const i32 z = chunk_z * 16 + random.next_int(16);
            i32       y = kNetherFossilLowest +
                  random.next_int(kNetherFossilTop - kNetherFossilLowest + 1);
            const auto solid = [&](i32 at) -> std::optional<bool> {
                return sampler->base_solid(x, at, z);
            };
            bool found = false;
            while (y > kNetherSeaLevel) {
                const auto here = solid(y);
                --y;
                const auto below = solid(y);
                if (!here || !below) {
                    return std::unexpected(
                        std::string{"nether fossil: the sampler has no base column"});
                }
                // Air is the noise's empty above the lava sea; the floor is
                // anything solid (netherrack, whose top face is sturdy).
                if (!*here && y + 1 > kNetherSeaLevel - 1 && *below) {
                    found = true;
                    break;
                }
            }
            if (!found || y <= kNetherSeaLevel) {
                return std::unexpected(std::string{"nether fossil: no floor in the column"});
            }
            const std::string_view biome = sampler->biome_at(x, y, z);
            if (std::ranges::find(impl_->fossil_biomes, biome) == impl_->fossil_biomes.end()) {
                return std::unexpected(std::string{"nether fossil: not a fossil biome there"});
            }
            const Rotation rotation = random_rotation(random);
            const i32      index    = random.next_int(kNetherFossils);
            StructurePiece piece;
            piece.kind           = PieceKind::NetherFossil;
            piece.template_name  = "minecraft:nether_fossils/fossil_" + std::to_string(index + 1);
            piece.rotation       = rotation;
            piece.origin         = {x, y, z};
            piece.height_settled = true;
            if (auto added = add(std::move(piece)); !added) {
                return std::unexpected(added.error());
            }
            break;
        }
        case StructureKind::DesertPyramid:
        case StructureKind::JungleTemple:
        case StructureKind::SwampHut: {  // ── temples ── scattered.cpp
            start.pieces.push_back(make_scattered_piece(*scattered_kind_for(definition.kind),
                                                        chunk_x, chunk_z, random));
            start.incomplete = std::string{to_string(definition.kind)} + ": the layout is not built";
            break;
        }
        case StructureKind::Jigsaw: {  // ── jigsaw ── grown from its pools
            const JigsawConfig* config =
                impl_->jigsaw ? impl_->jigsaw->config(definition.name) : nullptr;
            if (config == nullptr) {
                return std::unexpected(definition.name + ": no jigsaw settings were read");
            }
            return impl_->jigsaw->assemble(*config, level_seed, chunk_x, chunk_z, sampler);
        }
        default:
            return std::unexpected(std::string{to_string(definition.kind)} +
                                   ": not a structure this builder makes");
    }

    start.box = start.pieces.front().box;
    for (const StructurePiece& piece : start.pieces) {
        start.box.encapsulate(piece.box);
    }
    return start;
}

// ── From the game's NBT ─────────────────────────────────────────────────────

std::expected<StructurePiece, std::string> StructureBuilder::piece_from_nbt(
    const nbt::Tag& child) const {
    const nbt::Tag* id = child.find("id");
    if (id == nullptr) {
        return std::unexpected("piece without id");
    }
    const std::string_view kind = id->as_string();
    StructurePiece         piece;
    if (kind == "minecraft:iglu") {
        piece.kind = PieceKind::Igloo;
    } else if (kind == "minecraft:shipwreck") {
        piece.kind = PieceKind::Shipwreck;
    } else if (kind == "minecraft:orp") {
        piece.kind = PieceKind::OceanRuin;
    } else if (kind == "minecraft:rupo") {
        piece.kind = PieceKind::RuinedPortal;
    } else if (kind == "minecraft:btp") {
        piece.kind = PieceKind::BuriedTreasure;
    } else if (kind == "minecraft:nefos") {  // ── nether-2 ──
        piece.kind = PieceKind::NetherFossil;
    } else if (const auto scattered = scattered_kind_of(kind)) {  // ── temples ──
        piece.kind           = PieceKind::Scattered;
        piece.scattered.kind = *scattered;
    } else if (kind == "minecraft:jigsaw") {  // ── jigsaw ──
        if (!impl_->jigsaw) {
            return std::unexpected(std::string{"jigsaw piece and no pools"});
        }
        return jigsaw_piece_from_nbt(*impl_->jigsaw, child);
    } else {
        return std::unexpected("piece type " + std::string{kind} +
                               " is not one this builder makes");
    }

    const nbt::Tag* bb  = child.find("BB");
    const auto*     box = bb != nullptr ? bb->get_if<nbt::Tag::IntArray>() : nullptr;
    if (box == nullptr || box->size() != 6) {
        return std::unexpected("piece without BB");
    }
    piece.box            = {(*box)[0], (*box)[1], (*box)[2], (*box)[3], (*box)[4], (*box)[5]};
    piece.height_settled = true;

    if (piece.kind == PieceKind::BuriedTreasure) {
        piece.origin = {piece.box.min_x, piece.box.min_y, piece.box.min_z};
        return piece;
    }
    if (piece.kind == PieceKind::Scattered) {  // ── temples ──
        if (auto read = scattered_from_nbt(child, piece); !read) {
            return std::unexpected(read.error());
        }
        return piece;
    }

    const nbt::Tag* tpl_name = child.find("Template");
    if (tpl_name == nullptr) {
        return std::unexpected("template piece without Template");
    }
    piece.template_name          = std::string{tpl_name->as_string()};
    const StructureTemplate* tpl = impl_->find(piece.template_name);
    if (tpl == nullptr) {
        return std::unexpected("template " + piece.template_name + " is not in the jar");
    }
    const auto int_of = [&](std::string_view key) {
        const nbt::Tag* tag = child.find(key);
        return tag != nullptr ? static_cast<i32>(tag->as_i64()) : 0;
    };
    piece.origin           = {int_of("TPX"), int_of("TPY"), int_of("TPZ")};
    piece.generated_origin = piece.origin;  // ── structures ── as stored
    const nbt::Tag* rotation =
        child.find(piece.kind == PieceKind::RuinedPortal ? "Rotation" : "Rot");
    if (rotation != nullptr) {
        piece.rotation = parse_rotation(rotation->as_string()).value_or(Rotation::None);
    }
    if (const nbt::Tag* mirror = child.find("Mirror")) {
        piece.mirror = parse_mirror(mirror->as_string()).value_or(Mirror::None);
    }

    switch (piece.kind) {
        case PieceKind::Igloo: {
            const std::string leaf = strip_namespace(piece.template_name);
            piece.pivot            = leaf.ends_with("top")      ? kIglooTopPivot
                                     : leaf.ends_with("middle") ? kIglooMiddlePivot
                                                                : kIglooBottomPivot;
            break;
        }
        case PieceKind::Shipwreck:
            piece.pivot = kShipwreckPivot;
            if (const nbt::Tag* beached = child.find("isBeached")) {
                piece.beached = beached->as_bool();
            }
            break;
        case PieceKind::OceanRuin:
            if (const nbt::Tag* integrity = child.find("Integrity")) {
                piece.integrity = static_cast<f32>(integrity->as_f64());
            }
            if (const nbt::Tag* large = child.find("IsLarge")) {
                piece.large = large->as_bool();
            }
            if (const nbt::Tag* biome = child.find("BiomeType")) {
                piece.warm = biome->as_string() == "WARM";
            }
            break;
        case PieceKind::RuinedPortal: {
            piece.pivot = {tpl->size.x / 2, 0, tpl->size.z / 2};
            if (const nbt::Tag* placement = child.find("VerticalPlacement")) {
                piece.portal.placement = std::string{placement->as_string()};
            }
            if (const nbt::Tag* properties = child.find("Properties")) {
                const auto flag = [&](std::string_view key) {
                    const nbt::Tag* tag = properties->find(key);
                    return tag != nullptr && tag->as_bool();
                };
                piece.portal.cold                    = flag("cold");
                piece.portal.air_pocket              = flag("air_pocket");
                piece.portal.overgrown               = flag("overgrown");
                piece.portal.vines                   = flag("vines");
                piece.portal.replace_with_blackstone = flag("replace_with_blackstone");
                if (const nbt::Tag* mossiness = properties->find("mossiness")) {
                    piece.portal.mossiness = static_cast<f32>(mossiness->as_f64());
                }
            }
            break;
        }
        case PieceKind::BuriedTreasure:
        case PieceKind::NetherFossil:  // ── nether-2 ──
        case PieceKind::Scattered: break;  // ── temples ── read above
        case PieceKind::Jigsaw: break;     // ── jigsaw ── returned above
    }

    // The stored origin keeps the placeholder height for the igloo while the
    // box has moved to the terrain; the box is the truth for where the blocks
    // went, so the origin follows it.
    const BoundingBox natural =
        tpl->bounding_box(piece.origin, piece.mirror, piece.rotation, piece.pivot);
    piece.origin.y += piece.box.min_y - natural.min_y;
    return piece;
}

// ── Height ──────────────────────────────────────────────────────────────────

void StructureBuilder::settle_height(const StructureLevel& level, StructurePiece& piece,
                                     FeatureRandom* random) const {
    if (piece.height_settled) {
        return;
    }
    i32 dy = 0;
    switch (piece.kind) {
        case PieceKind::Igloo: {
            // The three pivots share one column; the igloo's floor sinks one
            // block below the surface there.
            const i32 x = piece.origin.x + piece.pivot.x;
            const i32 z = piece.origin.z + piece.pivot.z;
            dy = level.height(world::HeightmapType::WorldSurfaceWG, x, z) - kPlaceholderY - 1;
            break;
        }
        case PieceKind::Shipwreck: {
            const i32 sink = piece.beached && random != nullptr ? random->next_int(3) : 0;
            // The footprint the game averages over is the *unrotated* one,
            // measured from the origin. A beached ship takes the lowest column
            // instead and sinks by half its height.
            const StructureTemplate* tpl = impl_->find(piece.template_name);
            if (tpl == nullptr) {
                break;
            }
            const auto type   = piece.beached ? world::HeightmapType::WorldSurfaceWG
                                              : world::HeightmapType::OceanFloorWG;
            i64        total  = 0;
            i32        lowest = level.max_y() + 1;
            for (i32 dx = 0; dx < tpl->size.x; ++dx) {
                for (i32 dz = 0; dz < tpl->size.z; ++dz) {
                    const i32 h = level.height(type, piece.origin.x + dx, piece.origin.z + dz);
                    total += h;
                    lowest = std::min(lowest, h);
                }
            }
            const i32 area   = tpl->size.x * tpl->size.z;
            const i32 target = piece.beached ? lowest - tpl->size.y / 2 - sink
                                             : static_cast<i32>(total / std::max(area, 1));
            dy               = target - piece.origin.y;
            break;
        }
        case PieceKind::OceanRuin: {
            dy = level.height(world::HeightmapType::OceanFloorWG, piece.origin.x, piece.origin.z) -
                 piece.origin.y;
            break;
        }
        case PieceKind::BuriedTreasure:  // ── treasure ── down to its support
            dy = buried_treasure_height(level, impl_->treasure_support, piece.origin.x,
                                        piece.origin.z) -
                 piece.origin.y;
            break;
        case PieceKind::RuinedPortal:  // ── portals ── settled at generation
        case PieceKind::NetherFossil:  // ── nether-2 ── settled by its search
        case PieceKind::Jigsaw: break;  // ── jigsaw ── settled by the assembler
        case PieceKind::Scattered: break;  // ── temples ── not settled yet: refused
    }
    piece.origin.y += dy;
    piece.box.move(0, dy, 0);
    piece.height_settled = true;
}

// ── Placement ───────────────────────────────────────────────────────────────

PiecePlaceResult StructureBuilder::place(StructureLevel& level, const StructurePiece& piece,
                                         const BoundingBox& clip, FeatureRandom& random,
                                         bool height_drawn) const {
    PiecePlaceResult               result;
    const registry::BlockRegistry& blocks = *impl_->blocks;

    if (piece.kind == PieceKind::Jigsaw) {  // ── jigsaw ──
        if (impl_->jigsaw) {
            return impl_->jigsaw->place(level, piece, clip, random, blocks);
        }
        result.unknown_markers.push_back("jigsaw piece and no pools");
        return result;
    }

    if (piece.kind == PieceKind::BuriedTreasure) {
        const BlockPos at = piece.origin;
        if (!clip.contains(at.x, at.y, at.z)) {
            return result;
        }
        // Always facing east, and waterlogged when it lands in water.
        auto chest = with_value(blocks, impl_->chest_default, "facing", "east");
        chest      = with_waterlogged(blocks, chest,
                                      holds_water_source(blocks, level.block_at(at.x, at.y, at.z)));
        level.set_block(at.x, at.y, at.z, chest);
        impl_->set_loot(level, at, "minecraft:chests/buried_treasure", random);
        result.written = 1;
        result.chests  = 1;
        return result;
    }

    if (piece.kind == PieceKind::Scattered) {  // ── temples ── nothing to place yet
        result.unknown_markers.push_back("scattered piece: the layout is not built");
        return result;
    }

    const StructureTemplate* tpl = impl_->find(piece.template_name);
    if (tpl == nullptr) {
        result.unknown_markers.push_back("template " + piece.template_name + " missing");
        return result;
    }

    PlaceSettings settings;
    settings.rotation = piece.rotation;
    settings.mirror   = piece.mirror;
    settings.pivot    = piece.pivot;
    settings.clip     = clip;
    settings.random   = &random;

    switch (piece.kind) {
        case PieceKind::Igloo: break;
        case PieceKind::Shipwreck: settings.processors.push_back(impl_->structure_and_air); break;
        case PieceKind::OceanRuin:
            settings.processors.push_back(make_block_rot(piece.integrity));
            settings.processors.push_back(impl_->structure_and_air);
            break;
        case PieceKind::RuinedPortal: {
            if (impl_->jigsaw_replacement) {
                settings.processors.push_back(impl_->jigsaw_replacement);
            }
            // Without an air pocket the template's air is not written: the
            // sea stays in an ocean portal, the leaves in a forest one.
            if (!piece.portal.air_pocket) {
                const std::array<std::string_view, 1> air{"minecraft:air"};
                settings.processors.push_back(make_block_ignore(blocks, air));
            }
            // The replacement rules, as their JSON twin spells them.
            std::string rules =
                R"({"input_predicate":{"predicate_type":"minecraft:random_block_match","block":"minecraft:gold_block","probability":0.3},)"
                R"("location_predicate":{"predicate_type":"minecraft:always_true"},"output_state":{"Name":"minecraft:air"}})";
            if (piece.portal.placement == "on_ocean_floor") {
                rules +=
                    R"(,{"input_predicate":{"predicate_type":"minecraft:block_match","block":"minecraft:lava"},)"
                    R"("location_predicate":{"predicate_type":"minecraft:always_true"},"output_state":{"Name":"minecraft:magma_block"}})";
            } else if (!piece.portal.cold) {
                rules +=
                    R"(,{"input_predicate":{"predicate_type":"minecraft:random_block_match","block":"minecraft:lava","probability":0.2},)"
                    R"("location_predicate":{"predicate_type":"minecraft:always_true"},"output_state":{"Name":"minecraft:magma_block"}})";
            }
            if (!piece.portal.cold) {
                rules +=
                    R"(,{"input_predicate":{"predicate_type":"minecraft:random_block_match","block":"minecraft:netherrack","probability":0.07},)"
                    R"("location_predicate":{"predicate_type":"minecraft:always_true"},"output_state":{"Name":"minecraft:magma_block"}})";
            }
            std::string why;
            auto        list = ProcessorList::parse_json(
                R"({"processors":[{"processor_type":"minecraft:rule","rules":[)" + rules + "]}]}",
                blocks, impl_->tags, &why);
            if (list) {
                settings.processors.insert(settings.processors.end(), list->processors.begin(),
                                           list->processors.end());
            } else {
                result.unknown_markers.push_back("portal rules: " + why);
            }
            settings.processors.push_back(
                make_block_age(blocks, *impl_->tags, piece.portal.mossiness));
            settings.processors.push_back(
                make_protected_blocks(*impl_->tags, "minecraft:features_cannot_replace"));
            break;
        }
        case PieceKind::BuriedTreasure: break;
        // ── nether-2 ── the fossil's air and structure blocks are not written
        case PieceKind::NetherFossil: settings.processors.push_back(impl_->structure_and_air); break;
        case PieceKind::Scattered: break;  // ── temples ── returned above
        case PieceKind::Jigsaw: break;     // ── jigsaw ── placed above
    }

    // A beached ship settles on its lowest column minus a random 0 to 2, and
    // the game draws that in every chunk the ship crosses, before anything is
    // placed — which is where the draw sits in the loot seeds that follow.
    if (piece.kind == PieceKind::Shipwreck && piece.beached && !height_drawn) {
        (void)random.next_int(3);
    }

    const PlaceResult placed = place_template(level, *tpl, piece.origin, settings, blocks);
    result.written           = placed.written;
    result.dropped           = placed.dropped_by_processors;
    result.shaped            = placed.shaped;

    for (const DataMarker& marker : placed.markers) {
        switch (piece.kind) {
            case PieceKind::Igloo:
                if (marker.metadata == "chest") {
                    level.set_block(marker.pos.x, marker.pos.y, marker.pos.z, registry::kAirState);
                    const BlockPos below = marker.pos.below();
                    if (clip.contains(below.x, below.y, below.z)) {
                        impl_->set_loot(level, below, "minecraft:chests/igloo_chest", random);
                        ++result.chests;
                    }
                    continue;
                }
                break;
            case PieceKind::Shipwreck:
                if (const std::string_view table = shipwreck_loot(marker.metadata);
                    !table.empty()) {
                    const BlockPos below = marker.pos.below();
                    if (clip.contains(below.x, below.y, below.z)) {
                        impl_->set_loot(level, below, table, random);
                        ++result.chests;
                    }
                    continue;
                }
                break;
            case PieceKind::OceanRuin:
                if (marker.metadata == "chest") {
                    const auto existing = level.block_at(marker.pos.x, marker.pos.y, marker.pos.z);
                    const auto chest    = with_waterlogged(blocks, impl_->chest_default,
                                                           holds_water_source(blocks, existing));
                    level.set_block(marker.pos.x, marker.pos.y, marker.pos.z, chest);
                    impl_->set_loot(level, marker.pos,
                                    piece.large ? "minecraft:chests/underwater_ruin_big"
                                                : "minecraft:chests/underwater_ruin_small",
                                    random);
                    ++result.chests;
                    continue;
                }
                if (marker.metadata == "drowned") {
                    // The drowned itself is an entity and is not spawned here.
                    level.set_block(marker.pos.x, marker.pos.y, marker.pos.z,
                                    marker.pos.y > level.sea_level() ? registry::kAirState
                                                                     : impl_->water_default);
                    continue;
                }
                break;
            case PieceKind::RuinedPortal:
            case PieceKind::BuriedTreasure:
            case PieceKind::NetherFossil:  // ── nether-2 ──
            case PieceKind::Scattered: break;  // ── temples ──
            case PieceKind::Jigsaw: break;     // ── jigsaw ──
        }
        result.unknown_markers.push_back(marker.metadata);
    }
    return result;
}

}  // namespace ov::worldgen
