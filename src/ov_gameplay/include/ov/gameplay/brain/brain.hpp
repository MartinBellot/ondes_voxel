// A brain: sensors on their own intervals, activities picked by a schedule,
// and behaviours that start when the memories they need are there.
//
// Vanilla drives its villagers, piglins, axolotls, frogs and the warden this
// way instead of with goals. The pieces, and the order of one tick, are the
// Minecraft Wiki's description of the system ("Brain"):
//
//   1. memories forget what has expired        (Memories::tick)
//   2. every sensor whose countdown ran out senses, then waits its interval
//   3. every stopped behaviour of an active activity, in priority order, is
//      offered a start: its memory requirements must hold, then its own
//      extra conditions; it then runs for a duration drawn in [min, max]
//   4. every running behaviour ticks, or stops when its time is up or it can
//      no longer be used
//
// An activity is a set of behaviours. The *core* activities are always
// active; of the others exactly one is, chosen by the schedule for the time
// of day (at most once every 20 ticks) or forced by a behaviour (panic). An
// activity may require memories — work needs a job site, meet a meeting point
// — and when it cannot be entered the default activity is used instead.
//
// ── What differs, and is named ──────────────────────────────────────────────
//
// * Durations and sensor phases draw from the mob's own generator, not the
//   level's shared one: a shared generator makes the world depend on the
//   order mobs are ticked in (CLAUDE.md, determinism).
// * When the activity changes, the behaviours of the one left are stopped at
//   once. Vanilla leaves them to stop on their own conditions; every one of
//   ours that could outlive its activity checks the activity anyway.
//
// ── Cost ────────────────────────────────────────────────────────────────────
//
// Behaviours are built once with the mob; sensors are a fixed array; the tick
// allocates nothing.
#pragma once

#include "ov/base/types.hpp"
#include "ov/gameplay/brain/memory.hpp"
#include "ov/gameplay/goals.hpp"

#include <array>
#include <initializer_list>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace ov::gameplay::brain {

enum class Activity : u8 {
    Core,
    Idle,
    Work,
    Play,
    Rest,
    Meet,
    Panic,
    Raid,
    PreRaid,
    Hide,
    Fight,
    Celebrate,
    AdmireItem,
    Avoid,
    Ride,
    PlayDead,
    Count
};
inline constexpr usize kActivityCount = static_cast<usize>(Activity::Count);
[[nodiscard]] std::string_view activity_name(Activity activity) noexcept;

// ── The schedule ────────────────────────────────────────────────────────────

/// From `tick` on, `activity` — until the next key. Before the first key of a
/// day, the last key of the day before still holds.
struct ScheduleKey {
    i32      tick{0};
    Activity activity{Activity::Idle};
};

class Schedule {
public:
    constexpr explicit Schedule(std::span<const ScheduleKey> keys) noexcept : keys_{keys} {}
    [[nodiscard]] Activity at(i64 day_time) const noexcept;
    [[nodiscard]] std::span<const ScheduleKey> keys() const noexcept { return keys_; }

private:
    std::span<const ScheduleKey> keys_;
};

/// An adult villager's day, the wiki's: idle from 10, work from 2000, meet
/// from 9000, idle from 11000, rest from 12000. Measured transitions:
/// docs/provenance/cerveaux.md.
[[nodiscard]] const Schedule& villager_schedule() noexcept;
/// A baby's: play from 10, idle from 3000, play from 6000, idle from 10000,
/// rest from 12000.
[[nodiscard]] const Schedule& villager_baby_schedule() noexcept;

/// The schedule is consulted at most once in this many ticks.
inline constexpr i64 kScheduleUpdateInterval = 20;

// ── Behaviours ──────────────────────────────────────────────────────────────

enum class MemoryStatus : u8 { Present, Absent };
struct MemoryRequirement {
    MemoryType   type{MemoryType::Home};
    MemoryStatus status{MemoryStatus::Present};
};

class Brain;

/// What a behaviour sees: the goal context (level, entities, self, the mob's
/// state and generator, the caller's world), its brain, and the clock.
struct BrainContext {
    GoalContext& goal;
    Brain&       brain;
    i64          game_time{0};
    i64          day_time{0};

    [[nodiscard]] Memories& memories() noexcept { return goal.brain->memories; }
};

class Behavior {
public:
    /// `needs` must outlive the behaviour (a static array in practice).
    explicit Behavior(std::span<const MemoryRequirement> needs = {}, i32 min_duration = 60,
                      i32 max_duration = 60) noexcept
        : requires_{needs}, min_duration_{min_duration}, max_duration_{max_duration} {}
    Behavior(const Behavior&)            = delete;
    Behavior& operator=(const Behavior&) = delete;
    Behavior(Behavior&&)                 = delete;
    Behavior& operator=(Behavior&&)      = delete;
    virtual ~Behavior()                  = default;

    [[nodiscard]] bool running() const noexcept { return running_; }
    [[nodiscard]] bool requirements_met(const Memories& memories) const noexcept;

    /// Start if the memories and the extra conditions allow. True if started.
    bool try_start(BrainContext& context);
    /// Tick, or stop when the time is up or it can no longer be used.
    void tick_or_stop(BrainContext& context);
    void do_stop(BrainContext& context);

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

protected:
    [[nodiscard]] virtual bool check_extra_start_conditions(BrainContext&) { return true; }
    /// Default false: a behaviour runs its `start` and stops on the next tick.
    [[nodiscard]] virtual bool can_still_use(BrainContext&) { return false; }
    virtual void               start(BrainContext&) {}
    virtual void               tick(BrainContext&) {}
    virtual void               stop(BrainContext&) {}

private:
    std::span<const MemoryRequirement> requires_;
    i32                                min_duration_{60};
    i32                                max_duration_{60};
    bool                               running_{false};
    i64                                end_{0};
};

/// A duration long enough to never time out (vanilla's Integer.MAX_VALUE).
inline constexpr i32 kForeverDuration = 0x7FFFFFFF;

// ── Sensors ─────────────────────────────────────────────────────────────────

/// A sensor is a number the owner understands, an interval, and a countdown.
/// The owner's `SenseFn` does the sensing: which sensors a species has is a
/// table, and what a sensor fills is the species' business.
using SenseFn = void (*)(BrainContext& context, u8 sensor);

struct SensorSlot {
    u8  id{0};
    i32 interval{20};
    i32 countdown{0};
};
inline constexpr usize kMaxSensors = 12;

// ── The brain ───────────────────────────────────────────────────────────────

class Brain {
public:
    Brain() { entries_.reserve(48); }

    /// A sensor, its first countdown drawn in [0, interval): vanilla staggers
    /// its sensors so a hundred villagers do not all scan on one tick.
    void add_sensor(u8 id, i32 interval, math::LegacyRandomSource& random);
    void set_sense(SenseFn sense) noexcept { sense_ = sense; }
    /// Draw every sensor's phase again, from the mob's own generator (a brain
    /// built before the mob had one).
    void restagger(math::LegacyRandomSource& random);

    /// A behaviour of an activity. Lower priority starts first; ties keep
    /// insertion order.
    void add(Activity activity, i32 priority, std::unique_ptr<Behavior> behavior);
    /// Memories an activity needs to be entered (up to three).
    void require(Activity activity, std::initializer_list<MemoryRequirement> requirements);

    void set_core(Activity activity) noexcept;
    void set_default(Activity activity) noexcept { default_ = activity; }
    void set_schedule(const Schedule* schedule) noexcept { schedule_ = schedule; }
    [[nodiscard]] const Schedule* schedule() const noexcept { return schedule_; }

    [[nodiscard]] bool is_active(Activity activity) const noexcept {
        return (active_mask_ & bit(activity)) != 0;
    }
    /// The one non-core activity running.
    [[nodiscard]] Activity current() const noexcept { return current_; }
    [[nodiscard]] bool     requirements_met(Activity activity, const Memories& memories) const noexcept;

    /// Make this the activity, stopping the behaviours of the one left.
    void set_active(Activity activity, BrainContext& context);
    /// This activity if its memories allow, else the default.
    void set_active_if_possible(Activity activity, BrainContext& context);
    /// The first of these whose memories allow.
    void set_active_first_valid(std::span<const Activity> activities, BrainContext& context);
    /// The schedule's activity for the time of day, at most once in
    /// `kScheduleUpdateInterval` ticks, when it is not already active.
    void update_from_schedule(BrainContext& context);

    void tick(BrainContext& context);
    /// Stop everything (a mob that dies, converts or sleeps through a reset).
    void stop_all(BrainContext& context);

    [[nodiscard]] Behavior* find(std::string_view name) noexcept;
    [[nodiscard]] bool      is_running(std::string_view name) const;
    void running(std::vector<std::string_view>& out) const;

private:
    struct Entry {
        Activity                  activity{Activity::Core};
        i32                       priority{0};
        std::unique_ptr<Behavior> behavior;
    };
    [[nodiscard]] static constexpr u32 bit(Activity a) noexcept {
        return 1U << static_cast<u32>(a);
    }

    std::vector<Entry>                    entries_;
    std::array<SensorSlot, kMaxSensors>   sensors_{};
    usize                                 sensor_count_{0};
    SenseFn                               sense_{nullptr};
    std::array<std::array<MemoryRequirement, 3>, kActivityCount> requirements_{};
    std::array<u8, kActivityCount>        requirement_count_{};
    u32                                   core_mask_{0};
    u32                                   active_mask_{0};
    Activity                              current_{Activity::Idle};
    Activity                              default_{Activity::Idle};
    const Schedule*                       schedule_{nullptr};
    i64                                   last_schedule_update_{-kScheduleUpdateInterval - 1};
};

/// A goal that hands the mob's body to its brain: it holds Move and Look and
/// ticks the brain, so a brained mob still runs through `Mob::tick` and its
/// path follower like every other mob. The day time comes from the caller's
/// `VillagerWorld` when there is one.
class BrainGoal final : public Goal {
public:
    explicit BrainGoal(std::unique_ptr<Brain> brain) noexcept : brain_{std::move(brain)} {}
    [[nodiscard]] bool     can_use(GoalContext&) override { return true; }
    void                   tick(GoalContext& context) override;
    [[nodiscard]] GoalFlag flags() const noexcept override { return GoalFlag::Move | GoalFlag::Look; }
    [[nodiscard]] bool     interruptible() const noexcept override { return false; }
    [[nodiscard]] std::string_view name() const noexcept override { return "brain"; }

    [[nodiscard]] Brain&       brain() noexcept { return *brain_; }
    [[nodiscard]] const Brain& brain() const noexcept { return *brain_; }

private:
    std::unique_ptr<Brain> brain_;
};

/// The brain of a mob that has one, or null.
[[nodiscard]] Brain* brain_of(entity::EntityWorld& world, entity::EntityHandle handle) noexcept;

}  // namespace ov::gameplay::brain
