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

#include "session.hpp"
#include "world_source.hpp"

#include "ov/base/log.hpp"
#include "ov/base/time.hpp"
#include "ov/client/terrain_renderer.hpp"
#include "ov/client/window.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"
#include "ov/render/atlas.hpp"
#include "ov/render/biome_colours.hpp"
#include "ov/render/block_models.hpp"
#include "ov/render/camera.hpp"
#include "ov/render/environment.hpp"
#include "ov/render/chunk_mesher.hpp"
#include "ov/gameplay/physics.hpp"
#include "ov/math/raycast.hpp"
#include "ov/netclient/client.hpp"
#include "ov/render/frustum.hpp"
#include "ov/server/server.hpp"
#include "ov/rhi/device.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
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
    /// Hold forward for the whole run. A diagnostic: it makes the movement
    /// loop testable without a human at the keyboard, which is the only way a
    /// physics regression gets caught by a script.
    bool walk{false};
    /// Break the block under the player once it has settled, then report
    /// whether the world actually changed. The end-to-end check of
    /// client -> server -> client that no screenshot can make.
    bool dig{false};
    /// What to hold, by registry name. Creative only: the client asks the
    /// server to put it in the hotbar. Without it the hand is empty and every
    /// placement is silently a no-op — which looks exactly like a broken
    /// placement packet.
    std::string hold{"minecraft:stone"};
    /// Draw the terrain the old way, one call a section, for comparison.
    bool indirect{true};
    /// Time of day in ticks. 6000 is noon, 18000 midnight.
    i64 time{6000};
    /// Let the clock run at the game's twenty ticks a second, so a sunset can
    /// be watched rather than posed.
    bool daylight_cycle{true};
    /// Push the fog past the far plane. A diagnostic, like --no-cull: when the
    /// whole screen is one colour the first question is whether the terrain
    /// drew and the fog ate it, or whether it never drew at all.
    bool fog{true};
    /// The brightness slider, 0 Moody to 1 Bright. 1.20.1 defaults to 0.5.
    f32 gamma{0.5F};
    /// host:port. When set, the world comes from a server rather than from the
    /// disk, and the camera becomes a player: gravity, collision, and a
    /// position reported twenty times a second.
    std::string connect;
    std::string username{"OndesVoxel"};
    /// Host a server in this process and play on it.
    ///
    /// Not a shortcut past the protocol: the server runs on its own thread,
    /// binds a real socket, and the client connects to it the way it connects
    /// to any other. The bytes are the same ones a remote server would send,
    /// which is the whole point of the project's second principle — the plan's
    /// LoopbackTransport replaces the socket with an SPSC queue carrying those
    /// same bytes, and nothing above the transport changes when it does.
    bool singleplayer{false};
    u16  singleplayer_port{25599};
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
        } else if (argument.starts_with("--time=")) {
            options.time = std::atoll(value("--time=").c_str());
        } else if (argument.starts_with("--gamma=")) {
            options.gamma = static_cast<f32>(std::atof(value("--gamma=").c_str()));
        } else if (argument == "--no-daylight-cycle") {
            options.daylight_cycle = false;
        } else if (argument == "--no-fog") {
            options.fog = false;
        } else if (argument.starts_with("--connect=")) {
            options.connect = value("--connect=");
        } else if (argument.starts_with("--username=")) {
            options.username = value("--username=");
        } else if (argument == "--walk") {
            options.walk = true;
        } else if (argument == "--dig") {
            options.dig = true;
        } else if (argument.starts_with("--hold=")) {
            options.hold = value("--hold=");
        } else if (argument == "--singleplayer") {
            options.singleplayer = true;
        } else if (argument.starts_with("--singleplayer-port=")) {
            options.singleplayer_port =
                static_cast<u16>(std::atoi(value("--singleplayer-port=").c_str()));
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

/// The biome the camera is standing in, whose fog and sky colour the frame
/// takes. Vanilla samples where you are, not where you are looking.
[[nodiscard]] u32 camera_biome(const demo::LoadedWorld& world,
                               const registry::BlockRegistry& blocks, Vec3f position) {
    (void)blocks;
    const auto x = static_cast<i32>(std::floor(position.x));
    const auto y = static_cast<i32>(std::floor(position.y));
    const auto z = static_cast<i32>(std::floor(position.z));

    const world::Chunk* chunk = world.at(x >> 4, z >> 4);
    if (chunk == nullptr || !chunk->shape().contains_y(y)) {
        return 0;
    }
    return chunk->get_biome(static_cast<usize>(x & 15), y, static_cast<usize>(z & 15));
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
    Options                options = parse_arguments(args);
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

    // The item registry, for turning a held block's name into the id the
    // creative-slot packet carries. Same file, different section.
    auto registries_result = registry::Registries::load(options.registry);
    const registry::Registries* registries =
        registries_result ? &*registries_result : nullptr;
    if (!registries) {
        OV_LOG_WARN("item registry unavailable; nothing can be held");
    }

    const std::filesystem::path assets_root(options.assets);
    if (!std::filesystem::is_directory(assets_root / "assets")) {
        OV_LOG_ERROR(
            "{} has no assets/ directory. Run ov_assetimport first, or pass "
            "--assets=<path>.",
            assets_root.string());
        return 1;
    }
    const render::DirectoryAssetSource source(assets_root);

    // Two ways to get a world. Reading it off the disk is a viewer; asking a
    // server for it is the game — and the second is the one the project's
    // second principle is about, because the bytes on that socket are the same
    // whether the server is across the world or on the next thread.
    if (options.singleplayer && options.connect.empty()) {
        options.connect = "127.0.0.1:" + std::to_string(options.singleplayer_port);
    }
    const bool online = !options.connect.empty();

    std::optional<demo::LoadedWorld> world;
    if (!online) {
        const auto load_start = std::chrono::steady_clock::now();
        world = demo::load_world(options.world, *blocks, options.centre_x, options.centre_z,
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
    }

    // Resolve every state the world uses before the atlas is stitched: the
    // atlas needs the full set of sprites, and the render layer needs the
    // atlas. Resolve, stitch, classify, mesh.
    render::BlockModelCache models(source, *blocks);
    if (online) {
        // Every state in the game, because the atlas has to be complete before
        // the first chunk arrives and there is no way to know what will. This
        // is what vanilla does too: the atlas is stitched at start-up, not
        // grown as blocks are seen.
        for (u32 id = 0; id < blocks->state_count(); ++id) {
            (void)models.resolve(registry::BlockStateId{static_cast<u16>(id)});
        }
    } else {
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

    // The biome colours, resolved once against the pack's colormaps. The world
    // was loaded with the registry's own biome names, so a chunk's numeric
    // biome is already the index this is keyed by and the map is the identity —
    // stated explicitly rather than assumed, because the day a save carries a
    // biome the registry does not have, the identity stops holding.
    auto biome_colours = render::BiomeColours::load(source, *blocks);
    if (!biome_colours) {
        OV_LOG_ERROR("biome colours: {}", render::to_string(biome_colours.error()));
        return 1;
    }
    std::vector<u32> biome_identity(blocks->biome_count());
    for (u32 index = 0; index < biome_identity.size(); ++index) {
        biome_identity[index] = index;
    }
    const render::BiomeTints tints(*biome_colours, biome_identity);
    OV_LOG_INFO("atlas {}x{}, {} sprites, {} mip levels", atlas->width(), atlas->height(),
                atlas->sprites().size(), atlas->mips().size());

    // ── Mesh every section ──────────────────────────────────────────────────
    struct SectionMesh {
        Vec3f               origin;
        render::MeshBuffers buffers;
    };

    // An empty map when online: nothing has arrived yet, and the streaming
    // path meshes under a per-frame budget instead of all at once.
    static const std::map<std::pair<i32, i32>, std::unique_ptr<world::Chunk>> kNoChunks;
    const auto& offline_chunks = online ? kNoChunks : world->chunks;

    const auto               mesh_start = std::chrono::steady_clock::now();
    std::vector<SectionMesh> meshes;
    usize                    total_quads = 0;
    bool                     saw_light   = false;

    for (const auto& [position, chunk] : offline_chunks) {
        const auto neighbours = world->neighbours(position.first, position.second);
        const auto shape      = chunk->shape();

        for (usize index = 0; index < shape.section_count(); ++index) {
            const i32 origin_y = shape.min_y + static_cast<i32>(index) * 16;

            const render::ChunkSectionView view(*blocks, neighbours, position.first * 16, origin_y,
                                                position.second * 16, &tints);
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

    if (meshes.empty() && !online) {
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
    if (online) {
        // Nothing has been meshed yet, so the bound is stated instead of
        // measured. 32768 covers the worst a section can hold — 4096 blocks of
        // six faces is 24576, and a cutout model can exceed one quad a face.
        // The index buffer it fixes is 768 KiB, once, for the whole terrain.
        max_quads = 32768;
    }

    client::TerrainRendererDesc terrain_desc;
    terrain_desc.colour_format         = device.swapchain_format();
    terrain_desc.backface_culling      = options.backface;
    terrain_desc.force_per_section_draws = !options.indirect;
    terrain_desc.max_quads_per_section = static_cast<u32>(std::max<usize>(max_quads, 1));
    terrain_desc.max_sections = static_cast<u32>(std::max<usize>(
        meshes.size() * static_cast<usize>(render::RenderLayer::Count), online ? 65536 : 1024));

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

    // ── The server ──────────────────────────────────────────────────────────
    std::atomic<bool> stop_server{false};
    std::thread       server_thread;
    if (options.singleplayer) {
        const std::string port_argument = "--port=" + std::to_string(options.singleplayer_port);
        server_thread = std::thread([&stop_server, port_argument]() {
            // Argv-shaped because that is the server's own interface, and
            // giving it a second one would leave two ways to configure the
            // same thing.
            std::array<const char*, 2> arguments{"ov_voxel", port_argument.c_str()};
            std::array<char*, 2>       argv_copy{const_cast<char*>(arguments[0]),
                                           const_cast<char*>(arguments[1])};
            (void)ov::server::run(2, argv_copy.data(), &stop_server);
        });
    }

    std::unique_ptr<netclient::Client> client;
    std::unique_ptr<demo::Session>     session;
    netclient::ClientEvents            events;
    gameplay::MotionState              player;
    const gameplay::MotionConstants    motion;
    bool                               spawned = false;

    if (online) {
        std::string host = options.connect;
        u16         port = 25565;
        if (const auto colon = host.rfind(':'); colon != std::string::npos) {
            port = static_cast<u16>(std::atoi(host.substr(colon + 1).c_str()));
            host = host.substr(0, colon);
        }

        netclient::ClientDesc login;
        login.host          = host;
        login.port          = port;
        login.username      = options.username;
        login.registry      = &*blocks;
        login.view_distance = static_cast<u8>(std::clamp(options.radius, 2, 32));

        // Retried rather than probed. A hosted server is still binding its
        // socket when this runs, and an earlier version answered that by
        // opening a throwaway connection to test — which logged in, created a
        // player, and left a ghost in the player list. Retrying the real
        // connection costs nothing and creates nobody.
        std::expected<std::unique_ptr<netclient::Client>, netclient::ClientError> connected =
            std::unexpected(netclient::ClientError::CannotConnect);
        const int attempts = options.singleplayer ? 200 : 1;
        for (int attempt = 0; attempt < attempts; ++attempt) {
            connected = netclient::Client::connect(login);
            if (connected) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        if (!connected) {
            OV_LOG_ERROR("{}:{} — {}", host, port, netclient::to_string(connected.error()));
            return 1;
        }
        client  = std::move(*connected);
        session = std::make_unique<demo::Session>(*blocks, models, *atlas, tints, **terrain);
        OV_LOG_INFO("connected to {}:{} as {}", host, port, options.username);
    }

    render::Camera camera;
    camera.position = online ? Vec3f{0.0F, 0.0F, 0.0F} : world->suggested_camera(*blocks);
    camera.yaw_degrees   = -45.0F;
    camera.pitch_degrees = 20.0F;
    camera.far_plane     = static_cast<f32>(options.radius + 2) * 16.0F * 1.8F;

    // The render distance in blocks, which is what the fog is measured against.
    // Vanilla's terrain fog starts at 92 % of it and is complete at 100 %.
    const f32 render_distance = static_cast<f32>(options.radius) * 16.0F;

    // The biome under the camera decides the fog and the sky. One biome for the
    // whole frame, which is what vanilla does too — it samples where you are,
    // not where you are looking.
    render::Lightmap lightmap;
    i64              time_of_day = options.time;

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

    bool     dig_sent   = false;
    bool     place_sent = false;
    BlockPos dig_target{};
    BlockPos place_target{};
    registry::BlockStateId dig_before{};

    auto       start_time       = std::chrono::steady_clock::now();
    auto       last_tick        = start_time;
    f64        tick_accumulator = 0.0;
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

        if (online) {
            if (!client->connected()) {
                const auto why = client->disconnect_reason();
                OV_LOG_ERROR("disconnected: {}", why.empty() ? "the server went away" : why);
                running = false;
                continue;
            }

            client->poll(events);
            if (events.teleport && !spawned) {
                // The first teleport is the spawn. Ask for something in hand
                // once, here: the server has just accepted the login and the
                // inventory it will place from is the one it owns.
                std::optional<registry::ProtocolId> item;
                if (registries) {
                    if (const auto items = registries->find("minecraft:item")) {
                        item = registries->protocol_id(*items, options.hold);
                    }
                }
                if (item) {
                    client->send_creative_slot(36, *item, 1);
                    client->send_held_slot(0);
                    OV_LOG_INFO("holding {} (item {})", options.hold, *item);
                } else {
                    OV_LOG_WARN("{} is not an item; the hand stays empty", options.hold);
                }
            }
            if (events.teleport) {
                // The server is authoritative about where the player is. A
                // teleport is not a suggestion: it arrives at the spawn, and
                // again whenever the server disagrees with what we reported.
                player.position  = events.teleport->position;
                player.velocity  = Vec3d{};
                player.on_ground = false;
                camera.yaw_degrees   = events.teleport->yaw;
                camera.pitch_degrees = events.teleport->pitch;
                spawned              = true;
            }
            if (events.time_of_day) {
                // The world's clock comes from the server, so the sun is in the
                // same place for everyone standing in it.
                options.time         = *events.time_of_day;
                start_time           = std::chrono::steady_clock::now();
                options.daylight_cycle = true;
            }
            session->apply(events);

            // A budget, not a queue drain. A hundred chunks arriving at once
            // must cost several frames rather than one long one; the frame
            // percentile is what the milestone is judged on and a spike passes
            // a test of the mean.
            (void)session->mesh_pending(4.0);

            // Physics at the game's own twenty ticks a second, accumulated
            // against real time. Anything else makes gravity depend on the
            // frame rate.
            constexpr f64 kTickSeconds = 1.0 / 20.0;
            const auto    now          = std::chrono::steady_clock::now();
            tick_accumulator +=
                std::chrono::duration<f64>(now - last_tick).count();
            last_tick = now;
            // Bounded, so that a long stall does not run a hundred ticks at
            // once and teleport the player through the floor.
            tick_accumulator = std::min(tick_accumulator, 0.25);

            // Nothing is simulated until the ground under the player has
            // actually arrived. Without this the player spawns into a world
            // that is still empty, falls through it at terminal velocity, and
            // is a thousand blocks under the map by the time the chunks catch
            // up — which is exactly what vanilla's "loading terrain" screen
            // exists to prevent.
            const bool ground_ready =
                spawned && session->chunk_at(static_cast<i32>(std::floor(player.position.x)) >> 4,
                                             static_cast<i32>(std::floor(player.position.z)) >> 4) !=
                               nullptr;
            if (spawned && !ground_ready) {
                tick_accumulator = 0.0;
            }

            while (ground_ready && tick_accumulator >= kTickSeconds) {
                tick_accumulator -= kTickSeconds;

                gameplay::MoveInput move;
                move.forward = (input.held(client::Key::Forward) || options.walk ? 1.0F : 0.0F) -
                               (input.held(client::Key::Back) ? 1.0F : 0.0F);
                move.strafe = (input.held(client::Key::Right) ? 1.0F : 0.0F) -
                              (input.held(client::Key::Left) ? 1.0F : 0.0F);
                move.yaw    = camera.yaw_degrees;
                move.jump   = input.held(client::Key::Up);
                move.sprint = input.held(client::Key::Sprint);
                move.sneak  = input.held(client::Key::Down);

                const auto world_view = session->collision();
                const auto fluid_view = session->fluids();
                player = gameplay::step(player, move, motion, world_view, &fluid_view);

                netclient::PlayerInput report;
                report.position  = player.position;
                report.yaw       = camera.yaw_degrees;
                report.pitch     = camera.pitch_degrees;
                report.on_ground = player.on_ground;
                client->send_position(report);
            }

            // The eye, not the feet. 1.62 is vanilla's standing eye height and
            // it is what decides whether a one-block sill is at eye level.
            camera.position = Vec3f{static_cast<f32>(player.position.x),
                                    static_cast<f32>(player.position.y + 1.62),
                                    static_cast<f32>(player.position.z)};

            // The scripted dig: wait until the player has been standing for a
            // moment, break what is under its feet, and remember what was
            // there. Whether the server agreed is decided by looking at the
            // world afterwards, not by the fact that a packet was sent.
            if (options.dig && ground_ready && !dig_sent && rendered > 120) {
                dig_target = BlockPos{static_cast<i32>(std::floor(player.position.x)),
                                      static_cast<i32>(std::floor(player.position.y)) - 1,
                                      static_cast<i32>(std::floor(player.position.z))};
                dig_before = session->block_at(dig_target.x, dig_target.y, dig_target.z);
                client->send_dig(dig_target.x, dig_target.y, dig_target.z, 0, 1);
                client->send_dig(dig_target.x, dig_target.y, dig_target.z, 2, 1);
                dig_sent = true;
            }

            // And place one, two blocks to the side — not where the player is
            // standing. The first attempt put it under its own feet and the
            // server refused, correctly: a block may not appear inside a
            // player, and that is a rule this project measured from vanilla
            // and implemented on the server side. The failure looked exactly
            // like a broken placement packet.
            if (options.dig && dig_sent && !place_sent && rendered > 260) {
                place_target = BlockPos{dig_target.x + 2, dig_target.y + 1, dig_target.z};
                client->send_place(place_target.x, place_target.y - 1, place_target.z, 1, 0.5F,
                                   1.0F, 0.5F);
                place_sent = true;
            }

            // Breaking and placing. The ray starts at the eye and runs vanilla's
            // survival reach; the face it entered through is the side a new
            // block attaches to.
            if (input.attack_pressed || input.use_pressed) {
                const Vec3d eye{player.position.x, player.position.y + 1.62, player.position.z};
                const Vec3f look = camera.forward();
                const Vec3d forward{static_cast<f64>(look.x), static_cast<f64>(look.y),
                                    static_cast<f64>(look.z)};
                const auto  hit = raycast_voxels(eye, forward, 4.5, [&](BlockPos block) {
                    return session->block_at(block.x, block.y, block.z) != registry::kAirState;
                });
                if (hit) {
                    const i32 face = static_cast<i32>(hit->face);
                    if (input.attack_pressed) {
                        // Start and finish in the same tick: creative-style
                        // instant breaking. The timed dig belongs with the
                        // block-breaking progress the server already computes.
                        client->send_dig(hit->block.x, hit->block.y, hit->block.z, 0, face);
                        client->send_dig(hit->block.x, hit->block.y, hit->block.z, 2, face);
                    } else {
                        client->send_place(hit->block.x, hit->block.y, hit->block.z, face, 0.5F,
                                           0.5F, 0.5F);
                    }
                }
            }
        }

        const f32   speed   = input.held(client::Key::Sprint) ? 1.2F : 0.3F;
        const Vec3f forward = camera.forward();
        const Vec3f right   = camera.right();
        if (!online && input.held(client::Key::Forward)) {
            camera.position += forward * speed;
        }
        if (!online && input.held(client::Key::Back)) {
            camera.position -= forward * speed;
        }
        if (!online && input.held(client::Key::Right)) {
            camera.position += right * speed;
        }
        if (!online && input.held(client::Key::Left)) {
            camera.position -= right * speed;
        }
        if (!online && input.held(client::Key::Up)) {
            camera.position.y += speed;
        }
        if (!online && input.held(client::Key::Down)) {
            camera.position.y -= speed;
        }

        if (options.daylight_cycle) {
            // Twenty ticks a second, the game's own rate. Wall time here and
            // not a tick counter, because this viewer has no server to take a
            // tick from; the moment it does, the time comes from the server.
            time_of_day = options.time +
                          static_cast<i64>(std::chrono::duration<f64>(
                                               std::chrono::steady_clock::now() - start_time)
                                               .count() *
                                           20.0);
        }
        const f32 darken = render::sky_darken(time_of_day, 0.0F, 0.0F);
        lightmap.update(darken, render::kOverworldAmbientLight, options.gamma, 0.0F);

        const u32 biome = online ? session->biome_at(static_cast<i32>(std::floor(camera.position.x)),
                                                     static_cast<i32>(std::floor(camera.position.y)),
                                                     static_cast<i32>(std::floor(camera.position.z)))
                                 : camera_biome(*world, *blocks, camera.position);
        const auto effects = blocks->biome(biome);
        const u32   fog_rgb   = render::fog_colour(effects.fog_colour, darken);
        const u32   sky_rgb   = render::sky_colour(effects.sky_colour, darken);

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

        // The clear colour is the fog's, not the sky's. Anything the terrain
        // does not cover is at infinite distance, where the fog is complete —
        // so clearing to the sky colour would draw a hard line along the
        // horizon between faded terrain and unfaded sky.
        rhi::ColourAttachment colour;
        colour.clear           = true;
        colour.clear_colour[0] = static_cast<f32>((fog_rgb >> 16) & 0xFFU) / 255.0F;
        colour.clear_colour[1] = static_cast<f32>((fog_rgb >> 8) & 0xFFU) / 255.0F;
        colour.clear_colour[2] = static_cast<f32>(fog_rgb & 0xFFU) / 255.0F;
        colour.clear_colour[3] = 1.0F;
        (void)sky_rgb;

        rhi::DepthAttachment depth;
        depth.image       = depth_image;
        depth.clear       = true;
        depth.clear_depth = 1.0F;

        (*terrain)->upload_sky(cmd, lightmap);

        const std::array<rhi::ColourAttachment, 1> attachments{colour};
        cmd.begin_rendering(attachments, &depth, width, height);
        cmd.set_viewport(0.0F, 0.0F, static_cast<f32>(width), static_cast<f32>(height));
        cmd.set_scissor(0, 0, width, height);

        const auto view_projection =
            camera.view_projection(static_cast<f32>(width) / static_cast<f32>(height));
        const auto frustum = render::Frustum::from_view_projection(view_projection);

        client::SkyFrame sky;
        sky.lightmap   = &lightmap;
        sky.fog_colour = fog_rgb;
        sky.fog_start  = options.fog ? render_distance * 0.92F : 1.0e9F;
        sky.fog_end    = options.fog ? render_distance : 1.1e9F;

        (*terrain)->draw(cmd, view_projection, frustum, camera.position, *atlas_image, *sampler,
                         sky, options.cull);
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

    // The client goes first: a server torn down under a live connection logs a
    // disconnection that did not happen.
    client.reset();
    if (server_thread.joinable()) {
        stop_server = true;
        server_thread.join();
    }

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

    if (online) {
        if (dig_sent) {
            const auto after = session->block_at(dig_target.x, dig_target.y, dig_target.z);
            fmt::print("dug ({}, {}, {}): {} -> {}  {}\n", dig_target.x, dig_target.y,
                       dig_target.z, blocks->block_name(blocks->block_of(dig_before)),
                       blocks->block_name(blocks->block_of(after)),
                       after == registry::kAirState ? "BROKEN" : "unchanged");
            if (place_sent) {
                const auto placed =
                    session->block_at(place_target.x, place_target.y, place_target.z);
                fmt::print("placed at ({}, {}, {}): {}  {}\n", place_target.x, place_target.y,
                           place_target.z, blocks->block_name(blocks->block_of(placed)),
                           placed == registry::kAirState ? "NOTHING WAS PLACED" : "PLACED");
            }
        }
        fmt::print("player at ({:.2f}, {:.2f}, {:.2f}), {}, {} chunks, {} sections resident\n",
                   player.position.x, player.position.y, player.position.z,
                   player.on_ground ? "standing" : "in the air", session->chunk_count(),
                   session->resident_sections());
    }
    return 0;
}
