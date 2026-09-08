// ov_voxel — the playable client.
//
// Right now it opens a window and proves the graphics stack end to end. That is
// worth its own binary from the first day: when the terrain does not appear,
// the first question is always "is it the renderer or the data?", and a mode
// that draws a triangle with no game state attached answers it in a second.
//
// --frames and --screenshot exist so that the thing can be checked without a
// human looking at it, which is the difference between "it built" and "it
// works".

#define OV_LOG_CATEGORY "voxel"

#include "ov/base/log.hpp"
#include "ov/client/window.hpp"
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
#include <vector>

using namespace ov;

namespace {

struct Options {
    u32 width{1280};
    u32 height{720};
    /// Zero means "until the window is closed".
    u32         frames{0};
    std::string screenshot;
    bool        validation{true};
};

[[nodiscard]] Options parse_arguments(std::span<char*> args) {
    Options options;
    for (usize i = 1; i < args.size(); ++i) {
        const std::string_view argument(args[i]);
        const auto             value = [&argument](std::string_view prefix) {
            return argument.substr(prefix.size());
        };

        if (argument.starts_with("--frames=")) {
            options.frames = static_cast<u32>(std::atoi(value("--frames=").data()));
        } else if (argument.starts_with("--width=")) {
            options.width = static_cast<u32>(std::atoi(value("--width=").data()));
        } else if (argument.starts_with("--height=")) {
            options.height = static_cast<u32>(std::atoi(value("--height=").data()));
        } else if (argument.starts_with("--screenshot=")) {
            options.screenshot = std::string(value("--screenshot="));
        } else if (argument == "--no-validation") {
            options.validation = false;
        }
    }
    return options;
}

/// Where the build put the SPIR-V and the pipeline cache: next to the
/// executable, not relative to the working directory, so that running from
/// anywhere works.
[[nodiscard]] std::filesystem::path executable_directory(const char* argv0) {
    std::error_code error;
    auto            path = std::filesystem::weakly_canonical(std::filesystem::path(argv0), error);
    if (error) {
        return std::filesystem::current_path();
    }
    return path.parent_path();
}

/// PPM, because it needs no library and `head -2` says whether it is right.
/// Golden images belong on Linux with lavapipe, not here — MoltenVK is not a
/// conformance oracle.
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

}  // namespace

int main(int argc, char** argv) {
    const std::span<char*> args(argv, static_cast<usize>(argc));
    const Options          options = parse_arguments(args);

    const auto base = executable_directory(argv[0]);

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

    auto device = rhi::Device::create(desc);
    if (!device) {
        OV_LOG_ERROR("no graphics device: {}", rhi::to_string(device.error()));
        return 1;
    }
    OV_LOG_INFO("device: {} ({})", (*device)->info().name, (*device)->info().driver);

    rhi::GraphicsPipelineDesc pipeline_desc;
    pipeline_desc.vertex_shader   = "debug_triangle.vert.spv";
    pipeline_desc.fragment_shader = "debug_triangle.frag.spv";
    pipeline_desc.colour_format   = (*device)->swapchain_format();
    pipeline_desc.depth_test      = false;
    pipeline_desc.depth_write     = false;
    pipeline_desc.cull_mode       = rhi::CullMode::None;
    pipeline_desc.debug_name      = "debug triangle";

    auto pipeline = (*device)->create_graphics_pipeline(pipeline_desc);
    if (!pipeline) {
        OV_LOG_ERROR("pipeline: {}", rhi::to_string(pipeline.error()));
        return 1;
    }

    // A screenshot is a copy recorded inside the frame that owns the image, so
    // the buffer has to exist before the loop and the copy has to happen on the
    // last frame rather than after it.
    rhi::BufferHandle readback;
    if (!options.screenshot.empty()) {
        const usize bytes =
            static_cast<usize>((*device)->swapchain_width()) * (*device)->swapchain_height() * 4;
        auto buffer = (*device)->create_buffer(
            rhi::BufferDesc{bytes, rhi::BufferUsage::Upload, "screenshot readback"});
        if (!buffer) {
            OV_LOG_ERROR("readback buffer: {}", rhi::to_string(buffer.error()));
            return 1;
        }
        readback = *buffer;
    }

    u32  rendered = 0;
    bool running  = true;
    bool captured = false;
    while (running) {
        const auto& input = (*window)->poll();
        if ((*window)->should_close() || input.just_pressed(client::Key::Escape)) {
            running = false;
        }
        if ((*window)->minimised()) {
            continue;
        }

        auto frame = (*device)->begin_frame();
        if (!frame) {
            if (frame.error() == rhi::RhiError::SwapchainOutOfDate) {
                // Normal: the window changed size between the last present and
                // this acquire. Rebuild and take the next frame.
                (void)(*device)->resize((*window)->framebuffer_width(),
                                        (*window)->framebuffer_height());
                continue;
            }
            OV_LOG_ERROR("begin_frame: {}", rhi::to_string(frame.error()));
            return 1;
        }

        rhi::CommandList& cmd    = **frame;
        const u32         width  = (*device)->swapchain_width();
        const u32         height = (*device)->swapchain_height();

        // Every transition is written out. The validation layers are the only
        // thing that checks them, which is why they are on by default.
        cmd.transition_swapchain(rhi::ResourceState::Undefined,
                                 rhi::ResourceState::ColourAttachment);

        rhi::ColourAttachment colour;
        colour.clear           = true;
        colour.clear_colour[0] = 0.05F;
        colour.clear_colour[1] = 0.07F;
        colour.clear_colour[2] = 0.12F;
        colour.clear_colour[3] = 1.0F;

        const std::array<rhi::ColourAttachment, 1> attachments{colour};
        cmd.begin_rendering(attachments, nullptr, width, height);
        cmd.set_viewport(0.0F, 0.0F, static_cast<f32>(width), static_cast<f32>(height));
        cmd.set_scissor(0, 0, width, height);
        cmd.bind_pipeline(*pipeline);
        cmd.draw(3);
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

        auto presented = (*device)->end_frame();
        ++rendered;

        if (!presented && presented.error() == rhi::RhiError::SwapchainOutOfDate) {
            (void)(*device)->resize((*window)->framebuffer_width(),
                                    (*window)->framebuffer_height());
        }

        if (options.frames != 0 && rendered >= options.frames) {
            running = false;
        }
    }

    (*device)->wait_idle();

    if (captured) {
        const u32   width  = (*device)->swapchain_width();
        const u32   height = (*device)->swapchain_height();
        const usize bytes  = static_cast<usize>(width) * height * 4;

        const auto* mapped = static_cast<const u8*>((*device)->map(readback));
        if (mapped == nullptr) {
            OV_LOG_ERROR("readback buffer is not host visible");
            return 1;
        }

        std::vector<u8> pixels(mapped, mapped + bytes);
        // The swapchain is BGRA on every platform that matters. The RHI reports
        // its format rather than assuming, so the swizzle happens here where
        // the file format is known.
        const auto format = (*device)->swapchain_format();
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

    fmt::print("{} frames, last GPU time {:.3f} ms\n", rendered, (*device)->last_frame_gpu_ms());
    return 0;
}
