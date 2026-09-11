#include "scoreboard.hpp"

#include "ov/io/compression.hpp"
#include "ov/io/file.hpp"
#include "ov/nbt/binary.hpp"
#include "ov/registry/registries.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace ov::server {

namespace sb = net::scoreboard;

// ── Java's string hash ──────────────────────────────────────────────────────

i32 java_string_hash(std::string_view utf8) noexcept {
    u32  hash = 0;
    const auto mix = [&](u32 unit) { hash = hash * 31U + unit; };
    for (usize i = 0; i < utf8.size();) {
        const auto c = static_cast<u8>(utf8[i]);
        u32        code = c;
        usize      len  = 1;
        if (c >= 0xF0 && i + 3 < utf8.size() + 0) {
            code = ((c & 0x07U) << 18) | ((static_cast<u8>(utf8[i + 1]) & 0x3FU) << 12) |
                   ((static_cast<u8>(utf8[i + 2]) & 0x3FU) << 6) | (static_cast<u8>(utf8[i + 3]) & 0x3FU);
            len = 4;
        } else if (c >= 0xE0 && i + 2 < utf8.size()) {
            code = ((c & 0x0FU) << 12) | ((static_cast<u8>(utf8[i + 1]) & 0x3FU) << 6) |
                   (static_cast<u8>(utf8[i + 2]) & 0x3FU);
            len = 3;
        } else if (c >= 0xC0 && i + 1 < utf8.size()) {
            code = ((c & 0x1FU) << 6) | (static_cast<u8>(utf8[i + 1]) & 0x3FU);
            len  = 2;
        }
        if (code >= 0x10000) {
            // A supplementary character is two UTF-16 units: the surrogates.
            code -= 0x10000;
            mix(0xD800U + (code >> 10));
            mix(0xDC00U + (code & 0x3FFU));
        } else {
            mix(code);
        }
        i += len;
    }
    return static_cast<i32>(hash);
}

namespace {

// ── Names ───────────────────────────────────────────────────────────────────

constexpr std::array<std::string_view, kColorCount> kColors{
    "black", "dark_blue", "dark_green", "dark_aqua", "dark_red", "dark_purple", "gold", "gray",
    "dark_gray", "blue", "green", "aqua", "red", "light_purple", "yellow", "white"};

/// The stat types and the registry each one's values come from.
constexpr std::array<std::pair<std::string_view, std::string_view>, 9> kStatTypes{{
    {"minecraft:mined", "minecraft:block"},
    {"minecraft:crafted", "minecraft:item"},
    {"minecraft:used", "minecraft:item"},
    {"minecraft:broken", "minecraft:item"},
    {"minecraft:picked_up", "minecraft:item"},
    {"minecraft:dropped", "minecraft:item"},
    {"minecraft:killed", "minecraft:entity_type"},
    {"minecraft:killed_by", "minecraft:entity_type"},
    {"minecraft:custom", "minecraft:custom_stat"},
}};

/// ResourceLocation.of(text, separator): "minecraft.stone" → minecraft:stone,
/// "stone" → minecraft:stone. Nullopt for characters an id may not hold.
std::optional<std::string> id_with_separator(std::string_view text, char separator) {
    std::string_view name_space = "minecraft";
    std::string_view path       = text;
    if (const auto at = text.find(separator); at != std::string_view::npos) {
        path = text.substr(at + 1);
        if (at >= 1) {
            name_space = text.substr(0, at);
        }
    }
    const auto ns_ok = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '_' || c == '-' || c == '.';
    };
    const auto path_ok = [&](char c) { return ns_ok(c) || c == '/'; };
    if (!std::ranges::all_of(name_space, ns_ok) || !std::ranges::all_of(path, path_ok) || path.empty()) {
        return std::nullopt;
    }
    return std::string{name_space} + ":" + std::string{path};
}

std::string dotted(std::string_view id) {
    std::string out{id};
    std::ranges::replace(out, ':', '.');
    return out;
}

/// Java's `a.compareToIgnoreCase(b)` for ASCII, which is what holder names are.
int compare_ignore_case(std::string_view a, std::string_view b) {
    const usize n = std::min(a.size(), b.size());
    for (usize i = 0; i < n; ++i) {
        const auto lower = [](char c) {
            return static_cast<int>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        };
        const int ca = lower(a[i]);
        const int cb = lower(b[i]);
        if (ca != cb) {
            return ca - cb;
        }
    }
    return static_cast<int>(a.size()) - static_cast<int>(b.size());
}

/// A compound whose keys are written in the order a Java HashMap would hold
/// them — which is how the game writes every compound it builds.
nbt::Tag java_compound(std::vector<nbt::CompoundEntry> entries) {
    JavaHashOrder         order;
    std::vector<std::pair<nbt::CompoundEntry*, u64>> ranked;
    for (nbt::CompoundEntry& entry : entries) {
        ranked.emplace_back(&entry, order.insert());
    }
    order.sort(ranked, [](const auto& e) -> std::string_view { return e.first->name; },
               [](const auto& e) { return e.second; });
    nbt::Tag out = nbt::Tag::make_compound();
    for (auto& [entry, sequence] : ranked) {
        out.put(std::move(entry->name), std::move(entry->value));
    }
    return out;
}

std::string text_of(std::string_view name) {
    std::string out = R"({"text":")";
    for (const char c : name) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    out += "\"}";
    return out;
}

constexpr std::array<std::string_view, 4> kVisibility{"always", "never", "hideForOtherTeams",
                                                      "hideForOwnTeam"};
constexpr std::array<std::string_view, 4> kCollision{"always", "never", "pushOtherTeams",
                                                     "pushOwnTeam"};

}  // namespace

std::string_view color_name(u8 color) noexcept {
    return color < kColorCount ? kColors[color] : std::string_view{"reset"};
}

std::optional<u8> color_from_name(std::string_view name) noexcept {
    for (usize i = 0; i < kColors.size(); ++i) {
        if (kColors[i] == name) {
            return static_cast<u8>(i);
        }
    }
    if (name == "reset") {
        return kColorReset;
    }
    return std::nullopt;
}

std::optional<u8> display_slot_from_name(std::string_view name) noexcept {
    if (name == "list") {
        return sb::kSlotList;
    }
    if (name == "sidebar") {
        return sb::kSlotSidebar;
    }
    if (name == "belowName") {
        return sb::kSlotBelowName;
    }
    constexpr std::string_view kTeam = "sidebar.team.";
    if (name.starts_with(kTeam)) {
        const auto color = color_from_name(name.substr(kTeam.size()));
        if (color && *color < kColorCount) {
            return static_cast<u8>(sb::kSlotTeamBase + *color);
        }
    }
    return std::nullopt;
}

std::string display_slot_name(u8 slot) {
    switch (slot) {
    case sb::kSlotList:
        return "list";
    case sb::kSlotSidebar:
        return "sidebar";
    case sb::kSlotBelowName:
        return "belowName";
    default:
        return "sidebar.team." + std::string{color_name(static_cast<u8>(slot - sb::kSlotTeamBase))};
    }
}

std::string_view render_type_name(RenderType type) noexcept {
    return type == RenderType::Hearts ? "hearts" : "integer";
}

std::optional<RenderType> render_type_from_name(std::string_view name) noexcept {
    if (name == "integer") {
        return RenderType::Integer;
    }
    if (name == "hearts") {
        return RenderType::Hearts;
    }
    return std::nullopt;
}

std::string_view visibility_name(Visibility visibility) noexcept {
    return kVisibility[static_cast<usize>(visibility)];
}

std::optional<Visibility> visibility_from_name(std::string_view name) noexcept {
    for (usize i = 0; i < kVisibility.size(); ++i) {
        if (kVisibility[i] == name) {
            return static_cast<Visibility>(i);
        }
    }
    return std::nullopt;
}

std::string_view collision_rule_name(CollisionRule rule) noexcept {
    return kCollision[static_cast<usize>(rule)];
}

std::optional<CollisionRule> collision_rule_from_name(std::string_view name) noexcept {
    for (usize i = 0; i < kCollision.size(); ++i) {
        if (kCollision[i] == name) {
            return static_cast<CollisionRule>(i);
        }
    }
    return std::nullopt;
}

// ── Criteria ────────────────────────────────────────────────────────────────

bool Criterion::read_only() const noexcept {
    switch (kind) {
    case CriterionKind::Health:
    case CriterionKind::Food:
    case CriterionKind::Air:
    case CriterionKind::Armor:
    case CriterionKind::Xp:
    case CriterionKind::Level:
        return true;
    default:
        return false;
    }
}

RenderType Criterion::default_render() const noexcept {
    return kind == CriterionKind::Health ? RenderType::Hearts : RenderType::Integer;
}

std::optional<Criterion> parse_criterion(std::string_view name, const registry::Registries* registries) {
    static constexpr std::array<std::pair<std::string_view, CriterionKind>, 11> kSimple{{
        {"dummy", CriterionKind::Dummy},
        {"trigger", CriterionKind::Trigger},
        {"deathCount", CriterionKind::DeathCount},
        {"playerKillCount", CriterionKind::PlayerKillCount},
        {"totalKillCount", CriterionKind::TotalKillCount},
        {"health", CriterionKind::Health},
        {"food", CriterionKind::Food},
        {"air", CriterionKind::Air},
        {"armor", CriterionKind::Armor},
        {"xp", CriterionKind::Xp},
        {"level", CriterionKind::Level},
    }};
    for (const auto& [simple, kind] : kSimple) {
        if (name == simple) {
            Criterion out;
            out.kind = kind;
            out.name = std::string{name};
            return out;
        }
    }
    for (const auto& [prefix, kind] :
         {std::pair<std::string_view, CriterionKind>{"teamkill.", CriterionKind::TeamKill},
          std::pair<std::string_view, CriterionKind>{"killedByTeam.", CriterionKind::KilledByTeam}}) {
        if (name.starts_with(prefix)) {
            const auto color = color_from_name(name.substr(prefix.size()));
            if (!color || *color >= kColorCount) {
                return std::nullopt;
            }
            Criterion out;
            out.kind  = kind;
            out.color = *color;
            out.name  = std::string{name};
            return out;
        }
    }
    const auto colon = name.find(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    const auto type  = id_with_separator(name.substr(0, colon), '.');
    const auto value = id_with_separator(name.substr(colon + 1), '.');
    if (!type || !value) {
        return std::nullopt;
    }
    const auto known = std::ranges::find_if(kStatTypes, [&](const auto& t) { return t.first == *type; });
    if (known == kStatTypes.end()) {
        return std::nullopt;
    }
    if (registries != nullptr) {
        const auto registry = registries->find(known->second);
        if (!registry || !registries->protocol_id(*registry, *value)) {
            return std::nullopt;
        }
    }
    Criterion out;
    out.kind       = CriterionKind::Stat;
    out.name       = dotted(*type) + ":" + dotted(*value);
    out.stat_type  = *type;
    out.stat_value = *value;
    return out;
}

// ── Teams ───────────────────────────────────────────────────────────────────

std::vector<std::string> Team::members() const {
    std::vector<const Member*> sorted;
    sorted.reserve(members_.size());
    for (const Member& m : members_) {
        sorted.push_back(&m);
    }
    order_.sort(sorted, [](const Member* m) -> std::string_view { return m->name; },
                [](const Member* m) { return m->sequence; });
    std::vector<std::string> out;
    out.reserve(sorted.size());
    for (const Member* m : sorted) {
        out.push_back(m->name);
    }
    return out;
}

bool Team::has_member(std::string_view holder) const noexcept {
    return std::ranges::any_of(members_, [&](const Member& m) { return m.name == holder; });
}

sb::TeamParameters Team::parameters() const {
    sb::TeamParameters p;
    p.display_json       = display_json;
    p.flags              = static_cast<u8>((friendly_fire ? sb::kTeamFriendlyFire : 0) |
                                           (see_friendly_invisibles ? sb::kTeamSeeFriendlyInvisible : 0));
    p.nametag_visibility = std::string{visibility_name(name_tag)};
    p.collision_rule     = std::string{collision_rule_name(collision)};
    p.color              = color;
    p.prefix_json        = prefix_json;
    p.suffix_json        = suffix_json;
    return p;
}

// ── Lookups ─────────────────────────────────────────────────────────────────

Scoreboard::ObjectiveEntry* Scoreboard::find_objective(std::string_view name) noexcept {
    for (auto& entry : objectives_) {
        if (entry->objective.name == name) {
            return entry.get();
        }
    }
    return nullptr;
}

const Scoreboard::ObjectiveEntry* Scoreboard::find_objective(std::string_view name) const noexcept {
    return const_cast<Scoreboard*>(this)->find_objective(name);  // NOLINT(*-const-cast)
}

Scoreboard::Holder* Scoreboard::find_holder(std::string_view name) noexcept {
    const auto it = holders_.find(std::string{name});
    return it == holders_.end() ? nullptr : it->second.get();
}

const Scoreboard::Holder* Scoreboard::find_holder(std::string_view name) const noexcept {
    const auto it = holders_.find(std::string{name});
    return it == holders_.end() ? nullptr : it->second.get();
}

Scoreboard::TeamEntry* Scoreboard::find_team(std::string_view name) noexcept {
    for (auto& entry : teams_) {
        if (entry->team.name == name) {
            return entry.get();
        }
    }
    return nullptr;
}

const Scoreboard::TeamEntry* Scoreboard::find_team(std::string_view name) const noexcept {
    return const_cast<Scoreboard*>(this)->find_team(name);  // NOLINT(*-const-cast)
}

// ── Packets ─────────────────────────────────────────────────────────────────

void Scoreboard::emit(i32 id, std::vector<u8> payload) {
    packets_.push_back(ScoreboardPacket{id, std::move(payload)});
}

void Scoreboard::send_score(const Holder& holder, const Objective& objective, const Score& score) {
    if (tracked(objective)) {
        emit(sb::kUpdateScore, sb::encode_update_score(
                                   {holder.name, sb::ScoreAction::Change, objective.name, score.value}));
    }
}

void Scoreboard::start_tracking(const Objective& objective, std::vector<ScoreboardPacket>& out) const {
    out.push_back({sb::kUpdateObjectives,
                   sb::encode_update_objectives({objective.name, sb::ObjectiveMode::Create,
                                                 objective.display_json,
                                                 static_cast<i32>(objective.render)})});
    for (u8 slot = 0; slot < sb::kSlotCount; ++slot) {
        if (display_[slot] == &objective) {
            out.push_back({sb::kDisplayObjective,
                           sb::encode_display_objective({slot, objective.name})});
        }
    }
    // Sorted by value, then by holder in reverse, ignoring case: the capture's
    // `a` (-2147483648), `nobody`, `b`, `a,b` (all 0), `#hidden` (3), `ovprobe`.
    std::vector<std::pair<const Holder*, i32>> scores;
    for (const auto& [name, holder] : holders_) {
        for (const auto& [owner, score] : holder->scores) {
            if (owner == &objective) {
                scores.emplace_back(holder.get(), score.value);
            }
        }
    }
    std::ranges::sort(scores, [](const auto& a, const auto& b) {
        if (a.second != b.second) {
            return a.second < b.second;
        }
        return compare_ignore_case(b.first->name, a.first->name) < 0;
    });
    for (const auto& [holder, value] : scores) {
        out.push_back({sb::kUpdateScore,
                       sb::encode_update_score({holder->name, sb::ScoreAction::Change, objective.name, value})});
    }
}

void Scoreboard::arrival_packets(std::vector<ScoreboardPacket>& out) const {
    for (const Team* team : teams()) {
        sb::TeamUpdate update;
        update.name       = team->name;
        update.mode       = sb::TeamMode::Create;
        update.parameters = team->parameters();
        update.entities   = team->members();
        out.push_back({sb::kUpdateTeams, sb::encode_update_teams(update)});
    }
    std::vector<const Objective*> sent;
    for (u8 slot = 0; slot < sb::kSlotCount; ++slot) {
        const Objective* shown = display_[slot];
        if (shown != nullptr && std::ranges::find(sent, shown) == sent.end()) {
            sent.push_back(shown);
            start_tracking(*shown, out);
        }
    }
}

// ── Objectives ──────────────────────────────────────────────────────────────

const Objective* Scoreboard::objective(std::string_view name) const noexcept {
    const ObjectiveEntry* entry = find_objective(name);
    return entry == nullptr ? nullptr : &entry->objective;
}

const Objective* Scoreboard::add_objective(std::string name, Criterion criterion,
                                           std::string display_json, RenderType render) {
    if (find_objective(name) != nullptr) {
        return nullptr;
    }
    auto entry                    = std::make_unique<ObjectiveEntry>();
    entry->objective.name         = std::move(name);
    entry->objective.criterion    = std::move(criterion);
    entry->objective.display_json = std::move(display_json);
    entry->objective.render       = render;
    entry->sequence               = objective_order_.insert();
    entry->created                = objectives_created_++;
    objectives_.push_back(std::move(entry));
    dirty_ = true;
    return &objectives_.back()->objective;
}

bool Scoreboard::remove_objective(std::string_view name) {
    const auto it = std::ranges::find_if(objectives_, [&](const auto& e) { return e->objective.name == name; });
    if (it == objectives_.end()) {
        return false;
    }
    const Objective* gone = &(*it)->objective;
    const bool       was_tracked = tracked(*gone);
    for (auto& [holder_name, holder] : holders_) {
        // The holder stays tracked even with nothing left, as vanilla's does:
        // only a reset forgets a holder.
        std::erase_if(holder->scores, [&](const auto& s) { return s.first == gone; });
    }
    for (const Objective*& slot : display_) {
        if (slot == gone) {
            slot = nullptr;
        }
    }
    if (was_tracked) {
        emit(sb::kUpdateObjectives,
             sb::encode_update_objectives({gone->name, sb::ObjectiveMode::Remove, {}, 0}));
    }
    objectives_.erase(it);
    objective_order_.erase();
    dirty_ = true;
    return true;
}

std::vector<const Objective*> Scoreboard::objectives() const {
    std::vector<const ObjectiveEntry*> sorted;
    for (const auto& entry : objectives_) {
        sorted.push_back(entry.get());
    }
    objective_order_.sort(sorted, [](const ObjectiveEntry* e) -> std::string_view { return e->objective.name; },
                          [](const ObjectiveEntry* e) { return e->sequence; });
    std::vector<const Objective*> out;
    for (const ObjectiveEntry* e : sorted) {
        out.push_back(&e->objective);
    }
    return out;
}

void Scoreboard::set_display_name(std::string_view name, std::string display_json) {
    ObjectiveEntry* entry = find_objective(name);
    if (entry == nullptr) {
        return;
    }
    entry->objective.display_json = std::move(display_json);
    dirty_                        = true;
    if (tracked(entry->objective)) {
        emit(sb::kUpdateObjectives,
             sb::encode_update_objectives({entry->objective.name, sb::ObjectiveMode::Update,
                                           entry->objective.display_json,
                                           static_cast<i32>(entry->objective.render)}));
    }
}

void Scoreboard::set_render_type(std::string_view name, RenderType render) {
    ObjectiveEntry* entry = find_objective(name);
    if (entry == nullptr) {
        return;
    }
    entry->objective.render = render;
    dirty_                  = true;
    if (tracked(entry->objective)) {
        emit(sb::kUpdateObjectives,
             sb::encode_update_objectives({entry->objective.name, sb::ObjectiveMode::Update,
                                           entry->objective.display_json, static_cast<i32>(render)}));
    }
}

const Objective* Scoreboard::displayed(u8 slot) const noexcept {
    return slot < display_.size() ? display_[slot] : nullptr;
}

bool Scoreboard::tracked(const Objective& objective) const noexcept {
    return std::ranges::find(display_, &objective) != display_.end();
}

void Scoreboard::set_displayed(u8 slot, std::string_view name) {
    if (slot >= display_.size()) {
        return;
    }
    const Objective* next     = name.empty() ? nullptr : objective(name);
    const Objective* previous = display_[slot];
    if (previous == next) {
        return;
    }
    const bool next_was_tracked = next != nullptr && tracked(*next);
    display_[slot]              = next;
    dirty_                      = true;
    if (previous != nullptr) {
        if (tracked(*previous)) {
            // Still shown elsewhere: only this slot changes.
            if (next == nullptr) {
                emit(sb::kDisplayObjective, sb::encode_display_objective({slot, {}}));
            }
        } else {
            emit(sb::kUpdateObjectives,
                 sb::encode_update_objectives({previous->name, sb::ObjectiveMode::Remove, {}, 0}));
        }
    }
    if (next != nullptr) {
        if (next_was_tracked) {
            emit(sb::kDisplayObjective, sb::encode_display_objective({slot, next->name}));
        } else {
            start_tracking(*next, packets_);
        }
    }
}

// ── Scores ──────────────────────────────────────────────────────────────────

Scoreboard::Holder& Scoreboard::holder(std::string_view name) {
    if (Holder* found = find_holder(name)) {
        return *found;
    }
    auto created      = std::make_unique<Holder>();
    created->name     = std::string{name};
    created->sequence = holder_order_.insert();
    Holder& out       = *created;
    holders_.emplace(std::string{name}, std::move(created));
    return out;
}

Score& Scoreboard::score_or_create(Holder& holder, const Objective& objective) {
    for (auto& [owner, score] : holder.scores) {
        if (owner == &objective) {
            return score;
        }
    }
    holder.scores.emplace_back(&objective, Score{});
    dirty_ = true;
    // Created at 0, and the clients told so before anything else happens to
    // it: `players set @s d 5` is two packets, 0 then 5.
    send_score(holder, objective, holder.scores.back().second);
    return holder.scores.back().second;
}

void Scoreboard::change(Holder& holder, const Objective& objective, Score& score, i32 value) {
    if (score.value == value) {
        return;
    }
    score.value = value;
    dirty_      = true;
    send_score(holder, objective, score);
}

std::optional<Score> Scoreboard::score(std::string_view holder_name, std::string_view objective_name) const {
    const Holder*    h = find_holder(holder_name);
    const Objective* o = objective(objective_name);
    if (h == nullptr || o == nullptr) {
        return std::nullopt;
    }
    for (const auto& [owner, score] : h->scores) {
        if (owner == o) {
            return score;
        }
    }
    return std::nullopt;
}

std::optional<i32> Scoreboard::get_or_create(std::string_view holder_name, std::string_view objective_name) {
    const Objective* o = objective(objective_name);
    if (o == nullptr) {
        return std::nullopt;
    }
    return score_or_create(holder(holder_name), *o).value;
}

bool Scoreboard::set_score(std::string_view holder_name, std::string_view objective_name, i32 value) {
    const Objective* o = objective(objective_name);
    if (o == nullptr) {
        return false;
    }
    Holder& h = holder(holder_name);
    change(h, *o, score_or_create(h, *o), value);
    return true;
}

bool Scoreboard::set_locked(std::string_view holder_name, std::string_view objective_name, bool locked) {
    const Objective* o = objective(objective_name);
    if (o == nullptr) {
        return false;
    }
    Score& s = score_or_create(holder(holder_name), *o);
    if (s.locked != locked) {
        s.locked = locked;
        dirty_   = true;
    }
    return true;
}

void Scoreboard::remove_holder(std::string_view name) {
    holders_.erase(std::string{name});
    holder_order_.erase();
    dirty_ = true;
    emit(sb::kUpdateScore, sb::encode_update_score({std::string{name}, sb::ScoreAction::Remove, {}, 0}));
}

bool Scoreboard::reset(std::string_view holder_name, std::string_view objective_name) {
    Holder* h = find_holder(holder_name);
    if (h == nullptr) {
        return false;
    }
    if (objective_name.empty()) {
        remove_holder(holder_name);
        return true;
    }
    const Objective* o = objective(objective_name);
    if (o == nullptr) {
        return false;
    }
    const auto removed = std::erase_if(h->scores, [&](const auto& s) { return s.first == o; });
    if (removed == 0) {
        return false;
    }
    if (h->scores.empty()) {
        remove_holder(holder_name);
        return true;
    }
    dirty_ = true;
    if (tracked(*o)) {
        emit(sb::kUpdateScore,
             sb::encode_update_score({std::string{holder_name}, sb::ScoreAction::Remove, o->name, 0}));
    }
    return true;
}

std::vector<std::string> Scoreboard::holders() const {
    std::vector<const Holder*> sorted;
    for (const auto& [name, holder] : holders_) {
        sorted.push_back(holder.get());
    }
    holder_order_.sort(sorted, [](const Holder* h) -> std::string_view { return h->name; },
                       [](const Holder* h) { return h->sequence; });
    std::vector<std::string> out;
    for (const Holder* h : sorted) {
        out.push_back(h->name);
    }
    return out;
}

std::vector<std::pair<const Objective*, Score>> Scoreboard::scores_of(std::string_view holder_name) const {
    const Holder* h = find_holder(holder_name);
    return h == nullptr ? std::vector<std::pair<const Objective*, Score>>{} : h->scores;
}

// ── Criteria ────────────────────────────────────────────────────────────────

void Scoreboard::increment(CriterionKind kind, u8 color, std::string_view holder_name) {
    std::vector<const ObjectiveEntry*> fed;
    for (const auto& entry : objectives_) {
        const Criterion& c = entry->objective.criterion;
        if (c.kind == kind && (kind != CriterionKind::TeamKill && kind != CriterionKind::KilledByTeam
                                   ? true
                                   : c.color == color)) {
            fed.push_back(entry.get());
        }
    }
    std::ranges::sort(fed, {}, &ObjectiveEntry::created);
    for (const ObjectiveEntry* entry : fed) {
        Holder& h = holder(holder_name);
        Score&  s = score_or_create(h, entry->objective);
        change(h, entry->objective, s, static_cast<i32>(static_cast<u32>(s.value) + 1U));
    }
}

void Scoreboard::set_all(CriterionKind kind, std::string_view holder_name, i32 value) {
    std::vector<const ObjectiveEntry*> fed;
    for (const auto& entry : objectives_) {
        if (entry->objective.criterion.kind == kind) {
            fed.push_back(entry.get());
        }
    }
    std::ranges::sort(fed, {}, &ObjectiveEntry::created);
    for (const ObjectiveEntry* entry : fed) {
        Holder& h = holder(holder_name);
        change(h, entry->objective, score_or_create(h, entry->objective), value);
    }
}

void Scoreboard::on_death(std::string_view holder_name) {
    increment(CriterionKind::DeathCount, 0, holder_name);
}

void Scoreboard::on_kill(std::string_view killer, std::string_view victim, bool victim_is_player) {
    if (killer == victim) {
        return;
    }
    increment(CriterionKind::TotalKillCount, 0, killer);
    if (victim_is_player) {
        increment(CriterionKind::PlayerKillCount, 0, killer);
    }
    if (const Team* team = team_of(victim); team != nullptr && team->color < kColorCount) {
        increment(CriterionKind::TeamKill, team->color, killer);
    }
    if (const Team* team = team_of(killer); team != nullptr && team->color < kColorCount) {
        increment(CriterionKind::KilledByTeam, team->color, victim);
    }
}

void Scoreboard::update_player(std::string_view holder_name, const PlayerCriteria& now) {
    const auto it    = last_seen_.find(std::string{holder_name});
    const bool first = it == last_seen_.end();
    const PlayerCriteria before = first ? PlayerCriteria{} : it->second;
    // The order the game checks them in: health, food, air, armour,
    // experience, level.
    if (first || before.health != now.health) {
        set_all(CriterionKind::Health, holder_name, now.health);
    }
    if (first || before.food != now.food) {
        set_all(CriterionKind::Food, holder_name, now.food);
    }
    if (first || before.air != now.air) {
        set_all(CriterionKind::Air, holder_name, now.air);
    }
    if (first || before.armor != now.armor) {
        set_all(CriterionKind::Armor, holder_name, now.armor);
    }
    if (first || before.xp != now.xp) {
        set_all(CriterionKind::Xp, holder_name, now.xp);
    }
    if (first || before.level != now.level) {
        set_all(CriterionKind::Level, holder_name, now.level);
    }
    last_seen_[std::string{holder_name}] = now;
}

void Scoreboard::forget_player(std::string_view holder_name) { last_seen_.erase(std::string{holder_name}); }

void Scoreboard::on_stat(std::string_view holder_name, std::string_view stat_type,
                         std::string_view stat_value, i32 total) {
    std::vector<const ObjectiveEntry*> fed;
    for (const auto& entry : objectives_) {
        const Criterion& c = entry->objective.criterion;
        if (c.kind == CriterionKind::Stat && c.stat_type == stat_type && c.stat_value == stat_value) {
            fed.push_back(entry.get());
        }
    }
    std::ranges::sort(fed, {}, &ObjectiveEntry::created);
    for (const ObjectiveEntry* entry : fed) {
        Holder& h = holder(holder_name);
        change(h, entry->objective, score_or_create(h, entry->objective), total);
    }
}

// ── Teams ───────────────────────────────────────────────────────────────────

const Team* Scoreboard::team(std::string_view name) const noexcept {
    const TeamEntry* entry = find_team(name);
    return entry == nullptr ? nullptr : &entry->team;
}

Team* Scoreboard::edit_team(std::string_view name) noexcept {
    TeamEntry* entry = find_team(name);
    return entry == nullptr ? nullptr : &entry->team;
}

const Team* Scoreboard::add_team(std::string name, std::string display_json) {
    if (find_team(name) != nullptr) {
        return nullptr;
    }
    auto entry                = std::make_unique<TeamEntry>();
    entry->team.name          = std::move(name);
    entry->team.display_json  = text_of(entry->team.name);
    entry->sequence           = team_order_.insert();
    Team& team                = entry->team;
    teams_.push_back(std::move(entry));
    dirty_ = true;
    sb::TeamUpdate create;
    create.name       = team.name;
    create.mode       = sb::TeamMode::Create;
    create.parameters = team.parameters();
    emit(sb::kUpdateTeams, sb::encode_update_teams(create));
    // `team add` names the team after creating it, so an Update always
    // follows — even when the name is the default one.
    team.display_json = std::move(display_json);
    team_changed(team.name);
    return &team;
}

bool Scoreboard::remove_team(std::string_view name) {
    const auto it = std::ranges::find_if(teams_, [&](const auto& e) { return e->team.name == name; });
    if (it == teams_.end()) {
        return false;
    }
    for (const Team::Member& member : (*it)->team.members_) {
        team_by_holder_.erase(member.name);
    }
    emit(sb::kUpdateTeams, sb::encode_update_teams({std::string{name}, sb::TeamMode::Remove, {}, {}}));
    teams_.erase(it);
    team_order_.erase();
    dirty_ = true;
    return true;
}

std::vector<const Team*> Scoreboard::teams() const {
    std::vector<const TeamEntry*> sorted;
    for (const auto& entry : teams_) {
        sorted.push_back(entry.get());
    }
    team_order_.sort(sorted, [](const TeamEntry* e) -> std::string_view { return e->team.name; },
                     [](const TeamEntry* e) { return e->sequence; });
    std::vector<const Team*> out;
    for (const TeamEntry* e : sorted) {
        out.push_back(&e->team);
    }
    return out;
}

const Team* Scoreboard::team_of(std::string_view holder_name) const noexcept {
    const auto it = team_by_holder_.find(std::string{holder_name});
    return it == team_by_holder_.end() ? nullptr : team(it->second);
}

bool Scoreboard::leave(std::string_view holder_name) {
    const auto it = team_by_holder_.find(std::string{holder_name});
    if (it == team_by_holder_.end()) {
        return false;
    }
    if (TeamEntry* entry = find_team(it->second)) {
        const auto gone = std::erase_if(entry->team.members_,
                                        [&](const Team::Member& m) { return m.name == holder_name; });
        if (gone > 0) {
            entry->team.order_.erase();
        }
        emit(sb::kUpdateTeams, sb::encode_update_teams({entry->team.name, sb::TeamMode::RemoveEntities,
                                                        {}, {std::string{holder_name}}}));
    }
    team_by_holder_.erase(it);
    dirty_ = true;
    return true;
}

void Scoreboard::join(std::string_view team_name, std::string_view holder_name) {
    if (find_team(team_name) == nullptr) {
        return;
    }
    (void)leave(holder_name);
    TeamEntry* entry = find_team(team_name);
    entry->team.members_.push_back({std::string{holder_name}, entry->team.order_.insert()});
    team_by_holder_[std::string{holder_name}] = entry->team.name;
    dirty_                                   = true;
    emit(sb::kUpdateTeams, sb::encode_update_teams({entry->team.name, sb::TeamMode::AddEntities, {},
                                                    {std::string{holder_name}}}));
}

usize Scoreboard::empty_team(std::string_view team_name) {
    TeamEntry* entry = find_team(team_name);
    if (entry == nullptr) {
        return 0;
    }
    const std::vector<std::string> members = entry->team.members();
    for (const std::string& member : members) {
        (void)leave(member);
    }
    return members.size();
}

void Scoreboard::team_changed(std::string_view team_name) {
    const TeamEntry* entry = find_team(team_name);
    if (entry == nullptr) {
        return;
    }
    dirty_ = true;
    emit(sb::kUpdateTeams, sb::encode_update_teams({entry->team.name, sb::TeamMode::Update,
                                                    entry->team.parameters(), {}}));
}

bool Scoreboard::can_hurt(const Team* attacker, const Team* victim) noexcept {
    // Friendly fire only ever forbids: two holders on the same team that has
    // it off.
    return attacker == nullptr || attacker != victim || attacker->friendly_fire;
}

bool Scoreboard::can_push(const Team* a, const Team* b) noexcept {
    const bool allied = a != nullptr && a == b;
    for (const Team* side : {a, b}) {
        if (side == nullptr) {
            continue;
        }
        switch (side->collision) {
        case CollisionRule::Always:
            break;
        case CollisionRule::Never:
            return false;
        case CollisionRule::PushOtherTeams:
            if (allied) {
                return false;
            }
            break;
        case CollisionRule::PushOwnTeam:
            if (!allied) {
                return false;
            }
            break;
        }
    }
    return true;
}

// ── Persistence ─────────────────────────────────────────────────────────────

nbt::Tag Scoreboard::save() const {
    nbt::Tag objectives = nbt::Tag::make_list(nbt::TagType::Compound);
    for (const Objective* o : this->objectives()) {
        objectives.push(java_compound({{"Name", nbt::Tag{o->name}},
                                       {"CriteriaName", nbt::Tag{o->criterion.name}},
                                       {"DisplayName", nbt::Tag{o->display_json}},
                                       {"RenderType", nbt::Tag{std::string{render_type_name(o->render)}}}}));
    }
    for (const nbt::Tag& kept : unknown_objectives_) {
        objectives.push(kept);
    }

    nbt::Tag scores = nbt::Tag::make_list(nbt::TagType::Compound);
    for (const std::string& name : holders()) {
        const Holder* h = find_holder(name);
        for (const auto& [owner, score] : h->scores) {
            scores.push(java_compound({{"Name", nbt::Tag{h->name}},
                                       {"Objective", nbt::Tag{owner->name}},
                                       {"Score", nbt::Tag{score.value}},
                                       {"Locked", nbt::Tag::make_bool(score.locked)}}));
        }
    }
    for (const nbt::Tag& kept : unknown_scores_) {
        scores.push(kept);
    }

    nbt::Tag teams = nbt::Tag::make_list(nbt::TagType::Compound);
    for (const Team* t : this->teams()) {
        nbt::Tag players = nbt::Tag::make_list(nbt::TagType::String);
        for (const std::string& member : t->members()) {
            players.push(nbt::Tag{member});
        }
        std::vector<nbt::CompoundEntry> entries{
            {"Name", nbt::Tag{t->name}},
            {"DisplayName", nbt::Tag{t->display_json}},
            {"AllowFriendlyFire", nbt::Tag::make_bool(t->friendly_fire)},
            {"SeeFriendlyInvisibles", nbt::Tag::make_bool(t->see_friendly_invisibles)},
            {"MemberNamePrefix", nbt::Tag{t->prefix_json}},
            {"MemberNameSuffix", nbt::Tag{t->suffix_json}},
            {"NameTagVisibility", nbt::Tag{std::string{visibility_name(t->name_tag)}}},
            {"DeathMessageVisibility", nbt::Tag{std::string{visibility_name(t->death_message)}}},
            {"CollisionRule", nbt::Tag{std::string{collision_rule_name(t->collision)}}},
            {"Players", std::move(players)}};
        // A team with no colour has no TeamColor at all (the capture's
        // `abcdefghijklmnopqrstuvwxyz`), not "reset".
        if (t->color < kColorCount) {
            entries.push_back({"TeamColor", nbt::Tag{std::string{color_name(t->color)}}});
        }
        teams.push(java_compound(std::move(entries)));
    }

    std::vector<nbt::CompoundEntry> slots;
    for (u8 slot = 0; slot < display_.size(); ++slot) {
        if (display_[slot] != nullptr) {
            slots.push_back({"slot_" + std::to_string(slot), nbt::Tag{display_[slot]->name}});
        }
    }

    std::vector<nbt::CompoundEntry> data{{"Objectives", std::move(objectives)},
                                         {"PlayerScores", std::move(scores)},
                                         {"Teams", std::move(teams)},
                                         {"DisplaySlots", java_compound(std::move(slots))}};
    nbt::Tag data_tag = java_compound(std::move(data));
    for (const nbt::CompoundEntry& kept : extra_data_) {
        data_tag.put(kept.name, kept.value);
    }
    nbt::Tag root = java_compound({{"data", std::move(data_tag)},
                                   {"DataVersion", nbt::Tag{kScoreboardDataVersion}}});
    for (const nbt::CompoundEntry& kept : extra_root_) {
        root.put(kept.name, kept.value);
    }
    return root;
}

std::expected<Scoreboard, std::string> Scoreboard::load(const nbt::Tag& root,
                                                        const registry::Registries* registries) {
    if (root.compound() == nullptr) {
        return std::unexpected{std::string{"the root is not a compound"}};
    }
    if (const nbt::Tag* version = root.find("DataVersion");
        version != nullptr && version->as_i64() > kScoreboardDataVersion) {
        return std::unexpected{"DataVersion " + std::to_string(version->as_i64()) +
                               " is newer than this server's " +
                               std::to_string(kScoreboardDataVersion)};
    }
    Scoreboard out;
    for (const nbt::CompoundEntry& entry : *root.compound()) {
        if (entry.name != "data" && entry.name != "DataVersion") {
            out.extra_root_.push_back(entry);
        }
    }
    const nbt::Tag* data = root.find("data");
    if (data == nullptr || data->compound() == nullptr) {
        return out;  // an empty scoreboard, written by nobody in particular
    }
    for (const nbt::CompoundEntry& entry : *data->compound()) {
        if (entry.name != "Objectives" && entry.name != "PlayerScores" && entry.name != "Teams" &&
            entry.name != "DisplaySlots") {
            out.extra_data_.push_back(entry);
        }
    }
    const auto string_of = [](const nbt::Tag& compound, std::string_view key,
                              std::string_view fallback = {}) {
        const nbt::Tag* tag = compound.find(key);
        return std::string{tag != nullptr ? tag->as_string(fallback) : fallback};
    };

    std::vector<std::string> unknown_names;
    if (const nbt::Tag* list = data->find("Objectives"); list != nullptr && list->list() != nullptr) {
        for (const nbt::Tag& entry : *list->list()) {
            const std::string name     = string_of(entry, "Name");
            const auto        criterion = parse_criterion(string_of(entry, "CriteriaName"), registries);
            if (!criterion || name.empty() || out.objective(name) != nullptr) {
                unknown_names.push_back(name);
                out.unknown_objectives_.push_back(entry);
                continue;
            }
            const std::string display = string_of(entry, "DisplayName", text_of(name));
            const auto render = render_type_from_name(string_of(entry, "RenderType"))
                                    .value_or(criterion->default_render());
            (void)out.add_objective(name, *criterion, display, render);
        }
    }
    if (const nbt::Tag* list = data->find("PlayerScores"); list != nullptr && list->list() != nullptr) {
        for (const nbt::Tag& entry : *list->list()) {
            const std::string holder_name = string_of(entry, "Name");
            const std::string objective   = string_of(entry, "Objective");
            const Objective*  o           = out.objective(objective);
            if (o == nullptr || holder_name.empty()) {
                out.unknown_scores_.push_back(entry);
                continue;
            }
            Holder& h = out.holder(holder_name);
            Score   s;
            if (const nbt::Tag* value = entry.find("Score")) {
                s.value = static_cast<i32>(value->as_i64());
            }
            // Absent means unlocked: the game reads it as a plain boolean.
            const nbt::Tag* locked = entry.find("Locked");
            s.locked               = locked != nullptr && locked->as_bool();
            h.scores.emplace_back(o, s);
        }
    }
    if (const nbt::Tag* list = data->find("Teams"); list != nullptr && list->list() != nullptr) {
        for (const nbt::Tag& entry : *list->list()) {
            const std::string name = string_of(entry, "Name");
            if (name.empty() || out.find_team(name) != nullptr) {
                continue;
            }
            auto created            = std::make_unique<TeamEntry>();
            Team& t                 = created->team;
            t.name                  = name;
            t.display_json          = string_of(entry, "DisplayName", text_of(name));
            t.color                 = color_from_name(string_of(entry, "TeamColor", "reset")).value_or(kColorReset);
            t.prefix_json           = string_of(entry, "MemberNamePrefix", kEmptyComponent);
            t.suffix_json           = string_of(entry, "MemberNameSuffix", kEmptyComponent);
            const nbt::Tag* ff      = entry.find("AllowFriendlyFire");
            t.friendly_fire         = ff == nullptr || ff->as_bool(true);
            const nbt::Tag* see     = entry.find("SeeFriendlyInvisibles");
            t.see_friendly_invisibles = see == nullptr || see->as_bool(true);
            t.name_tag      = visibility_from_name(string_of(entry, "NameTagVisibility", "always")).value_or(Visibility::Always);
            t.death_message = visibility_from_name(string_of(entry, "DeathMessageVisibility", "always")).value_or(Visibility::Always);
            t.collision     = collision_rule_from_name(string_of(entry, "CollisionRule", "always")).value_or(CollisionRule::Always);
            created->sequence = out.team_order_.insert();
            if (const nbt::Tag* players = entry.find("Players"); players != nullptr && players->list() != nullptr) {
                for (const nbt::Tag& player : *players->list()) {
                    const std::string member{player.as_string()};
                    if (member.empty() || out.team_by_holder_.contains(member)) {
                        continue;
                    }
                    t.members_.push_back({member, t.order_.insert()});
                    out.team_by_holder_[member] = name;
                }
            }
            out.teams_.push_back(std::move(created));
        }
    }
    if (const nbt::Tag* slots = data->find("DisplaySlots"); slots != nullptr && slots->compound() != nullptr) {
        for (const nbt::CompoundEntry& entry : *slots->compound()) {
            constexpr std::string_view kPrefix = "slot_";
            if (!std::string_view{entry.name}.starts_with(kPrefix)) {
                continue;
            }
            const std::string_view digits = std::string_view{entry.name}.substr(kPrefix.size());
            int                    slot   = -1;
            const auto [end, failure]     = std::from_chars(digits.data(), digits.data() + digits.size(), slot);
            if (failure != std::errc{} || end != digits.data() + digits.size() || slot < 0 ||
                slot >= static_cast<int>(out.display_.size())) {
                continue;
            }
            out.display_[static_cast<usize>(slot)] = out.objective(entry.value.as_string());
        }
    }
    out.packets_.clear();
    out.dirty_ = false;
    return out;
}

std::expected<Scoreboard, std::string> read_scoreboard_file(const std::filesystem::path& path,
                                                            const registry::Registries* registries) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        return Scoreboard{};
    }
    const auto bytes = io::read_file(path);
    if (!bytes) {
        return std::unexpected{path.string() + " could not be read"};
    }
    const auto plain = io::gzip_decompress(*bytes);
    if (!plain) {
        return std::unexpected{path.string() + " is not a gzip file"};
    }
    const auto document = nbt::read(*plain);
    if (!document) {
        return std::unexpected{path.string() + " is not NBT"};
    }
    auto loaded = Scoreboard::load(document->root, registries);
    if (!loaded) {
        return std::unexpected{path.string() + ": " + loaded.error()};
    }
    return loaded;
}

bool write_scoreboard_file(const std::filesystem::path& path, const Scoreboard& scoreboard) {
    const auto compressed = io::gzip_compress(nbt::write(nbt::Document{"", scoreboard.save()}));
    if (!compressed) {
        return false;
    }
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    return io::write_file_atomic(path, *compressed).has_value();
}

}  // namespace ov::server
