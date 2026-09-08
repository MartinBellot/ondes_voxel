// ov_voxel — the playable client.
//
// Today it draws a hand-built scene out of real 1.20.1 block models and a real
// stitched atlas, which is the first point at which the model pipeline, the
// atlas, the mesher and the RHI are all checked at once by something that is
// not a unit test.
//
// --frames and --screenshot exist so the result can be checked without a human
// looking at it, which is the difference between "it built" and "it works".

#define OV_LOG_CATEGORY "voxel"

#include "scene.hpp"

#include "ov/base/log.hpp"
#include "ov/client/window.hpp"
#include "ov/render/atlas.hpp"
#include "ov/render/baked_model.hpp"
#include "ov/render/block_state_model.hpp"
#include "ov/render/camera.hpp"
#include "ov/render/mesher.hpp"
#include "ov/render/model.hpp"
#include "ov/rhi/device.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace ov;

namespace {

struct Options {
    u32 width{1280};
    u32 height{720};
    /// Zero means "until the window is closed".
    u32         frames{0};
    std::string screenshot;
    std::string assets{"run/assets"};
    bool        validation{true};
    /// x,y,z,yaw,pitch. Exists so that a face can be put in front of the
    /// camera and looked at, which is how the questions a unit test cannot
    /// answer — is this texture mirrored? — actually get settled.
    std::string camera;
};

[[nodiscard]] Options parse_arguments(std::span<char*> args) {
    Options options;
    for (usize i = 1; i < args.size(); ++i) {
        const std::string argument(args[i]);
        const auto        value = [&argument](std::string_view prefix) {
            return argument.substr(prefix.size());
        };

        if (argument.starts_with("--frames=")) {
            options.frames = static_cast<u32>(std::atoi(value("--frames=").c_str()));
        } else if (argument.starts_with("--width=")) {
            options.width = static_cast<u32>(std::atoi(value("--width=").c_str()));
        } else if (argument.starts_with("--height=")) {
            options.height = static_cast<u32>(std::atoi(value("--height=").c_str()));
        } else if (argument.starts_with("--screenshot=")) {
            options.screenshot = value("--screenshot=");
        } else if (argument.starts_with("--assets=")) {
            options.assets = value("--assets=");
        } else if (argument.starts_with("--camera=")) {
            options.camera = value("--camera=");
        } else if (argument == "--no-validation") {
            options.validation = false;
        }
    }
    return options;
}

/// Where the build put the SPIR-V and the pipeline cache: next to the
/// executable, not relative to the working directory, so running from anywhere
/// works.
[[nodiscard]] std::filesystem::path executable_directory(const char* argv0) {
    std::error_code error;
    auto            path = std::filesystem::weakly_canonical(std::filesystem::path(argv0), error);
    if (error) {
        return std::filesystem::current_path();
    }
    return path.parent_path();
}

/// PPM, because it needs no library and `head -2` says whether it is right.
/// Golden images belong on Linux with lavapipe — MoltenVK is not a conformance
/// oracle.
bool write_ppm(const std::filesystem::path& path, std::span<const u8> rgba, u32 width, u32 height) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    file << "P6\n" << width << ' ' << height << "\n255\n";
    for (usize i = 0; i + 3 < rgba.size(); i += 4) {
        file.put(static_cast<char>(rgba[i]));
        file.put(static_cast<char>(rgba[i + 1]));
        file.put(static_cast<char>(rgba[i + 2]));
    }
    return static_cast<bool>(file);
}

/// One block kind, resolved from the pack: its blockstate, its model, and the
/// quads it bakes to.
struct ResolvedKind {
    render::BakedModel      baked;
    render::BlockRenderInfo info;
};

[[nodiscard]] std::vector<ResolvedKind> resolve_palette(
    const render::AssetSource& source, render::ModelLoader& loader,
    const std::vector<demo::BlockKind>& palette) {
    std::vector<ResolvedKind> resolved(palette.size());

    for (usize i = 0; i < palette.size(); ++i) {
        const auto& kind = palette[i];
        resolved[i].info = render::BlockRenderInfo{kind.layer, kind.tint};
        if (kind.name == "minecraft:air") {
            continue;
        }

        const auto location = ResourceLocation::parse(kind.name);
        if (!location) {
            OV_LOG_WARN("{}: not a resource location", kind.name);
            continue;
        }
        const auto bytes = source.read(render::blockstate_asset_path(*location));
        if (!bytes) {
            OV_LOG_WARN("{}: no blockstate file", kind.name);
            continue;
        }
        const auto file = render::BlockStateFile::parse(*bytes);
        if (!file) {
            OV_LOG_WARN("{}: {}", kind.name, render::to_string(file.error()));
            continue;
        }

        std::vector<std::pair<std::string_view, std::string_view>> properties;
        properties.reserve(kind.properties.size());
        for (const auto& entry : kind.properties) {
            properties.emplace_back(entry.first, entry.second);
        }

        const auto groups = file->select(properties);
        if (groups.empty() || groups.front().alternatives.empty()) {
            OV_LOG_WARN("{}: no variant matched", kind.name);
            continue;
        }

        // The first alternative. Choosing among weighted ones needs the block
        // position hashed the way vanilla does it, which the mesher will own.
        const auto& variant = groups.front().alternatives.front();
        const auto  model   = loader.load(variant.model);
        if (!model) {
            OV_LOG_WARN("{}: {}", kind.name, render::to_string(model.error()));
            continue;
        }
        resolved[i].baked = render::bake(**model, variant);
    }
    return resolved;
}

struct PushConstants {
    render::Mat4       view_projection;
    std::array<f32, 4> section_origin;
};

[[nodiscard]] std::span<const u8> as_bytes(const auto& container) {
    return std::span(
        reinterpret_cast<const u8*>(container.data()),
        container.size() * sizeof(typename std::decay_t<decltype(container)>::value_type));
}

}  // namespace

int main(int argc, char** argv) {
    const std::span<char*> args(argv, static_cast<usize>(argc));
    const Options          options = parse_arguments(args);
    const auto             base    = executable_directory(argv[0]);

    // ── Assets, models, atlas, mesh: all before any Vulkan ──────────────────
    const std::filesystem::path assets_root(options.assets);
    if (!std::filesystem::is_directory(assets_root / "assets")) {
        OV_LOG_ERROR(
            "{} has no assets/ directory. Run ov_assetimport first, or pass "
            "--assets=<path>.",
            assets_root.string());
        return 1;
    }

    const render::DirectoryAssetSource source(assets_root);
    render::ModelLoader                loader(source);

    demo::Scene scene    = demo::build_demo_scene();
    const auto  resolved = resolve_palette(source, loader, scene.palette());

    render::AtlasBuilder builder(source);
    for (const auto& kind : resolved) {
        for (const auto& quad : kind.baked.quads) {
            builder.add(quad.sprite);
        }
    }
    auto atlas = builder.build();
    if (!atlas) {
        OV_LOG_ERROR("atlas: {}", render::to_string(atlas.error()));
        return 1;
    }
    OV_LOG_INFO("atlas {}x{}, {} sprites, {} mip levels", atlas->width(), atlas->height(),
                atlas->sprites().size(), atlas->mips().size());

    render::MeshBuffers mesh;
    for (i32 y = 0; y < demo::kSceneHeight; ++y) {
        for (i32 z = 0; z < demo::kSceneSize; ++z) {
            for (i32 x = 0; x < demo::kSceneSize; ++x) {
                const u8 kind = scene.at(x, y, z);
                if (kind == 0 || resolved[kind].baked.quads.empty()) {
                    continue;
                }
                render::emit_block(resolved[kind].baked, Vec3i{x, y, z}, resolved[kind].info,
                                   *atlas, scene, mesh);
            }
        }
    }
    OV_LOG_INFO("mesh: {} vertices, {} quads", mesh.total_vertices(), mesh.total_vertices() / 4);

    // ── Window and device ───────────────────────────────────────────────────
    auto window = client::Window::create(options.width, options.height, "Ondes VOXEL");
    if (!window) {
        OV_LOG_ERROR("{}", client::to_string(window.error()));
        return 1;
    }

    rhi::DeviceDesc desc;
    desc.application_name    = "Ondes VOXEL";
    desc.native_window       = (*window)->native_handle();
    desc.validation          = options.validation;
    desc.shader_directory    = (base / "shaders").string();
    desc.pipeline_cache_path = (base / "cache" / "pipelines.bin").string();

    auto device_result = rhi::Device::create(desc);
    if (!device_result) {
        OV_LOG_ERROR("no graphics device: {}", rhi::to_string(device_result.error()));
        return 1;
    }
    rhi::Device& device = **device_result;
    OV_LOG_INFO("device: {} ({})", device.info().name, device.info().driver);

    // ── GPU resources ───────────────────────────────────────────────────────
    auto atlas_image = device.create_image(
        rhi::ImageDesc{atlas->width(), atlas->height(), static_cast<u32>(atlas->mips().size()),
                       rhi::Format::Rgba8Srgb, true, false, "block atlas"});
    if (!atlas_image) {
        OV_LOG_ERROR("atlas image: {}", rhi::to_string(atlas_image.error()));
        return 1;
    }
    for (u32 level = 0; level < atlas->mips().size(); ++level) {
        const auto& mip = atlas->mip(level);
        if (!device.upload_image(*atlas_image, mip.rgba, mip.width, mip.height, level)) {
            OV_LOG_ERROR("atlas upload failed at mip {}", level);
            return 1;
        }
    }

    // Nearest magnification is not a preference: Minecraft's look depends on
    // unfiltered texels, and linear turns a 16x pack into mush. Minification
    // walks the mip chain, which is what the chain is for.
    auto sampler = device.create_sampler(rhi::SamplerDesc{
        rhi::Filter::Nearest, rhi::Filter::Nearest, rhi::MipFilter::Linear,
        rhi::AddressMode::ClampToEdge, 1.0F, static_cast<f32>(atlas->mips().size())});
    if (!sampler) {
        return 1;
    }

    // One vertex buffer per layer, and one index buffer shared by all of them.
    // The shared indices are the plan's first memory win: at six 32-bit indices
    // a quad, every section that does not store its own saves 24 bytes a quad.
    usize max_quads = 0;
    for (const auto& layer : mesh.layers) {
        max_quads = std::max(max_quads, layer.size() / 4);
    }
    const auto indices = render::build_shared_quad_indices(static_cast<u32>(max_quads));

    constexpr usize kLayerCount = static_cast<usize>(render::RenderLayer::Count);
    std::array<rhi::BufferHandle, kLayerCount> vertex_buffers{};
    std::array<u32, kLayerCount>               index_counts{};

    for (usize i = 0; i < kLayerCount; ++i) {
        const auto& vertices = mesh.layers[i];
        if (vertices.empty()) {
            continue;
        }
        const auto bytes  = as_bytes(vertices);
        auto       buffer = device.create_buffer(
            rhi::BufferDesc{bytes.size(), rhi::BufferUsage::Vertex, "terrain vertices"});
        if (!buffer) {
            return 1;
        }
        if (!device.upload_buffer(*buffer, bytes)) {
            return 1;
        }
        vertex_buffers[i] = *buffer;
        index_counts[i]   = static_cast<u32>(vertices.size() / 4 * 6);
    }

    rhi::BufferHandle index_buffer;
    if (!indices.empty()) {
        const auto bytes  = as_bytes(indices);
        auto       buffer = device.create_buffer(
            rhi::BufferDesc{bytes.size(), rhi::BufferUsage::Index, "shared quad indices"});
        if (!buffer) {
            return 1;
        }
        if (!device.upload_buffer(*buffer, bytes)) {
            return 1;
        }
        index_buffer = *buffer;
    }

    // Three uint attributes rather than one uvec3, because R32G32B32_UINT is
    // not universally supported for vertex input; reading three uints as a
    // uvec3 in the shader costs nothing and works everywhere.
    rhi::VertexBinding binding;
    binding.stride = sizeof(render::TerrainVertex);
    binding.attributes.push_back(rhi::VertexAttribute{0, rhi::Format::R32Uint, 0});
    binding.attributes.push_back(rhi::VertexAttribute{1, rhi::Format::R32Uint, 4});
    binding.attributes.push_back(rhi::VertexAttribute{2, rhi::Format::R32Uint, 8});

    const auto make_pipeline = [&](rhi::BlendMode blend, bool depth_write, rhi::CullMode cull) {
        rhi::GraphicsPipelineDesc pipeline;
        pipeline.vertex_shader              = "terrain.vert.spv";
        pipeline.fragment_shader            = "terrain.frag.spv";
        pipeline.vertex_bindings            = {binding};
        pipeline.layout.sampled_image_count = 1;
        pipeline.layout.push_constant_size  = sizeof(PushConstants);
        pipeline.colour_format              = device.swapchain_format();
        pipeline.depth_format               = rhi::Format::Depth32Float;
        pipeline.depth_test                 = true;
        pipeline.depth_write                = depth_write;
        pipeline.cull_mode                  = cull;
        pipeline.blend                      = blend;
        pipeline.debug_name                 = "terrain";
        return device.create_graphics_pipeline(pipeline);
    };

    auto opaque_pipeline = make_pipeline(rhi::BlendMode::None, true, rhi::CullMode::Back);
    if (!opaque_pipeline) {
        OV_LOG_ERROR("pipeline: {}", rhi::to_string(opaque_pipeline.error()));
        return 1;
    }
    // Cutout models are not closed solids — a leaf block's faces are all
    // visible from both sides — so back-face culling would eat half of them.
    auto cutout_pipeline = make_pipeline(rhi::BlendMode::None, true, rhi::CullMode::None);
    if (!cutout_pipeline) {
        return 1;
    }

    rhi::ImageHandle depth_image;
    u32              depth_width  = 0;
    u32              depth_height = 0;
    const auto       ensure_depth = [&]() {
        const u32 width  = device.swapchain_width();
        const u32 height = device.swapchain_height();
        if (depth_image.valid() && width == depth_width && height == depth_height) {
            return true;
        }
        if (depth_image.valid()) {
            device.wait_idle();
            device.destroy(depth_image);
        }
        auto created = device.create_image(
            rhi::ImageDesc{width, height, 1, rhi::Format::Depth32Float, false, true, "depth"});
        if (!created) {
            return false;
        }
        depth_image  = *created;
        depth_width  = width;
        depth_height = height;
        return true;
    };
    if (!ensure_depth()) {
        return 1;
    }

    // Looking down at the platform from a corner, so that the step's inside
    // corner, both trees and the glass are all in frame.
    render::Camera camera;
    camera.position      = Vec3f{-7.0F, 14.0F, -7.0F};
    camera.yaw_degrees   = -45.0F;
    camera.pitch_degrees = 27.0F;

    if (!options.camera.empty()) {
        std::array<f32, 5> values{camera.position.x, camera.position.y, camera.position.z,
                                  camera.yaw_degrees, camera.pitch_degrees};
        usize              index = 0;
        usize              start = 0;
        while (index < values.size() && start <= options.camera.size()) {
            const auto end  = options.camera.find(',', start);
            const auto text = options.camera.substr(
                start, end == std::string::npos ? std::string::npos : end - start);
            values[index++] = static_cast<f32>(std::atof(text.c_str()));
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
        camera.position      = Vec3f{values[0], values[1], values[2]};
        camera.yaw_degrees   = values[3];
        camera.pitch_degrees = values[4];
    }

    (*window)->set_cursor_captured(options.frames == 0);

    rhi::BufferHandle readback;
    if (!options.screenshot.empty()) {
        const usize bytes =
            static_cast<usize>(device.swapchain_width()) * device.swapchain_height() * 4;
        auto buffer = device.create_buffer(
            rhi::BufferDesc{bytes, rhi::BufferUsage::Upload, "screenshot readback"});
        if (!buffer) {
            return 1;
        }
        readback = *buffer;
    }

    u32  rendered = 0;
    bool running  = true;
    bool captured = false;

    while (running) {
        const auto& input = (*window)->poll();
        if ((*window)->should_close()) {
            running = false;
        }
        if (input.just_pressed(client::Key::Escape)) {
            (*window)->set_cursor_captured(false);
        }
        if ((*window)->minimised()) {
            continue;
        }

        camera.turn(static_cast<f32>(input.mouse_delta_x), static_cast<f32>(input.mouse_delta_y));

        const f32   speed   = input.held(client::Key::Sprint) ? 0.6F : 0.15F;
        const Vec3f forward = camera.forward();
        const Vec3f right   = camera.right();
        if (input.held(client::Key::Forward)) {
            camera.position += forward * speed;
        }
        if (input.held(client::Key::Back)) {
            camera.position -= forward * speed;
        }
        if (input.held(client::Key::Right)) {
            camera.position += right * speed;
        }
        if (input.held(client::Key::Left)) {
            camera.position -= right * speed;
        }
        if (input.held(client::Key::Up)) {
            camera.position.y += speed;
        }
        if (input.held(client::Key::Down)) {
            camera.position.y -= speed;
        }

        auto frame = device.begin_frame();
        if (!frame) {
            if (frame.error() == rhi::RhiError::SwapchainOutOfDate) {
                (void)device.resize((*window)->framebuffer_width(),
                                    (*window)->framebuffer_height());
                if (!ensure_depth()) {
                    return 1;
                }
                continue;
            }
            OV_LOG_ERROR("begin_frame: {}", rhi::to_string(frame.error()));
            return 1;
        }

        rhi::CommandList& cmd    = **frame;
        const u32         width  = device.swapchain_width();
        const u32         height = device.swapchain_height();

        cmd.transition_swapchain(rhi::ResourceState::Undefined,
                                 rhi::ResourceState::ColourAttachment);
        cmd.transition(depth_image, rhi::ResourceState::Undefined,
                       rhi::ResourceState::DepthAttachment);

        rhi::ColourAttachment colour;
        colour.clear           = true;
        colour.clear_colour[0] = 0.47F;
        colour.clear_colour[1] = 0.65F;
        colour.clear_colour[2] = 1.0F;
        colour.clear_colour[3] = 1.0F;

        rhi::DepthAttachment depth;
        depth.image       = depth_image;
        depth.clear       = true;
        depth.clear_depth = 1.0F;

        const std::array<rhi::ColourAttachment, 1> attachments{colour};
        cmd.begin_rendering(attachments, &depth, width, height);
        cmd.set_viewport(0.0F, 0.0F, static_cast<f32>(width), static_cast<f32>(height));
        cmd.set_scissor(0, 0, width, height);

        PushConstants push;
        push.view_projection =
            camera.view_projection(static_cast<f32>(width) / static_cast<f32>(height));
        push.section_origin = {0.0F, 0.0F, 0.0F, 0.0F};

        const std::array<rhi::ImageHandle, 1>   images{*atlas_image};
        const std::array<rhi::SamplerHandle, 1> samplers{*sampler};

        cmd.bind_index_buffer(index_buffer);

        for (usize i = 0; i < kLayerCount; ++i) {
            if (index_counts[i] == 0) {
                continue;
            }
            const bool cutout   = i == static_cast<usize>(render::RenderLayer::Cutout) ||
                                  i == static_cast<usize>(render::RenderLayer::CutoutMipped);
            const auto pipeline = cutout ? *cutout_pipeline : *opaque_pipeline;

            cmd.bind_pipeline(pipeline);
            cmd.push_constants(pipeline, &push, sizeof(push));
            cmd.bind_textures(pipeline, images, samplers);
            cmd.bind_vertex_buffer(0, vertex_buffers[i]);
            cmd.draw_indexed(index_counts[i]);
        }

        cmd.end_rendering();

        const bool last_frame = options.frames != 0 && rendered + 1 >= options.frames;
        if (readback.valid() && last_frame) {
            cmd.transition_swapchain(rhi::ResourceState::ColourAttachment,
                                     rhi::ResourceState::TransferSource);
            cmd.copy_swapchain_to_buffer(readback);
            cmd.transition_swapchain(rhi::ResourceState::TransferSource,
                                     rhi::ResourceState::Present);
            captured = true;
        } else {
            cmd.transition_swapchain(rhi::ResourceState::ColourAttachment,
                                     rhi::ResourceState::Present);
        }

        auto presented = device.end_frame();
        ++rendered;
        if (!presented && presented.error() == rhi::RhiError::SwapchainOutOfDate) {
            (void)device.resize((*window)->framebuffer_width(), (*window)->framebuffer_height());
            if (!ensure_depth()) {
                return 1;
            }
        }

        if (options.frames != 0 && rendered >= options.frames) {
            running = false;
        }
    }

    device.wait_idle();

    if (captured) {
        const u32   width  = device.swapchain_width();
        const u32   height = device.swapchain_height();
        const usize bytes  = static_cast<usize>(width) * height * 4;

        const auto* mapped = static_cast<const u8*>(device.map(readback));
        if (mapped == nullptr) {
            OV_LOG_ERROR("readback buffer is not host visible");
            return 1;
        }

        std::vector<u8> pixels(mapped, mapped + bytes);
        const auto      format = device.swapchain_format();
        if (format == rhi::Format::Bgra8Srgb || format == rhi::Format::Bgra8Unorm) {
            for (usize i = 0; i + 3 < pixels.size(); i += 4) {
                std::swap(pixels[i], pixels[i + 2]);
            }
        }
        if (!write_ppm(options.screenshot, pixels, width, height)) {
            OV_LOG_ERROR("could not write {}", options.screenshot);
            return 1;
        }
        fmt::print("wrote {} ({}x{})\n", options.screenshot, width, height);
    }

    fmt::print("{} frames, last GPU time {:.3f} ms\n", rendered, device.last_frame_gpu_ms());
    return 0;
}
