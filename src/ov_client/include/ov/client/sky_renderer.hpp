// The sky above the terrain: the sky disc, the twilight band, the sun and the
// moon, in the order the game draws them.
//
// Until this existed the frame was cleared to the fog colour and nothing else,
// so the whole sky was the colour the game only reaches at the horizon. The
// disc is render::sky_disc(): eight triangles fanned from the zenith, as the
// game's own is, because the fog is interpolated per triangle and the fan's
// shape shows in the gradient. The band, sun and moon are placed by
// render::sunrise_fan(), sun_quad() and moon_quad(), whose numbers are the real
// client's (docs/provenance/rendu-parite.md).
#pragma once

#include "ov/base/types.hpp"
#include "ov/render/camera.hpp"
#include "ov/render/environment.hpp"
#include "ov/render/texture_image.hpp"
#include "ov/rhi/device.hpp"

#include <expected>
#include <memory>
#include <optional>
#include <vector>

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
    // ── the sun, the moon and the band ──
    f64                                  celestial{0.0};
    std::optional<render::SunriseColour> sunrise;
    i32                                  moon_phase{0};
    /// 0 clear, 1 full rain: the sun and moon fade with it.
    f32 rain{0.0F};
};

class SkyRenderer {
public:
    /// `sun` and `moon` may be null: a pack without them draws no sun or moon
    /// and says so once.
    [[nodiscard]] static std::expected<std::unique_ptr<SkyRenderer>, rhi::RhiError> create(
        rhi::Device& device, rhi::Format colour_format, rhi::Format depth_format,
        const render::TextureImage* sun = nullptr, const render::TextureImage* moon = nullptr);

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
    struct SimplePush {
        render::Mat4       view_projection;
        std::array<f32, 4> colour;
    };

    SkyRenderer() = default;

    rhi::Device*        device_{nullptr};
    rhi::PipelineHandle pipeline_;
    rhi::BufferHandle   disc_;
    u32                 disc_vertices_{0};

    rhi::PipelineHandle band_pipeline_;
    rhi::PipelineHandle body_pipeline_;
    rhi::SamplerHandle  sampler_;
    rhi::ImageHandle    sun_;
    rhi::ImageHandle    moon_;
    /// One per frame in flight: the band and the two quads, rewritten each
    /// frame from the time of day.
    std::vector<rhi::BufferHandle> dynamic_;
    std::vector<f32>               scratch_;
};

}  // namespace ov::client
