// ov_playerdata — the server's player-file reader and writer, on the command line.
//
//   ov_playerdata roundtrip IN OUT         read IN as the server would, write OUT
//   ov_playerdata level-inject LEVEL FILE  put FILE's compound in LEVEL's Data.Player
//
// The registry pack is read from data/vanilla/1.20.1/registry.ovpack under the
// working directory, or from $OV_REGISTRY_PACK.
#include "../../../src/ov_server/src/player_data.hpp"

#include "ov/io/compression.hpp"
#include "ov/io/file.hpp"
#include "ov/nbt/binary.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

using namespace ov;

int usage() {
    std::fprintf(stderr,
                 "usage: ov_playerdata roundtrip IN OUT\n"
                 "       ov_playerdata level-inject LEVEL.dat PLAYER.dat\n");
    return 2;
}

int roundtrip(const char* in, const char* out, const server::ItemNames& names) {
    const auto bytes = io::read_file(in);
    if (!bytes) {
        std::fprintf(stderr, "%s: cannot read\n", in);
        return 1;
    }
    const auto root = server::decode_player_file(*bytes, in);
    if (!root) {
        std::fprintf(stderr, "%s\n", root.error().message.c_str());
        return 1;
    }
    const auto uuid   = server::uuid_of(*root);
    const auto loaded = server::read_player(*root, uuid ? &*uuid : nullptr, names, in);
    if (!loaded) {
        std::fprintf(stderr, "%s\n", loaded.error().message.c_str());
        return 1;
    }
    const server::PlayerRecord& r = loaded->record;
    usize                       stacks = 0;
    for (const auto& stack : r.inventory) {
        stacks += stack.empty() ? 0 : 1;
    }
    std::printf("pos %.6f %.6f %.6f rot %.3f %.3f ground %d\n", r.x, r.y, r.z,
                static_cast<double>(r.yaw), static_cast<double>(r.pitch), r.on_ground ? 1 : 0);
    std::printf("health %.3f food %d sat %.4f exh %.7f xp %d+%d/%d total %d mode %d\n",
                static_cast<double>(r.health), r.food.food, static_cast<double>(r.food.saturation),
                static_cast<double>(r.food.exhaustion), r.xp_level, r.xp_points,
                gameplay::experience_to_next_level(r.xp_level, gameplay::ExperienceCurve{}),
                r.xp_total, r.game_type);
    std::printf("stacks %zu held %d effects %zu unknown items %zu unknown effects %zu\n", stacks,
                r.selected_slot, r.effects.size(), loaded->unknown_items, loaded->unknown_effects);

    const net::Uuid written_uuid = uuid.value_or(net::Uuid{});
    const auto encoded = server::encode_player_file(
        server::write_player(r, &loaded->original, written_uuid, names));
    if (encoded.empty() || !io::write_file_atomic(out, encoded)) {
        std::fprintf(stderr, "%s: cannot write\n", out);
        return 1;
    }
    return 0;
}

int level_inject(const char* level, const char* player) {
    const auto level_bytes  = io::read_file(level);
    const auto player_bytes = io::read_file(player);
    if (!level_bytes || !player_bytes) {
        std::fprintf(stderr, "cannot read %s or %s\n", level, player);
        return 1;
    }
    const auto plain = io::gzip_decompress(*level_bytes);
    auto document    = plain ? nbt::read(*plain) : nbt::read(*level_bytes);
    const auto root  = server::decode_player_file(*player_bytes, player);
    if (!document || !root) {
        std::fprintf(stderr, "cannot parse %s or %s\n", level, player);
        return 1;
    }
    nbt::Tag* data = document->root.find("Data");
    if (data == nullptr) {
        std::fprintf(stderr, "%s has no Data\n", level);
        return 1;
    }
    (void)data->put("Player", *root);
    const auto packed = io::gzip_compress(nbt::write(*document));
    if (!packed || !io::write_file_atomic(level, *packed)) {
        std::fprintf(stderr, "cannot write %s\n", level);
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return usage();
    }
    const std::string command = argv[1];
    if (command == "level-inject") {
        return argc == 4 ? level_inject(argv[2], argv[3]) : usage();
    }
    if (command != "roundtrip" || argc != 4) {
        return usage();
    }

    const char*       env  = std::getenv("OV_REGISTRY_PACK");
    const std::string pack = env != nullptr ? env : "data/vanilla/1.20.1/registry.ovpack";
    auto              registries = registry::Registries::load(pack);
    if (!registries) {
        std::fprintf(stderr, "%s: cannot load the registry pack\n", pack.c_str());
        return 1;
    }
    const server::ItemNames names{&*registries, registries->find("minecraft:item")};
    return roundtrip(argv[2], argv[3], names);
}
