#include "ov/render/asset_path.hpp"

#include <string_view>

namespace ov::render {

namespace {

std::string asset_path(const ResourceLocation& location, std::string_view category,
                       std::string_view extension) {
    const auto name_space = location.name_space();
    const auto path       = location.path();

    std::string out;
    out.reserve(7 + name_space.size() + 1 + category.size() + 1 + path.size() + extension.size());
    out.append("assets/").append(name_space).push_back('/');
    out.append(category).push_back('/');
    out.append(path).append(extension);
    return out;
}

}  // namespace

std::string model_asset_path(const ResourceLocation& location) {
    return asset_path(location, "models", ".json");
}

std::string blockstate_asset_path(const ResourceLocation& location) {
    return asset_path(location, "blockstates", ".json");
}

std::string texture_asset_path(const ResourceLocation& location) {
    return asset_path(location, "textures", ".png");
}

bool is_builtin_model(const ResourceLocation& location) noexcept {
    return location.path().starts_with("builtin/");
}

}  // namespace ov::render
