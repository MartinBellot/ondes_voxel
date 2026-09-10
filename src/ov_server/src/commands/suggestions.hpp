// Tab completion, the way the client is answered.
//
// A builder collects candidates for the text between `start` and the cursor;
// several builders — a block id, then a property name inside `[` — are merged
// into one range, each candidate widened to it, and sorted. The capture shows
// the sort (`"!#minecraft:arrows"` before `"minecraft:allay"`) and the ranges
// (`/time s` answers `set` at 6, length 1).
#pragma once

#include "text.hpp"

#include "ov/base/types.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ov::server::cmd {

struct SuggestionEntry {
    usize                start{0};
    usize                end{0};
    std::string          text;
    std::optional<Text>  tooltip;
};

class SuggestionsBuilder {
public:
    SuggestionsBuilder(std::string_view input, usize start);

    [[nodiscard]] std::string_view input() const noexcept { return input_; }
    [[nodiscard]] usize            start() const noexcept { return start_; }
    [[nodiscard]] std::string_view remaining() const noexcept { return input_.substr(start_); }
    [[nodiscard]] std::string      remaining_lower() const;

    /// Offer `text` for [start, end of input). Skipped when it is exactly what
    /// is already typed, as Brigadier does.
    void suggest(std::string text, std::optional<Text> tooltip = std::nullopt);

    [[nodiscard]] SuggestionsBuilder at(usize start) const { return {input_, start}; }
    void add(const SuggestionsBuilder& other);

    [[nodiscard]] const std::vector<SuggestionEntry>& entries() const noexcept { return entries_; }

private:
    std::string_view             input_;
    usize                        start_{0};
    std::vector<SuggestionEntry> entries_;
};

struct MergedSuggestions {
    usize                                                  start{0};
    usize                                                  length{0};
    std::vector<std::pair<std::string, std::optional<Text>>> matches;
};

/// One range for all of them, every candidate widened to it, deduplicated and
/// sorted.
[[nodiscard]] MergedSuggestions merge_suggestions(std::string_view                    input,
                                                  const std::vector<SuggestionEntry>& entries);

/// Brigadier's shared filter: does `candidate` contain `input` at its start or
/// right after an underscore? `dia` matches `deepslate_diamond_ore`.
[[nodiscard]] bool matches_substring(std::string_view input, std::string_view candidate);

/// Offer resource ids: with a colon typed, matched against the full id;
/// without, against the namespace, or against the path when the namespace is
/// `minecraft`. `prefix` is prepended (`#` for tags, `!` for negation).
void suggest_resources(SuggestionsBuilder& builder, const std::vector<std::string>& ids,
                       std::string_view prefix = {},
                       const std::vector<std::optional<Text>>* tooltips = nullptr);

}  // namespace ov::server::cmd
