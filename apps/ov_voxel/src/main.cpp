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

#include "entities.hpp"
#include "interface.hpp"
#include "menus.hpp"  // ── screens ──
#include "session.hpp"
#include "world_source.hpp"

#include "ov/client/debug_overlay.hpp"  // ── screens ──
#include "ov/client/options_file.hpp"   // ── screens ──
#include "ov/io/file.hpp"               // ── screens ──
#include "ov/world/level_dat.hpp"       // ── screens ──

#include <random>        // ── screens ── a blank seed field's seed, as vanilla
#include <sys/sysctl.h>  // ── screens ── F3's CPU and memory lines
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

#include "ov/base/log.hpp"
#include "ov/base/time.hpp"
#include "ov/client/creative_screen.hpp"
#include "ov/client/entity_renderer.hpp"
#include "ov/client/overlay.hpp"
#include "ov/audio/sound_catalog.hpp"   // ── sound ──
#include "ov/audio/sound_engine.hpp"    // ── sound ──
#include "ov/client/sound_director.hpp" // ── sound ──
#include "ov/client/terrain_renderer.hpp"
#include "ov/client/window.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"
#include "ov/render/text_component.hpp"
#include "ov/render/atlas.hpp"
#include "ov/render/biome_colours.hpp"
#include "ov/render/block_models.hpp"
#include "ov/render/camera.hpp"
#include "ov/render/entity_mesh.hpp"
#include "ov/render/entity_model.hpp"
#include "ov/render/entity_pose.hpp"
#include "ov/render/environment.hpp"
#include "ov/render/precipitation.hpp"  // ── weather ──
#include "ov/gameplay/weather.hpp"      // ── weather ──
#include "ov/render/chunk_mesher.hpp"
#include "ov/render/creative_items.hpp"
#include "ov/render/creative_tabs.hpp"
#include "ov/render/language.hpp"
#include "ov/render/font.hpp"
#include "ov/render/item_model.hpp"
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
#include <unordered_map>
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
    // ── The interface ───────────────────────────────────────────────────
    /// Draw the HUD. --no-hud is a measuring instrument: the frame cost of the
    /// interface is the difference between the two on the same scene.
    bool hud{true};
    /// 0 is vanilla's automatic rule: the largest integer leaving 320x240.
    u32 gui_scale{0};
    std::string language{"en_us"};
    /// Put the player's own inventory up after this many frames. A diagnostic,
    /// so a screenshot of it can be taken without a hand on the keyboard.
    u32 open_inventory{0};
    /// x,y,z of a block to right-click once the world has settled — a chest,
    /// to open it.
    ///
    /// ⚠️ The lab bench's own chests cannot be opened: ov_lab writes the chest
    /// *block* and no block entity, and the server only opens a window when it
    /// finds one. So a scripted round trip places its own chest first; see
    /// --place-at and docs/provenance/interface.md.
    std::string use_block;
    /// x,y,z to place what is held at, by clicking the block below it.
    std::string place_at;
    /// from,to: two clicks in the window that opens, a pickup and a place.
    /// The scripted half of the round trip the mandate asks for.
    std::string move_slots;
    /// from,a,b,c…: pick the stack up from `from`, then spread it over the
    /// rest with a left drag. Sixty-four stone over three slots is 21/21/21
    /// with one left on the cursor, which is what the server was measured
    /// doing and what this checks we ask it for.
    std::string drag_slots;
    /// Print the open window's contents and the player's inventory at the end.
    bool dump_window{false};
    /// x,y,z[,yaw,pitch] to stand at once the server has spawned the player.
    ///
    /// The server believes what a client reports about its own position — a
    /// stated gap in server.cpp, not a hole this opens — so this is how a
    /// scripted run reaches a plot of the lab bench without walking there for
    /// forty seconds.
    std::string stand_at;
    /// Close the open window at this frame, so a run can prove the server kept
    /// what was put in it.
    u32 close_at{0};

    // ── The creative inventory ──────────────────────────────────────────────
    /// The catalogue, produced by scripts/measure_creative_tabs.py. Gitignored
    /// like every other datum derived from Mojang's files.
    std::string creative_tabs{"data/vanilla/1.20.1/creative_tabs.json"};
    /// Put the creative inventory up after this many frames, so a capture of
    /// each tab can be taken without a hand on the keyboard.
    u32 open_creative{0};
    /// Which tab to show, by registry id, e.g. minecraft:redstone_blocks.
    std::string creative_tab;
    /// Type this into the search field, which also selects the search tab.
    std::string creative_search;
    /// cell,slot: take the stack in a visible cell and put it in a window-0
    /// slot, exactly as two clicks would. The scripted half of the round trip.
    std::string creative_take;
    /// Print the visible page at the end.
    bool dump_creative{false};
    /// Tooltips, tints, durability and armour slots, asked of the real client
    /// by scripts/measure_creative_screen.py.
    std::string creative_items{"data/vanilla/1.20.1/creative_items.json"};
    /// The data generator's datapack, for '#' searches.
    std::string item_tags{"data/vanilla/1.20.1/generated/data"};
    /// The saved hotbars, vanilla's file format.
    std::string hotbar_file{"run/hotbar.nbt"};
    /// Vanilla's "Operator Items Tab" setting; off by default, as in vanilla.
    bool operator_tab{false};
    /// x,y: hold the pointer there, relative to the creative panel's corner,
    /// for a scripted capture of a hover and its tooltip.
    std::string creative_pointer;
    /// Compare our search page and the oracle's queries with the real
    /// client's answers, print the numbers, and exit. No window, no device.
    bool creative_parity{false};
    // ── chat ──
    /// Scripted chat, through the path Enter takes: each --chat line is sent
    /// in turn, the first at frame chat_at, then one every 40 frames.
    std::vector<std::string> chat_send;
    u32                      chat_at{150};
    /// Open the box at this frame, type chat_type into it (suggestions and
    /// all), and leave it open for the screenshot.
    u32         chat_open_at{0};
    std::string chat_type;
    /// Print every chat message at the end.
    bool dump_chat{false};
    /// Write the Commands packet as received (re-encoded) to this file.
    std::string dump_commands;
    /// At least this many milliseconds a frame. For scripted captures only:
    /// an occluded window is not throttled by vsync, and a frame-counted
    /// script then runs out before the server has answered.
    u32 frame_ms{0};
    /// End the run (and take --screenshot) on the frame the newest chat line
    /// reaches this age in ticks: a fade captured at a stated age, not at
    /// whatever age a frame count happens to land on.
    i32 chat_shot_age{0};
    // ── end chat ──
    /// Dump the font's advances and exit, for scripts/measure_font_widths.py.
    std::string font_widths;

    /// Where the entity geometry was generated. Gitignored, and produced by
    /// scripts/measure_entity_models.py — see docs/provenance/rendu-entites.md
    /// for why it cannot be committed.
    std::string entity_models{"data/vanilla/1.20.1/entity_models.json"};
    /// Draw entities at all. A diagnostic, and what the cost measurement in the
    /// provenance document is taken against.
    bool entities{true};
    /// Render an entity at the last position the server reported instead of
    /// between the last two. Exists to make the interpolation measurable: the
    /// same run, the same packets, one number each way.
    bool entity_interpolation{true};
    /// Report the largest distance any entity moved between two consecutive
    /// frames, which is the number that says whether the smoothing works.
    bool entity_stats{false};
    /// Print every model's rendered box against the type's measured collision
    /// box, then exit. The parity check: it goes through render::emit_entity
    /// and render::entity_bounds, the same code a frame uses, so it cannot
    /// agree with a renderer that disagrees with it.
    bool entity_bounds{false};

    /// x,y,z,yaw,pitch. Exists so a face can be put in front of the camera and
    /// looked at, which is how the questions a unit test cannot answer — is
    /// this texture mirrored? — actually get settled.
    std::string camera;

    // ── sound ──
    /// Play nothing and open no device.
    bool sound{true};
    /// Print every sound that started, at the end: what the end-to-end check reads.
    bool sound_log{false};
    /// `master:0.8,music:0` — the options screen that does not exist yet.
    std::string volumes;

    // ── screens ──
    /// Where the world list lives and where options.txt is, vanilla's layout.
    std::string saves{"run/saves"};
    std::string options_file{"run/options.txt"};
    /// Scripted captures: open this menu (title, options, video, sounds,
    /// controls, mouse, key_binds, language, select_world, create_world,
    /// direct_connect, pause, death) at frame `menu_at`, then press these
    /// widget ids one every 30 frames, type `menu_type` into the focused box.
    std::string              menu;
    u32                      menu_at{0};
    std::vector<std::string> menu_press;
    std::string              menu_type;
    bool                     dump_menu{false};
    /// Start with F3 open.
    bool debug_overlay{false};
    /// What the command line set, so options.txt only fills in the rest.
    bool world_given{false};
    bool lang_given{false};
    bool gui_scale_given{false};
    bool radius_given{false};
    bool vsync_given{false};
    bool hold_given{false};
    // ── end screens ──
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
        } else if (argument == "--no-hud") {
            options.hud = false;
        } else if (argument.starts_with("--gui-scale=")) {
            options.gui_scale = static_cast<u32>(std::atoi(value("--gui-scale=").c_str()));
        } else if (argument.starts_with("--lang=")) {
            options.language = value("--lang=");
        } else if (argument.starts_with("--open-inventory=")) {
            options.open_inventory =
                static_cast<u32>(std::atoi(value("--open-inventory=").c_str()));
        } else if (argument.starts_with("--use-block=")) {
            options.use_block = value("--use-block=");
        } else if (argument.starts_with("--place-at=")) {
            options.place_at = value("--place-at=");
        } else if (argument.starts_with("--move-slots=")) {
            options.move_slots = value("--move-slots=");
        } else if (argument.starts_with("--stand-at=")) {
            options.stand_at = value("--stand-at=");
        } else if (argument.starts_with("--close-at=")) {
            options.close_at = static_cast<u32>(std::atoi(value("--close-at=").c_str()));
        } else if (argument.starts_with("--drag-slots=")) {
            options.drag_slots = value("--drag-slots=");
        } else if (argument.starts_with("--creative-tabs=")) {
            options.creative_tabs = value("--creative-tabs=");
        } else if (argument.starts_with("--open-creative=")) {
            options.open_creative =
                static_cast<u32>(std::atoi(value("--open-creative=").c_str()));
        } else if (argument.starts_with("--creative-tab=")) {
            options.creative_tab = value("--creative-tab=");
        } else if (argument.starts_with("--creative-search=")) {
            options.creative_search = value("--creative-search=");
        } else if (argument.starts_with("--creative-take=")) {
            options.creative_take = value("--creative-take=");
        } else if (argument == "--dump-creative") {
            options.dump_creative = true;
        } else if (argument.starts_with("--creative-items=")) {
            options.creative_items = value("--creative-items=");
        } else if (argument.starts_with("--item-tags=")) {
            options.item_tags = value("--item-tags=");
        } else if (argument.starts_with("--hotbar-file=")) {
            options.hotbar_file = value("--hotbar-file=");
        } else if (argument == "--operator-tab") {
            options.operator_tab = true;
        } else if (argument.starts_with("--creative-pointer=")) {
            options.creative_pointer = value("--creative-pointer=");
        } else if (argument == "--creative-parity") {
            options.creative_parity = true;
        } else if (argument.starts_with("--chat=")) {  // ── chat ──
            options.chat_send.push_back(value("--chat="));
        } else if (argument.starts_with("--chat-file=")) {
            // One line to send per line of the file: JSON components without
            // a shell's quoting in the way.
            std::ifstream lines(value("--chat-file="));
            for (std::string line; std::getline(lines, line);) {
                if (!line.empty()) {
                    options.chat_send.push_back(line);
                }
            }
        } else if (argument.starts_with("--chat-at=")) {
            options.chat_at = static_cast<u32>(std::atoi(value("--chat-at=").c_str()));
        } else if (argument.starts_with("--chat-open=")) {
            options.chat_open_at = static_cast<u32>(std::atoi(value("--chat-open=").c_str()));
        } else if (argument.starts_with("--chat-type=")) {
            options.chat_type = value("--chat-type=");
        } else if (argument == "--dump-chat") {
            options.dump_chat = true;
        } else if (argument.starts_with("--dump-commands=")) {
            options.dump_commands = value("--dump-commands=");
        } else if (argument.starts_with("--frame-ms=")) {
            options.frame_ms = static_cast<u32>(std::atoi(value("--frame-ms=").c_str()));
        } else if (argument.starts_with("--chat-shot-age=")) {
            options.chat_shot_age = std::atoi(value("--chat-shot-age=").c_str());  // ── end chat ──
        } else if (argument == "--dump-window") {
            options.dump_window = true;
        } else if (argument.starts_with("--font-widths=")) {
            options.font_widths = value("--font-widths=");
        } else if (argument.starts_with("--entity-models=")) {
            options.entity_models = value("--entity-models=");
        } else if (argument == "--no-entities") {
            options.entities = false;
        } else if (argument == "--no-entity-interpolation") {
            options.entity_interpolation = false;
        } else if (argument == "--entity-stats") {
            options.entity_stats = true;
        } else if (argument == "--entity-bounds") {
            options.entity_bounds = true;
        } else if (argument == "--no-sound") {  // ── sound ──
            options.sound = false;
        } else if (argument == "--sound-log") {
            options.sound_log = true;
        } else if (argument.starts_with("--volume=")) {
            options.volumes = value("--volume=");
        } else if (argument.starts_with("--saves=")) {  // ── screens ──
            options.saves = value("--saves=");
        } else if (argument.starts_with("--options=")) {
            options.options_file = value("--options=");
        } else if (argument.starts_with("--menu=")) {
            options.menu = value("--menu=");
        } else if (argument.starts_with("--menu-at=")) {
            options.menu_at = static_cast<u32>(std::atoi(value("--menu-at=").c_str()));
        } else if (argument.starts_with("--menu-press=")) {
            std::string list = value("--menu-press=");
            for (usize start = 0; start <= list.size();) {
                const usize end = std::min(list.find(',', start), list.size());
                if (end > start) {
                    options.menu_press.push_back(list.substr(start, end - start));
                }
                start = end + 1;
            }
        } else if (argument.starts_with("--menu-type=")) {
            options.menu_type = value("--menu-type=");
        } else if (argument == "--dump-menu") {
            options.dump_menu = true;
        } else if (argument == "--f3") {
            options.debug_overlay = true;  // ── end screens ──
        } else if (argument == "--singleplayer") {
            options.singleplayer = true;
        } else if (argument.starts_with("--singleplayer-port=")) {
            options.singleplayer_port =
                static_cast<u16>(std::atoi(value("--singleplayer-port=").c_str()));
        }
    }
    // ── screens ── what the command line said, so options.txt fills in the rest
    for (usize i = 1; i < args.size(); ++i) {
        const std::string_view argument(args[i]);
        options.world_given     = options.world_given || argument.starts_with("--world=");
        options.lang_given      = options.lang_given || argument.starts_with("--lang=");
        options.gui_scale_given = options.gui_scale_given || argument.starts_with("--gui-scale=");
        options.radius_given    = options.radius_given || argument.starts_with("--radius=");
        options.vsync_given     = options.vsync_given || argument == "--no-vsync";
        options.hold_given      = options.hold_given || argument.starts_with("--hold=");
    }
    return options;
}

// ── screens ──
/// The client's keys that options.txt can rebind, by vanilla's option names.
constexpr std::array<std::pair<std::string_view, client::Key>, 12> kRebindable{{
    {"key.forward", client::Key::Forward},
    {"key.back", client::Key::Back},
    {"key.left", client::Key::Left},
    {"key.right", client::Key::Right},
    {"key.jump", client::Key::Up},
    {"key.sneak", client::Key::Down},
    {"key.sprint", client::Key::Sprint},
    {"key.inventory", client::Key::Inventory},
    {"key.drop", client::Key::Drop},
    {"key.chat", client::Key::Chat},
    {"key.saveToolbarActivator", client::Key::SaveToolbar},
    {"key.loadToolbarActivator", client::Key::LoadToolbar},
}};

void apply_key_bindings(const client::GameOptions& game, client::Window& window) {
    for (const auto& [name, key] : kRebindable) {
        if (const client::KeyBinding* binding = game.binding(name);
            binding != nullptr && binding->code >= 0) {
            window.bind(key, binding->code);
        }
    }
}

/// F3's memory line: this process's resident size and the machine's memory.
/// Not the JVM heap vanilla shows — there is no heap here — and named so in
/// docs/provenance/ecrans.md.
void memory_numbers(u64& used, u64& total) {
    used  = 0;
    total = 0;
#if defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t      count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                  &count) == KERN_SUCCESS) {
        used = info.resident_size;
    }
    u64    memory = 0;
    size_t size   = sizeof(memory);
    if (sysctlbyname("hw.memsize", &memory, &size, nullptr, 0) == 0) {
        total = memory;
    }
#endif
}

[[nodiscard]] std::string cpu_name() {
#if defined(__APPLE__)
    std::array<char, 128> name{};
    size_t                size = name.size();
    if (sysctlbyname("machdep.cpu.brand_string", name.data(), &size, nullptr, 0) == 0) {
        return std::string(name.data());
    }
#endif
    return {};
}
// ── end screens ──

/// The lightmap colour at an entity's feet.
///
/// One sample per entity, not per fragment: vanilla lights a mob as a whole
/// from the same 16x16 texture the terrain samples per vertex, and doing it on
/// the CPU keeps the entity pipeline down to a single bound image.
///
/// The block light is what the section stores; the sky light is taken at full
/// until ov_world carries a sky light array, and that is a gap rather than a
/// choice — a mob in an unlit cave is drawn as brightly as one in a field.
[[nodiscard]] std::array<u8, 3> entity_light_at(const demo::Session& session,
                                                const render::Lightmap& lightmap, Vec3f position) {
    const i32 block_x = static_cast<i32>(std::floor(position.x));
    const i32 block_y = static_cast<i32>(std::floor(position.y + 0.5F));
    const i32 block_z = static_cast<i32>(std::floor(position.z));
    (void)block_x;
    (void)block_y;
    (void)block_z;
    (void)session;

    constexpr u32 kBlockLight = 0;
    constexpr u32 kSkyLight   = 15;
    const std::span<const u8> pixels = lightmap.pixels();
    const usize               offset =
        (static_cast<usize>(kSkyLight) * render::Lightmap::kSize + kBlockLight) * 4;
    if (offset + 3 > pixels.size()) {
        return {255, 255, 255};
    }
    return {pixels[offset], pixels[offset + 1], pixels[offset + 2]};
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

    // The font's advances, and nothing else. Before the registry and before
    // any Vulkan, so scripts/measure_font_widths.py can compare our table with
    // its own walk of the pack without starting a graphics device.
    if (!options.font_widths.empty()) {
        const render::DirectoryAssetSource font_source{std::filesystem::path(options.assets)};
        auto                               font = render::Font::load_default(font_source);
        if (!font) {
            OV_LOG_ERROR("font: {}", render::to_string(font.error()));
            return 1;
        }
        std::ofstream out(options.font_widths);
        if (!out) {
            OV_LOG_ERROR("could not write {}", options.font_widths);
            return 1;
        }
        for (const char32_t codepoint : font->codepoints()) {
            out << static_cast<u32>(codepoint) << '\t'
                << static_cast<i32>(font->advance(codepoint)) << '\n';
        }
        fmt::print("wrote {} ({} glyphs)\n", options.font_widths, font->glyph_count());
        return 0;
    }

    // ── screens ── options.txt, in vanilla's format, fills in what the
    // command line did not say; a key it does not carry keeps this client's
    // own default. With no world asked for, the game starts on its title.
    client::OptionsFile startup_file;
    if (auto loaded = client::OptionsFile::load(options.options_file); loaded) {
        startup_file = std::move(*loaded);
    }
    const client::GameOptions startup_options = client::GameOptions::from(startup_file);
    if (!options.radius_given && startup_file.get("renderDistance")) {
        options.radius = startup_options.render_distance;
    }
    if (!options.lang_given && startup_file.get("lang")) {
        options.language = startup_options.language;
    }
    if (!options.gui_scale_given && startup_file.get("guiScale")) {
        options.gui_scale = static_cast<u32>(startup_options.gui_scale);
    }
    if (!options.vsync_given && startup_file.get("enableVsync")) {
        options.vsync = startup_options.vsync;
    }
    const bool menu_mode = options.connect.empty() && !options.singleplayer && !options.world_given;
    if (menu_mode && !options.hold_given) {
        options.hold.clear();  // a scripting aid, not something a played world gets
    }
    // ── end screens ──

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

    if (options.entity_bounds) {
        auto loaded = render::EntityModelSet::load(options.entity_models);
        if (!loaded) {
            fmt::print("no entity models ({}): {}\n", options.entity_models,
                       render::to_string(loaded.error()));
            return 1;
        }
        fmt::print("source: {}\n", loaded->source());
        fmt::print("{:<10} {:>4} {:>6} {:>8} {:>8} {:>8} {:>8} {:>8}\n", "model", "bone",
                   "quads", "width", "hitbox", "height", "hitbox", "y0");
        const auto types = registries != nullptr
                               ? registries->find("minecraft:entity_type")
                               : std::optional<registry::RegistryId>{};
        for (const render::EntityModel& model : loaded->models()) {
            std::vector<render::BonePose> poses;
            render::pose_model(model, render::EntityAnimation::Static, render::WalkState{},
                               poses);
            render::EntityPlacement placement;
            // Deliberately away from the origin and turned: a box computed at
            // (0, 0, 0) facing +Z hides both a translation bug and a swapped
            // axis, and this is the number the whole model is judged on.
            placement.position = Vec3f{100.5F, -60.0F, -37.25F};
            placement.body_yaw = 143.0F;

            Vec3f min;
            Vec3f max;
            render::entity_bounds(model, poses, placement, min, max);

            std::vector<render::EntityVertex> vertices;
            const u32 quads = render::emit_entity(model, poses, placement, vertices);

            // The box the *vertices* occupy, recomputed from what would be
            // uploaded. If this disagreed with entity_bounds, the bounds would
            // be describing a model the renderer does not draw.
            Vec3f vmin{1e9F, 1e9F, 1e9F};
            Vec3f vmax{-1e9F, -1e9F, -1e9F};
            for (const render::EntityVertex& vertex : vertices) {
                vmin = Vec3f{std::min(vmin.x, vertex.x), std::min(vmin.y, vertex.y),
                             std::min(vmin.z, vertex.z)};
                vmax = Vec3f{std::max(vmax.x, vertex.x), std::max(vmax.y, vertex.y),
                             std::max(vmax.z, vertex.z)};
            }
            const f32 drift = std::max({std::abs(vmin.x - min.x), std::abs(vmin.y - min.y),
                                        std::abs(vmin.z - min.z), std::abs(vmax.x - max.x),
                                        std::abs(vmax.y - max.y), std::abs(vmax.z - max.z)});

            std::string_view species;
            for (const std::string_view candidate :
                 {"minecraft:zombie", "minecraft:skeleton", "minecraft:creeper",
                  "minecraft:spider", "minecraft:cow", "minecraft:pig", "minecraft:sheep",
                  "minecraft:chicken"}) {
                if (render::entity_model_name(candidate) == model.name) {
                    species = candidate;
                }
            }
            f32 box_width  = 0.0F;
            f32 box_height = 0.0F;
            if (types && !species.empty() && registries != nullptr) {
                if (const auto id = registries->protocol_id(*types, species)) {
                    if (const auto info = registries->entity_type(*id)) {
                        box_width  = info->width;
                        box_height = info->height;
                    }
                }
            }

            // The width is taken facing south, not at the turned yaw above:
            // an axis-aligned box around a rotated model is bigger than the
            // model, and comparing *that* to a hitbox would flatter or damn it
            // by an angle nobody chose.
            render::EntityPlacement facing_south = placement;
            facing_south.body_yaw                = 0.0F;
            Vec3f south_min;
            Vec3f south_max;
            render::entity_bounds(model, poses, facing_south, south_min, south_max);
            const f32 width =
                std::max(south_max.x - south_min.x, south_max.z - south_min.z);

            fmt::print("{:<10} {:>4} {:>6} {:>8.3f} {:>8.3f} {:>8.3f} {:>8.3f} {:>8.3f}"
                       "   drift {:.6f}\n",
                       model.name, model.bones.size(), quads, width, box_width,
                       max.y - min.y, box_height, min.y - placement.position.y, drift);
        }
        return 0;
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

    // ── The creative parity check: no window, no device ─────────────────────
    //
    // Our search page, and the page our filter gives for each query the
    // oracle typed, against what the real 1.20.1 client showed. Item and
    // occurrence, in order: a list that has the right stacks in a different
    // order is a failure, because the order is what the player sees.
    if (options.creative_parity) {
        auto language = render::Language::load(source, options.language);
        auto tabs     = render::CreativeTabs::load(options.creative_tabs);
        if (!language || !tabs) {
            fmt::print("creative parity: language or catalogue missing\n");
            return 1;
        }
        auto items = render::CreativeItems::load(options.creative_items, *language);
        if (!items) {
            fmt::print("creative parity: {}: {}\n", options.creative_items,
                       render::to_string(items.error()));
            return 1;
        }
        const auto tags = render::load_item_tags(options.item_tags);
        client::CreativeScreen screen(*tabs, *language, &*items,
                                      client::CreativeScreenOptions{options.operator_tab});
        screen.set_item_tags(&tags);
        if (!screen.select("minecraft:search")) {
            fmt::print("creative parity: no search tab\n");
            return 1;
        }
        // The occurrence of each stack, from the unfiltered search page.
        std::map<std::string, u32> occurrence_of;
        {
            std::map<std::string, u32, std::less<>> seen;
            for (const client::CreativeCell& cell : screen.page()) {
                std::string key(cell.item);
                key.push_back('\0');
                key.append(reinterpret_cast<const char*>(cell.nbt.data()), cell.nbt.size());
                occurrence_of.emplace(key, seen[std::string(cell.item)]++);
            }
        }
        const auto listed = [&](std::span<const client::CreativeCell> page) {
            std::vector<std::pair<std::string, u32>> out;
            for (const client::CreativeCell& cell : page) {
                std::string key(cell.item);
                key.push_back('\0');
                key.append(reinterpret_cast<const char*>(cell.nbt.data()), cell.nbt.size());
                out.emplace_back(std::string(cell.item), occurrence_of[key]);
            }
            return out;
        };
        const auto ours = listed(screen.page());
        usize      same_prefix = 0;
        while (same_prefix < ours.size() && same_prefix < items->order().size() &&
               ours[same_prefix] == items->order()[same_prefix]) {
            ++same_prefix;
        }
        fmt::print("search page: ours {} stacks, client {}; identical in order: {}\n", ours.size(),
                   items->order().size(), ours == items->order() ? "yes" : "no");
        if (ours != items->order()) {
            fmt::print("  first difference at {}\n", same_prefix);
        }
        usize exact = 0;
        for (const render::CreativeQuery& query : items->queries()) {
            while (!screen.query().empty()) {
                screen.backspace();
            }
            screen.type(query.query);
            const auto mine = listed(screen.page());
            const bool same = mine == query.results;
            exact += same ? 1 : 0;
            fmt::print("  {:<18} client {:>5}  ours {:>5}  {}\n", fmt::format("\"{}\"", query.query),
                       query.results.size(), mine.size(), same ? "identical" : "DIFFERENT");
        }
        fmt::print("queries identical to the client's, in order: {} of {}\n", exact,
                   items->queries().size());
        return 0;
    }

    // Two ways to get a world. Reading it off the disk is a viewer; asking a
    // server for it is the game — and the second is the one the project's
    // second principle is about, because the bytes on that socket are the same
    // whether the server is across the world or on the next thread.
    if (options.singleplayer && options.connect.empty()) {
        options.connect = "127.0.0.1:" + std::to_string(options.singleplayer_port);
    }
    bool       online   = !options.connect.empty();  // ── screens ── a world can be joined later
    const bool streamed = online || menu_mode;         // ── screens ── so set up as for one

    std::optional<demo::LoadedWorld> world;
    if (!streamed) {
        const auto load_start = std::chrono::steady_clock::now();
        world = demo::load_world(options.world, *blocks, options.centre_x, options.centre_z,
                                 options.radius);
        if (!world) {
            return 1;
        }
        const auto load_ms =
            std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - load_start)
                .count();
        OV_LOG_INFO("world: {} chunks read, {} unfinished, {} failed, {:.0f} ms{}",
                    world->chunks_read, world->chunks_unfinished, world->chunks_failed, load_ms,
                    world->light_stored ? "" : " — WITHOUT stored light");
    }

    // Resolve every state the world uses before the atlas is stitched: the
    // atlas needs the full set of sprites, and the render layer needs the
    // atlas. Resolve, stitch, classify, mesh.
    render::BlockModelCache models(source, *blocks);
    if (streamed) {
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

    // Item models, resolved before the atlas is stitched for the same reason
    // the block models are: an item's sprite has to be in the sheet before a
    // hotbar can draw it, and there is exactly one sheet. A separate item atlas
    // would be a second texture to bind in the middle of the interface's one
    // batch.
    render::ItemModelCache item_models(source);
    if (registries != nullptr) {
        if (const auto items = registries->find("minecraft:item")) {
            for (const std::string_view name : registries->entries(*items)) {
                item_models.resolve(name);
            }
        }
    }
    OV_LOG_INFO("items: {} models resolved, {} sprites", item_models.resolved_count(),
                item_models.sprites().size());

    render::AtlasBuilder builder(source);
    for (const auto& sprite : models.sprites()) {
        builder.add(sprite);
    }
    for (const auto& sprite : item_models.sprites()) {
        builder.add(sprite);
    }
    // The survival page's empty-slot silhouettes. No model names them — the
    // game draws them straight from the block atlas — so they are added by
    // name, in the order the creative screen wants them.
    constexpr std::array<std::string_view, 5> kSlotIcons{
        "minecraft:item/empty_armor_slot_helmet", "minecraft:item/empty_armor_slot_chestplate",
        "minecraft:item/empty_armor_slot_leggings", "minecraft:item/empty_armor_slot_boots",
        "minecraft:item/empty_armor_slot_shield"};
    for (const std::string_view icon : kSlotIcons) {
        builder.add(icon);
    }
    auto atlas = builder.build();
    if (!atlas) {
        OV_LOG_ERROR("atlas: {}", render::to_string(atlas.error()));
        return 1;
    }
    models.classify_layers(*atlas);
    item_models.bake(*atlas);

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
    const auto& offline_chunks = streamed ? kNoChunks : world->chunks;

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

    if (meshes.empty() && !streamed) {
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
    if (streamed) {
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
        meshes.size() * static_cast<usize>(render::RenderLayer::Count), streamed ? 65536 : 1024));

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

    // ── Entities ────────────────────────────────────────────────────────────
    //
    // The geometry file is generated from Mojang data and gitignored, so a
    // checkout that has not run scripts/measure_entity_models.py has no models.
    // That case draws no entities and says so once, which is the difference
    // between "not built yet" and "broken".
    auto entity_renderer = client::EntityRenderer::create(device, device.swapchain_format(),
                                                          rhi::Format::Depth32Float);
    if (!entity_renderer) {
        OV_LOG_ERROR("entity renderer: {}", rhi::to_string(entity_renderer.error()));
        return 1;
    }

    std::optional<render::EntityModelSet>                    entity_models;
    std::unordered_map<std::string, client::EntityTexture>   entity_textures;
    if (options.entities) {
        auto loaded = render::EntityModelSet::load(options.entity_models);
        if (!loaded) {
            OV_LOG_WARN("no entity models ({}): {} — run "
                        "scripts/measure_entity_models.py --fetch",
                        options.entity_models, render::to_string(loaded.error()));
        } else {
            entity_models = std::move(*loaded);
            OV_LOG_INFO("entity models: {} from {}", entity_models->models().size(),
                        entity_models->source());
            for (const std::string& refusal : entity_models->refused()) {
                OV_LOG_WARN("entity geometry refused: {}", refusal);
            }
            // One texture per model, uploaded once. The sheep is two models
            // over one mob, and each carries its own sheet.
            const std::array<std::pair<std::string_view, std::string_view>, 10> kSkins{{
                {"humanoid", "minecraft:entity/player/wide/steve"},
                {"zombie", "minecraft:entity/zombie/zombie"},
                {"skeleton", "minecraft:entity/skeleton/skeleton"},
                {"creeper", "minecraft:entity/creeper/creeper"},
                {"spider", "minecraft:entity/spider/spider"},
                {"cow", "minecraft:entity/cow/cow"},
                {"pig", "minecraft:entity/pig/pig"},
                {"sheep", "minecraft:entity/sheep/sheep"},
                {"sheep_fur", "minecraft:entity/sheep/sheep_fur"},
                {"chicken", "minecraft:entity/chicken"},
            }};
            for (const auto& [model_name, texture_name] : kSkins) {
                if (entity_models->find(model_name) == nullptr) {
                    continue;
                }
                const auto location = ResourceLocation::parse(texture_name);
                if (!location) {
                    continue;
                }
                auto image = render::load_texture(source, *location);
                if (!image) {
                    OV_LOG_WARN("entity texture {} missing", texture_name);
                    continue;
                }
                auto uploaded = (*entity_renderer)->add_texture(*image, texture_name);
                if (!uploaded) {
                    OV_LOG_WARN("entity texture {}: {}", texture_name,
                                rhi::to_string(uploaded.error()));
                    continue;
                }
                entity_textures.emplace(std::string(model_name), *uploaded);
            }
        }
    }

    // ── weather ── Rain and snow: the entity vertices, blended, both sides.
    auto weather_renderer = client::EntityRenderer::create(
        device, device.swapchain_format(), rhi::Format::Depth32Float, client::EntityPass::Translucent);
    std::optional<client::EntityTexture> rain_texture;
    std::optional<client::EntityTexture> snow_texture;
    if (!weather_renderer) {
        OV_LOG_WARN("weather renderer: {} — no rain or snow will be drawn",
                    rhi::to_string(weather_renderer.error()));
    } else {
        for (const auto& [name, slot] :
             std::array<std::pair<std::string_view, std::optional<client::EntityTexture>*>, 2>{
                 {{"minecraft:environment/rain", &rain_texture},
                  {"minecraft:environment/snow", &snow_texture}}}) {
            const auto location = ResourceLocation::parse(name);
            if (!location) {
                continue;
            }
            auto image = render::load_texture(source, *location);
            if (!image) {
                OV_LOG_WARN("weather texture {} missing", name);
                continue;
            }
            if (auto uploaded = (*weather_renderer)->add_texture(*image, name)) {
                *slot = *uploaded;
            }
        }
    }

    auto overlay = client::Overlay::create(device, device.swapchain_format(),
                                           rhi::Format::Depth32Float);
    if (!overlay) {
        OV_LOG_ERROR("overlay: {}", rhi::to_string(overlay.error()));
        return 1;
    }

    // The interface: the HUD, the screens, and the mirror of the server's
    // windows. Built after the atlas image, because an item is drawn from the
    // same texture the terrain is.
    demo::InterfaceOptions interface_options;
    interface_options.hud       = options.hud;
    interface_options.gui_scale = options.gui_scale;
    interface_options.language  = options.language;
    interface_options.creative_tabs = options.creative_tabs;
    interface_options.creative_items = options.creative_items;
    interface_options.item_tags      = options.item_tags;
    interface_options.hotbar_file    = options.hotbar_file;
    interface_options.operator_tab   = options.operator_tab;
    for (usize i = 0; i < kSlotIcons.size(); ++i) {
        if (const render::AtlasSprite* sprite = atlas->find(kSlotIcons[i])) {
            interface_options.slot_icons[i] = sprite->uv;
        } else {
            OV_LOG_WARN("{} is not in the atlas; that slot is drawn bare", kSlotIcons[i]);
        }
    }

    // The tint a grass or leaf face takes in a GUI cell. Vanilla samples the
    // colormap at (0.5, 1.0) for an item, which is not any biome's point; this
    // is plains, which is a different green by a few levels. Named rather than
    // dressed up as the same thing.
    const u32 item_tint =
        blocks->biome_count() > 0 ? biome_colours->grass(0) : 0x91BD59U;

    auto interface = demo::Interface::create(device, device.swapchain_format(), source,
                                             item_models, *atlas_image, atlas->width(),
                                             atlas->height(), registries, item_tint,
                                             interface_options);
    if (!interface) {
        OV_LOG_ERROR("interface: {}", interface.error());
        return 1;
    }

    // ── screens ── the menus, with a GUI of their own, and the options.txt
    // they edit. The keys are rebound from it before the first frame.
    demo::MenusConfig menus_config;
    menus_config.saves              = options.saves;
    menus_config.options_file       = options.options_file;
    menus_config.assets_root        = options.assets;
    menus_config.language           = options.lang_given ? options.language : std::string{};
    menus_config.gui_scale_override = options.gui_scale_given ? options.gui_scale : 0U;
    {
        std::random_device entropy;
        menus_config.random_seed = static_cast<i64>((static_cast<u64>(entropy()) << 32U) ^
                                                    static_cast<u64>(entropy()));
    }
    auto menus_created =
        demo::Menus::create(device, device.swapchain_format(), source, menus_config);
    if (!menus_created) {
        OV_LOG_ERROR("menus: {}", menus_created.error());
        return 1;
    }
    std::unique_ptr<demo::Menus> menus = std::move(*menus_created);
    apply_key_bindings(menus->options(), **window);
    if (options.debug_overlay) {
        menus->toggle_debug();
    }
    if (menu_mode) {
        menus->open(demo::MenuScreen::Title);
    }
    // ── end screens ──

    // The block atlas, referenced rather than uploaded again: a dropped stack's
    // sprites were stitched into it with everything else.
    const client::EntityTexture atlas_entity_texture =
        (*entity_renderer)->borrow_texture(*atlas_image, atlas->width(), atlas->height());

    // ── The server ──────────────────────────────────────────────────────────
    std::atomic<bool> stop_server{false};
    std::atomic<bool> pause_server{false};  // ── screens ── the pause menu of a hosted world
    bool              integrated = false;   // ── screens ── the world is hosted here
    std::thread       server_thread;
    // ── screens ── a hosted server, now or later from the world list. `world`
    // empty is the server's own default (run/world), which --singleplayer
    // always used.
    const auto start_integrated = [&](const std::filesystem::path& world_dir) {
        stop_server  = false;
        pause_server = false;
        std::vector<std::string> arguments{"ov_voxel",
                                           "--port=" + std::to_string(options.singleplayer_port),
                                           // The host's name, so their record also goes into
                                           // level.dat's Data.Player — where vanilla looks for
                                           // a singleplayer world's player.
                                           "--host-player=" + options.username};
        if (!world_dir.empty()) {
            arguments.push_back("--world=" + world_dir.string());
        }
        server_thread = std::thread([&stop_server, &pause_server, arguments]() {
            // Argv-shaped because that is the server's own interface, and
            // giving it a second one would leave two ways to configure the
            // same thing.
            std::vector<char*> argv_copy;
            for (const std::string& argument : arguments) {
                argv_copy.push_back(const_cast<char*>(argument.c_str()));
            }
            (void)ov::server::run(static_cast<int>(argv_copy.size()), argv_copy.data(),
                                  &stop_server, &pause_server);
        });
        integrated = true;
    };
    if (options.singleplayer) {
        start_integrated({});
    }
    // ── end screens ──

    std::unique_ptr<netclient::Client> client;
    std::unique_ptr<demo::Session>     session;
    // ── sound ── in this order: the director points into the engine, the
    // engine into the catalogue, and destruction runs the other way.
    std::optional<audio::SoundCatalog>     sound_catalog;
    std::unique_ptr<audio::SoundEngine>    sound_engine;
    std::unique_ptr<client::SoundDirector> sound_director;
    std::optional<BlockPos>                pending_place;
    i32                                    pending_place_frames = 0;
    std::optional<std::pair<BlockPos, registry::BlockStateId>> pending_use;
    i32                                    pending_use_frames = 0;
    f64                                    fall_peak          = 0.0;
    std::vector<f64>                       audio_frame_ms;
    netclient::ClientEvents            events;
    gameplay::MotionState              player;
    const gameplay::MotionConstants    motion;
    bool                               spawned = false;
    // ── flight ──
    // What the server granted in Player Abilities, and what the player chose
    // with it. `jump_window` is vanilla's double-tap window, counted down in
    // ticks: a second press of jump while it is open toggles flight. Its
    // length, 7 ticks, is not in any document this project may read and has
    // not been measured — it is named here so a measurement has a place to go.
    netclient::ClientEvents::Abilities abilities;
    bool                               flying       = false;
    bool                               jump_was_held = false;
    i32                                jump_window  = 0;
    constexpr i32                      kJumpWindowTicks = 7;

    // ── loading: kept past the first connection, to knock again while the
    // server prepares its spawn area ──
    netclient::ClientDesc login;
    // ── screens ── a connection, opened at start or later from the menus
    const auto open_connection = [&](const std::string& address, int attempts) -> bool {
        std::string host = address;
        u16         port = 25565;
        if (const auto colon = host.rfind(':'); colon != std::string::npos) {
            port = static_cast<u16>(std::atoi(host.substr(colon + 1).c_str()));
            host = host.substr(0, colon);
        }

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
        for (int attempt = 0; attempt < attempts; ++attempt) {
            connected = netclient::Client::connect(login);
            if (connected) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        if (!connected) {
            OV_LOG_ERROR("{}:{} — {}", host, port, netclient::to_string(connected.error()));
            return false;
        }
        client  = std::move(*connected);
        session = std::make_unique<demo::Session>(*blocks, models, *atlas, tints, **terrain);
        OV_LOG_INFO("connected to {}:{} as {}", host, port, options.username);
        return true;
    };
    if (online && !open_connection(options.connect, options.singleplayer ? 200 : 1)) {
        return 1;
    }
    {  // ── screens ── the sound starts whether or not a world is joined yet

        // ── sound ── sounds.json, the device, and what decides what is heard.
        if (options.sound && registries != nullptr) {
            const std::filesystem::path root{options.assets};
            std::ifstream               in(root / "assets/minecraft/sounds.json", std::ios::binary);
            if (!in) {
                OV_LOG_WARN("sound: no sounds.json under {} — run ov-assetimport --sounds; "
                            "the game stays silent",
                            options.assets);
            } else {
                const std::string text((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());
                if (auto parsed = audio::SoundCatalog::parse(text); !parsed) {
                    OV_LOG_WARN("sound: {}", parsed.error());
                } else {
                    sound_catalog.emplace(std::move(*parsed));
                    audio::EngineDesc sound_desc;
                    sound_desc.catalog  = &*sound_catalog;
                    sound_desc.keep_log = options.sound_log;
                    sound_desc.read = [root](std::string_view path) -> std::optional<std::vector<u8>> {
                        std::ifstream file(root / path, std::ios::binary);
                        if (!file) {
                            return std::nullopt;
                        }
                        return std::vector<u8>(std::istreambuf_iterator<char>(file),
                                               std::istreambuf_iterator<char>());
                    };
                    auto engine = audio::SoundEngine::create(sound_desc);
                    if (!engine) {
                        // No device is not a reason to lose the rest: the null
                        // backend still decides, logs and counts every sound.
                        OV_LOG_WARN("sound: {} — continuing without output", engine.error());
                        sound_desc.backend = audio::Backend::Null;
                        engine             = audio::SoundEngine::create(sound_desc);
                    }
                    if (engine) {
                        sound_engine = std::move(*engine);
                        // `master:0.8,music:0`: one pair at a time, a name that
                        // is not a category said so rather than ignored.
                        usize start = 0;
                        while (start < options.volumes.size()) {
                            const usize end   = std::min(options.volumes.find(',', start),
                                                         options.volumes.size());
                            const auto  pair  = options.volumes.substr(start, end - start);
                            const usize colon = pair.find(':');
                            const auto  which = audio::category_from_name(pair.substr(0, colon));
                            if (colon == std::string::npos || !which) {
                                OV_LOG_WARN("--volume: '{}' is not category:value", pair);
                            } else {
                                sound_engine->set_volume(
                                    *which,
                                    static_cast<f32>(std::atof(pair.substr(colon + 1).c_str())));
                            }
                            start = end + 1;
                        }
                        sound_director = std::make_unique<client::SoundDirector>(
                            *sound_engine, *blocks, *registries, i64{0x5EED});
                        OV_LOG_INFO("sound: {} events, {} variants", sound_catalog->event_count(),
                                    sound_catalog->entry_count());
                    }
                }
            }
        }
        // ── end sound ──
    }
    // ── screens ── options.txt's volumes, where --volume said nothing
    if (sound_engine && options.volumes.empty()) {
        for (usize i = 0; i < client::kSoundCategoryNames.size(); ++i) {
            if (const auto which = audio::category_from_name(client::kSoundCategoryNames[i])) {
                sound_engine->set_volume(*which, static_cast<f32>(menus->options().volumes[i]));
            }
        }
    }

    render::Camera camera;
    camera.position = world ? world->suggested_camera(*blocks) : Vec3f{0.0F, 0.0F, 0.0F};
    camera.yaw_degrees   = -45.0F;
    camera.pitch_degrees = 20.0F;
    camera.far_plane     = static_cast<f32>(options.radius + 2) * 16.0F * 1.8F;
    camera.vertical_fov_degrees = static_cast<f32>(menus->options().fov);  // ── screens ──

    // The render distance in blocks, which is what the fog is measured against.
    // Vanilla's terrain fog starts at 92 % of it and is complete at 100 %.
    f32 render_distance = static_cast<f32>(options.radius) * 16.0F;  // ── screens ── not const

    // The biome under the camera decides the fog and the sky. One biome for the
    // whole frame, which is what vanilla does too — it samples where you are,
    // not where you are looking.
    render::Lightmap lightmap;
    i64              time_of_day = options.time;
    // ── weather ── What the server's Game Events 7 and 8 said, and the flash of
    // the last bolt. The client's thunder level is the thunder level times the
    // rain level, as the game's is.
    f32                                   rain_level    = 0.0F;
    f32                                   thunder_level = 0.0F;
    std::chrono::steady_clock::time_point flash_until{};
    std::unordered_set<i32>               bolts_seen;
    const gameplay::ClimateNoise          weather_climate;
    std::vector<render::WeatherColumn>    weather_columns;
    render::WeatherMesh                   weather_mesh;
    std::vector<f64>                      weather_record_ms;

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

    (*window)->set_cursor_captured(options.frames == 0 && !menu_mode);  // ── screens ──

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

    // The scripted half of the round trip: right-click a block, then make two
    // clicks in the window that opens. Every one of them goes through the same
    // path a hand on the mouse takes, so what it proves is what a player gets.
    const auto parse_block = [](const std::string& text) {
        std::array<i32, 3> parts{0, 0, 0};
        usize              index = 0;
        usize              start = 0;
        while (index < parts.size() && start <= text.size()) {
            const auto end = text.find(',', start);
            parts[index++] = std::atoi(
                text.substr(start, end == std::string::npos ? std::string::npos : end - start)
                    .c_str());
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
        return BlockPos{parts[0], parts[1], parts[2]};
    };

    BlockPos use_target{};
    bool     use_wanted = false;
    if (!options.use_block.empty()) {
        use_target = parse_block(options.use_block);
        use_wanted = true;
    }
    BlockPos place_target_scripted{};
    bool     place_wanted = false;
    if (!options.place_at.empty()) {
        place_target_scripted = parse_block(options.place_at);
        place_wanted          = true;
    }
    bool scripted_place_sent = false;
    Vec3d stand_position{};
    f32   stand_yaw   = 0.0F;
    f32   stand_pitch = 0.0F;
    bool  stand_wanted = false;
    if (!options.stand_at.empty()) {
        std::array<f64, 5> parts{0.0, 0.0, 0.0, 0.0, 0.0};
        usize              index = 0;
        usize              start = 0;
        while (index < parts.size() && start <= options.stand_at.size()) {
            const auto end = options.stand_at.find(',', start);
            parts[index++] = std::atof(
                options.stand_at
                    .substr(start, end == std::string::npos ? std::string::npos : end - start)
                    .c_str());
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
        stand_position = Vec3d{parts[0], parts[1], parts[2]};
        stand_yaw      = static_cast<f32>(parts[3]);
        stand_pitch    = static_cast<f32>(parts[4]);
        stand_wanted   = true;
    }
    bool stand_done = false;
    std::string window_dump;

    bool use_sent   = false;
    i16  move_from  = -1;
    i16  move_to    = -1;
    if (!options.move_slots.empty()) {
        const auto comma = options.move_slots.find(',');
        move_from = static_cast<i16>(std::atoi(options.move_slots.substr(0, comma).c_str()));
        if (comma != std::string::npos) {
            move_to = static_cast<i16>(std::atoi(options.move_slots.substr(comma + 1).c_str()));
        }
    }
    std::vector<i16> drag_sequence;
    if (!options.drag_slots.empty()) {
        usize start = 0;
        while (start <= options.drag_slots.size()) {
            const auto end = options.drag_slots.find(',', start);
            drag_sequence.push_back(static_cast<i16>(std::atoi(
                options.drag_slots
                    .substr(start, end == std::string::npos ? std::string::npos : end - start)
                    .c_str())));
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
    }
    bool drag_pick_sent = false;
    bool drag_sent      = false;

    bool move_pick_sent  = false;
    bool move_place_sent = false;
    bool inventory_opened = false;
    bool creative_opened  = false;
    bool creative_taken   = false;
    std::string creative_dump;
    usize       chat_sent     = 0;      // ── chat ──
    bool        chat_opened   = false;  // ── chat ──
    u32         chat_ready_at = 0;      // ── chat ──  the frame the command tree arrived

    std::optional<RayHit> aimed;
    bool                  dig_sent   = false;
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
    // ── perf ── the two frame costs that grow with what the server sends: the
    // network's chunks and entities folded into the world, and the meshing.
    std::vector<f64> apply_frame_ms;
    std::vector<f64> mesh_frame_ms;

    // The entity pass, measured separately from the rest of the recording, so
    // that "what do the mobs cost" has an answer that is not a subtraction of
    // two runs.
    demo::EntityWorld                 entity_world;
    demo::EntityScratch               entity_scratch;
    std::vector<f64>                  entity_record_ms;
    std::vector<f64>                  entity_steps;
    std::unordered_map<i32, Vec3f>    last_drawn;
    u32              drawn_last_frame = 0;
    u32              rendered         = 0;
    bool             running          = true;
    // ── loading ──
    // The line vanilla's loading screen would show this frame, or empty.
    std::string loading_line;
    auto        next_knock = std::chrono::steady_clock::now();
    bool             captured         = false;

    // ── screens ─────────────────────────────────────────────────────────────
    // Leaving a world and joining one from the menus. A world left is left
    // entirely: the connection, the level, the terrain arena, the entities,
    // the interface's mirror of the server — and the hosted server, whose
    // stop saves the world. What is joined next starts from nothing.
    bool        hardcore         = false;
    i32         experience_total = 0;
    std::string pending_address;       // connect once the message frame is drawn
    bool        pending_integrated = false;
    bool        pending_leave      = false;
    i32         pending_frames     = 0;
    usize       menu_pressed       = 0;
    u32         next_press_at      = 0;
    f64         paused_seconds     = 0.0;  // the pause, measured: see game_paused
    f64         playing_seconds    = 0.0;
    const auto  leave_world = [&]() {
        device.wait_idle();
        client.reset();
        session.reset();
        if (server_thread.joinable()) {
            stop_server = true;
            server_thread.join();
        }
        stop_server  = false;
        pause_server = false;
        integrated   = false;
        online       = false;
        terrain      = client::TerrainRenderer::create(device, terrain_desc);
        if (!terrain) {
            OV_LOG_ERROR("terrain renderer: {}", rhi::to_string(terrain.error()));
            running = false;
            return;
        }
        entity_world = demo::EntityWorld{};
        last_drawn.clear();
        events.clear();
        spawned     = false;
        player      = gameplay::MotionState{};
        flying      = false;
        abilities   = netclient::ClientEvents::Abilities{};
        jump_window = 0;
        hardcore    = false;
        interface_options.gui_scale =
            options.gui_scale_given ? options.gui_scale
                                    : static_cast<u32>(menus->options().gui_scale);
        interface_options.language = menus->options().language;
        interface = demo::Interface::create(device, device.swapchain_format(), source, item_models,
                                            *atlas_image, atlas->width(), atlas->height(),
                                            registries, item_tint, interface_options);
        if (!interface) {
            OV_LOG_ERROR("interface: {}", interface.error());
            running = false;
            return;
        }
        menus->set_in_game(false, false);
        (*window)->set_cursor_captured(false);
    };
    const auto handle_menu_action = [&](const demo::MenuAction& action) {
        switch (action.kind) {
            case demo::MenuAction::Kind::None: break;
            case demo::MenuAction::Kind::Resume: (*window)->set_cursor_captured(true); break;
            case demo::MenuAction::Kind::Quit: running = false; break;
            case demo::MenuAction::Kind::Respawn:
                if (client) {
                    client->send_respawn();
                    OV_LOG_INFO("death screen: Respawn pressed, Client Command sent");
                }
                break;
            case demo::MenuAction::Kind::Leave:
                menus->show_message(
                    menus->translate(integrated ? "menu.savingLevel" : "connect.aborted"));
                pending_leave  = true;
                pending_frames = 2;
                break;
            case demo::MenuAction::Kind::Connect:
                menus->show_message(menus->translate("connect.connecting"));
                pending_address    = action.address;
                pending_integrated = false;
                pending_frames     = 2;
                break;
            case demo::MenuAction::Kind::Play: {
                if (action.create) {
                    // level.dat first: the name, the game mode, and — for a
                    // seeded world — the generator and the seed, which the
                    // server reads back and generates from. Reopened later, the
                    // same file makes it the same world.
                    world::LevelSettings settings;
                    settings.name      = action.world_name;
                    settings.game_type = action.survival ? 0 : 1;
                    settings.generated = action.seed.has_value();
                    settings.seed      = action.seed.value_or(0);
                    std::error_code error;
                    std::filesystem::create_directories(action.world, error);
                    if (!io::write_file_atomic(action.world / "level.dat",
                                               world::encode_level_dat(settings))) {
                        OV_LOG_ERROR("could not write {}", (action.world / "level.dat").string());
                        break;
                    }
                    OV_LOG_INFO("new world \"{}\" in {}{}", action.world_name,
                                action.world.string(),
                                action.seed ? fmt::format(", seed {}", *action.seed)
                                            : std::string(", superflat"));
                }
                start_integrated(action.world);
                menus->show_message(menus->translate("menu.loadingLevel"));
                pending_address    = "127.0.0.1:" + std::to_string(options.singleplayer_port);
                pending_integrated = true;
                pending_frames     = 2;
                break;
            }
        }
    };
    // ── end screens ─────────────────────────────────────────────────────────

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

        // The interface gets the input first. When a screen is open it takes
        // all of it: a click that moves a stack must not also break the block
        // behind the window, and the camera must not turn while the pointer is
        // choosing a slot.
        const auto  now_for_ui = std::chrono::steady_clock::now();
        static auto last_ui    = now_for_ui;
        const f64   ui_delta   = std::chrono::duration<f64>(now_for_ui - last_ui).count();
        last_ui                = now_for_ui;

        bool ui_took_input = false;
        // ── screens ── the menus first: while one is up it takes every key and
        // click. Escape over the game — no screen, no chat — is the pause menu.
        const bool game_screen_up =
            online && ((*interface)->screen_open() || (*interface)->chat_open() ||
                       (*interface)->creative_open());
        // A scripted capture holds the pointer in the corner, off every
        // button, as the vanilla oracle does before each of its captures.
        const bool scripted_menus = !options.menu.empty() || !options.menu_press.empty();
        client::InputState scripted_input;
        if (scripted_menus) {
            scripted_input.mouse_x = 3.0;
            scripted_input.mouse_y = 3.0;
        }
        const demo::MenuAction menu_action =
            menus->update(scripted_menus ? scripted_input : input, **window, ui_delta);
        if (online && !menus->any_open() && !game_screen_up &&
            input.just_pressed(client::Key::Escape) &&
            menu_action.kind == demo::MenuAction::Kind::None) {
            menus->open(demo::MenuScreen::Pause);
        }
        handle_menu_action(menu_action);
        if (!options.menu.empty() && rendered == options.menu_at) {  // scripted captures
            for (u8 index = 0; index <= static_cast<u8>(demo::MenuScreen::Message); ++index) {
                const auto screen = static_cast<demo::MenuScreen>(index);
                if (demo::to_string(screen) == options.menu) {
                    if (screen == demo::MenuScreen::Death) {
                        menus->show_death(R"({"translate":"death.attack.generic","with":["OndesVoxel"]})",
                                          0, false);
                    } else {
                        menus->open(screen);
                    }
                }
            }
        }
        if (!options.menu_type.empty() && rendered == options.menu_at + 15) {
            menus->type(options.menu_type);
        }
        // The press script: `id` presses a widget, `@screen` opens a screen,
        // `+N` waits N frames, `?screen` waits for that screen (`?game`: in
        // the world, spawned, nothing open), `type:text` types.
        if (menu_pressed < options.menu_press.size() && rendered >= options.menu_at &&
            rendered >= next_press_at) {
            const std::string& step = options.menu_press[menu_pressed];
            bool               done = true;
            if (step.starts_with('+')) {
                next_press_at = rendered + static_cast<u32>(std::atoi(step.c_str() + 1));
                done          = false;
                ++menu_pressed;
            } else if (step == "?game") {
                done = online && spawned && !menus->any_open();
                if (done) {
                    OV_LOG_INFO("menu script: in the game at frame {}", rendered);
                }
            } else if (step.starts_with('?')) {
                done = demo::to_string(menus->screen()) == std::string_view(step).substr(1);
            } else if (step.starts_with("type:")) {
                menus->type(std::string_view(step).substr(5));
            } else if (step.starts_with("world:")) {
                menus->select_world(std::atoi(step.c_str() + 6));
            } else if (step == "end") {
                options.frames = rendered + 2;  // this frame's successor is captured
            } else if (step.starts_with('@')) {
                for (u8 index = 0; index <= static_cast<u8>(demo::MenuScreen::Message); ++index) {
                    const auto screen = static_cast<demo::MenuScreen>(index);
                    if (demo::to_string(screen) == std::string_view(step).substr(1)) {
                        menus->open(screen);
                    }
                }
            } else {
                handle_menu_action(menus->press(step));
            }
            if (done) {
                ++menu_pressed;
                next_press_at = std::max(next_press_at, rendered + 30);
            }
        }
        if (pending_frames > 0 && --pending_frames == 0) {
            if (pending_leave) {
                pending_leave = false;
                leave_world();
                menus->open(demo::MenuScreen::Title);
            } else if (!pending_address.empty()) {
                if (open_connection(pending_address, pending_integrated ? 400 : 1)) {
                    online = true;
                    menus->set_in_game(true, pending_integrated);
                    menus->show_message("");
                    (*window)->set_cursor_captured(options.frames == 0);
                } else {
                    leave_world();
                    menus->open(demo::MenuScreen::Title);
                }
                pending_address.clear();
            }
        }
        if (menus->take_options_changed()) {
            const client::GameOptions& game = menus->options();
            camera.vertical_fov_degrees     = static_cast<f32>(game.fov);
            apply_key_bindings(game, **window);
            if (!options.gui_scale_given) {
                (*interface)->set_gui_scale(static_cast<u32>(game.gui_scale));
            }
            if (sound_engine) {
                for (usize i = 0; i < client::kSoundCategoryNames.size(); ++i) {
                    if (const auto which =
                            audio::category_from_name(client::kSoundCategoryNames[i])) {
                        sound_engine->set_volume(*which, static_cast<f32>(game.volumes[i]));
                    }
                }
            }
            // The fog follows at once; the server is told at the next join
            // (Client Information is sent with the login only).
            options.radius   = game.render_distance;
            render_distance  = static_cast<f32>(options.radius) * 16.0F;
            camera.far_plane = static_cast<f32>(options.radius + 2) * 16.0F * 1.8F;
        }
        const bool game_paused = online && integrated && menus->any_open() &&
                                 menus->screen() != demo::MenuScreen::Death &&
                                 menus->screen() != demo::MenuScreen::Message;
        pause_server.store(game_paused, std::memory_order_relaxed);
        // The measure of the pause: the seconds spent paused and playing,
        // printed at the end of the run and set against the server's own
        // tick count (docs/provenance/ecrans.md).
        if (online && spawned) {
            (game_paused ? paused_seconds : playing_seconds) += ui_delta;
        }
        ui_took_input = menus->any_open();
        // ── end screens ──
        if (online && !ui_took_input) {
            ui_took_input = (*interface)->update(input, *client, **window, ui_delta);
        }

        if (!ui_took_input && online && !(*interface)->screen_open()) {
            camera.turn(static_cast<f32>(input.mouse_delta_x),
                        static_cast<f32>(input.mouse_delta_y),
                        client::degrees_per_pixel(menus->options().mouse_sensitivity));
        } else if (!ui_took_input && !online && !menus->any_open()) {
            camera.turn(static_cast<f32>(input.mouse_delta_x),
                        static_cast<f32>(input.mouse_delta_y));
        }

        // ── loading ──
        // A server still preparing its spawn area refuses a login with
        // `menu.preparingSpawn` and how far along it is. That is not a reason
        // to quit: stay on the loading screen and knock again once a second,
        // as a player would press Join again. Every other disconnect still
        // ends the client.
        loading_line.clear();
        if (online && !client->connected()) {
            const auto why       = client->disconnect_reason();
            const auto preparing = why.find("menu.preparingSpawn");
            if (preparing == std::string::npos) {
                OV_LOG_ERROR("disconnected: {}", why.empty() ? "the server went away" : why);
                if (menu_mode) {  // ── screens ── back to the title, not out of the game
                    leave_world();
                    menus->open(demo::MenuScreen::Title);
                } else {
                    running = false;
                }
                continue;
            }
            std::string percent = "0";
            if (const auto open = why.find("[\"", preparing); open != std::string::npos) {
                if (const auto close = why.find('"', open + 2); close != std::string::npos) {
                    percent = why.substr(open + 2, close - open - 2);
                }
            }
            const std::array<std::string, 1> arguments{percent};
            loading_line = render::format_translation(
                (*interface)->translate("menu.preparingSpawn"), arguments);
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_knock) {
                next_knock = now + std::chrono::seconds{1};
                if (auto again = netclient::Client::connect(login); again) {
                    client = std::move(*again);
                    events.clear();
                }
            }
        }
        if (online && client->connected()) {

            client->poll(events);
            // The interface first: Login (play) carries the game mode, and the
            // creative-slot branch below reads it. Applying it after would make
            // the first frame's decision on a default rather than on what the
            // server said.
            (*interface)->apply(events);
            // ── screens ── death is Combat Death's arrival; Respawn ends it
            if (events.hardcore) {
                hardcore = *events.hardcore;
            }
            if (events.experience) {
                experience_total = events.experience->total;
            }
            if (events.death_message) {
                OV_LOG_INFO("death screen: Combat Death received at frame {} ({})", rendered,
                            *events.death_message);
                menus->show_death(*events.death_message, experience_total, hardcore);
                (*window)->set_cursor_captured(false);
            }
            if (events.respawned) {
                OV_LOG_INFO("death screen: Respawn received at frame {}{}", rendered,
                            menus->screen() == demo::MenuScreen::Death ? ", screen closed" : "");
            }
            if (events.respawned && menus->screen() == demo::MenuScreen::Death) {
                menus->open(demo::MenuScreen::None);
                (*window)->set_cursor_captured(options.frames == 0);
            }
            // ── end screens ──
            if (events.abilities) {  // ── flight ──
                // The server grants and withdraws; it also says whether the
                // player is flying — a spectator always is, and a creative
                // player set down by a game-mode change is not.
                abilities = *events.abilities;
                flying    = abilities.flying && abilities.may_fly;
            }
            if (events.teleport && !spawned && !options.hold.empty()) {  // ── screens ──
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
                    client->send_creative_slot(36, *item, 64);
                    client->send_held_slot(0);
                    // The server accepts a creative slot and sends nothing
                    // back — it is client-authoritative by design, in vanilla
                    // too — so this is the one place the mirror is written
                    // from this side rather than from a packet.
                    //
                    // Only in creative. A survival server *ignores* the packet,
                    // and mirroring it anyway would draw an item in the hotbar
                    // that the player does not have — a HUD lying about the
                    // server, which is the one thing it must never do. The
                    // first survival screenshot showed exactly that.
                    if ((*interface)->hud().creative) {
                        (*interface)->note_creative(36, *item, 64);
                        OV_LOG_INFO("holding {} (item {})", options.hold, *item);
                    } else {
                        OV_LOG_INFO(
                            "survival: the server owns the inventory, so --hold does nothing");
                    }
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
                if (stand_wanted && !stand_done) {
                    player.position      = stand_position;
                    camera.yaw_degrees   = stand_yaw;
                    camera.pitch_degrees = stand_pitch;
                    stand_done           = true;
                }
            }
            if (events.time_of_day) {
                // The world's clock comes from the server, so the sun is in the
                // same place for everyone standing in it.
                options.time         = *events.time_of_day;
                start_time           = std::chrono::steady_clock::now();
                options.daylight_cycle = true;
            }
            // ── sound ── before the entity world forgets what was picked up.
            // Only the audio work is timed: this segment and the listener's
            // below. A first version timed from here to the engine update and
            // measured the meshing and the physics in between — 4.8 ms of
            // "audio" that was the frame.
            const auto audio_started = std::chrono::steady_clock::now();
            f64        audio_ms      = 0.0;
            if (sound_director) {
                sound_director->on_events(
                    events, [&entity_world](i32 id) -> std::optional<client::HeardEntity> {
                        const auto& known = entity_world.entities();
                        const auto  it    = known.find(id);
                        if (it == known.end()) {
                            return std::nullopt;
                        }
                        return client::HeardEntity{
                            it->second.to,
                            it->second.orb ? netclient::ClientEvents::kSpawnedAsExperienceOrb : 0};
                    });
                // The server leaves this player out of its own placing and of the
                // door it opened: the answer it does send is the Block Update, so
                // that is when this client plays them.
                for (const auto& change : events.changed) {
                    const BlockPos at{change.x, change.y, change.z};
                    // The first answer about that cell closes it: a block is the
                    // placement accepted, air is the refusal the server sends
                    // back — heard as nothing.
                    if (pending_place && at == *pending_place) {
                        if (change.state != registry::kAirState) {
                            OV_LOG_INFO("sound: placed ({}, {}, {}) confirmed", at.x, at.y, at.z);
                            sound_director->placed(change.state, at);
                        }
                        pending_place.reset();
                    }
                    if (pending_use && at == pending_use->first) {
                        sound_director->toggled(pending_use->second, change.state, at);
                        pending_use.reset();
                    }
                }
                // The answer closes a pending gesture, not a clock. A window of
                // 60 and then 240 frames — a fifth of a second, then 1.65 s at
                // 145 fps — both closed before a Debug server busy generating
                // chunks had answered, and the placement was heard as nothing.
                // What is left is a safety net for an answer that never comes.
                if (pending_place && ++pending_place_frames > 4000) {
                    OV_LOG_INFO("sound: no Block Update for the place at ({}, {}, {})",
                                pending_place->x, pending_place->y, pending_place->z);
                    pending_place.reset();  // refused: nothing to hear
                }
                if (pending_use && ++pending_use_frames > 4000) {
                    pending_use.reset();
                }
                audio_ms += std::chrono::duration<f64, std::milli>(
                                std::chrono::steady_clock::now() - audio_started)
                                .count();
            }
            // ── end sound ──
            // ── weather ──
            if (events.rain_level) {
                rain_level = *events.rain_level;
            }
            if (events.thunder_level) {
                thunder_level = *events.thunder_level;
            }
            const auto perf_apply = std::chrono::steady_clock::now();  // ── perf ──
            session->apply(events);
            entity_world.apply(events, registries);
            const auto perf_mesh = std::chrono::steady_clock::now();  // ── perf ──
            apply_frame_ms.push_back(
                std::chrono::duration<f64, std::milli>(perf_mesh - perf_apply).count());
            // ── weather ── A bolt the client has not seen yet lights the sky
            // for two ticks. The bolt itself is not drawn — named.
            for (const auto& [id, entity] : entity_world.entities()) {
                if (entity.type == "minecraft:lightning_bolt" && bolts_seen.insert(id).second) {
                    flash_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
                }
            }

            // A budget, not a queue drain. A hundred chunks arriving at once
            // must cost several frames rather than one long one; the frame
            // percentile is what the milestone is judged on and a spike passes
            // a test of the mean.
            // Nearest first, from where the camera is: Session::mesh_pending.
            (void)session->mesh_pending(4.0, Vec3d{static_cast<f64>(camera.position.x),
                                                   static_cast<f64>(camera.position.y),
                                                   static_cast<f64>(camera.position.z)});
            mesh_frame_ms.push_back(std::chrono::duration<f64, std::milli>(  // ── perf ──
                                        std::chrono::steady_clock::now() - perf_mesh)
                                        .count());

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
            if (game_paused) {  // ── screens ── this client does not tick behind a pause either
                tick_accumulator = 0.0;
            }

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
            if (!ground_ready) {  // ── loading ──
                loading_line = (*interface)->translate("multiplayer.downloadingTerrain");
            }
            if (spawned && !ground_ready) {
                tick_accumulator = 0.0;
                // ...but the server still has to be told where we are, or a
                // --stand-at run deadlocks: no tick means no position report,
                // no position report means the server keeps sending the chunks
                // around the *spawn*, and the ground under the player never
                // arrives so the tick never runs. Reporting outside the tick
                // costs one packet a frame while the world catches up.
                netclient::PlayerInput settling;
                settling.position  = player.position;
                settling.yaw       = camera.yaw_degrees;
                settling.pitch     = camera.pitch_degrees;
                settling.on_ground = false;
                client->send_position(settling);
            }

            while (ground_ready && tick_accumulator >= kTickSeconds) {
                tick_accumulator -= kTickSeconds;

                gameplay::MoveInput move;
                move.forward = (input.held(client::Key::Forward) || options.walk ? 1.0F : 0.0F) -
                               (input.held(client::Key::Back) ? 1.0F : 0.0F);
                // Positive is LEFT, as vanilla's `xxa`: see MoveInput::strafe.
                move.strafe = (input.held(client::Key::Left) ? 1.0F : 0.0F) -
                              (input.held(client::Key::Right) ? 1.0F : 0.0F);
                move.yaw    = camera.yaw_degrees;
                move.jump   = input.held(client::Key::Up);
                move.sprint = input.held(client::Key::Sprint);
                move.sneak  = input.held(client::Key::Down);
                // ── chat ──  With the box open the keys are letters: vanilla
                // reads no key binding while a screen is up, so "d" types and
                // does not strafe.
                if ((*interface)->chat_open()) {
                    move.forward = 0.0F;
                    move.strafe  = 0.0F;
                    move.jump    = false;
                    move.sprint  = false;
                    move.sneak   = false;
                }

                // ── flight ──
                // A second press of jump inside the window toggles flight, for
                // a player the server lets fly. An edge and not a level: holding
                // jump climbs, it does not flicker in and out of the air.
                const bool jump_held = move.jump;
                if (jump_window > 0) {
                    --jump_window;
                }
                if (abilities.may_fly && jump_held && !jump_was_held) {
                    if (jump_window > 0) {
                        flying      = !flying;
                        jump_window = 0;
                        client->send_abilities(flying);
                    } else {
                        jump_window = kJumpWindowTicks;
                    }
                }
                jump_was_held     = jump_held;
                move.flying       = flying;
                move.flying_speed = abilities.flying_speed;

                const auto world_view = session->collision();
                const auto fluid_view = session->fluids();
                const Vec3d before_step   = player.position;  // ── sound ──
                const bool  was_on_ground = player.on_ground;
                player = gameplay::step(player, move, motion, world_view, &fluid_view);
                if (sound_director) {  // ── sound ── footsteps, a landing, the music
                    const auto under = session->block_at(
                        static_cast<i32>(std::floor(player.position.x)),
                        static_cast<i32>(std::floor(player.position.y - 0.2)),
                        static_cast<i32>(std::floor(player.position.z)));
                    const f64 dx = player.position.x - before_step.x;
                    const f64 dz = player.position.z - before_step.z;
                    if (!move.sneak) {
                        sound_director->walked(std::sqrt(dx * dx + dz * dz), player.on_ground,
                                               under, player.position);
                    }
                    if (was_on_ground && !player.on_ground) {
                        fall_peak = player.position.y;
                    }
                    if (!player.on_ground) {
                        fall_peak = std::max(fall_peak, player.position.y);
                    }
                    if (!was_on_ground && player.on_ground && !(*interface)->hud().creative) {
                        // Survival's rule, measured: ceil(distance - 3).
                        const f64 fallen = fall_peak - player.position.y;
                        if (fallen > 3.0) {
                            sound_director->landed(static_cast<f32>(std::ceil(fallen - 3.0)),
                                                   under, player.position);
                        }
                    }
                    sound_director->tick((*interface)->hud().creative);
                }

                // Touching the ground ends flight — except for a spectator, who
                // may fly but cannot build, and is never set down.
                const bool spectator = abilities.may_fly && !abilities.instant_build;
                if (flying && player.on_ground && !spectator) {
                    flying = false;
                    client->send_abilities(false);
                }

                netclient::PlayerInput report;
                report.position  = player.position;
                report.yaw       = camera.yaw_degrees;
                report.pitch     = camera.pitch_degrees;
                report.on_ground = player.on_ground;
                client->send_position(report);
            }

            // The interpolation clock. The only place in this client where a
            // wall clock touches a position — and it never reaches the server:
            // what is sent back is the tick's own result, not this.
            entity_world.advance(ui_delta);

            // The eye, not the feet. 1.62 is vanilla's standing eye height and
            // it is what decides whether a one-block sill is at eye level.
            camera.position = Vec3f{static_cast<f32>(player.position.x),
                                    static_cast<f32>(player.position.y + 1.62),
                                    static_cast<f32>(player.position.z)};
            if (sound_director) {  // ── sound ── the ears are the camera
                const auto listen_started = std::chrono::steady_clock::now();
                sound_director->listen(
                    Vec3d{player.position.x, player.position.y + 1.62, player.position.z},
                    camera.yaw_degrees);
                sound_engine->update();
                audio_ms += std::chrono::duration<f64, std::milli>(
                                std::chrono::steady_clock::now() - listen_started)
                                .count();
                audio_frame_ms.push_back(audio_ms);
            }

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
                if (sound_director) {  // ── sound ── the breaker's own client plays it
                    sound_director->broke(dig_before, dig_target);
                }
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
                place_sent           = true;
                pending_place        = place_target;  // ── sound ──
                pending_place_frames = 0;
                OV_LOG_INFO("sound: waiting for the place at ({}, {}, {})", place_target.x,
                            place_target.y, place_target.z);
            }

            // Right-click a named block once the world has settled, and then
            // make two clicks in whatever window it opened.
            if (place_wanted && ground_ready && !scripted_place_sent && rendered > 120) {
                // Click the *top face of the block below*, which is how a
                // player places one: the target of a placement is the block you
                // are pointing at, not the space the block ends up in.
                client->send_place(place_target_scripted.x, place_target_scripted.y - 1,
                                   place_target_scripted.z, 1, 0.5F, 1.0F, 0.5F);
                scripted_place_sent  = true;
                pending_place        = place_target_scripted;  // ── sound ──
                pending_place_frames = 0;
            }
            if (use_wanted && ground_ready && !use_sent && rendered > 160) {
                client->send_place(use_target.x, use_target.y, use_target.z, 1, 0.5F, 1.0F,
                                   0.5F);
                use_sent = true;
            }
            if (use_sent && (*interface)->screen_open() && move_from >= 0 && !move_pick_sent &&
                rendered > 200) {
                (*interface)->click_slot(*client, move_from, 0, false, -1);
                move_pick_sent = true;
            }
            if (move_pick_sent && !move_place_sent && move_to >= 0 && rendered > 240) {
                (*interface)->click_slot(*client, move_to, 0, false, -1);
                move_place_sent = true;
            }
            if (drag_sequence.size() >= 2 && (*interface)->screen_open() && !drag_pick_sent &&
                rendered > 200) {
                (*interface)->click_slot(*client, drag_sequence.front(), 0, false, -1);
                drag_pick_sent = true;
            }
            if (drag_pick_sent && !drag_sent && rendered > 240) {
                (*interface)->drag_over(
                    *client, std::span<const i16>(drag_sequence).subspan(1), false);
                drag_sent = true;
            }
            if (options.dump_window && (*interface)->screen_open() && window_dump.empty() &&
                rendered > 280) {
                window_dump = (*interface)->describe_window();
            }
            if (options.close_at != 0 && rendered >= options.close_at &&
                (*interface)->screen_open()) {
                (*interface)->close(**window, *client);
            }
            if (options.open_inventory != 0 && rendered >= options.open_inventory &&
                !inventory_opened && !(*interface)->screen_open()) {
                (*interface)->toggle_inventory(**window, *client);
                inventory_opened = true;
            }
            // ── The creative inventory, scripted ────────────────────────────
            //
            // Opened, then posed, then read — in that order and in separate
            // frames, because the tab a capture wants is chosen after the
            // screen exists and the page is what a later frame draws.
            if (options.open_creative != 0 && rendered >= options.open_creative &&
                !creative_opened && !(*interface)->creative_open()) {
                (*interface)->toggle_creative(**window, *client);
                if ((*interface)->creative_open()) {
                    if (!options.creative_tab.empty() &&
                        !(*interface)->select_creative_tab(options.creative_tab)) {
                        fmt::print(stderr, "no creative tab named {}\n", options.creative_tab);
                    }
                    if (!options.creative_search.empty()) {
                        (*interface)->creative_search(options.creative_search);
                    }
                    if (!options.creative_pointer.empty()) {
                        const auto comma = options.creative_pointer.find(',');
                        (*interface)->set_creative_pointer(client::GuiPoint{
                            static_cast<f32>(std::atof(
                                options.creative_pointer.substr(0, comma).c_str())),
                            comma == std::string::npos
                                ? 0.0F
                                : static_cast<f32>(std::atof(
                                      options.creative_pointer.substr(comma + 1).c_str()))});
                    }
                }
                creative_opened = true;
            }
            if (!options.creative_take.empty() && creative_opened && !creative_taken &&
                rendered >= options.open_creative + 5) {
                const auto comma = options.creative_take.find(',');
                const i32  cell  = std::atoi(options.creative_take.substr(0, comma).c_str());
                const i16  slot  = comma == std::string::npos
                                     ? i16{36}
                                     : static_cast<i16>(std::atoi(
                                           options.creative_take.substr(comma + 1).c_str()));
                if ((*interface)->creative_take(cell)) {
                    (*interface)->creative_put(*client, slot);
                    fmt::print("creative: cell {} -> slot {}\n", cell, slot);
                } else {
                    fmt::print(stderr, "creative: cell {} is past the end of the page\n", cell);
                }
                creative_taken = true;
            }
            if (options.dump_creative && (*interface)->creative_open() && creative_dump.empty() &&
                rendered >= options.open_creative + 10) {
                creative_dump = (*interface)->describe_creative();
            }

            // ── chat, scripted ──────────────────────────────────────────────
            // Through the same calls Enter and typing make, so what a script
            // shows is what a player gets — and only once the game has begun
            // (the command tree is there), counted from that frame, because a
            // player cannot type before it either.
            if (chat_ready_at == 0 && (*interface)->chat().commands()) {
                chat_ready_at = rendered;
            }
            const bool chat_ready = chat_ready_at != 0;
            if (chat_ready && chat_sent < options.chat_send.size() &&
                rendered >= chat_ready_at + options.chat_at + 40 * static_cast<u32>(chat_sent)) {
                (*interface)->chat().submit(options.chat_send[chat_sent], *client, nullptr);
                ++chat_sent;
            }
            if (chat_ready && options.chat_open_at != 0 && !chat_opened &&
                rendered >= chat_ready_at + options.chat_open_at) {
                (*interface)->chat().open_box("", nullptr);
                (*interface)->chat().type(options.chat_type, *client);
                chat_opened = true;
            }
            if (options.chat_shot_age > 0 &&
                (*interface)->chat().newest_age() >= options.chat_shot_age &&
                (options.frames == 0 || options.frames > rendered + 1)) {
                options.frames = rendered + 1;  // this frame's successor is the last, and captured
            }
            // ── end chat ────────────────────────────────────────────────────

            // Where the player is looking, every frame rather than only on a
            // click: the outline has to follow the aim, and the click then uses
            // the same answer the player was shown.
            {
                const Vec3d eye{player.position.x, player.position.y + 1.62, player.position.z};
                const Vec3f look = camera.forward();
                const Vec3d forward{static_cast<f64>(look.x), static_cast<f64>(look.y),
                                    static_cast<f64>(look.z)};
                aimed = raycast_voxels(eye, forward, 4.5, [&](BlockPos block) {
                    return session->is_interaction_target(block.x, block.y, block.z);
                });
            }

            // Breaking and placing use exactly what the outline showed — and
            // only when no screen swallowed the click.
            if (!ui_took_input && (input.attack_pressed || input.use_pressed)) {
                const auto& hit = aimed;
                if (hit) {
                    const i32 face = static_cast<i32>(hit->face);
                    if (input.attack_pressed) {
                        // Start and finish in the same tick: creative-style
                        // instant breaking. The timed dig belongs with the
                        // block-breaking progress the server already computes.
                        const auto broken =  // ── sound ──
                            session->block_at(hit->block.x, hit->block.y, hit->block.z);
                        client->send_dig(hit->block.x, hit->block.y, hit->block.z, 0, face);
                        client->send_dig(hit->block.x, hit->block.y, hit->block.z, 2, face);
                        if (sound_director) {  // ── sound ──
                            sound_director->broke(broken, hit->block);
                        }
                    } else {
                        client->send_place(hit->block.x, hit->block.y, hit->block.z, face, 0.5F,
                                           0.5F, 0.5F);
                        // ── sound ── the placed cell is one step along the face
                        // (0..5: -Y, +Y, -Z, +Z, -X, +X); the clicked one may
                        // toggle instead. Whichever update comes back decides.
                        constexpr std::array<std::array<i32, 3>, 6> kFaceStep{
                            {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}}};
                        const auto& step     = kFaceStep[static_cast<usize>(std::clamp(face, 0, 5))];
                        pending_place        = BlockPos{hit->block.x + step[0],
                                                        hit->block.y + step[1],
                                                        hit->block.z + step[2]};
                        pending_place_frames = 0;
                        pending_use          = std::pair{
                            hit->block, session->block_at(hit->block.x, hit->block.y, hit->block.z)};
                        pending_use_frames = 0;
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
        // ── weather ── rain and a storm darken the sky; a flash lights it fully.
        const bool flashing = std::chrono::steady_clock::now() < flash_until;
        const f32  client_thunder = thunder_level * rain_level;
        const f32  darken = flashing ? 1.0F : render::sky_darken(time_of_day, rain_level, client_thunder);

        // Is the eye under water? Vanilla asks this every frame and changes
        // the fog completely when the answer is yes, and the difference is
        // enormous: without it, standing inside an ocean looks like standing in
        // clear air with the world mysteriously missing, because every face
        // between two water blocks is culled and there is genuinely nothing to
        // draw nearby.
        bool eye_in_water = false;
        if (online) {
            const auto sample = session->fluid_at(
                static_cast<i32>(std::floor(camera.position.x)),
                static_cast<i32>(std::floor(camera.position.y)),
                static_cast<i32>(std::floor(camera.position.z)));
            eye_in_water = sample.fluid == gameplay::Fluid::Water;
        }
        lightmap.update(darken, render::kOverworldAmbientLight, options.gamma, 0.0F);

        const u32 biome = online ? session->biome_at(static_cast<i32>(std::floor(camera.position.x)),
                                                     static_cast<i32>(std::floor(camera.position.y)),
                                                     static_cast<i32>(std::floor(camera.position.z)))
                                 : (world ? camera_biome(*world, *blocks, camera.position) : 0U);
        const auto effects = blocks->biome(biome);
        // ── weather ── the fog dims under rain and thunder, and flashes with a bolt.
        const u32 weather_fog = render::weather_fog_colour(render::fog_colour(effects.fog_colour, darken),
                                                           rain_level, client_thunder);
        const u32 fog_rgb = eye_in_water ? effects.water_fog_colour
                                         : (flashing ? render::weather_sky_colour(weather_fog, 0.0F, 0.0F, 1.0F)
                                                     : weather_fog);
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
        if (!options.fog) {
            sky.fog_start = 1.0e9F;
            sky.fog_end   = 1.1e9F;
        } else if (eye_in_water) {
            // Water fog is close and thick, and it uses the biome's own
            // water_fog_color rather than the sky's.
            //
            // The COLOUR is measured — it is the biome's own water_fog_color.
            // The DISTANCES are not, and are flagged here rather than dressed
            // up: vanilla's underwater fog also depends on how long you have
            // been submerged, on Respiration and on Water Breathing, none of
            // which exists here. A first attempt at 24 blocks drowned a seabed
            // seventeen blocks away, which is how it became obvious that
            // guessing tight was worse than guessing loose. See
            // docs/PROVENANCE.md.
            sky.fog_end   = std::min(render_distance, 96.0F);
            sky.fog_start = sky.fog_end * 0.25F;
        } else {
            sky.fog_start = render_distance * 0.92F;
            sky.fog_end   = render_distance;
        }

        (*terrain)->draw(cmd, view_projection, frustum, camera.position, *atlas_image, *sampler,
                         sky, options.cull);
        drawn_last_frame = (*terrain)->stats().sections_drawn;

        // Everything that moves, after the terrain and inside the same pass:
        // an entity is depth-tested against the ground it stands on, and a
        // second pass would need the depth attachment twice.
        {
            const auto entity_record_start = std::chrono::steady_clock::now();
            (*entity_renderer)->begin();
            if (options.entities && online) {
                client::EntitySky entity_sky;
                entity_sky.fog_colour = sky.fog_colour;
                entity_sky.fog_start  = sky.fog_start;
                entity_sky.fog_end    = sky.fog_end;

                demo::EntityDrawContext context;
                context.models       = entity_models ? &*entity_models : nullptr;
                context.textures     = &entity_textures;
                context.atlas        = atlas_entity_texture;
                context.items        = &item_models;
                context.foliage_tint = item_tint;

                for (const auto& [id, entity] : entity_world.entities()) {
                    const demo::EntityFrame entity_frame =
                        entity_world.frame_of(entity, options.entity_interpolation);

                    // Cull against the same frustum the terrain uses, with a
                    // box big enough for any model this build carries: the
                    // widest is the spider at 2.4 blocks.
                    constexpr f32 kCullRadius = 1.5F;
                    const Vec3f   low{entity_frame.position.x - kCullRadius,
                                    entity_frame.position.y - 0.5F,
                                    entity_frame.position.z - kCullRadius};
                    const Vec3f   high{entity_frame.position.x + kCullRadius,
                                     entity_frame.position.y + 2.5F,
                                     entity_frame.position.z + kCullRadius};
                    if (options.cull && !frustum.intersects(low, high)) {
                        continue;
                    }

                    // One lightmap lookup an entity, at the block its feet are
                    // in. Vanilla samples the same texture at the same point.
                    const std::array<u8, 3> entity_light =
                        entity_light_at(*session, lightmap, entity_frame.position);

                    if (demo::draw_entity(**entity_renderer, context, entity, entity_frame,
                                          entity_light, entity_scratch)) {
                        // Track how far the drawn position moved since the last
                        // frame. This is the interpolation measurement: without
                        // smoothing it is a whole tick's travel on one frame in
                        // three and zero on the others.
                        auto previous = last_drawn.find(id);
                        if (previous != last_drawn.end()) {
                            const Vec3f step = entity_frame.position - previous->second;
                            entity_steps.push_back(
                                static_cast<f64>(step.length()));
                        }
                        last_drawn[id] = entity_frame.position;
                    }
                }
            }
            (*entity_renderer)->draw(cmd, view_projection, camera.position,
                                     client::EntitySky{sky.fog_colour, sky.fog_start,
                                                        sky.fog_end});
            entity_record_ms.push_back(
                std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() -
                                                       entity_record_start)
                    .count());
        }

        // ── weather ── Rain and snow after everything opaque, in the same pass.
        if (online && weather_renderer && rain_level > 0.0F) {
            const auto weather_start = std::chrono::steady_clock::now();
            const f64  age_ticks =
                std::chrono::duration<f64>(weather_start - start_time).count() * 20.0;
            render::WeatherView view;
            view.camera     = Vec3d{static_cast<f64>(camera.position.x),
                                    static_cast<f64>(camera.position.y),
                                    static_cast<f64>(camera.position.z)};
            view.ticks      = static_cast<i64>(age_ticks);
            view.partial    = static_cast<f32>(age_ticks - std::floor(age_ticks));
            view.rain_level = rain_level;
            view.radius     = render::kWeatherRadiusFancy;
            render::weather_columns(view, weather_columns);
            const i32 eye_y = static_cast<i32>(std::floor(camera.position.y));
            for (render::WeatherColumn& column : weather_columns) {
                const world::Chunk* chunk = session->chunk_at(column.x >> 4, column.z >> 4);
                if (chunk == nullptr) {
                    continue;  // kind stays None: nothing falls where nothing has arrived
                }
                column.top = chunk->heightmap(world::HeightmapType::MotionBlocking)
                                 .first_free(static_cast<usize>(column.x & 15),
                                             static_cast<usize>(column.z & 15));
                // Rain or snow is decided at the bottom of the sheet.
                const i32 at_y  = std::max(eye_y - view.radius, column.top);
                const u32 column_biome = session->biome_at(column.x, at_y, column.z);
                switch (weather_climate.precipitation_at(
                            gameplay::climate_of(blocks->biome(column_biome)),
                                                         BlockPos{column.x, at_y, column.z})) {
                case gameplay::Precipitation::Rain: column.kind = render::WeatherKind::Rain; break;
                case gameplay::Precipitation::Snow: column.kind = render::WeatherKind::Snow; break;
                case gameplay::Precipitation::None: column.kind = render::WeatherKind::None; break;
                }
                column.light = entity_light_at(
                    *session, lightmap,
                    Vec3f{static_cast<f32>(column.x) + 0.5F,
                          static_cast<f32>(std::max(column.top, eye_y)),
                          static_cast<f32>(column.z) + 0.5F});
            }
            render::build_weather(view, weather_columns, weather_mesh);
            (*weather_renderer)->begin();
            if (rain_texture && !weather_mesh.rain.empty()) {
                (void)(*weather_renderer)->submit(*rain_texture, weather_mesh.rain);
            }
            if (snow_texture && !weather_mesh.snow.empty()) {
                (void)(*weather_renderer)->submit(*snow_texture, weather_mesh.snow);
            }
            (*weather_renderer)->draw(cmd, view_projection, camera.position,
                                      client::EntitySky{sky.fog_colour, sky.fog_start, sky.fog_end});
            weather_record_ms.push_back(std::chrono::duration<f64, std::milli>(
                                            std::chrono::steady_clock::now() - weather_start)
                                            .count());
        }

        // The two lines the game is played with. After the terrain, so the
        // outline blends over the face it surrounds rather than under it.
        if (aimed) {
            (*overlay)->draw_block_outline(cmd, view_projection,
                                           Vec3d{static_cast<f64>(camera.position.x),
                                                 static_cast<f64>(camera.position.y),
                                                 static_cast<f64>(camera.position.z)},
                                           aimed->block.x, aimed->block.y, aimed->block.z);
        }
        // The line crosshair only when the HUD is off: the HUD draws vanilla's
        // own crosshair sprite, and two crosshairs is one too many.
        if (online && !options.hud) {
            (*overlay)->draw_crosshair(cmd, width, height);
        }

        cmd.end_rendering();

        // The interface, in a pass of its own with no depth attachment at all.
        // It is on top of everything by definition, and sharing the terrain's
        // pass would mean either testing the hotbar against the world or
        // clearing a depth buffer nothing reads.
        // ── screens ── F3's numbers, gathered where they are
        if (menus->debug() && online && session) {
            client::DebugInfo info;
            if (cpu_frame_ms.size() >= 2) {
                const usize n = std::min<usize>(cpu_frame_ms.size(), 30);
                f64 sum = 0.0;
                for (usize i = cpu_frame_ms.size() - n; i < cpu_frame_ms.size(); ++i) {
                    sum += cpu_frame_ms[i];
                }
                info.fps = sum > 0.0 ? static_cast<i32>(std::lround(1000.0 * static_cast<f64>(n) / sum)) : 0;
            }
            info.sections_drawn    = drawn_last_frame;
            info.sections_resident = static_cast<u32>((*terrain)->stats().sections_resident);
            info.entities_drawn    = (*entity_renderer)->stats().entities;
            info.entities_known    = static_cast<u32>(entity_world.entities().size());
            info.x                 = player.position.x;
            info.y                 = player.position.y;
            info.z                 = player.position.z;
            info.yaw               = camera.yaw_degrees;
            info.pitch             = camera.pitch_degrees;
            info.biome             = std::string(blocks->biome_name(biome));
            const i32 fx = static_cast<i32>(std::floor(player.position.x));
            const i32 fy = static_cast<i32>(std::floor(player.position.y));
            const i32 fz = static_cast<i32>(std::floor(player.position.z));
            if (const world::Chunk* chunk = session->chunk_at(fx >> 4, fz >> 4)) {
                if (const world::ChunkSection* section = chunk->section_for_y(fy)) {
                    const usize cell = static_cast<usize>(((fy & 15) << 8) | ((fz & 15) << 4) | (fx & 15));
                    if (!section->sky_light().is_absent() || !section->block_light().is_absent()) {
                        info.sky_light   = section->sky_light().get(cell);
                        info.block_light = section->block_light().get(cell);
                    }
                }
            }
            if (aimed) {
                const auto state  = session->block_at(aimed->block.x, aimed->block.y, aimed->block.z);
                info.target_block = std::string(blocks->block_name(blocks->block_of(state)));
                info.target_x     = aimed->block.x;
                info.target_y     = aimed->block.y;
                info.target_z     = aimed->block.z;
            }
            info.day_time = time_of_day;
            memory_numbers(info.memory_used, info.memory_total);
            info.gpu                = std::string(device.info().name);
            info.cpu                = cpu_name();
            info.framebuffer_width  = width;
            info.framebuffer_height = height;
            menus->set_debug_info(std::move(info));
        }
        {  // ── screens ── always a UI pass: the menus draw over the game or alone
            rhi::ColourAttachment ui_colour;
            ui_colour.clear = false;
            const std::array<rhi::ColourAttachment, 1> ui_attachments{ui_colour};
            cmd.begin_rendering(ui_attachments, nullptr, width, height);
            cmd.set_viewport(0.0F, 0.0F, static_cast<f32>(width), static_cast<f32>(height));
            cmd.set_scissor(0, 0, width, height);
            if (online) {
                (*interface)->set_loading(loading_line);  // ── loading ──
                (*interface)->draw(cmd, width, height);
            }
            menus->draw(cmd, width, height);
            cmd.end_rendering();
        }

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
        if (options.frame_ms != 0) {  // ── chat ──  pacing for scripted captures
            std::this_thread::sleep_until(frame_start + std::chrono::milliseconds(options.frame_ms));
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
    {  // ── perf ── what a frame spends on the world arriving, apart from drawing it
        const auto apply = drop(apply_frame_ms);
        const auto mesh  = drop(mesh_frame_ms);
        if (!apply.empty()) {
            fmt::print("apply p50 {:.2f} ms   p99 {:.2f} ms   max {:.2f} ms   (chunks, blocks and "
                       "entities from the network)\n",
                       percentile(apply, 0.50), percentile(apply, 0.99), percentile(apply, 1.0));
        }
        if (!mesh.empty()) {
            fmt::print("mesh p50 {:.2f} ms   p99 {:.2f} ms   max {:.2f} ms   (4 ms budget, one "
                       "section at least)\n",
                       percentile(mesh, 0.50), percentile(mesh, 0.99), percentile(mesh, 1.0));
        }
    }

    if (!weather_record_ms.empty()) {  // ── weather ──
        const auto weather_ms = drop(weather_record_ms);
        fmt::print("wthr p50 {:.3f} ms   p99 {:.3f} ms   max {:.3f} ms   ({} frames with rain, "
                   "{} sheets last frame)\n",
                   percentile(weather_ms, 0.50), percentile(weather_ms, 0.99),
                   percentile(weather_ms, 1.0), weather_record_ms.size(), weather_mesh.columns);
    }
    if (online && options.entities) {
        const auto  entity_ms    = drop(entity_record_ms);
        const auto& entity_stats = (*entity_renderer)->stats();
        fmt::print("ent  p50 {:.3f} ms   p99 {:.3f} ms   max {:.3f} ms\n",
                   percentile(entity_ms, 0.50), percentile(entity_ms, 0.99),
                   percentile(entity_ms, 1.0));
        fmt::print("ent  {} tracked, {} drawn last frame, {} quads in {} draw(s), "
                   "{} vertices (peak {}){}\n",
                   entity_world.entities().size(), entity_stats.entities, entity_stats.quads,
                   entity_stats.draws, entity_stats.vertices, entity_stats.peak_vertices,
                   entity_stats.dropped != 0 ? "  DROPPED" : "");
        if (options.entity_stats && !entity_steps.empty()) {
            // The interpolation, as a number. Without smoothing an entity is
            // still on most frames and jumps a whole tick's travel on one in
            // three; with it, every frame carries its share. The maximum is
            // what a viewer sees as a stutter, so it is reported next to the
            // median rather than instead of it.
            fmt::print("ent  step between frames: p50 {:.5f}  p99 {:.5f}  max {:.5f} blocks "
                       "({} samples, interpolation {})\n",
                       percentile(entity_steps, 0.50), percentile(entity_steps, 0.99),
                       percentile(entity_steps, 1.0), entity_steps.size(),
                       options.entity_interpolation ? "on" : "off");
        }
        for (const std::string& unknown : entity_world.unknown_types()) {
            fmt::print("ent  not drawn: {}\n", unknown);
        }
    }

    if (sound_engine) {  // ── sound ──
        const auto audio = drop(audio_frame_ms);
        const auto stats = sound_engine->stats();
        fmt::print("snd  p50 {:.3f} ms   p99 {:.3f} ms   max {:.3f} ms per frame  "
                   "({} played, {} refused, {} dropped for voices, {} files decoded, {:.1f} MB)\n",
                   percentile(audio, 0.50), percentile(audio, 0.99), percentile(audio, 1.0),
                   stats.started, stats.refused, stats.dropped_no_voice, stats.cached_files,
                   static_cast<f64>(stats.cached_bytes) / (1024.0 * 1024.0));
        if (options.sound_log) {
            for (const audio::PlayedSound& played : sound_engine->take_log()) {
                fmt::print("snd  {} {} ({:.2f}, {:.2f}, {:.2f}) volume {:.3f} pitch {:.3f} {}\n",
                           audio::to_string(played.category), played.event, played.position.x,
                           played.position.y, played.position.z, played.volume, played.pitch,
                           played.file);
            }
        }
    }

    if (options.dump_menu) {  // ── screens ──
        fmt::print("{}\n", menus->describe());
    }
    if (paused_seconds > 0.0) {  // ── screens ── the pause, measured
        fmt::print("pause: {:.1f} s paused, {:.1f} s playing\n", paused_seconds,
                   playing_seconds);
    }
    if (online) {
        const auto& gui_stats = (*interface)->stats();
        fmt::print("gui  {} quads in {} draw(s), {} vertices (peak {}), scale {}\n",
                   gui_stats.quads, gui_stats.draws, gui_stats.vertices, gui_stats.peak_vertices,
                   options.gui_scale != 0
                       ? options.gui_scale
                       : client::auto_gui_scale(device.swapchain_width(),
                                                device.swapchain_height(), 0));
        if (options.dump_window) {
            fmt::print("{}\n",
                       window_dump.empty() ? (*interface)->describe_window() : window_dump);
            fmt::print("{}\n", (*interface)->describe_inventory());
        }
        if (options.dump_creative) {
            fmt::print("{}\n",
                       creative_dump.empty() ? (*interface)->describe_creative() : creative_dump);
            fmt::print("{}\n", (*interface)->describe_inventory());
        }
        // ── chat ──
        if (options.dump_chat) {
            for (const std::string& line : (*interface)->chat().transcript()) {
                fmt::print("chat: {}\n", line);
            }
            fmt::print("chat: newest line {} ticks old at the last frame\n",
                       (*interface)->chat().newest_age());
        }
        if (!options.dump_commands.empty()) {
            if (const auto& graph = (*interface)->chat().commands()) {
                const std::vector<u8> bytes = net::encode_commands(*graph);
                std::ofstream         out(options.dump_commands, std::ios::binary);
                out.write(reinterpret_cast<const char*>(bytes.data()),
                          static_cast<std::streamsize>(bytes.size()));
                fmt::print("commands: {} nodes, {} bytes -> {}\n", graph->nodes.size(),
                           bytes.size(), options.dump_commands);
            } else {
                fmt::print(stderr, "commands: the server sent no Commands packet\n");
            }
        }
        // ── end chat ──
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
