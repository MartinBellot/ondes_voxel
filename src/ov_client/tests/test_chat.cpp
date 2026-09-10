// The chat, against what the running 1.20.1 client did.
//
// Every expected value marked "measured" comes out of
// scripts/chat_screen_oracle.java (docs/provenance/chat-client.md): the real
// client driven by injected keys, asked for its numbers. The fixtures that
// need game assets — the font the wrap is measured with, the registry codec —
// are local and gitignored; those cases SKIP when the files are absent, and
// say so, rather than passing without having looked.
#include "ov/client/chat.hpp"
#include "ov/client/command_suggestions.hpp"
#include "ov/client/text_field.hpp"
#include "ov/client/window.hpp"
#include "ov/nbt/tag.hpp"
#include "ov/protocol/chat.hpp"
#include "ov/protocol/chat_types.hpp"
#include "ov/protocol/play.hpp"
#include "ov/render/asset_source.hpp"
#include "ov/render/font.hpp"
#include "ov/render/language.hpp"
#include "ov/render/text_component.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

using namespace ov;
using namespace ov::client;

namespace {

KeyEvent key(EditKey k, bool shift = false, bool control = false) {
    return KeyEvent{k, false, shift, control, false};
}

std::optional<std::vector<u8>> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    return std::vector<u8>(std::istreambuf_iterator<char>(in), {});
}

/// The pack the oracle ran with, imported into run/assets (gitignored).
const render::Font* real_font() {
    static const std::optional<render::Font> font = []() -> std::optional<render::Font> {
        const auto root = std::filesystem::path{OV_SOURCE_DIR} / "run" / "assets";
        if (!std::filesystem::exists(root / "assets" / "minecraft" / "font" / "default.json")) {
            return std::nullopt;
        }
        render::DirectoryAssetSource source(root);
        auto loaded = render::Font::load_default(source);
        return loaded ? std::optional<render::Font>(std::move(*loaded)) : std::nullopt;
    }();
    return font ? &*font : nullptr;
}

render::Language make_language() {
    render::MemoryAssetSource source;
    source.add("assets/minecraft/lang/en_us.json",
               R"JSON({"commands.time.set":"Set the time to %s",
                   "chat.type.text":"<%s> %s",
                   "commands.message.display.incoming":"%s whispers to you: %s",
                   "swap":"%2$s then %1$s",
                   "wrap":"[%s]"})JSON");
    auto language = render::Language::load(source, "en_us");
    REQUIRE(language.has_value());
    return std::move(*language);
}

/// Our server's Commands packet for an owner (level 4), captured by our own
/// client with `ov_voxel --dump-commands`.
CommandTree real_tree() {
    const auto bytes = read_file(std::filesystem::path{OV_SOURCE_DIR} / "src" / "ov_client" /
                                 "tests" / "data" / "commands_owner.bin");
    REQUIRE(bytes.has_value());
    auto graph = net::parse_commands(*bytes);
    REQUIRE(graph.has_value());
    return CommandTree(std::move(*graph));
}

std::string plain(const RunLine& line) {
    std::string out;
    for (const auto& run : line) {
        out += render::strip_formatting(run.text);
    }
    return out;
}

}  // namespace

// ── The text field ───────────────────────────────────────────────────────────

TEST_CASE("the field edits like the chat box of the running client", "[chat][field]") {
    TextField field(256);
    std::string clipboard;
    field.insert("hello world");
    CHECK(field.cursor_chars() == 11);
    for (int i = 0; i < 3; ++i) {
        (void)field.key(key(EditKey::Left), clipboard);
    }
    CHECK(field.cursor_chars() == 8);  // measured: "left x3" -> cursor 8
    (void)field.key(key(EditKey::Home), clipboard);
    CHECK(field.cursor_chars() == 0);
    (void)field.key(key(EditKey::End), clipboard);
    CHECK(field.cursor_chars() == 11);
    CHECK(field.key(key(EditKey::Backspace), clipboard) == FieldResult::Edited);
    CHECK(field.value() == "hello worl");
    field.insert("d");
    // Measured: "é§x" typed leaves "éx" — the section sign is refused.
    field.insert("\xC3\xA9\xC2\xA7x");
    CHECK(field.value() == "hello world\xC3\xA9x");
    CHECK(field.cursor_chars() == 13);
}

TEST_CASE("the field holds 256 UTF-16 units and cuts the rest", "[chat][field]") {
    TextField   field(256);
    std::string letters;
    for (int i = 0; i < 300; ++i) {
        letters += static_cast<char>('a' + i % 26);
    }
    field.insert(letters);
    CHECK(field.length_utf16() == 256);  // measured: maxLength 256, cursor 256
    CHECK(field.value() == letters.substr(0, 256));

    TextField small(3);
    small.insert("a\xF0\x9F\x98\x80"  // U+1F600: two UTF-16 units
                 "b");
    CHECK(small.length_utf16() == 3);
    small.set_value("ab\xF0\x9F\x98\x80");  // the emoji does not fit whole
    CHECK(small.value() == "ab");
    // Control characters and DEL never go in.
    TextField plain(10);
    plain.insert("a\tb\x7F" "c");
    CHECK(plain.value() == "abc");
}

TEST_CASE("words, selections and the clipboard", "[chat][field]") {
    TextField   field(256);
    std::string clipboard;
    field.insert("time set  day");
    (void)field.key(key(EditKey::Left, false, true), clipboard);
    CHECK(field.cursor() == 10);  // the start of "day"
    (void)field.key(key(EditKey::Left, false, true), clipboard);
    CHECK(field.cursor() == 5);  // the start of "set"
    (void)field.key(key(EditKey::Right, true, true), clipboard);
    CHECK(field.selection() == "set  ");
    (void)field.key(key(EditKey::C, false, true), clipboard);
    CHECK(clipboard == "set  ");
    field.insert("add ");
    CHECK(field.value() == "time add day");
    (void)field.key(key(EditKey::A, false, true), clipboard);
    (void)field.key(key(EditKey::X, false, true), clipboard);
    CHECK(field.value().empty());
    CHECK(clipboard == "time add day");
    (void)field.key(key(EditKey::V, false, true), clipboard);
    (void)field.key(key(EditKey::V, false, true), clipboard);
    CHECK(field.value() == "time add daytime add day");
    (void)field.key(key(EditKey::Backspace, false, true), clipboard);
    CHECK(field.value() == "time add daytime add ");
    (void)field.key(key(EditKey::Home), clipboard);
    (void)field.key(key(EditKey::Delete, false, true), clipboard);
    CHECK(field.value() == "add daytime add ");
    // A letter A without control is not a chord: the field ignores the key
    // and the character arrives as text.
    CHECK(field.key(key(EditKey::A), clipboard) == FieldResult::Ignored);
    // Unicode moves by codepoint.
    TextField accents(10);
    accents.insert("\xC3\xA9t\xC3\xA9");
    accents.move(-1, false);
    CHECK(accents.cursor() == 3);
    CHECK(accents.erase(-1, false));
    CHECK(accents.value() == "\xC3\xA9\xC3\xA9");
}

TEST_CASE("a long line scrolls to keep the cursor in the box", "[chat][field]") {
    const render::Font* font = real_font();
    if (font == nullptr) {
        SKIP("run/assets is not imported: no font to measure with");
    }
    TextField   field(256);
    std::string letters;
    for (int i = 0; i < 300; ++i) {
        letters += static_cast<char>('a' + i % 26);
    }
    field.insert(letters);
    const usize first =
        field.scroll_into_view(850.0F, [font](std::string_view s) { return font->width(s); });
    CHECK(first == 104);  // measured: displayPos 104 in an 850-pixel box
}

// ── The messages ─────────────────────────────────────────────────────────────

TEST_CASE("a message fades exactly as the running client fades it", "[chat][fade]") {
    // Sampled on the running client: getTimeFactor at these ages.
    CHECK(chat_opacity(0) == 1.0);
    CHECK(chat_opacity(179) == 1.0);
    CHECK(chat_opacity(180) == Catch::Approx(1.0));
    CHECK(chat_opacity(181) == Catch::Approx(0.9025));
    CHECK(chat_opacity(185) == Catch::Approx(0.5625));
    CHECK(chat_opacity(190) == Catch::Approx(0.25));
    CHECK(chat_opacity(195) == Catch::Approx(0.0625));
    CHECK(chat_opacity(199) == Catch::Approx(0.0025));
    CHECK(chat_opacity(200) == 0.0);
    CHECK(chat_opacity(220) == 0.0);
}

TEST_CASE("what is sent is trimmed and collapsed", "[chat]") {
    // Measured: "  spaced    out   message  " arrived as "spaced out message".
    CHECK(normalize_chat_message("  spaced    out   message  ") == "spaced out message");
    CHECK(normalize_chat_message("   ").empty());
    CHECK(normalize_chat_message("/time  set day") == "/time set day");
}

TEST_CASE("lines break where the running client breaks them", "[chat][wrap]") {
    const render::Font* font = real_font();
    if (font == nullptr) {
        SKIP("run/assets is not imported: the wrap is measured in its font");
    }
    const auto lines_of = [font](std::string_view text) {
        std::vector<std::string> out;
        for (const RunLine& line : wrap_runs(RunLine{render::StyledRun{std::string(text)}},
                                             chat_layout::kWidth, *font)) {
            out.push_back(plain(line));
        }
        return out;
    };
    // Measured, same pack, same width: splitLines at 320, then the chat's indent.
    CHECK(lines_of("The quick brown fox jumps over the lazy dog, and then it keeps running "
                   "because the lazy dog finally woke up and is now very, very angry.") ==
          std::vector<std::string>{
              "The quick brown fox jumps over the lazy dog, and then it",
              " keeps running because the lazy dog finally woke up and is now",
              " very, very angry."});
    std::string word;
    for (int i = 0; i < 4; ++i) {
        word += "Supercalifragilisticexpialidocious";
    }
    CHECK(lines_of(word) ==
          std::vector<std::string>{
              "SupercalifragilisticexpialidociousSupercalifragilisticexpialidocio",
              " usSupercalifragilisticexpialidociousSupercalifragilisticexpialidoc",
              " ious"});
    CHECK(lines_of("  leading spaces and a trailing one ") ==
          std::vector<std::string>{"  leading spaces and a trailing one "});
    CHECK(lines_of("first line\nsecond line") ==
          std::vector<std::string>{"first line", " second line"});
}

TEST_CASE("the log keeps 100 messages, newest first", "[chat][log]") {
    const render::Font* font = real_font();
    if (font == nullptr) {
        SKIP("run/assets is not imported");
    }
    ChatLog log;
    for (int i = 1; i <= 101; ++i) {
        log.add(RunLine{render::StyledRun{"line " + std::to_string(i)}}, i, *font);
    }
    CHECK(log.message_count() == 100);  // measured: MAX_CHAT_HISTORY
    CHECK(log.lines().size() == 100);
    CHECK(plain(log.lines().front().runs) == "line 101");
    CHECK(log.plain_lines().front() == "line 2");
    // Scrolling, measured with 40 lines open: a notch is 7, Page Up 19.
    ChatLog forty;
    for (int i = 1; i <= 40; ++i) {
        forty.add(RunLine{render::StyledRun{"line " + std::to_string(i)}}, 0, *font);
    }
    forty.scroll(chat_layout::kScrollLines, chat_layout::kLinesOpen);
    CHECK(forty.scroll_position() == 7);
    forty.scroll(-7, chat_layout::kLinesOpen);
    forty.scroll(static_cast<i32>(chat_layout::kLinesOpen) - 1, chat_layout::kLinesOpen);
    CHECK(forty.scroll_position() == 19);
    forty.scroll(1000, chat_layout::kLinesOpen);
    CHECK(forty.scroll_position() == 20);  // 40 lines, a page of 20
    forty.scroll(-1000, chat_layout::kLinesOpen);
    CHECK(forty.scroll_position() == 0);
    // A wrapped message: its last line is the newest, and ends the entry.
    ChatLog wrapped;
    std::string long_word(120, 'm');
    wrapped.add(RunLine{render::StyledRun{long_word}}, 0, *font);
    REQUIRE(wrapped.lines().size() >= 2);
    CHECK(wrapped.lines().front().end_of_entry);
    CHECK_FALSE(wrapped.lines().back().end_of_entry);
    CHECK(wrapped.plain_lines() == std::vector<std::string>{long_word});
}

// ── Components ───────────────────────────────────────────────────────────────

TEST_CASE("components resolve with the language, exact colours kept", "[chat][component]") {
    const render::Language language = make_language();
    // What our server sends for /time set day.
    auto runs = render::component_runs(R"({"translate":"commands.time.set","with":["1000"]})",
                                       language);
    CHECK(render::plain_text(runs) == "Set the time to 1000");

    // The oracle's styles line: exact #3080ff, not the nearest named colour.
    runs = render::component_runs(
        R"([{"text":"bold ","bold":true},{"text":"hex ","color":"#3080ff"},)"
        R"({"text":"gold","color":"gold","underlined":true}])",
        language);
    REQUIRE(runs.size() == 3);
    CHECK(runs[0].bold);
    CHECK(runs[1].rgb == 0x3080FFU);
    CHECK(runs[1].has_colour);
    // An array's rest inherits the first element's style: the running client
    // drew this whole line bold (capture 17-styles), "hex" and "gold" included.
    CHECK(runs[1].bold);
    CHECK(runs[2].rgb == 0xFFAA00U);
    CHECK(runs[2].underlined);

    // Arguments inherit the translatable's style and keep their own.
    runs = render::component_runs(
        R"({"translate":"commands.time.set","color":"green","with":[{"text":"42","bold":true}]})",
        language);
    REQUIRE(runs.size() == 2);
    CHECK(runs[0].text == "Set the time to ");
    CHECK(runs[0].rgb == 0x55FF55U);
    CHECK(runs[1].text == "42");
    CHECK(runs[1].bold);
    CHECK(runs[1].rgb == 0x55FF55U);

    // %1$s and %2$s, nested translations, missing arguments, fallback.
    CHECK(render::plain_text(render::component_runs(
              R"({"translate":"swap","with":["a",{"translate":"wrap","with":["b"]}]})", language)) ==
          "[b] then a");
    CHECK(render::plain_text(render::component_runs(R"({"translate":"wrap"})", language)) ==
          "[%s]");
    CHECK(render::plain_text(render::component_runs(
              R"({"translate":"no.such","fallback":"F %s","with":["x"]})", language)) == "F x");
    CHECK(render::plain_text(render::component_runs(R"({"translate":"no.such"})", language)) ==
          "no.such");
    // Not JSON: shown as itself.
    CHECK(render::plain_text(render::component_runs("{oops", language)) == "{oops");
}

// ── Chat types, from the codec ───────────────────────────────────────────────

TEST_CASE("chat types are read out of the codec, style included", "[chat][codec]") {
    nbt::Tag codec    = nbt::Tag::make_compound();
    nbt::Tag registry = nbt::Tag::make_compound();
    registry.put("type", nbt::Tag(std::string("minecraft:chat_type")));
    nbt::Tag values = nbt::Tag::make_list(nbt::TagType::Compound);
    const auto entry = [](std::string name, i32 id, std::string key, bool styled) {
        nbt::Tag chat = nbt::Tag::make_compound();
        chat.put("translation_key", nbt::Tag(std::move(key)));
        nbt::Tag parameters = nbt::Tag::make_list(nbt::TagType::String);
        parameters.push(nbt::Tag(std::string("sender")));
        parameters.push(nbt::Tag(std::string("content")));
        chat.put("parameters", std::move(parameters));
        if (styled) {
            nbt::Tag style = nbt::Tag::make_compound();
            style.put("color", nbt::Tag(std::string("gray")));
            style.put("italic", nbt::Tag::make_bool(true));
            chat.put("style", std::move(style));
        }
        nbt::Tag element = nbt::Tag::make_compound();
        element.put("chat", std::move(chat));
        nbt::Tag out = nbt::Tag::make_compound();
        out.put("name", nbt::Tag(std::move(name)));
        out.put("id", nbt::Tag(id));
        out.put("element", std::move(element));
        return out;
    };
    values.push(entry("minecraft:chat", 0, "chat.type.text", false));
    values.push(entry("minecraft:msg_command_incoming", 2, "commands.message.display.incoming", true));
    registry.put("value", std::move(values));
    codec.put("minecraft:chat_type", std::move(registry));

    const auto types = net::chat_types_from_codec(codec);
    REQUIRE(types.size() == 2);
    CHECK(types[0].translation_key == "chat.type.text");
    CHECK(types[0].parameters == std::vector<std::string>{"sender", "content"});
    CHECK(types[0].style_json.empty());
    CHECK(types[1].id == 2);
    CHECK(types[1].style_json == R"("italic":true,"color":"gray")");
}

TEST_CASE("the real codec, through a real Login (play)", "[chat][codec]") {
    const auto codec = read_file(std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" /
                                 "1.20.1" / "registry_codec.nbt");
    if (!codec) {
        SKIP("data/vanilla/1.20.1/registry_codec.nbt is not generated here");
    }
    net::LoginPlay login;
    login.registry_codec = *codec;
    const auto types     = net::read_login_chat_types(net::encode_login_play(login));
    REQUIRE(types.has_value());
    REQUIRE(types->size() == 7);  // chat, emote, msg ×2, say, team msg ×2
    const auto find = [&](std::string_view name) -> const net::ChatDecoration* {
        for (const auto& t : *types) {
            if (t.name == name) {
                return &t;
            }
        }
        return nullptr;
    };
    REQUIRE(find("minecraft:chat") != nullptr);
    CHECK(find("minecraft:chat")->id == 0);  // the index our server's chat lines use
    CHECK(find("minecraft:chat")->translation_key == "chat.type.text");
    REQUIRE(find("minecraft:msg_command_incoming") != nullptr);
    CHECK(find("minecraft:msg_command_incoming")->style_json.find(R"("italic":true)") !=
          std::string::npos);
    REQUIRE(find("minecraft:say_command") != nullptr);
    CHECK(find("minecraft:say_command")->translation_key == "chat.type.announcement");
}

// ── Completion, from our server's own Commands packet ────────────────────────

TEST_CASE("literals complete locally, arguments ask the server", "[chat][commands]") {
    const CommandTree tree = real_tree();
    REQUIRE_FALSE(tree.empty());

    auto ti = tree.complete("/ti");
    CHECK(ti.command);
    CHECK(ti.start == 1);  // measured: range start 1
    CHECK(ti.matches == std::vector<std::string>{"time", "title"});  // measured, in order
    CHECK_FALSE(ti.ask_server);

    auto slash = tree.complete("/");
    CHECK(slash.matches.size() == 34);  // our 34 commands; vanilla lists 79
    CHECK(std::is_sorted(slash.matches.begin(), slash.matches.end()));

    auto set = tree.complete("/time set ");
    CHECK(set.ask_server);  // day/midnight/night/noon *and* <time>: measured from the server
    CHECK(set.start == 10);
    CHECK(set.matches == std::vector<std::string>{"day", "midnight", "night", "noon"});

    CHECK(tree.complete("/gamemode ").ask_server);
    CHECK(tree.complete("/tp ").ask_server);
    CHECK_FALSE(tree.complete("hello").command);
    CHECK(tree.complete("/nosuch x").matches.empty());

    // A redirect: tp is teleport.
    CHECK(tree.complete("/tp @s ").ask_server);

    // Usage, as measured: "<time>" and "<gamemode> [<target>]".
    CHECK(tree.usage("/time set ").lines == std::vector<std::string>{"<time>"});
    CHECK(tree.usage("/gamemode ").lines == std::vector<std::string>{"<gamemode> [<target>]"});
    CHECK(tree.usage("/time set ").start == 10);

    // Highlight: "ti" of "/ti" is red (measured), a whole literal grey.
    const auto red = tree.highlight("/ti");
    REQUIRE(red.size() == 1);
    CHECK(red[0].rgb == 0xFF5555U);
    const auto grey = tree.highlight("/time set day");
    REQUIRE(grey.size() == 3);
    CHECK(grey[0].rgb == 0xAAAAAAU);
    CHECK(grey[2].rgb == 0xAAAAAAU);
    const auto args = tree.highlight("/tp @s");
    REQUIRE(args.size() == 2);
    CHECK(args[1].rgb == 0x55FFFFU);  // measured: "@s" aqua
}

TEST_CASE("the chat box behaves as the running client's", "[chat][input]") {
    ChatInput input;
    input.set_commands(real_tree());
    std::vector<std::string> history;
    std::string              clipboard;

    // "/" alone lists nothing until Tab — measured.
    input.open("/");
    (void)input.refresh();
    CHECK_FALSE(input.suggestions().visible);
    (void)input.key(key(EditKey::Tab), clipboard, history);
    CHECK(input.suggestions().visible);
    CHECK(input.field().value() == "/");  // the first Tab only shows
    (void)input.key(key(EditKey::Tab), clipboard, history);
    CHECK(input.field().value() == "/clear");  // the second applies the first entry
    // Escape hides the list, the second closes — measured.
    CHECK(input.key(key(EditKey::Escape), clipboard, history).kind == ChatAction::Kind::None);
    CHECK_FALSE(input.suggestions().visible);
    CHECK(input.key(key(EditKey::Escape), clipboard, history).kind == ChatAction::Kind::Close);

    // "/ti": shown at once, the ghost is "me", Tab gives "/time" and keeps the list.
    input.open("/");
    input.type("ti");
    (void)input.refresh();
    REQUIRE(input.suggestions().visible);
    CHECK(input.suggestions().items == std::vector<std::string>{"time", "title"});
    CHECK(input.ghost() == "me");
    (void)input.key(key(EditKey::Tab), clipboard, history);
    CHECK(input.field().value() == "/time");
    (void)input.refresh();
    CHECK(input.suggestions().items.size() == 2);
    (void)input.key(key(EditKey::Tab), clipboard, history);
    CHECK(input.field().value() == "/title");

    // "/time set ": the server is asked, and its answer is what is listed.
    input.open("/time set ");
    const ChatAction ask = input.refresh();
    REQUIRE(ask.kind == ChatAction::Kind::RequestSuggestions);
    CHECK(ask.text == "/time set ");
    input.on_suggestions(ask.transaction + 1, 10, 0, {"wrong"});  // a stale answer
    input.on_suggestions(ask.transaction, 10, 0, {"day", "midnight", "night", "noon"});
    CHECK(input.suggestions().visible);
    CHECK(input.suggestions().start == 10);
    CHECK(input.ghost() == "day");  // measured: suggestion "day"

    // Enter: a message, then a command, and the history they leave.
    input.open("");
    input.type("hello world");
    ChatAction sent = input.key(key(EditKey::Enter), clipboard, history);
    CHECK(sent.kind == ChatAction::Kind::SendMessage);
    CHECK(sent.text == "hello world");
    input.open("/");
    input.type("time set day");
    sent = input.key(key(EditKey::Enter), clipboard, history);
    CHECK(sent.kind == ChatAction::Kind::SendCommand);
    CHECK(sent.text == "time set day");  // Chat Command carries no slash
    CHECK(history == std::vector<std::string>{"hello world", "/time set day"});

    // Up, Up, Up, Down — measured: /time set day, hello world, hello world,
    // /time set day.
    input.open("");
    (void)input.key(key(EditKey::Up), clipboard, history);
    CHECK(input.field().value() == "/time set day");
    (void)input.refresh();
    CHECK_FALSE(input.suggestions().visible);  // measured: no list on a recalled line
    (void)input.key(key(EditKey::Up), clipboard, history);
    CHECK(input.field().value() == "hello world");
    (void)input.key(key(EditKey::Up), clipboard, history);
    CHECK(input.field().value() == "hello world");
    (void)input.key(key(EditKey::Down), clipboard, history);
    CHECK(input.field().value() == "/time set day");
    (void)input.key(key(EditKey::Down), clipboard, history);
    CHECK(input.field().value().empty());  // back to what was being typed

    // Whitespace only: closes, sends nothing.
    input.open("");
    input.type("   ");
    CHECK(input.key(key(EditKey::Enter), clipboard, history).kind == ChatAction::Kind::Close);
}

// ── Titles and the action bar ────────────────────────────────────────────────

TEST_CASE("titles and the action bar run their measured times", "[chat][title]") {
    TitleOverlay overlay;
    overlay.set_title(RunLine{render::StyledRun{"Title"}});
    CHECK(overlay.title_time() == 100);  // 10 + 70 + 20, measured defaults
    overlay.tick(27);
    CHECK(overlay.title_time() == 73);  // measured 73 a little over a second in
    overlay.tick(80);
    CHECK(overlay.title_time() == 0);
    overlay.set_action_bar(RunLine{render::StyledRun{"Action bar"}});
    CHECK(overlay.action_bar_time() == 60);
    overlay.tick(19);
    CHECK(overlay.action_bar_time() == 41);  // measured 41
    overlay.set_times(5, 10, 5);
    overlay.set_title(RunLine{render::StyledRun{"T"}});
    CHECK(overlay.title_time() == 20);
    overlay.clear(true);
    overlay.set_title(RunLine{render::StyledRun{"T"}});
    CHECK(overlay.title_time() == 100);
}
