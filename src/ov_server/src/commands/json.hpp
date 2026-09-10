// A strict JSON reader, for the three places the server reads JSON from a
// person: `tellraw` and `title` components, and `ops.json`.
//
// Strict because vanilla's is: `tellraw @s {bad json` answers "Use
// JsonReader.setLenient(true) to accept malformed JSON at line 1 column 3 path
// $." — Gson's own message, which the capture recorded and which this reader
// reproduces for the malformed-token case. Other Gson messages are
// approximated, and `docs/provenance/commandes.md` says which.
//
// Hostile input is the normal case here: depth is bounded, every read is
// bounds-checked, and nothing throws.
#pragma once

#include "ov/base/types.hpp"

#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ov::server::cmd {

struct JsonValue {
    enum class Type : u8 { Null, Bool, Number, String, Array, Object };

    Type type{Type::Null};
    bool boolean{false};
    /// The string's value, or the number exactly as written: Gson keeps a
    /// number's text, so `12` becomes the component text "12" and `1.0` stays
    /// "1.0".
    std::string                                   string;
    std::vector<JsonValue>                        array;
    std::vector<std::pair<std::string, JsonValue>> object;

    [[nodiscard]] const JsonValue* find(std::string_view key) const noexcept;
};

struct JsonError {
    std::string message;
    usize       offset{0};
};

/// Read one value from the start of `text`. `consumed` receives how many
/// bytes it used — a command argument continues after it.
[[nodiscard]] std::expected<JsonValue, JsonError> parse_json(std::string_view text,
                                                             usize&           consumed);

}  // namespace ov::server::cmd
