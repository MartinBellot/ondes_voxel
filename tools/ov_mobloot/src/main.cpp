// Roll entity loot tables many times and report what came out.
//
// The counterpart of `ov_inspect loot` for mobs. It exists for one reason: the
// real 1.20.1 server can be made to draw the same tables through its own `/loot
// give <player> kill <entity>` command, and scripts/check_entity_loot.py puts
// the two distributions side by side. A table that agrees on a thousand draws is
// evidence; a table that agrees on one is a coincidence.
//
// Usage:
//   ov_mobloot <registry.ovpack> <entity_loot.ovpack>
//
// One case per line on stdin:
//   <entity> <killed_by_player> <looting> <on_fire> <slime_size> <killer|-> <draws>
//
// One line per case on stdout, in the same order:
//   <entity> <item>:<total> <item>:<total> ...
#include "ov/gameplay/loot.hpp"
#include "ov/gameplay/recipe.hpp"
#include "ov/math/random.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <print>
#include <sstream>
#include <string>
#include <vector>

namespace {

[[nodiscard]] std::vector<ov::u8> read_file(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        return {};
    }
    return std::vector<ov::u8>{std::istreambuf_iterator<char>{stream},
                               std::istreambuf_iterator<char>{}};
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::println(stderr, "usage: ov_mobloot <registry.ovpack> <entity_loot.ovpack>");
        return 2;
    }
    const std::filesystem::path pack{argv[1]};
    const std::filesystem::path entity_pack{argv[2]};

    auto registries = ov::registry::Registries::load(pack);
    if (!registries) {
        std::println(stderr, "cannot read {}: {}", pack.string(),
                     ov::registry::to_string(registries.error()));
        return 1;
    }
    const ov::gameplay::RecipeBook recipes{*registries};

    auto bytes = read_file(entity_pack);
    if (bytes.empty()) {
        std::println(stderr, "cannot read {}", entity_pack.string());
        return 1;
    }
    auto tables =
        ov::gameplay::EntityLootTables::from_bytes(std::move(bytes), *registries, &recipes);
    if (!tables) {
        std::println(stderr, "cannot read {}: {}", entity_pack.string(),
                     ov::registry::to_string(tables.error()));
        return 1;
    }

    const auto item_registry = registries->find("minecraft:item");
    if (!item_registry) {
        std::println(stderr, "the pack has no minecraft:item registry");
        return 1;
    }

    // One seed for the whole run, advanced by every draw. A per-case seed would
    // make two adjacent cases correlated, which is exactly the failure mode a
    // distribution comparison cannot see.
    ov::math::XoroshiroRandomSource random{0x5EED'1234'ABCD'0001LL};

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }
        std::istringstream fields{line};
        std::string        entity;
        int                killed_by_player = 0;
        int                looting          = 0;
        int                on_fire          = 0;
        int                slime_size       = 0;
        std::string        killer;
        long long          draws = 0;
        fields >> entity >> killed_by_player >> looting >> on_fire >> slime_size >> killer >>
            draws;

        ov::gameplay::KillContext kill;
        kill.entity_type      = entity;
        kill.killed_by_player = killed_by_player != 0;
        kill.looting          = static_cast<ov::u8>(looting);
        kill.on_fire          = on_fire != 0;
        kill.slime_size       = slime_size;
        if (killer != "-") {
            kill.killer_type = killer;
        }

        std::map<ov::registry::ProtocolId, long long> totals;
        std::vector<ov::gameplay::Drop>               drops;
        ov::gameplay::DrawResult                      gaps;
        for (long long i = 0; i < draws; ++i) {
            drops.clear();
            const auto result = tables->drops(kill, random, drops);
            gaps.referenced_tables += result.referenced_tables;
            gaps.unsupported_entries += result.unsupported_entries;
            gaps.unsupported_functions += result.unsupported_functions;
            gaps.unsupported_conditions += result.unsupported_conditions;
            for (const ov::gameplay::Drop& drop : drops) {
                totals[drop.item] += drop.count;
            }
        }

        std::string out = entity;
        for (const auto& [item, total] : totals) {
            out += ' ';
            out += registries->entry_of(*item_registry, item);
            out += ':';
            out += std::to_string(total);
        }
        if (!gaps.complete()) {
            // Named on the line rather than dropped. A table this build cannot
            // finish must say so, or the comparison reads its missing items as
            // a difference in the rules.
            out += " !gaps:" + std::to_string(gaps.referenced_tables) + "," +
                   std::to_string(gaps.unsupported_entries) + "," +
                   std::to_string(gaps.unsupported_functions) + "," +
                   std::to_string(gaps.unsupported_conditions);
        }
        std::println("{}", out);
    }
    return 0;
}
