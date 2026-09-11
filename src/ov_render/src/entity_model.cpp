#include "ov/render/entity_model.hpp"

#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <numbers>

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

/// One format-2 quad: `{"n": [x, y, z], "v": [[x, y, z, u, v] × 4]}`.
[[nodiscard]] bool read_quad(const json::Value& value, EntityQuad& out) noexcept {
    if (!value.is_object() || !read_vec3(value["n"], out.normal)) {
        return false;
    }
    const json::Value corners = value["v"];
    if (!corners.is_array() || corners.size() != 4) {
        return false;
    }
    for (u32 corner = 0; corner < 4; ++corner) {
        const json::Value entry = corners[corner];
        if (!entry.is_array() || entry.size() != 5) {
            return false;
        }
        out.position[corner] = Vec3f{static_cast<f32>(entry[0].as_number()),
                                     static_cast<f32>(entry[1].as_number()),
                                     static_cast<f32>(entry[2].as_number())};
        out.u[corner]        = static_cast<f32>(entry[3].as_number());
        out.v[corner]        = static_cast<f32>(entry[4].as_number());
    }
    return true;
}

[[nodiscard]] f32 to_radians(f32 degrees) noexcept {
    return degrees * (std::numbers::pi_v<f32> / 180.0F);
}

/// Rotate `point` about `pivot` by `degrees`, Z·Y·X, as entity_mesh does.
[[nodiscard]] Vec3f rotate_about(Vec3f point, Vec3f pivot, Vec3f degrees) noexcept {
    const f32 cx = std::cos(to_radians(degrees.x));
    const f32 sx = std::sin(to_radians(degrees.x));
    const f32 cy = std::cos(to_radians(degrees.y));
    const f32 sy = std::sin(to_radians(degrees.y));
    const f32 cz = std::cos(to_radians(degrees.z));
    const f32 sz = std::sin(to_radians(degrees.z));
    const Vec3f d = point - pivot;
    // X, then Y, then Z.
    const Vec3f a{d.x, cx * d.y - sx * d.z, sx * d.y + cx * d.z};
    const Vec3f b{cy * a.x + sy * a.z, a.y, -sy * a.x + cy * a.z};
    const Vec3f c{cz * b.x - sz * b.y, sz * b.x + cz * b.y, b.z};
    return pivot + c;
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
    min                = Vec3f{kBig, kBig, kBig};
    max                = Vec3f{-kBig, -kBig, -kBig};

    const auto include = [&](Vec3f point) {
        min.x = std::min(min.x, point.x);
        min.y = std::min(min.y, point.y);
        min.z = std::min(min.z, point.z);
        max.x = std::max(max.x, point.x);
        max.y = std::max(max.y, point.y);
        max.z = std::max(max.z, point.z);
    };

    // A point on a bone is carried through the bone's rest rotation and then
    // every ancestor's: the chain is walked per point because this is a
    // diagnostic, called once per model, never per frame.
    const auto to_model = [&](usize index, Vec3f point) {
        for (i32 bone_index = static_cast<i32>(index); bone_index >= 0;
             bone_index     = bones[static_cast<usize>(bone_index)].parent) {
            const EntityBone& chain = bones[static_cast<usize>(bone_index)];
            point                   = rotate_about(point, chain.pivot, chain.rest_rotation);
        }
        return point;
    };

    bool any = false;
    for (usize index = 0; index < bones.size(); ++index) {
        const EntityBone& entry = bones[index];
        if (!entry.render) {
            continue;
        }
        for (const EntityCube& cube : entry.cubes) {
            const Vec3f low{cube.origin.x - cube.inflate, cube.origin.y - cube.inflate,
                            cube.origin.z - cube.inflate};
            const Vec3f high{cube.origin.x + cube.size.x + cube.inflate,
                             cube.origin.y + cube.size.y + cube.inflate,
                             cube.origin.z + cube.size.z + cube.inflate};
            for (u32 corner = 0; corner < 8; ++corner) {
                include(to_model(index, Vec3f{(corner & 1U) != 0 ? high.x : low.x,
                                              (corner & 2U) != 0 ? high.y : low.y,
                                              (corner & 4U) != 0 ? high.z : low.z}));
            }
            any = true;
        }
        for (const EntityQuad& quad : entry.quads) {
            for (const Vec3f& corner : quad.position) {
                include(to_model(index, corner));
            }
            any = true;
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
    const i32 version = static_cast<i32>(format.as_number());
    if (version != kFormat && version != kLegacyFormat) {
        return std::unexpected(EntityModelError::UnknownFormat);
    }

    EntityModelSet set;
    set.format_ = version;
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
        if (bones.size() > kMaxBones) {
            return std::unexpected(EntityModelError::TooManyBones);
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
            bone.render  = bone_json["render"].as_bool(true);
            bone.visible = bone_json["visible"].as_bool(true);
            if (bone_json["rotation"].valid() && !read_vec3(bone_json["rotation"], bone.rest_rotation)) {
                return std::unexpected(EntityModelError::Malformed);
            }
            if (bone_json["scale"].valid() && !read_vec3(bone_json["scale"], bone.rest_scale)) {
                return std::unexpected(EntityModelError::Malformed);
            }

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
                bone.cubes.push_back(cube);
            }

            const json::Value quads = bone_json["quads"];
            if (quads.valid() && !quads.is_array()) {
                return std::unexpected(EntityModelError::Malformed);
            }
            bone.quads.reserve(quads.size());
            for (u32 quad_index = 0; quad_index < quads.size(); ++quad_index) {
                EntityQuad quad;
                if (!read_quad(quads[quad_index], quad)) {
                    return std::unexpected(EntityModelError::Malformed);
                }
                bone.quads.push_back(quad);
            }

            model.bones.push_back(std::move(bone));
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
