#include "ov/registry/registries.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace ov;
using namespace ov::registry;

namespace {

[[nodiscard]] std::filesystem::path pack_path() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack";
}

/// The pack's raw bytes, for the tests that corrupt it on purpose.
[[nodiscard]] std::vector<u8> pack_bytes() {
    std::ifstream file{pack_path(), std::ios::binary};
    return std::vector<u8>{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

/// Byte offsets of the header fields this file pokes at. Spelled out because
/// the test is deliberately reading the format the way an outside tool would,
/// rather than through the struct it is checking.
constexpr usize kOffsetRegistryCount     = 48;
constexpr usize kOffsetRegistriesSection = 56;

[[nodiscard]] const Registries* loaded() {
    static const auto loaded = Registries::load(pack_path());
    return loaded ? &*loaded : nullptr;
}

}  // namespace

TEST_CASE("every hard-coded registry is present", "[registry][ids]") {
    if (loaded() == nullptr) {
        SKIP("no registry pack — run tools/ov_datagen/datagen.py");
    }
    REQUIRE(loaded()->count() == 66);
}

TEST_CASE("the registries the client hard-codes are all there", "[registry][ids]") {
    if (loaded() == nullptr) {
        SKIP("no registry pack");
    }
    const auto* r = loaded();

    // Named individually rather than counted: these are the ones whose ids
    // travel on the wire in 1.20.1, so a missing one is a protocol failure
    // rather than a missing feature.
    for (const std::string_view name :
         {"minecraft:block", "minecraft:item", "minecraft:entity_type",
          "minecraft:block_entity_type", "minecraft:fluid", "minecraft:particle_type",
          "minecraft:menu", "minecraft:sound_event", "minecraft:potion", "minecraft:mob_effect",
          "minecraft:enchantment", "minecraft:painting_variant", "minecraft:recipe_serializer",
          "minecraft:stat_type", "minecraft:command_argument_type"}) {
        INFO(name);
        REQUIRE(r->find(name).has_value());
    }
}

TEST_CASE("counts match the measured dataset", "[registry][ids]") {
    if (loaded() == nullptr) {
        SKIP("no registry pack");
    }
    const auto* r = loaded();

    struct Case {
        std::string_view name;
        usize            size;
    };

    for (const Case& c : std::vector<Case>{
             {"minecraft:block", 1003},
             {"minecraft:item", 1255},
             {"minecraft:entity_type", 124},
             {"minecraft:fluid", 5},
             {"minecraft:block_entity_type", 41},
             {"minecraft:sound_event", 1474},
             {"minecraft:particle_type", 95},
             {"minecraft:mob_effect", 33},
             {"minecraft:enchantment", 39},
             {"minecraft:potion", 43},
         }) {
        INFO(c.name);
        const auto id = r->find(c.name);
        REQUIRE(id.has_value());
        REQUIRE(r->size(*id) == c.size);
    }
}

TEST_CASE("ids are the index, in Mojang's order", "[registry][ids]") {
    if (loaded() == nullptr) {
        SKIP("no registry pack");
    }
    const auto* r  = loaded();
    const auto  id = r->find("minecraft:block");
    REQUIRE(id.has_value());

    // Air is block 0, which is what lets memset(0) produce an empty section.
    REQUIRE(r->protocol_id(*id, "minecraft:air") == 0);
    REQUIRE(r->entry_of(*id, 0) == "minecraft:air");

    // And the mapping is a bijection over the whole registry.
    const auto names = r->entries(*id);
    for (usize i = 0; i < names.size(); ++i) {
        REQUIRE(r->protocol_id(*id, names[i]) == static_cast<ProtocolId>(i));
        REQUIRE(r->entry_of(*id, static_cast<ProtocolId>(i)) == names[i]);
    }
}

TEST_CASE("mob_effect is the one 1-based registry", "[registry][ids]") {
    if (loaded() == nullptr) {
        SKIP("no registry pack");
    }
    const auto* r = loaded();

    // The single exception in the whole dataset, and an off-by-one here means
    // every potion in the game applies the neighbouring effect. Stored as a
    // field rather than special-cased on the name, so the next version can
    // change it in the data.
    const auto effects = r->find("minecraft:mob_effect");
    REQUIRE(effects.has_value());
    REQUIRE(r->first_id(*effects) == 1);
    REQUIRE(r->protocol_id(*effects, "minecraft:speed") == 1);
    REQUIRE(r->entry_of(*effects, 1) == "minecraft:speed");
    REQUIRE(r->entry_of(*effects, 0).empty());

    // Everything else starts at zero.
    usize non_zero = 0;
    for (u16 i = 0; i < r->count(); ++i) {
        if (r->first_id(RegistryId{i}) != 0) {
            ++non_zero;
        }
    }
    REQUIRE(non_zero == 1);
}

TEST_CASE("the six dynamic registries are deliberately absent", "[registry][ids]") {
    if (loaded() == nullptr) {
        SKIP("no registry pack");
    }
    const auto* r = loaded();

    // These are sent to the client as NBT during login, so their ids are ours
    // to choose. Finding them here would mean the emitter had started pinning
    // ids that are not pinned — harmless today, wrong the moment a datapack
    // adds a biome.
    for (const std::string_view name :
         {"minecraft:worldgen/biome", "minecraft:dimension_type", "minecraft:chat_type",
          "minecraft:damage_type", "minecraft:trim_material", "minecraft:trim_pattern"}) {
        INFO(name);
        REQUIRE_FALSE(r->find(name).has_value());
    }
}

// ── What has to be refused ──────────────────────────────────────────────────

TEST_CASE("an unknown name or id yields nothing rather than garbage",
          "[registry][ids][malformed]") {
    if (loaded() == nullptr) {
        SKIP("no registry pack");
    }
    const auto* r  = loaded();
    const auto  id = r->find("minecraft:entity_type");
    REQUIRE(id.has_value());

    REQUIRE_FALSE(r->find("minecraft:nonexistent").has_value());
    REQUIRE_FALSE(r->protocol_id(*id, "minecraft:not_a_mob").has_value());

    // Ids arriving from a peer are unbounded. Reading past the entry table
    // would hand back whatever string followed it.
    REQUIRE(r->entry_of(*id, -1).empty());
    REQUIRE(r->entry_of(*id, 100000).empty());
    REQUIRE(r->entry_of(*id, static_cast<ProtocolId>(r->size(*id))).empty());
}

TEST_CASE("a truncated or mislabelled pack is refused", "[registry][ids][malformed]") {
    REQUIRE(Registries::from_bytes({}).error() == RegistryError::Corrupt);
    REQUIRE(Registries::from_bytes(std::vector<u8>(64, 0)).error() == RegistryError::Corrupt);

    std::vector<u8> wrong_magic(256, 0);
    wrong_magic[0] = 'X';
    REQUIRE(Registries::from_bytes(wrong_magic).error() == RegistryError::Corrupt);

    // Right magic, wrong version: the ids would be plausible and wrong, which
    // is worse than no cache at all.
    std::vector<u8> stale(256, 0);
    stale[0] = 'O';
    stale[1] = 'V';
    stale[2] = 'P';
    stale[3] = 'K';
    stale[4] = 99;
    REQUIRE(Registries::from_bytes(stale).error() == RegistryError::VersionMismatch);
}

TEST_CASE("a registry whose entries leave the table is refused", "[registry][ids][malformed]") {
    if (loaded() == nullptr) {
        SKIP("no registry pack");
    }

    // Take the real pack and move one registry's entry span past the end. The
    // load has to fail; accepting it would index out of bounds on every lookup
    // of that registry, which no later check would catch.
    auto bytes = pack_bytes();
    REQUIRE(bytes.size() > 128);

    u32 registry_count    = 0;
    u32 registries_offset = 0;
    std::memcpy(&registry_count, bytes.data() + kOffsetRegistryCount, sizeof(u32));
    std::memcpy(&registries_offset, bytes.data() + kOffsetRegistriesSection, sizeof(u32));
    REQUIRE(registry_count == 66);
    REQUIRE(Registries::from_bytes(bytes).has_value());

    // Third u32 of the first record is its entry_count.
    const u32 huge = 0x7FFFFFFF;
    std::memcpy(bytes.data() + registries_offset + 8, &huge, sizeof(u32));

    REQUIRE(Registries::from_bytes(bytes).error() == RegistryError::Corrupt);
}
