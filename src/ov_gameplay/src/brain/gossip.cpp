#include "ov/gameplay/brain/gossip.hpp"

#include <algorithm>
#include <cmath>

namespace ov::gameplay::brain {
namespace {

// The wiki's table (Villager, Gossiping); see gossip.hpp.
constexpr std::array<GossipTypeInfo, kGossipTypeCount> kTypes{{
    {"major_negative", -5, 100, 10, 10},
    {"minor_negative", -1, 200, 20, 20},
    {"minor_positive", 1, 200, 1, 5},
    {"major_positive", 5, 100, 0, 100},
    {"trading", 1, 25, 2, 20},
}};

}  // namespace

const GossipTypeInfo& gossip_info(GossipType type) noexcept {
    return kTypes[static_cast<usize>(type)];
}

std::optional<GossipType> gossip_type_from_name(std::string_view name) noexcept {
    if (name.starts_with("minecraft:")) {
        name.remove_prefix(10);
    }
    for (usize i = 0; i < kTypes.size(); ++i) {
        if (kTypes[i].name == name) {
            return static_cast<GossipType>(i);
        }
    }
    return std::nullopt;
}

GossipEntry* Gossips::find(const net::Uuid& target, GossipType type) noexcept {
    for (GossipEntry& e : entries_) {
        if (e.type == type && e.target == target) {
            return &e;
        }
    }
    return nullptr;
}

const GossipEntry* Gossips::find(const net::Uuid& target, GossipType type) const noexcept {
    for (const GossipEntry& e : entries_) {
        if (e.type == type && e.target == target) {
            return &e;
        }
    }
    return nullptr;
}

void Gossips::put(const GossipEntry& entry) {
    if (GossipEntry* e = find(entry.target, entry.type)) {
        e->value = entry.value;
        return;
    }
    entries_.push_back(entry);
}

void Gossips::add(const net::Uuid& target, GossipType type, i32 amount) {
    const i32 max = gossip_info(type).max;
    if (GossipEntry* e = find(target, type)) {
        const i32 sum = e->value + amount;
        e->value      = sum > max ? std::max(max, e->value) : sum;
        return;
    }
    entries_.push_back(GossipEntry{target, type, std::min(amount, max)});
}

void Gossips::add_event(const net::Uuid& target, ReputationEvent event) {
    switch (event) {
        case ReputationEvent::ZombieVillagerCured:
            add(target, GossipType::MajorPositive, 20);
            add(target, GossipType::MinorPositive, 25);
            break;
        case ReputationEvent::Trade:
            add(target, GossipType::Trading, 2);
            break;
        case ReputationEvent::VillagerHurt:
            add(target, GossipType::MinorNegative, 25);
            break;
        case ReputationEvent::VillagerKilled:
        case ReputationEvent::GolemKilled:
            add(target, GossipType::MajorNegative, 25);
            break;
    }
}

i32 Gossips::reputation(const net::Uuid& target) const noexcept {
    i32 sum = 0;
    for (const GossipEntry& e : entries_) {
        if (e.target == target) {
            sum += e.value * gossip_info(e.type).weight;
        }
    }
    return sum;
}

i32 Gossips::value(const net::Uuid& target, GossipType type) const noexcept {
    const GossipEntry* e = find(target, type);
    return e != nullptr ? e->value : 0;
}

void Gossips::decay() {
    for (GossipEntry& e : entries_) {
        e.value -= gossip_info(e.type).decay_per_day;
    }
    std::erase_if(entries_, [](const GossipEntry& e) { return e.value < kGossipKeepAtLeast; });
}

void Gossips::transfer_from(const Gossips& other, math::LegacyRandomSource& random, i32 picks) {
    const std::span<const GossipEntry> from = other.entries();
    if (from.empty() || picks <= 0) {
        return;
    }
    // Cumulative |value × weight|: the draw lands on an entry with a chance
    // proportional to its weight in the reputation.
    i32 total = 0;
    for (const GossipEntry& e : from) {
        total += std::abs(e.value * gossip_info(e.type).weight);
    }
    if (total <= 0) {
        return;
    }
    // At most 16 distinct picks are remembered without allocating; a draw
    // that repeats a pick is a draw that adds nothing, as with a set.
    std::array<usize, 16> chosen{};
    usize                 count = 0;
    for (i32 k = 0; k < picks; ++k) {
        const i32 roll = random.next_int(total);
        i32       run  = 0;
        usize     hit  = from.size() - 1;
        for (usize i = 0; i < from.size(); ++i) {
            run += std::abs(from[i].value * gossip_info(from[i].type).weight);
            if (roll < run) {
                hit = i;
                break;
            }
        }
        if (count < chosen.size() &&
            std::find(chosen.begin(), chosen.begin() + static_cast<std::ptrdiff_t>(count), hit) ==
                chosen.begin() + static_cast<std::ptrdiff_t>(count)) {
            chosen[count++] = hit;
        }
    }
    for (usize k = 0; k < count; ++k) {
        const GossipEntry& e     = from[chosen[k]];
        const i32          heard = e.value - gossip_info(e.type).decay_per_transfer;
        if (heard < kGossipKeepAtLeast) {
            continue;
        }
        if (GossipEntry* mine = find(e.target, e.type)) {
            mine->value = std::max(mine->value, heard);
        } else {
            entries_.push_back(GossipEntry{e.target, e.type, heard});
        }
    }
}

bool maybe_decay(Gossips& gossips, i64& last_decay, i64 game_time) {
    if (last_decay == 0) {
        last_decay = game_time;
        return false;
    }
    if (game_time < last_decay + kGossipDecayInterval) {
        return false;
    }
    gossips.decay();
    last_decay = game_time;
    return true;
}

i32 reputation_price_diff(i32 reputation, f32 multiplier) noexcept {
    return -static_cast<i32>(std::floor(static_cast<f32>(reputation) * multiplier));
}

i32 hero_price_diff(i32 amplifier, i32 base_count) noexcept {
    const f64 share = 0.3 + 0.0625 * static_cast<f64>(amplifier);
    return -std::max(1, static_cast<i32>(std::floor(share * static_cast<f64>(base_count))));
}

}  // namespace ov::gameplay::brain
