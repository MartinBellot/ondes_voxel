// The template pools and the jigsaw structures' settings, read from the
// generated datapack; the templates from the jar, through `TemplateLibrary`.
#define OV_LOG_CATEGORY "worldgen"

#include "jigsaw_impl.hpp"

#include "ov/base/log.hpp"
#include "ov/worldgen/jigsaw.hpp"

#include <simdjson.h>

#include <algorithm>
#include <map>
#include <string>
#include <tuple>
#include <utility>

namespace ov::worldgen {

namespace {

void set_detail(std::string* detail, std::string text) {
    if (detail != nullptr) {
        *detail = std::move(text);
    }
}

[[nodiscard]] std::string with_namespace(std::string_view name) {
    return name.find(':') == std::string_view::npos ? "minecraft:" + std::string{name}
                                                    : std::string{name};
}

[[nodiscard]] std::string strip_namespace(std::string_view name) {
    const auto colon = name.find(':');
    return std::string{colon == std::string_view::npos ? name : name.substr(colon + 1)};
}

[[nodiscard]] std::optional<PoolElementType> element_type_from(std::string_view name) {
    const std::string bare = strip_namespace(name);
    if (bare == "single_pool_element") {
        return PoolElementType::Single;
    }
    if (bare == "legacy_single_pool_element") {
        return PoolElementType::LegacySingle;
    }
    if (bare == "list_pool_element") {
        return PoolElementType::List;
    }
    if (bare == "feature_pool_element") {
        return PoolElementType::Feature;
    }
    if (bare == "empty_pool_element") {
        return PoolElementType::Empty;
    }
    return std::nullopt;
}

/// A key naming an element by what the game serialises of it: two elements
/// with the same key are the same element to the game.
[[nodiscard]] std::string element_key(PoolElementType type, std::string_view location,
                                      std::string_view feature, std::string_view processors,
                                      Projection projection, std::string_view children) {
    std::string key{to_string(type)};
    key += '|';
    key += location;
    key += '|';
    key += feature;
    key += '|';
    key += processors;
    key += '|';
    key += to_string(projection);
    key += '|';
    key += children;
    return key;
}

[[nodiscard]] std::string element_key(const PoolElement& element) {
    std::string children;
    for (const PoolElement& child : element.elements) {
        children += '[' + element_key(child) + ']';
    }
    return element_key(element.type, element.location, element.feature, element.processors,
                       element.projection, children);
}

[[nodiscard]] std::optional<Projection> projection_from(std::string_view name) {
    if (name == "rigid") {
        return Projection::Rigid;
    }
    if (name == "terrain_matching") {
        return Projection::TerrainMatching;
    }
    return std::nullopt;
}

}  // namespace

std::string_view to_string(Projection projection) noexcept {
    return projection == Projection::Rigid ? "rigid" : "terrain_matching";
}

std::string_view to_string(PoolElementType type) noexcept {
    switch (type) {
        case PoolElementType::Single: return "minecraft:single_pool_element";
        case PoolElementType::LegacySingle: return "minecraft:legacy_single_pool_element";
        case PoolElementType::List: return "minecraft:list_pool_element";
        case PoolElementType::Feature: return "minecraft:feature_pool_element";
        case PoolElementType::Empty: return "minecraft:empty_pool_element";
    }
    return "?";
}

Direction rotate(Direction direction, Rotation rotation) noexcept {
    if (direction == Direction::Up || direction == Direction::Down) {
        return direction;
    }
    // North, east, south, west: clockwise order.
    constexpr Direction kClockwise[4] = {Direction::North, Direction::East, Direction::South,
                                         Direction::West};
    usize index = 0;
    while (kClockwise[index] != direction) {
        ++index;
    }
    return kClockwise[(index + static_cast<usize>(rotation)) & 3U];
}

BoundingBox PoolElement::box(BlockPos pos, Rotation rotation) const noexcept {
    switch (type) {
        case PoolElementType::Single:
        case PoolElementType::LegacySingle: {
            // The template's box, turned about the zero pivot. A template the
            // jar lacks has size 0 and a box one block below and behind the
            // position — what the game's arithmetic gives for size − 1 = −1.
            const BlockPos a = transform({0, 0, 0}, Mirror::None, rotation, {0, 0, 0});
            const BlockPos b = transform({size.x - 1, size.y - 1, size.z - 1}, Mirror::None,
                                         rotation, {0, 0, 0});
            return {pos.x + std::min(a.x, b.x), pos.y + std::min(a.y, b.y),
                    pos.z + std::min(a.z, b.z), pos.x + std::max(a.x, b.x),
                    pos.y + std::max(a.y, b.y), pos.z + std::max(a.z, b.z)};
        }
        case PoolElementType::List: {
            BoundingBox out = elements.front().box(pos, rotation);
            for (const PoolElement& child : elements) {
                out.encapsulate(child.box(pos, rotation));
            }
            return out;
        }
        case PoolElementType::Feature: return {pos.x, pos.y, pos.z, pos.x, pos.y, pos.z};
        case PoolElementType::Empty: break;
    }
    return {};
}

JigsawLibrary::JigsawLibrary() : impl_(std::make_unique<Impl>()) {}
JigsawLibrary::JigsawLibrary(JigsawLibrary&&) noexcept            = default;
JigsawLibrary& JigsawLibrary::operator=(JigsawLibrary&&) noexcept = default;
JigsawLibrary::~JigsawLibrary()                                   = default;

namespace {

struct Loader {
    const std::filesystem::path*   data_root{nullptr};
    const TemplateLibrary*         templates{nullptr};
    const registry::BlockRegistry* blocks{nullptr};
    const BlockTags*               tags{nullptr};
    std::map<std::string, std::shared_ptr<const ProcessorList>, std::less<>> lists;
    std::map<std::string, std::string, std::less<>>                          list_refusals;
    std::vector<std::string>*                                               missing{nullptr};
    std::optional<registry::BlockId>                                        jigsaw_block;

    /// A named processor list, read once.
    void resolve_list(const std::string& name, PoolElement& element) {
        if (const auto found = lists.find(name); found != lists.end()) {
            element.processor_list = found->second;
            return;
        }
        if (const auto refused = list_refusals.find(name); refused != list_refusals.end()) {
            element.refusal = refused->second;
            return;
        }
        const auto path = *data_root / "worldgen" / "processor_list" /
                          (strip_namespace(name) + ".json");
        auto text = simdjson::padded_string::load(path.string());
        if (text.error() != simdjson::SUCCESS) {
            list_refusals.emplace(name, name + ": no such processor list");
            element.refusal = name + ": no such processor list";
            return;
        }
        std::string why;
        auto        parsed = ProcessorList::parse_json(
            std::string_view{text.value().data(), text.value().size()}, *blocks, tags, &why);
        if (!parsed) {
            list_refusals.emplace(name, name + ": " + why);
            element.refusal = name + ": " + why;
            return;
        }
        auto shared = std::make_shared<const ProcessorList>(std::move(*parsed));
        lists.emplace(name, shared);
        element.processor_list = std::move(shared);
    }

    /// The template's jigsaw blocks, in (y, x, z) order — the order the game's
    /// template keeps its block entities in, and the order the shuffle starts
    /// from. On the 1.20.1 templates it is also the file's order.
    void read_connectors(PoolElement& element) {
        element.tpl = templates->find(element.location);
        if (element.tpl == nullptr) {
            element.size = {0, 0, 0};
            if (std::ranges::find(*missing, element.location) == missing->end()) {
                missing->push_back(element.location);
            }
            return;
        }
        element.size             = element.tpl->size;
        const auto& palette      = element.tpl->palettes.front();
        const auto  orientation  = jigsaw_block
                                       ? blocks->find_property(*jigsaw_block, "orientation")
                                       : std::nullopt;
        for (const TemplateBlock& block : element.tpl->blocks) {
            const registry::BlockStateId state = palette[block.state];
            if (!jigsaw_block || blocks->block_of(state) != *jigsaw_block || !orientation) {
                continue;
            }
            JigsawConnector connector;
            connector.pos                 = block.pos;
            const std::string_view facing = blocks->property_value(state, *orientation);
            const auto             split  = facing.find('_');
            connector.front =
                direction_from_name(facing.substr(0, split)).value_or(Direction::North);
            connector.top = direction_from_name(facing.substr(split + 1)).value_or(Direction::Up);
            const nbt::Tag* data =
                block.nbt >= 0 ? &element.tpl->block_nbt[static_cast<usize>(block.nbt)] : nullptr;
            const auto text = [&](std::string_view key) {
                const nbt::Tag* tag = data != nullptr ? data->find(key) : nullptr;
                return tag != nullptr ? std::string{tag->as_string()} : std::string{};
            };
            connector.name        = text("name");
            connector.target      = text("target");
            connector.pool        = text("pool");
            connector.final_state = text("final_state");
            const std::string joint = text("joint");
            const bool sideways = connector.front != Direction::Up && connector.front != Direction::Down;
            connector.rollable  = joint.empty() ? !sideways : joint == "rollable";
            element.connectors.push_back(std::move(connector));
        }
        std::ranges::stable_sort(element.connectors, [](const JigsawConnector& a,
                                                         const JigsawConnector& b) {
            return std::tie(a.pos.y, a.pos.x, a.pos.z) < std::tie(b.pos.y, b.pos.x, b.pos.z);
        });
    }

    std::expected<PoolElement, std::string> parse_element(simdjson::dom::element node) {
        PoolElement      element;
        std::string_view type;
        if (node["element_type"].get(type) != simdjson::SUCCESS) {
            return std::unexpected(std::string{"element without element_type"});
        }
        const auto parsed_type = element_type_from(type);
        if (!parsed_type) {
            return std::unexpected("element type " + std::string{type} + " is not known");
        }
        element.type = *parsed_type;
        std::string_view projection;
        if (node["projection"].get(projection) == simdjson::SUCCESS) {
            const auto parsed = projection_from(projection);
            if (!parsed) {
                return std::unexpected("projection " + std::string{projection} + " is not known");
            }
            element.projection = *parsed;
        }
        switch (element.type) {
            case PoolElementType::Single:
            case PoolElementType::LegacySingle: {
                std::string_view location;
                if (node["location"].get(location) != simdjson::SUCCESS) {
                    return std::unexpected(std::string{"single element without location"});
                }
                element.location = with_namespace(location);
                std::string_view reference;
                simdjson::dom::element processors;
                if (node["processors"].get(reference) == simdjson::SUCCESS) {
                    element.processors = with_namespace(reference);
                    resolve_list(element.processors, element);
                } else if (node["processors"].get(processors) == simdjson::SUCCESS) {
                    std::string why;
                    auto        inline_list =
                        ProcessorList::parse_json(simdjson::to_string(processors), *blocks, tags, &why);
                    if (inline_list) {
                        if (!inline_list->processors.empty()) {
                            element.refusal = "an inline processor list that is not empty";
                        }
                        element.processor_list =
                            std::make_shared<const ProcessorList>(std::move(*inline_list));
                    } else {
                        element.refusal = "inline processors: " + why;
                    }
                }
                read_connectors(element);
                break;
            }
            case PoolElementType::Feature: {
                std::string_view feature;
                if (node["feature"].get(feature) != simdjson::SUCCESS) {
                    return std::unexpected(std::string{"feature element without feature"});
                }
                element.feature = with_namespace(feature);
                break;
            }
            case PoolElementType::List: {
                simdjson::dom::array children;
                if (node["elements"].get(children) != simdjson::SUCCESS) {
                    return std::unexpected(std::string{"list element without elements"});
                }
                for (const simdjson::dom::element child : children) {
                    auto parsed = parse_element(child);
                    if (!parsed) {
                        return parsed;
                    }
                    // The list's projection is every child's.
                    parsed->projection = element.projection;
                    element.elements.push_back(std::move(*parsed));
                }
                if (element.elements.empty()) {
                    return std::unexpected(std::string{"empty list element"});
                }
                break;
            }
            case PoolElementType::Empty: break;
        }
        return element;
    }
};

void index_element(std::map<std::string, const PoolElement*, std::less<>>& by_key,
                   const PoolElement&                                       element) {
    by_key.emplace(element_key(element), &element);
}

}  // namespace

std::expected<JigsawLibrary, TemplateError> JigsawLibrary::load(
    const std::filesystem::path& data_root, const TemplateLibrary& templates,
    const registry::BlockRegistry& blocks, const BlockTags& tags, std::string* detail) {
    JigsawLibrary library;
    Loader        loader;
    loader.data_root    = &data_root;
    loader.templates    = &templates;
    loader.blocks       = &blocks;
    loader.tags         = &tags;
    loader.missing      = &library.impl_->missing;
    loader.jigsaw_block = blocks.find_block("minecraft:jigsaw");

    const auto      pool_dir = data_root / "worldgen" / "template_pool";
    std::error_code error;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(pool_dir, error)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            files.push_back(entry.path());
        }
    }
    if (error || files.empty()) {
        set_detail(detail, pool_dir.string() + ": no template pools");
        return std::unexpected(TemplateError::NotFound);
    }
    std::ranges::sort(files);

    simdjson::dom::parser parser;
    for (const auto& path : files) {
        std::string relative = std::filesystem::relative(path, pool_dir).generic_string();
        relative.resize(relative.size() - 5);  // .json
        auto pool  = std::make_unique<TemplatePool>();
        pool->name = "minecraft:" + relative;

        simdjson::dom::element document;
        if (parser.load(path.string()).get(document) != simdjson::SUCCESS) {
            set_detail(detail, path.string() + " is not JSON");
            return std::unexpected(TemplateError::Malformed);
        }
        std::string_view fallback;
        if (document["fallback"].get(fallback) == simdjson::SUCCESS) {
            pool->fallback = with_namespace(fallback);
        }
        simdjson::dom::array elements;
        if (document["elements"].get(elements) != simdjson::SUCCESS) {
            set_detail(detail, pool->name + ": no elements");
            return std::unexpected(TemplateError::Malformed);
        }
        std::vector<std::pair<PoolElement*, i64>> weights;
        for (const simdjson::dom::element entry : elements) {
            simdjson::dom::element node;
            i64                    weight = 1;
            if (entry["element"].get(node) != simdjson::SUCCESS) {
                set_detail(detail, pool->name + ": an entry without element");
                return std::unexpected(TemplateError::Malformed);
            }
            (void)entry["weight"].get(weight);
            auto parsed = loader.parse_element(node);
            if (!parsed) {
                set_detail(detail, pool->name + ": " + parsed.error());
                return std::unexpected(TemplateError::Malformed);
            }
            pool->elements.push_back(std::make_unique<PoolElement>(std::move(*parsed)));
            weights.emplace_back(pool->elements.back().get(), weight);
        }
        for (const auto& [owned, weight] : weights) {
            // One object per element the game would serialise the same way:
            // `bastion/mobs/melee_piglin` sits in several pools, and a piece
            // read back from the NBT must name the very element it was grown
            // from.
            index_element(library.impl_->by_key, *owned);
            const PoolElement* element = library.impl_->by_key.find(element_key(*owned))->second;
            for (i64 copy = 0; copy < weight; ++copy) {
                pool->weighted.push_back(element);
            }
            if (element->type != PoolElementType::Empty) {
                const BoundingBox box = element->box({0, 0, 0}, Rotation::None);
                pool->max_size        = std::max(pool->max_size, box.max_y - box.min_y + 1);
            }
        }
        library.impl_->pools.emplace(pool->name, std::move(pool));
    }

    // The jigsaw structures' own settings.
    const auto structure_dir = data_root / "worldgen" / "structure";
    for (const auto& entry : std::filesystem::directory_iterator(structure_dir, error)) {
        if (entry.path().extension() != ".json") {
            continue;
        }
        simdjson::dom::element document;
        if (parser.load(entry.path().string()).get(document) != simdjson::SUCCESS) {
            continue;
        }
        std::string_view type;
        if (document["type"].get(type) != simdjson::SUCCESS || type != "minecraft:jigsaw") {
            continue;
        }
        JigsawConfig config;
        config.name = "minecraft:" + entry.path().stem().string();
        std::string_view text;
        if (document["start_pool"].get(text) == simdjson::SUCCESS) {
            config.start_pool = with_namespace(text);
        }
        if (document["start_jigsaw_name"].get(text) == simdjson::SUCCESS) {
            config.start_jigsaw_name = with_namespace(text);
        }
        if (document["project_start_to_heightmap"].get(text) == simdjson::SUCCESS) {
            config.project_to = world::heightmap_type_from(text);
        }
        if (document["terrain_adaptation"].get(text) == simdjson::SUCCESS) {
            config.terrain_adaptation = std::string{text};
        }
        i64 number = 0;
        if (document["size"].get(number) == simdjson::SUCCESS) {
            config.size = static_cast<i32>(number);
        }
        if (document["max_distance_from_center"].get(number) == simdjson::SUCCESS) {
            config.max_distance = static_cast<i32>(number);
        }
        bool flag = false;
        if (document["use_expansion_hack"].get(flag) == simdjson::SUCCESS) {
            config.expansion_hack = flag;
        }
        // Every 1.20.1 start height is a constant `absolute` anchor, which draws
        // nothing. Any other height provider would draw from the structure's
        // random before the rotation, and is refused rather than guessed.
        if (document["start_height"]["absolute"].get(number) == simdjson::SUCCESS) {
            config.start_height = static_cast<i32>(number);
        } else {
            OV_LOG_ERROR("jigsaw: {} has a start height that is not a constant absolute one; "
                         "it is not built",
                         config.name);
            continue;
        }
        library.impl_->configs.emplace(config.name, std::move(config));
    }

    // The processors every element gets, spelled as their JSON twins.
    const auto common = [&](std::string_view body) -> ProcessorRef {
        std::string why;
        auto list = ProcessorList::parse_json(body, blocks, &tags, &why);
        return list && !list->processors.empty() ? list->processors.front() : nullptr;
    };
    library.impl_->ignore_structure_block = common(
        R"({"processors":[{"processor_type":"minecraft:block_ignore","blocks":[{"Name":"minecraft:structure_block"}]}]})");
    library.impl_->ignore_structure_and_air = common(
        R"({"processors":[{"processor_type":"minecraft:block_ignore","blocks":[{"Name":"minecraft:structure_block"},{"Name":"minecraft:air"}]}]})");
    library.impl_->jigsaw_replacement =
        common(R"({"processors":[{"processor_type":"minecraft:jigsaw_replacement"}]})");
    library.impl_->gravity = common(
        R"({"processors":[{"processor_type":"minecraft:gravity","heightmap":"WORLD_SURFACE_WG","offset":-1}]})");
    if (!library.impl_->ignore_structure_block || !library.impl_->ignore_structure_and_air ||
        !library.impl_->jigsaw_replacement || !library.impl_->gravity) {
        set_detail(detail, "jigsaw: the common processors did not parse");
        return std::unexpected(TemplateError::BadProcessor);
    }

    for (const std::string& name : library.impl_->missing) {
        OV_LOG_INFO("jigsaw: {} is named by a pool and absent from the jar; placed empty, as the "
                    "game does",
                    name);
    }
    return library;
}

const JigsawConfig* JigsawLibrary::config(std::string_view structure) const noexcept {
    const auto found = impl_->configs.find(structure);
    return found == impl_->configs.end() ? nullptr : &found->second;
}

const TemplatePool* JigsawLibrary::pool(std::string_view name) const noexcept {
    const auto found = impl_->pools.find(name);
    return found == impl_->pools.end() ? nullptr : found->second.get();
}

usize JigsawLibrary::pool_count() const noexcept {
    return impl_->pools.size();
}

const std::vector<std::string>& JigsawLibrary::missing_templates() const noexcept {
    return impl_->missing;
}

// ── The `pool_element` compound ─────────────────────────────────────────────

nbt::Tag JigsawLibrary::element_to_nbt(const PoolElement& element) {
    nbt::Tag out = nbt::Tag::make_compound();
    switch (element.type) {
        case PoolElementType::Single:
        case PoolElementType::LegacySingle: {
            out.put("location", nbt::Tag{element.location});
            if (element.processors.empty()) {
                nbt::Tag inline_list = nbt::Tag::make_compound();
                inline_list.put("processors", nbt::Tag::make_list(nbt::TagType::End));
                out.put("processors", std::move(inline_list));
            } else {
                out.put("processors", nbt::Tag{element.processors});
            }
            out.put("projection", nbt::Tag{std::string{to_string(element.projection)}});
            break;
        }
        case PoolElementType::Feature:
            out.put("feature", nbt::Tag{element.feature});
            out.put("projection", nbt::Tag{std::string{to_string(element.projection)}});
            break;
        case PoolElementType::List: {
            nbt::Tag children = nbt::Tag::make_list(nbt::TagType::Compound);
            for (const PoolElement& child : element.elements) {
                children.push(element_to_nbt(child));
            }
            out.put("elements", std::move(children));
            out.put("projection", nbt::Tag{std::string{to_string(element.projection)}});
            break;
        }
        case PoolElementType::Empty: break;
    }
    out.put("element_type", nbt::Tag{std::string{to_string(element.type)}});
    return out;
}

namespace {

[[nodiscard]] std::optional<std::string> key_from_nbt(const nbt::Tag& tag) {
    const nbt::Tag* type_tag = tag.find("element_type");
    if (type_tag == nullptr) {
        return std::nullopt;
    }
    const auto type = element_type_from(type_tag->as_string());
    if (!type) {
        return std::nullopt;
    }
    const auto text = [&](std::string_view key) {
        const nbt::Tag* found = tag.find(key);
        return found != nullptr ? std::string{found->as_string()} : std::string{};
    };
    Projection projection = Projection::Rigid;
    if (const auto parsed = projection_from(text("projection"))) {
        projection = *parsed;
    }
    std::string processors;
    if (const nbt::Tag* found = tag.find("processors");
        found != nullptr && found->type() == nbt::TagType::String) {
        processors = with_namespace(found->as_string());
    }
    std::string children;
    if (const nbt::Tag* list = tag.find("elements"); list != nullptr && list->list() != nullptr) {
        for (const nbt::Tag& child : *list->list()) {
            const auto key = key_from_nbt(child);
            if (!key) {
                return std::nullopt;
            }
            children += '[' + *key + ']';
        }
    }
    const std::string location = text("location");
    const std::string feature  = text("feature");
    return element_key(*type, location.empty() ? location : with_namespace(location),
                       feature.empty() ? feature : with_namespace(feature), processors,
                       projection, children);
}

}  // namespace

const PoolElement* JigsawLibrary::element_from_nbt(const nbt::Tag& pool_element) const {
    const auto key = key_from_nbt(pool_element);
    if (!key) {
        return nullptr;
    }
    const auto found = impl_->by_key.find(*key);
    return found == impl_->by_key.end() ? nullptr : found->second;
}

}  // namespace ov::worldgen
