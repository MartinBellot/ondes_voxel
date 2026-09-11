#include "ov/render/model.hpp"

#include "json.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>

namespace ov::render {

namespace {

/// A model file exactly as written, before the parent chain is walked.
/// `sprite` here is still whatever the file said, `#side` included.
struct RawModel {
    std::optional<ResourceLocation> parent;
    /// Absent, not empty: a model with `"elements": []` deliberately draws
    /// nothing and must not inherit its parent's boxes.
    std::optional<std::vector<Element>>              elements;
    std::vector<std::pair<std::string, std::string>> textures;
    std::optional<bool>                              ambient_occlusion;
    /// `display.gui` as written. Merged down the chain like a texture, because
    /// vanilla's own item models rely on it: `item/handheld` overrides two of
    /// `item/generated`'s positions and inherits the rest.
    std::optional<DisplayTransform> gui_display;
    /// `display.thirdperson_righthand` as written, merged the same way.
    std::optional<DisplayTransform> hand_display;
    /// `gui_light`, when the file states it.
    std::optional<bool> gui_light_front;
};

[[nodiscard]] std::optional<Vec3f> read_vec3(const json::Value& value) {
    if (!value.is_array() || value.size() != 3) {
        return std::nullopt;
    }
    for (u32 i = 0; i < 3; ++i) {
        if (!value[i].is_number()) {
            return std::nullopt;
        }
    }
    return Vec3f{static_cast<f32>(value[0].as_number()), static_cast<f32>(value[1].as_number()),
                 static_cast<f32>(value[2].as_number())};
}

[[nodiscard]] std::optional<std::array<f32, 4>> read_uv(const json::Value& value) {
    if (!value.is_array() || value.size() != 4) {
        return std::nullopt;
    }
    std::array<f32, 4> uv{};
    for (u32 i = 0; i < 4; ++i) {
        if (!value[i].is_number()) {
            return std::nullopt;
        }
        uv[i] = static_cast<f32>(value[i].as_number());
    }
    return uv;
}

[[nodiscard]] std::optional<FaceDefinition> read_face(const json::Value& value) {
    if (!value.is_object()) {
        return std::nullopt;
    }

    FaceDefinition face;
    face.sprite = std::string(value["texture"].as_string());
    face.uv     = read_uv(value["uv"]);

    if (const auto cull = value["cullface"]; cull.is_string()) {
        // "bottom" is an accepted spelling of "down", and not a hypothetical
        // one: four faces in the 1.20.1 assets use it. Anything else unknown
        // means no culling rather than an error, so one typo in a downloaded
        // pack does not make a whole block fail to load.
        auto name = cull.as_string();
        if (name == "bottom") {
            name = "down";
        }
        face.cullface = direction_from_name(name);
    }

    if (const auto rotation = value["rotation"]; rotation.is_number()) {
        face.rotation = static_cast<i32>(rotation.as_number());
    }
    if (const auto tint = value["tintindex"]; tint.is_number()) {
        face.tint_index = static_cast<i32>(tint.as_number());
    }
    return face;
}

[[nodiscard]] std::optional<Element> read_element(const json::Value& value) {
    const auto from = read_vec3(value["from"]);
    const auto to   = read_vec3(value["to"]);
    if (!from || !to) {
        return std::nullopt;
    }

    Element element;
    element.from  = *from;
    element.to    = *to;
    element.shade = value["shade"].as_bool(true);

    if (const auto rotation = value["rotation"]; rotation.is_object()) {
        ElementRotation r;
        if (const auto origin = read_vec3(rotation["origin"])) {
            r.origin = *origin;
        }
        if (const auto axis = axis_from_name(rotation["axis"].as_string())) {
            r.axis = *axis;
        }
        r.angle          = static_cast<f32>(rotation["angle"].as_number());
        r.rescale        = rotation["rescale"].as_bool(false);
        element.rotation = r;
    }

    const auto faces = value["faces"];
    for (u32 i = 0; i < faces.size(); ++i) {
        const auto direction = direction_from_name(faces.key_at(i));
        if (!direction) {
            continue;
        }
        element.faces[static_cast<usize>(*direction)] = read_face(faces.value_at(i));
    }
    return element;
}

[[nodiscard]] std::expected<RawModel, ModelError> parse_model(std::span<const u8> bytes) {
    auto document = json::Document::parse(bytes);
    if (!document) {
        return std::unexpected(ModelError::Malformed);
    }

    const auto root = document->root();
    if (!root.is_object()) {
        return std::unexpected(ModelError::Malformed);
    }

    RawModel model;
    if (const auto parent = root["parent"]; parent.is_string()) {
        auto location = ResourceLocation::parse(parent.as_string());
        if (!location) {
            return std::unexpected(ModelError::Malformed);
        }
        model.parent = std::move(*location);
    }
    if (const auto ao = root["ambientocclusion"]; ao.is_bool()) {
        model.ambient_occlusion = ao.as_bool();
    }

    const auto textures = root["textures"];
    for (u32 i = 0; i < textures.size(); ++i) {
        const auto value = textures.value_at(i);
        if (value.is_string()) {
            model.textures.emplace_back(std::string(textures.key_at(i)),
                                        std::string(value.as_string()));
        }
    }

    if (const auto light = root["gui_light"]; light.is_string()) {
        model.gui_light_front = light.as_string() == "front";
    }

    const auto read_display = [](const json::Value entry) -> std::optional<DisplayTransform> {
        if (!entry.is_object()) {
            return std::nullopt;
        }
        DisplayTransform transform;
        if (const auto rotation = read_vec3(entry["rotation"])) {
            transform.rotation = *rotation;
        }
        if (const auto translation = read_vec3(entry["translation"])) {
            // The file writes translation in sixteenths of a block, the same
            // unit an element's from/to uses; everything downstream works in
            // blocks.
            transform.translation = *translation * (1.0F / 16.0F);
        }
        if (const auto scale = read_vec3(entry["scale"])) {
            transform.scale = *scale;
        }
        return transform;
    };
    model.gui_display  = read_display(root["display"]["gui"]);
    model.hand_display = read_display(root["display"]["thirdperson_righthand"]);

    if (const auto elements = root["elements"]; elements.is_array()) {
        std::vector<Element> parsed;
        parsed.reserve(elements.size());
        for (u32 i = 0; i < elements.size(); ++i) {
            if (auto element = read_element(elements[i])) {
                parsed.push_back(std::move(*element));
            } else {
                return std::unexpected(ModelError::Malformed);
            }
        }
        model.elements = std::move(parsed);
    }

    return model;
}

/// Chase `#variable` through the merged texture map until a real sprite falls
/// out. Bounded because a pack may write `"#a": "#b", "#b": "#a"`.
[[nodiscard]] std::string resolve_sprite(
    std::string_view reference, const std::unordered_map<std::string, std::string>& textures) {
    std::string_view current = reference;
    for (u32 hop = 0; hop < ModelLoader::kMaxParentDepth; ++hop) {
        if (current.empty()) {
            return std::string(kMissingSprite);
        }
        if (current.front() != '#') {
            auto location = ResourceLocation::parse(current);
            return location ? location->full() : std::string(kMissingSprite);
        }
        const auto it = textures.find(std::string(current.substr(1)));
        if (it == textures.end()) {
            return std::string(kMissingSprite);
        }
        current = it->second;
    }
    return std::string(kMissingSprite);
}

}  // namespace

std::string_view axis_name(Axis axis) noexcept {
    switch (axis) {
        case Axis::X: return "x";
        case Axis::Y: return "y";
        case Axis::Z: return "z";
    }
    return "y";
}

std::optional<Axis> axis_from_name(std::string_view name) noexcept {
    if (name == "x") {
        return Axis::X;
    }
    if (name == "y") {
        return Axis::Y;
    }
    if (name == "z") {
        return Axis::Z;
    }
    return std::nullopt;
}

std::string_view to_string(ModelError error) noexcept {
    switch (error) {
        case ModelError::NotFound: return "model not found in the pack stack";
        case ModelError::Malformed: return "model file is malformed";
        case ModelError::ParentChainTooLong: return "model parent chain is too long or loops";
    }
    return "unknown model error";
}

struct ModelLoader::Impl {
    const AssetSource*                                       source;
    std::unordered_map<std::string, std::unique_ptr<Model>>  resolved;
    std::unordered_map<std::string, std::optional<RawModel>> raw;

    /// Parsed file, or nullptr when the pack does not have it. Cached either
    /// way: 1005 blockstates resolve to 2016 model files with a great deal of
    /// sharing, and re-reading `block/block` for every one of them is the
    /// difference between a startup and a wait.
    [[nodiscard]] std::expected<const RawModel*, ModelError> raw_model(
        const ResourceLocation& location) {
        const auto& key = location.full();
        if (const auto it = raw.find(key); it != raw.end()) {
            if (!it->second) {
                return std::unexpected(ModelError::NotFound);
            }
            return &*it->second;
        }

        const auto bytes = source->read(model_asset_path(location));
        if (!bytes) {
            raw.emplace(key, std::nullopt);
            return std::unexpected(ModelError::NotFound);
        }

        auto parsed = parse_model(*bytes);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        const auto [it, _] = raw.insert_or_assign(key, std::move(*parsed));
        return &*it->second;
    }
};

ModelLoader::ModelLoader(const AssetSource& source) : impl_(std::make_unique<Impl>()) {
    impl_->source = &source;
}

ModelLoader::ModelLoader(ModelLoader&&) noexcept            = default;
ModelLoader& ModelLoader::operator=(ModelLoader&&) noexcept = default;
ModelLoader::~ModelLoader()                                 = default;

usize ModelLoader::cached_models() const noexcept {
    return impl_->resolved.size();
}

std::expected<const Model*, ModelError> ModelLoader::load(const ResourceLocation& location) {
    const auto& key = location.full();
    if (const auto it = impl_->resolved.find(key); it != impl_->resolved.end()) {
        return it->second.get();
    }

    // Walk the parent chain leaf-first. Nothing recursive: the chain is a list,
    // and a pack that makes it a loop hits the depth limit instead of the
    // stack.
    std::vector<const RawModel*> chain;
    ResourceLocation             current    = location;
    bool                         generated  = false;
    for (u32 depth = 0;; ++depth) {
        if (depth >= kMaxParentDepth) {
            return std::unexpected(ModelError::ParentChainTooLong);
        }
        auto raw = impl_->raw_model(current);
        if (!raw) {
            return std::unexpected(raw.error());
        }
        chain.push_back(*raw);

        const auto& parent = (*raw)->parent;
        // builtin/generated and builtin/entity have no file behind them: they
        // tell the game to build geometry in code. For a block model the chain
        // simply ends there.
        if (!parent) {
            break;
        }
        if (is_builtin_model(*parent)) {
            // *Which* builtin matters for an item: `builtin/generated` means
            // "extrude layer0 into an icon", and an item whose chain ends there
            // has no elements at all. Losing that distinction is how every
            // flat item ends up drawn as nothing.
            generated = parent->path() == "builtin/generated";
            break;
        }
        current = *parent;
    }

    // Textures merge root-first so that a child overrides its parent, which is
    // what lets block/cube_all say `"all": "block/stone"` and inherit the six
    // faces that reference it.
    std::unordered_map<std::string, std::string> textures;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        for (const auto& [name, value] : (*it)->textures) {
            textures.insert_or_assign(name, value);
        }
    }

    auto model = std::make_unique<Model>();

    // Elements do not merge: the nearest file in the chain that declares them
    // replaces its parent's entirely. The format documents this as "if both
    // parent and elements are set, the elements tag overrides".
    for (const auto* raw : chain) {
        if (raw->elements) {
            model->elements = *raw->elements;
            break;
        }
    }

    // Ambient occlusion is declared by the root of the chain, not the leaf.
    // The format's own wording is "only works on Parent file"; measured against
    // the 1.20.1 assets, every model that declares it is itself a root.
    if (chain.back()->ambient_occlusion) {
        model->ambient_occlusion = *chain.back()->ambient_occlusion;
    }

    for (auto& element : model->elements) {
        for (auto& face : element.faces) {
            if (face) {
                face->sprite = resolve_sprite(face->sprite, textures);
            }
        }
    }

    if (const auto particle = textures.find("particle"); particle != textures.end()) {
        model->particle_sprite = resolve_sprite(particle->second, textures);
    }

    // The GUI transform and the light mode come from the nearest file in the
    // chain that states them, leaf first — the same rule as elements, and not
    // the same rule as textures.
    model->generated = generated;
    for (const auto* raw : chain) {
        if (raw->gui_display && !model->gui_display) {
            model->gui_display = raw->gui_display;
        }
        if (raw->hand_display && !model->hand_display) {
            model->hand_display = raw->hand_display;
        }
        if (raw->gui_light_front && !model->gui_light_front) {
            model->gui_light_front = *raw->gui_light_front;
        }
    }

    // `layer0`, `layer1`, … in order, stopping at the first gap. Vanilla's own
    // item models never skip a number, and stopping rather than scanning to a
    // limit keeps a pack from making this quadratic.
    for (u32 layer = 0;; ++layer) {
        const auto found = textures.find("layer" + std::to_string(layer));
        if (found == textures.end()) {
            break;
        }
        model->layers.push_back(resolve_sprite(found->second, textures));
    }

    const auto [it, _] = impl_->resolved.insert_or_assign(key, std::move(model));
    return it->second.get();
}

}  // namespace ov::render
