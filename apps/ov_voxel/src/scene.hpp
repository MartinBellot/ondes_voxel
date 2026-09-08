// A small hand-built world, so the renderer can be exercised before the
// integrated server exists.
//
// This is scaffolding and it says so. It exists because the alternative — wire
// the renderer straight to a real chunk — makes the first picture depend on the
// network, the world storage and the registry all being right at once, and when
// nothing appears there is no way to tell which one is wrong. A scene stated in
// forty lines of C++ has no such ambiguity.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/vec.hpp"
#include "ov/render/mesher.hpp"

#include <array>
#include <string>
#include <vector>

namespace ov::demo {

/// One block type in the demo, and how it is drawn.
struct BlockKind {
    /// Blockstate name, e.g. `minecraft:grass_block`.
    std::string name;
    /// The properties to select a variant with. Empty picks the first.
    std::vector<std::pair<std::string, std::string>> properties;
    render::RenderLayer                              layer{render::RenderLayer::Solid};
    render::TintChannel                              tint{render::TintChannel::None};
    /// Does it hide the faces of its neighbours, and cast ambient occlusion?
    /// A full cube does; glass and leaves do not, which is why you can see
    /// through a stack of them.
    bool solid{true};
    /// Light it emits, 0-15.
    u8 emission{0};
};

inline constexpr i32 kSceneSize   = 16;
inline constexpr i32 kSceneHeight = 16;

/// A cube of block indices into a palette, and the lighting to go with it.
class Scene final : public render::NeighbourhoodView {
public:
    Scene(std::vector<BlockKind> palette);

    void set(i32 x, i32 y, i32 z, u8 kind);

    [[nodiscard]] u8 at(i32 x, i32 y, i32 z) const;

    [[nodiscard]] const std::vector<BlockKind>& palette() const noexcept { return palette_; }

    /// Flood the scene with sky light from above and block light from any
    /// emitter, so that ambient occlusion has something to darken.
    ///
    /// Not vanilla's propagation — that lives in the server and arrives over
    /// the wire. This is enough to tell a lit face from a shadowed one, which
    /// is what the mesher is being tested on.
    void relight();

    // ── NeighbourhoodView ───────────────────────────────────────────────────
    [[nodiscard]] bool occludes(Vec3i position, Direction towards) const override;
    [[nodiscard]] bool casts_ambient_occlusion(Vec3i position) const override;
    [[nodiscard]] u8   sky_light(Vec3i position) const override;
    [[nodiscard]] u8   block_light(Vec3i position) const override;

private:
    [[nodiscard]] static bool inside(i32 x, i32 y, i32 z);

    [[nodiscard]] static usize index(i32 x, i32 y, i32 z);

    std::vector<BlockKind> palette_;
    std::vector<u8>        blocks_;
    std::vector<u8>        sky_;
    std::vector<u8>        block_;
};

/// The demo layout: a platform, a few trees and a pane of glass.
[[nodiscard]] Scene build_demo_scene();

}  // namespace ov::demo
