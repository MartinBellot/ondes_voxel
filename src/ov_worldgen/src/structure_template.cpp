#define OV_LOG_CATEGORY "worldgen"

#include "ov/worldgen/structure_template.hpp"

#include "ov/base/log.hpp"
#include "ov/io/compression.hpp"
#include "ov/io/zip.hpp"
#include "ov/math/random.hpp"
#include "ov/nbt/binary.hpp"

#include <simdjson.h>

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>

namespace ov::worldgen {

namespace {

constexpr std::string_view kStructuresPrefix = "data/minecraft/structures/";

/// A template file is a few kilobytes; the largest in 1.20.1 is well under a
/// megabyte decompressed. The cap bounds a crafted jar, nothing more.
constexpr usize kTemplateLimit = 16ull * 1024 * 1024;

void set_detail(std::string* detail, std::string text) {
    if (detail != nullptr) {
        *detail = std::move(text);
    }
}

// ── Directions ──────────────────────────────────────────────────────────────

/// The four horizontal directions in clockwise order, starting at north.
constexpr std::array<std::string_view, 4> kHorizontal{"north", "east", "south", "west"};

[[nodiscard]] i32 horizontal_index(std::string_view name) noexcept {
    for (usize index = 0; index < kHorizontal.size(); ++index) {
        if (kHorizontal[index] == name) {
            return static_cast<i32>(index);
        }
    }
    return -1;
}

/// Where a horizontal direction goes under a mirror. LEFT_RIGHT negates z, so
/// north and south swap; FRONT_BACK negates x, so east and west do.
[[nodiscard]] i32 mirror_direction(i32 index, Mirror mirror) noexcept {
    if (index < 0) {
        return index;
    }
    switch (mirror) {
        case Mirror::LeftRight: return (index == 0 || index == 2) ? (index + 2) & 3 : index;
        case Mirror::FrontBack: return (index == 1 || index == 3) ? (index + 2) & 3 : index;
        case Mirror::None: break;
    }
    return index;
}

[[nodiscard]] i32 rotate_direction(i32 index, Rotation rotation) noexcept {
    return index < 0 ? index : (index + static_cast<i32>(rotation)) & 3;
}

[[nodiscard]] std::string_view map_direction(std::string_view name, Mirror mirror,
                                             Rotation rotation) noexcept {
    const i32 index = horizontal_index(name);
    if (index < 0) {
        return name;
    }
    return kHorizontal[static_cast<usize>(
        rotate_direction(mirror_direction(index, mirror), rotation))];
}

/// A rail shape: two directions joined by `_`, or `ascending_<dir>`.
[[nodiscard]] std::string map_rail_shape(std::string_view shape, Mirror mirror, Rotation rotation) {
    if (shape.starts_with("ascending_")) {
        return "ascending_" + std::string{map_direction(shape.substr(10), mirror, rotation)};
    }
    const auto underscore = shape.find('_');
    if (underscore == std::string_view::npos) {
        return std::string{shape};
    }
    const std::string_view a = map_direction(shape.substr(0, underscore), mirror, rotation);
    const std::string_view b = map_direction(shape.substr(underscore + 1), mirror, rotation);
    const auto is_axis_z     = [](std::string_view d) { return d == "north" || d == "south"; };
    // Straight: "north_south" or "east_west", whichever order came out.
    if (is_axis_z(a) == is_axis_z(b)) {
        return is_axis_z(a) ? "north_south" : "east_west";
    }
    // A curve is always spelled with its north/south half first.
    return is_axis_z(a) ? std::string{a} + "_" + std::string{b}
                        : std::string{b} + "_" + std::string{a};
}

[[nodiscard]] i32 value_index(const registry::PropertyView& property, std::string_view value) {
    for (usize index = 0; index < property.values.size(); ++index) {
        if (property.values[index] == value) {
            return static_cast<i32>(index);
        }
    }
    return -1;
}

// ── State parsing ───────────────────────────────────────────────────────────

/// A `{Name, Properties}` compound, one property at a time from the default
/// state — trap 8 of the project's list: a partial set resolved in one call
/// has been seen to pick a waterlogged chest out of thin air.
[[nodiscard]] std::optional<registry::BlockStateId> state_from_compound(
    const nbt::Tag& entry, const registry::BlockRegistry& blocks, std::string* detail) {
    const nbt::Tag* name = entry.find("Name");
    if (name == nullptr) {
        set_detail(detail, "palette entry without Name");
        return std::nullopt;
    }
    const auto block = blocks.find_block(name->as_string());
    if (!block) {
        set_detail(detail, "unknown block " + std::string{name->as_string()});
        return std::nullopt;
    }
    registry::BlockStateId state = blocks.default_state(*block);
    if (const nbt::Tag* properties = entry.find("Properties")) {
        if (const auto* entries = properties->compound()) {
            for (const nbt::CompoundEntry& property : *entries) {
                const auto view = blocks.find_property(*block, property.name);
                if (!view) {
                    set_detail(detail, "block " + std::string{name->as_string()} +
                                           " has no property " + property.name);
                    return std::nullopt;
                }
                const i32 index = value_index(*view, property.value.as_string());
                if (index < 0) {
                    set_detail(detail, "block " + std::string{name->as_string()} + " property " +
                                           property.name + " has no value " +
                                           std::string{property.value.as_string()});
                    return std::nullopt;
                }
                state = blocks.with_property(state, *view, static_cast<u16>(index));
            }
        }
    }
    return state;
}

/// `minecraft:stone_bricks` or `minecraft:ladder[facing=north]`, the way a
/// jigsaw's `final_state` spells a state.
[[nodiscard]] std::optional<registry::BlockStateId> state_from_string(
    std::string_view text, const registry::BlockRegistry& blocks) {
    const auto             bracket = text.find('[');
    const std::string_view name    = text.substr(0, bracket);
    const auto             block   = blocks.find_block(name.find(':') == std::string_view::npos
                                                           ? "minecraft:" + std::string{name}
                                                           : std::string{name});
    if (!block) {
        return std::nullopt;
    }
    registry::BlockStateId state = blocks.default_state(*block);
    if (bracket == std::string_view::npos) {
        return state;
    }
    std::string_view rest = text.substr(bracket + 1);
    if (!rest.empty() && rest.back() == ']') {
        rest.remove_suffix(1);
    }
    while (!rest.empty()) {
        const auto             comma  = rest.find(',');
        const std::string_view pair   = rest.substr(0, comma);
        const auto             equals = pair.find('=');
        if (equals != std::string_view::npos) {
            const auto view = blocks.find_property(*block, pair.substr(0, equals));
            if (!view) {
                return std::nullopt;
            }
            const i32 index = value_index(*view, pair.substr(equals + 1));
            if (index < 0) {
                return std::nullopt;
            }
            state = blocks.with_property(state, *view, static_cast<u16>(index));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        rest = rest.substr(comma + 1);
    }
    return state;
}

}  // namespace

// ── Names ───────────────────────────────────────────────────────────────────

std::string_view to_string(TemplateError error) noexcept {
    switch (error) {
        case TemplateError::JarUnreadable: return "the server jar cannot be read";
        case TemplateError::NotFound: return "no such template";
        case TemplateError::Malformed: return "the template is malformed";
        case TemplateError::UnknownBlock: return "the template names an unknown block";
        case TemplateError::UnknownProcessor: return "a processor type is not implemented";
        case TemplateError::BadProcessor: return "a processor body is not understood";
    }
    return "unknown";
}

std::string_view to_string(Rotation rotation) noexcept {
    switch (rotation) {
        case Rotation::None: return "NONE";
        case Rotation::Clockwise90: return "CLOCKWISE_90";
        case Rotation::Clockwise180: return "CLOCKWISE_180";
        case Rotation::CounterClockwise90: return "COUNTERCLOCKWISE_90";
    }
    return "?";
}

std::string_view to_string(Mirror mirror) noexcept {
    switch (mirror) {
        case Mirror::None: return "NONE";
        case Mirror::LeftRight: return "LEFT_RIGHT";
        case Mirror::FrontBack: return "FRONT_BACK";
    }
    return "?";
}

std::optional<Rotation> parse_rotation(std::string_view name) noexcept {
    for (const Rotation rotation : {Rotation::None, Rotation::Clockwise90, Rotation::Clockwise180,
                                    Rotation::CounterClockwise90}) {
        if (to_string(rotation) == name) {
            return rotation;
        }
    }
    return std::nullopt;
}

std::optional<Mirror> parse_mirror(std::string_view name) noexcept {
    for (const Mirror mirror : {Mirror::None, Mirror::LeftRight, Mirror::FrontBack}) {
        if (to_string(mirror) == name) {
            return mirror;
        }
    }
    return std::nullopt;
}

void BoundingBox::encapsulate(const BoundingBox& other) noexcept {
    min_x = std::min(min_x, other.min_x);
    min_y = std::min(min_y, other.min_y);
    min_z = std::min(min_z, other.min_z);
    max_x = std::max(max_x, other.max_x);
    max_y = std::max(max_y, other.max_y);
    max_z = std::max(max_z, other.max_z);
}

// ── Arithmetic ──────────────────────────────────────────────────────────────

i64 position_seed(i32 x, i32 y, i32 z) noexcept {
    // Unsigned throughout so that the wrap is defined; the int product is
    // truncated to 32 bits and sign-extended before it meets the longs.
    const auto x_term =
        static_cast<i64>(static_cast<i32>(static_cast<u32>(x) * static_cast<u32>(3129871)));
    const u64 z_term = static_cast<u64>(static_cast<i64>(z)) * static_cast<u64>(116129781LL);
    u64       l      = static_cast<u64>(x_term) ^ z_term ^ static_cast<u64>(static_cast<i64>(y));
    l                = l * l * static_cast<u64>(42317861LL) + l * static_cast<u64>(11LL);
    return static_cast<i64>(l) >> 16;
}

BlockPos transform(BlockPos local, Mirror mirror, Rotation rotation, BlockPos pivot) noexcept {
    i32 x = local.x;
    i32 z = local.z;
    switch (mirror) {
        case Mirror::LeftRight: z = -z; break;
        case Mirror::FrontBack: x = -x; break;
        case Mirror::None: break;
    }
    const i32 px = pivot.x;
    const i32 pz = pivot.z;
    switch (rotation) {
        case Rotation::CounterClockwise90: return {px - pz + z, local.y, px + pz - x};
        case Rotation::Clockwise90: return {px + pz - z, local.y, pz - px + x};
        case Rotation::Clockwise180: return {px + px - x, local.y, pz + pz - z};
        case Rotation::None: break;
    }
    return {x, local.y, z};
}

registry::BlockStateId transform_state(const registry::BlockRegistry& blocks,
                                       registry::BlockStateId state, Mirror mirror,
                                       Rotation rotation) noexcept {
    if (mirror == Mirror::None && rotation == Rotation::None) {
        return state;
    }
    const registry::BlockId block      = blocks.block_of(state);
    const auto              properties = blocks.properties(block);
    if (properties.empty()) {
        return state;
    }

    registry::BlockStateId out = state;
    const auto set = [&](const registry::PropertyView& property, std::string_view value) {
        const i32 index = value_index(property, value);
        if (index >= 0) {
            out = blocks.with_property(out, property, static_cast<u16>(index));
        }
    };

    // The four sides as one permutation, read from the input state so that a
    // side written early is never read back as its own source.
    std::array<std::string_view, 4>              sides{};
    std::array<const registry::PropertyView*, 4> side_props{};
    bool                                         has_sides = false;

    for (const registry::PropertyView& property : properties) {
        const std::string_view value = blocks.property_value(state, property);
        const i32              side  = horizontal_index(property.name);
        if (side >= 0) {
            sides[static_cast<usize>(side)]      = value;
            side_props[static_cast<usize>(side)] = &property;
            has_sides                            = true;
            continue;
        }
        if (property.name == "facing" || property.name == "horizontal_facing") {
            set(property, map_direction(value, mirror, rotation));
        } else if (property.name == "axis") {
            if ((static_cast<u8>(rotation) & 1) != 0 && value != "y") {
                set(property, value == "x" ? "z" : "x");
            }
        } else if (property.name == "rotation" && property.values.size() == 16) {
            i32 r = value_index(property, value);
            switch (mirror) {
                case Mirror::FrontBack: r = (16 - r) & 15; break;
                case Mirror::LeftRight: r = (8 - r + 16) & 15; break;
                case Mirror::None: break;
            }
            r   = (r + 4 * static_cast<i32>(rotation)) & 15;
            out = blocks.with_property(out, property, static_cast<u16>(r));
        } else if (property.name == "shape" && value_index(property, "north_south") >= 0) {
            set(property, map_rail_shape(value, mirror, rotation));
        } else if (property.name == "shape" && mirror != Mirror::None) {
            // Stairs: a mirror reverses handedness.
            if (value.ends_with("_left")) {
                set(property, std::string{value.substr(0, value.size() - 5)} + "_right");
            } else if (value.ends_with("_right")) {
                set(property, std::string{value.substr(0, value.size() - 6)} + "_left");
            }
        } else if ((property.name == "hinge" || property.name == "type") &&
                   mirror != Mirror::None && value_index(property, "left") >= 0) {
            if (value == "left") {
                set(property, "right");
            } else if (value == "right") {
                set(property, "left");
            }
        }
    }

    if (has_sides) {
        for (i32 side = 0; side < 4; ++side) {
            const auto* property = side_props[static_cast<usize>(side)];
            if (property == nullptr) {
                continue;
            }
            const i32   target      = rotate_direction(mirror_direction(side, mirror), rotation);
            const auto* destination = side_props[static_cast<usize>(target)];
            if (destination != nullptr) {
                set(*destination, sides[static_cast<usize>(side)]);
            }
        }
    }
    return out;
}

// ── Parsing ─────────────────────────────────────────────────────────────────

std::expected<StructureTemplate, TemplateError> StructureTemplate::parse(
    std::span<const u8> bytes, const registry::BlockRegistry& blocks, std::string* detail) {
    std::vector<u8> raw;
    if (bytes.size() >= 2 && bytes[0] == 0x1F && bytes[1] == 0x8B) {
        auto inflated = io::gzip_decompress(bytes, kTemplateLimit);
        if (!inflated) {
            set_detail(detail, "gzip: " + std::string{io::to_string(inflated.error())});
            return std::unexpected(TemplateError::Malformed);
        }
        raw   = std::move(*inflated);
        bytes = raw;
    }
    auto document = nbt::read(bytes);
    if (!document) {
        set_detail(detail, "nbt: " + std::string{nbt::to_string(document.error())});
        return std::unexpected(TemplateError::Malformed);
    }
    const nbt::Tag& root = document->root;

    StructureTemplate out;
    const nbt::Tag*   size = root.find("size");
    if (size == nullptr || size->list() == nullptr || size->list()->size() != 3) {
        set_detail(detail, "no size");
        return std::unexpected(TemplateError::Malformed);
    }
    out.size = {static_cast<i32>((*size->list())[0].as_i64()),
                static_cast<i32>((*size->list())[1].as_i64()),
                static_cast<i32>((*size->list())[2].as_i64())};

    const auto read_palette = [&](const nbt::Tag& list)
        -> std::expected<std::vector<registry::BlockStateId>, TemplateError> {
        std::vector<registry::BlockStateId> palette;
        if (list.list() == nullptr) {
            return std::unexpected(TemplateError::Malformed);
        }
        for (const nbt::Tag& entry : *list.list()) {
            const auto state = state_from_compound(entry, blocks, detail);
            if (!state) {
                return std::unexpected(TemplateError::UnknownBlock);
            }
            palette.push_back(*state);
        }
        return palette;
    };

    if (const nbt::Tag* palette = root.find("palette")) {
        auto parsed = read_palette(*palette);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        out.palettes.push_back(std::move(*parsed));
    } else if (const nbt::Tag* palettes = root.find("palettes");
               palettes != nullptr && palettes->list() != nullptr) {
        for (const nbt::Tag& one : *palettes->list()) {
            auto parsed = read_palette(one);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            out.palettes.push_back(std::move(*parsed));
        }
    }
    if (out.palettes.empty()) {
        set_detail(detail, "no palette");
        return std::unexpected(TemplateError::Malformed);
    }
    const usize palette_size = out.palettes.front().size();

    const nbt::Tag* blocks_tag = root.find("blocks");
    if (blocks_tag == nullptr || blocks_tag->list() == nullptr) {
        set_detail(detail, "no blocks");
        return std::unexpected(TemplateError::Malformed);
    }
    for (const nbt::Tag& entry : *blocks_tag->list()) {
        const nbt::Tag* state = entry.find("state");
        const nbt::Tag* pos   = entry.find("pos");
        if (state == nullptr || pos == nullptr || pos->list() == nullptr ||
            pos->list()->size() != 3) {
            set_detail(detail, "block without state or pos");
            return std::unexpected(TemplateError::Malformed);
        }
        const auto index = static_cast<u32>(state->as_i64(-1));
        if (index >= palette_size) {
            set_detail(detail, "state index outside the palette");
            return std::unexpected(TemplateError::Malformed);
        }
        TemplateBlock block;
        block.pos   = {static_cast<i32>((*pos->list())[0].as_i64()),
                       static_cast<i32>((*pos->list())[1].as_i64()),
                       static_cast<i32>((*pos->list())[2].as_i64())};
        block.state = index;
        if (const nbt::Tag* data = entry.find("nbt")) {
            block.nbt = static_cast<i32>(out.block_nbt.size());
            out.block_nbt.push_back(*data);
        }
        out.blocks.push_back(block);
    }

    if (const nbt::Tag* entities = root.find("entities");
        entities != nullptr && entities->list() != nullptr) {
        for (const nbt::Tag& entry : *entities->list()) {
            TemplateEntity entity;
            if (const nbt::Tag* pos = entry.find("pos");
                pos != nullptr && pos->list() != nullptr && pos->list()->size() == 3) {
                entity.x = (*pos->list())[0].as_f64();
                entity.y = (*pos->list())[1].as_f64();
                entity.z = (*pos->list())[2].as_f64();
            }
            if (const nbt::Tag* block = entry.find("blockPos");
                block != nullptr && block->list() != nullptr && block->list()->size() == 3) {
                entity.block = {static_cast<i32>((*block->list())[0].as_i64()),
                                static_cast<i32>((*block->list())[1].as_i64()),
                                static_cast<i32>((*block->list())[2].as_i64())};
            }
            if (const nbt::Tag* data = entry.find("nbt")) {
                if (const nbt::Tag* id = data->find("id")) {
                    entity.id = std::string{id->as_string()};
                }
            }
            out.entities.push_back(std::move(entity));
        }
    }
    return out;
}

BoundingBox StructureTemplate::bounding_box(BlockPos origin, Mirror mirror, Rotation rotation,
                                            BlockPos pivot) const noexcept {
    const BlockPos a = transform({0, 0, 0}, mirror, rotation, pivot);
    const BlockPos b = transform({size.x - 1, size.y - 1, size.z - 1}, mirror, rotation, pivot);
    return {origin.x + std::min(a.x, b.x), origin.y + std::min(a.y, b.y),
            origin.z + std::min(a.z, b.z), origin.x + std::max(a.x, b.x),
            origin.y + std::max(a.y, b.y), origin.z + std::max(a.z, b.z)};
}

// ── Library ─────────────────────────────────────────────────────────────────

struct TemplateLibrary::Impl {
    std::map<std::string, StructureTemplate, std::less<>> templates;
};

TemplateLibrary::TemplateLibrary() : impl_(std::make_unique<Impl>()) {}

TemplateLibrary::TemplateLibrary(TemplateLibrary&&) noexcept            = default;
TemplateLibrary& TemplateLibrary::operator=(TemplateLibrary&&) noexcept = default;
TemplateLibrary::~TemplateLibrary()                                     = default;

std::expected<TemplateLibrary, TemplateError> TemplateLibrary::open(
    const std::filesystem::path& server_jar, const registry::BlockRegistry& blocks,
    std::span<const std::string_view> families, std::string* detail) {
    auto outer = io::ZipArchive::open(server_jar);
    if (!outer) {
        set_detail(detail, server_jar.string() + ": " + std::string{io::to_string(outer.error())});
        return std::unexpected(TemplateError::JarUnreadable);
    }

    // The bundler jar carries the real server as a jar inside it. Accept an
    // already unwrapped jar too: the templates are then directly visible.
    std::optional<io::ZipArchive> inner;
    if (outer->list(kStructuresPrefix).empty()) {
        std::string nested;
        for (const io::ZipEntry& entry : outer->entries()) {
            if (entry.name.starts_with("META-INF/versions/") && entry.name.ends_with(".jar")) {
                nested = entry.name;
                break;
            }
        }
        if (nested.empty()) {
            set_detail(detail, server_jar.string() + " holds neither templates nor a server jar");
            return std::unexpected(TemplateError::JarUnreadable);
        }
        auto bytes = outer->read(nested);
        if (!bytes) {
            set_detail(detail, nested + ": " + std::string{io::to_string(bytes.error())});
            return std::unexpected(TemplateError::JarUnreadable);
        }
        auto opened = io::ZipArchive::open(std::move(*bytes));
        if (!opened) {
            set_detail(detail, nested + ": " + std::string{io::to_string(opened.error())});
            return std::unexpected(TemplateError::JarUnreadable);
        }
        inner = std::move(*opened);
    }
    const io::ZipArchive& archive = inner ? *inner : *outer;

    TemplateLibrary library;
    for (const std::string_view path : archive.list(kStructuresPrefix)) {
        if (!path.ends_with(".nbt")) {
            continue;
        }
        const std::string_view relative =
            path.substr(kStructuresPrefix.size(), path.size() - kStructuresPrefix.size() - 4);
        const bool wanted = families.empty() ||
                            std::any_of(families.begin(), families.end(), [&](std::string_view f) {
                                return relative.starts_with(f);
                            });
        if (!wanted) {
            continue;
        }
        auto bytes = archive.read(path, kTemplateLimit);
        if (!bytes) {
            set_detail(detail,
                       std::string{path} + ": " + std::string{io::to_string(bytes.error())});
            return std::unexpected(TemplateError::JarUnreadable);
        }
        std::string why;
        auto        parsed = StructureTemplate::parse(*bytes, blocks, &why);
        if (!parsed) {
            set_detail(detail, std::string{relative} + ": " + why);
            return std::unexpected(parsed.error());
        }
        parsed->name = "minecraft:" + std::string{relative};
        library.impl_->templates.emplace(parsed->name, std::move(*parsed));
    }
    OV_LOG_INFO("structures: {} templates read from {}", library.impl_->templates.size(),
                server_jar.string());
    return library;
}

const StructureTemplate* TemplateLibrary::find(std::string_view name) const noexcept {
    const auto it = name.find(':') == std::string_view::npos
                        ? impl_->templates.find("minecraft:" + std::string{name})
                        : impl_->templates.find(name);
    return it == impl_->templates.end() ? nullptr : &it->second;
}

usize TemplateLibrary::size() const noexcept {
    return impl_->templates.size();
}

// ── Processors ──────────────────────────────────────────────────────────────

StructureProcessor::~StructureProcessor() = default;

namespace {

[[nodiscard]] math::LegacyRandomSource random_at(BlockPos pos) noexcept {
    return math::LegacyRandomSource{position_seed(pos.x, pos.y, pos.z)};
}

/// `block_rot`: keep a block with probability `integrity`, drawn from the
/// block's world position. Measured on the warm ocean ruins: every block this
/// keeps, the game kept (177 of 177); see docs/provenance/structures.md § 12.
class BlockRotProcessor final : public StructureProcessor {
public:
    BlockRotProcessor(f32 integrity, const BlockTags* tags, std::string rottable)
        : integrity_(integrity), tags_(tags), rottable_(std::move(rottable)) {}

    [[nodiscard]] std::optional<ProcessorBlock> process(const ProcessorContext& context,
                                                        const ProcessorBlock& /*original*/,
                                                        ProcessorBlock current) const override {
        if (!rottable_.empty() && tags_ != nullptr &&
            !tags_->contains(rottable_, context.blocks->block_of(current.state))) {
            return current;
        }
        // ── worldgen-3 ── The placement's random when it has one, in block order.
        if (context.shared_random != nullptr) {
            if (context.shared_random->next_float() <= integrity_) {
                return current;
            }
            return std::nullopt;
        }
        auto random = random_at(current.pos);
        if (random.next_float() <= integrity_) {
            return current;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::string_view type() const noexcept override { return "block_rot"; }

private:
    f32              integrity_;
    const BlockTags* tags_;
    std::string      rottable_;
};

class BlockIgnoreProcessor final : public StructureProcessor {
public:
    explicit BlockIgnoreProcessor(std::vector<registry::BlockId> ignored)
        : ignored_(std::move(ignored)) {}

    [[nodiscard]] std::optional<ProcessorBlock> process(const ProcessorContext& context,
                                                        const ProcessorBlock& /*original*/,
                                                        ProcessorBlock current) const override {
        const registry::BlockId block = context.blocks->block_of(current.state);
        if (std::find(ignored_.begin(), ignored_.end(), block) != ignored_.end()) {
            return std::nullopt;
        }
        return current;
    }

    [[nodiscard]] std::string_view type() const noexcept override { return "block_ignore"; }

private:
    std::vector<registry::BlockId> ignored_;
};

/// `protected_blocks`: never overwrite a block of the tag that is already in
/// the world — the fossils' "do not replace bedrock or a spawner".
class ProtectedBlocksProcessor final : public StructureProcessor {
public:
    ProtectedBlocksProcessor(const BlockTags& tags, std::string tag)
        : tags_(&tags), tag_(std::move(tag)) {}

    [[nodiscard]] std::optional<ProcessorBlock> process(const ProcessorContext& context,
                                                        const ProcessorBlock& /*original*/,
                                                        ProcessorBlock current) const override {
        const auto existing = context.level->block_at(current.pos.x, current.pos.y, current.pos.z);
        if (tags_->contains(tag_, context.blocks->block_of(existing))) {
            return std::nullopt;
        }
        return current;
    }

    [[nodiscard]] std::string_view type() const noexcept override { return "protected_blocks"; }

private:
    const BlockTags* tags_;
    std::string      tag_;
};

class GravityProcessor final : public StructureProcessor {
public:
    GravityProcessor(world::HeightmapType heightmap, i32 offset)
        : heightmap_(heightmap), offset_(offset) {}

    [[nodiscard]] std::optional<ProcessorBlock> process(const ProcessorContext& context,
                                                        const ProcessorBlock&   original,
                                                        ProcessorBlock current) const override {
        const i32 ground =
            context.level->height(heightmap_, current.pos.x, current.pos.z) + offset_;
        current.pos.y = ground + original.pos.y;
        return current;
    }

    [[nodiscard]] std::string_view type() const noexcept override { return "gravity"; }

private:
    world::HeightmapType heightmap_;
    i32                  offset_;
};

class JigsawReplacementProcessor final : public StructureProcessor {
public:
    [[nodiscard]] std::optional<ProcessorBlock> process(const ProcessorContext& context,
                                                        const ProcessorBlock& /*original*/,
                                                        ProcessorBlock current) const override {
        const auto block = context.blocks->block_of(current.state);
        if (context.blocks->block_name(block) != "minecraft:jigsaw") {
            return current;
        }
        registry::BlockStateId replacement = registry::kAirState;
        if (current.nbt) {
            if (const nbt::Tag* final_state = current.nbt->find("final_state")) {
                if (const auto parsed =
                        state_from_string(final_state->as_string(), *context.blocks)) {
                    replacement = *parsed;
                }
            }
        }
        current.state = replacement;
        current.nbt.reset();
        return current;
    }

    [[nodiscard]] std::string_view type() const noexcept override { return "jigsaw_replacement"; }
};

class NopProcessor final : public StructureProcessor {
public:
    [[nodiscard]] std::optional<ProcessorBlock> process(const ProcessorContext& /*context*/,
                                                        const ProcessorBlock& /*original*/,
                                                        ProcessorBlock current) const override {
        return current;
    }

    [[nodiscard]] std::string_view type() const noexcept override { return "nop"; }
};

/// `block_age`. Every draw comes from one `java.util.Random` keyed on the
/// block's world position, in the order below; the order was fixed against the
/// ruined portals of the reference worlds (docs/provenance/structures.md § 12).
class BlockAgeProcessor final : public StructureProcessor {
public:
    BlockAgeProcessor(const registry::BlockRegistry& blocks, const BlockTags& tags, f32 mossiness)
        : tags_(&tags), mossiness_(mossiness) {
        const auto state = [&](std::string_view name) {
            const auto block = blocks.find_block(name);
            return block ? blocks.default_state(*block) : registry::kAirState;
        };
        const auto block = [&](std::string_view name) {
            return blocks.find_block(name).value_or(registry::BlockId{});
        };
        stone_bricks_    = block("minecraft:stone_bricks");
        stone_           = block("minecraft:stone");
        chiseled_        = block("minecraft:chiseled_stone_bricks");
        obsidian_        = block("minecraft:obsidian");
        cracked_         = state("minecraft:cracked_stone_bricks");
        mossy_bricks_    = state("minecraft:mossy_stone_bricks");
        brick_stairs_    = state("minecraft:stone_brick_stairs");
        mossy_stairs_    = state("minecraft:mossy_stone_brick_stairs");
        mossy_slab_      = state("minecraft:mossy_stone_brick_slab");
        mossy_wall_      = state("minecraft:mossy_stone_brick_wall");
        crying_obsidian_ = state("minecraft:crying_obsidian");
    }

    [[nodiscard]] std::optional<ProcessorBlock> process(const ProcessorContext& context,
                                                        const ProcessorBlock& /*original*/,
                                                        ProcessorBlock current) const override {
        const registry::BlockRegistry& blocks = *context.blocks;
        const registry::BlockId        block  = blocks.block_of(current.state);
        auto                           random = random_at(current.pos);

        const auto random_stairs = [&](registry::BlockStateId stairs) {
            // Both lists are built before the choice, so both stairs draw.
            const i32 facing = random.next_int(4);
            const i32 half   = random.next_int(2);
            auto      state =
                with_value(blocks, stairs, "facing", kHorizontal[static_cast<usize>(facing)]);
            return with_value(blocks, state, "half", half == 0 ? "top" : "bottom");
        };
        const auto choose = [&](registry::BlockStateId plain_a, registry::BlockStateId plain_b,
                                registry::BlockStateId mossy_a, registry::BlockStateId mossy_b) {
            const bool mossy = random.next_float() < mossiness_;
            const i32  pick  = random.next_int(2);
            return mossy ? (pick == 0 ? mossy_a : mossy_b) : (pick == 0 ? plain_a : plain_b);
        };

        if (block == stone_bricks_ || block == stone_ || block == chiseled_) {
            if (random.next_float() >= 0.5F) {
                return current;
            }
            const auto plain_stairs = random_stairs(brick_stairs_);
            const auto mossy_stairs = random_stairs(mossy_stairs_);
            current.state           = choose(cracked_, plain_stairs, mossy_bricks_, mossy_stairs);
            return current;
        }
        if (tags_->contains("minecraft:stairs", block)) {
            if (random.next_float() >= 0.5F) {
                return current;
            }
            if (random.next_float() < mossiness_) {
                auto state = mossy_stairs_;
                state =
                    with_value(blocks, state, "facing", property(blocks, current.state, "facing"));
                state = with_value(blocks, state, "half", property(blocks, current.state, "half"));
                current.state = state;
            }
            return current;
        }
        if (tags_->contains("minecraft:slabs", block)) {
            if (random.next_float() < mossiness_) {
                current.state = mossy_slab_;
            }
            return current;
        }
        if (tags_->contains("minecraft:walls", block)) {
            if (random.next_float() < mossiness_) {
                current.state = mossy_wall_;
            }
            return current;
        }
        if (block == obsidian_) {
            if (random.next_float() < 0.15F) {
                current.state = crying_obsidian_;
            }
            return current;
        }
        return current;
    }

    [[nodiscard]] std::string_view type() const noexcept override { return "block_age"; }

private:
    [[nodiscard]] static std::string_view property(const registry::BlockRegistry& blocks,
                                                   registry::BlockStateId         state,
                                                   std::string_view               name) {
        const auto view = blocks.find_property(blocks.block_of(state), name);
        return view ? blocks.property_value(state, *view) : std::string_view{};
    }

    const BlockTags*       tags_;
    f32                    mossiness_;
    registry::BlockId      stone_bricks_{};
    registry::BlockId      stone_{};
    registry::BlockId      chiseled_{};
    registry::BlockId      obsidian_{};
    registry::BlockStateId cracked_{};
    registry::BlockStateId mossy_bricks_{};
    registry::BlockStateId brick_stairs_{};
    registry::BlockStateId mossy_stairs_{};
    registry::BlockStateId mossy_slab_{};
    registry::BlockStateId mossy_wall_{};
    registry::BlockStateId crying_obsidian_{};
};

// ── Rule tests ──────────────────────────────────────────────────────────────

struct RuleTest {
    enum class Kind : u8 {
        AlwaysTrue,
        BlockMatch,
        BlockStateMatch,
        TagMatch,
        RandomBlockMatch,
        RandomBlockStateMatch
    };
    Kind                   kind{Kind::AlwaysTrue};
    registry::BlockId      block{};
    registry::BlockStateId state{};
    std::string            tag;
    f32                    probability{1.0F};

    [[nodiscard]] bool test(registry::BlockStateId candidate, const registry::BlockRegistry& blocks,
                            const BlockTags* tags, math::LegacyRandomSource& random) const {
        switch (kind) {
            case Kind::AlwaysTrue: return true;
            case Kind::BlockMatch: return blocks.block_of(candidate) == block;
            case Kind::BlockStateMatch: return candidate == state;
            case Kind::TagMatch:
                return tags != nullptr && tags->contains(tag, blocks.block_of(candidate));
            case Kind::RandomBlockMatch:
                // `&&` short-circuits: no draw when the block does not match.
                return blocks.block_of(candidate) == block && random.next_float() < probability;
            case Kind::RandomBlockStateMatch:
                return candidate == state && random.next_float() < probability;
        }
        return false;
    }
};

struct ProcessorRule {
    RuleTest               input;
    RuleTest               location;
    registry::BlockStateId output{};
    /// `append_loot`'s table, empty for `passthrough`.
    std::string append_loot;
};

class RuleProcessor final : public StructureProcessor {
public:
    RuleProcessor(std::vector<ProcessorRule> rules, const BlockTags* tags)
        : rules_(std::move(rules)), tags_(tags) {}

    [[nodiscard]] std::optional<ProcessorBlock> process(const ProcessorContext& context,
                                                        const ProcessorBlock& /*original*/,
                                                        ProcessorBlock current) const override {
        auto       random = random_at(current.pos);
        const auto world  = context.level->block_at(current.pos.x, current.pos.y, current.pos.z);
        for (const ProcessorRule& rule : rules_) {
            const bool input = rule.input.test(current.state, *context.blocks, tags_, random);
            if (!input) {
                continue;
            }
            const bool location = rule.location.test(world, *context.blocks, tags_, random);
            if (!location) {
                continue;
            }
            current.state = rule.output;
            if (!rule.append_loot.empty()) {
                nbt::Tag data = current.nbt ? *current.nbt : nbt::Tag::make_compound();
                data.put("LootTable", nbt::Tag{rule.append_loot});
                data.put("LootTableSeed", nbt::Tag{random.next_long()});
                current.nbt = std::move(data);
            }
            return current;
        }
        return current;
    }

    [[nodiscard]] std::string_view type() const noexcept override { return "rule"; }

private:
    std::vector<ProcessorRule> rules_;
    const BlockTags*           tags_;
};

using Json = simdjson::dom::element;

[[nodiscard]] std::string strip_hash(std::string_view tag) {
    return std::string{tag.starts_with('#') ? tag.substr(1) : tag};
}

[[nodiscard]] std::optional<registry::BlockStateId> json_state(
    Json node, const registry::BlockRegistry& blocks, std::string* detail) {
    std::string_view name;
    if (node["Name"].get(name) != simdjson::SUCCESS) {
        set_detail(detail, "state without Name");
        return std::nullopt;
    }
    const auto block = blocks.find_block(name);
    if (!block) {
        set_detail(detail, "unknown block " + std::string{name});
        return std::nullopt;
    }
    registry::BlockStateId state = blocks.default_state(*block);
    simdjson::dom::object  properties;
    if (node["Properties"].get(properties) == simdjson::SUCCESS) {
        for (const auto field : properties) {
            std::string_view value;
            if (field.value.get(value) != simdjson::SUCCESS) {
                continue;
            }
            const auto view = blocks.find_property(*block, field.key);
            if (!view) {
                set_detail(detail, "no property " + std::string{field.key});
                return std::nullopt;
            }
            const i32 index = value_index(*view, value);
            if (index < 0) {
                set_detail(detail, "no value " + std::string{value});
                return std::nullopt;
            }
            state = blocks.with_property(state, *view, static_cast<u16>(index));
        }
    }
    return state;
}

[[nodiscard]] std::optional<RuleTest> parse_rule_test(Json                           node,
                                                      const registry::BlockRegistry& blocks,
                                                      const BlockTags* tags, std::string* detail) {
    std::string_view type;
    if (node["predicate_type"].get(type) != simdjson::SUCCESS) {
        set_detail(detail, "rule test without predicate_type");
        return std::nullopt;
    }
    if (type.starts_with("minecraft:")) {
        type.remove_prefix(10);
    }
    RuleTest   test;
    const auto block_of = [&](std::string_view key) -> bool {
        std::string_view name;
        if (node[key].get(name) != simdjson::SUCCESS) {
            return false;
        }
        const auto block = blocks.find_block(name);
        if (!block) {
            set_detail(detail, "unknown block " + std::string{name});
            return false;
        }
        test.block = *block;
        return true;
    };
    double probability = 1.0;
    if (type == "always_true") {
        test.kind = RuleTest::Kind::AlwaysTrue;
    } else if (type == "block_match" || type == "random_block_match") {
        if (!block_of("block")) {
            return std::nullopt;
        }
        test.kind =
            type == "block_match" ? RuleTest::Kind::BlockMatch : RuleTest::Kind::RandomBlockMatch;
        (void)node["probability"].get(probability);
    } else if (type == "blockstate_match" || type == "random_blockstate_match") {
        Json state_node;
        if (node["block_state"].get(state_node) != simdjson::SUCCESS) {
            set_detail(detail, "blockstate test without block_state");
            return std::nullopt;
        }
        const auto state = json_state(state_node, blocks, detail);
        if (!state) {
            return std::nullopt;
        }
        test.state = *state;
        test.kind  = type == "blockstate_match" ? RuleTest::Kind::BlockStateMatch
                                                : RuleTest::Kind::RandomBlockStateMatch;
        (void)node["probability"].get(probability);
    } else if (type == "tag_match") {
        std::string_view tag;
        if (node["tag"].get(tag) != simdjson::SUCCESS) {
            set_detail(detail, "tag_match without tag");
            return std::nullopt;
        }
        test.tag = strip_hash(tag);
        if (tags == nullptr || !tags->known(test.tag)) {
            set_detail(detail, "unknown block tag " + test.tag);
            return std::nullopt;
        }
        test.kind = RuleTest::Kind::TagMatch;
    } else {
        set_detail(detail, "rule test '" + std::string{type} + "' is not implemented");
        return std::nullopt;
    }
    test.probability = static_cast<f32>(probability);
    return test;
}

}  // namespace

ProcessorRef make_block_rot(f32 integrity) {
    return std::make_shared<BlockRotProcessor>(integrity, nullptr, std::string{});
}

ProcessorRef make_block_ignore(const registry::BlockRegistry&    blocks,
                               std::span<const std::string_view> names) {
    std::vector<registry::BlockId> ids;
    for (const std::string_view name : names) {
        if (const auto block = blocks.find_block(name)) {
            ids.push_back(*block);
        } else {
            OV_LOG_ERROR("structures: block_ignore names {}, which the registry does not have",
                         name);
        }
    }
    return std::make_shared<BlockIgnoreProcessor>(std::move(ids));
}

ProcessorRef make_protected_blocks(const BlockTags& tags, std::string tag) {
    return std::make_shared<ProtectedBlocksProcessor>(tags, strip_hash(tag));
}

ProcessorRef make_block_age(const registry::BlockRegistry& blocks, const BlockTags& tags,
                            f32 mossiness) {
    return std::make_shared<BlockAgeProcessor>(blocks, tags, mossiness);
}

std::expected<ProcessorList, TemplateError> ProcessorList::parse_json(
    std::string_view json, const registry::BlockRegistry& blocks, const BlockTags* tags,
    std::string* detail) {
    simdjson::dom::parser   parser;
    simdjson::padded_string text{json};
    Json                    document;
    if (parser.parse(text).get(document) != simdjson::SUCCESS) {
        set_detail(detail, "not JSON");
        return std::unexpected(TemplateError::BadProcessor);
    }
    simdjson::dom::array list;
    if (document["processors"].get(list) != simdjson::SUCCESS) {
        set_detail(detail, "no processors array");
        return std::unexpected(TemplateError::BadProcessor);
    }
    ProcessorList out;
    for (const Json node : list) {
        std::string_view type;
        if (node["processor_type"].get(type) != simdjson::SUCCESS) {
            set_detail(detail, "processor without processor_type");
            return std::unexpected(TemplateError::BadProcessor);
        }
        if (type.starts_with("minecraft:")) {
            type.remove_prefix(10);
        }
        if (type == "block_rot") {
            double      integrity = 1.0;
            std::string rottable;
            (void)node["integrity"].get(integrity);
            std::string_view rottable_tag;
            if (node["rottable_blocks"].get(rottable_tag) == simdjson::SUCCESS) {
                rottable = strip_hash(rottable_tag);
            }
            out.processors.push_back(std::make_shared<BlockRotProcessor>(
                static_cast<f32>(integrity), tags, std::move(rottable)));
        } else if (type == "block_ignore") {
            simdjson::dom::array ignored;
            if (node["blocks"].get(ignored) != simdjson::SUCCESS) {
                set_detail(detail, "block_ignore without blocks");
                return std::unexpected(TemplateError::BadProcessor);
            }
            std::vector<registry::BlockId> ids;
            for (const Json entry : ignored) {
                const auto state = json_state(entry, blocks, detail);
                if (!state) {
                    return std::unexpected(TemplateError::BadProcessor);
                }
                ids.push_back(blocks.block_of(*state));
            }
            out.processors.push_back(std::make_shared<BlockIgnoreProcessor>(std::move(ids)));
        } else if (type == "protected_blocks") {
            std::string_view tag;
            if (node["value"].get(tag) != simdjson::SUCCESS || tags == nullptr ||
                !tags->known(strip_hash(tag))) {
                set_detail(detail, "protected_blocks without a known tag");
                return std::unexpected(TemplateError::BadProcessor);
            }
            out.processors.push_back(
                std::make_shared<ProtectedBlocksProcessor>(*tags, strip_hash(tag)));
        } else if (type == "gravity") {
            std::string_view heightmap = "WORLD_SURFACE_WG";
            i64              offset    = 0;
            (void)node["heightmap"].get(heightmap);
            (void)node["offset"].get(offset);
            world::HeightmapType kind = world::HeightmapType::WorldSurfaceWG;
            if (heightmap == "OCEAN_FLOOR_WG") {
                kind = world::HeightmapType::OceanFloorWG;
            } else if (heightmap == "WORLD_SURFACE") {
                kind = world::HeightmapType::WorldSurface;
            } else if (heightmap == "OCEAN_FLOOR") {
                kind = world::HeightmapType::OceanFloor;
            } else if (heightmap == "MOTION_BLOCKING") {
                kind = world::HeightmapType::MotionBlocking;
            } else if (heightmap == "MOTION_BLOCKING_NO_LEAVES") {
                kind = world::HeightmapType::MotionBlockingNoLeaves;
            } else if (heightmap != "WORLD_SURFACE_WG") {
                set_detail(detail, "gravity heightmap " + std::string{heightmap});
                return std::unexpected(TemplateError::BadProcessor);
            }
            out.processors.push_back(
                std::make_shared<GravityProcessor>(kind, static_cast<i32>(offset)));
        } else if (type == "jigsaw_replacement") {
            out.processors.push_back(std::make_shared<JigsawReplacementProcessor>());
        } else if (type == "nop") {
            out.processors.push_back(std::make_shared<NopProcessor>());
        } else if (type == "block_age") {
            double mossiness = 0.0;
            (void)node["mossiness"].get(mossiness);
            if (tags == nullptr) {
                set_detail(detail, "block_age needs the block tags");
                return std::unexpected(TemplateError::BadProcessor);
            }
            out.processors.push_back(
                std::make_shared<BlockAgeProcessor>(blocks, *tags, static_cast<f32>(mossiness)));
        } else if (type == "rule") {
            simdjson::dom::array rules;
            if (node["rules"].get(rules) != simdjson::SUCCESS) {
                set_detail(detail, "rule processor without rules");
                return std::unexpected(TemplateError::BadProcessor);
            }
            std::vector<ProcessorRule> parsed;
            for (const Json rule_node : rules) {
                ProcessorRule rule;
                Json          input;
                Json          location;
                Json          output;
                if (rule_node["input_predicate"].get(input) != simdjson::SUCCESS ||
                    rule_node["location_predicate"].get(location) != simdjson::SUCCESS ||
                    rule_node["output_state"].get(output) != simdjson::SUCCESS) {
                    set_detail(detail, "rule without input, location or output");
                    return std::unexpected(TemplateError::BadProcessor);
                }
                Json position;
                if (rule_node["position_predicate"].get(position) == simdjson::SUCCESS) {
                    std::string_view position_type;
                    (void)position["predicate_type"].get(position_type);
                    if (position_type != "minecraft:always_true") {
                        set_detail(detail, "position predicate '" + std::string{position_type} +
                                               "' is not implemented");
                        return std::unexpected(TemplateError::UnknownProcessor);
                    }
                }
                Json modifier;
                if (rule_node["block_entity_modifier"].get(modifier) == simdjson::SUCCESS) {
                    std::string_view modifier_type;
                    (void)modifier["type"].get(modifier_type);
                    if (modifier_type == "minecraft:append_loot") {
                        std::string_view table;
                        (void)modifier["loot_table"].get(table);
                        rule.append_loot = std::string{table};
                    } else if (modifier_type != "minecraft:passthrough") {
                        set_detail(detail, "block entity modifier '" + std::string{modifier_type} +
                                               "' is not implemented");
                        return std::unexpected(TemplateError::UnknownProcessor);
                    }
                }
                auto input_test    = parse_rule_test(input, blocks, tags, detail);
                auto location_test = parse_rule_test(location, blocks, tags, detail);
                auto output_state  = json_state(output, blocks, detail);
                if (!input_test || !location_test || !output_state) {
                    return std::unexpected(TemplateError::BadProcessor);
                }
                rule.input    = *input_test;
                rule.location = *location_test;
                rule.output   = *output_state;
                parsed.push_back(std::move(rule));
            }
            out.processors.push_back(std::make_shared<RuleProcessor>(std::move(parsed), tags));
        } else {
            // capped, block_age, blackstone_replace, lava_submerged_block …
            // Refused and named: a silently skipped processor places a
            // structure that looks right and is not.
            set_detail(detail, "processor '" + std::string{type} + "' is not implemented");
            return std::unexpected(TemplateError::UnknownProcessor);
        }
    }
    return out;
}

// ── Placement ───────────────────────────────────────────────────────────────

u32 palette_for(const StructureTemplate& tpl, BlockPos origin) noexcept {
    if (tpl.palettes.size() <= 1) {
        return 0;
    }
    auto random = random_at(origin);
    return static_cast<u32>(random.next_int(static_cast<i32>(tpl.palettes.size())));
}

bool holds_water_source(const registry::BlockRegistry& blocks,
                        registry::BlockStateId         state) noexcept {
    const auto block = blocks.block_of(state);
    if (blocks.block_name(block) == "minecraft:water") {
        const auto level_property = blocks.find_property(block, "level");
        return !level_property || blocks.property_index(state, *level_property) == 0;
    }
    if (const auto logged = blocks.find_property(block, "waterlogged")) {
        return blocks.property_value(state, *logged) == "true";
    }
    return false;
}

registry::BlockStateId with_value(const registry::BlockRegistry& blocks,
                                  registry::BlockStateId state, std::string_view property,
                                  std::string_view value) noexcept {
    const auto view = blocks.find_property(blocks.block_of(state), property);
    if (!view) {
        return state;
    }
    const i32 index = value_index(*view, value);
    return index < 0 ? state : blocks.with_property(state, *view, static_cast<u16>(index));
}

registry::BlockStateId with_waterlogged(const registry::BlockRegistry& blocks,
                                        registry::BlockStateId state, bool value) noexcept {
    return with_value(blocks, state, "waterlogged", value ? "true" : "false");
}

namespace {

struct StairView {
    bool             stairs{false};
    std::string_view facing;
    std::string_view half;
};

[[nodiscard]] StairView view_stairs(const registry::BlockRegistry& blocks,
                                    registry::BlockStateId         state) {
    const auto block = blocks.block_of(state);
    if (!blocks.block_name(block).ends_with("_stairs")) {
        return {};
    }
    const auto facing = blocks.find_property(block, "facing");
    const auto half   = blocks.find_property(block, "half");
    if (!facing || !half) {
        return {};
    }
    return {true, blocks.property_value(state, *facing), blocks.property_value(state, *half)};
}

[[nodiscard]] BlockPos step(BlockPos pos, i32 direction) {
    switch (direction) {
        case 0: return pos.offset(0, 0, -1);
        case 1: return pos.offset(1, 0, 0);
        case 2: return pos.offset(0, 0, 1);
        default: return pos.offset(-1, 0, 0);
    }
}

/// The block entities that are `RandomizableContainerBlockEntity`s in the game:
/// the ones that can carry a loot table.
[[nodiscard]] bool is_container(std::string_view id) {
    return id == "minecraft:chest" || id == "minecraft:trapped_chest" || id == "minecraft:barrel" ||
           id == "minecraft:dispenser" || id == "minecraft:dropper" || id == "minecraft:hopper" ||
           id == "minecraft:shulker_box";
}

[[nodiscard]] bool is_fence(const registry::BlockRegistry& blocks, registry::BlockStateId state) {
    return blocks.block_name(blocks.block_of(state)).ends_with("_fence");
}

/// Does a fence at `pos` join the neighbour on side `side`?
///
/// Another fence of the same family (wood with wood, nether brick with nether
/// brick), a gate lying across that side, or any block whose facing side is
/// sturdy — except the handful the game excludes: leaves, barriers, the
/// pumpkins and the melon, shulker boxes.
[[nodiscard]] bool fence_joins(const registry::BlockRegistry& blocks, registry::BlockStateId self,
                               registry::BlockStateId neighbour, i32 side) {
    const std::string_view own   = blocks.block_name(blocks.block_of(self));
    const std::string_view other = blocks.block_name(blocks.block_of(neighbour));
    if (other.ends_with("_fence")) {
        const bool own_nether   = own == "minecraft:nether_brick_fence";
        const bool other_nether = other == "minecraft:nether_brick_fence";
        return own_nether == other_nether;
    }
    if (other.ends_with("_fence_gate")) {
        const auto facing = blocks.find_property(blocks.block_of(neighbour), "facing");
        if (!facing) {
            return false;
        }
        const i32 gate = horizontal_index(blocks.property_value(neighbour, *facing));
        return gate >= 0 && (gate & 1) != (side & 1);
    }
    if (other.ends_with("_leaves") || other.ends_with("shulker_box") ||
        other == "minecraft:barrier" || other == "minecraft:pumpkin" ||
        other == "minecraft:carved_pumpkin" || other == "minecraft:jack_o_lantern" ||
        other == "minecraft:melon") {
        return false;
    }
    // The neighbour's face that looks back at us.
    constexpr std::array<registry::BlockRegistry::Face, 4> kFacing{
        registry::BlockRegistry::Face::South, registry::BlockRegistry::Face::West,
        registry::BlockRegistry::Face::North, registry::BlockRegistry::Face::East};
    return blocks.face_is_sturdy(neighbour, kFacing[static_cast<usize>(side)]);
}

}  // namespace

registry::BlockStateId fence_connections(const registry::BlockRegistry& blocks,
                                         const FeatureLevel& level, BlockPos pos,
                                         registry::BlockStateId state) noexcept {
    if (!is_fence(blocks, state)) {
        return state;
    }
    registry::BlockStateId out = state;
    for (i32 side = 0; side < 4; ++side) {
        const BlockPos there = step(pos, side);
        const bool     joins =
            fence_joins(blocks, state, level.block_at(there.x, there.y, there.z), side);
        out = with_value(blocks, out, kHorizontal[static_cast<usize>(side)],
                         joins ? "true" : "false");
    }
    return out;
}

registry::BlockStateId stair_shape(const registry::BlockRegistry& blocks, const FeatureLevel& level,
                                   BlockPos pos, registry::BlockStateId state) noexcept {
    const StairView self   = view_stairs(blocks, state);
    const i32       facing = horizontal_index(self.facing);
    if (!self.stairs || facing < 0) {
        return state;
    }
    const i32  counter_clockwise = (facing + 3) & 3;
    const auto at                = [&](BlockPos where) {
        return view_stairs(blocks, level.block_at(where.x, where.y, where.z));
    };
    // A side may bend towards a neighbour only if the stair on the far side
    // is not a twin of this one.
    const auto can_take = [&](i32 side) {
        const StairView other = at(step(pos, side));
        return !other.stairs || other.facing != self.facing || other.half != self.half;
    };

    const StairView behind = at(step(pos, facing));
    if (behind.stairs && behind.half == self.half) {
        const i32 d = horizontal_index(behind.facing);
        if (d >= 0 && (d & 1) != (facing & 1) && can_take((d + 2) & 3)) {
            return with_value(blocks, state, "shape",
                              d == counter_clockwise ? "outer_left" : "outer_right");
        }
    }
    const StairView front = at(step(pos, (facing + 2) & 3));
    if (front.stairs && front.half == self.half) {
        const i32 d = horizontal_index(front.facing);
        if (d >= 0 && (d & 1) != (facing & 1) && can_take(d)) {
            return with_value(blocks, state, "shape",
                              d == counter_clockwise ? "inner_left" : "inner_right");
        }
    }
    return with_value(blocks, state, "shape", "straight");
}

PlaceResult place_template(StructureLevel& level, const StructureTemplate& tpl, BlockPos origin,
                           const PlaceSettings& settings, const registry::BlockRegistry& blocks) {
    PlaceResult result;
    const u32   palette_index = settings.palette.value_or(palette_for(tpl, origin));
    const auto& palette = tpl.palettes[std::min<usize>(palette_index, tpl.palettes.size() - 1)];

    const auto structure_block = blocks.find_block("minecraft:structure_block");

    ProcessorContext context;
    context.level    = &level;
    context.blocks   = &blocks;
    context.origin   = origin;
    context.pivot    = settings.pivot;
    context.rotation = settings.rotation;
    context.mirror   = settings.mirror;
    context.shared_random = settings.processor_random;  // ── worldgen-3 ──

    const auto inside = [&](BlockPos pos) {
        return !settings.clip || settings.clip->contains(pos.x, pos.y, pos.z);
    };

    for (const TemplateBlock& block : tpl.blocks) {
        const BlockPos offset =
            transform(block.pos, settings.mirror, settings.rotation, settings.pivot);
        const BlockPos world{origin.x + offset.x, origin.y + offset.y, origin.z + offset.z};
        const registry::BlockStateId template_state = palette[block.state];
        const nbt::Tag*              data =
            block.nbt >= 0 ? &tpl.block_nbt[static_cast<usize>(block.nbt)] : nullptr;

        // Data markers come from the template itself, whatever the processors
        // then do with the structure block: the shipwrecks drop the block and
        // still act on the marker.
        if (structure_block && data != nullptr &&
            blocks.block_of(template_state) == *structure_block && inside(world)) {
            const nbt::Tag* mode = data->find("mode");
            const nbt::Tag* meta = data->find("metadata");
            if (mode != nullptr && mode->as_string() == "DATA" && meta != nullptr) {
                result.markers.push_back({world, std::string{meta->as_string()}});
            }
        }

        ProcessorBlock original{block.pos, template_state, std::nullopt};
        ProcessorBlock current{world, template_state, std::nullopt};
        if (data != nullptr) {
            original.nbt = *data;
            current.nbt  = *data;
        }
        bool dropped = false;
        for (const ProcessorRef& processor : settings.processors) {
            auto next = processor->process(context, original, std::move(current));
            if (!next) {
                dropped = true;
                break;
            }
            current = std::move(*next);
        }
        if (dropped) {
            ++result.dropped_by_processors;
            continue;
        }
        if (!inside(current.pos) || level.outside_build_height(current.pos.y)) {
            continue;
        }

        registry::BlockStateId state =
            transform_state(blocks, current.state, settings.mirror, settings.rotation);

        // A water source already there stays, inside the block if the block
        // can hold it.
        if (settings.keep_liquids &&
            holds_water_source(blocks,
                               level.block_at(current.pos.x, current.pos.y, current.pos.z))) {
            state = with_waterlogged(blocks, state, true);
        }

        level.set_block(current.pos.x, current.pos.y, current.pos.z, state);
        ++result.written;
        if (needs_shape_update(blocks, state)) {
            result.shaped.push_back(current.pos);
        }

        if (current.nbt) {
            nbt::Tag entity = std::move(*current.nbt);
            // A container draws its loot seed from the piece's random as it is
            // placed — every container, with or without a table. Measured: the
            // ocean ruins' marker chests only match the game's seeds once the
            // draws of the chests *in* the other templates are counted.
            const nbt::Tag* id = entity.find("id");
            if (id != nullptr && is_container(id->as_string()) && settings.random != nullptr) {
                const i64 seed = settings.random->next_long();
                if (entity.find("LootTable") != nullptr) {
                    entity.put("LootTableSeed", nbt::Tag{seed});
                }
            }
            level.set_block_entity(current.pos.x, current.pos.y, current.pos.z, std::move(entity));
        }
    }

    // Once everything is down, the shapes follow the neighbours. After the
    // loop and not inside it: a stair's shape depends on stairs placed after
    // it. A neighbour in a chunk not placed yet is not there either — the
    // caller runs `update_shapes` again once it is.
    update_shapes(level, blocks, result.shaped);
    return result;
}

bool needs_shape_update(const registry::BlockRegistry& blocks,
                        registry::BlockStateId         state) noexcept {
    const std::string_view name = blocks.block_name(blocks.block_of(state));
    return name.ends_with("_stairs") || name.ends_with("_fence") || name.ends_with("_door");
}

void update_shapes(FeatureLevel& level, const registry::BlockRegistry& blocks,
                   std::span<const BlockPos> positions) {
    // Two passes, and the order is not free: whether a fence joins a stair
    // depends on the stair's *shape* (a side is sturdy or not), while a stair's
    // shape never depends on a fence. Stairs and doors first, fences after —
    // otherwise the answer depends on which chunk was placed last.
    for (const bool fences : {false, true}) {
        for (const BlockPos pos : positions) {
            const auto             now  = level.block_at(pos.x, pos.y, pos.z);
            const std::string_view name = blocks.block_name(blocks.block_of(now));
            registry::BlockStateId next = now;
            if (name.ends_with("_fence") != fences) {
                continue;
            }
            if (name.ends_with("_stairs")) {
                next = stair_shape(blocks, level, pos, now);
            } else if (name.ends_with("_fence")) {
                next = fence_connections(blocks, level, pos, now);
            } else if (name.ends_with("_door")) {
                // A door's lower half copies its upper half.
                const auto half = blocks.find_property(blocks.block_of(now), "half");
                if (half && blocks.property_value(now, *half) == "lower") {
                    const auto above = level.block_at(pos.x, pos.y + 1, pos.z);
                    if (blocks.block_of(above) == blocks.block_of(now) &&
                        blocks.property_value(above, *half) == "upper") {
                        for (const std::string_view key : {"facing", "hinge", "open", "powered"}) {
                            const auto view = blocks.find_property(blocks.block_of(now), key);
                            if (view) {
                                next = with_value(blocks, next, key,
                                                  blocks.property_value(above, *view));
                            }
                        }
                    }
                }
            }
            if (next != now) {
                level.set_block(pos.x, pos.y, pos.z, next);
            }
        }
    }
}

}  // namespace ov::worldgen
