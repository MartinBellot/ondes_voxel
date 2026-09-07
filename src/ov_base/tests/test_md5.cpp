#include "ov/base/md5.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace ov;

namespace {

std::string hex(const Md5Digest& digest) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string           out;
    for (const u8 byte : digest) {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0F]);
    }
    return out;
}

}  // namespace

TEST_CASE("MD5 matches the RFC 1321 test suite", "[base][md5]") {
    // The vectors from the specification itself, not from our own output. MD5
    // is here only because a version 3 UUID is defined in terms of it, and that
    // definition has to be followed exactly rather than approximately.
    REQUIRE(hex(md5("")) == "d41d8cd98f00b204e9800998ecf8427e");
    REQUIRE(hex(md5("a")) == "0cc175b9c0f1b6a831c399e269772661");
    REQUIRE(hex(md5("abc")) == "900150983cd24fb0d6963f7d28e17f72");
    REQUIRE(hex(md5("message digest")) == "f96b697d7cb7938d525a2f31aaf161d0");
    REQUIRE(hex(md5("abcdefghijklmnopqrstuvwxyz")) == "c3fcd3d76192e4007dfb496cca67e13b");
    REQUIRE(hex(md5("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789")) ==
            "d174ab98d277d9f5a5611c2c9f419d9f");
    REQUIRE(
        hex(md5(
            "12345678901234567890123456789012345678901234567890123456789012345678901234567890")) ==
        "57edf4a22be3c955ac49da2e2107b67a");
}

TEST_CASE("MD5 padding is right at every block boundary", "[base][md5]") {
    // Padding needs a second block when the remainder leaves no room for the
    // length field, and 55 to 56 bytes is exactly that edge. An implementation
    // that gets it wrong is correct for most inputs and wrong for a few, which
    // is the hardest kind of wrong to notice.
    REQUIRE(hex(md5(std::string(55, 'a'))) == "ef1772b6dff9a122358552954ad0df65");
    REQUIRE(hex(md5(std::string(56, 'a'))) == "3b0c8ac703f828b04c6c197006d17218");
    REQUIRE(hex(md5(std::string(63, 'a'))) == "b06521f39153d618550606be297466d5");
    REQUIRE(hex(md5(std::string(64, 'a'))) == "014842d480b571495a4a0363793f7367");
    REQUIRE(hex(md5(std::string(65, 'a'))) == "c743a45e0d2e6a95cb859adae0248435");
}
