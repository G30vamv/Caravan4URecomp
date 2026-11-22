// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: screen_layout.h, platform.h and screen_layout_expected.inc.

#include "screen_layout.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace mh4n::screen_layout {
namespace {

constexpr std::array kModeNames{"overlay", "stacked", "single", "side", "large"};
constexpr float kLargeTouchScale = 0.44f;  // MODIFIED: the touch screen of the "large" mode, as a fraction of the top screen's scale
constexpr u32 kBottomWidth = 320;
constexpr u32 kScreenHeight = 240;

u32 TopWidth(const Inputs& in) {
    return in.top_width ? in.top_width : 400;
}

Rect At(u32 x, u32 y, u32 width, u32 height) {
    return {x, y, x + width, y + height};
}

u32 Round(float pixels) {
    return static_cast<u32>(std::round(pixels));
}

Rect Fit(u32 width, u32 height, float height_per_width) {
    const float extent = std::min(float(width), float(height) / height_per_width);
    const u32 w = Round(extent);
    const u32 h = Round(extent * height_per_width);
    return At((width - w) / 2, (height - h) / 2, w, h);
}

Rect FitScreen(u32 width, u32 height, u32 native_width) {
    const auto w = static_cast<u32>(std::min(u64{width}, u64{height} * native_width / kScreenHeight));
    const auto h = static_cast<u32>(std::min(u64{height}, u64{width} * kScreenHeight / native_width));
    return At((width - w) / 2, (height - h) / 2, w, h);
}

float SingleAspect(std::string_view aspect, float fallback) {
    if (aspect == "4:3") return 3.0f / 4;
    if (aspect == "16:9") return 9.0f / 16;
    if (aspect == "16:10") return 10.0f / 16;
    if (aspect == "21:9") return 9.0f / 21;
    return fallback;
}

float Unit(float value) {
    return std::clamp(std::isnan(value) ? 0.0f : value, 0.0f, 1.0f);
}

template <typename Overlay>
void ClampCrop(Overlay& overlay) {
    constexpr float minimum = 0.01f;
    overlay.crop_left = std::min(Unit(overlay.crop_left), 1.0f - minimum);
    overlay.crop_top = std::min(Unit(overlay.crop_top), 1.0f - minimum);
    overlay.crop_right = std::max(Unit(overlay.crop_right), overlay.crop_left + minimum);
    overlay.crop_bottom = std::max(Unit(overlay.crop_bottom), overlay.crop_top + minimum);
}

template <typename Overlay>
RectF Crop(const Overlay& overlay) {
    return {overlay.crop_left, overlay.crop_top, overlay.crop_right, overlay.crop_bottom};
}

Rect OverlayBox(const Layout& layout, float height, float x, float y, RectF crop) {
    const u32 h = std::max(1u, Round(height * layout.height));
    const float crop_w = std::max(1u, Round(kBottomWidth * (crop.right - crop.left)));
    const float crop_h = std::max(1u, Round(kScreenHeight * (crop.bottom - crop.top)));
    const float aspect = crop_w / crop_h;
    const u32 w = std::max(1u, static_cast<u32>(std::min(float(layout.width), h * aspect)));
    return At(Round(x * (layout.width - w)), Round(y * (layout.height - h)), w, h);
}

} // namespace

Mode ParseMode(std::string_view name, bool* known) {
    const auto it = std::find(kModeNames.begin(), kModeNames.end(), name);
    if (known) *known = it != kModeNames.end();
    return it == kModeNames.end() ? Mode::Overlay : static_cast<Mode>(it - kModeNames.begin());
}

const char* ModeName(Mode mode) {
    const auto index = static_cast<std::size_t>(mode);
    return index < kModeNames.size() ? kModeNames[index] : kModeNames.front();
}

std::pair<u32, u32> MinimumSize(const Inputs& in) {
    const u32 top = TopWidth(in);
    switch (in.mode) {
    case Mode::Side:
        return {top + kBottomWidth, kScreenHeight};
    case Mode::Large:
        return {top + static_cast<u32>(kBottomWidth * kLargeTouchScale), kScreenHeight};
    case Mode::Stacked:
        return {top, 2 * kScreenHeight};
    case Mode::Overlay:
        if (!(in.overlay.height > 0)) return {400, 2 * kScreenHeight};
        break;
    case Mode::Single:
        break;
    }
    return {top, kScreenHeight};
}

Port::ScreenOverlay ClampOverlay(const Port::ScreenOverlay& overlay) {
    auto clamped = overlay;
    clamped.height = Unit(clamped.height);
    clamped.opacity = Unit(clamped.opacity);
    clamped.x = clamped.x < 0 ? -1.0f : Unit(clamped.x);
    clamped.y = clamped.y < 0 ? -1.0f : Unit(clamped.y);
    ClampCrop(clamped);
    for (auto& view : clamped.views) {
        view.height = Unit(view.height);
        view.x = Unit(view.x);
        view.y = Unit(view.y);
        ClampCrop(view);
    }
    return clamped;
}

Layout Compute(const Inputs& in) {
    Layout result{};
    const auto [min_w, min_h] = MinimumSize(in);
    const u32 w = result.width = std::max(in.window_w, min_w);
    const u32 h = result.height = std::max(in.window_h, min_h);
    const u32 top_width = TopWidth(in);
    result.top_shown = true;
    result.bottom_shown = in.mode != Mode::Single;

    // MODIFIED: an overlay with no touch screen size used to be a fixed 800 x 480 / 640 x 480 pair whatever the window; it is the stacked layout now
    const Mode mode = (in.mode == Mode::Overlay && !(in.overlay.height > 0)) ? Mode::Stacked : in.mode;
    switch (mode) {
    case Mode::Single:
        if (in.aspect_ratio == "stretch") {
            result.top = result.bottom = At(0, 0, w, h);
        } else {
            const float aspect = SingleAspect(in.aspect_ratio, 0);
            result.top = aspect ? Fit(w, h, aspect) : FitScreen(w, h, top_width);
            result.bottom = aspect ? Fit(w, h, aspect) : FitScreen(w, h, kBottomWidth);
        }
        break;
    case Mode::Stacked: {
        const float scale = std::min(float(w) / top_width, float(h / 2) / kScreenHeight);
        const u32 screen_h = Round(kScreenHeight * scale);
        const u32 top_w = static_cast<u32>(top_width * scale);
        const u32 bottom_w = static_cast<u32>(kBottomWidth * scale);
        const u32 x = (w - top_w) / 2;
        const u32 y = (h - 2 * screen_h) / 2;
        result.top = At(x, y, top_w, screen_h);
        result.bottom = At(x + (top_w - bottom_w) / 2, y + screen_h, bottom_w, screen_h);
        break;
    }
    case Mode::Side: {
        const u32 screen_h = static_cast<u32>(
            std::min(u64{h}, u64{w} * kScreenHeight / (top_width + kBottomWidth)));
        const u32 top_w = static_cast<u32>(u64{screen_h} * top_width / kScreenHeight);
        const u32 bottom_w = static_cast<u32>(u64{screen_h} * kBottomWidth / kScreenHeight);
        const u32 y = (h - screen_h) / 2;
        result.top = At(0, y, top_w, screen_h);
        result.bottom = At(top_w, y, bottom_w, screen_h);
        break;
    }
    case Mode::Large: {
        // the top screen as big as it can be with the small touch screen beside it (its top on the top screen's), the pair centred
        const float total_w = static_cast<float>(top_width) + kBottomWidth * kLargeTouchScale;
        const float scale = std::min(static_cast<float>(w) / total_w, static_cast<float>(h) / kScreenHeight);
        const u32 top_w = static_cast<u32>(top_width * scale);
        const u32 top_h = Round(kScreenHeight * scale);
        const u32 bottom_w = static_cast<u32>(kBottomWidth * scale * kLargeTouchScale);
        const u32 bottom_h = Round(kScreenHeight * scale * kLargeTouchScale);
        const u32 x = (w - top_w - bottom_w) / 2;
        const u32 y = (h - top_h) / 2;
        result.top = At(x, y, top_w, top_h);
        result.bottom = At(x + top_w, y, bottom_w, bottom_h);
        break;
    }
    case Mode::Overlay: {
        const auto overlay = ClampOverlay(in.overlay);
        result.top = FitScreen(w, h, top_width);
        result.bottom_opacity = overlay.opacity;
        result.bottom_crop = Crop(overlay);
        using Corner = Port::ScreenOverlay::Corner;
        const bool placed = overlay.x >= 0 && overlay.y >= 0;
        const bool right = overlay.corner == Corner::TopRight || overlay.corner == Corner::BottomRight;
        const bool bottom = overlay.corner == Corner::BottomLeft || overlay.corner == Corner::BottomRight;
        result.bottom = OverlayBox(result, overlay.height, placed ? overlay.x : float(right),
                                   placed ? overlay.y : float(bottom), result.bottom_crop);
        for (const auto& view : overlay.views) {
            if (view.height > 0) {
                const auto crop = Crop(view);
                result.views[result.view_count++] = {
                    OverlayBox(result, view.height, view.x, view.y, crop), crop};
            }
        }
        break;
    }
    }
    return result;
}

} // namespace mh4n::screen_layout
