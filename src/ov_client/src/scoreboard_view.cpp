#include "ov/client/scoreboard_view.hpp"

#include "ov/client/gui.hpp"
#include "ov/render/font.hpp"
#include "ov/render/language.hpp"
#include "ov/render/text_component.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace ov::client {

namespace sb = net::scoreboard;

namespace {

constexpr usize kMaxLines   = 15;
constexpr f32   kLineHeight = 9.0F;
/// Black at three tenths and four tenths: the rows and the title row.
constexpr u32 kRowBackground   = 0x4C000000U;
constexpr u32 kTitleBackground = 0x66000000U;

bool less_ignoring_case(std::string_view a, std::string_view b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
    });
}

}  // namespace

void ScoreboardView::apply(const sb::DisplayObjective& packet) {
    if (packet.slot < display_.size()) {
        display_[packet.slot] = packet.objective;
    }
}

void ScoreboardView::apply(const sb::ObjectiveUpdate& packet) {
    switch (packet.mode) {
    case sb::ObjectiveMode::Create:
        objectives_[packet.name] = Objective{packet.display_json, packet.render_type};
        scores_[packet.name].clear();
        break;
    case sb::ObjectiveMode::Update:
        objectives_[packet.name] = Objective{packet.display_json, packet.render_type};
        break;
    case sb::ObjectiveMode::Remove:
        objectives_.erase(packet.name);
        scores_.erase(packet.name);
        for (std::string& slot : display_) {
            if (slot == packet.name) {
                slot.clear();
            }
        }
        break;
    }
}

void ScoreboardView::apply(const sb::TeamUpdate& packet) {
    switch (packet.mode) {
    case sb::TeamMode::Create:
        teams_[packet.name] = packet.parameters;
        [[fallthrough]];
    case sb::TeamMode::AddEntities:
        for (const std::string& member : packet.entities) {
            team_of_[member] = packet.name;
        }
        break;
    case sb::TeamMode::Update:
        teams_[packet.name] = packet.parameters;
        break;
    case sb::TeamMode::RemoveEntities:
        for (const std::string& member : packet.entities) {
            if (const auto it = team_of_.find(member); it != team_of_.end() && it->second == packet.name) {
                team_of_.erase(it);
            }
        }
        break;
    case sb::TeamMode::Remove:
        teams_.erase(packet.name);
        std::erase_if(team_of_, [&](const auto& entry) { return entry.second == packet.name; });
        break;
    }
}

void ScoreboardView::apply(const sb::ScoreUpdate& packet) {
    if (packet.action == sb::ScoreAction::Change) {
        scores_[packet.objective][packet.holder] = packet.value;
        return;
    }
    if (packet.objective.empty()) {
        for (auto& [objective, holders] : scores_) {
            holders.erase(packet.holder);
        }
    } else if (const auto it = scores_.find(packet.objective); it != scores_.end()) {
        it->second.erase(packet.holder);
    }
}

std::string ScoreboardView::sidebar_objective() const {
    if (const auto own = team_of_.find(own_name_); !own_name_.empty() && own != team_of_.end()) {
        if (const auto team = teams_.find(own->second);
            team != teams_.end() && team->second.color >= 0 && team->second.color < 16) {
            const std::string& shown = display_[sb::kSlotTeamBase + static_cast<usize>(team->second.color)];
            if (!shown.empty()) {
                return shown;
            }
        }
    }
    return display_[sb::kSlotSidebar];
}

std::vector<ScoreboardView::Line> ScoreboardView::sidebar_lines() const {
    std::vector<Line> lines;
    const auto        it = scores_.find(sidebar_objective());
    if (it == scores_.end()) {
        return lines;
    }
    for (const auto& [holder, score] : it->second) {
        if (!holder.starts_with('#')) {
            lines.push_back(Line{holder, score});
        }
    }
    std::ranges::sort(lines, [](const Line& a, const Line& b) {
        return a.score != b.score ? a.score > b.score : less_ignoring_case(a.holder, b.holder);
    });
    if (lines.size() > kMaxLines) {
        lines.resize(kMaxLines);
    }
    return lines;
}

std::string ScoreboardView::dressed_name(std::string_view holder, const render::Language& language) const {
    const auto member = team_of_.find(std::string{holder});
    if (member == team_of_.end()) {
        return std::string{holder};
    }
    const auto team = teams_.find(member->second);
    if (team == teams_.end()) {
        return std::string{holder};
    }
    static constexpr std::string_view kCodes = "0123456789abcdef";
    const sb::TeamParameters& p      = team->second;
    const std::string         colour = p.color >= 0 && p.color < 16
                                           ? std::string{"§"} + kCodes[static_cast<usize>(p.color)]
                                           : std::string{};
    return colour + render::flatten_component(p.prefix_json, language) + colour + std::string{holder} +
           render::flatten_component(p.suffix_json, language);
}

std::optional<i32> ScoreboardView::below_name_score(std::string_view holder) const {
    const auto it = scores_.find(display_[sb::kSlotBelowName]);
    if (display_[sb::kSlotBelowName].empty() || it == scores_.end()) {
        return std::nullopt;
    }
    const auto score = it->second.find(std::string{holder});
    return score == it->second.end() ? std::nullopt : std::optional<i32>{score->second};
}

void ScoreboardView::draw(Gui& gui, const render::Language& language) const {
    const std::string name      = sidebar_objective();
    const auto        objective = objectives_.find(name);
    if (name.empty() || objective == objectives_.end() || !gui.has_font()) {
        return;
    }
    const render::Font& font  = gui.font();
    const std::string   title = render::flatten_component(objective->second.display_json, language);
    struct Row {
        std::string name;
        std::string score;
    };
    std::vector<Row> rows;
    f32              width = font.width(title);
    for (const Line& line : sidebar_lines()) {
        Row row{dressed_name(line.holder, language), "§c" + std::to_string(line.score)};
        width = std::max(width, font.width(row.name) + font.width(": ") + font.width(row.score));
        rows.push_back(std::move(row));
    }
    const f32 height = static_cast<f32>(rows.size()) * kLineHeight;
    const f32 bottom = std::floor(gui.height() / 2.0F + height / 3.0F);
    const f32 left   = std::floor(gui.width() - width - 3.0F);
    const f32 right  = gui.width() - 1.0F;
    f32       top    = bottom;
    for (usize i = 0; i < rows.size(); ++i) {
        const f32 y = bottom - static_cast<f32>(i + 1) * kLineHeight;
        gui.fill(left - 2.0F, y, right - (left - 2.0F), kLineHeight, kRowBackground);
        (void)gui.text(left, y, rows[i].name, 0xFFFFFFFFU, false);
        (void)gui.text_right(right - 2.0F, y, rows[i].score, 0xFFFFFFFFU, false);
        top = y;
    }
    // The title row, above the highest score.
    gui.fill(left - 2.0F, top - kLineHeight - 1.0F, right - (left - 2.0F), kLineHeight, kTitleBackground);
    gui.fill(left - 2.0F, top - 1.0F, right - (left - 2.0F), 1.0F, kRowBackground);
    (void)gui.text_centred(left + width / 2.0F, top - kLineHeight, title, 0xFFFFFFFFU, false);
}

}  // namespace ov::client
