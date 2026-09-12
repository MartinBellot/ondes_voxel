#include "hud_script.hpp"

#include <fmt/format.h>

#include <charconv>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace ov::demo {
namespace {

constexpr f64 kTick       = 0.05;
constexpr f64 kShotSettle = 0.12;

[[nodiscard]] std::string_view strip(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

[[nodiscard]] bool parse_number(std::string_view text, i32& out) {
    const auto* end    = text.data() + text.size();
    const auto  result = std::from_chars(text.data(), end, out);
    return result.ec == std::errc{} && result.ptr == end;
}

/// Degrees. strtof rather than from_chars: the platform library's
/// floating-point from_chars is not there everywhere.
[[nodiscard]] bool parse_number(std::string_view text, f32& out) {
    const std::string copy(text);
    char*             end = nullptr;
    out                   = std::strtof(copy.c_str(), &end);
    return !copy.empty() && end == copy.c_str() + copy.size();
}

}  // namespace

std::expected<std::vector<HudStep>, std::string> parse_hud_scenes(std::string_view text) {
    std::vector<HudStep> steps;
    usize                line_number = 0;
    while (!text.empty()) {
        const usize      eol  = text.find('\n');
        std::string_view line = strip(text.substr(0, eol));
        text                  = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);
        ++line_number;
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const usize      space = line.find(' ');
        const auto       verb  = line.substr(0, space);
        const auto       rest  = space == std::string_view::npos ? std::string_view{} : strip(line.substr(space));
        HudStep          step;
        const auto       bad = [&](std::string_view why) {
            return std::unexpected(fmt::format("line {}: {} ({})", line_number, why, line));
        };
        if (verb == "cmd") {
            step.kind = HudStep::Kind::Command;
            step.text = std::string(rest);
        } else if (verb == "wait") {
            step.kind = HudStep::Kind::Wait;
            if (!parse_number(rest, step.ticks) || step.ticks < 0) {
                return bad("a wait is a number of ticks");
            }
        } else if (verb == "shot") {
            step.kind = HudStep::Kind::Shot;
            step.text = std::string(rest);
            if (step.text.empty()) {
                return bad("a shot needs a name");
            }
        } else if (verb == "key") {
            step.kind         = HudStep::Kind::Key;
            const usize split = rest.find(' ');
            step.text         = std::string(rest.substr(0, split));
            step.second = split == std::string_view::npos ? std::string{} : std::string(strip(rest.substr(split)));
            if ((step.text != "tab" && step.text != "f3" && step.text != "e" && step.text != "esc") ||
                (step.second != "press" && step.second != "release" && step.second != "tap")) {
                return bad("key tab|f3|e|esc press|release|tap");
            }
        } else if (verb == "use") {
            step.kind = HudStep::Kind::Use;
        } else if (verb == "look") {
            step.kind         = HudStep::Kind::Look;
            const usize split = rest.find(' ');
            if (split == std::string_view::npos || !parse_number(rest.substr(0, split), step.yaw) ||
                !parse_number(strip(rest.substr(split)), step.pitch)) {
                return bad("look <yaw> <pitch>");
            }
        } else if (verb == "tabfoot") {
            step.kind         = HudStep::Kind::TabFoot;
            const usize split = rest.find('|');
            if (split == std::string_view::npos) {
                return bad("tabfoot <header json> | <footer json>");
            }
            step.text   = std::string(strip(rest.substr(0, split)));
            step.second = std::string(strip(rest.substr(split + 1)));
        } else {
            return bad("unknown verb");
        }
        steps.push_back(std::move(step));
    }
    return steps;
}

std::expected<HudScript, std::string> HudScript::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        return std::unexpected(fmt::format("{}: cannot be read", path));
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    auto steps = parse_hud_scenes(buffer.str());
    if (!steps) {
        return std::unexpected(fmt::format("{}: {}", path, steps.error()));
    }
    return HudScript{std::move(*steps)};
}

std::optional<HudStep> HudScript::next(f64 now) {
    while (index_ < steps_.size()) {
        const HudStep& step = steps_[index_];
        if (ready_at_ && now < *ready_at_) {
            return std::nullopt;
        }
        if (step.kind == HudStep::Kind::Wait) {
            if (!ready_at_) {
                ready_at_ = now + kTick * static_cast<f64>(step.ticks);
                continue;
            }
            ready_at_.reset();
            ++index_;
            continue;
        }
        if (step.kind == HudStep::Kind::Shot && !ready_at_) {
            ready_at_ = now + kShotSettle;
            continue;
        }
        ready_at_.reset();
        return steps_[index_++];
    }
    return std::nullopt;
}

}  // namespace ov::demo
