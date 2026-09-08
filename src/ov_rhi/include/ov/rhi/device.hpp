// The device, the swapchain and the command list: everything Vulkan, behind a
// wall.
//
// ov_rhi knows nothing about the game — no block, no chunk, no registry — and
// nothing above it knows anything about Vulkan. The only thing that crosses
// that wall is a window pointer, and the reasoning is in the comment on
// DeviceDesc::native_window.
#pragma once

#include "ov/base/types.hpp"
#include "ov/rhi/types.hpp"

#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ov::rhi {

class Device;

/// Records commands for one frame. Never constructed by the caller: obtained
/// from Device::begin_frame and invalid once the frame ends.
class CommandList {
public:
    CommandList(const CommandList&)            = delete;
    CommandList& operator=(const CommandList&) = delete;

    /// Move a resource to the state the next commands need.
    ///
    /// Mandatory, and never inferred. Missing one is what the validation layers
    /// are for; guessing one is what a state tracker would do, and it would be
    /// right until the frame graph changed.
    void transition(ImageHandle image, ResourceState from, ResourceState to);

    /// The swapchain image of the frame in progress, for the transitions that
    /// bracket a pass drawing to the screen.
    void transition_swapchain(ResourceState from, ResourceState to);

    /// VK_KHR_dynamic_rendering: no VkRenderPass, no VkFramebuffer, no
    /// compatibility rules between a pipeline and a pass it was not compiled
    /// against. The objects a render pass exists to describe are all known one
    /// call before they are used.
    void begin_rendering(std::span<const ColourAttachment> colour, const DepthAttachment* depth,
                         u32 width, u32 height);
    void end_rendering();

    void set_viewport(f32 x, f32 y, f32 width, f32 height);
    void set_scissor(i32 x, i32 y, u32 width, u32 height);

    void bind_pipeline(PipelineHandle pipeline);
    void bind_vertex_buffer(u32 binding, BufferHandle buffer, u64 offset = 0);
    /// Index buffers are always 32-bit here. A section's shared index buffer
    /// addresses more than 65536 vertices and the saving is not worth a second
    /// code path.
    void bind_index_buffer(BufferHandle buffer, u64 offset = 0);
    /// Bind the images a pipeline samples, in binding order, on set 0.
    void bind_textures(PipelineHandle pipeline, std::span<const ImageHandle> images,
                       std::span<const SamplerHandle> samplers);
    /// The same, plus the storage buffers, which take the bindings after the
    /// images. One descriptor set, written and bound in a single call: there
    /// is one of these per layer per frame, so nothing here is worth caching.
    void bind_resources(PipelineHandle pipeline, std::span<const ImageHandle> images,
                        std::span<const SamplerHandle> samplers,
                        std::span<const BufferHandle>  storage_buffers);
    void push_constants(PipelineHandle pipeline, const void* data, u32 size);

    void draw(u32 vertex_count, u32 instance_count = 1, u32 first_vertex = 0,
              u32 first_instance = 0);
    void draw_indexed(u32 index_count, u32 instance_count = 1, u32 first_index = 0,
                      i32 vertex_offset = 0, u32 first_instance = 0);
    /// One call per render layer, with a draw count the CPU already knows.
    ///
    /// Deliberately not drawIndexedIndirectCount: its support across MoltenVK
    /// versions is uneven, and the count is not a secret — the frustum cull
    /// that produced the commands ran on the CPU this frame. See risk R6.
    void draw_indexed_indirect(BufferHandle commands, u64 offset, u32 draw_count, u32 stride);

    void copy_buffer_to_image(BufferHandle source, u64 source_offset, ImageHandle destination,
                              u32 mip_level, u32 width, u32 height);
    /// Copy the frame's colour target into a host-visible buffer.
    ///
    /// Recorded inside the open frame, on purpose. A swapchain image may only
    /// be touched between the acquire and the present that own it, so a
    /// "screenshot" call after end_frame is a validation error and, on a real
    /// driver, a race. The caller brackets this with its own transitions like
    /// any other read: that is the whole point of not having a state tracker.
    void copy_swapchain_to_buffer(BufferHandle destination);
    void copy_buffer(BufferHandle source, u64 source_offset, BufferHandle destination,
                     u64 destination_offset, u64 size);

private:
    friend class Device;

    CommandList() = default;

    /// A VkCommandBuffer, as an integer. Naming the type here would put
    /// vulkan.h in a public header, which is the one thing this module must
    /// never do.
    u64   raw_{0};
    void* device_{nullptr};
};

struct DeviceDesc {
    std::string_view application_name{"Ondes VOXEL"};

    /// A GLFWwindow*, or null for a device with no swapchain.
    ///
    /// This is the seam, and it is worth saying why it is shaped like this.
    /// The client owns the window and the input; the RHI owns Vulkan. Creating
    /// a surface needs both at once. Handing the RHI a VkSurfaceKHR would put
    /// Vulkan types in ov_client; handing it a raw NSWindow would put
    /// Objective-C here. So the client passes its window as an opaque pointer
    /// and ov_rhi links GLFW privately for the one call that turns it into a
    /// surface. Nothing about a window escapes into any header.
    void* native_window{nullptr};

    /// The validation layers, when the SDK provides them. On by default in
    /// debug builds: the barriers in this module are written by hand, and the
    /// layers are the only thing that checks them.
    bool validation{true};

    /// Where the driver's compiled pipelines are cached between runs. Empty
    /// disables it.
    std::string pipeline_cache_path;

    /// Where the SPIR-V compiled at build time lives.
    std::string shader_directory;

    /// FIFO when true, which is the right default: tearing is not a trade to
    /// make by accident.
    ///
    /// Turning it off is not a performance feature, it is a measurement one.
    /// Under FIFO every frame time is quantised to the refresh interval, so a
    /// renderer doing 4 ms of work and one doing 15 both report 16.67 ms and
    /// the p99 the milestone is judged on says nothing at all. The number only
    /// means something when nothing is waiting on the display.
    bool vsync{true};
};

/// What the device turned out to be. Reported rather than assumed, because on
/// macOS there is more than one driver behind the loader and they do not
/// behave the same.
struct DeviceInfo {
    std::string name;
    std::string driver;
    u32         api_major{0};
    u32         api_minor{0};
    bool        validation_enabled{false};
    bool        portability_subset{false};
    /// Whether one indirect call can draw many sections and still tell the
    /// vertex shader which is which. False means the renderer has to fall back
    /// to a draw per section, so it is reported rather than assumed.
    bool indirect_first_instance{false};
};

class Device {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<Device>, RhiError> create(
        const DeviceDesc& desc);

    Device(const Device&)            = delete;
    Device& operator=(const Device&) = delete;
    ~Device();

    [[nodiscard]] const DeviceInfo& info() const noexcept;

    // ── Resources ───────────────────────────────────────────────────────────
    [[nodiscard]] std::expected<BufferHandle, RhiError>   create_buffer(const BufferDesc& desc);
    [[nodiscard]] std::expected<ImageHandle, RhiError>    create_image(const ImageDesc& desc);
    [[nodiscard]] std::expected<SamplerHandle, RhiError>  create_sampler(const SamplerDesc& desc);
    [[nodiscard]] std::expected<PipelineHandle, RhiError> create_graphics_pipeline(
        const GraphicsPipelineDesc& desc);

    void destroy(BufferHandle handle);
    void destroy(ImageHandle handle);
    void destroy(SamplerHandle handle);
    void destroy(PipelineHandle handle);

    /// Host-visible memory of an Upload buffer. Null for anything else.
    [[nodiscard]] void* map(BufferHandle handle);

    /// Copy into an Upload buffer. Returns false if it would not fit.
    [[nodiscard]] bool write_buffer(BufferHandle handle, const void* data, usize size,
                                    usize offset = 0);

    /// Upload pixels through a staging buffer and leave the image in
    /// ShaderRead. Synchronous: for start-up data such as the block atlas, not
    /// for anything in a frame.
    [[nodiscard]] std::expected<void, RhiError> upload_image(ImageHandle         image,
                                                             std::span<const u8> pixels, u32 width,
                                                             u32 height, u32 mip_level = 0);

    /// Upload into a device-local buffer through staging. Same caveat.
    [[nodiscard]] std::expected<void, RhiError> upload_buffer(BufferHandle        buffer,
                                                              std::span<const u8> data,
                                                              u64                 offset = 0);

    // ── Frames ──────────────────────────────────────────────────────────────
    /// Acquire the next swapchain image and open a command list.
    ///
    /// Returns SwapchainOutOfDate when the window changed size, which is a
    /// normal event and not an error: call resize and try the next frame.
    [[nodiscard]] std::expected<CommandList*, RhiError> begin_frame();
    [[nodiscard]] std::expected<void, RhiError>         end_frame();

    [[nodiscard]] std::expected<void, RhiError> resize(u32 width, u32 height);

    [[nodiscard]] u32    swapchain_width() const noexcept;
    [[nodiscard]] u32    swapchain_height() const noexcept;
    [[nodiscard]] Format swapchain_format() const noexcept;

    /// GPU time of the last completed frame, in milliseconds, or 0 before one
    /// has completed. Timestamps are on from the first frame on purpose: a
    /// renderer that gains them later gets them after the decisions they should
    /// have informed.
    [[nodiscard]] f64 last_frame_gpu_ms() const noexcept;

    /// How many frames the device lets the CPU run ahead. Anything the caller
    /// writes from the CPU and the GPU reads in a frame — an indirect command
    /// buffer, above all — needs this many copies, or frame N+1 overwrites
    /// what frame N is still reading.
    [[nodiscard]] static u32 frames_in_flight() noexcept;

    /// Which of those copies the frame in progress is using.
    [[nodiscard]] u32 frame_index() const noexcept;

    /// Block until the device is idle. Only for teardown and resize.
    void wait_idle();

private:
    friend class CommandList;

    struct Impl;

    Device();

    std::unique_ptr<Impl> impl_;
};

}  // namespace ov::rhi
