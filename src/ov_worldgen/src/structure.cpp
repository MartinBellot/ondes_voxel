#include "ov/worldgen/structure.hpp"

#include "ov/base/log.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/math/random.hpp"

#include <simdjson.h>

#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>
#include <string>

namespace ov::worldgen {

StructureWorldSampler::~StructureWorldSampler() = default;

std::string_view to_string(StructureKind kind) noexcept {
    switch (kind) {
        case StructureKind::DesertPyramid:
            return "desert_pyramid";
        case StructureKind::JungleTemple:
            return "jungle_temple";
        case StructureKind::SwampHut:
            return "swamp_hut";
        case StructureKind::Igloo:
            return "igloo";
        case StructureKind::Mineshaft:
            return "mineshaft";
        case StructureKind::OceanMonument:
            return "ocean_monument";
        case StructureKind::OceanRuin:
            return "ocean_ruin";
        case StructureKind::Shipwreck:
            return "shipwreck";
        case StructureKind::BuriedTreasure:
            return "buried_treasure";
        case StructureKind::RuinedPortal:
            return "ruined_portal";
        case StructureKind::WoodlandMansion:
            return "woodland_mansion";
        case StructureKind::Stronghold:
            return "stronghold";
        case StructureKind::Fortress:
            return "fortress";
        case StructureKind::NetherFossil:
            return "nether_fossil";
        case StructureKind::EndCity:
            return "end_city";
        case StructureKind::Jigsaw:
            return "jigsaw";
    }
    return "?";
}

std::string_view to_string(PlacementDecision decision) noexcept {
    switch (decision) {
        case PlacementDecision::NotCandidate:
            return "not-candidate";
        case PlacementDecision::FrequencyRejected:
            return "frequency";
        case PlacementDecision::ExcludedByZone:
            return "exclusion-zone";
        case PlacementDecision::BiomeRejected:
            return "biome";
        case PlacementDecision::BiomeUnknown:
            return "biome-unknown";
        case PlacementDecision::PlacedByPlacement:
            return "placed";
        case PlacementDecision::Unsupported:
            return "unsupported";
        case PlacementDecision::OtherDimension:
            return "other-dimension";
    }
    return "?";
}

// ── BiomeTags ───────────────────────────────────────────────────────────────

struct BiomeTags::Impl {
    /// Tag name without `#`, `minecraft:` included, to its resolved members.
    std::map<std::string, std::set<std::string>, std::less<>> resolved;
};

namespace {

[[nodiscard]] std::string qualify(std::string_view name) {
    if (name.starts_with('#')) {
        name.remove_prefix(1);
    }
    return name.find(':') == std::string_view::npos ? "minecraft:" + std::string{name}
                                                    : std::string{name};
}

/// Resolve one tag's `#other` references, depth-first, refusing cycles.
///
/// The tags reference each other four deep — `has_structure/mineshaft` names
/// `is_forest`, which names nothing, but `is_overworld` chains further — and a
/// flat pass would leave the references unresolved and silently empty. A cycle
/// is refused rather than truncated: an infinite tag is a malformed pack, and
/// answering "no" for it would be a lie the world would carry for ever.
bool resolve(const std::map<std::string, std::vector<std::string>, std::less<>>& raw,
             std::string_view tag, std::map<std::string, std::set<std::string>, std::less<>>& out,
             std::set<std::string>& in_progress) {
    const std::string key{tag};
    if (out.contains(key)) {
        return true;
    }
    if (!in_progress.insert(key).second) {
        OV_LOG_ERROR("worldgen: biome tag {} references itself", key);
        return false;
    }
    std::set<std::string> members;
    const auto            entry = raw.find(key);
    if (entry == raw.end()) {
        in_progress.erase(key);
        return false;
    }
    for (const std::string& value : entry->second) {
        if (value.starts_with('#')) {
            const std::string other = qualify(value);
            if (!resolve(raw, other, out, in_progress)) {
                OV_LOG_ERROR("worldgen: biome tag {} names {}, which does not resolve", key, other);
                in_progress.erase(key);
                return false;
            }
            const auto& those = out.at(other);
            members.insert(those.begin(), those.end());
        } else {
            members.insert(qualify(value));
        }
    }
    in_progress.erase(key);
    out.emplace(key, std::move(members));
    return true;
}

}  // namespace

std::expected<BiomeTags, StructureSetError> BiomeTags::load(
    const std::filesystem::path& data_root) {
    const auto directory = data_root / "tags" / "worldgen" / "biome";
    if (!std::filesystem::is_directory(directory)) {
        OV_LOG_ERROR("worldgen: {} is missing", directory.string());
        return std::unexpected(StructureSetError::Missing);
    }

    std::map<std::string, std::vector<std::string>, std::less<>> raw;
    simdjson::dom::parser                                        parser;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") {
            continue;
        }
        const auto relative = std::filesystem::relative(entry.path(), directory);
        std::string name    = relative.generic_string();
        name.resize(name.size() - 5);  // ".json"
        name = "minecraft:" + name;

        auto text = simdjson::padded_string::load(entry.path().string());
        if (text.error() != simdjson::SUCCESS) {
            return std::unexpected(StructureSetError::Malformed);
        }
        auto document = parser.parse(text.value());
        if (document.error() != simdjson::SUCCESS) {
            return std::unexpected(StructureSetError::Malformed);
        }
        simdjson::dom::array values;
        if (document.at_key("values").get(values) != simdjson::SUCCESS) {
            return std::unexpected(StructureSetError::Malformed);
        }
        std::vector<std::string> members;
        for (auto value : values) {
            std::string_view text_value;
            if (value.get(text_value) == simdjson::SUCCESS) {
                members.emplace_back(text_value);
                continue;
            }
            // The optional form, `{"id": …, "required": false}`.
            std::string_view id;
            if (value["id"].get(id) == simdjson::SUCCESS) {
                members.emplace_back(id);
            }
        }
        raw.emplace(std::move(name), std::move(members));
    }

    auto impl = std::make_shared<Impl>();
    for (const auto& [name, _] : raw) {
        std::set<std::string> in_progress;
        if (!resolve(raw, name, impl->resolved, in_progress)) {
            return std::unexpected(StructureSetError::Malformed);
        }
    }

    BiomeTags tags;
    tags.impl_ = std::move(impl);
    return tags;
}

bool BiomeTags::contains(std::string_view tag, std::string_view biome) const {
    if (!impl_) {
        return false;
    }
    const auto entry = impl_->resolved.find(qualify(tag));
    if (entry == impl_->resolved.end()) {
        return false;
    }
    return entry->second.contains(qualify(biome));
}

bool BiomeTags::known(std::string_view tag) const {
    return impl_ && impl_->resolved.contains(qualify(tag));
}

usize BiomeTags::tag_count() const noexcept { return impl_ ? impl_->resolved.size() : 0; }

std::vector<std::string_view> BiomeTags::members(std::string_view tag) const {
    std::vector<std::string_view> out;
    if (!impl_) {
        return out;
    }
    const auto entry = impl_->resolved.find(qualify(tag));
    if (entry == impl_->resolved.end()) {
        return out;
    }
    out.reserve(entry->second.size());
    for (const std::string& member : entry->second) {
        out.emplace_back(member);
    }
    return out;
}

// ── StructurePlacer ─────────────────────────────────────────────────────────

struct StructurePlacer::Impl {
    const StructureSetRegistry*      sets{nullptr};
    std::vector<StructureDefinition> structures;
    BiomeTags                        tags;
    /// Set names the dimension cannot produce. Empty means no restriction was
    /// asked for, which is not the same as "every set is impossible".
    std::set<std::string> foreign;
};

namespace {

/// Read once, at load: `OV_STRUCT_JIGSAW_ANCHOR=corner` moves the jigsaw
/// structures' biome sample from the chunk's middle column to its corner.
///
/// A measuring instrument. Neither column is what the game uses — see
/// `StructureDefinition::anchor_at_corner` — and holding both in one binary is
/// the only honest way to say which of two wrong rules is less wrong on a given
/// sample, since taking the before from one build and the after from another is
/// a mistake this repository has already paid for.
[[nodiscard]] bool jigsaw_anchor_is_corner() {
    const char* choice = std::getenv("OV_STRUCT_JIGSAW_ANCHOR");
    return choice != nullptr && std::string_view{choice} == "corner";
}

struct KindRow {
    std::string_view name;
    StructureKind    kind;
    GenerationAnchor anchor;
    i32              height;
    /// Sample at the chunk corner rather than its middle.
    bool corner;
};

/// The type name to the kind, and with it the anchor the biome check uses.
///
/// The anchor is part of the type and not a separate field in the JSON, which
/// is exactly why it belongs in one table: a structure whose anchor is wrong
/// generates in a plausible biome that is not the game's, and there is nothing
/// in the data to catch it. The mineshaft's fixed y = 50 is the one value here
/// that is a bare constant, and it is the game's.
constexpr KindRow kKinds[] = {
    {"minecraft:desert_pyramid", StructureKind::DesertPyramid, GenerationAnchor::SurfaceCentre, 0,
     false},
    {"minecraft:jungle_temple", StructureKind::JungleTemple, GenerationAnchor::SurfaceCentre, 0,
     false},
    {"minecraft:swamp_hut", StructureKind::SwampHut, GenerationAnchor::SurfaceCentre, 0, false},
    {"minecraft:igloo", StructureKind::Igloo, GenerationAnchor::SurfaceCentre, 0, false},
    {"minecraft:mineshaft", StructureKind::Mineshaft, GenerationAnchor::FixedHeight, 50, false},
    {"minecraft:ocean_monument", StructureKind::OceanMonument, GenerationAnchor::OceanFloorCentre,
     0, false},
    {"minecraft:ocean_ruin", StructureKind::OceanRuin, GenerationAnchor::OceanFloorCentre, 0,
     false},
    {"minecraft:shipwreck", StructureKind::Shipwreck, GenerationAnchor::OceanFloorCentre, 0,
     false},
    {"minecraft:buried_treasure", StructureKind::BuriedTreasure,
     GenerationAnchor::OceanFloorCentre, 0, false},
    {"minecraft:ruined_portal", StructureKind::RuinedPortal, GenerationAnchor::SurfaceCentre, 0,
     false},
    {"minecraft:woodland_mansion", StructureKind::WoodlandMansion,
     GenerationAnchor::SurfaceCentre, 0, false},
    {"minecraft:stronghold", StructureKind::Stronghold, GenerationAnchor::None, 0, false},
    {"minecraft:fortress", StructureKind::Fortress, GenerationAnchor::SurfaceCentre, 0, false},
    // ── nether-2 ── No anchor: the fossil's biome is read at the point its own
    // search finds (`StructureBuilder::generate`), which the grid cannot know —
    // measured, 185 of 185 starts identical (nether-2.md § 2.1).
    {"minecraft:nether_fossil", StructureKind::NetherFossil, GenerationAnchor::None, 0, false},
    {"minecraft:end_city", StructureKind::EndCity, GenerationAnchor::SurfaceCentre, 0, false},
    // The jigsaw structures' true anchor is the start template's own bounding
    // box, which this layer cannot compute yet. `corner` is the other simple
    // approximation and is reachable through OV_STRUCT_JIGSAW_ANCHOR so the two
    // can be measured in one binary on one sample.
    {"minecraft:jigsaw", StructureKind::Jigsaw, GenerationAnchor::SurfaceCentre, 0, false},
};

}  // namespace

StructurePlacer::StructurePlacer() : impl_{std::make_unique<Impl>()} {}
StructurePlacer::StructurePlacer(StructurePlacer&&) noexcept            = default;
StructurePlacer& StructurePlacer::operator=(StructurePlacer&&) noexcept = default;
StructurePlacer::~StructurePlacer()                                     = default;

std::expected<StructurePlacer, StructureSetError> StructurePlacer::load(
    const std::filesystem::path& data_root, const StructureSetRegistry& sets) {
    const auto directory = data_root / "worldgen" / "structure";
    if (!std::filesystem::is_directory(directory)) {
        OV_LOG_ERROR("worldgen: {} is missing", directory.string());
        return std::unexpected(StructureSetError::Missing);
    }

    auto tags = BiomeTags::load(data_root);
    if (!tags) {
        return std::unexpected(tags.error());
    }

    StructurePlacer placer;
    placer.impl_->sets = &sets;
    placer.impl_->tags = std::move(tags.value());

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    simdjson::dom::parser parser;
    for (const auto& path : files) {
        auto text = simdjson::padded_string::load(path.string());
        if (text.error() != simdjson::SUCCESS) {
            return std::unexpected(StructureSetError::Malformed);
        }
        auto document = parser.parse(text.value());
        if (document.error() != simdjson::SUCCESS) {
            return std::unexpected(StructureSetError::Malformed);
        }

        StructureDefinition definition;
        definition.name = "minecraft:" + path.stem().string();

        std::string_view type;
        if (document.at_key("type").get(type) != simdjson::SUCCESS) {
            return std::unexpected(StructureSetError::Malformed);
        }
        const auto row = std::find_if(std::begin(kKinds), std::end(kKinds),
                                      [type](const KindRow& r) { return r.name == type; });
        if (row == std::end(kKinds)) {
            OV_LOG_ERROR("worldgen: {} has type {}, which we do not know", definition.name, type);
            return std::unexpected(StructureSetError::UnknownPlacement);
        }
        definition.kind             = row->kind;
        definition.anchor           = row->anchor;
        definition.anchor_height    = row->height;
        definition.anchor_at_corner = row->corner || (row->kind == StructureKind::Jigsaw &&
                                                      jigsaw_anchor_is_corner());

        std::string_view biomes;
        if (document.at_key("biomes").get(biomes) == simdjson::SUCCESS) {
            definition.biomes = std::string{biomes};
        } else {
            // The inline-list form. No 1.20.1 structure uses it, but a pack may,
            // and a silently empty biome list would stop the structure from
            // ever generating with no message anywhere.
            simdjson::dom::array list;
            if (document.at_key("biomes").get(list) != simdjson::SUCCESS) {
                return std::unexpected(StructureSetError::Malformed);
            }
            std::string joined;
            for (auto value : list) {
                std::string_view name;
                if (value.get(name) == simdjson::SUCCESS) {
                    if (!joined.empty()) {
                        joined += ',';
                    }
                    joined += name;
                }
            }
            definition.biomes = std::move(joined);
        }

        if (definition.biomes.starts_with('#') && !placer.impl_->tags.known(definition.biomes)) {
            OV_LOG_ERROR("worldgen: {} names biome tag {}, which the pack does not have",
                         definition.name, definition.biomes);
            return std::unexpected(StructureSetError::Malformed);
        }

        std::string_view step;
        if (document.at_key("step").get(step) == simdjson::SUCCESS) {
            definition.step = std::string{step};
        }

        placer.impl_->structures.push_back(std::move(definition));
    }

    return placer;
}

const StructureDefinition* StructurePlacer::find(std::string_view name) const noexcept {
    for (const StructureDefinition& definition : impl_->structures) {
        if (definition.name == name) {
            return &definition;
        }
    }
    return nullptr;
}

const std::vector<StructureDefinition>& StructurePlacer::structures() const noexcept {
    return impl_->structures;
}

const BiomeTags& StructurePlacer::biome_tags() const noexcept { return impl_->tags; }

namespace {

/// Whether a biome satisfies a structure's `biomes` field, tag or list.
[[nodiscard]] bool biome_allowed(const BiomeTags& tags, const std::string& field,
                                 std::string_view biome) {
    if (field.starts_with('#')) {
        return tags.contains(field, biome);
    }
    // A comma-joined inline list, as the loader flattened it.
    usize start = 0;
    while (start <= field.size()) {
        const usize end  = field.find(',', start);
        const auto  part = std::string_view{field}.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (part == biome) {
            return true;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return false;
}

}  // namespace

void StructurePlacer::restrict_to_biomes(const std::vector<std::string_view>& biomes) {
    impl_->foreign.clear();
    if (biomes.empty() || impl_->sets == nullptr) {
        return;
    }
    for (const StructureSet& set : impl_->sets->sets()) {
        bool reachable = false;
        for (const StructureSetEntry& entry : set.entries) {
            const StructureDefinition* definition = find(entry.structure);
            if (definition == nullptr) {
                continue;
            }
            for (std::string_view biome : biomes) {
                if (biome_allowed(impl_->tags, definition->biomes, biome)) {
                    reachable = true;
                    break;
                }
            }
            if (reachable) {
                break;
            }
        }
        // The stronghold is the exception the rule would get wrong: its biome
        // field is consulted when the rings are laid out, not when a chunk is
        // asked, so a set with no biome match is still an overworld set. It is
        // already refused as unsupported, and marking it foreign as well would
        // hide which of the two reasons applies.
        if (!reachable && !set.concentric) {
            impl_->foreign.insert(set.name);
        }
    }
}

StructurePlacementResult StructurePlacer::decide_set(const StructureSet& set, i64 level_seed,
                                                     i32 chunk_x, i32 chunk_z,
                                                     const StructureWorldSampler* sampler,
                                                     AnchorColumns* columns) const {
    AnchorColumns    own;
    AnchorColumns&   cache = columns != nullptr ? *columns : own;
    StructurePlacementResult result;
    result.set = set.name;

    if (impl_->foreign.contains(set.name)) {
        result.decision = PlacementDecision::OtherDimension;
        return result;
    }
    if (!set.spread) {
        result.decision = PlacementDecision::Unsupported;
        return result;
    }
    const RandomSpreadPlacement& spread = *set.spread;

    if (!spread.is_candidate_chunk(level_seed, chunk_x, chunk_z)) {
        result.decision = PlacementDecision::NotCandidate;
        return result;
    }
    if (!spread.passes_frequency(level_seed, chunk_x, chunk_z)) {
        result.decision = PlacementDecision::FrequencyRejected;
        return result;
    }
    if (spread.exclusion) {
        const StructureSet* other = impl_->sets->find(spread.exclusion->other_set);
        if (other != nullptr && other->spread) {
            const i32 radius = spread.exclusion->chunk_count;
            for (i32 dz = -radius; dz <= radius; ++dz) {
                for (i32 dx = -radius; dx <= radius; ++dx) {
                    if (other->spread->is_candidate_chunk(level_seed, chunk_x + dx, chunk_z + dz) &&
                        other->spread->passes_frequency(level_seed, chunk_x + dx, chunk_z + dz)) {
                        result.decision = PlacementDecision::ExcludedByZone;
                        return result;
                    }
                }
            }
        }
    }

    // The set's members are tried by repeated weighted draw from one stream,
    // the failed one removed each time — not scanned in order. The difference
    // only shows on a set of more than two: with the five village variants, a
    // scan takes the first whose biome allows it and the game takes the one its
    // second draw names. Measured: the plains village of chunk (3006, 10) is a
    // false negative under a scan and correct under the loop.
    std::vector<const StructureSetEntry*> remaining;
    remaining.reserve(set.entries.size());
    for (const StructureSetEntry& entry : set.entries) {
        remaining.push_back(&entry);
    }
    i32 total = set.total_weight();

    math::LegacyRandomSource random{0};
    random.set_seed(large_feature_seed(level_seed, chunk_x, chunk_z));

    PlacementDecision last = PlacementDecision::BiomeRejected;
    while (!remaining.empty() && total > 0) {
        usize index = 0;
        i32   roll  = random.next_int(total);
        for (usize candidate = 0; candidate < remaining.size(); ++candidate) {
            roll -= remaining[candidate]->weight;
            if (roll < 0) {
                index = candidate;
                break;
            }
        }
        const StructureSetEntry* entry = remaining[index];
        result.structure               = entry->structure;

        const StructureDefinition* definition = find(entry->structure);
        if (definition == nullptr) {
            result.decision = PlacementDecision::Unsupported;
            return result;
        }

        if (definition->anchor == GenerationAnchor::None) {
            result.decision = PlacementDecision::PlacedByPlacement;
            return result;
        }
        if (sampler == nullptr) {
            result.decision = PlacementDecision::BiomeUnknown;
            return result;
        }

        // The column the biome is read from: the chunk's middle for the
        // scattered structures, its minimum corner for the jigsaw ones.
        const usize slot    = definition->anchor_at_corner ? 1u : 0u;
        const i32   offset  = definition->anchor_at_corner ? 0 : 8;
        const i32   block_x = chunk_x * kSectionSize + offset;
        const i32   block_z = chunk_z * kSectionSize + offset;
        switch (definition->anchor) {
            case GenerationAnchor::SurfaceCentre:
                if (cache.surface[slot] < 0) {
                    cache.surface[slot] = sampler->surface_height(block_x, block_z);
                }
                result.anchor_y = cache.surface[slot];
                break;
            case GenerationAnchor::OceanFloorCentre:
                if (cache.ocean_floor[slot] < 0) {
                    cache.ocean_floor[slot] = sampler->ocean_floor_height(block_x, block_z);
                }
                result.anchor_y = cache.ocean_floor[slot];
                break;
            case GenerationAnchor::FixedHeight:
                result.anchor_y = definition->anchor_height;
                break;
            case GenerationAnchor::None:
                break;
        }
        result.biome = sampler->biome_at(block_x, result.anchor_y, block_z);
        if (result.biome.empty()) {
            result.decision = PlacementDecision::BiomeUnknown;
            return result;
        }
        if (biome_allowed(impl_->tags, definition->biomes, result.biome)) {
            result.decision = PlacementDecision::PlacedByPlacement;
            return result;
        }

        last = PlacementDecision::BiomeRejected;
        total -= entry->weight;
        remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(index));
    }

    result.decision = last;
    return result;
}

std::vector<StructurePlacementResult> StructurePlacer::decide(
    i64 level_seed, i32 chunk_x, i32 chunk_z, const StructureWorldSampler* sampler) const {
    std::vector<StructurePlacementResult> out;
    out.reserve(impl_->sets->sets().size());
    // One cache for the whole chunk: the nineteen sets ask about two columns.
    AnchorColumns columns;
    for (const StructureSet& set : impl_->sets->sets()) {
        out.push_back(decide_set(set, level_seed, chunk_x, chunk_z, sampler, &columns));
    }
    return out;
}

}  // namespace ov::worldgen
