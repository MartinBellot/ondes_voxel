#include "suggestions.hpp"

#include <algorithm>
#include <cctype>

namespace ov::server::cmd {

SuggestionsBuilder::SuggestionsBuilder(std::string_view input, usize start)
    : input_{input}, start_{std::min(start, input.size())} {}

std::string SuggestionsBuilder::remaining_lower() const {
    std::string out{remaining()};
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

void SuggestionsBuilder::suggest(std::string text, std::optional<Text> tooltip) {
    if (text == remaining()) {
        return;
    }
    entries_.push_back(SuggestionEntry{start_, input_.size(), std::move(text), std::move(tooltip)});
}

void SuggestionsBuilder::add(const SuggestionsBuilder& other) {
    entries_.insert(entries_.end(), other.entries_.begin(), other.entries_.end());
}

MergedSuggestions merge_suggestions(std::string_view                    input,
                                    const std::vector<SuggestionEntry>& entries) {
    MergedSuggestions out;
    if (entries.empty()) {
        return out;
    }
    usize start = entries.front().start;
    usize end   = entries.front().end;
    for (const SuggestionEntry& e : entries) {
        start = std::min(start, e.start);
        end   = std::max(end, e.end);
    }
    out.start  = start;
    out.length = end - start;
    for (const SuggestionEntry& e : entries) {
        std::string text;
        if (start < e.start) {
            text += input.substr(start, e.start - start);
        }
        text += e.text;
        if (end > e.end && e.end <= input.size()) {
            text += input.substr(e.end, end - e.end);
        }
        const bool seen = std::ranges::any_of(out.matches, [&](const auto& m) {
            return m.first == text &&
                   (m.second.has_value() == e.tooltip.has_value()) &&
                   (!m.second || to_json(*m.second) == to_json(*e.tooltip));
        });
        if (!seen) {
            out.matches.emplace_back(std::move(text), e.tooltip);
        }
    }
    std::ranges::stable_sort(out.matches, [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

bool matches_substring(std::string_view input, std::string_view candidate) {
    usize i = 0;
    while (candidate.substr(std::min(i, candidate.size())).substr(0, input.size()) != input) {
        const auto underscore = candidate.find('_', i);
        if (underscore == std::string_view::npos) {
            return false;
        }
        i = underscore + 1;
    }
    return true;
}

void suggest_resources(SuggestionsBuilder& builder, const std::vector<std::string>& ids,
                       std::string_view prefix, const std::vector<std::optional<Text>>* tooltips) {
    std::string typed = builder.remaining_lower();
    if (!prefix.empty()) {
        if (typed.substr(0, prefix.size()) != prefix) {
            // "!" typed partially, or not at all: only offer when what is typed
            // could still become this family.
            if (std::string_view{prefix}.substr(0, typed.size()) != typed) {
                return;
            }
            typed.clear();
        } else {
            typed = typed.substr(prefix.size());
        }
    }
    const bool has_colon = typed.find(':') != std::string::npos;
    for (usize i = 0; i < ids.size(); ++i) {
        const std::string& id        = ids[i];
        const auto         colon     = id.find(':');
        const std::string  name_space = colon == std::string::npos ? "minecraft" : id.substr(0, colon);
        const std::string  path       = colon == std::string::npos ? id : id.substr(colon + 1);
        bool               matched    = false;
        if (has_colon) {
            matched = matches_substring(typed, id);
        } else {
            matched = matches_substring(typed, name_space) ||
                      (name_space == "minecraft" && matches_substring(typed, path));
        }
        if (matched) {
            std::optional<Text> tooltip;
            if (tooltips != nullptr && i < tooltips->size()) {
                tooltip = (*tooltips)[i];
            }
            builder.suggest(std::string{prefix} + id, std::move(tooltip));
        }
    }
}

}  // namespace ov::server::cmd
