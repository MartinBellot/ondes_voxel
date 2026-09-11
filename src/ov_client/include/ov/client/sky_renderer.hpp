// The sky above the terrain: a disc of sky colour sixteen blocks over the eye,
// fading into the fog colour towards the horizon.
//
// Until this existed the frame was cleared to the fog colour and nothing else,
// so the whole sky was the colour the game only reaches at the horizon. The
// geometry is render::sky_disc(): eight triangles fanned from the zenith, as
// the game's own sky disc is, because the fog is interpolated per triangle and
// the fan's shape is visible in the gradient.
#pragma once

#include "ov/base/types.hpp"
#include "ov/render/camera.hpp"
#include "ov/rhi/device.hpp"

#include <expected>
#include <memory>

namespace ov::client {

struct SkyDraw {
    /// Projection times the camera's rotation, with the eye at the origin.
    render::Mat4 view_projection;
    /// 0xRRGGBB, both already faded for the time of day.
    u32 sky_colour{0x78A7FF};
    u32 fog_colour{0xC0D8FF};
    /// The sky pass's fog, in blocks.
    f32  fog_start{0.0F};
    f32  fog_end{128.0F};
    bool spherical_fog{true};
};

class SkyRenderer {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<SkyRenderer>, rhi::RhiError> create(
        rhi::Device& device, rhi::Format colour_format, rhi::Format depth_format);

    SkyRenderer(const SkyRenderer&)            = delete;
    SkyRenderer& operator=(const SkyRenderer&) = delete;
    ~SkyRenderer();

    /// Recorded first in the world pass: never writes depth, so the terrain
    /// draws over it wherever there is terrain.
    void draw(rhi::CommandList& cmd, const SkyDraw& sky);

private:
    struct Push {
        render::Mat4       view_projection;
        std::array<f32, 4> colour;
        std::array<f32, 4> fog_colour;
        std::array<f32, 4> fog;
    };

    SkyRenderer() = default;

    rhi::Device*        device_{nullptr};
    rhi::PipelineHandle pipeline_;
    rhi::BufferHandle   disc_;
    u32                 disc_vertices_{0};
};

}  // namespace ov::client
