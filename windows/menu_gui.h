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
    int hover_tab = -1, hover_row = -1;
    std::string notice;              // settings that wait for a restart
    std::string message;             // the last thing the menu said
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
void WindowOf(const Layout& layout, std::size_t total, int sel, int* first, int* fit);

// The renderer: Dear ImGui over the process's Vulkan device. It draws on the present thread only.
class GuiRenderer {
public:
    virtual ~GuiRenderer() = default;
    // Records the menu into the target's open pass. False when it can't (the caller falls back to plain rectangles).
    virtual bool Draw(void* recorder, const View& view, std::uint32_t width, std::uint32_t height) = 0;
};
// `format` is the swapchain's VkFormat (as an int, to keep Vulkan out of this header). Null when it can't be made.
std::unique_ptr<GuiRenderer> MakeGuiRenderer(std::shared_ptr<mh4n::gpu::rhi::Backend> backend, int format,
                                             std::function<void()> redraw);

}  // namespace ingame
