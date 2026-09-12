// Every entity texture on one sheet.
//
// The first entity renderer bound one image per texture and cut a draw per
// image. That held for ten textures. A village is not ten textures: a
// librarian is four (skin, biome, profession, badge), and seven biomes times
// fifteen professions times five levels, plus cats, horses, rabbits and boats,
// is two hundred images — and the descriptor pool of `ov_rhi` holds sixty-four
// sets a frame. So they are packed once at startup into one sheet, and an
// entity pass is one draw whatever the mix of species on screen.
//
// No padding between rectangles, deliberately: the sampler is nearest with no
// mips, so a fragment never reads a texel outside the face it belongs to, and a
// model's nets address their sheet at texel granularity with no border. Pixels
// are copied at the pack's own resolution; a model's coordinates are fractions
// of its logical sheet (entity_mesh.hpp's UvRect), so a 2× pack needs nothing.
#pragma once

#include "ov/base/types.hpp"
#include "ov/render/entity_mesh.hpp"
#include "ov/render/texture_image.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ov::render {

class EntityAtlas {
public:
    /// Pack `images` (name → image) into one sheet no wider than `max_width`.
    /// Rows of equal-ish height, tallest first: a shelf packer, which for a set
    /// of power-of-two skins wastes little and is simple enough to test.
    [[nodiscard]] static EntityAtlas pack(std::vector<std::pair<std::string, TextureImage>> images,
                                          u32 max_width = 4096);

    [[nodiscard]] const TextureImage& image() const noexcept { return image_; }

    /// Where a texture lies on the sheet, or nullopt when it was not packed.
    [[nodiscard]] std::optional<UvRect> find(const std::string& name) const noexcept;

    [[nodiscard]] usize size() const noexcept { return rects_.size(); }

private:
    TextureImage                            image_;
    std::unordered_map<std::string, UvRect> rects_;
};

}  // namespace ov::render
