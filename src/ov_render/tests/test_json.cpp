#include "../src/json.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace ov;
using namespace ov::render;

namespace {

json::Document parse_ok(std::string_view text) {
    auto document = json::Document::parse(text);
    REQUIRE(document.has_value());
    return std::move(*document);
}

}  // namespace

TEST_CASE("scalars round-trip through the flat document", "[json]") {
    const auto document = parse_ok(R"({"a": 1.5, "b": "text", "c": true, "d": null})");
    const auto root     = document.root();

    REQUIRE(root.is_object());
    CHECK(root.size() == 4);
    CHECK(root["a"].as_number() == 1.5);
    CHECK(root["b"].as_string() == "text");
    CHECK(root["c"].as_bool());
    CHECK(root["d"].kind() == json::Kind::Null);
}

TEST_CASE("a missing member is an invalid value, not a crash", "[json]") {
    const auto document = parse_ok(R"({"a": 1})");

    // Chaining through absent members has to be safe: model files are full of
    // optional fields and every one of them would otherwise need a guard.
    CHECK_FALSE(document.root()["nope"].valid());
    CHECK(document.root()["nope"]["deeper"].as_number(42.0) == 42.0);
    CHECK(document.root()["nope"].as_string("fallback") == "fallback");
}

TEST_CASE("arrays keep their order", "[json]") {
    const auto document = parse_ok(R"({"uv": [0, 8, 16, 16]})");
    const auto uv       = document.root()["uv"];

    REQUIRE(uv.is_array());
    REQUIRE(uv.size() == 4);
    CHECK(uv[0].as_number() == 0.0);
    CHECK(uv[1].as_number() == 8.0);
    CHECK(uv[3].as_number() == 16.0);
    CHECK_FALSE(uv[4].valid());
}

TEST_CASE("object members are readable by position and by name", "[json]") {
    const auto document = parse_ok(R"({"down": 1, "up": 2})");
    const auto root     = document.root();

    CHECK(root.key_at(0) == "down");
    CHECK(root.value_at(0).as_number() == 1.0);
    CHECK(root.key_at(1) == "up");
    CHECK(root["up"].as_number() == 2.0);
}

TEST_CASE("the empty key is a key", "[json]") {
    // Not a curiosity: a blockstate file for a block with one model writes
    // `"variants": { "": { "model": ... } }`, and treating "" as absent loses
    // every such block.
    const auto document = parse_ok(R"({"variants": {"": {"model": "block/stone"}}})");
    const auto variants = document.root()["variants"];

    REQUIRE(variants.is_object());
    REQUIRE(variants.size() == 1);
    CHECK(variants.key_at(0).empty());
    CHECK(variants[""]["model"].as_string() == "block/stone");
}

TEST_CASE("nesting deeper than the limit is refused, not survived", "[json]") {
    // A pack is a file someone downloaded. Deep nesting has to come back as an
    // error rather than as a stack overflow, which is why the walk is
    // iterative and the limit explicit.
    std::string deep;
    for (u32 i = 0; i < json::kMaxDepth + 10; ++i) {
        deep += '[';
    }
    deep += '1';
    for (u32 i = 0; i < json::kMaxDepth + 10; ++i) {
        deep += ']';
    }

    const auto document = json::Document::parse(deep);
    REQUIRE_FALSE(document.has_value());
    CHECK(document.error() == json::ParseError::TooDeep);
}

TEST_CASE("malformed input is an error", "[json]") {
    CHECK_FALSE(json::Document::parse("{").has_value());
    CHECK_FALSE(json::Document::parse("").has_value());
    CHECK_FALSE(json::Document::parse(R"({"a": })").has_value());
}
