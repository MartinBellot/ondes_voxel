#include "ov/client/tab_list_view.hpp"

#include "ov/client/chat.hpp"
#include "ov/client/scoreboard_view.hpp"
#include "ov/render/language.hpp"
#include "ov/render/text_component.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <variant>

namespace ov::client {
namespace {

constexpr f32 kSheet       = 256.0F;
constexpr u32 kBlock       = 0x80000000U;
constexpr u32 kCell        = 0x20FFFFFFU;
constexpr u32 kSpectator   = 0x90FFFFFFU;
constexpr i32 kMaxRows     = 20;
constexpr usize kMaxPlayers = 80;
constexpr i32 kSpectatorMode = 3;

bool less_ignoring_case(std::string_view a, std::string_view b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
    });
}

/// The component's lines, wrapped to `width`; none for an empty component —
/// vanilla's handler turns an empty header into no header at all.
std::vector<RunLine> lines_of(const std::string& json, f32 width, const Gui& gui,
                              const render::Language& language) {
    std::vector<RunLine> out;
    if (json.empty()) {
        return out;
    }
    RunLine runs = render::component_runs(json, language);
    if (render::plain_text(runs).empty()) {
        return out;
    }
    out = wrap_runs(runs, width, gui.font());
    for (RunLine& line : out) {
        encode_styles(line);
    }
    return out;
}

}  // namespace

void TabListView::apply(const netclient::ClientEvents::TabListEvent& event) {
    if (const auto* update = std::get_if<net::PlayerInfoUpdate>(&event)) {
        for (const net::PlayerInfoEntry& info : update->entries) {
            auto it = std::ranges::find_if(entries_, [&](const Entry& e) { return e.uuid == info.uuid; });
            if ((update->actions & net::player_info::kAddPlayer) != 0) {
                if (it == entries_.end()) {
                    entries_.push_back(Entry{});
                    it = entries_.end() - 1;
                }
                *it      = Entry{};
                it->uuid = info.uuid;
                it->name = info.name;
            }
            if (it == entries_.end()) {
                continue;  // an update for a player never added: vanilla ignores it
            }
            if ((update->actions & net::player_info::kUpdateGameMode) != 0) {
                it->game_mode = info.game_mode;
            }
            if ((update->actions & net::player_info::kUpdateListed) != 0) {
                it->listed = info.listed;
            }
            if ((update->actions & net::player_info::kUpdateLatency) != 0) {
                it->latency = info.latency;
            }
            if ((update->actions & net::player_info::kUpdateDisplayName) != 0) {
                it->display_json = info.display_name_json;
            }
        }
    } else if (const auto* removed = std::get_if<std::vector<net::Uuid>>(&event)) {
        for (const net::Uuid& uuid : *removed) {
            std::erase_if(entries_, [&](const Entry& e) { return e.uuid == uuid; });
        }
    } else if (const auto* texts = std::get_if<net::TabListHeaderFooter>(&event)) {
        header_json_ = texts->header_json;
        footer_json_ = texts->footer_json;
    }
}

void TabListView::clear() {
    entries_.clear();
    header_json_.clear();
    footer_json_.clear();
}

std::vector<const TabListView::Entry*> TabListView::listed(const ScoreboardView& scoreboard) const {
    std::vector<const Entry*> out;
    for (const Entry& entry : entries_) {
        if (entry.listed) {
            out.push_back(&entry);
        }
    }
    std::ranges::stable_sort(out, [&](const Entry* a, const Entry* b) {
        const bool sa = a->game_mode == kSpectatorMode;
        const bool sb = b->game_mode == kSpectatorMode;
        if (sa != sb) {
            return !sa;
        }
        const std::string ta = scoreboard.team_name(a->name);
        const std::string tb = scoreboard.team_name(b->name);
        if (ta != tb) {
            return ta < tb;
        }
        return less_ignoring_case(a->name, b->name);
    });
    if (out.size() > kMaxPlayers) {
        out.resize(kMaxPlayers);
    }
    return out;
}

bool TabListView::should_show(bool key_down, bool integrated, const ScoreboardView& scoreboard) const {
    if (!key_down) {
        return false;
    }
    if (!integrated) {
        return true;
    }
    usize listed_count = 0;
    for (const Entry& e : entries_) {
        listed_count += e.listed ? 1 : 0;
    }
    return listed_count > 1 || !scoreboard.list_objective().empty();
}

i32 TabListView::ping_row(i32 latency) noexcept {
    if (latency < 0) {
        return 5;
    }
    if (latency < 150) {
        return 0;
    }
    if (latency < 300) {
        return 1;
    }
    if (latency < 600) {
        return 2;
    }
    if (latency < 1000) {
        return 3;
    }
    return 4;
}

void TabListView::draw(Gui& gui, GuiTexture icons, const ScoreboardView& scoreboard,
                       const render::Language& language) const {
    const std::vector<const Entry*> players = listed(scoreboard);
    const render::Font&             font    = gui.font();
    const auto                      W       = static_cast<i32>(gui.width());

    const std::string objective   = scoreboard.list_objective();
    const bool        with_scores = !objective.empty() && !scoreboard.objective_is_hearts(objective);

    std::vector<std::string> names;
    std::vector<std::string> scores;
    names.reserve(players.size());
    i32 name_w  = 0;
    i32 score_w = 0;
    for (const Entry* p : players) {
        std::string name = p->display_json ? render::flatten_component(*p->display_json, language)
                                           : scoreboard.dressed_name(p->name, language);
        if (p->game_mode == kSpectatorMode) {
            name = "§o" + name;
        }
        name_w = std::max(name_w, static_cast<i32>(font.width(name)));
        names.push_back(std::move(name));
        if (with_scores) {
            const i32 score = scoreboard.score(objective, p->name).value_or(0);
            score_w = std::max(score_w, static_cast<i32>(font.width(" " + std::to_string(score))));
            scores.push_back("§e" + std::to_string(score));
        }
    }

    const auto n    = static_cast<i32>(players.size());
    i32        rows = n;
    i32        cols = 1;
    while (rows > kMaxRows) {
        ++cols;
        rows = (n + cols - 1) / cols;
    }
    const i32 col_w = std::min(cols * (name_w + score_w + 13), W - 50) / cols;
    const i32 x0    = W / 2 - (col_w * cols + (cols - 1) * 5) / 2;
    i32       block = col_w * cols + (cols - 1) * 5;

    const auto header = lines_of(header_json_, static_cast<f32>(W - 50), gui, language);
    const auto footer = lines_of(footer_json_, static_cast<f32>(W - 50), gui, language);
    for (const auto* lines : {&header, &footer}) {
        for (const RunLine& line : *lines) {
            block = std::max(block, static_cast<i32>(runs_width(line, font)));
        }
    }
    const auto band = [&](i32 top, i32 height) {
        gui.fill(static_cast<f32>(W / 2 - block / 2 - 1), static_cast<f32>(top - 1),
                 static_cast<f32>(block / 2 * 2 + 2), static_cast<f32>(height + 1), kBlock);
    };

    // Backgrounds first, then the ping bars, then every line of text: three
    // texture changes whatever the number of players.
    i32 y = 10;
    const i32 header_y = y;
    if (!header.empty()) {
        band(y, static_cast<i32>(header.size()) * 9);
        y += static_cast<i32>(header.size()) * 9 + 1;
    }
    const i32 list_y = y;
    band(y, rows * 9);
    for (i32 i = 0; i < n; ++i) {
        const i32 col = i / rows;
        const i32 row = i % rows;
        gui.fill(static_cast<f32>(x0 + col * col_w + col * 5), static_cast<f32>(list_y + row * 9),
                 static_cast<f32>(col_w), 8.0F, kCell);
    }
    const i32 footer_y = list_y + rows * 9 + 1;
    if (!footer.empty()) {
        band(footer_y, static_cast<i32>(footer.size()) * 9);
    }
    for (i32 i = 0; i < n; ++i) {
        const i32 col = i / rows;
        const i32 row = i % rows;
        const i32 x   = x0 + col * col_w + col * 5;
        gui.blit(icons, static_cast<f32>(x + col_w - 11), static_cast<f32>(list_y + row * 9), 10.0F,
                 8.0F, 0.0F, static_cast<f32>(176 + ping_row(players[static_cast<usize>(i)]->latency) * 8),
                 10.0F, 8.0F, kSheet, kSheet);
    }
    y = header_y;
    for (const RunLine& line : header) {
        const auto w = static_cast<i32>(runs_width(line, font));
        (void)draw_runs(gui, static_cast<f32>(W / 2 - w / 2), static_cast<f32>(y), line, 255, true);
        y += 9;
    }
    for (i32 i = 0; i < n; ++i) {
        const Entry& p   = *players[static_cast<usize>(i)];
        const i32    col = i / rows;
        const i32    row = i % rows;
        const i32    x   = x0 + col * col_w + col * 5;
        const auto   ry  = static_cast<f32>(list_y + row * 9);
        const bool   spectator = p.game_mode == kSpectatorMode;
        (void)gui.text(static_cast<f32>(x), ry, names[static_cast<usize>(i)],
                       spectator ? kSpectator : 0xFFFFFFFFU, true);
        if (with_scores && !spectator) {
            const i32 x1 = x + name_w + 1;
            const i32 x2 = x1 + score_w;
            if (x2 - x1 > 5) {
                const std::string& s = scores[static_cast<usize>(i)];
                (void)gui.text(static_cast<f32>(x2) - font.width(s), ry, s, 0xFFFFFFFFU, true);
            }
        }
    }
    y = footer_y;
    for (const RunLine& line : footer) {
        const auto w = static_cast<i32>(runs_width(line, font));
        (void)draw_runs(gui, static_cast<f32>(W / 2 - w / 2), static_cast<f32>(y), line, 255, true);
        y += 9;
    }
}

}  // namespace ov::client
