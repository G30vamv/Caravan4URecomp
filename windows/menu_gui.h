// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by
// khorne-hub (https://github.com/khorne-hub/4u-native); see ADDITIONAL-TERMS and THIRD_PARTY_NOTICES.
//
// The look of the in-game menu (ingame_menu.h is what it holds): a panel over the dimmed game with a fine lattice, double
// borders with corner studs, the pages as tabs with a pill sliding under the one in force, rows with a bar sliding to the
// marked one, the help of the marked row and the keys at the bottom, in the Cinzel font (SIL Open Font License, see
// licenses/Cinzel-OFL.txt). It is the design of the menu of my MH3U port, with this game's colours. Drawn with Dear ImGui's
// Vulkan backend into the presenter's pass, on the present thread.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gpu/backend/rhi.h"

namespace ingame {

// One row of the page in force, as the drawing needs it.
struct ViewRow {
    enum Kind { Info, Choice, Toggle, Slider, Button, Text, Room };
    Kind kind = Info;
    std::string label, value, help;
    bool selected = false;
    bool enabled = true;
    bool left = true, right = true;  // Choice: which arrows are usable
    bool on = false;                 // Toggle
    bool caret = false;              // Text: being typed into
    float frac = 0.0f;               // Slider: 0 to 1
    int color = 0;                   // Info: 0 text, 1 notice, 2 dim, 3 good, 4 bad
    bool operator==(const ViewRow&) const = default;
};

struct View {
    bool open = false;
    std::string title;               // the page's name, beside the brand
    std::vector<std::string> tabs;
    int tab = 0;
    std::vector<ViewRow> rows;
    int sel = -1;                    // the marked row, or -1
    int scroll = -1;                 // first row shown when the wheel scrolled the list (-1: follow the marked row)
    int hover_tab = -1, hover_row = -1;
    std::string notice;              // settings that wait for a restart
    std::string message;             // the last thing the menu said
    std::vector<std::pair<std::string, std::string>> keys;  // the bottom line: a key cap and what it does
    bool operator==(const View&) const = default;
};

struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float W() const { return x1 - x0; }
    float H() const { return y1 - y0; }
    bool Contains(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
};

// Where each part of the menu goes in a window of this size, in pixels. `u` is a 640th of the panel's height.
struct Layout {
    Rect panel, header, tabs, rows, help, keys;
    float u = 1.0f, row_h = 55.0f;
};
Layout LayoutOf(float width, float height);
// How many rows fit, and the first one shown so that the marked one is in view.
void WindowOf(const Layout& layout, std::size_t total, int sel, int* first, int* fit, int scroll = -1);

// The life of a large monster over the game (the panel of the ini's monster_hp): what the drawing needs of it.
struct HudBar {
    std::uint32_t id = 0;     // the monster (the trail of its bar is kept by it)
    std::string name;         // "Monster 1"
    int hp = 0, max = 1;
    float since_hit = 99.0f;  // seconds since its life last went down
    bool dead = false;
};
// The damage meter (the ini's damage_meter): what the monster being fought has taken, drawn under its life plate.
struct HudMeter {
    bool show = false;
    std::string total, dps, last;  // already formatted ("12 345", "210", "456")
};
struct HudView {
    std::vector<HudBar> bars; // the monster being fought, or none (nothing is drawn)
    float bar_scale = 1.0f;   // the size of the plate (monster_hp_size / 100)
    int bar_pos = 0;          // 0 top right, 1 top left, 2 top middle (monster_hp_pos)
    bool bar_percent = false; // the percent of life left over the bar (monster_hp_percent)
    bool spanish = false;
    HudMeter meter;
    // the hold of the buttons that end the wait after a quest (the ini's skip_end): how much is done, 0..1 (0: not held), the
    // player has asked (the hold is done; the others may not have), and the words
    float end_wait = 0.0f;
    bool end_asked = false;
    std::string end_text;
    // the world map is open: the words drawn over the touch button below it (Solo / multiplayer) that say how to press it ("" = none)
    std::string map_hint;
};

// The first start's screen: choosing the two files of the game, then the progress of building the code library for this PC.
// Every text is already in the language.
struct SetupView {
    bool open = false;
    int stage = 0;                     // 0 choose the files, 1 working, 2 failed, 3 done
    std::string title, intro, base_label, update_label, base_path, update_path, placeholder, hint;
    std::string browse, primary, secondary;  // the words of the buttons ("" = no such button)
    bool primary_enabled = false;
    int focus = -1, hover = -1;        // the button with the keyboard's focus and under the mouse: 0, 1 browse, 2 primary, 3 secondary
    float progress = -1.0f;            // 0 to 1 (-1: not known)
    std::string step, eta, error;
    std::vector<std::string> log;      // the last lines of what the tool says
    bool operator==(const SetupView&) const = default;
};

struct SetupLayout {
    Layout l;
    Rect intro, base_field, update_field, base_browse, update_browse, hint, primary, secondary, bar, log;
    float base_label_y = 0, update_label_y = 0, step_y = 0, eta_y = 0;
};
SetupLayout SetupLayoutOf(float width, float height);

// The renderer: Dear ImGui over the process's Vulkan device. It draws on the present thread only.
class GuiRenderer {
public:
    virtual ~GuiRenderer() = default;
    // The first start's screen, drawn over everything.
    virtual bool DrawSetup(void* recorder, const SetupView& view, std::uint32_t width, std::uint32_t height) = 0;
    // Records the menu into the target's open pass. False when it can't (the caller falls back to plain rectangles).
    virtual bool Draw(void* recorder, const View& view, std::uint32_t width, std::uint32_t height) = 0;
    // The monster life panel over the game, in the same look and palette.
    virtual bool DrawHud(void*, const HudView&, std::uint32_t, std::uint32_t) { return false; }
};
// `format` is the swapchain's VkFormat (as an int, to keep Vulkan out of this header). Null when it can't be made.
std::unique_ptr<GuiRenderer> MakeGuiRenderer(std::shared_ptr<mh4n::gpu::rhi::Backend> backend, int format,
                                             std::function<void()> redraw);

}  // namespace ingame
