// The vocabulary of the RHI: formats, states, and the descriptions used to
// create things. No Vulkan type appears here, or in any other public header of
// this module — that is what keeps a one-line edit from rebuilding the world on
// an 8 GB machine (risk R5), and what stops the rest of the codebase quietly
// growing a dependency on one graphics API.
#pragma once

#include "ov/base/types.hpp"
#include "ov/rhi/handle.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ov::rhi {

struct BufferTag {};

struct ImageTag {};

struct SamplerTag {};

struct PipelineTag {};

using BufferHandle   = Handle<BufferTag>;
using ImageHandle    = Handle<ImageTag>;
using SamplerHandle  = Handle<SamplerTag>;
using PipelineHandle = Handle<PipelineTag>;

enum class RhiError {
    /// No Vulkan loader, or no driver behind it.
    NoVulkan,
    /// A loader is present but exposes no device we can use.
    NoDevice,
    /// The window system refused a surface.
    NoSurface,
    /// Out of device or host memory.
    OutOfMemory,
    /// A shader, pipeline cache or other file would not load.
    FileNotFound,
    /// Shader bytecode the driver rejected.
    BadShader,
    /// The swapchain is out of date and the caller should resize and retry.
    SwapchainOutOfDate,
    /// Anything the driver reported that we cannot act on specifically.
    DeviceLost,
    InvalidArgument,
};

[[nodiscard]] std::string_view to_string(RhiError error) noexcept;

enum class Format : u8 {
    Undefined,
    R8Unorm,
    Rgba8Unorm,
    Rgba8Srgb,
    Bgra8Unorm,
    Bgra8Srgb,
    R16Uint,
    R32Uint,
    Rg32Float,
    Rgb32Float,
    Rgba32Float,
    Depth32Float,
    Depth24UnormStencil8,
};

[[nodiscard]] bool is_depth_format(Format format) noexcept;

[[nodiscard]] u32 format_size(Format format) noexcept;

/// What a resource is being used for right now. The RHI does NOT track this:
/// every transition is written out by the caller.
///
/// An automatic state tracker is the wrong trade for a renderer whose frame
/// graph is known at compile time. It costs a hash lookup per binding, it
/// serialises work the author knew was independent, and — worst — it hides the
/// barrier so that when it is wrong the symptom is a flicker on one driver.
/// Explicit transitions are more typing and the validation layers check them.
enum class ResourceState : u8 {
    Undefined,
    ColourAttachment,
    DepthAttachment,
    ShaderRead,
    TransferSource,
    TransferDest,
    Present,
};

enum class BufferUsage : u8 {
    /// Host-visible, written every frame: staging, uniforms.
    Upload,
    /// Device-local vertex data.
    Vertex,
    Index,
    Uniform,
    Storage,
    Indirect,
};

struct BufferDesc {
    usize       size{0};
    BufferUsage usage{BufferUsage::Upload};
    /// Shown in captures and validation messages. Worth the bytes.
    std::string_view debug_name;
};

struct ImageDesc {
    u32              width{1};
    u32              height{1};
    u32              mip_levels{1};
    Format           format{Format::Rgba8Unorm};
    bool             sampled{true};
    bool             render_target{false};
    std::string_view debug_name;
};

enum class Filter : u8 { Nearest, Linear };

enum class MipFilter : u8 { Nearest, Linear };

enum class AddressMode : u8 { Repeat, ClampToEdge };

struct SamplerDesc {
    /// Nearest by default, and that is not laziness: Minecraft's look depends
    /// on unfiltered texels. Linear magnification turns a 16x pack into mush.
    Filter      min_filter{Filter::Nearest};
    Filter      mag_filter{Filter::Nearest};
    MipFilter   mip_filter{MipFilter::Linear};
    AddressMode address_mode{AddressMode::ClampToEdge};
    f32         max_anisotropy{1.0F};
    f32         max_lod{16.0F};
};

enum class CompareOp : u8 { Never, Less, LessOrEqual, Greater, GreaterOrEqual, Always };

enum class CullMode : u8 { None, Back, Front };

enum class BlendMode : u8 {
    /// Opaque. The terrain's solid and cutout layers.
    None,
    /// Source alpha over destination. The translucent layer.
    Alpha,
};

enum class VertexInputRate : u8 { Vertex, Instance };

struct VertexAttribute {
    u32    location{0};
    Format format{Format::Rgba32Float};
    u32    offset{0};
};

struct VertexBinding {
    u32                          stride{0};
    VertexInputRate              input_rate{VertexInputRate::Vertex};
    std::vector<VertexAttribute> attributes;
};

/// What a pipeline reads. Deliberately tiny.
///
/// Bindless is *prepared* but not implemented: texture handles are u32 in
/// material data from day one, wired onto one fixed descriptor set. MoltenVK
/// routes descriptorIndexing through Metal argument buffers and has a history
/// of doing it badly, and a chunk renderer has exactly one atlas — so the
/// complexity buys nothing today and the u32 keeps the door open. See risk R6.
struct PipelineLayoutDesc {
    /// Combined image samplers, in binding order, on set 0.
    u32 sampled_image_count{0};
    /// Push constant bytes. The whole per-draw state fits here for now.
    u32 push_constant_size{0};
};

struct GraphicsPipelineDesc {
    /// Paths to SPIR-V compiled offline by glslc at build time. There is no
    /// shader compiler in this process: shaderc at runtime costs 40 MB of
    /// binary and a second of startup to solve a problem the build already has.
    std::string_view vertex_shader;
    std::string_view fragment_shader;

    std::vector<VertexBinding> vertex_bindings;
    PipelineLayoutDesc         layout;

    Format    colour_format{Format::Bgra8Srgb};
    Format    depth_format{Format::Depth32Float};
    bool      depth_test{true};
    bool      depth_write{true};
    CompareOp depth_compare{CompareOp::Less};
    CullMode  cull_mode{CullMode::Back};
    BlendMode blend{BlendMode::None};

    std::string_view debug_name;
};

/// One colour attachment of a dynamic rendering pass.
struct ColourAttachment {
    /// An invalid handle means "the swapchain image for this frame".
    ImageHandle image;
    bool        clear{true};
    f32         clear_colour[4]{0.0F, 0.0F, 0.0F, 1.0F};
};

struct DepthAttachment {
    ImageHandle image;
    bool        clear{true};
    f32         clear_depth{1.0F};
};

}  // namespace ov::rhi
