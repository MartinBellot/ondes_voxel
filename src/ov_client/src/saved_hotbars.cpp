#define OV_LOG_CATEGORY "client"

#include "ov/client/saved_hotbars.hpp"

#include "ov/base/log.hpp"
#include "ov/io/byte_writer.hpp"
#include "ov/nbt/binary.hpp"
#include "ov/nbt/tag.hpp"

#include <fstream>
#include <iterator>
#include <string>

namespace ov::client {

namespace {

[[nodiscard]] nbt::Tag stack_tag(const SavedStack& stack) {
    nbt::Tag tag = nbt::Tag::make_compound();
    if (stack.empty()) {
        // Vanilla's empty stack, exactly: ItemStack.EMPTY saves as air x0.
        tag.put("id", nbt::Tag(std::string("minecraft:air")));
        tag.put("Count", nbt::Tag(i8{0}));
        return tag;
    }
    tag.put("id", nbt::Tag(stack.item));
    tag.put("Count", nbt::Tag(static_cast<i8>(stack.count)));
    if (!stack.nbt.empty()) {
        if (auto document = nbt::read(stack.nbt); document && !document->root.is_end()) {
            tag.put("tag", std::move(document->root));
        }
    }
    return tag;
}

[[nodiscard]] SavedStack stack_of(const nbt::Tag& tag) {
    SavedStack stack;
    if (const nbt::Tag* id = tag.find("id")) {
        stack.item = std::string(id->as_string());
    }
    if (const nbt::Tag* count = tag.find("Count")) {
        stack.count = static_cast<i32>(count->as_i64(0));
    }
    if (const nbt::Tag* inner = tag.find("tag"); inner != nullptr && !inner->is_end()) {
        // Back into the Slot's own form: a named-empty root compound.
        stack.nbt = nbt::write(nbt::Document{std::string{}, *inner});
    }
    if (stack.empty()) {
        stack = SavedStack{};
    }
    return stack;
}

}  // namespace

std::string_view to_string(SavedHotbarsError error) noexcept {
    switch (error) {
        case SavedHotbarsError::Malformed:
            return "malformed hotbar file";
        case SavedHotbarsError::WriteFailed:
            return "the hotbar file could not be written";
    }
    return "unknown";
}

std::expected<SavedHotbars, SavedHotbarsError> SavedHotbars::load(
    const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return SavedHotbars{};
    }
    const std::vector<u8> bytes((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    return parse(bytes);
}

std::expected<SavedHotbars, SavedHotbarsError> SavedHotbars::parse(std::span<const u8> bytes) {
    auto document = nbt::read(bytes);
    if (!document || document->root.compound() == nullptr) {
        return std::unexpected(SavedHotbarsError::Malformed);
    }
    SavedHotbars out;
    for (usize row = 0; row < kRows; ++row) {
        const nbt::Tag* list = document->root.find(std::to_string(row));
        if (list == nullptr || list->list() == nullptr) {
            continue;
        }
        const auto& stacks = *list->list();
        for (usize column = 0; column < kColumns && column < stacks.size(); ++column) {
            out.rows_[row][column] = stack_of(stacks[column]);
        }
    }
    return out;
}

std::vector<u8> SavedHotbars::serialise() const {
    nbt::Tag root = nbt::Tag::make_compound();
    for (usize row = 0; row < kRows; ++row) {
        nbt::Tag list = nbt::Tag::make_list(nbt::TagType::Compound);
        for (const SavedStack& stack : rows_[row]) {
            (void)list.push(stack_tag(stack));
        }
        root.put(std::to_string(row), std::move(list));
    }
    root.put("DataVersion", nbt::Tag(kDataVersion));
    return nbt::write(nbt::Document{std::string{}, std::move(root)});
}

std::expected<void, SavedHotbarsError> SavedHotbars::save(const std::filesystem::path& file) const {
    const std::vector<u8> bytes = serialise();
    std::error_code       error;
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path(), error);
    }
    // Written beside the target and renamed over it, so a crash mid-write
    // leaves the old file rather than half of a new one.
    const std::filesystem::path temporary = file.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            return std::unexpected(SavedHotbarsError::WriteFailed);
        }
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            return std::unexpected(SavedHotbarsError::WriteFailed);
        }
    }
    std::filesystem::rename(temporary, file, error);
    if (error) {
        return std::unexpected(SavedHotbarsError::WriteFailed);
    }
    return {};
}

bool SavedHotbars::row_empty(usize index) const noexcept {
    for (const SavedStack& stack : rows_[index]) {
        if (!stack.empty()) {
            return false;
        }
    }
    return true;
}

}  // namespace ov::client
