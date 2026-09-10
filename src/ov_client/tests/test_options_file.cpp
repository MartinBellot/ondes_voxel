#include "ov/client/options_file.hpp"

#include <catch2/catch_test_macros.hpp>

#include <charconv>
#include <cmath>
#include <string>

using namespace ov;
using namespace ov::client;

// options.txt in vanilla's format. The lines below are the shapes a real
// 1.20.1 client writes (docs/provenance/ecrans.md): a fixture file written by
// the real client is round-tripped in the test further down.

TEST_CASE("doubles are written the way Java writes them", "[options]") {
    CHECK(java_double(0.0) == "0.0");
    CHECK(java_double(1.0) == "1.0");
    CHECK(java_double(0.5) == "0.5");
    CHECK(java_double(0.75) == "0.75");
    CHECK(java_double(0.25) == "0.25");
    CHECK(java_double(-0.5) == "-0.5");
    CHECK(java_double(0.4375) == "0.4375");
    CHECK(java_double(0.001) == "0.001");
    CHECK(java_double(0.0001) == "1.0E-4");
    CHECK(java_double(1.0e7) == "1.0E7");
    CHECK(java_double(12.0) == "12.0");
    // The shortest digits that read back to the same double.
    const f64    odd  = 0.3380281690140845;
    const auto   text = java_double(odd);
    f64          back = 0.0;
    std::from_chars(text.data(), text.data() + text.size(), back);
    CHECK(back == odd);
    CHECK(text == "0.3380281690140845");
}

TEST_CASE("a file is written back line for line, unknown keys included", "[options]") {
    const std::string text =
        "version:3465\n"
        "autoJump:false\n"
        "fov:0.0\n"
        "renderClouds:\"true\"\n"
        "resourcePacks:[\"vanilla\",\"file/Faithful 32x - 1.20.1.zip\"]\n"
        "lastServer:\n"
        "lang:fr_fr\n"
        "key_key.sneak:key.keyboard.q\n"
        "soundCategory_master:0.0\n";
    const OptionsFile file = OptionsFile::parse(text);
    CHECK(file.size() == 9);
    CHECK(file.serialize() == text);
    REQUIRE(file.get("resourcePacks"));
    CHECK(*file.get("resourcePacks") == "[\"vanilla\",\"file/Faithful 32x - 1.20.1.zip\"]");
    CHECK(*file.get("lastServer") == "");
    CHECK_FALSE(file.get("nonsense"));
}

TEST_CASE("setting a value replaces it in place and appends a new key", "[options]") {
    OptionsFile file = OptionsFile::parse("a:1\nfov:0.0\nb:2\n");
    file.set("fov", "0.5");
    file.set("new", "x");
    CHECK(file.serialize() == "a:1\nfov:0.5\nb:2\nnew:x\n");
}

TEST_CASE("the typed options read and write vanilla's encodings", "[options]") {
    const OptionsFile file = OptionsFile::parse(
        "fov:0.5\nrenderDistance:8\nguiScale:3\nmouseSensitivity:0.75\nenableVsync:false\n"
        "maxFps:60\nlang:fr_fr\nsoundCategory_music:0.25\nsoundCategory_block:0.5\n"
        "key_key.sneak:key.keyboard.q\nkey_key.drop:key.keyboard.z\n");
    std::vector<std::string> problems;
    const GameOptions        options = GameOptions::from(file, &problems);
    CHECK(problems.empty());
    CHECK(options.fov == 90);  // (90 − 70) / 40 = 0.5
    CHECK(options.render_distance == 8);
    CHECK(options.gui_scale == 3);
    CHECK(options.mouse_sensitivity == 0.75);
    CHECK_FALSE(options.vsync);
    CHECK(options.max_fps == 60);
    CHECK(options.language == "fr_fr");
    CHECK(options.volumes[1] == 0.25);
    CHECK(options.volumes[4] == 0.5);
    CHECK(options.volumes[0] == 1.0);
    REQUIRE(options.binding("key.sneak"));
    CHECK(key_name(options.binding("key.sneak")->code) == "key.keyboard.q");
    CHECK(key_name(options.binding("key.forward")->code) == "key.keyboard.w");

    OptionsFile written;
    options.store(written);
    CHECK(*written.get("fov") == "0.5");
    CHECK(*written.get("mouseSensitivity") == "0.75");
    CHECK(*written.get("enableVsync") == "false");
    CHECK(*written.get("soundCategory_music") == "0.25");
    CHECK(*written.get("soundCategory_master") == "1.0");
    CHECK(*written.get("key_key.drop") == "key.keyboard.z");
    CHECK(*written.get("key_key.smoothCamera") == "key.keyboard.unknown");
    CHECK(*written.get("key_key.attack") == "key.mouse.left");
}

TEST_CASE("a value that does not read is named, and the default kept", "[options]") {
    std::vector<std::string> problems;
    const GameOptions        options =
        GameOptions::from(OptionsFile::parse("renderDistance:far\nfov:x\n"), &problems);
    CHECK(options.render_distance == 12);
    CHECK(options.fov == 70);
    CHECK(problems.size() == 2);
}

TEST_CASE("every key name reads back to its code", "[options]") {
    usize named = 0;
    for (i32 code = 0; code < kMouseCodeBase + 8; ++code) {
        const std::string name = key_name(code);
        if (name == "key.keyboard.unknown") {
            continue;
        }
        ++named;
        CHECK(key_code(name) == code);
    }
    CHECK(named > 100);
    CHECK(key_code("key.keyboard.unknown") == kUnknownKey);
    CHECK(key_code("key.keyboard.left.shift") == 340);
    CHECK(key_code("key.keyboard.f11") == 300);
}

TEST_CASE("the mouse-look rate at the default sensitivity is 0.15 degrees a pixel",
          "[options]") {
    CHECK(std::abs(degrees_per_pixel(0.5) - 0.15F) < 1e-6F);
    CHECK(degrees_per_pixel(0.0) < degrees_per_pixel(1.0));
}
