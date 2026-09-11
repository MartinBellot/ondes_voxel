#define OV_LOG_CATEGORY "client"

#include "ov/client/overlay.hpp"

#include "ov/base/log.hpp"

#include <array>
#include <cstring>

namespace ov::client {

namespace {

/// Room for a block outline and a crosshair several times over. A line is two
/// vertices of three floats, so this is 4096 lines a frame — far more than
/// anything here draws, and still only 96 KiB.
constexpr u32 kMaxVertices = 8192;
constexpr u32 kFloatsPerVertex = 3;

}  // namespace

Overlay::~Overlay() {
    if (device_ == nullptr) {
        return;
    }
    for (auto buffer : vertices_) {
        device_->destroy(buffer);
    }
    if (screen_pipeline_.valid()) {
        device_->destroy(screen_pipeline_);
    }
    if (world_pipeline_.valid()) {
        device_->destroy(world_pipeline_);
    }
}

std::expected<std::unique_ptr<Overlay>, rhi::RhiError> Overlay::create(rhi::Device& device,
                                                                      rhi::Format colour_format,
                                                                      rhi::Format depth_format) {
    std::unique_ptr<Overlay> self(new Overlay);
    self->device_ = &device;

    rhi::VertexBinding binding;
    binding.stride = kFloatsPerVertex * sizeof(f32);
    binding.attributes.push_back(rhi::VertexAttribute{0, rhi::Format::Rgb32Float, 0});

    const auto make = [&](bool depth_test, std::string_view name) {
        rhi::GraphicsPipelineDesc pipeline;
        pipeline.vertex_shader             = "overlay.vert.spv";
        pipeline.fragment_shader           = "overlay.frag.spv";
        pipeline.vertex_bindings           = {binding};
        pipeline.layout.push_constant_size = sizeof(Push);
        pipeline.colour_format             = colour_format;
        pipeline.depth_format              = depth_format;
        pipeline.depth_test                = depth_test;
        // Never writes depth. The outline sits on a surface, and writing would
        // let it hide the very face it is drawn around.
        pipeline.depth_write = false;
        pipeline.cull_mode   = rhi::CullMode::None;
        // Blended, because vanilla's outline is a dark line at partial opacity
        // rather than a hard black one.
        pipeline.blend      = rhi::BlendMode::Alpha;
        pipeline.topology   = rhi::PrimitiveTopology::LineList;
        pipeline.debug_name = name;
        return device.create_graphics_pipeline(pipeline);
    };

    auto world = make(true, "overlay world lines");
    if (!world) {
        return std::unexpected(world.error());
    }
    self->world_pipeline_ = *world;

    // The crosshair takes no depth test at all: it is in front of everything by
    // definition, and testing it against the world makes it vanish inside a
    // wall.
    auto screen = make(false, "overlay screen lines");
    if (!screen) {
        return std::unexpected(screen.error());
    }
    self->screen_pipeline_ = *screen;

    const u32 ring = rhi::Device::frames_in_flight();
    for (u32 i = 0; i < ring; ++i) {
        auto buffer = device.create_buffer(
            rhi::BufferDesc{static_cast<usize>(kMaxVertices) * kFloatsPerVertex * sizeof(f32),
                            rhi::BufferUsage::Vertex, "overlay lines", true});
        if (!buffer) {
            return std::unexpected(buffer.error());
        }
        self->vertices_.push_back(*buffer);
        self->used_.push_back(0);
    }
    return self;
}

void Overlay::begin_frame() {
    // The device's own slot, not a counter of ours: it is the one whose fence
    // the device waited on, so the GPU is done reading this buffer. A counter
    // advanced by a draw call stalls whenever that draw is skipped — the HUD
    // hides the line crosshair, and the ring then filled up and stayed full.
    used_[device_->frame_index() % static_cast<u32>(used_.size())] = 0;
}

void Overlay::draw_lines(rhi::CommandList& cmd, std::span<const f32> vertices, const Push& push) {
    if (vertices.empty()) {
        return;
    }
    const u32 slot   = device_->frame_index() % static_cast<u32>(vertices_.size());
    auto*     mapped = static_cast<f32*>(device_->map(vertices_[slot]));
    if (mapped == nullptr) {
        return;
    }
    const u32 first = used_[slot];
    const auto count = static_cast<u32>(vertices.size() / kFloatsPerVertex);
    if (first + count > kMaxVertices) {
        OV_LOG_WARN("overlay line buffer full this frame");
        return;
    }
    std::memcpy(mapped + static_cast<usize>(first) * kFloatsPerVertex, vertices.data(),
                vertices.size() * sizeof(f32));
    used_[slot] = first + count;

    const rhi::PipelineHandle pipeline =
        push.flags[0] > 0.5F ? world_pipeline_ : screen_pipeline_;
    cmd.bind_pipeline(pipeline);
    cmd.push_constants(pipeline, &push, sizeof(push));
    cmd.bind_vertex_buffer(0, vertices_[slot]);
    cmd.draw(count, 1, first, 0);
}

void Overlay::draw_block_outline(rhi::CommandList& cmd, const render::Mat4& view_projection,
                                 Vec3d camera, i32 x, i32 y, i32 z) {
    // Grown by a thousandth. A wireframe drawn exactly on the surface fights it
    // for the depth test and comes out dotted and flickering.
    constexpr f64 kGrow = 0.002;
    const f64     x0    = static_cast<f64>(x) - kGrow;
    const f64     y0    = static_cast<f64>(y) - kGrow;
    const f64     z0    = static_cast<f64>(z) - kGrow;
    const f64     x1    = static_cast<f64>(x) + 1.0 + kGrow;
    const f64     y1    = static_cast<f64>(y) + 1.0 + kGrow;
    const f64     z1    = static_cast<f64>(z) + 1.0 + kGrow;

    // Absolute world coordinates, the same as the terrain uses, so the outline
    // and the face it surrounds are transformed identically and cannot drift
    // apart. f32 costs about eight millimetres of precision at a hundred
    // thousand blocks out and two metres at the world border — the border case
    // wants a camera-relative matrix, and the terrain will want it first.
    (void)camera;
    const auto corner = [](f64 px, f64 py, f64 pz) {
        return std::array<f32, 3>{static_cast<f32>(px), static_cast<f32>(py),
                                  static_cast<f32>(pz)};
    };

    const std::array<std::array<f32, 3>, 8> c{
        corner(x0, y0, z0), corner(x1, y0, z0), corner(x1, y0, z1), corner(x0, y0, z1),
        corner(x0, y1, z0), corner(x1, y1, z0), corner(x1, y1, z1), corner(x0, y1, z1)};
    constexpr std::array<std::pair<u8, u8>, 12> kEdges{
        std::pair<u8, u8>{0, 1}, {1, 2}, {2, 3}, {3, 0},   // bottom
        {4, 5}, {5, 6}, {6, 7}, {7, 4},                     // top
        {0, 4}, {1, 5}, {2, 6}, {3, 7}};                    // uprights

    scratch_.clear();
    for (const auto& [a, b] : kEdges) {
        for (const u8 index : {a, b}) {
            scratch_.push_back(c[index][0]);
            scratch_.push_back(c[index][1]);
            scratch_.push_back(c[index][2]);
        }
    }

    Push push;
    push.view_projection = view_projection;
    push.colour          = {0.0F, 0.0F, 0.0F, 0.4F};
    push.flags           = {1.0F, 0.0F, 0.0F, 0.0F};
    draw_lines(cmd, scratch_, push);
}

void Overlay::draw_shape_outline(rhi::CommandList& cmd, const render::Mat4& view_projection,
                                 std::span<const std::array<Vec3d, 2>> edges) {  // ── breaking ──
    scratch_.clear();
    for (const auto& [from, to] : edges) {
        for (const Vec3d& point : {from, to}) {
            scratch_.push_back(static_cast<f32>(point.x));
            scratch_.push_back(static_cast<f32>(point.y));
            scratch_.push_back(static_cast<f32>(point.z));
        }
    }
    Push push;
    push.view_projection = view_projection;
    push.colour          = {0.0F, 0.0F, 0.0F, 0.4F};
    push.flags           = {1.0F, 0.0F, 0.0F, 0.0F};
    draw_lines(cmd, scratch_, push);
}

void Overlay::draw_crosshair(rhi::CommandList& cmd, u32 width, u32 height) {
    // Nine pixels each way, which is vanilla's. In clip space that is a
    // fraction of the viewport, so it stays the same size on screen whatever
    // the window does.
    constexpr f32 kHalfPixels = 7.0F;
    const f32     dx          = kHalfPixels / static_cast<f32>(width) * 2.0F;
    const f32     dy          = kHalfPixels / static_cast<f32>(height) * 2.0F;

    scratch_.clear();
    const auto point = [&](f32 x, f32 y) {
        scratch_.push_back(x);
        scratch_.push_back(y);
        scratch_.push_back(0.0F);
    };
    point(-dx, 0.0F);
    point(dx, 0.0F);
    point(0.0F, -dy);
    point(0.0F, dy);

    Push push;
    push.view_projection = render::Mat4{};
    // Vanilla inverts the colour underneath so the crosshair is visible on any
    // background. Inversion needs a blend mode this RHI does not have yet, so
    // this is white at three quarters — visible on the terrain, and honestly
    // not the same thing.
    push.colour = {1.0F, 1.0F, 1.0F, 0.75F};
    push.flags  = {0.0F, 0.0F, 0.0F, 0.0F};
    draw_lines(cmd, scratch_, push);
}

}  // namespace ov::client
