#include "game_rules.hpp"

#include "string_reader.hpp"

#include <limits>

namespace ov::server::cmd {

GameRules::GameRules() {
    for (usize i = 0; i < kGameRules.size(); ++i) {
        values_[i] = kGameRules[i].default_value;
    }
}

std::optional<usize> GameRules::index_of(std::string_view name) noexcept {
    for (usize i = 0; i < kGameRules.size(); ++i) {
        if (kGameRules[i].name == name) {
            return i;
        }
    }
    return std::nullopt;
}

bool GameRules::flag(std::string_view name) const noexcept {
    const auto index = index_of(name);
    return index && values_[*index] != 0;
}

i32 GameRules::number(std::string_view name) const noexcept {
    const auto index = index_of(name);
    return index ? values_[*index] : 0;
}

std::string GameRules::text(usize index) const {
    if (kGameRules[index].integer) {
        return std::to_string(values_[index]);
    }
    return values_[index] != 0 ? "true" : "false";
}

void GameRules::load(const std::vector<std::pair<std::string, std::string>>& stored) {
    for (const auto& [name, value] : stored) {
        const auto index = index_of(name);
        if (!index) {
            unknown_.emplace_back(name, value);
            continue;
        }
        if (kGameRules[*index].integer) {
            // Integer.parseInt; a value it refuses keeps the default, which is
            // what vanilla's rule deserialiser does with garbage.
            if (const auto parsed = parse_java_long(value, std::numeric_limits<i32>::min(),
                                                    std::numeric_limits<i32>::max())) {
                values_[*index] = static_cast<i32>(*parsed);
            }
        } else {
            // Boolean.parseBoolean: "true" in any case is true, anything else
            // false.
            std::string lowered = value;
            for (char& c : lowered) {
                c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
            }
            values_[*index] = lowered == "true" ? 1 : 0;
        }
    }
}

std::vector<std::pair<std::string, std::string>> GameRules::store() const {
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(kGameRules.size() + unknown_.size());
    for (usize i = 0; i < kGameRules.size(); ++i) {
        out.emplace_back(std::string{kGameRules[i].name}, text(i));
    }
    out.insert(out.end(), unknown_.begin(), unknown_.end());
    return out;
}

}  // namespace ov::server::cmd
