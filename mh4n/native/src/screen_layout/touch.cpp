// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: screen_layout.h and the touch vectors in screen_layout_expected.inc.

#include "screen_layout.h"

#include <algorithm>

namespace mh4n::screen_layout {
namespace {

float MapAxis(u32 pixel, u32 start, u32 end, float low, float high) {
    return low + (high - low) * (float(pixel - start) / float(end - start));
}

u32 ToPixel(float fraction, u32 size) {
    return static_cast<u32>(std::clamp(fraction * size, 0.0f, float(size - 1)));
}

} // namespace

int Layout::ViewAt(u32 x, u32 y) const {
    for (u32 i = view_count; i != 0; --i) {
        if (views[i - 1].screen.Contains(x, y)) return static_cast<int>(i - 1);
    }
    return -1;
}

bool Layout::IsWithinTouchscreen(u32 x, u32 y) const {
    return ViewAt(x, y) >= 0 || (bottom_shown && bottom.Contains(x, y));
}

void TouchTracker::PressInView(const Layout& layout, int view, u32 x, u32 y) {
    const Rect& rect = view < 0 ? layout.bottom : layout.views[view].screen;
    const RectF& crop = view < 0 ? layout.bottom_crop : layout.views[view].crop;
    if (rect.Width() == 0 || rect.Height() == 0) return;
    x = std::clamp(x, rect.left, rect.right - 1);
    y = std::clamp(y, rect.top, rect.bottom - 1);
    x_ = MapAxis(x, rect.left, rect.right, crop.left, crop.right);
    y_ = MapAxis(y, rect.top, rect.bottom, crop.top, crop.bottom);
}

bool TouchTracker::Press(const Layout& layout, u32 x, u32 y) {
    if (!layout.IsWithinTouchscreen(x, y)) return false;
    view_ = layout.ViewAt(x, y);
    pressed_ = true;
    PressInView(layout, view_, x, y);
    return true;
}

void TouchTracker::Move(const Layout& layout, u32 x, u32 y) {
    if (!pressed_) return;
    if (view_ < 0 ? layout.bottom_shown : static_cast<u32>(view_) < layout.view_count) {
        PressInView(layout, view_, x, y);
    }
}

void TouchTracker::Release() {
    pressed_ = false;
    view_ = -1;
}

std::optional<TouchPoint> TouchTracker::Point() const {
    if (!pressed_) return std::nullopt;
    return TouchPoint{ToPixel(x_, 320), ToPixel(y_, 240), x_, y_};
}

} // namespace mh4n::screen_layout
