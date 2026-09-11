// The trading screen's packets against what a real 1.20.1 server sent
// (measure_villagers.py `packet`): Merchant Offers byte for byte, the title,
// Select Trade, and the tags on what a villager sells.
#include "../src/merchant_session.hpp"

#include "ov/gameplay/trading.hpp"
#include "ov/nbt/binary.hpp"
#include "ov/nbt/tag.hpp"
#include "ov/registry/registries.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

using namespace ov;
using namespace ov::server;

namespace {

[[nodiscard]] const registry::Registries* registries() {
    static const auto loaded = registry::Registries::load(
        std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack");
    return loaded ? &*loaded : nullptr;
}

[[nodiscard]] std::vector<u8> from_hex(std::string_view hex) {
    std::vector<u8> out;
    for (usize i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<u8>(std::stoi(std::string{hex.substr(i, 2)}, nullptr, 16)));
    }
    return out;
}

/// The packet the real server sent for a librarian (level 2, Xp 37) whose
/// offers were written by hand: 17 emeralds and a book for a Mending book,
/// used 3 of 12, demand 4; 24 paper for an emerald, used 16 of 16, demand -7.
constexpr std::string_view kCaptured =
    "010201fd05110001ac08010a000009001253746f726564456e6368616e746d656e74730a0000000102"
    "00036c766c0001080002696400116d696e6563726166743a6d656e64696e67000001f5060100000000"
    "00030000000c00000005000000003e4ccccd0000000401f406180001fd0501000001000000100000"
    "001000000002000000003d4ccccdfffffff902250101";

}  // namespace

TEST_CASE("Merchant Offers, against the real server's bytes", "[villager][parity]") {
    const registry::Registries* r = registries();
    REQUIRE(r != nullptr);
    const auto items = r->find("minecraft:item");
    REQUIRE(items);

    gameplay::VillagerState v;
    v.active     = true;
    v.profession = gameplay::Profession::Librarian;
    v.level      = 2;
    v.xp         = 37;

    gameplay::MerchantOffer book;
    book.cost_a       = gameplay::TradeItem{.item = "minecraft:emerald", .count = 17};
    book.cost_b       = gameplay::TradeItem{.item = "minecraft:book", .count = 1};
    book.result.item  = "minecraft:enchanted_book";
    book.result.count = 1;
    book.result.stored = true;
    book.result.enchantments.set(gameplay::Enchantment::Mending, 1);
    book.uses             = 3;
    book.max_uses         = 12;
    book.xp               = 5;
    book.price_multiplier = 0.2F;
    book.demand           = 4;

    gameplay::MerchantOffer paper;
    paper.cost_a           = gameplay::TradeItem{.item = "minecraft:paper", .count = 24};
    paper.result           = gameplay::TradeItem{.item = "minecraft:emerald", .count = 1};
    paper.uses             = 16;
    paper.max_uses         = 16;
    paper.xp               = 2;
    paper.price_multiplier = 0.05F;
    paper.demand           = -7;
    v.offers               = {book, paper};

    const std::vector<u8> ours     = encode_merchant_offers(*r, *items, 1, v);
    const std::vector<u8> captured = from_hex(kCaptured);
    REQUIRE(ours.size() == captured.size());

    // The book's tag starts at byte 11 (window, count, the first cost, the
    // result's id and count) and ends before the second cost (01 f5 06). Its
    // keys come in HashMap order on the real server — `lvl` before `id` — and
    // NBT compares tag by tag, never byte by byte (briefing, trap 25).
    constexpr usize kTagStart = 11;
    usize           tag_end   = kTagStart;
    while (tag_end + 2 < captured.size() &&
           !(captured[tag_end] == 0x01 && captured[tag_end + 1] == 0xf5 && captured[tag_end + 2] == 0x06)) {
        ++tag_end;
    }
    REQUIRE(tag_end < captured.size());
    CHECK(std::vector<u8>(ours.begin(), ours.begin() + kTagStart) ==
          std::vector<u8>(captured.begin(), captured.begin() + kTagStart));
    CHECK(std::vector<u8>(ours.begin() + static_cast<std::ptrdiff_t>(tag_end), ours.end()) ==
          std::vector<u8>(captured.begin() + static_cast<std::ptrdiff_t>(tag_end), captured.end()));

    const auto read_book = [&](const std::vector<u8>& bytes) {
        const auto doc = nbt::read(std::span<const u8>{bytes}.subspan(kTagStart, tag_end - kTagStart));
        REQUIRE(doc);
        const nbt::Tag* list = doc->root.find("StoredEnchantments");
        REQUIRE(list != nullptr);
        REQUIRE(list->list() != nullptr);
        REQUIRE(list->list()->size() == 1);
        const nbt::Tag& entry = list->list()->front();
        return std::pair{std::string{entry.find("id")->as_string()}, entry.find("lvl")->as_i64()};
    };
    CHECK(read_book(ours) == read_book(captured));
    CHECK(read_book(ours).first == "minecraft:mending");
}

TEST_CASE("the trading screen's title is the villager's name", "[villager][parity]") {
    gameplay::VillagerState v;
    v.profession = gameplay::Profession::Librarian;
    const net::Uuid uuid{0x00004F5600000000ULL, 0x0000000000000922ULL};
    // Captured from the real server's Open Screen.
    CHECK(merchant_title(v, uuid) ==
          R"({"insertion":"00004f56-0000-0000-0000-000000000922","hoverEvent":{"action":)"
          R"("show_entity","contents":{"type":"minecraft:villager","id":)"
          R"("00004f56-0000-0000-0000-000000000922","name":{"translate":)"
          R"("entity.minecraft.villager.librarian"}}},"translate":)"
          R"("entity.minecraft.villager.librarian"})");
}

TEST_CASE("Select Trade is one VarInt", "[villager]") {
    CHECK(parse_select_trade(std::vector<u8>{0x00}) == 0);
    CHECK(parse_select_trade(std::vector<u8>{0x05}) == 5);
    CHECK(parse_select_trade(std::vector<u8>{0x80, 0x01}) == 128);
    CHECK_FALSE(parse_select_trade(std::vector<u8>{}).has_value());
    CHECK_FALSE(parse_select_trade(std::vector<u8>{0x05, 0x00}).has_value());
}

TEST_CASE("what a villager sells carries the tags the real one's does", "[villager]") {
    const registry::Registries* r = registries();
    REQUIRE(r != nullptr);
    const auto items = r->find("minecraft:item");
    REQUIRE(items);

    // A tool: Damage 0, as every damageable stack a villager sold did.
    const net::ItemStack axe =
        trade_stack(*r, *items, gameplay::TradeItem{.item = "minecraft:stone_axe", .count = 1});
    REQUIRE_FALSE(axe.nbt.empty());
    const auto axe_tag = nbt::read(axe.nbt);
    REQUIRE(axe_tag);
    REQUIRE(axe_tag->root.find("Damage") != nullptr);
    CHECK(axe_tag->root.find("Damage")->as_i64() == 0);

    // A plain stack has no tag at all.
    CHECK(trade_stack(*r, *items, gameplay::TradeItem{.item = "minecraft:paper", .count = 24})
              .nbt.empty());

    // Leather armour: display.color.
    gameplay::TradeItem pants{.item = "minecraft:leather_leggings", .count = 1};
    pants.dye_colour = 16383998;
    const auto pants_tag = nbt::read(trade_stack(*r, *items, pants).nbt);
    REQUIRE(pants_tag);
    REQUIRE(pants_tag->root.find("display") != nullptr);
    CHECK(pants_tag->root.find("display")->find("color")->as_i64() == 16383998);
}
