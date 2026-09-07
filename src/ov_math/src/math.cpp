#include "ov/math/block_pos.hpp"
#include "ov/math/vec.hpp"

namespace ov {

// Compile-time proof that the section index layout is YZX and dense. If anyone
// reorders it, chunks stop round-tripping through Anvil and the network packet
// at the same time, and this fires at build time instead.
static_assert(BlockPos{0, 0, 0}.section_index() == 0);
static_assert(BlockPos{1, 0, 0}.section_index() == 1);
static_assert(BlockPos{0, 0, 1}.section_index() == kSectionSize);
static_assert(BlockPos{0, 1, 0}.section_index() == kSectionSize * kSectionSize);
static_assert(BlockPos{15, 15, 15}.section_index() == kSectionVolume - 1);

std::string_view direction_name(Direction d) noexcept {
    switch (d) {
        case Direction::Down: return "down";
        case Direction::Up: return "up";
        case Direction::North: return "north";
        case Direction::South: return "south";
        case Direction::West: return "west";
        case Direction::East: return "east";
    }
    return "down";
}

std::optional<Direction> direction_from_name(std::string_view name) noexcept {
    if (name == "down")
        return Direction::Down;
    if (name == "up")
        return Direction::Up;
    if (name == "north")
        return Direction::North;
    if (name == "south")
        return Direction::South;
    if (name == "west")
        return Direction::West;
    if (name == "east")
        return Direction::East;
    return std::nullopt;
}

}  // namespace ov
