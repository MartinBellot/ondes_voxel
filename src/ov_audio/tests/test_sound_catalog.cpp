#include "ov/audio/sound_catalog.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <map>
#include <string>

using ov::audio::SoundCatalog;
using ov::audio::SoundEntry;

namespace {

// The shapes sounds.json actually uses: a bare string, an object with the
// optional fields, a weight, an event reference with its own factors.
constexpr std::string_view kDocument = R"json({
  "block.stone.place": {
    "subtitle": "subtitles.block.generic.place",
    "sounds": ["dig/stone1", "dig/stone2", {"name": "dig/stone3", "weight": 2}]
  },
  "entity.cow.ambient": {
    "sounds": [{"name": "mob/cow/say1", "volume": 0.5, "pitch": 1.2,
                "attenuation_distance": 8, "stream": false, "preload": true}]
  },
  "entity.parrot.imitate.cow": {
    "sounds": [{"name": "entity.cow.ambient", "type": "event", "volume": 0.6, "pitch": 1.8}]
  },
  "music.game": {
    "sounds": [{"name": "music/game/calm1", "stream": true}]
  },
  "other.dangling": {
    "sounds": [{"name": "does.not.exist", "type": "event"}]
  },
  "other.empty": {}
})json";

SoundCatalog parsed() {
    auto catalog = SoundCatalog::parse(kDocument);
    REQUIRE(catalog.has_value());
    return std::move(*catalog);
}

}  // namespace

TEST_CASE("sounds.json: events are qualified and files become pack paths", "[audio][catalog]") {
    const SoundCatalog catalog = parsed();
    CHECK(catalog.event_count() == 6);
    CHECK(catalog.entry_count() == 7);

    const auto* stone = catalog.find("minecraft:block.stone.place");
    REQUIRE(stone != nullptr);
    CHECK(catalog.find("block.stone.place") == stone);
    CHECK(stone->subtitle == "subtitles.block.generic.place");
    REQUIRE(stone->entries.size() == 3);
    CHECK(stone->entries[0].name == "assets/minecraft/sounds/dig/stone1.ogg");
    CHECK(stone->entries[2].weight == 2);

    const auto* cow = catalog.find("entity.cow.ambient");
    REQUIRE(cow != nullptr);
    const SoundEntry& say = cow->entries[0];
    CHECK_THAT(say.volume, Catch::Matchers::WithinAbs(0.5, 1e-6));
    CHECK_THAT(say.pitch, Catch::Matchers::WithinAbs(1.2, 1e-6));
    CHECK(say.attenuation_distance == 8);
    CHECK(say.preload);
    CHECK_FALSE(say.stream);
    CHECK(catalog.find("music.game")->entries[0].stream);
    CHECK(catalog.find("minecraft:nothing") == nullptr);
}

TEST_CASE("sounds.json: the default fields are the documented ones", "[audio][catalog]") {
    const SoundCatalog catalog = parsed();
    const SoundEntry&  plain   = catalog.find("block.stone.place")->entries[0];
    CHECK(plain.volume == 1.0F);
    CHECK(plain.pitch == 1.0F);
    CHECK(plain.weight == 1);
    CHECK(plain.attenuation_distance == 16);
    CHECK_FALSE(plain.stream);
    CHECK_FALSE(plain.preload);
    CHECK(plain.kind == SoundEntry::Kind::File);
}

TEST_CASE("sounds.json: a weighted choice follows the weights", "[audio][catalog]") {
    const SoundCatalog         catalog = parsed();
    std::map<std::string, int> counts;
    // Spread seeds, like the random 64-bit ones the server sends. Consecutive
    // small seeds are not: a Java-style generator's first draw barely moves
    // between seed 0 and seed 4000, and every one of them picked the same
    // variant the first time this test ran.
    ov::u64 state = 0x243F6A8885A308D3ULL;
    for (int draw = 0; draw < 4000; ++draw) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        const auto chosen = catalog.choose("block.stone.place", static_cast<ov::i64>(state));
        REQUIRE(chosen.has_value());
        ++counts[chosen->file->name];
    }
    // Weights 1, 1, 2 over 4000 draws: 1000, 1000, 2000 expected.
    CHECK(counts["assets/minecraft/sounds/dig/stone1.ogg"] > 850);
    CHECK(counts["assets/minecraft/sounds/dig/stone1.ogg"] < 1150);
    CHECK(counts["assets/minecraft/sounds/dig/stone3.ogg"] > 1800);
    CHECK(counts["assets/minecraft/sounds/dig/stone3.ogg"] < 2200);

    // One seed is one sound, every time: the server's seed names a variant.
    const auto a = catalog.choose("block.stone.place", 1234);
    const auto b = catalog.choose("block.stone.place", 1234);
    CHECK(a->file == b->file);
}

TEST_CASE("sounds.json: an event reference resolves and multiplies", "[audio][catalog]") {
    const SoundCatalog catalog = parsed();
    const auto         chosen  = catalog.choose("entity.parrot.imitate.cow", 7);
    REQUIRE(chosen.has_value());
    CHECK(chosen->file->name == "assets/minecraft/sounds/mob/cow/say1.ogg");
    CHECK_THAT(chosen->volume, Catch::Matchers::WithinAbs(0.5 * 0.6, 1e-6));
    CHECK_THAT(chosen->pitch, Catch::Matchers::WithinAbs(1.2 * 1.8, 1e-5));

    std::vector<const SoundEntry*> files;
    catalog.files_of("entity.parrot.imitate.cow", files);
    REQUIRE(files.size() == 1);
    CHECK(files[0]->name == "assets/minecraft/sounds/mob/cow/say1.ogg");
}

TEST_CASE("sounds.json: dead ends are empty, not silence", "[audio][catalog]") {
    const SoundCatalog catalog = parsed();
    CHECK_FALSE(catalog.choose("other.dangling", 1).has_value());
    CHECK_FALSE(catalog.choose("other.empty", 1).has_value());
    CHECK_FALSE(catalog.choose("nothing.at.all", 1).has_value());
    CHECK(catalog.preloaded().size() == 1);
}

TEST_CASE("sounds.json: an unknown entry type is refused by name", "[audio][catalog]") {
    const auto catalog =
        SoundCatalog::parse(R"({"a": {"sounds": [{"name": "x", "type": "synth"}]}})");
    REQUIRE_FALSE(catalog.has_value());
    CHECK(catalog.error().find("synth") != std::string::npos);
    CHECK_FALSE(SoundCatalog::parse("[1, 2]").has_value());
    CHECK_FALSE(SoundCatalog::parse("{").has_value());
}

TEST_CASE("sounds.json: a namespaced file keeps its namespace", "[audio][catalog]") {
    const auto catalog = SoundCatalog::parse(R"({"x": {"sounds": ["ondes:beep/one"]}})", "ondes");
    REQUIRE(catalog.has_value());
    const auto* event = catalog->find("ondes:x");
    REQUIRE(event != nullptr);
    CHECK(event->entries[0].name == "assets/ondes/sounds/beep/one.ogg");
}
