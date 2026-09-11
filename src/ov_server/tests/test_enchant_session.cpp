// The anvil and the grindstone against the real server's panel, and the three
// windows driven through their packets.
//
// scripts/measure_enchanting.py put each pair of stacks — written as SNBT, the
// way `/item replace` takes them — into a real 1.20.1 anvil and grindstone and
// recorded the cost and the output stack, NBT included. The SNBT is parsed
// here by the command engine's own reader, so the inputs are exactly what the
// real server was given. When the measurement is missing, the parity cases say
// so and skip.
#include "../src/enchant_session.hpp"

#include "../src/commands/snbt.hpp"
#include "../src/commands/string_reader.hpp"

#include "ov/nbt/binary.hpp"
#include "ov/nbt/tag.hpp"
#include "ov/protocol/recipe_packets.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

using namespace ov;
using namespace ov::server;

namespace {

struct Loaded {
    std::optional<registry::Registries> registries;
    EnchantContext                      context;
};

[[nodiscard]] const Loaded& loaded() {
    static const Loaded state = [] {
        Loaded     out;
        const auto path = std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" /
                          "registry.ovpack";
        auto regs = registry::Registries::load(path);
        if (regs) {
            out.registries = std::move(*regs);
            out.context.registries    = &*out.registries;
            out.context.item_registry = *out.registries->find("minecraft:item");
            out.context.menu_registry = *out.registries->find("minecraft:menu");
            out.context.block_registry = *out.registries->find("minecraft:block");
        }
        return out;
    }();
    return state;
}

/// `minecraft:diamond_sword{…} 2` → a stack, the way `/item replace` reads it.
[[nodiscard]] net::ItemStack stack_of(const EnchantContext& context, std::string_view spec) {
    net::ItemStack out;
    if (spec.empty()) {
        return out;
    }
    usize end = 0;
    while (end < spec.size() && spec[end] != '{' && spec[end] != ' ') {
        ++end;
    }
    const auto id = context.registries->protocol_id(context.item_registry, spec.substr(0, end));
    REQUIRE(id.has_value());
    out.item_id = *id;
    out.count   = 1;
    std::string_view rest = spec.substr(end);
    if (!rest.empty() && rest.front() == '{') {
        server::cmd::StringReader reader{rest};
        auto                      tag = server::cmd::read_snbt_compound(reader);
        REQUIRE(tag.has_value());
        out.nbt = nbt::write(nbt::Document{"tag", *tag});
        rest    = reader.remaining();
    }
    while (!rest.empty() && rest.front() == ' ') {
        rest.remove_prefix(1);
    }
    if (!rest.empty()) {
        out.count = static_cast<i8>(std::stoi(std::string{rest}));
    }
    return out;
}

/// The output tag as vanilla wrote it into the JSON — nested maps, lists and
/// numbers — compared to ours key by key.
[[nodiscard]] std::string canonical(const nbt::Tag& tag) {
    std::ostringstream out;
    switch (tag.type()) {
        case nbt::TagType::Compound: {
            std::map<std::string, std::string> sorted;
            for (const auto& entry : *tag.compound()) {
                sorted[entry.name] = canonical(entry.value);
            }
            out << '{';
            for (const auto& [k, v] : sorted) {
                out << k << ':' << v << ',';
            }
            out << '}';
            break;
        }
        case nbt::TagType::List: {
            out << '[';
            for (const nbt::Tag& v : *tag.list()) {
                out << canonical(v) << ',';
            }
            out << ']';
            break;
        }
        case nbt::TagType::String:
            out << '"' << tag.as_string() << '"';
            break;
        case nbt::TagType::Float:
        case nbt::TagType::Double:
            out << tag.as_f64();
            break;
        default:
            out << tag.as_i64();
            break;
    }
    return out.str();
}

[[nodiscard]] std::filesystem::path measured_file() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "normalized" /
           "enchanting.json";
}

/// A host that records what it was asked to send and holds a player.
struct FakeHost {
    std::vector<std::pair<i32, std::vector<u8>>> sent;
    std::vector<net::ItemStack>                  dropped;
    std::string                                  block{"minecraft:anvil"};
    i32                                          level{30};
    i32                                          seed{0};
    bool                                         creative{false};
    math::LegacyRandomSource                     random{7};

    [[nodiscard]] EnchantHost host() {
        EnchantHost h;
        h.send       = [this](i32 id, std::vector<u8> payload) { sent.emplace_back(id, std::move(payload)); };
        h.drop       = [this](const net::ItemStack& s) { dropped.push_back(s); };
        h.block_name = [this](i32, i32, i32) -> std::string_view { return block; };
        h.replace_block = [this](i32, i32, i32, std::string_view next) {
            block = next.empty() ? "minecraft:air" : std::string{next};
        };
        h.hover_name  = [](const net::ItemStack&) { return std::string{"Stick"}; };
        h.level       = [this] { return level; };
        h.take_levels = [this](i32 n) { level -= n; };
        h.creative    = [this] { return creative; };
        h.xp_seed     = [this] { return seed; };
        h.set_xp_seed = [this](i32 s) { seed = s; };
        h.random      = &random;
        return h;
    }
};

}  // namespace

TEST_CASE("the anvil panel the real server answered", "[enchanting][anvil][parity]") {
    if (!std::filesystem::exists(measured_file()) || !loaded().registries) {
        WARN("no enchanting.json or registry.ovpack — run scripts/measure_enchanting.py anvil");
        return;
    }
    // The panel is flattened by scripts/check_enchanting.py into one line per
    // combination, its output tag already in the canonical form built below.
    const auto flat = measured_file().parent_path() / "enchanting_anvil_panel.txt";
    if (!std::filesystem::exists(flat)) {
        WARN("no enchanting_anvil_panel.txt — run scripts/check_enchanting.py");
        return;
    }
    const EnchantContext& context = loaded().context;
    std::ifstream         rows{flat};
    std::string           line;
    usize                 total = 0;
    usize                 same  = 0;
    std::string           differing;
    while (std::getline(rows, line)) {
        // name \t left \t right \t rename(or \x01 for none) \t cost \t output(canonical or -)
        std::vector<std::string> f;
        std::stringstream        split{line};
        std::string              cell;
        while (std::getline(split, cell, '\t')) {
            f.push_back(cell);
        }
        REQUIRE(f.size() == 6);
        FakeHost fake;
        fake.level = 1000;
        EnchantHost host = fake.host();
        host.hover_name  = [&](const net::ItemStack& s) {
            // The only hover names the panel compares against are defaults
            // and the one custom name it sets; neither equals a typed name.
            return enchant_stack_of(context, s).has_custom_name ? std::string{"<custom>"}
                                                                : std::string{"<default>"};
        };
        EnchantWindow window;
        window.kind     = EnchantScreen::Anvil;
        window.slots[0] = stack_of(context, f[1]);
        if (f[2] != "-") {
            window.slots[1] = stack_of(context, f[2]);
        }
        if (f[3] != "\x01") {
            window.rename = f[3];
        }
        refresh_enchant_window(context, host, window);
        std::string ours = "-";
        if (!window.slots[2].empty()) {
            ours = std::string{context.registries->entry_of(context.item_registry,
                                                            window.slots[2].item_id)} +
                   " " + std::to_string(window.slots[2].count) + " ";
            if (!window.slots[2].nbt.empty()) {
                ours += canonical(nbt::read(window.slots[2].nbt)->root);
            } else {
                ours += "{}";
            }
        }
        const bool cost_ok   = std::to_string(window.anvil.cost) == f[4];
        const bool output_ok = ours == f[5];
        ++total;
        if (cost_ok && output_ok) {
            ++same;
        } else {
            differing += "\n  " + f[0] + ": vanilla cost " + f[4] + " " + f[5] + "\n" +
                         std::string(4 + f[0].size(), ' ') + "   ours cost " +
                         std::to_string(window.anvil.cost) + " " + ours;
        }
    }
    INFO(same << " / " << total << " anvil cells identical" << differing);
    REQUIRE(total > 0);
    CHECK(same == total);
}

TEST_CASE("the table window: offers, the button, the seed", "[enchanting][window]") {
    if (!loaded().registries) {
        WARN("no registry.ovpack");
        return;
    }
    const EnchantContext& context = loaded().context;
    FakeHost              fake;
    fake.block = "minecraft:enchanting_table";
    fake.seed  = 0;
    EnchantHost host = fake.host();
    std::optional<EnchantWindow> window;
    std::array<net::ItemStack, 46> inventory{};
    REQUIRE(open_enchant_screen(context, host, 0, 64, 0, 3, inventory, window));
    REQUIRE(window.has_value());

    window->slots[0] = stack_of(context, "minecraft:diamond_sword");
    window->slots[1] = stack_of(context, "minecraft:lapis_lazuli 5");
    refresh_enchant_window(context, host, *window);
    // No bookshelves around a table in a void: the bottom offer is at most 8.
    REQUIRE(window->offers.costs[2] > 0);
    CHECK(window->offers.costs[2] <= 8);

    const i32 cost = window->offers.costs[2];
    fake.sent.clear();
    enchant_button(context, host, *window, 2, inventory, {});
    CHECK(fake.level == 30 - 3);            // the index plus one, not the cost
    CHECK(window->slots[1].count == 5 - 3);  // and as much lapis
    CHECK(fake.seed != 0);                   // a new seed
    // …and the client is told: a Container Property 3 carrying the new seed's
    // bits 4..15. The end-to-end probe saw property 3 stay at 0.
    std::optional<i16> seed_property;
    for (const auto& [id, payload] : fake.sent) {
        if (id == net::clientbound::kContainerProperty && payload.size() == 5 &&
            payload[1] == 0 && payload[2] == 3) {
            seed_property = static_cast<i16>((payload[3] << 8) | payload[4]);
        }
    }
    REQUIRE(seed_property.has_value());
    CHECK(*seed_property == static_cast<i16>(fake.seed));  // unmasked: measured
    CHECK(enchant_stack_of(context, window->slots[0]).enchantments.size() >= 1);
    CHECK(window->offers.costs == std::array<i32, 3>{0, 0, 0});  // enchanted: no more offers
    (void)cost;

    // Closing gives the item and the lapis back.
    close_enchant_screen(context, host, *window, inventory);
    usize held = 0;
    for (const net::ItemStack& s : inventory) {
        held += s.empty() ? 0 : 1;
    }
    CHECK(held == 2);
}

TEST_CASE("the anvil window: take, pay, degrade", "[enchanting][window]") {
    if (!loaded().registries) {
        WARN("no registry.ovpack");
        return;
    }
    const EnchantContext& context = loaded().context;
    FakeHost              fake;
    EnchantHost           host = fake.host();
    std::optional<EnchantWindow> window;
    std::array<net::ItemStack, 46> inventory{};
    REQUIRE(open_enchant_screen(context, host, 0, 64, 0, 4, inventory, window));
    window->slots[0] = stack_of(context, "minecraft:stick");
    enchant_rename(context, host, *window, "Bob", inventory, {});
    REQUIRE(window->anvil.valid);
    CHECK(window->anvil.cost == 1);

    net::ItemStack        carried;
    net::ContainerClick   click;
    click.window_id = 4;
    click.slot      = 2;
    click.mode      = 0;
    enchant_click(context, host, *window, click, inventory, carried, window);
    REQUIRE(window.has_value());
    CHECK(fake.level == 29);
    CHECK_FALSE(carried.empty());  // the renamed stick is on the cursor
    CHECK(window->slots[0].empty());

    // Many takes: the anvil breaks after two stages, about one use in eight.
    usize uses = 0;
    while (fake.block != "minecraft:air" && uses < 10'000) {
        window->slots[0] = stack_of(context, "minecraft:stick");
        enchant_rename(context, host, *window, "n" + std::to_string(uses), inventory, {});
        carried = {};
        enchant_click(context, host, *window, click, inventory, carried, window);
        if (!window) {
            break;
        }
        ++uses;
        fake.level = 30;
    }
    CHECK(fake.block == "minecraft:air");
    CHECK(uses > 3);
}

TEST_CASE("a player's generator never starts at state 0", "[enchanting][window]") {
    // The regression, pinned: Java's setSeed XORs with 0x5DEECE66D, so seeding
    // with entity_id * 0x5DEECE66D for entity 1 left the state at 0 and the
    // first nextInt — the "new" XpSeed — at exactly 0. Found end to end: the
    // table kept showing seed 0 after every enchant.
    math::LegacyRandomSource old_way{1LL * 0x5DEECE66DLL};
    CHECK(old_way.next_int() == 0);

    const net::Uuid a{0x9b9a47360b7f3b16ULL, 0x966863bce3b9edc8ULL};
    const net::Uuid b{0x0f7c206042d83a0cULL, 0xa7f8d5e3786f2e22ULL};
    math::LegacyRandomSource first{enchant_random_seed(a)};
    math::LegacyRandomSource second{enchant_random_seed(b)};
    const i32 seed_a = first.next_int();
    const i32 seed_b = second.next_int();
    CHECK(seed_a != 0);
    CHECK(seed_b != 0);
    CHECK(seed_a != seed_b);
    // And what the client is shown of it is not zero either.
    CHECK(static_cast<i16>(seed_a & -16) != 0);
}

TEST_CASE("Click Container Button and Rename Item decode", "[enchanting][window]") {
    const std::array<u8, 2> button{3, 2};
    const auto              parsed = parse_click_button(button);
    REQUIRE(parsed.has_value());
    CHECK(parsed->first == 3);
    CHECK(parsed->second == 2);
    const std::array<u8, 4> rename{3, 'B', 'o', 'b'};
    CHECK(parse_rename_item(rename) == std::optional<std::string>{"Bob"});
    const std::array<u8, 2> truncated{3, 'B'};
    CHECK_FALSE(parse_rename_item(truncated).has_value());
}
