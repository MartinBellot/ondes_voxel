#include "ov/render/entity_atlas.hpp"

#include <algorithm>
#include <cstring>

namespace ov::render {

namespace {

[[nodiscard]] u32 next_power_of_two(u32 value) noexcept {
    u32 result = 1;
    while (result < value) {
        result <<= 1U;
    }
    return result;
}

}  // namespace

EntityAtlas EntityAtlas::pack(std::vector<std::pair<std::string, TextureImage>> images,
                              u32 max_width) {
    EntityAtlas atlas;
    std::erase_if(images, [](const auto& entry) { return entry.second.empty(); });
    if (images.empty()) {
        return atlas;
    }
    std::stable_sort(images.begin(), images.end(), [](const auto& a, const auto& b) {
        return a.second.height > b.second.height;
    });

    u32 widest = 0;
    for (const auto& [name, image] : images) {
        widest = std::max(widest, image.width);
    }

    // Place first, then allocate: the height is known only at the end. The
    // width starts at 1024 and doubles while the sheet would come out more than
    // twice as tall as it is wide — a long thin sheet is legal, but a squarer
    // one stays well inside every device's image limit.
    struct Placement {
        u32 x;
        u32 y;
    };
    std::vector<Placement> placements;
    u32                    width  = std::max(next_power_of_two(widest), std::min(max_width, 1024U));
    u32                    height = 0;
    for (;;) {
        placements.clear();
        u32 x         = 0;
        u32 y         = 0;
        u32 row_limit = 0;
        for (const auto& [name, image] : images) {
            if (x + image.width > width) {
                x = 0;
                y += row_limit;
                row_limit = 0;
            }
            placements.push_back(Placement{x, y});
            x += image.width;
            row_limit = std::max(row_limit, image.height);
        }
        height = next_power_of_two(y + row_limit);
        if (height <= width * 2 || width * 2 > max_width) {
            break;
        }
        width *= 2;
    }

    atlas.image_.width  = width;
    atlas.image_.height = height;
    atlas.image_.rgba.assign(static_cast<usize>(width) * height * 4, 0);

    for (usize index = 0; index < images.size(); ++index) {
        const TextureImage& image = images[index].second;
        const Placement&    at    = placements[index];
        for (u32 row = 0; row < image.height; ++row) {
            std::memcpy(&atlas.image_.rgba[atlas.image_.index(at.x, at.y + row)],
                        &image.rgba[image.index(0, row)], static_cast<usize>(image.width) * 4);
        }
        atlas.rects_[images[index].first] =
            UvRect{static_cast<f32>(at.x) / static_cast<f32>(width),
                   static_cast<f32>(at.y) / static_cast<f32>(height),
                   static_cast<f32>(at.x + image.width) / static_cast<f32>(width),
                   static_cast<f32>(at.y + image.height) / static_cast<f32>(height)};
    }
    return atlas;
}

std::optional<UvRect> EntityAtlas::find(const std::string& name) const noexcept {
    const auto found = rects_.find(name);
    if (found == rects_.end()) {
        return std::nullopt;
    }
    return found->second;
}

}  // namespace ov::render
