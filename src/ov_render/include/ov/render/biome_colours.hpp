// The colours a biome paints its grass, its leaves and its water.
//
// Three of them work differently, and the difference is the whole reason this
// file exists. Water is stored: the biome JSON names an exact RGB. Grass and
// foliage are not stored for most biomes — they are *sampled from a texture in
// the resource pack*, `colormap/grass.png` and `colormap/foliage.png`, at a
// point given by the biome's temperature and rainfall. A handful of biomes
// override the sample outright (badlands, cherry grove) and two modify it
// afterwards (dark forest, swamp).
//
// That is why the pack ships the climate rather than the colour, and why this
// lives in ov_render: the texture is a client asset, and the server has no
// business owning it.
//
// The whole computation is checkable, which is the point: fifteen biome
// colours are published values, and the unit test reproduces all fifteen
// exactly from the climate and the colormap.
#pragma once

#include "ov/base/types.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/render/asset_source.hpp"

#include <expected>
#include <string_view>
#include <vector>

namespace ov::render {

enum class BiomeColourError : u8 {
    /// The pack has no colormap/grass.png or colormap/foliage.png. Vanilla
    /// ships both; a pack without them is broken rather than minimal, so this
    /// is an error and not a silent fallback to green.
    MissingColormap,
    /// Present but not decodable, or not the 256x256 the sampling assumes.
    BadColormap,
};

[[nodiscard]] std::string_view to_string(BiomeColourError error) noexcept;

/// An RGB triple packed 0xRRGGBB, the form the biome data itself uses.
using Rgb = u32;

class BiomeColours {
public:
    /// Resolve every biome in the registry once, at load.
    ///
    /// 64 biomes times three colours is a table of 768 bytes; sampling a
    /// colormap per block would put a texture read in the mesher's inner loop
    /// to recompute a value that cannot change.
    [[nodiscard]] static std::expected<BiomeColours, BiomeColourError> load(
        const AssetSource& assets, const registry::BlockRegistry& registry);

    [[nodiscard]] Rgb grass(u32 biome) const noexcept;
    [[nodiscard]] Rgb foliage(u32 biome) const noexcept;
    [[nodiscard]] Rgb water(u32 biome) const noexcept;

    [[nodiscard]] usize size() const noexcept { return entries_.size(); }

    /// Sample a colormap the way vanilla does, exposed for the test that
    /// checks it against published values.
    ///
    /// Both axes are inverted and rainfall is scaled by temperature first,
    /// which is what makes the used region of the image a triangle: hot and
    /// wet is a real climate, cold and wet is not.
    /// Double, not float: the index truncates and one biome straddles the
    /// boundary. See the comment on BiomeRecord in the pack format.
    [[nodiscard]] static Rgb sample_colormap(std::span<const u8> rgba, u32 width, f64 temperature,
                                             f64 rainfall) noexcept;

private:
    struct Entry {
        Rgb grass{0x91BD59};
        Rgb foliage{0x77AB2F};
        Rgb water{0x3F76E4};
    };

    std::vector<Entry> entries_;
};

}  // namespace ov::render
