#define OV_LOG_CATEGORY "worldgen"

#include "ov/worldgen/tree_feature.hpp"

#include "feature_json.hpp"

#include "ov/base/log.hpp"
#include "ov/worldgen/carver.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <limits>
#include <list>
#include <unordered_map>

namespace ov::worldgen {

namespace {

// ── Small pieces of Java that the tree code leans on ────────────────────────

/// `Direction.Plane.HORIZONTAL`, in its own declaration order.
///
/// The order is the seed: `getRandomDirection` is one `nextInt(4)` used as an
/// index into this array, so listing them north/south/east/west instead would
/// turn every forking oak a quarter turn.
constexpr std::array<std::array<i32, 2>, 4> kHorizontal{{
    {0, -1},  // north
    {1, 0},   // east
    {0, 1},   // south
    {-1, 0},  // west
}};

[[nodiscard]] std::array<i32, 2> random_horizontal(FeatureRandom& random) {
    return kHorizontal[static_cast<usize>(random.next_int(4))];
}

/// `Mth.floor` on a double: floor, then narrow. Not a cast, which truncates
/// towards zero and puts every negative coordinate one block too high.
[[nodiscard]] i32 mth_floor(f64 value) noexcept {
    return static_cast<i32>(std::floor(value));
}

[[nodiscard]] i32 mth_floor(f32 value) noexcept {
    return static_cast<i32>(std::floor(value));
}

/// `Util.shuffle`: Fisher-Yates from the top, one `nextInt(j)` per step.
void java_shuffle(std::vector<BlockPos>& list, FeatureRandom& random) {
    for (usize j = list.size(); j > 1; --j) {
        const auto k = static_cast<usize>(random.next_int(static_cast<i32>(j)));
        std::swap(list[j - 1], list[k]);
    }
}

/// Set one named property on a state, leaving it alone when the block has no
/// such property — which is what `BlockState.trySetValue` does.
[[nodiscard]] registry::BlockStateId try_set_property(const registry::BlockRegistry& blocks,
                                                      registry::BlockStateId         state,
                                                      std::string_view               name,
                                                      std::string_view               value) {
    const auto block    = blocks.block_of(state);
    const auto property = blocks.find_property(block, name);
    if (!property) {
        return state;
    }
    for (usize index = 0; index < property->values.size(); ++index) {
        if (property->values[index] == value) {
            return blocks.with_property(state, *property, static_cast<u16>(index));
        }
    }
    return state;
}

[[nodiscard]] bool has_property(const registry::BlockRegistry& blocks,
                                registry::BlockStateId state, std::string_view name) {
    return blocks.find_property(blocks.block_of(state), name).has_value();
}

/// The axis a limb should carry, from the vector it runs along.
[[nodiscard]] std::string_view log_axis(BlockPos from, BlockPos to) {
    const i32 dx    = std::abs(to.x - from.x);
    const i32 dz    = std::abs(to.z - from.z);
    const i32 wider = std::max(dx, dz);
    if (wider <= 0) {
        return "y";
    }
    return dx == wider ? "x" : "z";
}

}  // namespace

// ── The tags ────────────────────────────────────────────────────────────────

bool TreeTags::holds(const std::vector<u16>& set, registry::BlockId block) noexcept {
    return std::ranges::binary_search(set, block.value());
}

std::expected<TreeTagsRef, FeatureError> load_tree_tags(const registry::BlockRegistry& blocks,
                                                        const BlockTags&               tags) {
    auto result = std::make_shared<TreeTags>();

    const auto collect = [&](std::string_view tag,
                             std::vector<u16>& into) -> std::expected<void, FeatureError> {
        if (!tags.known(tag)) {
            OV_LOG_ERROR("worldgen: a tree needs tag {}, which was not exported", tag);
            return std::unexpected(FeatureError::Missing);
        }
        for (usize index = 0; index < blocks.block_count(); ++index) {
            const registry::BlockId block{static_cast<u16>(index)};
            if (tags.contains(tag, block)) {
                into.push_back(block.value());
            }
        }
        std::ranges::sort(into);
        return {};
    };

    if (auto ok = collect("minecraft:logs", result->logs); !ok) return std::unexpected(ok.error());
    if (auto ok = collect("minecraft:leaves", result->leaves); !ok)
        return std::unexpected(ok.error());
    if (auto ok = collect("minecraft:dirt", result->dirt); !ok) return std::unexpected(ok.error());
    if (auto ok = collect("minecraft:replaceable_by_trees", result->replaceable_by_trees); !ok)
        return std::unexpected(ok.error());

    const auto vine  = blocks.find_block("minecraft:vine");
    const auto water = blocks.find_block("minecraft:water");
    const auto air   = blocks.find_block("minecraft:air");
    if (!vine || !water || !air) {
        return std::unexpected(FeatureError::Missing);
    }
    result->vine  = *vine;
    result->water = *water;
    result->air   = *air;
    return std::static_pointer_cast<const TreeTags>(result);
}

// ── The world as a tree sees it ─────────────────────────────────────────────

registry::BlockId TreeWorld::block_at(BlockPos at) const {
    return blocks->block_of(level->block_at(at.x, at.y, at.z));
}

bool TreeWorld::valid_tree_pos(BlockPos at) const {
    const auto block = block_at(at);
    return blocks->is_air(block) || TreeTags::holds(tags->replaceable_by_trees, block);
}

bool TreeWorld::is_free(BlockPos at) const {
    if (valid_tree_pos(at)) {
        return true;
    }
    return TreeTags::holds(tags->logs, block_at(at));
}

bool TreeWorld::is_air_or_leaves(BlockPos at) const {
    const auto block = block_at(at);
    return blocks->is_air(block) || TreeTags::holds(tags->leaves, block);
}

bool TreeWorld::is_dirt(BlockPos at) const {
    return TreeTags::holds(tags->dirt, block_at(at));
}

bool TreeWorld::is_air(BlockPos at) const {
    return blocks->is_air(block_at(at));
}

bool TreeWorld::is_vine(BlockPos at) const {
    return block_at(at) == tags->vine;
}

bool TreeWorld::is_water_source(BlockPos at) const {
    const auto state = level->block_at(at.x, at.y, at.z);
    const auto block = blocks->block_of(state);
    if (block == tags->water) {
        // A source is level 0; a flowing block is not, and `isSourceOfType`
        // says no to it.
        const auto property = blocks->find_property(block, "level");
        return !property || blocks->property_value(state, *property) == "0";
    }
    const auto waterlogged = blocks->find_property(block, "waterlogged");
    return waterlogged && blocks->property_value(state, *waterlogged) == "true";
}

// ── java.util.HashSet iteration order ───────────────────────────────────────

i32 java_block_pos_hash(BlockPos at) noexcept {
    const auto y = static_cast<u32>(at.y);
    const auto z = static_cast<u32>(at.z);
    const auto x = static_cast<u32>(at.x);
    return static_cast<i32>(((y + z * 31U) * 31U) + x);
}

namespace {

/// A bucket of eight in a table of 64 or more: counted, and said at most once a
/// minute with the running total. Every generation worker reaches this, and at
/// one line each it drowned the console and cost time. A diagnostic's own
/// counter, not world state: nothing reads it back.
void note_untreeified_bucket(usize capacity) noexcept {
    static std::atomic<u64> total{0};
    static std::atomic<i64> last_said{std::numeric_limits<i64>::min()};
    const u64  seen = total.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto now  = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    i64 last = last_said.load(std::memory_order_relaxed);
    if (last != std::numeric_limits<i64>::min() && now - last < 60'000) {
        return;
    }
    if (!last_said.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
        return;  // another worker is saying it
    }
    OV_LOG_WARN("worldgen: {} tree position buckets have reached eight in a table of 64 or more "
                "(latest table {}); java would treeify them and this does not",
                seen, capacity);
}

/// `HashMap.hash`: spread the high bits down, because the table index only
/// uses the low ones.
[[nodiscard]] u32 java_spread(i32 hash) noexcept {
    const auto h = static_cast<u32>(hash);
    return h ^ (h >> 16);
}

/// The diagnostic count behind `java_hash_treeified_bins`.
[[nodiscard]] std::atomic<u64>& treeified_bins() noexcept {
    static std::atomic<u64> count{0};
    return count;
}

/// One entry of the model HashMap: a node of its bin's linked list (`next`,
/// `prev` — the iteration order) and, once the bin is treeified, of the bin's
/// red-black tree (`parent`, `left`, `right`, `red`).
struct JavaNode {
    BlockPos pos;
    i32      hash{0};  ///< the spread hash, compared as Java's signed int
    i32      next{-1};
    i32      prev{-1};
    i32      parent{-1};
    i32      left{-1};
    i32      right{-1};
    bool     red{false};
};

/// ── worldgen-3 ── `java.util.HashSet<BlockPos>` as far as its iteration
/// order goes, treeified bins included — the documented behaviour of the JDK's
/// HashMap:
///   * a list bin that an insertion brings to nine entries (eight already
///     there) is treeified, or, while the table has fewer than 64 bins, the
///     table is resized instead;
///   * treeification builds a red-black tree in list order, ordered by hash,
///     and moves the tree's root to the front of the bin's list;
///   * an insertion into a tree bin links the new node right after its tree
///     parent in the list, rebalances, and moves the (new) root to the front;
///   * a resize splits a tree bin into its low and high halves in list order:
///     a half of six or fewer goes back to a plain list, a half that kept every
///     node keeps its tree, and otherwise each half is treeified again.
/// Two positions of equal full hash would be ordered by Java's identity hash,
/// which nothing outside that JVM can know; they are counted and, among one
/// tree's few hundred positions, do not occur.
class JavaHashSet {
public:
    void add(BlockPos pos) {
        if (table_.empty()) {
            resize(16);
        }
        const i32   h     = static_cast<i32>(java_spread(java_block_pos_hash(pos)));
        const usize index = static_cast<u32>(h) & (table_.size() - 1);
        Bin&        bin   = table_[index];
        if (bin.head < 0) {
            bin.head = make_node(pos, h);
        } else {
            usize count = 0;
            i32   last  = -1;
            for (i32 at = bin.head; at >= 0; at = node(at).next) {
                if (node(at).pos == pos) {
                    return;  // a set: an equal position is not stored twice
                }
                ++count;
                last = at;
            }
            if (bin.tree) {
                put_tree(index, pos, h);
            } else {
                const i32 fresh    = make_node(pos, h);
                node(last).next  = fresh;
                node(fresh).prev = last;
                if (count >= 8) {  // binCount >= TREEIFY_THRESHOLD - 1
                    if (table_.size() < 64) {
                        resize(table_.size() * 2);
                    } else {
                        treeify(index);
                        treeified_bins().fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        }
        if (++size_ > table_.size() * 3 / 4) {
            resize(table_.size() * 2);
        }
    }

    [[nodiscard]] std::vector<BlockPos> iteration() const {
        std::vector<BlockPos> out;
        out.reserve(size_);
        for (const Bin& bin : table_) {
            for (i32 at = bin.head; at >= 0; at = nodes_[static_cast<usize>(at)].next) {
                out.push_back(nodes_[static_cast<usize>(at)].pos);
            }
        }
        return out;
    }

private:
    struct Bin {
        i32  head{-1};
        bool tree{false};
    };

    JavaNode& node(i32 at) { return nodes_[static_cast<usize>(at)]; }

    i32 make_node(BlockPos pos, i32 h) {
        JavaNode fresh;
        fresh.pos  = pos;
        fresh.hash = h;
        nodes_.push_back(fresh);
        return static_cast<i32>(nodes_.size() - 1);
    }

    /// Left when the parent's hash is greater, right when smaller; an equal
    /// hash goes left, standing in for Java's identity tie-break.
    [[nodiscard]] i32 direction(i32 h, i32 p) {
        return node(p).hash < h ? 1 : -1;
    }

    i32 rotate_left(i32 root, i32 p) {
        const i32 r = node(p).right;
        if (r < 0) {
            return root;
        }
        const i32 rl  = node(r).left;
        node(p).right = rl;
        if (rl >= 0) {
            node(rl).parent = p;
        }
        const i32 pp   = node(p).parent;
        node(r).parent = pp;
        if (pp < 0) {
            root        = r;
            node(r).red = false;
        } else if (node(pp).left == p) {
            node(pp).left = r;
        } else {
            node(pp).right = r;
        }
        node(r).left   = p;
        node(p).parent = r;
        return root;
    }

    i32 rotate_right(i32 root, i32 p) {
        const i32 l = node(p).left;
        if (l < 0) {
            return root;
        }
        const i32 lr = node(l).right;
        node(p).left = lr;
        if (lr >= 0) {
            node(lr).parent = p;
        }
        const i32 pp   = node(p).parent;
        node(l).parent = pp;
        if (pp < 0) {
            root        = l;
            node(l).red = false;
        } else if (node(pp).right == p) {
            node(pp).right = l;
        } else {
            node(pp).left = l;
        }
        node(l).right  = p;
        node(p).parent = l;
        return root;
    }

    /// The red-black fix-up after an insertion; returns the root.
    i32 balance_insertion(i32 root, i32 x) {
        node(x).red = true;
        for (;;) {
            i32 xp = node(x).parent;
            if (xp < 0) {
                node(x).red = false;
                return x;
            }
            i32 xpp = node(xp).parent;
            if (!node(xp).red || xpp < 0) {
                return root;
            }
            const i32 xppl = node(xpp).left;
            if (xp == xppl) {
                const i32 xppr = node(xpp).right;
                if (xppr >= 0 && node(xppr).red) {
                    node(xppr).red = false;
                    node(xp).red   = false;
                    node(xpp).red  = true;
                    x              = xpp;
                    continue;
                }
                if (x == node(xp).right) {
                    x    = xp;
                    root = rotate_left(root, x);
                    xp   = node(x).parent;
                    xpp  = xp < 0 ? -1 : node(xp).parent;
                }
                if (xp >= 0) {
                    node(xp).red = false;
                    if (xpp >= 0) {
                        node(xpp).red = true;
                        root          = rotate_right(root, xpp);
                    }
                }
            } else {
                if (xppl >= 0 && node(xppl).red) {
                    node(xppl).red = false;
                    node(xp).red   = false;
                    node(xpp).red  = true;
                    x              = xpp;
                    continue;
                }
                if (x == node(xp).left) {
                    x    = xp;
                    root = rotate_right(root, x);
                    xp   = node(x).parent;
                    xpp  = xp < 0 ? -1 : node(xp).parent;
                }
                if (xp >= 0) {
                    node(xp).red = false;
                    if (xpp >= 0) {
                        node(xpp).red = true;
                        root          = rotate_left(root, xpp);
                    }
                }
            }
        }
    }

    void move_root_to_front(std::vector<Bin>& table, usize index, i32 root) {
        Bin&      bin   = table[index];
        const i32 first = bin.head;
        if (root < 0 || root == first) {
            return;
        }
        bin.head     = root;
        const i32 rp = node(root).prev;
        const i32 rn = node(root).next;
        if (rn >= 0) {
            node(rn).prev = rp;
        }
        if (rp >= 0) {
            node(rp).next = rn;
        }
        if (first >= 0) {
            node(first).prev = root;
        }
        node(root).next = first;
        node(root).prev = -1;
    }

    /// Build a bin's tree from its list, in list order.
    void treeify_in(std::vector<Bin>& table, usize index) {
        Bin& bin  = table[index];
        i32  root = -1;
        for (i32 x = bin.head; x >= 0; x = node(x).next) {
            node(x).left  = -1;
            node(x).right = -1;
            if (root < 0) {
                node(x).parent = -1;
                node(x).red    = false;
                root           = x;
                continue;
            }
            const i32 h = node(x).hash;
            for (i32 p = root;;) {
                const i32 dir = direction(h, p);
                const i32 xp  = p;
                p             = dir <= 0 ? node(p).left : node(p).right;
                if (p < 0) {
                    node(x).parent = xp;
                    if (dir <= 0) {
                        node(xp).left = x;
                    } else {
                        node(xp).right = x;
                    }
                    root = balance_insertion(root, x);
                    break;
                }
            }
        }
        bin.tree = true;
        move_root_to_front(table, index, root);
    }
    void treeify(usize index) { treeify_in(table_, index); }

    /// `putTreeVal` for a position known not to be in the set.
    void put_tree(usize index, BlockPos pos, i32 h) {
        const i32 root = table_[index].head;  // a tree bin keeps its root first
        for (i32 p = root;;) {
            const i32 dir = direction(h, p);
            const i32 xp  = p;
            p             = dir <= 0 ? node(p).left : node(p).right;
            if (p < 0) {
                const i32 xpn = node(xp).next;
                const i32 x   = make_node(pos, h);
                node(x).next  = xpn;
                if (dir <= 0) {
                    node(xp).left = x;
                } else {
                    node(xp).right = x;
                }
                node(xp).next  = x;
                node(x).parent = xp;
                node(x).prev   = xp;
                if (xpn >= 0) {
                    node(xpn).prev = x;
                }
                move_root_to_front(table_, index, balance_insertion(root, x));
                return;
            }
        }
    }

    void resize(usize next_capacity) {
        std::vector<Bin> grown(next_capacity);
        const usize      old_capacity = table_.size();
        for (usize j = 0; j < old_capacity; ++j) {
            const Bin old = table_[j];
            if (old.head < 0) {
                continue;
            }
            // Split the list in order; the bit the doubling adds decides.
            i32   lo_head = -1, lo_tail = -1, hi_head = -1, hi_tail = -1;
            usize lo_count = 0, hi_count = 0;
            for (i32 e = old.head, next = -1; e >= 0; e = next) {
                next         = node(e).next;
                node(e).next = -1;
                const bool low  = (static_cast<u32>(node(e).hash) & old_capacity) == 0;
                i32&       head = low ? lo_head : hi_head;
                i32&       tail = low ? lo_tail : hi_tail;
                node(e).prev    = tail;
                if (tail < 0) {
                    head = e;
                } else {
                    node(tail).next = e;
                }
                tail = e;
                ++(low ? lo_count : hi_count);
            }
            grown[j].head                = lo_head;
            grown[j + old_capacity].head = hi_head;
            if (!old.tree) {
                continue;
            }
            // A tree bin's halves: a plain list at six or fewer, the same tree
            // when the other half is empty, a new tree otherwise.
            const auto settle = [&](usize at, i32 head, usize count, bool other_empty) {
                if (head < 0 || count <= 6) {
                    return;
                }
                if (other_empty) {
                    grown[at].tree = true;
                } else {
                    treeify_in(grown, at);
                }
            };
            settle(j, lo_head, lo_count, hi_head < 0);
            settle(j + old_capacity, hi_head, hi_count, lo_head < 0);
        }
        table_ = std::move(grown);
    }

    std::vector<JavaNode> nodes_;
    std::vector<Bin>      table_;
    usize                 size_{0};
};

/// The model before treeified bins, kept as the instrument's "before"
/// (`OV_HASHMAP=legacy`): no tree, and a small table resized one entry early.
std::vector<BlockPos> java_hash_order_legacy(const std::vector<BlockPos>& inserted) {
    // A faithful little HashMap: buckets of insertion-ordered lists, doubling
    // when the size passes three quarters of the capacity, and splitting each
    // bucket in a way that preserves relative order.
    //
    // Treeification is the one thing not modelled. A bucket only turns into a
    // tree at eight entries *and* a capacity of 64 or more; below 64 the map
    // resizes instead, which is modelled. Eight positions of a single tree
    // colliding into one bucket of a table that large has not been observed,
    // and if it ever is the log below says so rather than the world quietly
    // changing shape.
    usize capacity = 0;
    std::vector<std::vector<BlockPos>> table;
    std::vector<u32>                   hashes;
    usize                              size = 0;

    const auto resize = [&](usize next_capacity) {
        std::vector<std::vector<BlockPos>> grown(next_capacity);
        for (usize bucket = 0; bucket < table.size(); ++bucket) {
            for (const BlockPos& pos : table[bucket]) {
                const u32 hash = java_spread(java_block_pos_hash(pos));
                grown[hash & (next_capacity - 1)].push_back(pos);
            }
        }
        table    = std::move(grown);
        capacity = next_capacity;
    };

    for (const BlockPos& pos : inserted) {
        if (capacity == 0) {
            resize(16);
        }
        const u32   hash   = java_spread(java_block_pos_hash(pos));
        const usize bucket = hash & (capacity - 1);
        if (std::ranges::find(table[bucket], pos) != table[bucket].end()) {
            continue;  // a set: a repeat is not stored twice
        }
        table[bucket].push_back(pos);
        ++size;
        if (table[bucket].size() >= 8) {
            if (capacity >= 64) {
                note_untreeified_bucket(capacity);
            } else {
                resize(capacity * 2);
                continue;
            }
        }
        if (size > capacity * 3 / 4) {
            resize(capacity * 2);
        }
    }

    std::vector<BlockPos> out;
    out.reserve(size);
    for (const auto& bucket : table) {
        for (const BlockPos& pos : bucket) {
            out.push_back(pos);
        }
    }
    (void)hashes;
    return out;
}

}  // namespace

std::vector<BlockPos> java_hash_order(const std::vector<BlockPos>& inserted) {
    // `OV_HASHMAP=legacy` is the instrument that gives the "before" from the
    // same binary. Read once.
    static const bool legacy = [] {
        const char* setting = std::getenv("OV_HASHMAP");
        return setting != nullptr && std::string_view(setting) == "legacy";
    }();
    if (legacy) {
        return java_hash_order_legacy(inserted);
    }
    JavaHashSet set;
    for (const BlockPos& pos : inserted) {
        set.add(pos);
    }
    return set.iteration();
}

u64 java_hash_treeified_bins() noexcept {
    return treeified_bins().load(std::memory_order_relaxed);
}

// ── The writer ──────────────────────────────────────────────────────────────

struct TreeWriter::Set {
    /// Insertion order, which `java_hash_order` turns into iteration order.
    std::vector<BlockPos> inserted;
    /// Membership, so that `foliage_set` is not a linear scan of a canopy.
    std::vector<BlockPos> sorted_view;

    [[nodiscard]] bool contains(BlockPos at) const {
        return std::ranges::find(inserted, at) != inserted.end();
    }

    void add(BlockPos at) {
        if (!contains(at)) {
            inserted.push_back(at);
        }
    }
};

TreeWriter::TreeWriter(FeatureLevel& level, const registry::BlockRegistry& blocks)
    : level_(&level),
      blocks_(&blocks),
      roots_(std::make_shared<Set>()),
      logs_(std::make_shared<Set>()),
      foliage_(std::make_shared<Set>()),
      decorations_(std::make_shared<Set>()) {}

void TreeWriter::set_root(BlockPos at, registry::BlockStateId state) {
    roots_->add(at);
    (void)level_->set_block(at.x, at.y, at.z, state);
}

void TreeWriter::set_log(BlockPos at, registry::BlockStateId state) {
    logs_->add(at);
    (void)level_->set_block(at.x, at.y, at.z, state);
}

void TreeWriter::set_foliage(BlockPos at, registry::BlockStateId state) {
    foliage_->add(at);
    (void)level_->set_block(at.x, at.y, at.z, state);
}

void TreeWriter::set_decoration(BlockPos at, registry::BlockStateId state) {
    decorations_->add(at);
    (void)level_->set_block(at.x, at.y, at.z, state);
}

bool TreeWriter::foliage_set(BlockPos at) const {
    return foliage_->contains(at);
}

bool TreeWriter::empty() const {
    return logs_->inserted.empty() && foliage_->inserted.empty();
}

namespace {

/// The hash order, then a **stable** sort by ascending y — exactly what
/// `TreeDecorator.Context` does with `Comparator.comparingInt(Vec3i::getY)`
/// over a stream. Stability is the point: within one y the hash order survives,
/// and that is what the decorators walk.
[[nodiscard]] std::vector<BlockPos> sorted_by_y(const std::vector<BlockPos>& inserted) {
    std::vector<BlockPos> order = java_hash_order(inserted);
    std::ranges::stable_sort(order, [](const BlockPos& a, const BlockPos& b) { return a.y < b.y; });
    return order;
}

}  // namespace

std::vector<BlockPos> TreeWriter::sorted_logs() const {
    return sorted_by_y(logs_->inserted);
}

std::vector<BlockPos> TreeWriter::sorted_foliage() const {
    return sorted_by_y(foliage_->inserted);
}

std::vector<BlockPos> TreeWriter::sorted_roots() const {
    return sorted_by_y(roots_->inserted);
}

// ── Bases ───────────────────────────────────────────────────────────────────

FeatureSize::~FeatureSize()     = default;
TrunkPlacer::~TrunkPlacer()     = default;
FoliagePlacer::~FoliagePlacer() = default;
RootPlacer::~RootPlacer()       = default;
TreeDecorator::~TreeDecorator() = default;

i32 TrunkPlacer::tree_height(FeatureRandom& random) const {
    // Two draws, in this order, both always taken. `nextInt(1)` is a draw.
    const i32 first  = random.next_int(height_rand_a_ + 1);
    const i32 second = random.next_int(height_rand_b_ + 1);
    return base_height_ + first + second;
}

namespace {

/// `TrunkPlacer.placeLog`: write a log only where the world allows one.
bool place_log(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random, BlockPos at,
               const TreeConfig& config, std::string_view axis = {}) {
    if (!world.valid_tree_pos(at)) {
        return false;
    }
    auto state = config.trunk_provider->state(*world.level, random, at);
    if (!axis.empty()) {
        state = try_set_property(*world.blocks, state, "axis", axis);
    }
    writer.set_log(at, state);
    return true;
}

/// `TrunkPlacer.setDirtAt`: put dirt under the trunk, unless there already is
/// some and the config does not insist.
void set_dirt_at(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random, BlockPos at,
                 const TreeConfig& config) {
    if (!config.force_dirt && world.is_dirt(at)) {
        return;
    }
    writer.set_log(at, config.dirt_provider->state(*world.level, random, at));
}

}  // namespace

// ── Foliage placer base ─────────────────────────────────────────────────────

void FoliagePlacer::create_foliage(const TreeWorld& world, TreeWriter& writer,
                                   FeatureRandom& random, const TreeConfig& config,
                                   i32 free_height, const FoliageAttachment& attachment,
                                   i32 height, i32 radius) const {
    const i32 offset = offset_->sample(random);
    grow(world, writer, random, config, free_height, attachment, height, radius, offset);
}

i32 FoliagePlacer::foliage_radius(FeatureRandom& random, i32) const {
    return radius_->sample(random);
}

bool FoliagePlacer::should_skip_signed(FeatureRandom& random, i32 local_x, i32 local_y,
                                       i32 local_z, i32 range, bool large) const {
    i32 x = 0;
    i32 z = 0;
    if (large) {
        // A two-by-two trunk has no single centre column, so the fold is around
        // the pair: 0 and 1 are both "distance zero".
        x = std::min(std::abs(local_x), std::abs(local_x - 1));
        z = std::min(std::abs(local_z), std::abs(local_z - 1));
    } else {
        x = std::abs(local_x);
        z = std::abs(local_z);
    }
    return should_skip(random, x, local_y, z, range, large);
}

void FoliagePlacer::place_leaves_row(const TreeWorld& world, TreeWriter& writer,
                                     FeatureRandom& random, const TreeConfig& config, BlockPos at,
                                     i32 range, i32 local_y, bool large) const {
    const i32 extra = large ? 1 : 0;
    for (i32 dx = -range; dx <= range + extra; ++dx) {
        for (i32 dz = -range; dz <= range + extra; ++dz) {
            if (!should_skip_signed(random, dx, local_y, dz, range, large)) {
                try_place_leaf(world, writer, random, config,
                               {at.x + dx, at.y + local_y, at.z + dz});
            }
        }
    }
}

void FoliagePlacer::try_place_leaf(const TreeWorld& world, TreeWriter& writer,
                                   FeatureRandom& random, const TreeConfig& config, BlockPos at) {
    if (!world.valid_tree_pos(at)) {
        return;
    }
    auto state = config.foliage_provider->state(*world.level, random, at);
    if (has_property(*world.blocks, state, "waterlogged")) {
        state = try_set_property(*world.blocks, state, "waterlogged",
                                 world.is_water_source(at) ? "true" : "false");
    }
    writer.set_foliage(at, state);
}

// ── Feature sizes ───────────────────────────────────────────────────────────

namespace {

class TwoLayersFeatureSize final : public FeatureSize {
public:
    TwoLayersFeatureSize(i32 limit, i32 lower, i32 upper, std::optional<i32> min_clipped)
        : limit_(limit), lower_(lower), upper_(upper), min_clipped_(min_clipped) {}

    [[nodiscard]] i32 size_at_height(i32, i32 y) const override {
        return y < limit_ ? lower_ : upper_;
    }
    [[nodiscard]] std::optional<i32> min_clipped_height() const override { return min_clipped_; }

private:
    i32                limit_;
    i32                lower_;
    i32                upper_;
    std::optional<i32> min_clipped_;
};

class ThreeLayersFeatureSize final : public FeatureSize {
public:
    ThreeLayersFeatureSize(i32 limit, i32 upper_limit, i32 lower, i32 middle, i32 upper,
                           std::optional<i32> min_clipped)
        : limit_(limit),
          upper_limit_(upper_limit),
          lower_(lower),
          middle_(middle),
          upper_(upper),
          min_clipped_(min_clipped) {}

    [[nodiscard]] i32 size_at_height(i32 height, i32 y) const override {
        if (y < limit_) {
            return lower_;
        }
        return height - upper_limit_ <= y ? upper_ : middle_;
    }
    [[nodiscard]] std::optional<i32> min_clipped_height() const override { return min_clipped_; }

private:
    i32                limit_;
    i32                upper_limit_;
    i32                lower_;
    i32                middle_;
    i32                upper_;
    std::optional<i32> min_clipped_;
};

// ── Trunk placers ───────────────────────────────────────────────────────────

class StraightTrunkPlacer final : public TrunkPlacer {
public:
    using TrunkPlacer::TrunkPlacer;

    [[nodiscard]] std::string_view type_name() const override { return "straight_trunk_placer"; }

    void place_trunk(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
                     i32 height, BlockPos at, const TreeConfig& config,
                     std::vector<FoliageAttachment>& out) const override {
        set_dirt_at(world, writer, random, at.below(), config);
        for (i32 i = 0; i < height; ++i) {
            (void)place_log(world, writer, random, at.above(i), config);
        }
        out.push_back({at.above(height), 0, false});
    }
};

class ForkingTrunkPlacer final : public TrunkPlacer {
public:
    using TrunkPlacer::TrunkPlacer;

    [[nodiscard]] std::string_view type_name() const override { return "forking_trunk_placer"; }

    void place_trunk(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
                     i32 height, BlockPos at, const TreeConfig& config,
                     std::vector<FoliageAttachment>& out) const override {
        set_dirt_at(world, writer, random, at.below(), config);

        const auto direction = random_horizontal(random);
        // Split, in order: Java sequences `height - nextInt(4) - 1` left to
        // right and C++ does not.
        const i32 bend_draw   = random.next_int(4);
        const i32 bend_start  = height - bend_draw - 1;
        const i32 steps_draw  = random.next_int(3);
        i32       bend_steps  = 3 - steps_draw;

        i32 x   = at.x;
        i32 z   = at.z;
        i32 top = 0;
        for (i32 i = 0; i < height; ++i) {
            const i32 y = at.y + i;
            if (i >= bend_start && bend_steps > 0) {
                x += direction[0];
                z += direction[1];
                --bend_steps;
            }
            if (place_log(world, writer, random, {x, y, z}, config)) {
                top = y + 1;
            }
        }
        out.push_back({{x, top, z}, 1, false});

        x = at.x;
        z = at.z;
        const auto second = random_horizontal(random);
        if (second != direction) {
            const i32 offset_draw = random.next_int(2);
            i32       branch_y    = bend_start - offset_draw - 1;
            const i32 length_draw = random.next_int(3);
            i32       length      = 1 + length_draw;
            top                   = 0;
            for (; branch_y < height && length > 0; --length) {
                if (branch_y >= 1) {
                    const i32 y = at.y + branch_y;
                    x += second[0];
                    z += second[1];
                    if (place_log(world, writer, random, {x, y, z}, config)) {
                        top = y + 1;
                    }
                }
                ++branch_y;
            }
            if (top > 1) {
                out.push_back({{x, top, z}, 0, false});
            }
        }
    }
};

class GiantTrunkPlacer : public TrunkPlacer {
public:
    using TrunkPlacer::TrunkPlacer;

    [[nodiscard]] std::string_view type_name() const override { return "giant_trunk_placer"; }

    void place_trunk(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
                     i32 height, BlockPos at, const TreeConfig& config,
                     std::vector<FoliageAttachment>& out) const override {
        const BlockPos below = at.below();
        set_dirt_at(world, writer, random, below, config);
        set_dirt_at(world, writer, random, below.offset(1, 0, 0), config);
        set_dirt_at(world, writer, random, below.offset(0, 0, 1), config);
        set_dirt_at(world, writer, random, below.offset(1, 0, 1), config);

        for (i32 i = 0; i < height; ++i) {
            place_if_free(world, writer, random, at.offset(0, i, 0), config);
            if (i < height - 1) {
                place_if_free(world, writer, random, at.offset(1, i, 0), config);
                place_if_free(world, writer, random, at.offset(1, i, 1), config);
                place_if_free(world, writer, random, at.offset(0, i, 1), config);
            }
        }
        out.push_back({at.above(height), 0, true});
    }

protected:
    static void place_if_free(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
                              BlockPos at, const TreeConfig& config) {
        if (world.is_free(at)) {
            (void)place_log(world, writer, random, at, config);
        }
    }
};

class MegaJungleTrunkPlacer final : public GiantTrunkPlacer {
public:
    using GiantTrunkPlacer::GiantTrunkPlacer;

    [[nodiscard]] std::string_view type_name() const override { return "mega_jungle_trunk_placer"; }

    void place_trunk(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
                     i32 height, BlockPos at, const TreeConfig& config,
                     std::vector<FoliageAttachment>& out) const override {
        GiantTrunkPlacer::place_trunk(world, writer, random, height, at, config, out);

        const i32 first_draw = random.next_int(4);
        for (i32 i = height - 2 - first_draw; i > height / 2;) {
            // Mth.cos / Mth.sin: the 65536-entry table, not libm. The
            // difference is a few 1e-5 and it lands a branch a block over.
            const f32 angle = random.next_float() * (2.0F * 3.1415927F);
            i32       dx    = 0;
            i32       dz    = 0;
            for (i32 step = 0; step < 5; ++step) {
                dx = static_cast<i32>(1.5F + mth_cos(angle) * static_cast<f32>(step));
                dz = static_cast<i32>(1.5F + mth_sin(angle) * static_cast<f32>(step));
                (void)place_log(world, writer, random,
                                at.offset(dx, i - 3 + step / 2, dz), config);
            }
            out.push_back({at.offset(dx, i, dz), -2, false});
            const i32 gap = random.next_int(4);
            i -= 2 + gap;
        }
    }
};

class DarkOakTrunkPlacer final : public TrunkPlacer {
public:
    using TrunkPlacer::TrunkPlacer;

    [[nodiscard]] std::string_view type_name() const override { return "dark_oak_trunk_placer"; }

    void place_trunk(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
                     i32 height, BlockPos at, const TreeConfig& config,
                     std::vector<FoliageAttachment>& out) const override {
        const BlockPos below = at.below();
        set_dirt_at(world, writer, random, below, config);
        set_dirt_at(world, writer, random, below.offset(1, 0, 0), config);
        set_dirt_at(world, writer, random, below.offset(0, 0, 1), config);
        set_dirt_at(world, writer, random, below.offset(1, 0, 1), config);

        const auto direction  = random_horizontal(random);
        const i32  bend_draw  = random.next_int(4);
        const i32  bend_start = height - bend_draw;
        const i32  step_draw  = random.next_int(3);
        i32        bend_steps = 2 - step_draw;

        i32       x   = at.x;
        i32       z   = at.z;
        const i32 top = at.y + height - 1;
        for (i32 i = 0; i < height; ++i) {
            if (i >= bend_start && bend_steps > 0) {
                x += direction[0];
                z += direction[1];
                --bend_steps;
            }
            const BlockPos here{x, at.y + i, z};
            if (world.is_air_or_leaves(here)) {
                (void)place_log(world, writer, random, here, config);
                (void)place_log(world, writer, random, here.offset(1, 0, 0), config);
                (void)place_log(world, writer, random, here.offset(0, 0, 1), config);
                (void)place_log(world, writer, random, here.offset(1, 0, 1), config);
            }
        }
        out.push_back({{x, top, z}, 0, true});

        for (i32 dx = -1; dx <= 2; ++dx) {
            for (i32 dz = -1; dz <= 2; ++dz) {
                if (dx >= 0 && dx <= 1 && dz >= 0 && dz <= 1) {
                    continue;
                }
                const i32 chance = random.next_int(3);
                if (chance > 0) {
                    continue;
                }
                const i32 length_draw = random.next_int(3);
                const i32 length      = length_draw + 2;
                for (i32 step = 0; step < length; ++step) {
                    (void)place_log(world, writer, random,
                                    {at.x + dx, top - step - 1, at.z + dz}, config);
                }
                out.push_back({{x + dx, top, z + dz}, 0, false});
            }
        }
    }
};

class BendingTrunkPlacer final : public TrunkPlacer {
public:
    BendingTrunkPlacer(i32 base, i32 a, i32 b, i32 min_height_for_leaves, IntProviderRef bend)
        : TrunkPlacer(base, a, b),
          min_height_for_leaves_(min_height_for_leaves),
          bend_length_(std::move(bend)) {}

    [[nodiscard]] std::string_view type_name() const override { return "bending_trunk_placer"; }

    void place_trunk(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
                     i32 height, BlockPos at, const TreeConfig& config,
                     std::vector<FoliageAttachment>& out) const override {
        const auto direction = random_horizontal(random);
        const i32  top       = height - 1;
        BlockPos   cursor    = at;
        set_dirt_at(world, writer, random, cursor.below(), config);

        for (i32 i = 0; i <= top; ++i) {
            const i32 wobble = random.next_int(2);
            if (i + 1 >= top + wobble) {
                cursor = cursor.offset(direction[0], 0, direction[1]);
            }
            if (world.is_free(cursor)) {
                (void)place_log(world, writer, random, cursor, config);
            }
            if (i >= min_height_for_leaves_) {
                out.push_back({cursor, 0, false});
            }
            cursor = cursor.above();
        }

        const i32 bend = bend_length_->sample(random);
        for (i32 i = 0; i <= bend; ++i) {
            if (world.is_free(cursor)) {
                (void)place_log(world, writer, random, cursor, config);
            }
            out.push_back({cursor, 0, false});
            cursor = cursor.offset(direction[0], 0, direction[1]);
        }
    }

private:
    i32            min_height_for_leaves_;
    IntProviderRef bend_length_;
};

class UpwardsBranchingTrunkPlacer final : public TrunkPlacer {
public:
    UpwardsBranchingTrunkPlacer(i32 base, i32 a, i32 b, IntProviderRef extra_steps,
                                f32 branch_probability, IntProviderRef extra_length,
                                std::vector<u16> can_grow_through)
        : TrunkPlacer(base, a, b),
          extra_branch_steps_(std::move(extra_steps)),
          place_branch_per_log_probability_(branch_probability),
          extra_branch_length_(std::move(extra_length)),
          can_grow_through_(std::move(can_grow_through)) {}

    [[nodiscard]] std::string_view type_name() const override {
        return "upwards_branching_trunk_placer";
    }

    void place_trunk(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
                     i32 height, BlockPos at, const TreeConfig& config,
                     std::vector<FoliageAttachment>& out) const override {
        for (i32 i = 0; i < height; ++i) {
            const i32      y = at.y + i;
            const BlockPos here{at.x, y, at.z};
            if (place_log_through(world, writer, random, here, config) && i + 1 < height) {
                const f32 roll = random.next_float();
                if (roll < place_branch_per_log_probability_) {
                    const auto direction = random_horizontal(random);
                    // The branch length is drawn twice — once for the length,
                    // once for the cut — not once plus a bounded jitter.
                    const i32 length = extra_branch_length_->sample(random);
                    const i32 cut    = extra_branch_length_->sample(random);
                    const i32 start  = std::max(0, length - cut - 1);
                    const i32 steps  = extra_branch_steps_->sample(random);
                    place_branch(world, writer, random, height, config, out, at, y, direction,
                                 start, steps);
                }
            }
            if (i == height - 1) {
                out.push_back({{at.x, y + 1, at.z}, 0, false});
            }
        }
    }

private:
    /// A log that may also replace what the config says it can grow through —
    /// mud and mangrove roots, for the mangrove.
    [[nodiscard]] bool place_log_through(const TreeWorld& world, TreeWriter& writer,
                                         FeatureRandom& random, BlockPos at,
                                         const TreeConfig& config) const {
        if (!world.valid_tree_pos(at) &&
            !TreeTags::holds(can_grow_through_, world.block_at(at))) {
            return false;
        }
        writer.set_log(at, config.trunk_provider->state(*world.level, random, at));
        return true;
    }

    void place_branch(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
                      i32 height, const TreeConfig& config, std::vector<FoliageAttachment>& out,
                      BlockPos at, i32 y, std::array<i32, 2> direction, i32 start,
                      i32 steps) const {
        i32 x   = at.x;
        i32 z   = at.z;
        i32 tip = y + start;
        for (i32 step = start; step < height && steps > 0; ++step, --steps) {
            if (step < 1) {
                continue;
            }
            const i32 branch_y = y + step;
            x += direction[0];
            z += direction[1];
            tip = branch_y;
            if (place_log_through(world, writer, random, {x, branch_y, z}, config)) {
                tip = branch_y + 1;
            }
            // Foliage hangs on the log itself at every step…
            out.push_back({{x, branch_y, z}, 0, false});
        }
        // …and, on a branch that climbed, twice more: at its tip and two
        // below it.
        if (tip - y > 1) {
            out.push_back({{x, tip, z}, 0, false});
            out.push_back({{x, tip - 2, z}, 0, false});
        }
    }

    IntProviderRef   extra_branch_steps_;
    f32              place_branch_per_log_probability_;
    IntProviderRef   extra_branch_length_;
    std::vector<u16> can_grow_through_;
};

/// The big oak. Two hundred lines of geometry and the densest run of draws in
/// worldgen.
///
/// Its trigonometry is `Math.sin`/`Math.cos` on **doubles** — not `Mth`. The
/// mega jungle trunk a few classes up uses the table. Swapping them is silent
/// and moves every branch.
class FancyTrunkPlacer final : public TrunkPlacer {
public:
    using TrunkPlacer::TrunkPlacer;

    [[nodiscard]] std::string_view type_name() const override { return "fancy_trunk_placer"; }

    void place_trunk(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
                     i32 free_height, BlockPos at, const TreeConfig& config,
                     std::vector<FoliageAttachment>& out) const override {
        const i32 height    = free_height + 2;
        const i32 trunk_top = mth_floor(static_cast<f64>(height) * 0.618);
        set_dirt_at(world, writer, random, at.below(), config);

        // `Math.min`, not max. It reads like a mistake and it is the game's:
        // the cluster count is one at every height the overworld reaches.
        // `Math.min`, not max. It reads like a mistake and it is the game's:
        // the cluster count is one at every height the overworld reaches.
        // Measured, not assumed — `max` costs four matched trunks and nine
        // matched blocks over the same sample.
        const i32 clusters = std::min(
            1, mth_floor(1.382 + std::pow(1.0 * static_cast<f64>(height) / 13.0, 2.0)));
        const i32 base_y = at.y + trunk_top;

        struct FoliageCoords {
            BlockPos pos{};
            i32      branch_base{0};
        };
        std::vector<FoliageCoords> coords;
        coords.push_back({at.above(height - 5), base_y});

        for (i32 y = height - 5; y >= 0; --y) {
            const f32 shape = tree_shape(height, y);
            if (shape < 0.0F) {
                continue;
            }
            for (i32 cluster = 0; cluster < clusters; ++cluster) {
                const f32 spread_draw = random.next_float();
                const f64 spread =
                    1.0 * static_cast<f64>(shape) * (static_cast<f64>(spread_draw) + 0.328);
                const f32 angle_draw = random.next_float();
                const f64 angle = static_cast<f64>(angle_draw * 2.0F) * 3.141592653589793;
                const f64 dx    = spread * std::sin(angle) + 0.5;
                const f64 dz    = spread * std::cos(angle) + 0.5;
                const BlockPos start{at.x + mth_floor(dx), at.y + y - 1, at.z + mth_floor(dz)};
                const BlockPos tip = start.above(5);
                if (!make_limb(world, writer, random, start, tip, false, config)) {
                    continue;
                }
                const i32 back_x = at.x - start.x;
                const i32 back_z = at.z - start.z;
                const f64 slope  = static_cast<f64>(start.y) -
                                  std::sqrt(static_cast<f64>(back_x * back_x + back_z * back_z)) *
                                      0.381;
                const i32      branch_base = slope > static_cast<f64>(base_y)
                                                 ? base_y
                                                 : static_cast<i32>(slope);
                const BlockPos from{at.x, branch_base, at.z};
                if (make_limb(world, writer, random, from, start, false, config)) {
                    coords.push_back({start, branch_base});
                }
            }
        }

        (void)make_limb(world, writer, random, at, at.above(trunk_top), true, config);

        for (const FoliageCoords& coord : coords) {
            const BlockPos from{at.x, coord.branch_base, at.z};
            if (from != coord.pos && trim_branches(height, coord.branch_base - at.y)) {
                (void)make_limb(world, writer, random, from, coord.pos, true, config);
            }
        }

        for (const FoliageCoords& coord : coords) {
            if (trim_branches(height, coord.branch_base - at.y)) {
                out.push_back({coord.pos, 0, false});
            }
        }
    }

private:
    [[nodiscard]] static bool trim_branches(i32 height, i32 y) {
        return static_cast<f64>(y) >= static_cast<f64>(height) * 0.2;
    }

    [[nodiscard]] static f32 tree_shape(i32 height, i32 y) {
        if (static_cast<f32>(y) < static_cast<f32>(height) * 0.3F) {
            return -1.0F;
        }
        const f32 half = static_cast<f32>(height) / 2.0F;
        const f32 from = half - static_cast<f32>(y);
        f32       out  = std::sqrt(half * half - from * from);
        if (from == 0.0F) {
            out = half;
        } else if (std::abs(from) >= half) {
            return 0.0F;
        }
        return out * 0.5F;
    }

    [[nodiscard]] static i32 steps_between(BlockPos delta) {
        return std::max(std::abs(delta.x), std::max(std::abs(delta.y), std::abs(delta.z)));
    }

    static bool make_limb(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
                          BlockPos from, BlockPos to, bool write, const TreeConfig& config) {
        if (!write && from == to) {
            return true;
        }
        const BlockPos delta{to.x - from.x, to.y - from.y, to.z - from.z};
        const i32      steps = steps_between(delta);
        const f32      step_x = static_cast<f32>(delta.x) / static_cast<f32>(steps);
        const f32      step_y = static_cast<f32>(delta.y) / static_cast<f32>(steps);
        const f32      step_z = static_cast<f32>(delta.z) / static_cast<f32>(steps);
        for (i32 i = 0; i <= steps; ++i) {
            const BlockPos here = from.offset(mth_floor(0.5F + static_cast<f32>(i) * step_x),
                                              mth_floor(0.5F + static_cast<f32>(i) * step_y),
                                              mth_floor(0.5F + static_cast<f32>(i) * step_z));
            if (write) {
                (void)place_log(world, writer, random, here, config, log_axis(from, here));
            } else if (!world.is_free(here)) {
                return false;
            }
        }
        return true;
    }
};

/// The cherry: a trunk that stops where its branches leave it, and one or two
/// branches that walk out and up to a target, one block at a time.
///
/// The draws, in order: the first branch's start, the second's start (from a
/// range one narrower at the top, and pushed past the first when they
/// collide), the branch count, then one direction shared by both branches —
/// the second goes the opposite way. Each branch then draws its end height,
/// its length, and one float per step of its walk.
class CherryTrunkPlacer final : public TrunkPlacer {
public:
    CherryTrunkPlacer(i32 base, i32 a, i32 b, IntProviderRef branch_count,
                      IntProviderRef branch_horizontal_length, i32 start_min, i32 start_max,
                      IntProviderRef end_offset)
        : TrunkPlacer(base, a, b),
          branch_count_(std::move(branch_count)),
          branch_horizontal_length_(std::move(branch_horizontal_length)),
          start_min_(start_min),
          start_max_(start_max),
          branch_end_offset_from_top_(std::move(end_offset)) {}

    [[nodiscard]] std::string_view type_name() const override { return "cherry_trunk_placer"; }

    void place_trunk(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
                     i32 height, BlockPos at, const TreeConfig& config,
                     std::vector<FoliageAttachment>& out) const override {
        set_dirt_at(world, writer, random, at.below(), config);

        const i32 first_draw = uniform(random, start_min_, start_max_);
        const i32 first      = std::max(0, height - 1 + first_draw);
        // The second start's range is the first's with its top lowered by one,
        // and a collision is resolved by stepping past: the two never share a
        // height, and the draw count does not change.
        const i32 second_draw = uniform(random, start_min_, start_max_ - 1);
        i32       second      = std::max(0, height - 1 + second_draw);
        if (second >= first) {
            ++second;
        }
        const i32  count        = branch_count_->sample(random);
        const bool three        = count == 3;
        const bool two_or_more  = count >= 2;
        const i32  trunk_height = three         ? height
                                  : two_or_more ? std::max(first, second) + 1
                                                : first + 1;
        for (i32 i = 0; i < trunk_height; ++i) {
            (void)place_log(world, writer, random, at.above(i), config);
        }
        if (three) {
            out.push_back({at.above(trunk_height), 0, false});
        }

        const auto direction = random_horizontal(random);
        // Both branches carry the first direction's axis; the vertical steps
        // of the walk keep the provider's own.
        const std::string_view axis = direction[0] != 0 ? "x" : "z";
        out.push_back(branch(world, writer, random, config, height, at, direction, axis, first,
                             first < trunk_height - 1));
        if (two_or_more) {
            const std::array<i32, 2> opposite{-direction[0], -direction[1]};
            out.push_back(branch(world, writer, random, config, height, at, opposite, axis,
                                 second, second < trunk_height - 1));
        }
    }

private:
    /// `Mth.randomBetweenInclusive`: one `nextInt` over the width.
    [[nodiscard]] static i32 uniform(FeatureRandom& random, i32 low, i32 high) {
        return random.next_int(high - low + 1) + low;
    }

    FoliageAttachment branch(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
                             const TreeConfig& config, i32 height, BlockPos at,
                             std::array<i32, 2> direction, std::string_view axis, i32 start,
                             bool trunk_continues) const {
        BlockPos  cursor   = at.above(start);
        const i32 end_draw = branch_end_offset_from_top_->sample(random);
        const i32 end_y    = height - 1 + end_draw;
        // A branch that leaves a trunk which goes on, or that ends below where
        // it started, takes one extra step out before it turns.
        const bool extended   = trunk_continues || end_y < start;
        const i32  length     = branch_horizontal_length_->sample(random) + (extended ? 1 : 0);
        const BlockPos target = at.offset(direction[0] * length, end_y, direction[1] * length);

        const i32 straight = extended ? 2 : 1;
        for (i32 step = 0; step < straight; ++step) {
            cursor = cursor.offset(direction[0], 0, direction[1]);
            (void)place_log(world, writer, random, cursor, config, axis);
        }
        const i32 vertical = target.y > cursor.y ? 1 : -1;
        for (;;) {
            const i32 distance = std::abs(target.x - cursor.x) + std::abs(target.y - cursor.y) +
                                 std::abs(target.z - cursor.z);
            if (distance == 0) {
                return {target.above(), 0, false};
            }
            const f32  chance = static_cast<f32>(std::abs(target.y - cursor.y)) /
                               static_cast<f32>(distance);
            const f32  roll   = random.next_float();
            const bool up     = roll < chance;
            cursor = up ? cursor.above(vertical) : cursor.offset(direction[0], 0, direction[1]);
            (void)place_log(world, writer, random, cursor, config, up ? std::string_view{} : axis);
        }
    }

    IntProviderRef branch_count_;
    IntProviderRef branch_horizontal_length_;
    i32            start_min_;
    i32            start_max_;
    IntProviderRef branch_end_offset_from_top_;
};

// ── Foliage placers ─────────────────────────────────────────────────────────

class BlobFoliagePlacer : public FoliagePlacer {
public:
    BlobFoliagePlacer(IntProviderRef radius, IntProviderRef offset, i32 height)
        : FoliagePlacer(std::move(radius), std::move(offset)), height_(height) {}

    [[nodiscard]] std::string_view type_name() const override { return "blob_foliage_placer"; }

    [[nodiscard]] i32 foliage_height(FeatureRandom&, i32, const TreeConfig&) const override {
        return height_;
    }

protected:
    void grow(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
              const TreeConfig& config, i32, const FoliageAttachment& attachment, i32 height,
              i32 radius, i32 offset) const override {
        for (i32 y = offset; y >= offset - height; --y) {
            const i32 range = std::max(radius + attachment.radius_offset - 1 - y / 2, 0);
            place_leaves_row(world, writer, random, config, attachment.pos, range, y,
                             attachment.double_trunk);
        }
    }

    [[nodiscard]] bool should_skip(FeatureRandom& random, i32 x, i32 y, i32 z, i32 range,
                                   bool) const override {
        if (x != range || z != range) {
            return false;
        }
        // The draw is the *left* operand of `||`, so it always happens once the
        // corner is reached, whatever y is.
        const bool roll = random.next_int(2) == 0;
        return roll || y == 0;
    }

    i32 height_;
};

/// The big oak's canopy, measured rather than reasoned.
///
/// `scripts/probe_tree.sh` grows one fancy oak per chunk in a world whose only
/// feature is that oak, then three copies of the same tree whose foliage placer
/// differs in exactly one field. Reading the rows off the disk gives the rule
/// directly, with no silhouette-matching in between:
///
/// | radius | offset | height | rows, top to bottom |
/// |---|---|---|---|
/// | 2 | 4 | 4 | 1, 2, 2, 2, 1 |
/// | 4 | 4 | 4 | 3, 4, 4, 4, 3 |
/// | 2 | 6 | 6 | 1, 2, 2, 2, 2, 2, 1 |
/// | 2 | 1 | 4 | 1, 2, 2, 2, 1 (at `dy` 1 down to −3) |
///
/// So: the rows run from `offset` down to `offset - height` as everywhere else,
/// the range is the radius, and the **first and last** rows are one narrower.
/// The reading that had been guessed here instead — `radius + 1 - y` — makes a
/// cone that is widest at the bottom, which is why a fancy oak came out with
/// its canopy two levels too low and no tree ever matched.
///
/// The cell count fixes the skip just as tightly: 21 of 25 at range 2, 37 of 49
/// at range 3, 61 of 81 at range 4. Those are exactly the lattice points with
/// `x² + z² < range² + range`, and nothing else.
class FancyFoliagePlacer final : public BlobFoliagePlacer {
public:
    using BlobFoliagePlacer::BlobFoliagePlacer;

    [[nodiscard]] std::string_view type_name() const override { return "fancy_foliage_placer"; }

protected:
    void grow(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
              const TreeConfig& config, i32, const FoliageAttachment& attachment, i32 height,
              i32 radius, i32 offset) const override {
        const i32 wide = radius + attachment.radius_offset;
        for (i32 y = offset; y >= offset - height; --y) {
            const bool cap   = y == offset || y == offset - height;
            const i32  range = cap ? wide - 1 : wide;
            place_leaves_row(world, writer, random, config, attachment.pos, range, y,
                             attachment.double_trunk);
        }
    }

    [[nodiscard]] bool should_skip(FeatureRandom&, i32 x, i32, i32 z, i32 range,
                                   bool) const override {
        // Measured at ranges 1 to 4. A radius above 4 is not reachable from any
        // vanilla file and is therefore not measured.
        return x * x + z * z >= range * range + range;
    }
};

/// The jungle bush.
///
/// Its corner is **drawn for**, and the draw is not the blob's. Measured on a
/// probe world of nothing but `trees_jungle`: the game keeps three corners of a
/// range-1 row out of four — so the corner is not skipped outright — and the
/// same row at local y = 0 keeps them, so the blob's `|| y == 0` clause is not
/// there either. The degenerate corner of a **range-zero** row, which is the
/// single block at the top of a bush, is sometimes written and sometimes not,
/// which is how one knows the draw happens even when there is no corner to
/// speak of.
///
/// This mattered far past the bush's own three rows: the draw was missing
/// entirely, so every feature placed after a bush in the same chunk was reading
/// the generator one step early.
class BushFoliagePlacer final : public BlobFoliagePlacer {
public:
    using BlobFoliagePlacer::BlobFoliagePlacer;

    [[nodiscard]] std::string_view type_name() const override { return "bush_foliage_placer"; }

protected:
    void grow(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
              const TreeConfig& config, i32, const FoliageAttachment& attachment, i32 height,
              i32 radius, i32 offset) const override {
        for (i32 y = offset; y >= offset - height; --y) {
            const i32 range = radius + attachment.radius_offset - 1 - y;
            place_leaves_row(world, writer, random, config, attachment.pos, range, y,
                             attachment.double_trunk);
        }
    }

    [[nodiscard]] bool should_skip(FeatureRandom& random, i32 x, i32, i32 z, i32 range,
                                   bool) const override {
        if (x != range || z != range) {
            return false;
        }
        return random.next_int(2) == 0;
    }
};

class AcaciaFoliagePlacer final : public FoliagePlacer {
public:
    using FoliagePlacer::FoliagePlacer;

    [[nodiscard]] std::string_view type_name() const override { return "acacia_foliage_placer"; }

    [[nodiscard]] i32 foliage_height(FeatureRandom&, i32, const TreeConfig&) const override {
        return 0;
    }

protected:
    void grow(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
              const TreeConfig& config, i32, const FoliageAttachment& attachment, i32 height,
              i32 radius, i32 offset) const override {
        const bool     large = attachment.double_trunk;
        const BlockPos pos   = attachment.pos.above(offset);
        place_leaves_row(world, writer, random, config, pos, radius + attachment.radius_offset,
                         -1 - height, large);
        place_leaves_row(world, writer, random, config, pos, radius - 1, -height, large);
        place_leaves_row(world, writer, random, config, pos,
                         radius + attachment.radius_offset - 1, 0, large);
    }

    [[nodiscard]] bool should_skip(FeatureRandom&, i32 x, i32 y, i32 z, i32 range,
                                   bool) const override {
        if (y == 0) {
            return (x > 1 || z > 1) && x != 0 && z != 0;
        }
        return x == range && z == range;
    }
};

class SpruceFoliagePlacer final : public FoliagePlacer {
public:
    SpruceFoliagePlacer(IntProviderRef radius, IntProviderRef offset, IntProviderRef trunk_height)
        : FoliagePlacer(std::move(radius), std::move(offset)),
          trunk_height_(std::move(trunk_height)) {}

    [[nodiscard]] std::string_view type_name() const override { return "spruce_foliage_placer"; }

    [[nodiscard]] i32 foliage_height(FeatureRandom& random, i32 height,
                                     const TreeConfig&) const override {
        const i32 trunk = trunk_height_->sample(random);
        return std::max(4, height - trunk);
    }

protected:
    void grow(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
              const TreeConfig& config, i32, const FoliageAttachment& attachment, i32 height,
              i32 radius, i32 offset) const override {
        const BlockPos pos = attachment.pos;
        i32            range = random.next_int(2);
        i32            cap   = 1;
        i32            floor = 0;
        for (i32 y = offset; y >= -height; --y) {
            place_leaves_row(world, writer, random, config, pos, range, y,
                             attachment.double_trunk);
            if (range >= cap) {
                range = floor;
                floor = 1;
                cap   = std::min(cap + 1, radius + attachment.radius_offset);
            } else {
                ++range;
            }
        }
    }

    [[nodiscard]] bool should_skip(FeatureRandom&, i32 x, i32, i32 z, i32 range,
                                   bool) const override {
        return x == range && z == range && range > 0;
    }

private:
    IntProviderRef trunk_height_;
};

class PineFoliagePlacer final : public FoliagePlacer {
public:
    PineFoliagePlacer(IntProviderRef radius, IntProviderRef offset, IntProviderRef height)
        : FoliagePlacer(std::move(radius), std::move(offset)), height_(std::move(height)) {}

    [[nodiscard]] std::string_view type_name() const override { return "pine_foliage_placer"; }

    [[nodiscard]] i32 foliage_height(FeatureRandom& random, i32, const TreeConfig&) const override {
        return height_->sample(random);
    }

protected:
    void grow(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
              const TreeConfig& config, i32, const FoliageAttachment& attachment, i32 height,
              i32 radius, i32 offset) const override {
        i32 range = 0;
        for (i32 y = offset; y >= offset - height; --y) {
            place_leaves_row(world, writer, random, config, attachment.pos, range, y,
                             attachment.double_trunk);
            if (range >= 1 && y == offset - height + 1) {
                --range;
            } else if (range < radius + attachment.radius_offset) {
                ++range;
            }
        }
    }

    [[nodiscard]] bool should_skip(FeatureRandom&, i32 x, i32, i32 z, i32 range,
                                   bool) const override {
        return x == range && z == range && range > 0;
    }

private:
    IntProviderRef height_;
};

class JungleFoliagePlacer final : public FoliagePlacer {
public:
    JungleFoliagePlacer(IntProviderRef radius, IntProviderRef offset, i32 height)
        : FoliagePlacer(std::move(radius), std::move(offset)), height_(height) {}

    [[nodiscard]] std::string_view type_name() const override { return "jungle_foliage_placer"; }

    [[nodiscard]] i32 foliage_height(FeatureRandom&, i32, const TreeConfig&) const override {
        return height_;
    }

protected:
    void grow(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
              const TreeConfig& config, i32, const FoliageAttachment& attachment, i32 height,
              i32 radius, i32 offset) const override {
        i32 depth = 0;
        if (attachment.double_trunk) {
            depth = height;
        } else {
            const i32 draw = random.next_int(2);
            depth          = 1 + draw;
        }
        for (i32 y = offset; y >= offset - depth; --y) {
            const i32 range = radius + attachment.radius_offset + 1 - y;
            place_leaves_row(world, writer, random, config, attachment.pos, range, y,
                             attachment.double_trunk);
        }
    }

    /// A disc, cut at `x + z >= 7` — the mega pine's mask, not the acacia's.
    /// The mega jungle tree is the only vanilla user of this placer, and the
    /// acacia-style mask it had here cut every one of its canopies wrong.
    [[nodiscard]] bool should_skip(FeatureRandom&, i32 x, i32, i32 z, i32 range,
                                   bool) const override {
        if (x + z >= 7) {
            return true;
        }
        return x * x + z * z > range * range;
    }

private:
    i32 height_;
};

class MegaPineFoliagePlacer final : public FoliagePlacer {
public:
    MegaPineFoliagePlacer(IntProviderRef radius, IntProviderRef offset, IntProviderRef crown)
        : FoliagePlacer(std::move(radius), std::move(offset)), crown_height_(std::move(crown)) {}

    [[nodiscard]] std::string_view type_name() const override { return "mega_pine_foliage_placer"; }

    [[nodiscard]] i32 foliage_height(FeatureRandom& random, i32, const TreeConfig&) const override {
        return crown_height_->sample(random);
    }

protected:
    /// Bottom up. The smooth taper `floor(down / height × 3.5)` alone gives a
    /// monotone cone, and the game's crown is not monotone: read layer by
    /// layer off a probe world its ranges go `0, 0, 1, 0, 2, 1, 2` from the
    /// top. The difference is a ridge — a level whose smooth range equals the
    /// level below it grows by one when its `y` is even — which is what turns
    /// the cone into the stepped silhouette of a giant spruce.
    void grow(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
              const TreeConfig& config, i32, const FoliageAttachment& attachment, i32 height,
              i32 radius, i32 offset) const override {
        const BlockPos pos      = attachment.pos;
        i32            previous = 0;
        for (i32 y = pos.y - height + offset; y <= pos.y + offset; ++y) {
            const i32 down   = pos.y - y;
            const i32 smooth = radius + attachment.radius_offset +
                               mth_floor(static_cast<f32>(down) / static_cast<f32>(height) * 3.5F);
            const bool ridge = down > 0 && smooth == previous && (y & 1) == 0;
            const i32  range = ridge ? smooth + 1 : smooth;
            place_leaves_row(world, writer, random, config, {pos.x, y, pos.z}, range, 0,
                             attachment.double_trunk);
            previous = smooth;
        }
    }

    [[nodiscard]] bool should_skip(FeatureRandom&, i32 x, i32, i32 z, i32 range,
                                   bool) const override {
        if (x + z >= 7) {
            return true;
        }
        return x * x + z * z > range * range;
    }

private:
    IntProviderRef crown_height_;
};

class DarkOakFoliagePlacer final : public FoliagePlacer {
public:
    using FoliagePlacer::FoliagePlacer;

    [[nodiscard]] std::string_view type_name() const override { return "dark_oak_foliage_placer"; }

    [[nodiscard]] i32 foliage_height(FeatureRandom&, i32, const TreeConfig&) const override {
        return 4;
    }

protected:
    void grow(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
              const TreeConfig& config, i32, const FoliageAttachment& attachment, i32,
              i32 radius, i32 offset) const override {
        const BlockPos pos   = attachment.pos.above(offset);
        const bool     large = attachment.double_trunk;
        if (large) {
            place_leaves_row(world, writer, random, config, pos, radius + 2, -1, large);
            place_leaves_row(world, writer, random, config, pos, radius + 3, 0, large);
            place_leaves_row(world, writer, random, config, pos, radius + 2, 1, large);
            if (random.next_boolean()) {
                place_leaves_row(world, writer, random, config, pos, radius, 2, large);
            }
        } else {
            place_leaves_row(world, writer, random, config, pos, radius + 2, -1, large);
            place_leaves_row(world, writer, random, config, pos, radius + 1, 0, large);
        }
    }

    /// The wide middle row of a two-by-two attachment loses its four corners
    /// outright — tested on the *signed* coordinates, before the fold, so the
    /// corner is the true corner of the (2r+2)-wide square. Everything else
    /// goes to the folded test below. Nothing here draws.
    [[nodiscard]] bool should_skip_signed(FeatureRandom& random, i32 local_x, i32 local_y,
                                          i32 local_z, i32 range, bool large) const override {
        // Read off the probe: on the range-3 row the game cuts the cells whose
        // x and z are both in {-3, 3, 4} — the far column of the square
        // (range + 1) and the one before it both count as its edge.
        const bool x_edge = local_x == -range || local_x >= range;
        const bool z_edge = local_z == -range || local_z >= range;
        if (local_y == 0 && large && x_edge && z_edge) {
            return true;
        }
        return FoliagePlacer::should_skip_signed(random, local_x, local_y, local_z, range, large);
    }

    /// The lower row of a single attachment is a square with its corners cut;
    /// the top row is a diamond-cut square, `x + z ≤ 2r - 2` on the folded
    /// coordinates — which is the "x' + z' ≤ 2" the probe read off the range-2
    /// row. The middle row keeps everything the signed test let through.
    [[nodiscard]] bool should_skip(FeatureRandom&, i32 x, i32 y, i32 z, i32 range,
                                   bool large) const override {
        if (y == -1 && !large) {
            return x == range && z == range;
        }
        if (y == 1) {
            return x + z > range * 2 - 2;
        }
        return false;
    }
};

class RandomSpreadFoliagePlacer final : public FoliagePlacer {
public:
    RandomSpreadFoliagePlacer(IntProviderRef radius, IntProviderRef offset,
                              IntProviderRef foliage_height, i32 attempts)
        : FoliagePlacer(std::move(radius), std::move(offset)),
          foliage_height_(std::move(foliage_height)),
          attempts_(attempts) {}

    [[nodiscard]] std::string_view type_name() const override {
        return "random_spread_foliage_placer";
    }

    [[nodiscard]] i32 foliage_height(FeatureRandom& random, i32, const TreeConfig&) const override {
        return foliage_height_->sample(random);
    }

protected:
    void grow(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
              const TreeConfig& config, i32, const FoliageAttachment& attachment, i32 height,
              i32 radius, i32) const override {
        const BlockPos pos = attachment.pos;
        for (i32 attempt = 0; attempt < attempts_; ++attempt) {
            // Six draws, named and in order: `nextInt(r) - nextInt(r)` is the
            // exact expression C++ is free to evaluate backwards.
            const i32 x_hi = random.next_int(radius);
            const i32 x_lo = random.next_int(radius);
            const i32 y_hi = random.next_int(height);
            const i32 y_lo = random.next_int(height);
            const i32 z_hi = random.next_int(radius);
            const i32 z_lo = random.next_int(radius);
            try_place_leaf(world, writer, random, config,
                           pos.offset(x_hi - x_lo, y_hi - y_lo, z_hi - z_lo));
        }
    }

    [[nodiscard]] bool should_skip(FeatureRandom&, i32, i32, i32, i32, bool) const override {
        return false;
    }

private:
    IntProviderRef foliage_height_;
    i32            attempts_;
};

class CherryFoliagePlacer final : public FoliagePlacer {
public:
    CherryFoliagePlacer(IntProviderRef radius, IntProviderRef offset, IntProviderRef height,
                        f32 wide_bottom_layer_hole_chance, f32 corner_hole_chance,
                        f32 hanging_leaves_chance, f32 hanging_leaves_extension_chance)
        : FoliagePlacer(std::move(radius), std::move(offset)),
          height_(std::move(height)),
          wide_bottom_layer_hole_chance_(wide_bottom_layer_hole_chance),
          corner_hole_chance_(corner_hole_chance),
          hanging_leaves_chance_(hanging_leaves_chance),
          hanging_leaves_extension_chance_(hanging_leaves_extension_chance) {}

    [[nodiscard]] std::string_view type_name() const override { return "cherry_foliage_placer"; }

    [[nodiscard]] i32 foliage_height(FeatureRandom& random, i32, const TreeConfig&) const override {
        return height_->sample(random);
    }

protected:
    void grow(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
              const TreeConfig& config, i32, const FoliageAttachment& attachment, i32 height,
              i32 radius, i32 offset) const override {
        const bool     large = attachment.double_trunk;
        const BlockPos pos   = attachment.pos.above(offset);
        const i32      wide  = radius + attachment.radius_offset - 1;

        place_leaves_row(world, writer, random, config, pos, wide - 2, height - 3, large);
        place_leaves_row(world, writer, random, config, pos, wide - 1, height - 4, large);
        for (i32 y = height - 5; y >= 0; --y) {
            place_leaves_row(world, writer, random, config, pos, wide, y, large);
        }
        place_row_with_hanging_leaves(world, writer, random, config, pos, wide, -1, large);
        place_row_with_hanging_leaves(world, writer, random, config, pos, wide - 1, -2, large);
    }

    /// Only the rims draw. The bottom row's rim cells draw once for a hole;
    /// a corner draws once for a hole; a wide row (range above two) also
    /// draws for the diamond-cut cells next to its corners. Every other cell
    /// is kept without a draw.
    [[nodiscard]] bool should_skip(FeatureRandom& random, i32 x, i32 y, i32 z, i32 range,
                                   bool) const override {
        if (y == -1 && (x == range || z == range)) {
            const f32 roll = random.next_float();
            if (roll < wide_bottom_layer_hole_chance_) {
                return true;
            }
        }
        const bool corner = x == range && z == range;
        if (range > 2) {
            if (corner) {
                return true;
            }
            if (x + z > range * 2 - 2) {
                return random.next_float() < corner_hole_chance_;
            }
            return false;
        }
        if (!corner) {
            return false;
        }
        return random.next_float() < corner_hole_chance_;
    }

private:
    /// The row, then a walk around its rim: under every leaf the row put on
    /// the rim, a leaf may hang, and under that one a second.
    ///
    /// The walk goes edge by edge, one per horizontal direction in the order
    /// north, east, south, west, each edge along its direction and starting
    /// from the corner the clockwise neighbour of that direction points at.
    /// That order is the order of the draws.
    void place_row_with_hanging_leaves(const TreeWorld& world, TreeWriter& writer,
                                       FeatureRandom& random, const TreeConfig& config,
                                       BlockPos pos, i32 range, i32 local_y, bool large) const {
        place_leaves_row(world, writer, random, config, pos, range, local_y, large);
        const i32 extra = large ? 1 : 0;
        for (usize index = 0; index < kHorizontal.size(); ++index) {
            const auto direction = kHorizontal[index];
            const auto clockwise = kHorizontal[(index + 1) % kHorizontal.size()];
            const bool positive  = clockwise[0] > 0 || clockwise[1] > 0;
            const i32  reach     = positive ? range + extra : range;
            BlockPos   cursor    = pos.offset(clockwise[0] * reach - direction[0] * range,
                                              local_y - 1,
                                              clockwise[1] * reach - direction[1] * range);
            for (i32 k = -range; k < range + extra; ++k) {
                if (writer.foliage_set(cursor.above()) &&
                    try_extension(world, writer, random, config, hanging_leaves_chance_, pos,
                                  cursor)) {
                    (void)try_extension(world, writer, random, config,
                                        hanging_leaves_extension_chance_, pos, cursor.below());
                }
                cursor = cursor.offset(direction[0], 0, direction[1]);
            }
        }
    }

    /// Too far from the centre is refused before anything is drawn; otherwise
    /// one float decides.
    static bool try_extension(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
                              const TreeConfig& config, f32 chance, BlockPos centre,
                              BlockPos at) {
        const i32 distance =
            std::abs(at.x - centre.x) + std::abs(at.y - centre.y) + std::abs(at.z - centre.z);
        if (distance >= 7) {
            return false;
        }
        const f32 roll = random.next_float();
        if (roll > chance) {
            return false;
        }
        if (!world.valid_tree_pos(at)) {
            return false;
        }
        try_place_leaf(world, writer, random, config, at);
        return true;
    }

public:

private:
    IntProviderRef height_;
    f32            wide_bottom_layer_hole_chance_;
    f32            corner_hole_chance_;
    f32            hanging_leaves_chance_;
    f32            hanging_leaves_extension_chance_;
};

// ── Root placers ────────────────────────────────────────────────────────────

class MangroveRootPlacer final : public RootPlacer {
public:
    MangroveRootPlacer(IntProviderRef trunk_offset_y, StateProviderRef root_provider,
                       StateProviderRef above_root_provider, f32 above_root_chance,
                       std::vector<u16> can_grow_through, std::vector<u16> muddy_roots_in,
                       StateProviderRef muddy_roots_provider, i32 max_root_width,
                       i32 max_root_length, f32 random_skew_chance)
        : trunk_offset_y_(std::move(trunk_offset_y)),
          root_provider_(std::move(root_provider)),
          above_root_provider_(std::move(above_root_provider)),
          above_root_chance_(above_root_chance),
          can_grow_through_(std::move(can_grow_through)),
          muddy_roots_in_(std::move(muddy_roots_in)),
          muddy_roots_provider_(std::move(muddy_roots_provider)),
          max_root_width_(max_root_width),
          max_root_length_(max_root_length),
          random_skew_chance_(random_skew_chance) {}

    [[nodiscard]] std::string_view type_name() const override { return "mangrove_root_placer"; }

    [[nodiscard]] BlockPos trunk_origin(BlockPos at, FeatureRandom& random) const override {
        return at.above(trunk_offset_y_->sample(random));
    }

    /// The column under the trunk must be clear, then four roots are
    /// *simulated* — one per horizontal side, walked down until they meet
    /// something they cannot grow through — and only if all four end in
    /// ground within the length limit is anything written. A root that is
    /// still in the air at its fifteenth step fails the whole tree.
    [[nodiscard]] bool place_roots(const TreeWorld& world, TreeWriter& writer,
                                   FeatureRandom& random, BlockPos at, BlockPos trunk,
                                   const TreeConfig&) const override {
        BlockPos cursor = at;
        while (cursor.y < trunk.y) {
            if (!can_place(world, cursor)) {
                return false;
            }
            cursor = cursor.above();
        }

        std::vector<BlockPos> roots;
        roots.push_back(trunk.below());
        for (const auto& side : kHorizontal) {
            const BlockPos        start = trunk.offset(side[0], 0, side[1]);
            std::vector<BlockPos> branch;
            if (!simulate_roots(world, random, start, side, trunk, branch, 0)) {
                return false;
            }
            roots.insert(roots.end(), branch.begin(), branch.end());
            roots.push_back(start);
        }

        for (const BlockPos& root : roots) {
            place_root(world, writer, random, root);
        }
        return true;
    }

private:
    [[nodiscard]] bool can_place(const TreeWorld& world, BlockPos at) const {
        return world.valid_tree_pos(at) || TreeTags::holds(can_grow_through_, world.block_at(at));
    }

    [[nodiscard]] bool simulate_roots(const TreeWorld& world, FeatureRandom& random, BlockPos at,
                                      std::array<i32, 2> side, BlockPos trunk,
                                      std::vector<BlockPos>& out, i32 length) const {
        if (length == max_root_length_ || static_cast<i32>(out.size()) > max_root_length_) {
            return false;
        }
        std::array<BlockPos, 2> next{};
        const usize             count = potential_roots(random, at, side, trunk, next);
        for (usize index = 0; index < count; ++index) {
            const BlockPos candidate = next[index];
            if (!can_place(world, candidate)) {
                continue;
            }
            out.push_back(candidate);
            if (!simulate_roots(world, random, candidate, side, trunk, out, length + 1)) {
                return false;
            }
        }
        return true;
    }

    /// Where a root may go from here: straight down, or one step out along
    /// its side. Near the rim of the allowed width it may fork down both
    /// ways; past it, it only falls. The draws depend on the distance.
    [[nodiscard]] usize potential_roots(FeatureRandom& random, BlockPos at,
                                        std::array<i32, 2> side, BlockPos trunk,
                                        std::array<BlockPos, 2>& out) const {
        const BlockPos below    = at.below();
        const BlockPos outward  = at.offset(side[0], 0, side[1]);
        const i32      distance = std::abs(at.x - trunk.x) + std::abs(at.y - trunk.y) +
                             std::abs(at.z - trunk.z);
        if (distance > max_root_width_ - 3 && distance <= max_root_width_) {
            const f32 roll = random.next_float();
            out[0]         = below;
            if (roll < random_skew_chance_) {
                out[1] = outward.below();
                return 2;
            }
            return 1;
        }
        if (distance > max_root_width_) {
            out[0] = below;
            return 1;
        }
        const f32 roll = random.next_float();
        if (roll < random_skew_chance_) {
            out[0] = below;
            return 1;
        }
        out[0] = random.next_boolean() ? outward : below;
        return 1;
    }

    /// Mud turns into muddy roots; anything else a root can take gets a
    /// root, and maybe a carpet of moss on top.
    void place_root(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
                    BlockPos at) const {
        if (TreeTags::holds(muddy_roots_in_, world.block_at(at))) {
            writer.set_root(at, waterlogged(world, at,
                                            muddy_roots_provider_->state(*world.level, random, at)));
            return;
        }
        if (!can_place(world, at)) {
            return;
        }
        writer.set_root(at, waterlogged(world, at, root_provider_->state(*world.level, random, at)));
        const f32      roll  = random.next_float();
        const BlockPos above = at.above();
        if (roll < above_root_chance_ && world.is_air(above)) {
            writer.set_root(above, waterlogged(world, above, above_root_provider_->state(
                                                                 *world.level, random, above)));
        }
    }

    /// `waterlogged` follows the water that was there, source or flowing.
    [[nodiscard]] static registry::BlockStateId waterlogged(const TreeWorld& world, BlockPos at,
                                                            registry::BlockStateId state) {
        if (!has_property(*world.blocks, state, "waterlogged")) {
            return state;
        }
        const auto here  = world.level->block_at(at.x, at.y, at.z);
        const auto block = world.blocks->block_of(here);
        bool       water = block == world.tags->water;
        if (!water) {
            if (const auto property = world.blocks->find_property(block, "waterlogged")) {
                water = world.blocks->property_value(here, *property) == "true";
            }
        }
        return try_set_property(*world.blocks, state, "waterlogged", water ? "true" : "false");
    }

    IntProviderRef   trunk_offset_y_;
    StateProviderRef root_provider_;
    StateProviderRef above_root_provider_;
    f32              above_root_chance_;
    std::vector<u16> can_grow_through_;
    std::vector<u16> muddy_roots_in_;
    StateProviderRef muddy_roots_provider_;
    i32              max_root_width_;
    i32              max_root_length_;
    f32              random_skew_chance_;
};

// ── Tree decorators ─────────────────────────────────────────────────────────

class TrunkVineDecorator final : public TreeDecorator {
public:
    explicit TrunkVineDecorator(registry::BlockId vine) : vine_(vine) {}

    [[nodiscard]] std::string_view type_name() const override { return "trunk_vine"; }

    void decorate(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
                  const std::vector<BlockPos>& logs, const std::vector<BlockPos>&,
                  const std::vector<BlockPos>&) const override {
        for (const BlockPos& log : logs) {
            for (const auto& [side, property] : kSides) {
                const i32 roll = random.next_int(3);
                if (roll <= 0) {
                    continue;
                }
                const BlockPos at = log.offset(side[0], 0, side[1]);
                if (!world.is_air(at)) {
                    continue;
                }
                auto state = world.blocks->default_state(vine_);
                state      = try_set_property(*world.blocks, state, property, "true");
                writer.set_decoration(at, state);
            }
        }
    }

private:
    /// West, east, north, south — the order `TrunkVineDecorator` writes them —
    /// with the vine property that faces *back* at the log.
    static constexpr std::array<std::pair<std::array<i32, 2>, std::string_view>, 4> kSides{{
        {{-1, 0}, "east"},
        {{1, 0}, "west"},
        {{0, -1}, "south"},
        {{0, 1}, "north"},
    }};

    registry::BlockId vine_;
};

class LeaveVineDecorator final : public TreeDecorator {
public:
    LeaveVineDecorator(registry::BlockId vine, f32 probability)
        : vine_(vine), probability_(probability) {}

    [[nodiscard]] std::string_view type_name() const override { return "leave_vine"; }

    void decorate(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
                  const std::vector<BlockPos>&, const std::vector<BlockPos>& leaves,
                  const std::vector<BlockPos>&) const override {
        for (const BlockPos& leaf : leaves) {
            for (const auto& [side, property] : kSides) {
                const f32 roll = random.next_float();
                if (roll >= probability_) {
                    continue;
                }
                const BlockPos at = leaf.offset(side[0], 0, side[1]);
                if (!world.is_air(at)) {
                    continue;
                }
                add_hanging_vine(world, writer, at, property);
            }
        }
    }

private:
    void add_hanging_vine(const TreeWorld& world, TreeWriter& writer, BlockPos at,
                          std::string_view property) const {
        auto state = world.blocks->default_state(vine_);
        state      = try_set_property(*world.blocks, state, property, "true");
        BlockPos cursor = at;
        for (i32 length = 4; length > 0; --length) {
            if (!world.is_air(cursor)) {
                return;
            }
            writer.set_decoration(cursor, state);
            cursor = cursor.below();
        }
    }

    static constexpr std::array<std::pair<std::array<i32, 2>, std::string_view>, 4> kSides{{
        {{-1, 0}, "east"},
        {{1, 0}, "west"},
        {{0, -1}, "south"},
        {{0, 1}, "north"},
    }};

    registry::BlockId vine_;
    f32               probability_;
};

class CocoaDecorator final : public TreeDecorator {
public:
    CocoaDecorator(registry::BlockId cocoa, f32 probability)
        : cocoa_(cocoa), probability_(probability) {}

    [[nodiscard]] std::string_view type_name() const override { return "cocoa"; }

    void decorate(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
                  const std::vector<BlockPos>& logs, const std::vector<BlockPos>&,
                  const std::vector<BlockPos>&) const override {
        const f32 gate = random.next_float();
        if (gate >= probability_ || logs.empty()) {
            return;
        }
        const i32 base = logs.front().y;
        for (const BlockPos& log : logs) {
            if (log.y - base > 2) {
                continue;
            }
            for (const auto& [side, facing] : kSides) {
                const f32 roll = random.next_float();
                if (roll >= 0.2F) {
                    continue;
                }
                const BlockPos at = log.offset(side[0], 0, side[1]);
                if (!world.is_air(at)) {
                    continue;
                }
                const i32 age   = random.next_int(3);
                auto      state = world.blocks->default_state(cocoa_);
                state           = try_set_property(*world.blocks, state, "age",
                                                   age == 0   ? "0"
                                                   : age == 1 ? "1"
                                                              : "2");
                state = try_set_property(*world.blocks, state, "facing", facing);
                writer.set_decoration(at, state);
            }
        }
    }

private:
    static constexpr std::array<std::pair<std::array<i32, 2>, std::string_view>, 4> kSides{{
        {{0, -1}, "north"},
        {{0, 1}, "south"},
        {{-1, 0}, "west"},
        {{1, 0}, "east"},
    }};

    registry::BlockId cocoa_;
    f32               probability_;
};

class BeehiveDecorator final : public TreeDecorator {
public:
    BeehiveDecorator(registry::BlockId hive, f32 probability)
        : hive_(hive), probability_(probability) {}

    [[nodiscard]] std::string_view type_name() const override { return "beehive"; }

    void decorate(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
                  const std::vector<BlockPos>& logs, const std::vector<BlockPos>& leaves,
                  const std::vector<BlockPos>&) const override {
        const f32 gate = random.next_float();
        if (gate >= probability_ || logs.empty()) {
            return;
        }
        i32 target = 0;
        if (!leaves.empty()) {
            target = std::max(leaves.front().y - 1, logs.front().y + 1);
        } else {
            const i32 lift = random.next_int(3);
            target         = std::min(logs.front().y + 1 + lift, logs.back().y);
        }

        std::vector<BlockPos> candidates;
        for (const BlockPos& log : logs) {
            if (log.y != target) {
                continue;
            }
            for (const auto& side : kSpawnSides) {
                candidates.push_back(log.offset(side[0], 0, side[1]));
            }
        }
        if (candidates.empty()) {
            return;
        }
        java_shuffle(candidates, random);
        for (const BlockPos& candidate : candidates) {
            if (!world.is_air(candidate)) {
                continue;
            }
            // The hive faces the empty side it was reached from; vanilla only
            // ever writes a north-facing one because it re-checks the south
            // neighbour is a log.
            const BlockPos behind = candidate.offset(0, 0, 1);
            if (!TreeTags::holds(world.tags->logs, world.block_at(behind))) {
                continue;
            }
            auto state = world.blocks->default_state(hive_);
            state      = try_set_property(*world.blocks, state, "facing", "north");
            writer.set_decoration(candidate, state);
            return;
        }
    }

private:
    /// The four sides a hive may be reached from, in the order vanilla lists
    /// them. Only the north one ever survives the log test below, but the list
    /// order still decides what the shuffle sees.
    static constexpr std::array<std::array<i32, 2>, 4> kSpawnSides{{
        {0, -1},
        {0, 1},
        {-1, 0},
        {1, 0},
    }};

    registry::BlockId hive_;
    f32               probability_;
};

class AlterGroundDecorator final : public TreeDecorator {
public:
    explicit AlterGroundDecorator(StateProviderRef provider) : provider_(std::move(provider)) {}

    [[nodiscard]] std::string_view type_name() const override { return "alter_ground"; }

    void decorate(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
                  const std::vector<BlockPos>& logs, const std::vector<BlockPos>&,
                  const std::vector<BlockPos>& roots) const override {
        std::vector<BlockPos> anchors;
        if (roots.empty()) {
            anchors = logs;
        } else if (!logs.empty() && roots.front().y == logs.front().y) {
            anchors = logs;
            anchors.insert(anchors.end(), roots.begin(), roots.end());
        } else {
            anchors = roots;
        }
        if (anchors.empty()) {
            return;
        }
        const i32 base = anchors.front().y;
        for (const BlockPos& anchor : anchors) {
            if (anchor.y != base) {
                continue;
            }
            place_circle(world, writer, random, anchor.offset(-1, 0, -1));
            place_circle(world, writer, random, anchor.offset(2, 0, -1));
            place_circle(world, writer, random, anchor.offset(-1, 0, 2));
            place_circle(world, writer, random, anchor.offset(2, 0, 2));
            for (i32 i = 0; i < 5; ++i) {
                const i32 dx = random.next_int(64);
                const i32 dz = random.next_int(64);
                const i32 x  = dx % 8;
                const i32 z  = dz % 8;
                if (x + z > 8) {
                    continue;
                }
                place_circle(world, writer, random, anchor.offset(-3 + x, 0, -3 + z));
            }
        }
    }

private:
    void place_circle(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
                      BlockPos centre) const {
        for (i32 dx = -2; dx <= 2; ++dx) {
            for (i32 dz = -2; dz <= 2; ++dz) {
                if (std::abs(dx) == 2 && std::abs(dz) == 2) {
                    continue;
                }
                place_block(world, writer, random, centre.offset(dx, 0, dz));
            }
        }
    }

    void place_block(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
                     BlockPos at) const {
        for (i32 dy = 2; dy >= -3; --dy) {
            const BlockPos here = at.above(dy);
            if (world.is_dirt(here)) {
                writer.set_decoration(here, provider_->state(*world.level, random, here));
                break;
            }
            if (!world.is_air(here) && dy < 0) {
                break;
            }
        }
    }

    StateProviderRef provider_;
};

class AttachedToLeavesDecorator final : public TreeDecorator {
public:
    AttachedToLeavesDecorator(f32 probability, i32 exclusion_radius_xz, i32 exclusion_radius_y,
                              StateProviderRef provider, i32 required_empty_blocks,
                              std::vector<std::array<i32, 3>> directions)
        : probability_(probability),
          exclusion_radius_xz_(exclusion_radius_xz),
          exclusion_radius_y_(exclusion_radius_y),
          provider_(std::move(provider)),
          required_empty_blocks_(required_empty_blocks),
          directions_(std::move(directions)) {}

    [[nodiscard]] std::string_view type_name() const override { return "attached_to_leaves"; }

    void decorate(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
                  const std::vector<BlockPos>&, const std::vector<BlockPos>& leaves,
                  const std::vector<BlockPos>&) const override {
        std::vector<BlockPos> taken;
        for (const BlockPos& leaf : leaves) {
            const f32 roll = random.next_float();
            if (roll >= probability_) {
                continue;
            }
            const auto direction = pick_direction(random);
            const BlockPos at    = leaf.offset(direction[0], direction[1], direction[2]);
            if (!has_room(world, at, direction) || excluded(taken, at)) {
                continue;
            }
            writer.set_decoration(at, provider_->state(*world.level, random, at));
            taken.push_back(at);
        }
    }

private:
    [[nodiscard]] std::array<i32, 3> pick_direction(FeatureRandom& random) const {
        if (directions_.size() == 1) {
            return directions_.front();
        }
        return directions_[static_cast<usize>(
            random.next_int(static_cast<i32>(directions_.size())))];
    }

    [[nodiscard]] bool has_room(const TreeWorld& world, BlockPos at,
                                std::array<i32, 3> direction) const {
        for (i32 step = 0; step < required_empty_blocks_; ++step) {
            const BlockPos here =
                at.offset(direction[0] * step, direction[1] * step, direction[2] * step);
            if (!world.is_air(here)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool excluded(const std::vector<BlockPos>& taken, BlockPos at) const {
        return std::ranges::any_of(taken, [&](const BlockPos& other) {
            return std::abs(other.x - at.x) <= exclusion_radius_xz_ &&
                   std::abs(other.z - at.z) <= exclusion_radius_xz_ &&
                   std::abs(other.y - at.y) <= exclusion_radius_y_;
        });
    }

    f32                            probability_;
    i32                            exclusion_radius_xz_;
    i32                            exclusion_radius_y_;
    StateProviderRef               provider_;
    i32                            required_empty_blocks_;
    std::vector<std::array<i32, 3>> directions_;
};

// ── The feature ─────────────────────────────────────────────────────────────

class TreeFeature final : public Feature {
public:
    explicit TreeFeature(TreeConfig config) : config_(std::move(config)) {}

    [[nodiscard]] std::string_view type_name() const override { return "tree"; }

    bool place(const FeatureContext& context, FeatureLevel& level, FeatureRandom& random,
               BlockPos at) const override {
        TreeWorld world;
        world.blocks = context.blocks;
        world.tags   = config_.tags.get();
        world.level  = &level;

        TreeWriter writer(level, *context.blocks);

        // 1. how tall, 2. how much of it is leaves, 3. how wide the leaves are.
        // Each one draws, and the order is fixed.
        const i32 height         = config_.trunk_placer->tree_height(random);
        const i32 foliage_height = config_.foliage_placer->foliage_height(random, height, config_);
        const i32 trunk_height   = height - foliage_height;
        const i32 foliage_radius = config_.foliage_placer->foliage_radius(random, trunk_height);

        const BlockPos trunk_origin =
            config_.root_placer ? config_.root_placer->trunk_origin(at, random) : at;

        const i32 lowest  = std::min(at.y, trunk_origin.y);
        const i32 highest = std::max(at.y, trunk_origin.y) + height + 1;
        if (lowest < level.min_y() + 1 || highest > level.max_y() + 1) {
            return false;
        }

        const i32  free    = free_height(world, height, trunk_origin);
        const auto clipped = config_.minimum_size->min_clipped_height();
        if (free < height && !(clipped && free >= *clipped)) {
            return false;
        }
        if (config_.root_placer &&
            !config_.root_placer->place_roots(world, writer, random, at, trunk_origin, config_)) {
            return false;
        }

        std::vector<FoliageAttachment> attachments;
        config_.trunk_placer->place_trunk(world, writer, random, free, trunk_origin, config_,
                                          attachments);
        for (const FoliageAttachment& attachment : attachments) {
            config_.foliage_placer->create_foliage(world, writer, random, config_, free,
                                                   attachment, foliage_height, foliage_radius);
        }

        if (writer.empty()) {
            return false;
        }
        if (!config_.decorators.empty()) {
            const auto logs   = writer.sorted_logs();
            const auto leaves = writer.sorted_foliage();
            const auto roots  = writer.sorted_roots();
            for (const TreeDecoratorRef& decorator : config_.decorators) {
                decorator->decorate(world, writer, random, logs, leaves, roots);
            }
        }
        update_leaf_distances(world, level, writer);
        return true;
    }

private:
    /// The last pass the game makes over a finished tree: every leaf in the
    /// tree's box gets its `distance` from the nearest `#logs` block, through
    /// leaves, up to six. It draws nothing and so is not part of the tree's
    /// shape — features.md compares by block for that reason — but without it
    /// every generated leaf carries the provider's `distance=7`, and the first
    /// random tick decays whole forests.
    ///
    /// Measured on the real game: the reference world's generated leaves carry
    /// distances 1..6 (docs/provenance/agriculture.md § arbres), and a leaf's
    /// distance is exactly the steps through leaves from a log (288 / 288
    /// cells of a measured sheet). A leaf the pass does not reach in six steps
    /// keeps its 7, as in the game.
    static void update_leaf_distances(const TreeWorld& world, FeatureLevel& level,
                                      const TreeWriter& writer) {
        const auto logs    = writer.sorted_logs();
        const auto foliage = writer.sorted_foliage();
        const auto roots   = writer.sorted_roots();
        if (foliage.empty()) {
            return;
        }
        BlockPos lo = foliage.front();
        BlockPos hi = foliage.front();
        const auto grow = [&](const std::vector<BlockPos>& set) {
            for (const BlockPos& p : set) {
                lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
            }
        };
        grow(logs);
        grow(foliage);
        grow(roots);

        const i32 sx = hi.x - lo.x + 1;
        const i32 sy = hi.y - lo.y + 1;
        const i32 sz = hi.z - lo.z + 1;
        const auto index = [&](BlockPos p) {
            return static_cast<usize>(((p.y - lo.y) * sz + (p.z - lo.z)) * sx + (p.x - lo.x));
        };
        const auto inside = [&](BlockPos p) {
            return p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y && p.z >= lo.z &&
                   p.z <= hi.z;
        };

        const registry::BlockRegistry& blocks = *world.blocks;
        std::vector<u8>       seen(static_cast<usize>(sx) * static_cast<usize>(sy) * static_cast<usize>(sz), 0);
        std::vector<BlockPos> frontier;
        for (i32 y = lo.y; y <= hi.y; ++y) {
            for (i32 z = lo.z; z <= hi.z; ++z) {
                for (i32 x = lo.x; x <= hi.x; ++x) {
                    if (TreeTags::holds(world.tags->logs, blocks.block_of(level.block_at(x, y, z)))) {
                        seen[index({x, y, z})] = 1;
                        frontier.push_back({x, y, z});
                    }
                }
            }
        }

        constexpr std::array<std::array<i32, 3>, 6> kSix{{
            {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
        }};
        std::vector<BlockPos> next;
        for (i32 distance = 1; distance <= 6 && !frontier.empty(); ++distance) {
            next.clear();
            for (const BlockPos& from : frontier) {
                for (const auto& d : kSix) {
                    const BlockPos at{from.x + d[0], from.y + d[1], from.z + d[2]};
                    if (!inside(at) || seen[index(at)] != 0) {
                        continue;
                    }
                    const registry::BlockStateId state = level.block_at(at.x, at.y, at.z);
                    if (!TreeTags::holds(world.tags->leaves, blocks.block_of(state))) {
                        continue;
                    }
                    seen[index(at)] = 1;
                    const auto property = blocks.find_property(blocks.block_of(state), "distance");
                    if (property) {
                        // Values run "1".."7", so the index is distance - 1.
                        (void)level.set_block(at.x, at.y, at.z,
                                              blocks.with_property(state, *property,
                                                                   static_cast<u16>(distance - 1)));
                    }
                    next.push_back(at);
                }
            }
            frontier.swap(next);
        }
    }

    /// How much of the asked-for height is actually clear, checked as a column
    /// whose width the feature size decides.
    [[nodiscard]] i32 free_height(const TreeWorld& world, i32 height, BlockPos at) const {
        for (i32 y = 0; y <= height + 1; ++y) {
            const i32 half = config_.minimum_size->size_at_height(height, y);
            for (i32 dx = -half; dx <= half; ++dx) {
                for (i32 dz = -half; dz <= half; ++dz) {
                    const BlockPos here = at.offset(dx, y, dz);
                    if (!world.is_free(here) ||
                        (!config_.ignore_vines && world.is_vine(here))) {
                        return y - 2;
                    }
                }
            }
        }
        return height;
    }

    TreeConfig config_;
};

}  // namespace

FeatureRef make_tree_feature(TreeConfig config) {
    return std::static_pointer_cast<const Feature>(
        std::make_shared<const TreeFeature>(std::move(config)));
}


// ── Reading a tree out of the datapack ──────────────────────────────────────

namespace {

/// The blocks a `#tag` or a list of names resolves to, sorted.
///
/// A tag that was never exported is a failure and not an empty set: an empty
/// "can grow through" turns a mangrove into a tree that cannot grow.
[[nodiscard]] std::expected<std::vector<u16>, FeatureError> parse_block_set(
    Json node, const registry::BlockRegistry& blocks, const BlockTags& tags) {
    std::vector<u16> out;
    std::string_view text;
    if (node.get(text) == simdjson::SUCCESS) {
        if (text.starts_with('#')) {
            const std::string tag = qualify(text.substr(1));
            if (!tags.known(tag)) {
                OV_LOG_ERROR("worldgen: a tree names tag {}, which was not exported", tag);
                return std::unexpected(FeatureError::Missing);
            }
            for (usize index = 0; index < blocks.block_count(); ++index) {
                const registry::BlockId block{static_cast<u16>(index)};
                if (tags.contains(tag, block)) {
                    out.push_back(block.value());
                }
            }
            std::ranges::sort(out);
            return out;
        }
        const auto block = blocks.find_block(qualify(text));
        if (!block) {
            return std::unexpected(FeatureError::Malformed);
        }
        out.push_back(block->value());
        return out;
    }

    simdjson::dom::array list;
    if (node.get(list) != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    for (auto entry : list) {
        std::string_view name;
        if (entry.get(name) != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        const auto block = blocks.find_block(qualify(name));
        if (!block) {
            return std::unexpected(FeatureError::Malformed);
        }
        out.push_back(block->value());
    }
    std::ranges::sort(out);
    return out;
}

[[nodiscard]] std::optional<i64> optional_int(Json node, std::string_view key) {
    i64 value = 0;
    if (node.at_key(key).get(value) == simdjson::SUCCESS) {
        return value;
    }
    return std::nullopt;
}

[[nodiscard]] f32 optional_float(Json node, std::string_view key, f32 fallback) {
    f64 value = 0.0;
    if (node.at_key(key).get(value) == simdjson::SUCCESS) {
        return static_cast<f32>(value);
    }
    return fallback;
}

/// A constant int provider, for the fields spelled as a bare number.
///
/// It does **not** draw. That is the point: a foliage placer's `radius` is an
/// `IntProvider` in the format and a plain integer in every vanilla file, and
/// giving it a draw would shift the whole tree.
class ConstantInt final : public IntProvider {
public:
    explicit ConstantInt(i32 value) : value_(value) {}
    [[nodiscard]] i32 sample(FeatureRandom&) const override { return value_; }

private:
    i32 value_;
};

/// A uniform range written without a `type` — the shape
/// `branch_start_offset_from_top` uses. Rebuilt as the uniform provider so the
/// draw is the same one.
class UniformRange final : public IntProvider {
public:
    UniformRange(i32 low, i32 high) : low_(low), high_(high) {}
    [[nodiscard]] i32 sample(FeatureRandom& random) const override {
        return low_ + random.next_int(high_ - low_ + 1);
    }

private:
    i32 low_;
    i32 high_;
};

[[nodiscard]] std::expected<IntProviderRef, FeatureError> parse_int_provider_or_range(Json node) {
    std::string_view type;
    if (node.at_key("type").get(type) == simdjson::SUCCESS) {
        return parse_int_provider(node);
    }
    i64 low  = 0;
    i64 high = 0;
    if (node.at_key("min_inclusive").get(low) != simdjson::SUCCESS ||
        node.at_key("max_inclusive").get(high) != simdjson::SUCCESS) {
        return parse_int_provider(node);
    }
    return std::static_pointer_cast<const IntProvider>(
        std::make_shared<const UniformRange>(static_cast<i32>(low), static_cast<i32>(high)));
}

[[nodiscard]] std::expected<IntProviderRef, FeatureError> parse_int_field(Json node,
                                                                          std::string_view key) {
    auto field = node.at_key(key);
    if (field.error() != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    i64 value = 0;
    if (field.get(value) == simdjson::SUCCESS) {
        return std::static_pointer_cast<const IntProvider>(
            std::make_shared<const ConstantInt>(static_cast<i32>(value)));
    }
    return parse_int_provider_or_range(field.value());
}

[[nodiscard]] std::expected<TrunkPlacerRef, FeatureError> parse_trunk_placer(
    Json node, const registry::BlockRegistry& blocks, const BlockTags& tags) {
    std::string_view type;
    if (node.at_key("type").get(type) != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    const std::string kind = strip_namespace(type);

    i64 base = 0;
    i64 a    = 0;
    i64 b    = 0;
    if (node.at_key("base_height").get(base) != simdjson::SUCCESS ||
        node.at_key("height_rand_a").get(a) != simdjson::SUCCESS ||
        node.at_key("height_rand_b").get(b) != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    const auto base_i = static_cast<i32>(base);
    const auto a_i    = static_cast<i32>(a);
    const auto b_i    = static_cast<i32>(b);

    if (kind == "straight_trunk_placer") {
        return std::static_pointer_cast<const TrunkPlacer>(
            std::make_shared<const StraightTrunkPlacer>(base_i, a_i, b_i));
    }
    if (kind == "forking_trunk_placer") {
        return std::static_pointer_cast<const TrunkPlacer>(
            std::make_shared<const ForkingTrunkPlacer>(base_i, a_i, b_i));
    }
    if (kind == "giant_trunk_placer") {
        return std::static_pointer_cast<const TrunkPlacer>(
            std::make_shared<const GiantTrunkPlacer>(base_i, a_i, b_i));
    }
    if (kind == "mega_jungle_trunk_placer") {
        return std::static_pointer_cast<const TrunkPlacer>(
            std::make_shared<const MegaJungleTrunkPlacer>(base_i, a_i, b_i));
    }
    if (kind == "dark_oak_trunk_placer") {
        return std::static_pointer_cast<const TrunkPlacer>(
            std::make_shared<const DarkOakTrunkPlacer>(base_i, a_i, b_i));
    }
    if (kind == "fancy_trunk_placer") {
        return std::static_pointer_cast<const TrunkPlacer>(
            std::make_shared<const FancyTrunkPlacer>(base_i, a_i, b_i));
    }
    if (kind == "bending_trunk_placer") {
        const auto min_height = optional_int(node, "min_height_for_leaves").value_or(1);
        auto       bend       = parse_int_field(node, "bend_length");
        if (!bend) {
            return std::unexpected(bend.error());
        }
        return std::static_pointer_cast<const TrunkPlacer>(
            std::make_shared<const BendingTrunkPlacer>(base_i, a_i, b_i,
                                                       static_cast<i32>(min_height), *bend));
    }
    if (kind == "upwards_branching_trunk_placer") {
        auto steps = parse_int_field(node, "extra_branch_steps");
        if (!steps) return std::unexpected(steps.error());
        auto length = parse_int_field(node, "extra_branch_length");
        if (!length) return std::unexpected(length.error());
        auto through = node.at_key("can_grow_through");
        if (through.error() != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        auto set = parse_block_set(through.value(), blocks, tags);
        if (!set) return std::unexpected(set.error());
        const f32 probability = optional_float(node, "place_branch_per_log_probability", 0.0F);
        return std::static_pointer_cast<const TrunkPlacer>(
            std::make_shared<const UpwardsBranchingTrunkPlacer>(
                base_i, a_i, b_i, *steps, probability, *length, std::move(*set)));
    }
    if (kind == "cherry_trunk_placer") {
        auto count = parse_int_field(node, "branch_count");
        if (!count) return std::unexpected(count.error());
        auto horizontal = parse_int_field(node, "branch_horizontal_length");
        if (!horizontal) return std::unexpected(horizontal.error());
        auto end = parse_int_field(node, "branch_end_offset_from_top");
        if (!end) return std::unexpected(end.error());
        // The start is a bare uniform range, and its bounds are needed as
        // numbers: the second branch draws from the same range with the top
        // lowered by one.
        auto start = node.at_key("branch_start_offset_from_top");
        i64  low   = 0;
        i64  high  = 0;
        if (start.error() != simdjson::SUCCESS ||
            start.at_key("min_inclusive").get(low) != simdjson::SUCCESS ||
            start.at_key("max_inclusive").get(high) != simdjson::SUCCESS || high <= low) {
            return std::unexpected(FeatureError::Malformed);
        }
        return std::static_pointer_cast<const TrunkPlacer>(std::make_shared<const CherryTrunkPlacer>(
            base_i, a_i, b_i, *count, *horizontal, static_cast<i32>(low), static_cast<i32>(high),
            *end));
    }

    OV_LOG_ERROR("worldgen: trunk placer '{}' is not implemented", kind);
    return std::unexpected(FeatureError::Unsupported);
}

[[nodiscard]] std::expected<FoliagePlacerRef, FeatureError> parse_foliage_placer(Json node) {
    std::string_view type;
    if (node.at_key("type").get(type) != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    const std::string kind = strip_namespace(type);

    auto radius = parse_int_field(node, "radius");
    if (!radius) return std::unexpected(radius.error());
    auto offset = parse_int_field(node, "offset");
    if (!offset) return std::unexpected(offset.error());

    const auto height_int = optional_int(node, "height");

    if (kind == "blob_foliage_placer" || kind == "fancy_foliage_placer" ||
        kind == "bush_foliage_placer" || kind == "jungle_foliage_placer") {
        if (!height_int) {
            return std::unexpected(FeatureError::Malformed);
        }
        const auto height = static_cast<i32>(*height_int);
        if (kind == "blob_foliage_placer") {
            return std::static_pointer_cast<const FoliagePlacer>(
                std::make_shared<const BlobFoliagePlacer>(*radius, *offset, height));
        }
        if (kind == "fancy_foliage_placer") {
            return std::static_pointer_cast<const FoliagePlacer>(
                std::make_shared<const FancyFoliagePlacer>(*radius, *offset, height));
        }
        if (kind == "bush_foliage_placer") {
            return std::static_pointer_cast<const FoliagePlacer>(
                std::make_shared<const BushFoliagePlacer>(*radius, *offset, height));
        }
        return std::static_pointer_cast<const FoliagePlacer>(
            std::make_shared<const JungleFoliagePlacer>(*radius, *offset, height));
    }
    if (kind == "acacia_foliage_placer") {
        return std::static_pointer_cast<const FoliagePlacer>(
            std::make_shared<const AcaciaFoliagePlacer>(*radius, *offset));
    }
    if (kind == "dark_oak_foliage_placer") {
        return std::static_pointer_cast<const FoliagePlacer>(
            std::make_shared<const DarkOakFoliagePlacer>(*radius, *offset));
    }
    if (kind == "spruce_foliage_placer") {
        auto trunk = parse_int_field(node, "trunk_height");
        if (!trunk) return std::unexpected(trunk.error());
        return std::static_pointer_cast<const FoliagePlacer>(
            std::make_shared<const SpruceFoliagePlacer>(*radius, *offset, *trunk));
    }
    if (kind == "pine_foliage_placer") {
        auto height = parse_int_field(node, "height");
        if (!height) return std::unexpected(height.error());
        return std::static_pointer_cast<const FoliagePlacer>(
            std::make_shared<const PineFoliagePlacer>(*radius, *offset, *height));
    }
    if (kind == "mega_pine_foliage_placer") {
        auto crown = parse_int_field(node, "crown_height");
        if (!crown) return std::unexpected(crown.error());
        return std::static_pointer_cast<const FoliagePlacer>(
            std::make_shared<const MegaPineFoliagePlacer>(*radius, *offset, *crown));
    }
    if (kind == "random_spread_foliage_placer") {
        auto height = parse_int_field(node, "foliage_height");
        if (!height) return std::unexpected(height.error());
        const auto attempts = optional_int(node, "leaf_placement_attempts");
        if (!attempts) {
            return std::unexpected(FeatureError::Malformed);
        }
        return std::static_pointer_cast<const FoliagePlacer>(
            std::make_shared<const RandomSpreadFoliagePlacer>(*radius, *offset, *height,
                                                              static_cast<i32>(*attempts)));
    }
    if (kind == "cherry_foliage_placer") {
        auto height = parse_int_field(node, "height");
        if (!height) return std::unexpected(height.error());
        return std::static_pointer_cast<const FoliagePlacer>(
            std::make_shared<const CherryFoliagePlacer>(
                *radius, *offset, *height,
                optional_float(node, "wide_bottom_layer_hole_chance", 0.0F),
                optional_float(node, "corner_hole_chance", 0.0F),
                optional_float(node, "hanging_leaves_chance", 0.0F),
                optional_float(node, "hanging_leaves_extension_chance", 0.0F)));
    }

    OV_LOG_ERROR("worldgen: foliage placer '{}' is not implemented", kind);
    return std::unexpected(FeatureError::Unsupported);
}

[[nodiscard]] std::expected<FeatureSizeRef, FeatureError> parse_feature_size(Json node) {
    std::string_view type;
    if (node.at_key("type").get(type) != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    const std::string  kind    = strip_namespace(type);
    const auto         clipped = optional_int(node, "min_clipped_height");
    std::optional<i32> min_clipped;
    if (clipped) {
        min_clipped = static_cast<i32>(*clipped);
    }
    const auto limit = static_cast<i32>(optional_int(node, "limit").value_or(1));
    const auto lower = static_cast<i32>(optional_int(node, "lower_size").value_or(0));
    const auto upper = static_cast<i32>(optional_int(node, "upper_size").value_or(1));
    if (kind == "two_layers_feature_size") {
        return std::static_pointer_cast<const FeatureSize>(
            std::make_shared<const TwoLayersFeatureSize>(limit, lower, upper, min_clipped));
    }
    if (kind == "three_layers_feature_size") {
        const auto upper_limit = static_cast<i32>(optional_int(node, "upper_limit").value_or(1));
        const auto middle      = static_cast<i32>(optional_int(node, "middle_size").value_or(1));
        return std::static_pointer_cast<const FeatureSize>(
            std::make_shared<const ThreeLayersFeatureSize>(limit, upper_limit, lower, middle,
                                                           upper, min_clipped));
    }
    OV_LOG_ERROR("worldgen: feature size '{}' is not implemented", kind);
    return std::unexpected(FeatureError::Unsupported);
}

[[nodiscard]] std::expected<RootPlacerRef, FeatureError> parse_root_placer(
    Json node, const registry::BlockRegistry& blocks, const BlockTags& tags) {
    std::string_view type;
    if (node.at_key("type").get(type) != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    const std::string kind = strip_namespace(type);
    if (kind != "mangrove_root_placer") {
        OV_LOG_ERROR("worldgen: root placer '{}' is not implemented", kind);
        return std::unexpected(FeatureError::Unsupported);
    }

    auto trunk_offset = parse_int_field(node, "trunk_offset_y");
    if (!trunk_offset) return std::unexpected(trunk_offset.error());

    auto root_field = node.at_key("root_provider");
    if (root_field.error() != simdjson::SUCCESS) return std::unexpected(FeatureError::Malformed);
    auto root_provider = parse_state_provider(root_field.value(), blocks, tags);
    if (!root_provider) return std::unexpected(root_provider.error());

    StateProviderRef above_provider;
    f32              above_chance = 0.0F;
    if (auto above = node.at_key("above_root_placement"); above.error() == simdjson::SUCCESS) {
        auto provider_field = above.at_key("above_root_provider");
        if (provider_field.error() != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        auto parsed = parse_state_provider(provider_field.value(), blocks, tags);
        if (!parsed) return std::unexpected(parsed.error());
        above_provider = *parsed;
        above_chance   = optional_float(above.value(), "above_root_placement_chance", 0.0F);
    }

    auto placement = node.at_key("mangrove_root_placement");
    if (placement.error() != simdjson::SUCCESS) return std::unexpected(FeatureError::Malformed);

    auto through_field = placement.at_key("can_grow_through");
    if (through_field.error() != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    auto through = parse_block_set(through_field.value(), blocks, tags);
    if (!through) return std::unexpected(through.error());

    auto muddy_field = placement.at_key("muddy_roots_in");
    if (muddy_field.error() != simdjson::SUCCESS) return std::unexpected(FeatureError::Malformed);
    auto muddy = parse_block_set(muddy_field.value(), blocks, tags);
    if (!muddy) return std::unexpected(muddy.error());

    auto muddy_provider_field = placement.at_key("muddy_roots_provider");
    if (muddy_provider_field.error() != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    auto muddy_provider = parse_state_provider(muddy_provider_field.value(), blocks, tags);
    if (!muddy_provider) return std::unexpected(muddy_provider.error());

    const auto width  = optional_int(placement.value(), "max_root_width").value_or(8);
    const auto length = optional_int(placement.value(), "max_root_length").value_or(15);
    const f32  skew   = optional_float(placement.value(), "random_skew_chance", 0.0F);

    if (!above_provider) {
        above_provider = *root_provider;
    }
    return std::static_pointer_cast<const RootPlacer>(std::make_shared<const MangroveRootPlacer>(
        *trunk_offset, *root_provider, above_provider, above_chance, std::move(*through),
        std::move(*muddy), *muddy_provider, static_cast<i32>(width), static_cast<i32>(length),
        skew));
}

[[nodiscard]] std::expected<TreeDecoratorRef, FeatureError> parse_tree_decorator(
    Json node, const registry::BlockRegistry& blocks, const BlockTags& tags) {
    std::string_view type;
    if (node.at_key("type").get(type) != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    const std::string kind = strip_namespace(type);

    if (kind == "trunk_vine") {
        const auto vine = blocks.find_block("minecraft:vine");
        if (!vine) return std::unexpected(FeatureError::Missing);
        return std::static_pointer_cast<const TreeDecorator>(
            std::make_shared<const TrunkVineDecorator>(*vine));
    }
    if (kind == "leave_vine") {
        const auto vine = blocks.find_block("minecraft:vine");
        if (!vine) return std::unexpected(FeatureError::Missing);
        return std::static_pointer_cast<const TreeDecorator>(
            std::make_shared<const LeaveVineDecorator>(
                *vine, optional_float(node, "probability", 0.0F)));
    }
    if (kind == "cocoa") {
        const auto cocoa = blocks.find_block("minecraft:cocoa");
        if (!cocoa) return std::unexpected(FeatureError::Missing);
        return std::static_pointer_cast<const TreeDecorator>(
            std::make_shared<const CocoaDecorator>(*cocoa,
                                                   optional_float(node, "probability", 0.0F)));
    }
    if (kind == "beehive") {
        const auto hive = blocks.find_block("minecraft:bee_nest");
        if (!hive) return std::unexpected(FeatureError::Missing);
        return std::static_pointer_cast<const TreeDecorator>(
            std::make_shared<const BeehiveDecorator>(*hive,
                                                     optional_float(node, "probability", 0.0F)));
    }
    if (kind == "alter_ground") {
        auto provider_field = node.at_key("provider");
        if (provider_field.error() != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        auto provider = parse_state_provider(provider_field.value(), blocks, tags);
        if (!provider) return std::unexpected(provider.error());
        return std::static_pointer_cast<const TreeDecorator>(
            std::make_shared<const AlterGroundDecorator>(*provider));
    }
    if (kind == "attached_to_leaves") {
        auto provider_field = node.at_key("block_provider");
        if (provider_field.error() != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        auto provider = parse_state_provider(provider_field.value(), blocks, tags);
        if (!provider) return std::unexpected(provider.error());
        std::vector<std::array<i32, 3>> directions;
        simdjson::dom::array            list;
        if (node.at_key("directions").get(list) != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        for (auto entry : list) {
            std::string_view name;
            if (entry.get(name) != simdjson::SUCCESS) {
                return std::unexpected(FeatureError::Malformed);
            }
            if (name == "down") {
                directions.push_back({0, -1, 0});
            } else if (name == "up") {
                directions.push_back({0, 1, 0});
            } else if (name == "north") {
                directions.push_back({0, 0, -1});
            } else if (name == "south") {
                directions.push_back({0, 0, 1});
            } else if (name == "west") {
                directions.push_back({-1, 0, 0});
            } else if (name == "east") {
                directions.push_back({1, 0, 0});
            } else {
                return std::unexpected(FeatureError::Malformed);
            }
        }
        return std::static_pointer_cast<const TreeDecorator>(
            std::make_shared<const AttachedToLeavesDecorator>(
                optional_float(node, "probability", 0.0F),
                static_cast<i32>(optional_int(node, "exclusion_radius_xz").value_or(0)),
                static_cast<i32>(optional_int(node, "exclusion_radius_y").value_or(0)), *provider,
                static_cast<i32>(optional_int(node, "required_empty_blocks").value_or(1)),
                std::move(directions)));
    }

    OV_LOG_ERROR("worldgen: tree decorator '{}' is not implemented", kind);
    return std::unexpected(FeatureError::Unsupported);
}

}  // namespace

std::expected<FeatureRef, FeatureError> parse_tree_feature(Json                           config,
                                                           const registry::BlockRegistry& blocks,
                                                           const BlockTags&               tags) {
    TreeConfig built;

    auto tree_tags = load_tree_tags(blocks, tags);
    if (!tree_tags) {
        return std::unexpected(tree_tags.error());
    }
    built.tags = *tree_tags;

    const auto provider =
        [&](std::string_view key) -> std::expected<StateProviderRef, FeatureError> {
        auto field = config.at_key(key);
        if (field.error() != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        return parse_state_provider(field.value(), blocks, tags);
    };

    auto trunk = provider("trunk_provider");
    if (!trunk) return std::unexpected(trunk.error());
    built.trunk_provider = *trunk;
    auto foliage = provider("foliage_provider");
    if (!foliage) return std::unexpected(foliage.error());
    built.foliage_provider = *foliage;
    auto dirt = provider("dirt_provider");
    if (!dirt) return std::unexpected(dirt.error());
    built.dirt_provider = *dirt;

    auto trunk_placer_field = config.at_key("trunk_placer");
    if (trunk_placer_field.error() != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    auto trunk_placer = parse_trunk_placer(trunk_placer_field.value(), blocks, tags);
    if (!trunk_placer) return std::unexpected(trunk_placer.error());
    built.trunk_placer = *trunk_placer;

    auto foliage_placer_field = config.at_key("foliage_placer");
    if (foliage_placer_field.error() != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    auto foliage_placer = parse_foliage_placer(foliage_placer_field.value());
    if (!foliage_placer) return std::unexpected(foliage_placer.error());
    built.foliage_placer = *foliage_placer;

    auto size_field = config.at_key("minimum_size");
    if (size_field.error() != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    auto size = parse_feature_size(size_field.value());
    if (!size) return std::unexpected(size.error());
    built.minimum_size = *size;

    if (auto root_field = config.at_key("root_placer"); root_field.error() == simdjson::SUCCESS) {
        auto root = parse_root_placer(root_field.value(), blocks, tags);
        if (!root) return std::unexpected(root.error());
        built.root_placer = *root;
    }

    simdjson::dom::array decorators;
    if (config.at_key("decorators").get(decorators) == simdjson::SUCCESS) {
        for (auto entry : decorators) {
            auto parsed = parse_tree_decorator(entry, blocks, tags);
            if (!parsed) return std::unexpected(parsed.error());
            built.decorators.push_back(*parsed);
        }
    }

    (void)config.at_key("ignore_vines").get(built.ignore_vines);
    (void)config.at_key("force_dirt").get(built.force_dirt);

    return make_tree_feature(std::move(built));
}

}  // namespace ov::worldgen
