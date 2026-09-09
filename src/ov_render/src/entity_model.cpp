#include "ov/render/entity_model.hpp"

#include "json.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <limits>

namespace ov::render {

namespace {

[[nodiscard]] bool read_vec3(const json::Value& value, Vec3f& out) noexcept {
    if (!value.is_array() || value.size() != 3) {
        return false;
    }
    out.x = static_cast<f32>(value[0].as_number());
    out.y = static_cast<f32>(value[1].as_number());
    out.z = static_cast<f32>(value[2].as_number());
    return true;
}

}  // namespace

std::string_view to_string(EntityModelError error) noexcept {
    switch (error) {
        case EntityModelError::Malformed:
            return "malformed entity model file";
        case EntityModelError::BadHierarchy:
            return "a bone names a parent that is not declared before it";
        case EntityModelError::UnknownFormat:
            return "entity model file format version this build does not read";
        case EntityModelError::NotFound:
            return "entity model file not found";
        case EntityModelError::TooManyBones:
            return "a model has more bones than this build can pose";
    }
    return "unknown";
}

i32 EntityModel::bone(std::string_view bone_name) const noexcept {
    for (usize index = 0; index < bones.size(); ++index) {
        if (bones[index].name == bone_name) {
            return static_cast<i32>(index);
        }
    }
    return -1;
}

void EntityModel::rest_bounds(Vec3f& min, Vec3f& max) const noexcept {
    constexpr f32 kBig = std::numeric_limits<f32>::max();
    min               = Vec3f{kBig, kBig, kBig};
    max               = Vec3f{-kBig, -kBig, -kBig};

    bool any = false;
    for (const EntityBone& bone_entry : bones) {
        if (!bone_entry.render) {
            continue;
        }
        for (const EntityCube& cube : bone_entry.cubes) {
            const Vec3f low{cube.origin.x - cube.inflate, cube.origin.y - cube.inflate,
                            cube.origin.z - cube.inflate};
            const Vec3f high{cube.origin.x + cube.size.x + cube.inflate,
                             cube.origin.y + cube.size.y + cube.inflate,
                             cube.origin.z + cube.size.z + cube.inflate};
            min.x = std::min(min.x, low.x);
            min.y = std::min(min.y, low.y);
            min.z = std::min(min.z, low.z);
            max.x = std::max(max.x, high.x);
            max.y = std::max(max.y, high.y);
            max.z = std::max(max.z, high.z);
            any   = true;
        }
    }
    if (!any) {
        min = Vec3f{};
        max = Vec3f{};
        return;
    }
    // Model units are sixteenths of a block, and every caller of this wants
    // blocks: the number it is compared against is a collision box.
    constexpr f32 kPerBlock = 1.0F / 16.0F;
    min                     = min * kPerBlock;
    max                     = max * kPerBlock;
}

std::expected<EntityModelSet, EntityModelError> EntityModelSet::parse(std::span<const u8> bytes) {
    auto document = json::Document::parse(bytes);
    if (!document) {
        return std::unexpected(EntityModelError::Malformed);
    }

    const json::Value root = document->root();
    if (!root.is_object()) {
        return std::unexpected(EntityModelError::Malformed);
    }
    const json::Value format = root["format"];
    if (!format.is_number()) {
        return std::unexpected(EntityModelError::Malformed);
    }
    if (static_cast<i32>(format.as_number()) != kFormat) {
        return std::unexpected(EntityModelError::UnknownFormat);
    }

    EntityModelSet set;
    set.source_ = std::string(root["source"].as_string());

    const json::Value refused = root["refused"];
    for (u32 index = 0; index < refused.size(); ++index) {
        set.refused_.emplace_back(refused[index].as_string());
    }

    const json::Value models = root["models"];
    if (!models.is_object()) {
        return std::unexpected(EntityModelError::Malformed);
    }

    set.models_.reserve(models.size());
    for (u32 index = 0; index < models.size(); ++index) {
        const json::Value entry = models.value_at(index);
        if (!entry.is_object()) {
            return std::unexpected(EntityModelError::Malformed);
        }

        EntityModel model;
        model.name = std::string(models.key_at(index));

        const json::Value texture_width  = entry["texture_width"];
        const json::Value texture_height = entry["texture_height"];
        if (!texture_width.is_number() || !texture_height.is_number()) {
            return std::unexpected(EntityModelError::Malformed);
        }
        model.texture_width  = static_cast<f32>(texture_width.as_number());
        model.texture_height = static_cast<f32>(texture_height.as_number());
        if (model.texture_width <= 0.0F || model.texture_height <= 0.0F) {
            return std::unexpected(EntityModelError::Malformed);
        }

        const json::Value bones = entry["bones"];
        if (!bones.is_array()) {
            return std::unexpected(EntityModelError::Malformed);
        }
        model.bones.reserve(bones.size());

        for (u32 bone_index = 0; bone_index < bones.size(); ++bone_index) {
            const json::Value bone_json = bones[bone_index];
            if (!bone_json.is_object()) {
                return std::unexpected(EntityModelError::Malformed);
            }

            EntityBone bone;
            bone.name = std::string(bone_json["name"].as_string());
            if (bone.name.empty()) {
                return std::unexpected(EntityModelError::Malformed);
            }
            if (!read_vec3(bone_json["pivot"], bone.pivot)) {
                return std::unexpected(EntityModelError::Malformed);
            }
            bone.render = bone_json["render"].as_bool(true);

            const std::string_view parent = bone_json["parent"].as_string();
            if (!parent.empty()) {
                // Parent-before-child is not a convention here, it is the
                // invariant that makes posing one forward pass with no
                // recursion and no visited set. The generator sorts for it and
                // this refuses the file if it did not.
                bone.parent = model.bone(parent);
                if (bone.parent < 0) {
                    return std::unexpected(EntityModelError::BadHierarchy);
                }
            }

            const json::Value cubes = bone_json["cubes"];
            if (cubes.valid() && !cubes.is_array()) {
                return std::unexpected(EntityModelError::Malformed);
            }
            bone.cubes.reserve(cubes.size());
            for (u32 cube_index = 0; cube_index < cubes.size(); ++cube_index) {
                const json::Value cube_json = cubes[cube_index];
                EntityCube        cube;
                if (!read_vec3(cube_json["origin"], cube.origin) ||
                    !read_vec3(cube_json["size"], cube.size)) {
                    return std::unexpected(EntityModelError::Malformed);
                }
                if (cube.size.x < 0.0F || cube.size.y < 0.0F || cube.size.z < 0.0F) {
                    return std::unexpected(EntityModelError::Malformed);
                }
                const json::Value uv = cube_json["uv"];
                if (!uv.is_array() || uv.size() != 2) {
                    return std::unexpected(EntityModelError::Malformed);
                }
                cube.uv_u    = static_cast<f32>(uv[0].as_number());
                cube.uv_v    = static_cast<f32>(uv[1].as_number());
                cube.inflate = static_cast<f32>(cube_json["inflate"].as_number(0.0));
                cube.mirror  = cube_json["mirror"].as_bool(false);
                bone.cubes.push_back(std::move(cube));
            }

            model.bones.push_back(std::move(bone));
        }
        if (model.bones.size() > kMaxBones) {
            return std::unexpected(EntityModelError::TooManyBones);
        }

        set.models_.push_back(std::move(model));
    }

    return set;
}

std::expected<EntityModelSet, EntityModelError> EntityModelSet::load(
    const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::unexpected(EntityModelError::NotFound);
    }
    const std::vector<u8> bytes((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
    return parse(bytes);
}

const EntityModel* EntityModelSet::find(std::string_view name) const noexcept {
    for (const EntityModel& model : models_) {
        if (model.name == name) {
            return &model;
        }
    }
    return nullptr;
}

}  // namespace ov::render
