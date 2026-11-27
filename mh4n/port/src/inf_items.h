// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
//
// The rules of the inf_items cheat (hooks.cpp, HunterMods, reads and writes the game's memory): which change of the hunter's
// pouch is a use to put back. Putting back every count that goes down would copy items: what is moved to the box, sold,
// handed in or swapped with another pile goes down too, and the copy is saved. So only this is put back:
//   - while a hunt is on, with its own screen up (no menu, box or reward screen), and not right after one of those;
//   - one slot changed in the frame, and no other (a move, a swap, a combination, the quest taking its supplies back change two
//     or more);
//   - it holds one less of the same item, or it held one and is empty now (the game empties the slot with the last one).
// Everything else is taken as it is.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace Port::InfItems {

// The pouch's 32 slots, as the game's own code walks them (the item bar's next item, 0x00b31fa4, and the code that hands the
// hunter items, 0x006b5e..): first the gunner's pouch, for ammunition and coatings, then the item pouch.
constexpr std::size_t SLOTS = 32;
constexpr std::size_t FIRST_ITEM_SLOT = 8;  // 0..7 the gunner's pouch, 8..31 the item pouch
constexpr std::uint16_t NO_ITEM = 0x4d5;  // (an empty slot, as 0 is)
constexpr std::uint32_t SETTLE_FRAMES = 30;  // of hunting, after anything else, before a drop is taken for a use

struct Slot {
    std::uint16_t id = 0;
    std::int16_t count = 0;
};
using Pouch = std::array<Slot, SLOTS>;

inline bool Empty(const Slot& s) { return s.id == 0 || s.id == NO_ITEM; }
inline bool SameSlot(const Slot& a, const Slot& b) {
    return Empty(a) || Empty(b) ? Empty(a) && Empty(b) : a.id == b.id && a.count == b.count;
}
inline bool OneUsed(const Slot& was, const Slot& now) {
    if (Empty(was) || was.count < 1) return false;
    if (Empty(now)) return was.count == 1;
    return now.id == was.id && now.count == was.count - 1;
}

// What the game puts in the gunner's pouch, whatever weapon the hunter carries: ammunition and coatings (byte 0 of the item's
// row of the item table is 1 or 4). Anything else left there is not taken out again when it is put in the item box, which
// copies it.
inline bool IsAmmunition(std::uint8_t category) { return category == 1 || category == 4; }

// The slot an item handed to the hunter goes to: its own pile if there is one, else the first empty slot; -1 none. Only with
// `gunner_pouch` (ammunition and coatings) are the gunner's slots looked at, and they come first.
inline int SlotFor(const Pouch& pouch, std::uint16_t item, bool gunner_pouch) {
    const std::size_t first = gunner_pouch ? 0 : FIRST_ITEM_SLOT;
    for (std::size_t i = first; i < SLOTS; ++i) {
        if (!Empty(pouch[i]) && pouch[i].id == item) return static_cast<int>(i);
    }
    for (std::size_t i = first; i < SLOTS; ++i) {
        if (Empty(pouch[i])) return static_cast<int>(i);
    }
    return -1;
}

enum class Verdict {
    Same,          // nothing changed (or the first look at this pouch)
    PutBack,       // a use: write `was` back into `slot`
    NotHunting,    // changed in the village, in a menu, with the quest over...
    JustResumed,   // changed right after one of those
    SeveralSlots,  // more than one slot changed
    NotOneUse,     // not one less of the same item: a pick-up, more than one gone, another item
};

struct Change {
    Verdict verdict = Verdict::Same;
    std::size_t slot = 0;  // the first slot that changed
    Slot was, now;
    std::uint32_t changed = 0;  // how many slots did
};

class Keeper {
public:
    // Another hunter, or no hunter: the next pouch seen is taken as it is.
    void Forget() {
        known_ = false;
        calm_ = 0;
    }

    // One frame. `pouch` is what the game holds now; `hunting`: whether a drop now can be the hunter using an item. With
    // PutBack, `pouch[slot]` is already as it must be written.
    Change Step(Pouch& pouch, bool hunting) {
        if (!hunting) {
            calm_ = 0;
        } else if (calm_ <= SETTLE_FRAMES) {
            ++calm_;
        }
        Change change;
        if (!known_) {
            last_ = pouch;
            known_ = true;
            return change;
        }
        for (std::size_t i = 0; i < SLOTS; ++i) {
            if (SameSlot(pouch[i], last_[i])) continue;
            if (change.changed++ == 0) {
                change.slot = i;
                change.was = last_[i];
                change.now = pouch[i];
            }
        }
        if (change.changed != 0) {
            if (!hunting) {
                change.verdict = Verdict::NotHunting;
            } else if (calm_ <= SETTLE_FRAMES) {
                change.verdict = Verdict::JustResumed;
            } else if (change.changed > 1) {
                change.verdict = Verdict::SeveralSlots;
            } else if (!OneUsed(change.was, change.now)) {
                change.verdict = Verdict::NotOneUse;
            } else {
                change.verdict = Verdict::PutBack;
                pouch[change.slot] = change.was;
            }
        }
        last_ = pouch;
        return change;
    }

private:
    Pouch last_{};
    bool known_ = false;
    std::uint32_t calm_ = 0;
};

}  // namespace Port::InfItems
