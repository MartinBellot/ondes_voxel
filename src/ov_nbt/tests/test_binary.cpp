#include "ov/nbt/binary.hpp"
#include "ov/nbt/tag.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace ov;
using namespace ov::nbt;

namespace {

std::span<const u8> span_of(const std::vector<u8>& v) {
    return {v.data(), v.size()};
}

/// Encode, decode, and require the result to be identical to the input both
/// structurally and byte for byte. Byte equality is the stronger claim and the
/// one that matters: it is what makes "our save opens in vanilla" testable.
void require_round_trip(const Document& document) {
    const auto encoded = write(document);
    const auto decoded = read(span_of(encoded));
    REQUIRE(decoded.has_value());
    REQUIRE(*decoded == document);

    const auto re_encoded = write(*decoded);
    REQUIRE(re_encoded == encoded);
}

}  // namespace

TEST_CASE("the canonical hello_world.nbt decodes exactly", "[nbt][binary]") {
    // The reference file from the original NBT specification. Hand-written here
    // so the parser is pinned to bytes someone else chose, not to our own
    // encoder's output — a round-trip test alone would pass even if both sides
    // were wrong in the same way.
    const std::vector<u8> bytes{
        0x0A,        // TAG_Compound
        0x00, 0x0B,  // name length 11
        'h',  'e',  'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd',
        0x08,                            // TAG_String
        0x00, 0x04, 'n', 'a', 'm', 'e',  // key "name"
        0x00, 0x09, 'B', 'a', 'n', 'a', 'n', 'r', 'a', 'm', 'a',
        0x00,  // TAG_End
    };

    const auto document = read(span_of(bytes));
    REQUIRE(document.has_value());
    REQUIRE(document->name == "hello world");
    REQUIRE(document->root.type() == TagType::Compound);
    REQUIRE(document->root.size() == 1);

    const Tag* name = document->root.find("name");
    REQUIRE(name != nullptr);
    REQUIRE(name->as_string() == "Bananrama");

    // And re-encoding reproduces the original bytes.
    REQUIRE(write(*document) == bytes);
}

TEST_CASE("every tag type survives a byte-exact round trip", "[nbt][binary]") {
    Tag root = Tag::make_compound();
    root.put("byte", Tag{static_cast<i8>(-128)});
    root.put("short", Tag{static_cast<i16>(-32768)});
    root.put("int", Tag{static_cast<i32>(-2147483647 - 1)});
    root.put("long", Tag{static_cast<i64>(-9223372036854775807LL - 1)});
    root.put("float", Tag{0.49823147f});
    root.put("double", Tag{0.4931287132182315});
    root.put("byte_array", Tag{Tag::ByteArray{0, 62, 34, 16, 8}});
    root.put("string",
             Tag{std::string{"HELLO WORLD THIS IS A TEST STRING \xC3\x85\xC3\x84\xC3\x96!"}});
    root.put("int_array", Tag{Tag::IntArray{1, -2, 3, -2147483648LL + 1}});
    root.put("long_array", Tag{Tag::LongArray{1, -2, 9223372036854775807LL}});

    Tag long_list = Tag::make_list(TagType::Long);
    for (i64 i = 11; i <= 15; ++i) {
        long_list.push(Tag{i});
    }
    root.put("list_of_long", std::move(long_list));

    Tag compound_list = Tag::make_list(TagType::Compound);
    for (int i = 0; i < 2; ++i) {
        Tag entry = Tag::make_compound();
        entry.put("created-on", Tag{static_cast<i64>(1264099775885LL)});
        entry.put("name", Tag{std::string{i == 0 ? "Compound tag #0" : "Compound tag #1"}});
        compound_list.push(std::move(entry));
    }
    root.put("list_of_compound", std::move(compound_list));

    Tag nested = Tag::make_compound();
    Tag ham    = Tag::make_compound();
    ham.put("name", Tag{std::string{"Hampus"}});
    ham.put("value", Tag{0.75f});
    nested.put("ham", std::move(ham));
    root.put("nested", std::move(nested));

    require_round_trip(Document{"Level", std::move(root)});
}

TEST_CASE("an empty list keeps its declared element type through a round trip", "[nbt][binary]") {
    // The element type is on disk even with zero elements. Losing it changes
    // the bytes, and vanilla does write empty typed lists.
    Tag root = Tag::make_compound();
    root.put("empty_ints", Tag::make_list(TagType::Int));
    root.put("empty_compounds", Tag::make_list(TagType::Compound));
    root.put("empty_untyped", Tag::make_list(TagType::End));

    const Document document{"", std::move(root)};
    const auto     encoded = write(document);
    const auto     decoded = read(span_of(encoded));

    REQUIRE(decoded.has_value());
    REQUIRE(decoded->root.find("empty_ints")->list_element_type() == TagType::Int);
    REQUIRE(decoded->root.find("empty_compounds")->list_element_type() == TagType::Compound);
    REQUIRE(decoded->root.find("empty_untyped")->list_element_type() == TagType::End);
    REQUIRE(write(*decoded) == encoded);
}

TEST_CASE("strings with astral characters round-trip", "[nbt][binary]") {
    // Modified UTF-8 writes these as surrogate pairs. A sign with an emoji is
    // the realistic case, and getting it wrong corrupts a chunk on save.
    Tag root = Tag::make_compound();
    root.put("text", Tag{std::string{"Sign \xF0\x9F\x98\x80 line"}});
    root.put("with_null", Tag{std::string{"a\0b", 3}});
    require_round_trip(Document{"", std::move(root)});
}

TEST_CASE("an empty compound round-trips", "[nbt][binary]") {
    require_round_trip(Document{"", Tag::make_compound()});
    require_round_trip(Document{"named", Tag::make_compound()});
}

TEST_CASE("deeply nested but legal data is accepted", "[nbt][binary]") {
    Tag current = Tag::make_compound();
    for (u32 i = 0; i < 100; ++i) {
        Tag parent = Tag::make_compound();
        parent.put("child", std::move(current));
        current = std::move(parent);
    }
    require_round_trip(Document{"", std::move(current)});
}

// ── Malformed input ─────────────────────────────────────────────────────────
// This parser is reachable from a region file on disk and from a packet on the
// network. Every one of these must be an error value, never a crash.

TEST_CASE("truncated input is rejected", "[nbt][binary][malformed]") {
    const std::vector<u8> full{
        0x0A, 0x00, 0x03, 'a', 'b', 'c', 0x03, 0x00, 0x01, 'x', 0x00, 0x00, 0x00, 0x2A, 0x00,
    };
    REQUIRE(read(span_of(full)).has_value());

    // Every proper prefix must fail cleanly rather than read past the end.
    for (usize length = 1; length < full.size(); ++length) {
        const std::vector<u8> partial{full.begin(), full.begin() + static_cast<isize>(length)};
        const auto            result = read(span_of(partial));
        REQUIRE_FALSE(result.has_value());
    }
}

TEST_CASE("empty input is rejected", "[nbt][binary][malformed]") {
    const std::vector<u8> empty;
    const auto            result = read(span_of(empty));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == NbtError::UnexpectedEnd);
}

TEST_CASE("an unknown tag type is rejected", "[nbt][binary][malformed]") {
    const std::vector<u8> bytes{0x63, 0x00, 0x00};
    const auto            result = read(span_of(bytes));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == NbtError::UnknownTagType);
}

TEST_CASE("a non-compound root is rejected", "[nbt][binary][malformed]") {
    const std::vector<u8> bytes{0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    const auto            result = read(span_of(bytes));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == NbtError::RootNotCompound);
}

TEST_CASE("a negative array length is rejected", "[nbt][binary][malformed]") {
    // Lengths are signed on disk. A negative one is malformed, not empty —
    // and casting it to unsigned would produce a four-billion-element read.
    const std::vector<u8> bytes{
        0x0A, 0x00, 0x00, 0x07, 0x00, 0x01, 'a', 0xFF, 0xFF, 0xFF, 0xFF,  // length = -1
        0x00,
    };
    const auto result = read(span_of(bytes));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == NbtError::NegativeLength);
}

TEST_CASE("a length larger than the buffer is rejected before allocating",
          "[nbt][binary][malformed]") {
    // The shape of a real denial-of-service: eleven bytes of file declaring a
    // two-billion-element array. It must fail on the bound, not in the
    // allocator.
    for (const u8 type : {u8{0x07}, u8{0x0B}, u8{0x0C}}) {
        const std::vector<u8> bytes{
            0x0A, 0x00, 0x00, type, 0x00, 0x01, 'a', 0x7F, 0xFF, 0xFF, 0xFF,  // length = 2147483647
            0x00,
        };
        const auto result = read(span_of(bytes));
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error() == NbtError::LengthTooLarge);
    }
}

TEST_CASE("a huge list length is rejected before allocating", "[nbt][binary][malformed]") {
    const std::vector<u8> bytes{
        0x0A, 0x00, 0x00, 0x09, 0x00, 0x01, 'a',
        0x04,                    // element type TAG_Long
        0x7F, 0xFF, 0xFF, 0xFF,  // 2147483647 elements
        0x00,
    };
    const auto result = read(span_of(bytes));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == NbtError::LengthTooLarge);
}

TEST_CASE("unbounded nesting is rejected instead of overflowing the stack",
          "[nbt][binary][malformed]") {
    // A file that is nothing but nested lists would otherwise recurse until the
    // thread's stack runs out — a crash reachable from a region file or a
    // packet. Vanilla's limit is 512; anything past it is hostile, not data.
    // Root compound with one unnamed TAG_List entry, then a chain of lists each
    // holding exactly one list. Inside a list, elements carry no type byte and
    // no name of their own — only the list header declares them.
    std::vector<u8> bytes{0x0A, 0x00, 0x00,   // TAG_Compound, empty name
                          0x09, 0x00, 0x00};  // entry: TAG_List, empty name
    for (u32 i = 0; i < kMaxNestingDepth + 50; ++i) {
        bytes.insert(bytes.end(), {0x09, 0x00, 0x00, 0x00, 0x01});  // list of 1 list
    }
    bytes.insert(bytes.end(), {0x00, 0x00, 0x00, 0x00, 0x00});  // innermost: empty
    bytes.push_back(0x00);                                      // compound end

    const auto result = read(span_of(bytes));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == NbtError::TooDeep);
}

TEST_CASE("nesting just under the limit is still accepted", "[nbt][binary][malformed]") {
    // The guard has to reject hostile depth without rejecting legitimate data.
    // A test that only checks the rejection side would pass with the limit set
    // to zero.
    constexpr u32   kDepth = kMaxNestingDepth - 4;
    std::vector<u8> bytes{0x0A, 0x00, 0x00, 0x09, 0x00, 0x00};
    for (u32 i = 0; i < kDepth; ++i) {
        bytes.insert(bytes.end(), {0x09, 0x00, 0x00, 0x00, 0x01});
    }
    bytes.insert(bytes.end(), {0x00, 0x00, 0x00, 0x00, 0x00});
    bytes.push_back(0x00);

    const auto result = read(span_of(bytes));
    REQUIRE(result.has_value());
    REQUIRE(write(*result) == bytes);
}

TEST_CASE("a list of TAG_End with a non-zero count is rejected", "[nbt][binary][malformed]") {
    // TAG_End has no payload, so a list claiming to hold some of them is
    // lying about its own size.
    const std::vector<u8> bytes{
        0x0A, 0x00, 0x00, 0x09, 0x00, 0x01, 'a',
        0x00,                    // element type TAG_End
        0x00, 0x00, 0x00, 0x05,  // but five of them
        0x00,
    };
    const auto result = read(span_of(bytes));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == NbtError::UnknownTagType);
}

TEST_CASE("a malformed string is rejected", "[nbt][binary][malformed]") {
    const std::vector<u8> bytes{
        0x0A, 0x00, 0x00, 0x08, 0x00, 0x01, 'a', 0x00, 0x02, 0xC3, 0x28,  // bad continuation byte
        0x00,
    };
    const auto result = read(span_of(bytes));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == NbtError::MalformedString);
}

TEST_CASE("every single-byte corruption of a valid document is handled",
          "[nbt][binary][malformed]") {
    // A cheap stand-in for the fuzzer that arrives later: flip each byte of a
    // valid document to a few hostile values and require the parser to either
    // succeed or return an error — never to crash or read out of bounds. Run
    // under ASan in CI, this catches the whole class.
    Tag root = Tag::make_compound();
    root.put("i", Tag{static_cast<i32>(7)});
    root.put("s", Tag{std::string{"hello"}});
    Tag list = Tag::make_list(TagType::Int);
    list.push(Tag{static_cast<i32>(1)});
    root.put("l", std::move(list));
    root.put("a", Tag{Tag::IntArray{1, 2, 3}});

    const auto original = write(Document{"root", std::move(root)});

    for (usize i = 0; i < original.size(); ++i) {
        for (const u8 replacement : {u8{0x00}, u8{0xFF}, u8{0x7F}, u8{0x80}, u8{0x0C}}) {
            auto corrupted = original;
            corrupted[i]   = replacement;
            // The contract is simply that this returns.
            const auto result = read(span_of(corrupted));
            if (result.has_value()) {
                // If it parsed, it must also re-encode without exploding.
                (void)write(*result);
            }
        }
    }
}

TEST_CASE("errors have readable names", "[nbt][binary]") {
    REQUIRE(to_string(NbtError::UnexpectedEnd) == "unexpected end of data");
    REQUIRE(to_string(NbtError::TooDeep) == "nesting too deep");
    REQUIRE(to_string(NbtError::NegativeLength) == "negative length");
}
