// The creative inventory's saved hotbars: nine rows of nine stacks, kept by
// the *client* in `hotbar.nbt`, never sent to a server.
//
// C+digit saves the current hotbar into that row, X+digit loads it back
// (vanilla's default bindings, key.saveToolbarActivator and
// key.loadToolbarActivator). The saved rows are shown on the "Saved Hotbars"
// tab, where a click takes a stack like any catalogue cell.
//
// The file is vanilla's own layout, so a hotbar.nbt copied out of a vanilla
// game directory loads here and one written here loads there: an uncompressed
// NBT compound holding `DataVersion` and one list per row, keyed "0" to "8",
// of nine item compounds `{id, Count, tag}` — an empty stack is
// `{id:"minecraft:air", Count:0b}`.
#pragma once

#include "ov/base/types.hpp"

#include <array>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ov::client {

/// A stack as the hotbar file holds it: by name, so a file stays valid when
/// the protocol ids move, with the Slot-format NBT bytes the rest of the
/// client carries (TAG_Compound, empty name, payload — or nothing).
struct SavedStack {
    std::string     item;
    i32             count{0};
    std::vector<u8> nbt;

    [[nodiscard]] bool empty() const noexcept {
        return item.empty() || item == "minecraft:air" || count <= 0;
    }
};

enum class SavedHotbarsError : u8 {
    /// Not NBT, or not the layout above.
    Malformed,
    /// The file could not be written.
    WriteFailed,
};

[[nodiscard]] std::string_view to_string(SavedHotbarsError error) noexcept;

class SavedHotbars {
public:
    static constexpr usize kRows    = 9;
    static constexpr usize kColumns = 9;
    /// 1.20.1's data version, written into the file as vanilla does.
    static constexpr i32 kDataVersion = 3465;

    using Row = std::array<SavedStack, kColumns>;

    /// Nine empty rows.
    SavedHotbars() = default;

    /// Read a file. A missing file is not an error — it is nine empty rows,
    /// which is what vanilla shows before anything is saved.
    [[nodiscard]] static std::expected<SavedHotbars, SavedHotbarsError> load(
        const std::filesystem::path& file);

    [[nodiscard]] static std::expected<SavedHotbars, SavedHotbarsError> parse(
        std::span<const u8> bytes);

    [[nodiscard]] std::vector<u8> serialise() const;

    [[nodiscard]] std::expected<void, SavedHotbarsError> save(
        const std::filesystem::path& file) const;

    [[nodiscard]] const Row& row(usize index) const noexcept { return rows_[index]; }

    void set_row(usize index, const Row& row) { rows_[index] = row; }

    /// True when every stack of the row is empty.
    [[nodiscard]] bool row_empty(usize index) const noexcept;

private:
    std::array<Row, kRows> rows_{};
};

}  // namespace ov::client
