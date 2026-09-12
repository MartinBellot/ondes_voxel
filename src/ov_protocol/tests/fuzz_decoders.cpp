// Deterministic fuzzing of every decoder that reads bytes from a socket.
//
// Not libFuzzer: that needs a coverage-instrumented toolchain the three CI
// platforms do not share, and a corpus that drifts. This is a fixed-seed
// generator, so a failure reproduces from the seed alone, on every machine,
// and the run fits in the unit test budget. Its value comes from running under
// the ASan + UBSan preset, where an out-of-bounds read or an absurd allocation
// is a crash instead of a silent wrong answer.
//
// Four kinds of input, each fed to every decoder rather than only its own —
// a socket does not promise the bytes match the id:
//   1. every prefix of a valid packet (truncation is the commonest attack);
//   2. valid packets with bytes flipped, overwritten with boundary values, or
//      with a hostile VarInt spliced in (lengths of 2^31-1, negative counts);
//   3. uniformly random bytes;
//   4. a byte stream cut at random points and pushed through the framer, with
//      and without compression.
//
// The property checked is the one the protocol layer promises: malformed input
// yields nullopt or an error, never a crash, a hang or an unbounded allocation.
//
// OV_FUZZ_ITERATIONS and OV_FUZZ_SEED widen a run by hand; the defaults are
// what CI executes.

#include "ov/io/byte_reader.hpp"
#include "ov/io/byte_writer.hpp"
#include "ov/protocol/blast.hpp"
#include "ov/protocol/breaking.hpp"
#include "ov/protocol/chat.hpp"
#include "ov/protocol/chat_types.hpp"
#include "ov/protocol/client_play.hpp"
#include "ov/protocol/effect_packets.hpp"
#include "ov/protocol/framing.hpp"
#include "ov/protocol/hud.hpp"
#include "ov/protocol/interaction.hpp"
#include "ov/protocol/login.hpp"
#include "ov/protocol/play.hpp"
#include "ov/protocol/sound.hpp"
#include "ov/protocol/status.hpp"
#include "ov/protocol/survival.hpp"
#include "ov/protocol/types.hpp"
#include "ov/protocol/varint.hpp"
#include "ov/world/chunk.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

using namespace ov;
using namespace ov::net;

namespace {

// SplitMix64: explicit, bit-exact, no dependence on the standard library's
// distributions, whose output differs between implementations.
class Rng {
public:
    explicit Rng(u64 seed) noexcept : state_{seed} {}

    u64 next() noexcept {
        u64 z = (state_ += 0x9E3779B97F4A7C15ULL);
        z     = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z     = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    /// In [0, bound). The modulo bias is irrelevant for fuzzing.
    usize below(usize bound) noexcept {
        return bound == 0 ? 0 : static_cast<usize>(next() % bound);
    }

    u8 byte() noexcept { return static_cast<u8>(next()); }

private:
    u64 state_;
};

u64 env_or(const char* name, u64 fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    return std::strtoull(value, nullptr, 0);
}

using Bytes   = std::vector<u8>;
using Decoder = std::function<void(std::span<const u8>)>;

struct NamedDecoder {
    std::string_view name;
    Decoder          decode;
};

// The results are discarded: only the absence of a crash is under test. They
// are still observed, so the optimiser cannot drop the call.
template<typename T>
void observe(const T& result) {
    volatile bool sink = result.has_value();
    (void)sink;
}

std::vector<NamedDecoder> all_decoders() {
    std::vector<NamedDecoder> decoders;
    auto                      add = [&](std::string_view name, Decoder decode) {
        decoders.push_back({name, std::move(decode)});
    };

    // Handshake, status and login: what an unauthenticated stranger reaches.
    add("handshake", [](auto p) { observe(parse_handshake(p)); });
    add("login_start", [](auto p) { observe(parse_login_start(p)); });

    // Serverbound play: what a connected client can send the server.
    add("client_information", [](auto p) { observe(parse_client_information(p)); });
    add("confirm_teleport", [](auto p) { observe(parse_confirm_teleport(p)); });
    add("keep_alive", [](auto p) { observe(parse_keep_alive(p)); });
    add("move_position",
        [](auto p) { observe(parse_movement(serverbound::kSetPlayerPosition, p)); });
    add("move_position_rot",
        [](auto p) { observe(parse_movement(serverbound::kSetPlayerPositionRot, p)); });
    add("move_rotation",
        [](auto p) { observe(parse_movement(serverbound::kSetPlayerRotation, p)); });
    add("move_on_ground",
        [](auto p) { observe(parse_movement(serverbound::kSetPlayerOnGround, p)); });
    add("player_action", [](auto p) { observe(parse_player_action(p)); });
    add("use_item_on", [](auto p) { observe(parse_use_item_on(p)); });
    add("use_item", [](auto p) { observe(parse_use_item(p)); });
    add("interact", [](auto p) { observe(parse_interact(p)); });
    add("swing_arm", [](auto p) { observe(parse_swing_arm(p)); });
    add("set_creative_slot", [](auto p) { observe(parse_set_creative_slot(p)); });
    add("set_held_item", [](auto p) { observe(parse_set_held_item(p)); });
    add("container_click", [](auto p) { observe(parse_container_click(p)); });
    add("close_container", [](auto p) { observe(parse_close_container(p)); });
    add("update_sign", [](auto p) { observe(parse_update_sign(p)); });
    add("chat_command", [](auto p) { observe(parse_chat_command(p)); });
    add("chat_message", [](auto p) { observe(parse_chat_message(p)); });
    add("message_acknowledgment", [](auto p) { observe(parse_message_acknowledgment(p)); });
    add("suggestions_request", [](auto p) { observe(parse_suggestions_request(p)); });
    add("change_difficulty", [](auto p) { observe(parse_change_difficulty(p)); });
    add("client_command", [](auto p) { observe(parse_client_command(p)); });
    add("player_command", [](auto p) { observe(parse_player_command(p)); });

    // Clientbound play: what a hostile server can send ov_netclient.
    add("login_play_chat_types", [](auto p) { observe(read_login_chat_types(p)); });
    add("chunk_data_overworld", [](auto p) {
        observe(parse_chunk_data(p, world::WorldShape::overworld(), world::AirStates{}, nullptr));
    });
    add("chunk_data_nether", [](auto p) {
        observe(parse_chunk_data(p, world::WorldShape::nether(), world::AirStates{}, nullptr));
    });
    add("set_health", [](auto p) { observe(parse_set_health(p)); });
    add("set_experience", [](auto p) { observe(parse_set_experience(p)); });
    add("container_content", [](auto p) { observe(parse_container_content(p)); });
    add("container_slot", [](auto p) { observe(parse_container_slot(p)); });
    add("open_screen", [](auto p) { observe(parse_open_screen(p)); });
    add("clientbound_close_container",
        [](auto p) { observe(parse_clientbound_close_container(p)); });
    add("system_chat", [](auto p) { observe(parse_system_chat(p)); });
    add("player_chat", [](auto p) { observe(parse_player_chat(p)); });
    add("disguised_chat", [](auto p) { observe(parse_disguised_chat(p)); });
    add("suggestions_response", [](auto p) { observe(parse_suggestions_response(p)); });
    add("commands", [](auto p) { observe(parse_commands(p)); });
    add("update_section_blocks", [](auto p) { observe(parse_update_section_blocks(p)); });
    add("explosion", [](auto p) { observe(parse_explosion(p)); });
    add("block_destroy_stage", [](auto p) { observe(parse_block_destroy_stage(p)); });
    add("entity_effect", [](auto p) { observe(decode_entity_effect(p)); });
    add("remove_entity_effect", [](auto p) { observe(decode_remove_entity_effect(p)); });
    add("update_attributes", [](auto p) { observe(decode_update_attributes(p)); });
    add("sound_effect", [](auto p) { observe(parse_sound_effect(p)); });
    add("entity_sound_effect", [](auto p) { observe(parse_entity_sound_effect(p)); });
    add("stop_sound", [](auto p) { observe(parse_stop_sound(p)); });
    add("world_event", [](auto p) { observe(parse_world_event(p)); });

    // HUD: boss bars, the border, the scoreboard, statistics, advancement tabs.
    add("boss_bar", [](auto p) { observe(parse_boss_bar(p)); });
    add("initialize_world_border", [](auto p) { observe(parse_initialize_world_border(p)); });
    add("set_border_center", [](auto p) { observe(parse_set_border_center(p)); });
    add("set_border_lerp_size", [](auto p) { observe(parse_set_border_lerp_size(p)); });
    add("set_border_size", [](auto p) { observe(parse_set_border_size(p)); });
    add("set_border_warning_delay", [](auto p) { observe(parse_set_border_warning_delay(p)); });
    add("set_border_warning_distance",
        [](auto p) { observe(parse_set_border_warning_distance(p)); });
    add("display_objective", [](auto p) { observe(parse_display_objective(p)); });
    add("update_objectives", [](auto p) { observe(parse_update_objectives(p)); });
    add("update_teams", [](auto p) { observe(parse_update_teams(p)); });
    add("update_score", [](auto p) { observe(parse_update_score(p)); });
    add("award_statistics", [](auto p) { observe(parse_award_statistics(p)); });
    add("select_advancements_tab", [](auto p) { observe(parse_select_advancements_tab(p)); });
    add("seen_advancements", [](auto p) { observe(parse_seen_advancements(p)); });

    // The primitives every packet is made of.
    add("slot", [](auto p) {
        io::ByteReader reader{p};
        observe(read_slot(reader));
    });
    add("string", [](auto p) {
        io::ByteReader reader{p};
        observe(read_string(reader));
    });
    add("uuid", [](auto p) {
        io::ByteReader reader{p};
        observe(read_uuid(reader));
    });
    add("uuid_text", [](auto p) {
        observe(Uuid::parse(std::string_view{reinterpret_cast<const char*>(p.data()), p.size()}));
    });
    add("position", [](auto p) {
        io::ByteReader reader{p};
        observe(read_position_raw(reader));
    });
    add("angle", [](auto p) {
        io::ByteReader reader{p};
        observe(read_angle(reader));
    });
    add("varint", [](auto p) {
        io::ByteReader reader{p};
        const auto     value = read_varint(reader);
        observe(value);
        // A VarInt is at most five bytes; a reader that walks further on a run
        // of continuation bits is how a 4-byte packet becomes a hang.
        REQUIRE(reader.position() <= 5);
    });
    add("varlong", [](auto p) {
        io::ByteReader reader{p};
        const auto     value = read_varlong(reader);
        observe(value);
        REQUIRE(reader.position() <= 10);
    });
    return decoders;
}

void write_hostile_varint(io::ByteWriter& writer, Rng& rng) {
    static constexpr std::array<i32, 8> kValues{
        0x7FFFFFFF, -1,    static_cast<i32>(0x80000000),           0x00200000, 0x7FFF,
        256,        65536, static_cast<i32>(kMaxPacketLength) + 1,
    };
    write_varint(writer, kValues[rng.below(kValues.size())]);
}

/// Valid packets, from our own encoders and hand-built from the spec for the
/// serverbound ones that only a client encodes. A mutation of something
/// well-formed reaches much deeper than random bytes, which most decoders
/// reject at the first field.
std::vector<Bytes> seed_corpus() {
    std::vector<Bytes> seeds;

    {  // Handshake: protocol, address, port, next state.
        io::ByteWriter w;
        write_varint(w, 763);
        write_string(w, "localhost");
        w.write_u16(25565);
        write_varint(w, 2);
        seeds.push_back(w.take());
    }
    {  // Login Start: name, has-UUID flag, UUID.
        io::ByteWriter w;
        write_string(w, "Steve");
        w.write_u8(1);
        w.write_u64(0x0123456789ABCDEFULL);
        w.write_u64(0xFEDCBA9876543210ULL);
        seeds.push_back(w.take());
    }
    {  // Client Information.
        io::ByteWriter w;
        write_string(w, "en_us");
        w.write_i8(12);
        write_varint(w, 0);
        w.write_u8(1);
        w.write_u8(0x7F);
        write_varint(w, 1);
        w.write_u8(0);
        w.write_u8(1);
        seeds.push_back(w.take());
    }
    {  // Set Player Position And Rotation.
        io::ByteWriter w;
        w.write_f64(0.5);
        w.write_f64(64.0);
        w.write_f64(-0.5);
        w.write_f32(90.0F);
        w.write_f32(-30.0F);
        w.write_u8(1);
        seeds.push_back(w.take());
    }
    {  // Interact, attack variant, then interact-at with a target and a hand.
        io::ByteWriter w;
        write_varint(w, 17);
        write_varint(w, 1);
        w.write_u8(0);
        seeds.push_back(w.take());
        io::ByteWriter at;
        write_varint(at, 17);
        write_varint(at, 2);
        at.write_f32(0.1F);
        at.write_f32(0.2F);
        at.write_f32(0.3F);
        write_varint(at, 0);
        at.write_u8(1);
        seeds.push_back(at.take());
    }
    {  // Chat Command: command, timestamp, salt, zero signatures, ack.
        io::ByteWriter w;
        write_string(w, "gamemode creative");
        w.write_i64(1);
        w.write_i64(2);
        write_varint(w, 0);
        write_varint(w, 0);
        w.write_u8(0);
        w.write_u8(0);
        w.write_u8(0);
        seeds.push_back(w.take());
    }
    {  // Update Sign: position, front, four lines.
        io::ByteWriter w;
        w.write_i64(0);
        w.write_u8(1);
        for (int line = 0; line < 4; ++line) {
            write_string(w, "line");
        }
        seeds.push_back(w.take());
    }
    {  // Use Item On: hand, position, face, cursor, inside, sequence.
        io::ByteWriter w;
        write_varint(w, 0);
        w.write_i64(0x0000004000000040LL);
        write_varint(w, 1);
        w.write_f32(0.5F);
        w.write_f32(1.0F);
        w.write_f32(0.5F);
        w.write_u8(0);
        write_varint(w, 3);
        seeds.push_back(w.take());
    }

    ItemStack stone;
    stone.item_id = 1;
    stone.count   = 64;
    const ItemStack                none{};
    const std::array<ItemStack, 3> slots{stone, none, stone};

    seeds.push_back(encode_keep_alive(42));
    seeds.push_back(encode_player_action(0, WirePosition{1, -2, 3}, 1, 7));
    seeds.push_back(encode_swing_arm(Hand::Off));
    seeds.push_back(encode_container_click(1, 4, 5, 0, 0, {}, stone));
    seeds.push_back(encode_serverbound_close_container(1));
    seeds.push_back(encode_suggestions_request(SuggestionsRequest{}));
    seeds.push_back(encode_chat_command(ChatCommand{}));
    seeds.push_back(encode_chat_message(ChatMessage{}));

    seeds.push_back(encode_set_health(20.0F, 20, 5.0F));
    seeds.push_back(encode_set_experience(0.5F, 3, 30));
    seeds.push_back(encode_container_content(0, 1, slots, none));
    seeds.push_back(encode_container_slot(0, 1, 36, stone));
    seeds.push_back(encode_open_screen(1, 2, R"({"text":"Chest"})"));
    seeds.push_back(encode_close_container(1));
    seeds.push_back(encode_system_chat(R"({"text":"hello"})", false));
    seeds.push_back(encode_player_chat(PlayerChat{}));
    seeds.push_back(encode_disguised_chat(DisguisedChat{}));
    seeds.push_back(encode_suggestions_response(SuggestionsResponse{}));
    seeds.push_back(encode_commands(CommandGraphWire{}));
    seeds.push_back(encode_change_difficulty(2, false));
    seeds.push_back(encode_update_section_blocks(0, 4, 0, {}));
    seeds.push_back(encode_world_event(1000, WirePosition{1, 2, 3}, 0, false));
    seeds.push_back(encode_block_destroy_stage(BlockDestroyStage{}));
    seeds.push_back(encode_explosion(Explosion{}));
    seeds.push_back(encode_entity_effect(EntityEffect{}));
    seeds.push_back(encode_remove_entity_effect(5, 1));
    seeds.push_back(encode_update_attributes_full(5, {}));
    seeds.push_back(encode_sound_effect(SoundEffect{}));
    seeds.push_back(encode_entity_sound_effect(EntitySoundEffect{}));
    seeds.push_back(encode_stop_sound(StopSound{}));

    {  // HUD packets, with every optional part present so mutations reach it.
        BossBar bar;
        bar.title_json = R"({"text":"Boss"})";
        bar.health     = 0.5F;
        bar.color      = 2;
        bar.division   = 1;
        bar.flags      = kBossBarDarkenSky;
        seeds.push_back(encode_boss_bar(bar));

        WorldBorderInit border;
        border.old_diameter = 100.0;
        border.new_diameter = 50.0;
        border.lerp_ms      = 60'000;
        seeds.push_back(encode_initialize_world_border(border));
        seeds.push_back(encode_set_border_lerp_size(BorderLerp{100.0, 50.0, i64{1} << 40}));
        seeds.push_back(encode_set_border_center(1.0, 2.0));

        seeds.push_back(encode_display_objective(DisplayObjective{1, "kills"}));
        UpdateObjectives objectives;
        objectives.name         = "kills";
        objectives.display_json = R"({"text":"Kills"})";
        seeds.push_back(encode_update_objectives(objectives));

        UpdateTeams team;
        team.team              = "red";
        team.info.display_json = R"({"text":"Red"})";
        team.entities          = {"Steve", "Alex"};
        seeds.push_back(encode_update_teams(team));
        seeds.push_back(encode_update_score(UpdateScore{"Steve", ScoreAction::Change, "kills", 3}));

        const std::array<Statistic, 2> stats{Statistic{8, 5, 300}, Statistic{0, 1, 1}};
        seeds.push_back(encode_award_statistics(stats));
        seeds.push_back(encode_select_advancements_tab(
            SelectAdvancementsTab{std::string{"minecraft:story/root"}}));
        seeds.push_back(encode_seen_advancements(
            SeenAdvancements{SeenAdvancementsAction::OpenedTab, "minecraft:story/root"}));
    }

    // An empty chunk in both shapes: the largest decoder, and the one whose
    // counts (sections, palette, longs, light arrays) come straight off the wire.
    for (const auto shape : {world::WorldShape::overworld(), world::WorldShape::nether()}) {
        const world::Chunk chunk{ChunkPos{3, -7}, shape, world::AirStates{}, nullptr};
        seeds.push_back(encode_chunk_data(chunk));
    }
    return seeds;
}

Bytes mutate(const Bytes& seed, Rng& rng) {
    Bytes       out   = seed;
    const usize edits = 1 + rng.below(4);
    for (usize edit = 0; edit < edits; ++edit) {
        switch (rng.below(6)) {
            case 0:  // flip one bit
                if (!out.empty()) {
                    out[rng.below(out.size())] ^= static_cast<u8>(1U << rng.below(8));
                }
                break;
            case 1: {  // overwrite with a boundary byte
                static constexpr std::array<u8, 6> kBoundary{0x00, 0x01, 0x7F, 0x80, 0xFE, 0xFF};
                if (!out.empty()) {
                    out[rng.below(out.size())] = kBoundary[rng.below(kBoundary.size())];
                }
                break;
            }
            case 2: {  // splice a hostile VarInt in place of whatever was there
                io::ByteWriter hostile;
                write_hostile_varint(hostile, rng);
                const usize at = rng.below(out.size() + 1);
                const auto  v  = hostile.take();
                out.insert(out.begin() + static_cast<std::ptrdiff_t>(at), v.begin(), v.end());
                break;
            }
            case 3:  // truncate
                out.resize(rng.below(out.size() + 1));
                break;
            case 4: {  // insert random bytes
                const usize at    = rng.below(out.size() + 1);
                const usize count = 1 + rng.below(8);
                for (usize i = 0; i < count; ++i) {
                    out.insert(out.begin() + static_cast<std::ptrdiff_t>(at), rng.byte());
                }
                break;
            }
            default:  // a run of continuation bytes, the VarInt reader's worst case
                if (!out.empty()) {
                    const usize at  = rng.below(out.size());
                    const usize run = std::min<usize>(out.size() - at, 1 + rng.below(12));
                    for (usize i = 0; i < run; ++i) {
                        out[at + i] = 0xFF;
                    }
                }
                break;
        }
    }
    return out;
}

void run_all(const std::vector<NamedDecoder>& decoders, std::span<const u8> input) {
    for (const auto& decoder : decoders) {
        decoder.decode(input);
    }
}

}  // namespace

TEST_CASE("the seed corpus decodes cleanly", "[protocol][fuzz]") {
    // Guards the harness itself: a seed our own decoders reject would make every
    // mutation of it a test of the first field only.
    const auto seeds = seed_corpus();
    REQUIRE(seeds.size() > 40);
    CHECK(parse_handshake(seeds[0]).has_value());
    CHECK(parse_login_start(seeds[1]).has_value());
    CHECK(parse_keep_alive(encode_keep_alive(42)) == std::optional<i64>{42});
    CHECK(parse_set_health(encode_set_health(20.0F, 20, 5.0F)).has_value());
    CHECK(parse_container_content(encode_container_content(0, 1, {}, ItemStack{})).has_value());
    const world::Chunk chunk{ChunkPos{3, -7}, world::WorldShape::overworld(), world::AirStates{},
                             nullptr};
    CHECK(parse_chunk_data(encode_chunk_data(chunk), world::WorldShape::overworld(),
                           world::AirStates{}, nullptr)
              .has_value());
}

TEST_CASE("every prefix of every valid packet is rejected or decoded, never a crash",
          "[protocol][fuzz]") {
    const auto decoders = all_decoders();
    for (const auto& seed : seed_corpus()) {
        // The chunk seeds are several kilobytes; every prefix through every
        // decoder is quadratic, so those are sampled with a stride.
        const usize stride = seed.size() > 512 ? 7 : 1;
        for (usize length = 0; length <= seed.size(); length += stride) {
            run_all(decoders, std::span<const u8>{seed}.first(length));
        }
    }
    SUCCEED();
}

TEST_CASE("mutated packets are rejected or decoded, never a crash", "[protocol][fuzz]") {
    const u64 seed       = env_or("OV_FUZZ_SEED", 0x0763'0763'0763'0763ULL);
    const u64 iterations = env_or("OV_FUZZ_ITERATIONS", 4000);
    INFO("OV_FUZZ_SEED=" << seed);

    Rng        rng{seed};
    const auto decoders = all_decoders();
    const auto seeds    = seed_corpus();
    for (u64 i = 0; i < iterations; ++i) {
        const Bytes input = mutate(seeds[rng.below(seeds.size())], rng);
        run_all(decoders, input);
    }
    SUCCEED();
}

TEST_CASE("random bytes are rejected or decoded, never a crash", "[protocol][fuzz]") {
    const u64 seed       = env_or("OV_FUZZ_SEED", 0x0763'0763'0763'0763ULL) ^ 0xA5A5A5A5ULL;
    const u64 iterations = env_or("OV_FUZZ_ITERATIONS", 4000);
    INFO("OV_FUZZ_SEED=" << seed);

    Rng        rng{seed};
    const auto decoders = all_decoders();
    Bytes      input;
    for (u64 i = 0; i < iterations; ++i) {
        // Mostly short, the length real packets have; now and then a long one.
        const usize length = rng.below(8) == 0 ? rng.below(4096) : rng.below(64);
        input.resize(length);
        for (auto& b : input) {
            b = rng.byte();
        }
        run_all(decoders, input);
    }
    SUCCEED();
}

TEST_CASE("the framer survives a hostile byte stream cut anywhere", "[protocol][fuzz]") {
    const u64 seed       = env_or("OV_FUZZ_SEED", 0x0763'0763'0763'0763ULL) ^ 0x5A5A5A5AULL;
    const u64 iterations = env_or("OV_FUZZ_ITERATIONS", 4000);
    INFO("OV_FUZZ_SEED=" << seed);

    Rng        rng{seed};
    const auto seeds = seed_corpus();

    for (const i32 threshold : {kNoCompression, 0, 256}) {
        for (u64 i = 0; i < iterations / 4; ++i) {
            // A stream of well-framed packets, then mutated as a whole: the
            // length prefixes and the inner compressed lengths get hit too.
            Bytes       stream;
            const usize packets = 1 + rng.below(4);
            for (usize p = 0; p < packets; ++p) {
                const auto& body = seeds[rng.below(seeds.size())];
                const auto  encoded =
                    encode_packet(static_cast<i32>(rng.below(0x80)), body, threshold);
                REQUIRE(encoded.has_value());
                stream.insert(stream.end(), encoded->begin(), encoded->end());
            }
            stream = mutate(stream, rng);

            FrameDecoder decoder;
            decoder.set_compression_threshold(threshold);
            usize offset = 0;
            bool  failed = false;
            while (offset < stream.size() && !failed) {
                const usize cut = std::min(stream.size() - offset, 1 + rng.below(64));
                decoder.feed(std::span<const u8>{stream}.subspan(offset, cut));
                offset += cut;
                for (;;) {
                    auto packet = decoder.next();
                    if (!packet) {
                        // Incomplete means wait; anything else drops the
                        // connection, which is the end of this stream.
                        failed = packet.error() != FrameError::Incomplete;
                        break;
                    }
                    // Whatever the framer accepts is bounded by the protocol's
                    // own ceiling, compressed or not.
                    REQUIRE(packet->body.size() <= kMaxPacketLength);
                }
            }
        }
    }
    SUCCEED();
}

TEST_CASE("a frame that lies about its inflated size is refused", "[protocol][fuzz]") {
    // Targeted rather than random: a compressed frame announcing an inner
    // length far over the ceiling. Inflating it trustingly is a decompression
    // bomb; the framer must refuse before allocating.
    for (const i32 claimed : {static_cast<i32>(kMaxPacketLength) + 1, 0x7FFFFFFF, -1}) {
        io::ByteWriter inner;
        write_varint(inner, claimed);
        inner.write_bytes(std::string_view{"\x78\x9c\x03\x00\x00\x00\x00\x01", 8});
        io::ByteWriter frame;
        write_varint(frame, static_cast<i32>(inner.size()));
        frame.write_bytes(inner.data());

        FrameDecoder decoder;
        decoder.set_compression_threshold(256);
        decoder.feed(frame.data());
        const auto packet = decoder.next();
        CHECK_FALSE(packet.has_value());
    }
}
