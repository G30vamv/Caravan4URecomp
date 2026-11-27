// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
//
// The life of the large monsters the hunter is fighting, for the Windows shell to draw over the game (read only: nothing here
// changes the game). Every frame the port's hook on sMhMain::frame reads the monsters of the game's sEnemy list (hooks.cpp,
// MonsterLifeStep) and hands their lives to Poll; the shell's present thread takes a Snapshot to draw. The monster that was hurt
// last is the one the panel follows.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

namespace Port::MonsterHp {

// What one frame's read of a monster gives.
struct Sample {
    std::uint32_t actor = 0;  // the monster's object in the guest's memory (a key)
    std::uint32_t kind = 0;   // its row of the game's monster table
    std::int32_t hp = 0;
    std::int32_t max = 0;
    bool dead = false;
};

struct Entry {
    std::uint32_t actor = 0;
    std::uint32_t kind = 0;
    std::int32_t hp = 0;
    std::int32_t max = 0;
    bool dead = false;
    double hit_at = 0.0;      // when its life last went down (seconds, steady clock; 0: never)
    double dead_at = 0.0;     // when it was first seen dead
    int order = 0;            // 1, 2, ... in the order the monsters were first seen
    // what the damage meter shows: every drop of its life since it was first seen is counted (whoever dealt it)
    std::int64_t total_damage = 0;
    int hits = 0;
    std::int32_t last_hit = 0;
    double first_hit_at = 0.0;                          // when its life first went down (0: never)
    std::vector<std::pair<double, std::int32_t>> recent;  // the hits of the last 10 seconds: (time, damage)
};

struct Snapshot {
    std::vector<Entry> list;
    std::uint32_t focus = 0;  // the monster that was hurt last (0: none)
    double now = 0.0;
};

namespace detail {
struct State {
    std::mutex mutex;
    std::vector<Entry> entries;
    std::uint32_t focus = 0;
    int next_order = 1;
};
inline State& S() {
    static State state;
    return state;
}
inline double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace detail

// Off until the shell turns it on (the ini's monster_hp): the frame hook then reads nothing.
inline std::atomic<bool>& Enabled() {
    static std::atomic<bool> enabled{false};
    return enabled;
}

// The monsters of this frame (all of them: the ones not in the list are gone).
inline void Poll(const std::vector<Sample>& samples) {
    const double now = detail::Now();
    auto& s = detail::S();
    std::lock_guard lock(s.mutex);
    std::vector<Entry> next;
    for (const Sample& sample : samples) {
        if (sample.actor == 0 || sample.max <= 0 || sample.hp < 0 || sample.hp > sample.max || next.size() >= 16) continue;
        Entry e;
        const auto old = std::find_if(s.entries.begin(), s.entries.end(), [&](const Entry& x) { return x.actor == sample.actor; });
        if (old != s.entries.end()) {
            e = *old;
            if (sample.hp < old->hp) {
                const std::int32_t dealt = old->hp - sample.hp;
                e.hit_at = now;
                s.focus = sample.actor;
                e.total_damage += dealt;
                ++e.hits;
                e.last_hit = dealt;
                if (e.first_hit_at == 0.0) e.first_hit_at = now;
                e.recent.emplace_back(now, dealt);
            }
            e.recent.erase(e.recent.begin(), std::find_if(e.recent.begin(), e.recent.end(), [&](const auto& h) { return now - h.first <= 10.0; }));
        } else {
            e.actor = sample.actor;
            e.order = s.next_order++;
            if (sample.hp < sample.max) {  // seen already hurt: it is the one being fought
                e.hit_at = now;
                s.focus = sample.actor;
            }
        }
        e.kind = sample.kind;
        e.hp = sample.hp;
        e.max = sample.max;
        if (sample.dead && !e.dead) e.dead_at = now;
        e.dead = sample.dead;
        // a monster that fell leaves the panel a few seconds later
        if (e.dead && now - e.dead_at > 6.0) continue;
        next.push_back(e);
    }
    s.entries = std::move(next);
    if (s.focus != 0 && std::none_of(s.entries.begin(), s.entries.end(), [&](const Entry& e) { return e.actor == s.focus; })) s.focus = 0;
    if (s.entries.empty()) s.next_order = 1;
}

inline Snapshot Take() {
    auto& s = detail::S();
    std::lock_guard lock(s.mutex);
    return Snapshot{s.entries, s.focus, detail::Now()};
}

}  // namespace Port::MonsterHp
