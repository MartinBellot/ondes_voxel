#include "ov/render/texture_image.hpp"

#include "ov/render/asset_path.hpp"
#include "png.hpp"

namespace ov::render {

std::string_view to_string(TextureError error) noexcept {
    switch (error) {
        case TextureError::NotFound:
            return "no such texture in the pack stack";
        case TextureError::Malformed:
            return "the texture is not a PNG this decoder can read";
    }
    return "unknown";
}

std::expected<TextureImage, TextureError> load_texture(const AssetSource&      source,
                                                       const ResourceLocation& location) {
    return load_texture_path(source, texture_asset_path(location));
}

std::expected<TextureImage, TextureError> load_texture_path(const AssetSource& source,
                                                            std::string_view   path) {
    const auto bytes = source.read(path);
    if (!bytes) {
        return std::unexpected(TextureError::NotFound);
    }
    auto decoded = png::decode_rgba8(*bytes);
    if (!decoded) {
        return std::unexpected(TextureError::Malformed);
    }
    TextureImage image;
    image.width  = decoded->width;
    image.height = decoded->height;
    image.rgba   = std::move(decoded->rgba);
    return image;
}

}  // namespace ov::render
