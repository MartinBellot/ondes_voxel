// Stringified NBT: `stone{display:{Name:'"x"'}}`, `summon cow ~ ~ ~ {NoAI:1b}`.
//
// Read into `nbt::Tag` so the result is the same object a chunk or a slot
// carries, and written back the way vanilla prints a tag in a hover event —
// `{Damage:0}`, `{display:{Name:'"x"'}}` — which the capture shows verbatim.
#pragma once

#include "string_reader.hpp"

#include "ov/nbt/tag.hpp"

#include <string>

namespace ov::server::cmd {

/// `{…}` at the reader's cursor.
[[nodiscard]] Parsed<nbt::Tag> read_snbt_compound(StringReader& reader);

/// Any value at the reader's cursor.
[[nodiscard]] Parsed<nbt::Tag> read_snbt_value(StringReader& reader);

/// The tag as vanilla prints it: compound keys sorted, suffixes `b s L f d`,
/// strings quoted with whichever quote they do not contain.
[[nodiscard]] std::string to_snbt(const nbt::Tag& tag);

/// Does `tag` contain everything `pattern` does? Vanilla's partial NBT match,
/// used by `clear` with an NBT filter: compounds match key by key, a list
/// pattern matches if each of its elements is found somewhere in the list.
[[nodiscard]] bool snbt_matches(const nbt::Tag& pattern, const nbt::Tag& tag);

}  // namespace ov::server::cmd
