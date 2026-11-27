// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
//
// The rules of two cheats, apart from the game's memory (hooks.cpp reads and writes it), so that they can be checked alone.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Port::Cheats {

// invincible: a health number is kept at the highest it has been, never above the largest the hunter may have now (which can
// go down, as when what a meal gave ends).
struct KeptHealth {
    std::uint32_t mark = 0;
    void Forget() { mark = 0; }
    // `now`: the number as the game left it this frame; `largest`: the hunter's largest health now. The number it must be.
    std::uint32_t Step(std::uint32_t now, std::uint32_t largest) {
        mark = std::min(mark, largest);
        mark = std::max(mark, now);
        return mark;
    }
};

// damage_mult: what the game takes from a large monster's life in a frame is taken again, times (multiplier - 1).
struct Monster {
    std::uint32_t actor = 0;  // its object's address: the next monster can get the same one
    std::uint32_t kind = 0;
    std::int32_t life = 0, largest = 0;
};

class DamageMultiplier {
public:
    void Forget() { last_.clear(); }

    // One frame: the monsters there are. Each one's `life` is left as it must be written. One is only followed from one frame
    // to the next while it is the same: the same address, kind and largest life, and seen in the frame before.
    void Step(std::vector<Monster>& monsters, float multiplier) {
        if (multiplier <= 1.0f) {  // (switched on in mid fight, it starts from the lives it finds)
            last_.clear();
            return;
        }
        std::unordered_map<std::uint32_t, Monster> now;
        for (Monster& m : monsters) {
            if (m.largest <= 0 || m.life <= 0 || m.life > m.largest) continue;
            const auto before = last_.find(m.actor);
            if (before != last_.end() && before->second.kind == m.kind && before->second.largest == m.largest && m.life < before->second.life) {
                const std::int32_t taken = before->second.life - m.life;
                const auto extra = static_cast<std::int32_t>(std::lround(static_cast<double>(taken) * (multiplier - 1.0f)));
                // (never to 0 by memory: the game's own hit does the killing, so its death sequence runs as always)
                m.life = std::max<std::int32_t>(1, m.life - extra);
            }
            now[m.actor] = m;
        }
        last_ = std::move(now);
    }

private:
    std::unordered_map<std::uint32_t, Monster> last_;
};

}  // namespace Port::Cheats
