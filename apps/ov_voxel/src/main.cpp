// ov_voxel — the playable client.
//
// It reads a real 1.20.1 save off the disk, resolves every block state it finds
// through the registry into models, stitches the sprites those models name into
// an atlas, meshes the sections, and draws them. The hand-built demo scene it
// used to draw is gone: everything on screen now comes from a world the game
// itself wrote, which is the only way the numbers mean anything.
//
// --frames and --screenshot exist so the result can be checked without a human
// looking at it, and --stats reports the frame time percentiles the milestone
// is actually judged on.

#define OV_LOG_CATEGORY "voxel"

#include "world_source.hpp"

#include "ov/base/log.hpp"
#include "ov/base/time.hpp"
#include "ov/client/terrain_renderer.hpp"
#include "ov/client/window.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/render/atlas.hpp"
#include "ov/render/block_models.hpp"
#include "ov/render/camera.hpp"
#include "ov/render/chunk_mesher.hpp"
#include "ov/render/frustum.hpp"
#include "ov/rhi/device.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <chrono>
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
    std::string world{"run/world"};
    std::string registry{"data/vanilla/1.20.1/registry.ovpack"};
    /// Chunk radius to load. 12 is the milestone's render distance.
    i32  radius{6};
    i32  centre_x{0};
    i32  centre_z{0};
    bool validation{true};
    /// Diagnostics. When something is missing from the picture the first two
    /// questions are always "was it culled?" and "was it facing away?", and a
    /// flag answers each in one run instead of a rebuild.
    bool cull{true};
    bool backface{true};
    /// FIFO by default. --no-vsync is a measuring instrument, not a speed
    /// setting: under FIFO every frame reads back the refresh interval and the
    /// p99 the milestone is judged on measures the display, not the renderer.
    bool vsync{true};
    /// Draw the terrain the old way, one call a section, for comparison.
    bool indirect{true};
    /// x,y,z,yaw,pitch. Exists so a face can be put in front of the camera and
    /// looked at, which is how the questions a unit test cannot answer — is
    /// this texture mirrored? — actually get settled.
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
        } else if (argument.starts_with("--world=")) {
            options.world = value("--world=");
        } else if (argument.starts_with("--registry=")) {
            options.registry = value("--registry=");
        } else if (argument.starts_with("--radius=")) {
            options.radius = std::atoi(value("--radius=").c_str());
        } else if (argument.starts_with("--at=")) {
            const auto text  = value("--at=");
            const auto comma = text.find(',');
            options.centre_x = std::atoi(text.substr(0, comma).c_str());
            if (comma != std::string::npos) {
                options.centre_z = std::atoi(text.substr(comma + 1).c_str());
            }
        } else if (argument.starts_with("--camera=")) {
            options.camera = value("--camera=");
        } else if (argument == "--no-cull") {
            options.cull = false;
        } else if (argument == "--no-backface") {
            options.backface = false;
        } else if (argument == "--no-validation") {
            options.validation = false;
        } else if (argument == "--no-vsync") {
            options.vsync = false;
        } else if (argument == "--no-indirect") {
            options.indirect = false;
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

[[nodiscard]] std::span<const u8> as_bytes(const auto& container) {
    return std::span(
        reinterpret_cast<const u8*>(container.data()),
        container.size() * sizeof(typename std::decay_t<decltype(container)>::value_type));
}

/// The percentile the milestone is judged on. Not the mean: 60 FPS on average
/// with spikes to 45 ms is unplayable and would pass a test of the mean.
[[nodiscard]] f64 percentile(std::vector<f64> samples, f64 fraction) {
    if (samples.empty()) {
        return 0.0;
    }
    std::ranges::sort(samples);
    const auto index = std::min(samples.size() - 1,
                                static_cast<usize>(fraction * static_cast<f64>(samples.size())));
    return samples[index];
}

}  // namespace

int main(int argc, char** argv) {
    const std::span<char*> args(argv, static_cast<usize>(argc));
    const Options          options = parse_arguments(args);
    const auto             base    = executable_directory(argv[0]);

    // ── Registry, world, models, atlas, mesh: all before any Vulkan ─────────
    auto blocks = registry::BlockRegistry::load(options.registry);
    if (!blocks) {
        OV_LOG_ERROR(
            "registry {}: {}. Generate it with tools/ov_datagen, or pass "
            "--registry=<path>.",
            options.registry, registry::to_string(blocks.error()));
        return 1;
    }
    OV_LOG_INFO("registry: {} blocks, {} states", blocks->block_count(), blocks->state_count());

    const std::filesystem::path assets_root(options.assets);
    if (!std::filesystem::is_directory(assets_root / "assets")) {
        OV_LOG_ERROR(
            "{} has no assets/ directory. Run ov_assetimport first, or pass "
            "--assets=<path>.",
            assets_root.string());
        return 1;
    }
    const render::DirectoryAssetSource source(assets_root);

    const auto load_start = std::chrono::steady_clock::now();
    auto       world = demo::load_world(options.world, *blocks, options.centre_x, options.centre_z,
                                        options.radius);
    if (!world) {
        return 1;
    }
    const auto load_ms =
        std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - load_start)
            .count();
    OV_LOG_INFO("world: {} chunks read, {} failed, {:.0f} ms{}", world->chunks_read,
                world->chunks_failed, load_ms,
                world->light_stored ? "" : " — WITHOUT stored light");

    // Resolve every state the world uses before the atlas is stitched: the
    // atlas needs the full set of sprites, and the render layer needs the
    // atlas. Resolve, stitch, classify, mesh.
    render::BlockModelCache models(source, *blocks);
    for (const auto& [position, chunk] : world->chunks) {
        (void)position;
        const auto shape = chunk->shape();
        for (i32 y = shape.min_y; y <= shape.max_y(); ++y) {
            for (usize z = 0; z < 16; ++z) {
                for (usize x = 0; x < 16; ++x) {
                    (void)models.resolve(chunk->get_block(x, y, z));
                }
            }
        }
    }
    OV_LOG_INFO("models: {} states resolved, {} with no geometry, {} sprites",
                models.resolved_count(), models.missing_count(), models.sprites().size());

    render::AtlasBuilder builder(source);
    for (const auto& sprite : models.sprites()) {
        builder.add(sprite);
    }
    auto atlas = builder.build();
    if (!atlas) {
        OV_LOG_ERROR("atlas: {}", render::to_string(atlas.error()));
        return 1;
    }
    models.classify_layers(*atlas);
    OV_LOG_INFO("atlas {}x{}, {} sprites, {} mip levels", atlas->width(), atlas->height(),
                atlas->sprites().size(), atlas->mips().size());

    // ── Mesh every section ──────────────────────────────────────────────────
    struct SectionMesh {
        Vec3f               origin;
        render::MeshBuffers buffers;
    };

    const auto               mesh_start = std::chrono::steady_clock::now();
    std::vector<SectionMesh> meshes;
    usize                    total_quads = 0;
    bool                     saw_light   = false;

    for (const auto& [position, chunk] : world->chunks) {
        const auto neighbours = world->neighbours(position.first, position.second);
        const auto shape      = chunk->shape();

        for (usize index = 0; index < shape.section_count(); ++index) {
            const i32 origin_y = shape.min_y + static_cast<i32>(index) * 16;

            const render::ChunkSectionView view(*blocks, neighbours, position.first * 16, origin_y,
                                                position.second * 16);
            SectionMesh                    mesh;
            mesh.origin = Vec3f{static_cast<f32>(position.first * 16), static_cast<f32>(origin_y),
                                static_cast<f32>(position.second * 16)};

            const auto stats = render::mesh_section(view, models, *atlas, mesh.buffers);
            saw_light        = saw_light || view.any_light_stored();
            if (stats.quads == 0) {
                continue;
            }
            total_quads += stats.quads;
            meshes.push_back(std::move(mesh));
        }
    }
    const auto mesh_ms =
        std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - mesh_start)
            .count();
    OV_LOG_INFO("mesh: {} sections with geometry, {} quads, {:.0f} ms{}", meshes.size(),
                total_quads, mesh_ms, saw_light ? "" : " — no stored light was read");

    if (meshes.empty()) {
        OV_LOG_ERROR("nothing to draw: every section meshed to zero quads");
        return 1;
    }

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
    desc.vsync               = options.vsync;

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

    // Every section's vertices go into one device-local arena and the terrain
    // is drawn with one indirect call per layer. What was here before — a
    // VkBuffer per section per layer, a bind and a push and a draw for each —
    // cost 10.49 ms at the 99th percentile just to record, at a radius of 12
    // on an M2. The plan called for this; the measurement is what justified
    // doing it now rather than later.
    usize max_quads = 0;
    for (const auto& mesh : meshes) {
        for (const auto& layer : mesh.buffers.layers) {
            max_quads = std::max(max_quads, layer.size() / 4);
        }
    }

    client::TerrainRendererDesc terrain_desc;
    terrain_desc.colour_format         = device.swapchain_format();
    terrain_desc.backface_culling      = options.backface;
    terrain_desc.force_per_section_draws = !options.indirect;
    terrain_desc.max_quads_per_section = static_cast<u32>(std::max<usize>(max_quads, 1));
    terrain_desc.max_sections =
        static_cast<u32>(std::max<usize>(meshes.size() * static_cast<usize>(render::RenderLayer::Count), 1024));

    auto terrain = client::TerrainRenderer::create(device, terrain_desc);
    if (!terrain) {
        OV_LOG_ERROR("terrain renderer: {}", rhi::to_string(terrain.error()));
        return 1;
    }

    usize uploaded_sections = 0;
    for (const auto& mesh : meshes) {
        for (usize i = 0; i < static_cast<usize>(render::RenderLayer::Count); ++i) {
            const auto& vertices = mesh.buffers.layers[i];
            if (vertices.empty()) {
                continue;
            }
            if (!(*terrain)->add_section(mesh.origin, static_cast<render::RenderLayer>(i),
                                         vertices)) {
                OV_LOG_ERROR("the terrain arena would not take section {}", uploaded_sections);
                return 1;
            }
            ++uploaded_sections;
        }
    }
    {
        const auto& terrain_stats = (*terrain)->stats();
        OV_LOG_INFO("gpu: {} sections in one arena, {:.1f} of {} MiB used, {} free block(s)",
                    terrain_stats.sections_resident,
                    static_cast<f64>(terrain_stats.arena_used) / (1024.0 * 1024.0),
                    terrain_stats.arena_capacity / (1024 * 1024),
                    terrain_stats.arena_largest_free == 0 ? 0 : 1);
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

    render::Camera camera;
    camera.position      = world->suggested_camera(*blocks);
    camera.yaw_degrees   = -45.0F;
    camera.pitch_degrees = 20.0F;
    camera.far_plane     = static_cast<f32>(options.radius + 2) * 16.0F * 1.8F;

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
    OV_LOG_INFO("camera at ({:.1f}, {:.1f}, {:.1f})", camera.position.x, camera.position.y,
                camera.position.z);

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

    std::vector<f64> cpu_frame_ms;
    std::vector<f64> record_ms;
    std::vector<f64> gpu_frame_ms;
    u32              drawn_last_frame = 0;
    u32              rendered         = 0;
    bool             running          = true;
    bool             captured         = false;

    while (running) {
        const auto  frame_start = std::chrono::steady_clock::now();
        const auto& input       = (*window)->poll();
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

        const f32   speed   = input.held(client::Key::Sprint) ? 1.2F : 0.3F;
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

        // The timer starts here and not before begin_frame: acquiring an image
        // waits on the display and on the frame the GPU is still running, and
        // folding that into "what the CPU spends" would make the number report
        // the GPU's cost as the CPU's.
        const auto        record_start = std::chrono::steady_clock::now();
        rhi::CommandList& cmd          = **frame;
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

        const auto view_projection =
            camera.view_projection(static_cast<f32>(width) / static_cast<f32>(height));
        const auto frustum = render::Frustum::from_view_projection(view_projection);

        (*terrain)->draw(cmd, view_projection, frustum, *atlas_image, *sampler, options.cull);
        drawn_last_frame = (*terrain)->stats().sections_drawn;

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

        record_ms.push_back(
            std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - record_start)
                .count());

        auto presented = device.end_frame();
        ++rendered;
        if (!presented && presented.error() == rhi::RhiError::SwapchainOutOfDate) {
            (void)device.resize((*window)->framebuffer_width(), (*window)->framebuffer_height());
            if (!ensure_depth()) {
                return 1;
            }
        }

        cpu_frame_ms.push_back(
            std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - frame_start)
                .count());
        if (device.last_frame_gpu_ms() > 0.0) {
            gpu_frame_ms.push_back(device.last_frame_gpu_ms());
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

    // The milestone's target is a percentile, so that is what gets printed —
    // and the first frames are dropped, because they carry the driver's
    // one-off shader translation and would put a 100 ms outlier in every p99
    // that has nothing to do with the renderer.
    constexpr usize kWarmUpFrames = 10;
    const auto      drop          = [](std::vector<f64> samples) {
        if (samples.size() > kWarmUpFrames) {
            samples.erase(samples.begin(), samples.begin() + static_cast<isize>(kWarmUpFrames));
        }
        return samples;
    };
    const auto cpu    = drop(cpu_frame_ms);
    const auto record = drop(record_ms);
    const auto gpu    = drop(gpu_frame_ms);

    const auto& terrain_stats = (*terrain)->stats();
    fmt::print("{} frames ({} after warm-up), {} of {} sections drawn last frame in {} call(s)\n",
               rendered, cpu.size(), drawn_last_frame, terrain_stats.sections_resident,
               terrain_stats.draw_calls);
    fmt::print("cpu  p50 {:.2f} ms   p99 {:.2f} ms   max {:.2f} ms{}\n", percentile(cpu, 0.50),
               percentile(cpu, 0.99), percentile(cpu, 1.0),
               options.vsync ? "   (vsync: this is the refresh, not the work)" : "");
    fmt::print("rec  p50 {:.2f} ms   p99 {:.2f} ms   max {:.2f} ms\n", percentile(record, 0.50),
               percentile(record, 0.99), percentile(record, 1.0));
    fmt::print("gpu  p50 {:.2f} ms   p99 {:.2f} ms   max {:.2f} ms\n", percentile(gpu, 0.50),
               percentile(gpu, 0.99), percentile(gpu, 1.0));
    return 0;
}
