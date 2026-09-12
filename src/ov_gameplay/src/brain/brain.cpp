#include "ov/gameplay/brain/brain.hpp"

#include "ov/gameplay/mob_logic.hpp"
#include "ov/gameplay/villager.hpp"

#include <algorithm>
#include <limits>

namespace ov::gameplay::brain {
namespace {

constexpr std::array<std::string_view, kActivityCount> kActivityNames{
    "core", "idle",       "work",        "play",  "rest", "meet",
    "panic", "raid",      "pre_raid",    "hide",  "fight", "celebrate",
    "admire_item", "avoid", "ride", "play_dead",
};

// The wiki's schedules ("Villager", Schedules).
constexpr std::array<ScheduleKey, 5> kVillagerDay{{
    {10, Activity::Idle},
    {2000, Activity::Work},
    {9000, Activity::Meet},
    {11000, Activity::Idle},
    {12000, Activity::Rest},
}};
constexpr std::array<ScheduleKey, 5> kBabyDay{{
    {10, Activity::Play},
    {3000, Activity::Idle},
    {6000, Activity::Play},
    {10000, Activity::Idle},
    {12000, Activity::Rest},
}};
constexpr Schedule kVillagerSchedule{kVillagerDay};
constexpr Schedule kBabySchedule{kBabyDay};

}  // namespace

std::string_view activity_name(Activity activity) noexcept {
    return kActivityNames[static_cast<usize>(activity)];
}

Activity Schedule::at(i64 day_time) const noexcept {
    if (keys_.empty()) {
        return Activity::Idle;
    }
    const i64 t    = ((day_time % 24000) + 24000) % 24000;
    Activity  last = keys_.back().activity;  // before the first key: yesterday's last
    for (const ScheduleKey& key : keys_) {
        if (key.tick > t) {
            break;
        }
        last = key.activity;
    }
    return last;
}

const Schedule& villager_schedule() noexcept { return kVillagerSchedule; }
const Schedule& villager_baby_schedule() noexcept { return kBabySchedule; }

// ── Behaviour ───────────────────────────────────────────────────────────────

bool Behavior::requirements_met(const Memories& memories) const noexcept {
    for (const MemoryRequirement& r : requires_) {
        if (memories.has(r.type) != (r.status == MemoryStatus::Present)) {
            return false;
        }
    }
    return true;
}

bool Behavior::try_start(BrainContext& context) {
    if (!requirements_met(context.memories()) || !check_extra_start_conditions(context)) {
        return false;
    }
    running_ = true;
    i32 duration = min_duration_;
    if (max_duration_ > min_duration_ && max_duration_ != kForeverDuration &&
        context.goal.random != nullptr) {
        duration += context.goal.random->next_int(max_duration_ + 1 - min_duration_);
    }
    end_ = duration >= kForeverDuration ? std::numeric_limits<i64>::max()
                                        : context.game_time + duration;
    start(context);
    return true;
}

void Behavior::tick_or_stop(BrainContext& context) {
    if (context.game_time <= end_ && can_still_use(context)) {
        tick(context);
    } else {
        do_stop(context);
    }
}

void Behavior::do_stop(BrainContext& context) {
    running_ = false;
    stop(context);
}

// ── Brain ───────────────────────────────────────────────────────────────────

void Brain::add_sensor(u8 id, i32 interval, math::LegacyRandomSource& random) {
    if (sensor_count_ >= sensors_.size()) {
        return;
    }
    sensors_[sensor_count_++] =
        SensorSlot{id, interval, interval > 0 ? random.next_int(interval) : 0};
}

void Brain::restagger(math::LegacyRandomSource& random) {
    for (usize i = 0; i < sensor_count_; ++i) {
        SensorSlot& s = sensors_[i];
        s.countdown   = s.interval > 0 ? random.next_int(s.interval) : 0;
    }
}

void Brain::add(Activity activity, i32 priority, std::unique_ptr<Behavior> behavior) {
    Entry e{activity, priority, std::move(behavior)};
    const auto at = std::upper_bound(entries_.begin(), entries_.end(), priority,
                                     [](i32 p, const Entry& x) { return p < x.priority; });
    entries_.insert(at, std::move(e));
}

void Brain::require(Activity activity, std::initializer_list<MemoryRequirement> requirements) {
    auto& slots = requirements_[static_cast<usize>(activity)];
    u8    n     = 0;
    for (const MemoryRequirement& r : requirements) {
        if (n < slots.size()) {
            slots[n++] = r;
        }
    }
    requirement_count_[static_cast<usize>(activity)] = n;
}

void Brain::set_core(Activity activity) noexcept {
    core_mask_ |= bit(activity);
    active_mask_ |= bit(activity);
}

bool Brain::requirements_met(Activity activity, const Memories& memories) const noexcept {
    const auto& slots = requirements_[static_cast<usize>(activity)];
    for (u8 i = 0; i < requirement_count_[static_cast<usize>(activity)]; ++i) {
        if (memories.has(slots[i].type) != (slots[i].status == MemoryStatus::Present)) {
            return false;
        }
    }
    return true;
}

void Brain::set_active(Activity activity, BrainContext& context) {
    if (is_active(activity) && current_ == activity) {
        return;
    }
    const u32 kept = core_mask_ | bit(activity);
    for (Entry& e : entries_) {
        if (e.behavior->running() && (kept & bit(e.activity)) == 0) {
            e.behavior->do_stop(context);
        }
    }
    active_mask_ = kept;
    current_     = activity;
}

void Brain::set_active_if_possible(Activity activity, BrainContext& context) {
    set_active(requirements_met(activity, context.memories()) ? activity : default_, context);
}

void Brain::set_active_first_valid(std::span<const Activity> activities, BrainContext& context) {
    for (const Activity a : activities) {
        if (requirements_met(a, context.memories())) {
            set_active(a, context);
            return;
        }
    }
}

void Brain::update_from_schedule(BrainContext& context) {
    if (schedule_ == nullptr ||
        context.game_time - last_schedule_update_ <= kScheduleUpdateInterval) {
        return;
    }
    last_schedule_update_ = context.game_time;
    const Activity wanted = schedule_->at(context.day_time);
    if (!is_active(wanted)) {
        set_active_if_possible(wanted, context);
    }
}

void Brain::tick(BrainContext& context) {
    context.memories().tick();
    if (sense_ != nullptr) {
        for (usize i = 0; i < sensor_count_; ++i) {
            SensorSlot& s = sensors_[i];
            if (--s.countdown <= 0) {
                s.countdown = s.interval;
                sense_(context, s.id);
            }
        }
    }
    for (Entry& e : entries_) {
        if (!e.behavior->running() && is_active(e.activity)) {
            (void)e.behavior->try_start(context);
        }
    }
    for (Entry& e : entries_) {
        if (e.behavior->running()) {
            e.behavior->tick_or_stop(context);
        }
    }
}

void Brain::stop_all(BrainContext& context) {
    for (Entry& e : entries_) {
        if (e.behavior->running()) {
            e.behavior->do_stop(context);
        }
    }
}

Behavior* Brain::find(std::string_view name) noexcept {
    for (Entry& e : entries_) {
        if (e.behavior->name() == name) {
            return e.behavior.get();
        }
    }
    return nullptr;
}

bool Brain::is_running(std::string_view name) const {
    return std::ranges::any_of(entries_, [&](const Entry& e) {
        return e.behavior->running() && e.behavior->name() == name;
    });
}

void Brain::running(std::vector<std::string_view>& out) const {
    out.clear();
    for (const Entry& e : entries_) {
        if (e.behavior->running()) {
            out.push_back(e.behavior->name());
        }
    }
}

// ── BrainGoal ───────────────────────────────────────────────────────────────

void BrainGoal::tick(GoalContext& context) {
    const i64 game = context.villagers != nullptr ? context.villagers->game_time : context.tick;
    const i64 day  = context.villagers != nullptr ? context.villagers->day_time : 6000;
    BrainContext brain_context{context, *brain_, game, day};
    brain_->tick(brain_context);
}

Brain* brain_of(entity::EntityWorld& world, entity::EntityHandle handle) noexcept {
    auto* mob = dynamic_cast<Mob*>(world.logic(handle));
    if (mob == nullptr) {
        return nullptr;
    }
    // The selector is the mob's own; `find` hands its goal back.
    auto* goal = dynamic_cast<BrainGoal*>(const_cast<GoalSelector&>(mob->goals()).find("brain"));
    return goal != nullptr ? &goal->brain() : nullptr;
}

}  // namespace ov::gameplay::brain
