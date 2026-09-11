// The image the world is drawn into, and the pass that hands it to the screen.
//
// Minecraft 1.20.1 does all of its colour arithmetic on stored 8-bit values:
// a side face is its texture times 0.6, water is blended over the ground as
// numbers, fog is a mix of numbers. Nothing is converted to linear light. Our
// swapchain is sRGB, and a render target in an sRGB format *does* convert: the
// hardware encodes every write and blends in linear light. Drawn straight into
// it, a face shaded at 0.6 came out at 0.6^(1/2.2) = 0.79 of its texture — a
// fifth brighter than the game — and every shadow, every AO corner, every
// dusk and every pane of water with it.
//
// So the world goes into an RGBA8 UNORM image, where every multiply and every
// blend happens on the stored numbers exactly as the game does them, and a
// full-screen pass copies it into the swapchain through the inverse transfer
// function, which the swapchain's encoder undoes byte for byte. The interface
// is drawn afterwards straight into the swapchain, as before.
//
// The captures before and after are in docs/provenance/rendu-parite.md.
#pragma once

#include "ov/base/types.hpp"
#include "ov/rhi/device.hpp"

#include <expected>
#include <memory>

namespace ov::client {

class SceneTarget {
public:
    /// What the world's pipelines render into.
    static constexpr rhi::Format kFormat = rhi::Format::Rgba8Unorm;

    [[nodiscard]] static std::expected<std::unique_ptr<SceneTarget>, rhi::RhiError> create(
        rhi::Device& device, rhi::Format swapchain_format, u32 width, u32 height);

    SceneTarget(const SceneTarget&)            = delete;
    SceneTarget& operator=(const SceneTarget&) = delete;
    ~SceneTarget();

    /// A new image at the new size, after a swapchain resize. The caller has
    /// waited for the device to be idle.
    [[nodiscard]] std::expected<void, rhi::RhiError> resize(u32 width, u32 height);

    [[nodiscard]] rhi::ImageHandle image() const noexcept { return image_; }

    /// Draw the scene into the current pass, which must target the swapchain
    /// at the scene's size. The scene image must be in ShaderRead.
    void present(rhi::CommandList& cmd);

private:
    SceneTarget() = default;

    rhi::Device*        device_{nullptr};
    rhi::ImageHandle    image_;
    rhi::SamplerHandle  sampler_;
    rhi::PipelineHandle pipeline_;
    u32                 width_{0};
    u32                 height_{0};
};

}  // namespace ov::client
