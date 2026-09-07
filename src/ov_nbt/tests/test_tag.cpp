#include "ov/nbt/tag.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ov;
using namespace ov::nbt;

TEST_CASE("tag type ids match the format", "[nbt][tag]") {
    // These are the numbers written to disk and to the wire. They are not an
    // internal choice and must never be renumbered.
    STATIC_REQUIRE(static_cast<u8>(TagType::End) == 0);
    STATIC_REQUIRE(static_cast<u8>(TagType::Byte) == 1);
    STATIC_REQUIRE(static_cast<u8>(TagType::Short) == 2);
    STATIC_REQUIRE(static_cast<u8>(TagType::Int) == 3);
    STATIC_REQUIRE(static_cast<u8>(TagType::Long) == 4);
    STATIC_REQUIRE(static_cast<u8>(TagType::Float) == 5);
    STATIC_REQUIRE(static_cast<u8>(TagType::Double) == 6);
    STATIC_REQUIRE(static_cast<u8>(TagType::ByteArray) == 7);
    STATIC_REQUIRE(static_cast<u8>(TagType::String) == 8);
    STATIC_REQUIRE(static_cast<u8>(TagType::List) == 9);
    STATIC_REQUIRE(static_cast<u8>(TagType::Compound) == 10);
    STATIC_REQUIRE(static_cast<u8>(TagType::IntArray) == 11);
    STATIC_REQUIRE(static_cast<u8>(TagType::LongArray) == 12);

    REQUIRE(is_valid_tag_type(12));
    REQUIRE_FALSE(is_valid_tag_type(13));
    REQUIRE_FALSE(is_valid_tag_type(255));
}

TEST_CASE("a tag reports the type it holds", "[nbt][tag]") {
    REQUIRE(Tag{}.type() == TagType::End);
    REQUIRE(Tag{static_cast<i8>(1)}.type() == TagType::Byte);
    REQUIRE(Tag{static_cast<i16>(1)}.type() == TagType::Short);
    REQUIRE(Tag{static_cast<i32>(1)}.type() == TagType::Int);
    REQUIRE(Tag{static_cast<i64>(1)}.type() == TagType::Long);
    REQUIRE(Tag{1.0f}.type() == TagType::Float);
    REQUIRE(Tag{1.0}.type() == TagType::Double);
    REQUIRE(Tag{std::string{"x"}}.type() == TagType::String);
    REQUIRE(Tag{Tag::ByteArray{1, 2}}.type() == TagType::ByteArray);
    REQUIRE(Tag{Tag::IntArray{1, 2}}.type() == TagType::IntArray);
    REQUIRE(Tag{Tag::LongArray{1, 2}}.type() == TagType::LongArray);
    REQUIRE(Tag::make_list(TagType::Int).type() == TagType::List);
    REQUIRE(Tag::make_compound().type() == TagType::Compound);
}

TEST_CASE("numeric access is permissive across integral widths", "[nbt][tag]") {
    // Vanilla has changed a field's tag type between versions more than once.
    // Refusing to read a TAG_Int where a TAG_Byte was expected would fail on
    // real worlds, so as_i64 accepts any numeric tag.
    REQUIRE(Tag{static_cast<i8>(42)}.as_i64() == 42);
    REQUIRE(Tag{static_cast<i16>(42)}.as_i64() == 42);
    REQUIRE(Tag{static_cast<i32>(42)}.as_i64() == 42);
    REQUIRE(Tag{static_cast<i64>(42)}.as_i64() == 42);
    REQUIRE(Tag{42.9}.as_i64() == 42);

    // A non-numeric tag yields the caller's fallback rather than a lie.
    REQUIRE(Tag{std::string{"42"}}.as_i64(-1) == -1);
    REQUIRE(Tag{}.as_i64(-1) == -1);
}

TEST_CASE("booleans are stored as TAG_Byte", "[nbt][tag]") {
    const Tag yes = Tag::make_bool(true);
    const Tag no  = Tag::make_bool(false);

    REQUIRE(yes.type() == TagType::Byte);
    REQUIRE(no.type() == TagType::Byte);
    REQUIRE(yes.as_bool());
    REQUIRE_FALSE(no.as_bool());
    // Anything non-zero is true, as in vanilla.
    REQUIRE(Tag{static_cast<i8>(2)}.as_bool());
    REQUIRE(Tag{}.as_bool(true));
}

TEST_CASE("get_if returns nullptr on a type mismatch", "[nbt][tag]") {
    // NBT from a world or a packet is untrusted: a field that "must" be an int
    // may not be one, and the caller has to be able to find that out.
    const Tag tag{static_cast<i32>(7)};
    REQUIRE(tag.get_if<i32>() != nullptr);
    REQUIRE(*tag.get_if<i32>() == 7);
    REQUIRE(tag.get_if<i64>() == nullptr);
    REQUIRE(tag.get_if<std::string>() == nullptr);
}

TEST_CASE("compounds preserve insertion order", "[nbt][tag]") {
    // This is what makes read-then-write byte-identical, which is the only way
    // to test Anvil support properly.
    Tag compound = Tag::make_compound();
    REQUIRE(compound.put("zebra", Tag{static_cast<i32>(1)}));
    REQUIRE(compound.put("apple", Tag{static_cast<i32>(2)}));
    REQUIRE(compound.put("mango", Tag{static_cast<i32>(3)}));

    const auto& entries = *compound.compound();
    REQUIRE(entries.size() == 3);
    REQUIRE(entries[0].name == "zebra");
    REQUIRE(entries[1].name == "apple");
    REQUIRE(entries[2].name == "mango");
}

TEST_CASE("replacing a key keeps its position", "[nbt][tag]") {
    Tag compound = Tag::make_compound();
    compound.put("a", Tag{static_cast<i32>(1)});
    compound.put("b", Tag{static_cast<i32>(2)});
    compound.put("a", Tag{static_cast<i32>(99)});

    const auto& entries = *compound.compound();
    REQUIRE(entries.size() == 2);
    REQUIRE(entries[0].name == "a");
    REQUIRE(entries[0].value.as_i64() == 99);
    REQUIRE(entries[1].name == "b");
}

TEST_CASE("compound lookup and removal", "[nbt][tag]") {
    Tag compound = Tag::make_compound();
    compound.put("Level", Tag{static_cast<i32>(5)});

    REQUIRE(compound.contains("Level"));
    REQUIRE(compound.find("Level")->as_i64() == 5);
    REQUIRE(compound.find("Missing") == nullptr);
    REQUIRE_FALSE(compound.contains("Missing"));

    REQUIRE(compound.erase("Level"));
    REQUIRE_FALSE(compound.erase("Level"));
    REQUIRE(compound.empty());
}

TEST_CASE("compound operations fail on non-compounds", "[nbt][tag]") {
    Tag number{static_cast<i32>(1)};
    REQUIRE_FALSE(number.put("x", Tag{static_cast<i32>(1)}));
    REQUIRE_FALSE(number.erase("x"));
    REQUIRE(number.find("x") == nullptr);
}

TEST_CASE("lists reject elements of the wrong type", "[nbt][tag]") {
    // NBT lists are homogeneous, and vanilla refuses to load a file that mixes
    // types in one list. Accepting it here would defer the failure to somewhere
    // far harder to diagnose.
    Tag list = Tag::make_list(TagType::Int);
    REQUIRE(list.push(Tag{static_cast<i32>(1)}));
    REQUIRE(list.push(Tag{static_cast<i32>(2)}));
    REQUIRE_FALSE(list.push(Tag{static_cast<i64>(3)}));
    REQUIRE_FALSE(list.push(Tag{std::string{"three"}}));
    REQUIRE(list.size() == 2);
}

TEST_CASE("an empty list still declares an element type", "[nbt][tag]") {
    // The element type is written to disk even when there is nothing in the
    // list, so two empty lists of different types are genuinely different tags.
    const Tag ints    = Tag::make_list(TagType::Int);
    const Tag strings = Tag::make_list(TagType::String);

    REQUIRE(ints.empty());
    REQUIRE(ints.list_element_type() == TagType::Int);
    REQUIRE(strings.list_element_type() == TagType::String);
    REQUIRE_FALSE(ints == strings);
}

TEST_CASE("size reports children or elements", "[nbt][tag]") {
    REQUIRE(Tag{Tag::ByteArray{1, 2, 3}}.size() == 3);
    REQUIRE(Tag{Tag::IntArray{1, 2}}.size() == 2);
    REQUIRE(Tag{Tag::LongArray{1}}.size() == 1);
    REQUIRE(Tag{std::string{"abcd"}}.size() == 4);
    REQUIRE(Tag{static_cast<i32>(1)}.size() == 0);
}

TEST_CASE("nested structures compare structurally", "[nbt][tag]") {
    auto build = [] {
        Tag inner = Tag::make_compound();
        inner.put("x", Tag{1.0});
        Tag list = Tag::make_list(TagType::Compound);
        list.push(std::move(inner));
        Tag root = Tag::make_compound();
        root.put("entries", std::move(list));
        return root;
    };

    REQUIRE(build() == build());

    Tag different = build();
    different.find("entries")->list()->at(0).put("x", Tag{2.0});
    REQUIRE_FALSE(build() == different);
}

TEST_CASE("tag type names match the specification", "[nbt][tag]") {
    REQUIRE(to_string(TagType::End) == "TAG_End");
    REQUIRE(to_string(TagType::ByteArray) == "TAG_Byte_Array");
    REQUIRE(to_string(TagType::LongArray) == "TAG_Long_Array");
    REQUIRE(to_string(TagType::Compound) == "TAG_Compound");
}
