// The two lines the game is played with: the box around the block you are
// aiming at, and the crosshair.
//
// Small, and not optional. Without them a player cannot tell what a click will
// hit, which makes breaking and placing a matter of trial and error rather than
// aim. Vanilla draws both, and both are lines rather than surfaces.
//
// One pipeline, two coordinate spaces. The outline is in world space and goes
// through the view projection; the crosshair is already in clip space and does
// not. A flag in the push constants picks, because two pipelines for two
// multiplications would be two pipelines to keep in step.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/vec.hpp"
#include "ov/render/camera.hpp"
#include "ov/rhi/device.hpp"

#include <expected>
#include <memory>
#include <vector>

namespace ov::client {

class Overlay {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<Overlay>, rhi::RhiError> create(
        rhi::Device& device, rhi::Format colour_format, rhi::Format depth_format);

    Overlay(const Overlay&)            = delete;
    Overlay& operator=(const Overlay&) = delete;
    ~Overlay();

    /// Draw the wireframe of one block. Recorded inside a render pass.
    ///
    /// The box is grown by a thousandth on every side, exactly as vanilla's is:
    /// a wireframe drawn on the surface itself fights the surface for the depth
    /// test and comes out as a dotted line that flickers as the camera moves.
    void draw_block_outline(rhi::CommandList& cmd, const render::Mat4& view_projection,
                            Vec3d camera, i32 x, i32 y, i32 z);

    /// Draw the crosshair, in the middle of the screen.
    void draw_crosshair(rhi::CommandList& cmd, u32 width, u32 height);

private:
    struct Push {
        render::Mat4       view_projection;
        std::array<f32, 4> colour;
        std::array<f32, 4> flags;
    };

    Overlay() = default;

    /// Upload a batch of line endpoints into this frame's slice and draw them.
    void draw_lines(rhi::CommandList& cmd, std::span<const f32> vertices, const Push& push);

    rhi::Device*        device_{nullptr};
    rhi::PipelineHandle world_pipeline_;
    rhi::PipelineHandle screen_pipeline_;
    /// One vertex buffer per frame in flight, written straight from the CPU.
    /// A block outline is twenty-four vertices; staging it through a copy would
    /// cost more than the draw.
    std::vector<rhi::BufferHandle> vertices_;
    std::vector<u32>               used_;
    u32                            ring_{0};
    std::vector<f32>               scratch_;
};

}  // namespace ov::client
