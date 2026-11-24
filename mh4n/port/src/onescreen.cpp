// One screen (single_screen in mh4u.ini): the game's touch-screen menus drawn on the top screen, by the game.
//
// The game draws both screens with one GUI manager (sMhLayout's uLayout): each root group of a layout carries the
// screen it is drawn on (bits 14-17 of group + 0x44: 0 the top one, 7 the touch one), and the layout pass of each
// screen draws the groups of its own. So a group of a touch-screen menu sent to the top screen is drawn there, at the
// game's own resolution, by the game's own code; this file only says where (docs/research/onescreen.md):
//
//  * once a frame, before the game's frame (Frame), it walks the manager's groups, finds the screen of Heavenfall's
//    arrangement in force from the groups shown (port/src/onescreen_table.h, made by tools/onescreen_table.py from
//    config/onescreen.json and the layouts of the player's game), and writes each placed group's screen (and its draw
//    priority); what it changes it keeps a note of, and gives back as the game had it when the group is no longer
//    placed or the option goes off;
//  * as each group is drawn on the top screen (ObjectConstants, from nLayout::objectConstants), a placed group gets a
//    copy of the layout pass's constants whose projection scales and moves it: the group's every pane, animated or set
//    by the game's code, follows, and nothing of the game's own state (positions, layouts, files) changes.
//
// Nothing is read from or written to the game's files: the layouts are the player's own, in any language, as the game
// loads them. The arrangement is Heavenfall's (popzol and BlitzWOLF, GPL-3.0), without its gameplay changes.
#include "onescreen.h"

#include "game_addr.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>
#include <fmt/format.h>

extern "C" {
#include "recomp.h"
}

namespace Port::OneScreen {

namespace Table {
struct Name {
    u32 hash;
    u16 block;
};
struct Place {
    u8 flags;
    s16 priority;  // set, or added to the game's (a negative one sends a group back)
    float scale;
    float tx[2];  // by the group's own screen (0 the top one, 1 the touch one)
    float ty;
};
struct Screen {
    u8 channel;
    u16 all_first, all_count, any_first, any_count, none_first, none_count, entry_first, entry_count;
};
struct Entry {
    u16 block;
    Place place;
};
struct Hide {
    u32 group, pane;  // name hashes
};
}  // namespace Table
}  // namespace Port::OneScreen

#include "onescreen_table.h"

namespace Port::OneScreen {

namespace {

using Table::Place;

// Place flags (tools/onescreen_table.py)
constexpr u8 F_TOP = 1, F_HIDDEN = 2, F_KEEP = 4, F_PRIORITY_SET = 8, F_PRIORITY_ADD = 16, F_COMPOSE = 32, F_BOTTOM = 64;
constexpr int kChannels = 3;
constexpr u32 PSEUDO_PROMPT = 1, PSEUDO_QUEST = 2;  // kNames' pseudo names: a yes / no prompt is open, a quest's scene

// (game addresses: GameAddr, port/src/game_addr.h and config/port_addresses.txt; USA v1.3.0's values)
GameAddr SMHLAYOUT_INSTANCE{0x01057534};        // sMhLayout*
constexpr u32 SMHLAYOUT_MAIN = 0x100;           // uLayout*: the one that holds every loaded layout
constexpr u32 ULAYOUT_GROUPS = 0x154;           // group*[]
constexpr u32 ULAYOUT_GROUP_COUNT = 0x160;
constexpr u32 GROUP_ORDER = 0x18;               // bits 19-26 the draw priority
constexpr u32 GROUP_FLAGS = 0x44;               // 0x400 shown, bits 14-17 the screen (view)
constexpr u32 GROUP_SHOWN = 0x400;
constexpr u32 VIEW_SHIFT = 14, VIEW_MASK = 0xFu << VIEW_SHIFT;
constexpr u32 VIEW_TOP = 0, VIEW_TOUCH = 7, VIEW_NONE = 15;  // (15: a view no camera draws)
constexpr u32 PRIORITY_SHIFT = 19, PRIORITY_MASK = 0xFFu << PRIORITY_SHIFT;
GameAddr FN_GROUP_CHANGED{0x00ae655c};          // (group): the draw list and its caches follow a new priority
// The yes / no box: ui006's owner (static), its button slots' kinds (u8, two apart; 3 "Yes", 6 "No") and whether its
// groups are bound (Heavenfall's single_screen_prompt.s, FUN_002921cc)
GameAddr PROMPT_OWNER{0x0109cdc8};
constexpr u32 PROMPT_KIND0 = 0x1f8, PROMPT_KIND1 = 0x1fa, PROMPT_BOUND = 0x21f;
GameAddr GAME_CONTROL{0x00fb6b7c};              // sGameControl*
constexpr u32 GAME_CONTROL_MODE = 0x2b;           // u8: 1 a quest, 5 its results
// The layout pass's constants (sLayout::beginViewport 0x00af3d60 makes them, cDraw + 0x824 points at them): its view
// and projection matrices, column by column; the projection's first two rows map layout units to the rotated target's
// x (the screen's height) and y (its width). The draw's constant room: cDraw + 0x14 the next free byte, + 0x18 the end.
constexpr u32 CDRAW_SHADER = 0x08, CDRAW_ROOM = 0x14, CDRAW_ROOM_END = 0x18, CDRAW_VIEWPORT = 0x824,
              CDRAW_DIRTY = 0x188;
constexpr u32 SHADER_VIEWPORT_LAYOUT = 0x614;   // the shader's description of that block: its size in words at + 0x14
constexpr u32 PROJ = 16;                        // the projection's first float in the block
// A group's panes (Heavenfall's game_gui.h): + 0x08 the group's first; a pane's + 0x00 name hash, + 0x08 its kind's data,
// + 0x0C its draw node (+ 0x12 = 3 redraws it), + 0x10 its kind (0 sprite, 1 null, 2 text), + 0x14 the next, + 0x20 its
// first child. Shown: a sprite's data + 0x58 bit 7, a null's + 0x24 bit 0, a text's + 0x5C bit 28.
constexpr u32 GROUP_FIRST_PANE = 0x08, PANE_DATA = 0x08, PANE_NODE = 0x0c, PANE_KIND = 0x10, PANE_NEXT = 0x14,
              PANE_CHILD = 0x20;
GameAddr FN_PANE_REDRAW{0x00ae6b78};            // (group, pane): a pane with no draw node redrawn

float F(RCtx* ctx, u32 a) {
    const u32 v = rd32(ctx, a);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
void WF(RCtx* ctx, u32 a, float f) {
    u32 v;
    std::memcpy(&v, &f, 4);
    wr32(ctx, a, v);
}

/// A game function, called from the hook's context (as hooks.cpp's CallGame).
u32 CallGame(RCtx* ctx, u32 function, u32 arg, u32 arg1 = 0) {
    u32 regs[16];
    std::copy(std::begin(ctx->r), std::end(ctx->r), regs);
    const u32 flags[4] = {ctx->fn, ctx->fz, ctx->fc, ctx->fv};
    const u32 expect_next = ctx->expect_next;
    ctx->expect_next = 0;
    ctx->r[0] = arg;
    ctx->r[1] = arg1;
    ctx->r[14] = function;
    rc_call(ctx, function);
    const u32 result = ctx->r[0];
    std::copy(std::begin(regs), std::end(regs), ctx->r);
    ctx->fn = flags[0];
    ctx->fz = flags[1];
    ctx->fc = flags[2];
    ctx->fv = flags[3];
    ctx->expect_next = expect_next;
    return result;
}

/// What this file changed of a group: its screen and priority as the game last had them, and what was written.
struct Note {
    u32 hash = 0;  // the group's name: another layout's group at the same address starts a note of its own
    u32 game_view = 0, written_view = 0;
    u32 game_priority = 0, written_priority = 0;
    bool view = false, priority = false;
};
std::unordered_map<u32, Note> notes;  // group address -> note

/// The move of a group drawn on the top screen this frame.
struct Move {
    float scale, tx, ty;
};
std::unordered_map<u32, Move> moves;  // group address -> move (this frame's)

/// The layout pass's constants a moved group was given (ObjectConstants), to be given back for the next group.
struct Swap {
    u32 draw = 0, game = 0, ours = 0;
} swap;

bool was_on = false;
std::atomic<bool> hide_help{false};
std::atomic<bool> map_tap_ok{false};  // MapTapAvailable()
std::atomic<bool> map_open{false};    // MapOpen()
/// A layout name's hash: the CRC-32 of the name without its final inversion (as hooks.cpp's LayoutNameHash).
constexpr u32 NameHash(const char* name) {
    u32 crc = 0xFFFFFFFF;
    for (; *name; ++name) {
        crc ^= static_cast<u8>(*name);
        for (int b = 0; b < 8; ++b) crc = crc & 1 ? (crc >> 1) ^ 0xEDB88320 : crc >> 1;
    }
    return crc;
}
constexpr u32 HELP_GROUPS[2] = {NameHash("ui006_help00"), NameHash("ui006_help01")};
int no_room_logged = 0;
int rooms_made = 0;
constexpr u32 MOVED_CACHE = 16;
u32 moved_cache[MOVED_CACHE] = {};
u32 moved_cache_end[MOVED_CACHE] = {};  // the room's end each copy was made in: copies are only reused inside the same room
constexpr u32 FN_MAKE_ROOM = 0x005deb68;  // the game's own: cDraw (r0) takes a new page-sized room for r1 bytes when the room is full
u32 moved_next = 0;
std::string pending_log;  // a line for the log, said by the next Frame
u32 frames_on = 0;
std::string last_screens;

int NameIndex(u32 hash) {
    const auto* begin = std::begin(Table::kNames);
    const auto* end = std::end(Table::kNames);
    const auto* it = std::lower_bound(begin, end, hash, [](const Table::Name& n, u32 h) { return n.hash < h; });
    return it != end && it->hash == hash ? static_cast<int>(it - begin) : -1;
}

bool Has(const std::vector<bool>& shown, u16 first, u16 count, bool all) {
    for (u16 i = 0; i < count; ++i) {
        const bool on = shown[Table::kTerms[first + i]];
        if (all && !on) return false;
        if (!all && on) return true;
    }
    return all;
}

/// `entry` on top of `base`: the entry's map after the base's (a frame or a shift of a place already placed).
Place Compose(const Place& entry, const Place& base) {
    Place p = base;
    if (entry.flags & (F_TOP | F_HIDDEN | F_BOTTOM)) {
        p.flags = static_cast<u8>((p.flags & ~(F_TOP | F_HIDDEN | F_BOTTOM | F_KEEP)) | (entry.flags & (F_TOP | F_HIDDEN | F_BOTTOM)));
    }
    if (entry.flags & (F_PRIORITY_SET | F_PRIORITY_ADD)) {
        p.flags = static_cast<u8>((p.flags & ~(F_PRIORITY_SET | F_PRIORITY_ADD)) |
                                  (entry.flags & (F_PRIORITY_SET | F_PRIORITY_ADD)));
        p.priority = entry.priority;
    }
    p.scale = entry.scale * base.scale;
    for (int own = 0; own < 2; ++own) p.tx[own] = entry.scale * base.tx[own] + entry.tx[own];
    p.ty = entry.scale * base.ty + entry.ty;
    return p;
}

/// Gives back `group`'s screen as the game last had it, if it is still what was written.
void GiveBackView(RCtx* ctx, u32 group, Note& note) {
    if (!note.view) return;
    const u32 flags = rd32(ctx, group + GROUP_FLAGS);
    if ((flags & VIEW_MASK) == note.written_view << VIEW_SHIFT) {
        wr32(ctx, group + GROUP_FLAGS, (flags & ~VIEW_MASK) | note.game_view << VIEW_SHIFT);
        wr8(ctx, group + 0x42, 3);
    }
    note.view = false;
}

/// The same for its draw priority.
void GiveBackPriority(RCtx* ctx, u32 group, Note& note) {
    if (!note.priority) return;
    const u32 order = rd32(ctx, group + GROUP_ORDER);
    if ((order & PRIORITY_MASK) >> PRIORITY_SHIFT == note.written_priority) {
        wr32(ctx, group + GROUP_ORDER, (order & ~PRIORITY_MASK) | note.game_priority << PRIORITY_SHIFT);
        CallGame(ctx, FN_GROUP_CHANGED, group);
    }
    note.priority = false;
}

/// A pane hidden while its group is placed (kHides), to be shown again after.
struct Hidden {
    u32 group = 0, hash = 0;
    bool seen = false;  // its group was walked this frame
    bool kept = false;  // and it is still to be hidden
};
std::unordered_map<u32, Hidden> hidden;  // pane address -> note

/// A pane's "shown" bit: where it is in its data, and the bit (0: a kind that has none).
u32 ShownBit(RCtx* ctx, u32 pane, u32* at) {
    const u32 data = rd32(ctx, pane + PANE_DATA);
    if (data == 0) return 0;
    switch (rd8(ctx, pane + PANE_KIND)) {
    case 0: *at = data + 0x58; return 0x80;        // a sprite
    case 1: *at = data + 0x24; return 0x1;         // a null
    case 2: *at = data + 0x5c; return 1u << 28;    // a text
    default: return 0;
    }
}

void Redraw(RCtx* ctx, u32 group, u32 pane) {
    if (const u32 node = rd32(ctx, pane + PANE_NODE)) {
        wr8(ctx, node + 0x12, 3);
    } else {
        CallGame(ctx, FN_PANE_REDRAW, group, pane);
    }
}

void ShowAgain(RCtx* ctx, u32 pane, const Hidden& h) {
    u32 at = 0;
    if (rd32(ctx, pane) != h.hash) return;
    if (const u32 bit = ShownBit(ctx, pane, &at); bit != 0 && !(rd32(ctx, at) & bit)) {
        wr32(ctx, at, rd32(ctx, at) | bit);
        Redraw(ctx, h.group, pane);
    }
}

/// The panes of `group` the table hides while it is placed: hidden (each frame: the game may show one again).
void HidePanes(RCtx* ctx, u32 group) {
    const u32 hash = rd32(ctx, group);
    const auto* first = std::lower_bound(std::begin(Table::kHides), std::end(Table::kHides), hash,
                                         [](const Table::Hide& h, u32 g) { return h.group < g; });
    if (first == std::end(Table::kHides) || first->group != hash) return;
    u32 stack[8];
    int depth = 0;
    u32 pane = rd32(ctx, group + GROUP_FIRST_PANE);
    for (int walked = 0; pane != 0 && walked < 4096; ++walked) {
        const u32 name = rd32(ctx, pane);
        for (const auto* h = first; h != std::end(Table::kHides) && h->group == hash; ++h) {
            if (h->pane != name) continue;
            u32 at = 0;
            const u32 bit = ShownBit(ctx, pane, &at);
            if (bit == 0) break;
            Hidden& note = hidden[pane];
            note.group = group, note.hash = name, note.seen = note.kept = true;
            if (rd32(ctx, at) & bit) {
                wr32(ctx, at, rd32(ctx, at) & ~bit);
                Redraw(ctx, group, pane);
            }
            break;
        }
        // depth first: its children, then the next pane, then the next of a parent
        if (const u32 child = rd32(ctx, pane + PANE_CHILD); child != 0 && depth < 8) {
            stack[depth++] = rd32(ctx, pane + PANE_NEXT);
            pane = child;
            continue;
        }
        pane = rd32(ctx, pane + PANE_NEXT);
        while (pane == 0 && depth > 0) pane = stack[--depth];
    }
}

/// Every group back as the game had it (the option went off).
void GiveAllBack(RCtx* ctx) {
    const u32 layout = rd32(ctx, SMHLAYOUT_INSTANCE);
    const u32 main = layout ? rd32(ctx, layout + SMHLAYOUT_MAIN) : 0;
    const u32 list = main ? rd32(ctx, main + ULAYOUT_GROUPS) : 0;
    const u32 count = main ? std::min<u32>(rd32(ctx, main + ULAYOUT_GROUP_COUNT), 0x2000) : 0;
    for (u32 i = 0; list != 0 && i < count; ++i) {
        const u32 group = rd32(ctx, list + 4 * i);
        auto it = notes.find(group);
        if (it != notes.end()) {
            GiveBackView(ctx, group, it->second);
            GiveBackPriority(ctx, group, it->second);
        }
        for (const auto& [pane, h] : hidden) {
            if (h.group == group) ShowAgain(ctx, pane, h);
        }
    }
    notes.clear();
    moves.clear();
    hidden.clear();
}

}  // namespace

bool MapOpen() {
    return map_open.load(std::memory_order_relaxed);
}

bool MapTapAvailable() {
    return map_tap_ok.load(std::memory_order_relaxed);
}

void SetHideHelp(bool hide) {
    hide_help.store(hide);
}

std::string Frame(RCtx* ctx, bool on) {
    if (!on) {
        if (was_on) {
            GiveAllBack(ctx);
            was_on = false;
            return "one screen: off, the touch screen's menus back on it";
        }
        return {};
    }
    const u32 layout = rd32(ctx, SMHLAYOUT_INSTANCE);
    const u32 main = layout ? rd32(ctx, layout + SMHLAYOUT_MAIN) : 0;
    const u32 list = main ? rd32(ctx, main + ULAYOUT_GROUPS) : 0;
    const u32 count = main ? rd32(ctx, main + ULAYOUT_GROUP_COUNT) : 0;
    moves.clear();
    if (list == 0 || count == 0 || count > 0x2000) return {};
    std::string line = std::move(pending_log);
    pending_log.clear();
    if (!was_on) {
        was_on = true;
        line = fmt::format("one screen: on ({} groups placed by {} rules)", Table::kBlockCount, std::size(Table::kScreens));
    }
    ++frames_on;

    // the names shown, and the pseudo names
    std::vector<bool> shown(Table::kNameCount, false);
    std::vector<std::pair<u32, int>> placed;  // (group, block)
    placed.reserve(256);
    std::vector<u32> shown_names;  // (the hash of every group shown, known to the table or not)
    std::vector<u32> unknown_shown;  // (shown groups the table does not know: logged on the fallback screen, to tell a menu apart)
    for (u32 i = 0; i < count; ++i) {
        const u32 group = rd32(ctx, list + 4 * i);
        if (group == 0) continue;
        const int n = NameIndex(rd32(ctx, group));
        if (rd32(ctx, group + GROUP_FLAGS) & GROUP_SHOWN) shown_names.push_back(rd32(ctx, group));
        if (n < 0) {
            if (rd32(ctx, group + GROUP_FLAGS) & GROUP_SHOWN) unknown_shown.push_back(rd32(ctx, group));
            continue;
        }
        if (rd32(ctx, group + GROUP_FLAGS) & GROUP_SHOWN) shown[n] = true;
        if (Table::kNames[n].block != 0xFFFF) placed.emplace_back(group, Table::kNames[n].block);
    }
    if (const int n = NameIndex(PSEUDO_PROMPT); n >= 0) {
        shown[n] = rd8(ctx, PROMPT_OWNER + PROMPT_BOUND) != 0 && rd8(ctx, PROMPT_OWNER + PROMPT_KIND0) == 3 &&
                   rd8(ctx, PROMPT_OWNER + PROMPT_KIND1) == 6;
    }
    if (const int n = NameIndex(PSEUDO_QUEST); n >= 0) {
        const u32 control = rd32(ctx, GAME_CONTROL);
        const u32 mode = control ? rd8(ctx, control + GAME_CONTROL_MODE) : 0;
        shown[n] = mode == 1 || mode == 5;
    }

    // the village menu (Bag, Combine, Recipes...): its four groups, which the table does not know (seen in the log of 2026-11-24)
    auto shown_unknown = [&](u32 hash) { return std::find(shown_names.begin(), shown_names.end(), hash) != shown_names.end(); };
    const bool village_menu = shown_unknown(0x688A1837u) && shown_unknown(0x285C518Du) && shown_unknown(0x5FDCF4A2u) &&
                              shown_unknown(0x44C3F6EBu);

    // the screen in force on each channel: the first whose rule holds
    int chosen[kChannels] = {-1, -1, -1};
    for (int s = 0; s < static_cast<int>(std::size(Table::kScreens)); ++s) {
        const Table::Screen& sc = Table::kScreens[s];
        if (sc.channel >= kChannels || chosen[sc.channel] >= 0) continue;
        if (Has(shown, sc.all_first, sc.all_count, true) &&
            (sc.any_count == 0 || Has(shown, sc.any_first, sc.any_count, false)) &&
            !Has(shown, sc.none_first, sc.none_count, false)) {
            chosen[sc.channel] = s;
        }
    }
    // the village's plain screen (no touch menu in force, so the right strip rules) with the touch screen's four groups, out of a quest:
    // the map icon is there to tap (the log of 2026-11-24: a tap at 286, 165 opens the world map)
    const bool on_right_strip = chosen[2] == static_cast<int>(std::size(Table::kScreens)) - 2;
    const int quest_name = NameIndex(PSEUDO_QUEST);
    // (open: the world map screen holds and the map itself is shown: the search settings of the same screen are not the map)
    map_open.store(chosen[2] == static_cast<int>(std::size(Table::kScreens)) - 16 && shown_unknown(0x0A4A3626u), std::memory_order_relaxed);
    map_tap_ok.store(on_right_strip && village_menu && !(quest_name >= 0 && shown[quest_name]), std::memory_order_relaxed);
    // the world map open: every 3 s of frames (6 times) the groups shown, as the buttons below the map appear after the screen changes
    if (chosen[2] == static_cast<int>(std::size(Table::kScreens)) - 16) {
        static int map_frames = 0, map_lines = 0;
        if (++map_frames % 180 == 90 && map_lines < 6 && line.empty()) {
            ++map_lines;
            line = "one screen: world map shown:";
            for (const auto& [group, block] : placed) {
                if (rd32(ctx, group + GROUP_FLAGS) & GROUP_SHOWN) line += fmt::format(" {:08X}/{}", rd32(ctx, group), block);
            }
            line += " | other:";
            for (const u32 name : unknown_shown) line += fmt::format(" {:08X}", name);
        }
    }
    // (the log says which screens are in force when they change: the first 200 times, so a menu that goes back and forth
    // can't fill it)
    const std::string screens = fmt::format("{} {} {}", chosen[0], chosen[1], chosen[2]);
    if (screens != last_screens) {
        last_screens = screens;
        static int logged = 0;
        if (line.empty() && logged < 200) {
            line = "one screen: screens " + screens + (++logged == 200 ? " (no more of these)" : "");
            if (chosen[2] == static_cast<int>(std::size(Table::kScreens)) - 16) {  // (the world map: which groups are shown, to place its buttons)
                static int map_logged = 0;
                if (map_logged < 6) {
                    ++map_logged;
                    line += " | shown:";
                    for (const auto& [group, block] : placed) {
                        if (rd32(ctx, group + GROUP_FLAGS) & GROUP_SHOWN) line += fmt::format(" {:08X}/{}", rd32(ctx, group), block);
                    }
                    line += " | other:";
                    for (const u32 name : unknown_shown) line += fmt::format(" {:08X}", name);
                }
            }
            if (chosen[2] == static_cast<int>(std::size(Table::kScreens)) - 2) {
                static int groups_logged = 0;
                if (groups_logged < 40) {
                    ++groups_logged;
                    line += " | other groups shown:";
                    for (const u32 name : unknown_shown) line += fmt::format(" {:08X}", name);
                }
            }
        }
    }

    for (const auto& [group, block] : placed) {
        Place place = Table::kDefaults[block];
        for (int c = 0; c < kChannels; ++c) {
            if (chosen[c] < 0) continue;
            const Table::Screen& sc = Table::kScreens[chosen[c]];
            const Table::Entry* first = Table::kEntries + sc.entry_first;
            const Table::Entry* last = first + sc.entry_count;
            const auto* it = std::lower_bound(first, last, block,
                                              [](const Table::Entry& e, int b) { return e.block < b; });
            if (it != last && it->block == block) {
                place = it->place.flags & F_COMPOSE ? Compose(it->place, place) : it->place;
            }
        }
        // The description panel (ui006_help) of the village menu and of the world map is drawn at half its size, its bottom edge in the
        // middle kept where it was. A group's pixels go to centre + scale * (p - centre) + (tx, ty), the centre being the top screen's
        // (200, 120), p the group's own (the panel's bottom middle is at 160, 189 on the touch screen's 320 x 240, and its left edge at
        // 41.5: measured on the game, it fits the places seen). The village menu's goes to the left edge as well (5 px), so it does not
        // cover the menu's last rows.
        if (const u32 name = rd32(ctx, group); name == HELP_GROUPS[0] || name == HELP_GROUPS[1]) {
            const bool world_map = chosen[2] == static_cast<int>(std::size(Table::kScreens)) - 16;
            if ((village_menu && on_right_strip) || world_map) {
                const float smaller = place.scale * 0.5f, d = place.scale - smaller;
                for (float& x : place.tx) x += d * (160.0f - 200.0f);
                place.ty += d * (189.0f - 120.0f);
                place.scale = smaller;
                if (world_map) {
                    place.ty += 16.0f;  // (below the bar of buttons, which sits right under the map)
                } else {
                    for (float& x : place.tx) x = 5.0f - (200.0f + smaller * (41.5f - 200.0f));
                }
            }
        }
        if (hide_help.load(std::memory_order_relaxed)) {
            const u32 name = rd32(ctx, group);
            if (name == HELP_GROUPS[0] || name == HELP_GROUPS[1]) {
                place.flags = static_cast<u8>((place.flags & ~(F_TOP | F_BOTTOM)) | F_HIDDEN);
            }
        }
        Note& note = notes[group];
        if (const u32 hash = rd32(ctx, group); note.hash != hash) note = Note{hash};
        const u32 flags = rd32(ctx, group + GROUP_FLAGS);
        const u32 view = (flags & VIEW_MASK) >> VIEW_SHIFT;
        if (!note.view || view != note.written_view) note.game_view = view;  // (the game's own, or it set it again)
        u32 want = note.game_view;
        if (place.flags & F_HIDDEN) {
            want = VIEW_NONE;
        } else if (place.flags & F_TOP) {
            want = VIEW_TOP;
        } else if (place.flags & F_BOTTOM) {
            want = VIEW_TOUCH;
        }
        if (want != note.game_view) {
            if (view != want) {
                wr32(ctx, group + GROUP_FLAGS, (flags & ~VIEW_MASK) | want << VIEW_SHIFT);
                wr8(ctx, group + 0x42, 3);
            }
            note.view = true;
            note.written_view = want;
        } else {
            GiveBackView(ctx, group, note);
        }
        // the draw priority: the place's, or the game's plus the place's
        const u32 order = rd32(ctx, group + GROUP_ORDER);
        const u32 priority = (order & PRIORITY_MASK) >> PRIORITY_SHIFT;
        if (!note.priority || priority != note.written_priority) note.game_priority = priority;
        if (want == VIEW_TOP && (place.flags & (F_PRIORITY_SET | F_PRIORITY_ADD))) {
            const int wanted = place.flags & F_PRIORITY_SET ? place.priority
                                                            : static_cast<int>(note.game_priority) + place.priority;
            const u32 target = static_cast<u32>(std::clamp(wanted, 0, 0xFF));
            if (priority != target) {
                wr32(ctx, group + GROUP_ORDER, (order & ~PRIORITY_MASK) | target << PRIORITY_SHIFT);
                CallGame(ctx, FN_GROUP_CHANGED, group);
            }
            note.priority = true;
            note.written_priority = target;
        } else {
            GiveBackPriority(ctx, group, note);
        }
        if (want == VIEW_TOP && (place.scale != 1.0f || place.tx[0] != 0 || place.tx[1] != 0 || place.ty != 0)) {
            const int own = note.game_view == VIEW_TOUCH ? 1 : 0;
            moves[group] = Move{place.scale, place.tx[own], place.ty};
        }
        for (auto& [pane, h] : hidden) {
            if (h.group == group) h.seen = true;
        }
        if (want == VIEW_TOP) HidePanes(ctx, group);
    }
    // the panes hidden before: shown again once their group is no longer on the top screen (forgotten if it is gone)
    for (auto it = hidden.begin(); it != hidden.end();) {
        Hidden& h = it->second;
        if (h.seen && h.kept) {
            h.seen = h.kept = false;
            ++it;
            continue;
        }
        if (h.seen) ShowAgain(ctx, it->first, h);
        it = hidden.erase(it);
    }
    return line;
}

void ObjectConstants(RCtx* ctx, u32 object, u32 draw, u32 constants, u32 screen, const float* hud) {
    (void)constants;
    const u32 block = rd32(ctx, draw + CDRAW_VIEWPORT);
    auto it = screen == 0 ? moves.find(object) : moves.end();
    Move hud_move{1.0f, 0.0f, 0.0f};
    if (it == moves.end() && hud != nullptr && screen == 0) {
        hud_move = Move{hud[0], hud[1], hud[2]};
    }
    const bool own_hud = it == moves.end() && hud != nullptr && screen == 0;
    if (it == moves.end() && !own_hud) {
        if (swap.ours != 0 && swap.draw == draw && block == swap.ours) {  // the pass's own constants again
            wr32(ctx, draw + CDRAW_VIEWPORT, swap.game);
            wr32(ctx, draw + CDRAW_DIRTY, rd32(ctx, draw + CDRAW_DIRTY) | 1);
        }
        return;
    }
    const u32 game = swap.draw == draw && swap.ours != 0 && block == swap.ours ? swap.game : block;
    const u32 shader = rd32(ctx, draw + CDRAW_SHADER);
    const u32 layout = shader ? rd32(ctx, shader + SHADER_VIEWPORT_LAYOUT) : 0;
    const u32 size = layout ? ((rd32(ctx, layout + 0x14) & 0xFFFF) * 4 + 15) & ~15u : 0;
    const u32 at = rd32(ctx, draw + CDRAW_ROOM);
    if (game == 0 || size < (PROJ + 16) * 4 || size > 0x400) {
        return;
    }
    // pixels p of the top screen go to scale * (p - centre) + centre + (tx, ty): the projection's two rows scaled,
    // then moved (its x row is the screen's height, up; its y row the screen's width, to the left)
    const Move& m = own_hud ? hud_move : it->second;
    u32 words[0x100];
    const u32 count = size / 4;
    for (u32 i = 0; i < count; ++i) words[i] = rd32(ctx, game + 4 * i);
    auto fl = [&](u32 i) { float f; std::memcpy(&f, &words[i], 4); return f; };
    auto setf = [&](u32 i, float f) { std::memcpy(&words[i], &f, 4); };
    const float m00 = fl(PROJ + 0), m11 = fl(PROJ + 5);
    for (u32 column = 0; column < 4; ++column) {
        for (u32 row = 0; row < 2; ++row) {
            const u32 i = PROJ + 4 * column + row;
            setf(i, fl(i) * m.scale);
        }
    }
    setf(PROJ + 12, fl(PROJ + 12) - m.ty * m00);
    setf(PROJ + 13, fl(PROJ + 13) - m.tx * m11);
    // the same moved block serves every object of the pass that moves alike (the room is small: a copy per object ran out
    // and the rest were drawn unmoved for a frame): a copy already in the room, still holding exactly these words
    u32 where = 0;
    for (u32 c = 0; c < MOVED_CACHE && where == 0; ++c) {
        const u32 old = moved_cache[(moved_next + c) % MOVED_CACHE];
        if (old == 0 || old >= rd32(ctx, draw + CDRAW_ROOM) ||
            moved_cache_end[(moved_next + c) % MOVED_CACHE] != rd32(ctx, draw + CDRAW_ROOM_END)) continue;
        bool same = true;
        for (u32 i = 0; i < count && same; ++i) same = rd32(ctx, old + 4 * i) == words[i];
        if (same) where = old;
    }
    if (where == 0) {
        u32 room = at;
        if (room + size > rd32(ctx, draw + CDRAW_ROOM_END)) {
            // the room is full (the game sizes it for what it draws): ask the game for a new one, as its own code does
            // (0x00ae65a4 calls this same function when its object constants do not fit)
            CallGame(ctx, FN_MAKE_ROOM, draw, size);
            room = rd32(ctx, draw + CDRAW_ROOM);
            if (room == 0 || room + size > rd32(ctx, draw + CDRAW_ROOM_END)) {
                if (no_room_logged < 20) {  // (the layout of a busy menu going back and forth is this: it shows in the log)
                    ++no_room_logged;
                    pending_log = fmt::format("one screen: no room for a moved object's constants (block {:08X}, size {:#x}, room {:08X} of {:08X})",
                                              game, size, room, rd32(ctx, draw + CDRAW_ROOM_END));
                }
                return;  // (no room: drawn where the game puts it this frame)
            }
            if (rooms_made < 5) {
                ++rooms_made;
                pending_log = fmt::format("one screen: the room for object constants was full: a new one taken from the game ({:08X} of {:08X})",
                                          room, rd32(ctx, draw + CDRAW_ROOM_END));
            }
        }
        wr32(ctx, draw + CDRAW_ROOM, room + size);
        for (u32 i = 0; i < count; ++i) wr32(ctx, room + 4 * i, words[i]);
        where = room;
        moved_cache[moved_next] = room;
        moved_cache_end[moved_next] = rd32(ctx, draw + CDRAW_ROOM_END);
        moved_next = (moved_next + 1) % MOVED_CACHE;
    }
    wr32(ctx, draw + CDRAW_VIEWPORT, where);
    wr32(ctx, draw + CDRAW_DIRTY, rd32(ctx, draw + CDRAW_DIRTY) | 1);
    const u32 at_used = where;
    swap = Swap{draw, game, at_used};
}

}  // namespace Port::OneScreen
