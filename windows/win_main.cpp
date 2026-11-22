// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors
//
// Part of a MODIFIED version of 4u-native (MH4U for Android) by khorne-hub, https://github.com/khorne-hub/4u-native,
// which builds on MH4UltimateDC by CompaLarios18 and contains code derived from Azahar and Citra. This file is new in
// the Windows port. Free software under the GNU GPL, version 3 or later (LICENSE), with the additional terms of
// ADDITIONAL-TERMS (author attribution, a modified version marked as such, no use of the names); the third-party parts
// keep their own licenses (THIRD_PARTY_NOTICES, windows/THIRD_PARTY_NOTICES_WINDOWS).
//
// mh4u-win: milestone 1 of the Windows shell. An SDL3 window, a Vulkan surface made on the backend's own
// instance, and the runtime's Presenter (mh4n/native/src/swapchain) showing a moving test pattern in the 3DS
// screens' layout. No game runs yet: this proves the window, the Win32 surface, the swapchain, resizing and
// the present thread on this machine (roadmap phase 3). The game, input and audio come next.
//
//   mh4u-win [--seconds N] [--vsync on|off] [--mode popup|overlay|stacked|single|side]
#include <algorithm>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "net_addresses.h"  // (winsock2.h before windows.h)
#include "config.h"
#include "gpu/backend/rhi.h"
#include "gpu/backend/sync/sync.h"
#include "gpu/backend/vulkan_access.h"
#include "discord_presence.h"
#include "mod_manager.h"
#include "save_manager.h"
#include "hid_pads.h"
#include "hidhide.h"
#include "input/azahar_rules.h"
#include "input/input_mapper.h"
#include "input_live/live_input.h"
#include "mh4n/options.h"
#include "report/report.h"
#include "mh4n/present.h"
#include "mh4n/registry.h"
#include "mh4n/rooms.h"
#include "mh4n/runtime.h"
#include "screen_layout/screen_layout.h"
#include "swapchain/policy.h"
#include "swapchain/presenter.h"
#include "swkbd_bridge.h"

#include <windows.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "audio/app_audio.h"
#include "hooks.h"
#include "ingame_menu.h"
#include "setup_ui.h"

namespace rhi = ::mh4n::gpu::rhi;
namespace pr = ::mh4n::present;
namespace sc = ::mh4n::swapchain;
namespace sl = ::mh4n::screen_layout;
namespace mh4n::display_mem { std::string Mark(const char* how); }  // display_mem.cpp: a numbered log line with the time


namespace {

// A frame of two CPU-pixel screens: a gradient that scrolls with `sequence`, so movement and tearing show.
pr::DisplayFrame TestFrame(std::uint64_t sequence) {
    const auto make = [&](std::uint32_t w, std::uint32_t h, std::uint8_t base) {
        auto pixels = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(w) * h * 4);
        for (std::uint32_t y = 0; y < h; ++y) {
            for (std::uint32_t x = 0; x < w; ++x) {
                std::uint8_t* p = pixels->data() + (static_cast<std::size_t>(y) * w + x) * 4;
                const std::uint32_t band = (x + static_cast<std::uint32_t>(sequence * 3)) % 64;
                p[0] = static_cast<std::uint8_t>(base + band * 2);
                p[1] = static_cast<std::uint8_t>(y * 255 / h);
                p[2] = static_cast<std::uint8_t>((x * 255 / w) ^ (sequence & 0xFF));
                p[3] = 255;
            }
        }
        pr::ScreenImage screen;
        screen.pixels = pixels;
        screen.width = w;
        screen.height = h;
        screen.region = {0, 0, w, h};
        return screen;
    };
    pr::DisplayFrame frame;
    frame.sequence = sequence;
    frame.game_frame = static_cast<std::uint32_t>(sequence);
    frame.top = make(pr::kTopWidth, pr::kTopHeight, 20);
    frame.bottom = make(pr::kBottomWidth, pr::kBottomHeight, 80);
    return frame;
}


// ---- the game, when a program is given (mh4u-win [options] -- PROGRAM --code LIB ...) ------------------------------------

std::atomic<mh4n::Runtime*> g_runtime{nullptr};
std::recursive_mutex g_runtime_mutex;  // held while the shell uses the runtime; the game thread takes the pointer away under it before the runtime dies
std::atomic<sc::Presenter*> g_presenter{nullptr};
std::atomic<bool> g_game_done{false};

// The skip key (--skip-key, Tab by default): while it is held the Y button is pressed on top of whatever else is held, and
// nothing else changes: no other key, button or stick is remapped, and the mapper never sees the key. Y is the port's own
// skip button (hooks.cpp "skip", 0x800): in a cutscene it taps the Skip button the game shows on the touch screen, and
// the port shows a hint while a cutscene can be skipped (skip_hint). Start does nothing there.
// Main thread only.
mh4n::InputState g_last_state;
bool g_skip_held = false;
bool g_skip_pad_held = false;  // MODIFIED: the controller button / trigger chosen for skip (skip_pad); it keeps its own job in the game
void SendInput(const mh4n::InputState& state) {
    g_last_state = state;
    mh4n::InputState out = state;
    // MODIFIED: the skip key no longer presses Y (the game's Y is 0x8000, the hook's skip button is ZR 0x800: Tab never skipped)
    Port::Hooks::SetHostSkip(g_skip_held || g_skip_pad_held);
    const std::lock_guard<std::recursive_mutex> rt_guard(g_runtime_mutex);
    if (mh4n::Runtime* rt = g_runtime.load()) {
        if (auto live = mh4n::input_live::Find(*rt)) live->Set(out);
    }
}
std::atomic<int> g_game_status{0};
std::atomic<int> g_layout_mode{static_cast<int>(sl::Mode::Stacked)};  // the screen layout, changed live by the menu
// "popup": the top screen fills the window and the touch screen is a picture-in-picture over it, only while the game
// waits for something on it (the layout is then Overlay, else Single). The mouse works on the popup as on the touch screen.
std::atomic<bool> g_popup{true};
std::atomic<bool> g_touch_visible{false};
// the picture's shape: default (the 3DS's 5:3, bars at the sides on a wide screen), stretch (fills the window) or a fixed ratio
constexpr const char* kAspects[] = {"default", "stretch", "16:9", "16:10", "21:9", "4:3"};
std::atomic<int> g_aspect{0};
std::atomic<int> g_popup_size{38};    // the popup's height, % of the window's
std::atomic<int> g_popup_corner{3};   // Port::ScreenOverlay::Corner: 0 top left, 1 top right, 2 bottom left, 3 bottom right

// Once the display GPU exists (the `devices` phase), the presenter becomes one of its screen sinks and the runtime is
// kept for the input.
void AttachWindow(mh4n::Runtime& rt) {
    if (auto* display = pr::FindDisplay(rt.Interfaces().gpu)) {
        display->AddScreenSink(g_presenter.load());
    } else {
        std::fprintf(stderr, "mh4u-win: the GPU shows no screens: nothing will be drawn\n");
    }
    const std::lock_guard<std::recursive_mutex> guard(g_runtime_mutex);
    g_runtime.store(&rt);
}
MH4N_REGISTER(mh4n::BootStep, "zz.win.window", mh4n::BootStep{mh4n::Phase::Started, AttachWindow});

// The controllers SDL has plus the HID ones read by hid_pads.h (a DualSense over Bluetooth that SDL does not open...): the mapper
// lists both. `active` is what the shell decided to use (see pump_hid in main); their buttons arrive as pushed SDL events.
class HidAugmentedPads final : public mh4n::app::IGamepadLayer {
public:
    explicit HidAugmentedPads(std::unique_ptr<mh4n::app::IGamepadLayer> inner) : inner_(std::move(inner)) {}
    std::vector<mh4n::app::PadInfo> List() override {
        std::vector<mh4n::app::PadInfo> list = inner_->List();
        for (const hidpads::Snapshot& pad : active) {
            mh4n::app::PadInfo info;
            info.id = pad.id;
            char guid[32];
            std::snprintf(guid, sizeof guid, "hid%04x%04x", pad.vendor, pad.product);
            info.guid = guid;
            info.name = pad.name;
            list.push_back(std::move(info));
        }
        return list;
    }
    std::vector<hidpads::Snapshot> active;

private:
    std::unique_ptr<mh4n::app::IGamepadLayer> inner_;
};

// An SDL3 event as the shared sources read it (app_window.h). SDL3 keeps SDL2's numbers for scancodes, mouse buttons,
// gamepad buttons and gamepad axes, so those pass through unchanged.
bool ToInputEvent(const SDL_Event& e, App::InputEvent& out) {
    using T = App::InputEvent::Type;
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        out.type = e.type == SDL_EVENT_KEY_DOWN ? T::KeyDown : T::KeyUp;
        out.scancode = static_cast<int>(e.key.scancode);
        out.repeat = e.key.repeat;
        out.mods = e.key.mod;
        return true;
    case SDL_EVENT_MOUSE_MOTION:
        out.type = T::MouseMotion;
        out.x = static_cast<int>(e.motion.x);
        out.y = static_cast<int>(e.motion.y);
        out.dx = static_cast<int>(e.motion.xrel);
        out.dy = static_cast<int>(e.motion.yrel);
        out.from_touch = e.motion.which == SDL_TOUCH_MOUSEID;
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        out.type = T::MouseButton;
        out.button = e.button.button;
        out.down = e.button.down;
        out.x = static_cast<int>(e.button.x);
        out.y = static_cast<int>(e.button.y);
        out.from_touch = e.button.which == SDL_TOUCH_MOUSEID;
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        out.type = T::Wheel;
        out.wheel_x = e.wheel.integer_x;
        out.wheel_y = e.wheel.integer_y;
        out.from_touch = e.wheel.which == SDL_TOUCH_MOUSEID;
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        out.type = T::PadButton;
        out.pad = static_cast<std::int32_t>(e.gbutton.which);
        out.button = e.gbutton.button;
        out.down = e.gbutton.down;
        return true;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        out.type = T::PadAxis;
        out.pad = static_cast<std::int32_t>(e.gaxis.which);
        out.axis = e.gaxis.axis;
        out.value = e.gaxis.value;
        return true;
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
        out.type = e.type == SDL_EVENT_GAMEPAD_ADDED ? T::PadAdded : T::PadRemoved;
        out.pad = static_cast<std::int32_t>(e.gdevice.which);
        return true;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        out.type = T::FocusLost;
        return true;
    default:
        return false;
    }
}


// UTF-8 <-> UTF-16 for the game's text box (it holds UTF-16 units).
std::u16string ToU16(const char* utf8) {
    std::u16string out;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8);
    while (*p) {
        std::uint32_t cp = 0;
        int extra = 0;
        if (*p < 0x80) { cp = *p; extra = 0; }
        else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; extra = 1; }
        else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; extra = 2; }
        else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; extra = 3; }
        else { ++p; continue; }
        ++p;
        for (int i = 0; i < extra && (*p & 0xC0) == 0x80; ++i, ++p) cp = (cp << 6) | (*p & 0x3F);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

std::string ToUtf8(const std::u16string& in) {
    std::string out;
    for (std::size_t i = 0; i < in.size(); ++i) {
        std::uint32_t cp = in[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < in.size()) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (in[++i] - 0xDC00);
        }
        if (cp < 0x80) out.push_back(static_cast<char>(cp));
        else if (cp < 0x800) { out.push_back(static_cast<char>(0xC0 | (cp >> 6))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) { out.push_back(static_cast<char>(0xE0 | (cp >> 12))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
        else { out.push_back(static_cast<char>(0xF0 | (cp >> 18))); out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
    }
    return out;
}


// The name of this program: a modified version of 4u-native, said in its name (ADDITIONAL-TERMS b).
constexpr const char* kAppName = "Caravan4URecomp (modified 4u-native)";

// What the presenter's UI layer draws over the composed frame, with raw clears (vkCmdClearAttachments, no pipeline, no
// shader; the same way as present_test.cpp's RawLayer):
//  - the pointer the controller moves over the game's own on-screen keyboard (the touch screen): a white hand with a
//    black outline, its fingertip where the touch lands, yellow while A is held;
//  - the frame-rate counter ("60 FPS", a 5 x 7 pixel font made of squares), when fps_counter is on in the settings.
// Position in the bottom screen's pixels (320 x 240). Written by the main thread, read by the present thread.
class CursorLayer final : public pr::UiLayer {
public:
    std::atomic<bool> visible{false};
    std::atomic<bool> pressed{false};
    std::atomic<float> x{160.0f};
    std::atomic<float> y{150.0f};
    std::atomic<bool> show_fps{false};
    std::atomic<int> fps{-1};
    std::atomic<sc::Presenter*> presenter{nullptr};
    const ingame::Overlay* menu = nullptr;  // the in-game settings menu, drawn last (ingame_menu.h)

    void Record(void* recorder, std::uint32_t width, std::uint32_t height) override {
        sc::Presenter* owner = presenter.load();
        if (owner == nullptr) return;
        VkCommandBuffer commands = rhi::CommandBuffer(*static_cast<rhi::Recorder*>(recorder));
        if (show_fps.load() && fps.load() >= 0) DrawFps(commands, width, height);
        if (visible.load()) DrawHand(commands, owner->CurrentLayout(), width, height);
        if (menu != nullptr) menu->Draw(commands, recorder, width, height);
    }

private:
    struct Square {
        int x, y, w, h;
    };
    // one clear call per colour: every square of the list in that colour, clipped to the target
    static void Fill(VkCommandBuffer commands, const std::vector<Square>& squares, std::uint32_t width, std::uint32_t height,
                     float r, float g, float b) {
        std::vector<VkClearRect> rects;
        for (const Square& q : squares) {
            const int x0 = std::max(0, q.x), y0 = std::max(0, q.y);
            const int x1 = std::min<int>(static_cast<int>(width), q.x + q.w), y1 = std::min<int>(static_cast<int>(height), q.y + q.h);
            if (x1 <= x0 || y1 <= y0) continue;
            VkClearRect rect = {};
            rect.rect = {{x0, y0}, {static_cast<std::uint32_t>(x1 - x0), static_cast<std::uint32_t>(y1 - y0)}};
            rect.layerCount = 1;
            rects.push_back(rect);
        }
        if (rects.empty()) return;
        VkClearAttachment attachment = {};
        attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        attachment.colorAttachment = 0;
        attachment.clearValue.color = {{r, g, b, 1.0f}};
        vkCmdClearAttachments(commands, 1, &attachment, static_cast<std::uint32_t>(rects.size()), rects.data());
    }

    // ---- the counter -----------------------------------------------------------------------------------------------------
    static const char* Glyph(char c) {  // 5 columns x 7 rows, '1' is a lit square
        switch (c) {
        case '0': return "01110" "10001" "10011" "10101" "11001" "10001" "01110";
        case '1': return "00100" "01100" "00100" "00100" "00100" "00100" "01110";
        case '2': return "01110" "10001" "00001" "00010" "00100" "01000" "11111";
        case '3': return "11110" "00001" "00001" "01110" "00001" "00001" "11110";
        case '4': return "00010" "00110" "01010" "10010" "11111" "00010" "00010";
        case '5': return "11111" "10000" "11110" "00001" "00001" "10001" "01110";
        case '6': return "00110" "01000" "10000" "11110" "10001" "10001" "01110";
        case '7': return "11111" "00001" "00010" "00100" "01000" "01000" "01000";
        case '8': return "01110" "10001" "10001" "01110" "10001" "10001" "01110";
        case '9': return "01110" "10001" "10001" "01111" "00001" "00010" "01100";
        case 'F': return "11111" "10000" "10000" "11110" "10000" "10000" "10000";
        case 'P': return "11110" "10001" "10001" "11110" "10000" "10000" "10000";
        case 'S': return "01111" "10000" "10000" "01110" "00001" "00001" "11110";
        default: return nullptr;  // a space
        }
    }
    void DrawFps(VkCommandBuffer commands, std::uint32_t width, std::uint32_t height) const {
        const int unit = std::max(1, static_cast<int>(height) / 300);  // a font square, in pixels (half the first size)
        const std::string text = std::to_string(fps.load()) + " FPS";
        std::vector<Square> lit;
        int pen = 3 * unit;
        for (const char c : text) {
            if (const char* glyph = Glyph(c)) {
                for (int row = 0; row < 7; ++row) {
                    for (int col = 0; col < 5; ++col) {
                        if (glyph[row * 5 + col] == '1') lit.push_back({pen + col * unit, 3 * unit + row * unit, unit, unit});
                    }
                }
            }
            pen += 6 * unit;
        }
        std::vector<Square> shadow;
        for (const Square& q : lit) shadow.push_back({q.x + unit, q.y + unit, q.w, q.h});
        Fill(commands, shadow, width, height, 0.0f, 0.0f, 0.0f);
        Fill(commands, lit, width, height, 0.55f, 1.0f, 0.55f);
    }

    // ---- the hand --------------------------------------------------------------------------------------------------------
    void DrawHand(VkCommandBuffer commands, const sl::Layout& layout, std::uint32_t width, std::uint32_t height) const {
        if (!layout.bottom_shown || layout.bottom.Width() == 0 || layout.bottom.Height() == 0) return;
        const int unit = std::max(2, static_cast<int>(layout.bottom.Width()) / 240);  // a sprite unit, in pixels
        const int tip_x = static_cast<int>(layout.bottom.left + x.load() * (static_cast<float>(layout.bottom.Width()) / 320.0f));
        const int tip_y = static_cast<int>(layout.bottom.top + y.load() * (static_cast<float>(layout.bottom.Height()) / 240.0f));
        // The sprite, in units, its origin at the fingertip (the index finger's top middle): a pointing hand, the index up and
        // three curled fingers, the thumb on the left. White shapes; the outline is the same shapes one unit larger in black,
        // and a few black lines between the fingers.
        static const Square kWhite[] = {{-1, 0, 3, 9},    // the index finger
                                        {2, 5, 3, 6},     // the middle finger's knuckle
                                        {5, 6, 3, 5},     // the ring finger's
                                        {8, 7, 3, 4},     // the little finger's
                                        {-4, 9, 15, 7},   // the palm
                                        {-6, 8, 3, 5}};   // the thumb
        static const Square kLines[] = {{2, 7, 1, 3}, {5, 8, 1, 3}, {8, 9, 1, 2}};
        const auto place = [&](const Square& q, int grow) {
            return Square{tip_x + (q.x - grow) * unit, tip_y + (q.y - grow) * unit, (q.w + 2 * grow) * unit, (q.h + 2 * grow) * unit};
        };
        std::vector<Square> outline, fill, lines;
        for (const Square& q : kWhite) {
            outline.push_back(place(q, 1));
            fill.push_back(place(q, 0));
        }
        for (const Square& q : kLines) lines.push_back(place(q, 0));
        Fill(commands, outline, width, height, 0.0f, 0.0f, 0.0f);
        if (pressed.load()) {
            Fill(commands, fill, width, height, 1.0f, 0.85f, 0.2f);  // pressing: the hand turns yellow
        } else {
            Fill(commands, fill, width, height, 1.0f, 1.0f, 1.0f);
        }
        Fill(commands, lines, width, height, 0.0f, 0.0f, 0.0f);
    }
};
CursorLayer g_cursor;

// The controller's press of the touch screen (the selector's square), through the live input source.
void PadTouch(bool down, float x, float y) {
    const std::lock_guard<std::recursive_mutex> rt_guard(g_runtime_mutex);
    if (mh4n::Runtime* rt = g_runtime.load()) {
        if (auto live = mh4n::input_live::Find(*rt)) {
            if (down) {
                live->SetTouch(mh4n::input_live::TouchPoint{static_cast<mh4n::u16>(std::clamp(x, 0.0f, 319.0f)),
                                                            static_cast<mh4n::u16>(std::clamp(y, 0.0f, 239.0f))});
            } else {
                live->SetTouch(std::nullopt);
            }
        }
    }
}

// The game's own software keyboard (names, the Guild Card's greeting...), typed on from the PC keyboard while it is
// open: the text goes into the game's text box (Port::Keyboard::SetText), Enter presses its OK, Escape its Cancel.
// The text shows in the window's title (no panel of ours yet). The physical keyboard's keys press no button while it
// is open; a controller's still do, to move over the game's on-screen keyboard.
struct KeyboardTyping {
    bool active = false;
    std::uint32_t serial = 0;
    std::uint32_t text_serial = 0;
    std::u16string text;
    std::uint32_t max_length = 0;

    // The controller works on the game's own on-screen keyboard like a stylus: a yellow selector (CursorLayer) moves over
    // the touch screen with the D-pad (steps, repeating while held) and the left stick (smoothly); A presses where it is
    // (the game sees a touch, so it types the key under the selector), B deletes the last letter, Start presses the
    // game's OK and Select its Cancel. The physical keyboard keeps typing as before; a controller's button never presses a
    // 3DS button while the keyboard is open. The selector shows only with a controller connected.
    float cursor_x = 160.0f, cursor_y = 150.0f;
    bool a_held = false;
    int dpad_x = 0, dpad_y = 0;          // the D-pad directions held
    Uint64 next_dpad_ms = 0;
    Uint64 last_tick_ms = 0;
    static constexpr float kStep = 18.0f;  // about one key of the game's keyboard

    std::u16string Shown() const { return text; }
    void MoveCursor(float dx, float dy) {
        cursor_x = std::clamp(cursor_x + dx, 0.0f, 319.0f);
        cursor_y = std::clamp(cursor_y + dy, 0.0f, 239.0f);
        g_cursor.x.store(cursor_x);
        g_cursor.y.store(cursor_y);
        if (a_held) PadTouch(true, cursor_x, cursor_y);  // dragging with A held
    }
    void Push(SDL_Window* window) const {
        std::fprintf(stderr, "mh4u-win: keyboard shows %u letters (serial %u)\n", static_cast<unsigned>(Shown().size()), serial);
        Port::Keyboard::SetText(serial, Shown());
        Show(window);
    }

    // Once per loop, on the main thread; returns true while the keyboard is open.
    bool Update(SDL_Window* window, mh4n::app::InputMapper& /*mapper*/) {
        const Port::Keyboard::Request req = Port::Keyboard::Current();
        if (req.open && (!active || req.serial != serial)) {
            active = true;
            serial = req.serial;
            text = req.text;
            text_serial = req.text_serial;
            max_length = req.max_length;
            cursor_x = 160.0f;
            cursor_y = 150.0f;
            g_cursor.x.store(cursor_x);
            g_cursor.y.store(cursor_y);
            last_tick_ms = SDL_GetTicks();
            SDL_StartTextInput(window);
            Show(window);
        } else if (!req.open && active) {
            active = false;
            if (a_held) PadTouch(false, 0.0f, 0.0f);
            a_held = false;
            g_cursor.pressed.store(false);
            dpad_x = dpad_y = 0;
            g_cursor.visible.store(false);
            SDL_StopTextInput(window);
            SDL_SetWindowTitle(window, kAppName);
        } else if (active && req.text_serial != text_serial) {  // the game's own keys changed the text
            text_serial = req.text_serial;
            text = req.text;
            Show(window);
        }
        if (active) {
            g_cursor.visible.store(SDL_HasGamepad());
            const Uint64 now = SDL_GetTicks();
            const float dt = static_cast<float>(now - last_tick_ms) / 1000.0f;
            last_tick_ms = now;
            // the left stick moves the selector smoothly (the first connected controller's)
            int count = 0;
            SDL_JoystickID* ids = SDL_GetGamepads(&count);
            if (ids && count > 0) {
                if (SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[0])) {
                    const auto axis = [&](SDL_GamepadAxis a) {
                        const int v = SDL_GetGamepadAxis(pad, a);
                        return std::abs(v) < 8000 ? 0.0f : static_cast<float>(v) / 32767.0f;
                    };
                    const float sx = axis(SDL_GAMEPAD_AXIS_LEFTX), sy = axis(SDL_GAMEPAD_AXIS_LEFTY);
                    if (sx != 0.0f || sy != 0.0f) MoveCursor(sx * 160.0f * dt, sy * 160.0f * dt);
                }
            }
            SDL_free(ids);
            if ((dpad_x != 0 || dpad_y != 0) && now >= next_dpad_ms) {  // repeats while held
                MoveCursor(dpad_x * kStep, dpad_y * kStep);
                next_dpad_ms = now + 90;
            }
        }
        return active;
    }

    void Show(SDL_Window* window) const {
        const std::string title = "MH4U - typing: " + ToUtf8(Shown()) +
                                  "_   (keyboard: Enter OK, Esc Cancel | controller: D-pad/stick move the selector, A press, "
                                  "B delete, Start OK, Select Cancel)";
        SDL_SetWindowTitle(window, title.c_str());
    }

    // True when the event was typing (the text, Backspace, Enter, Escape).
    bool OnEvent(const SDL_Event& e, SDL_Window* window) {
        if (!active) return false;
        if (e.type == SDL_EVENT_TEXT_INPUT) {
            std::fprintf(stderr, "mh4u-win: keyboard text event \"%s\" (mods 0x%x)\n", e.text.text,
                         static_cast<unsigned>(SDL_GetModState()));
            text += ToU16(e.text.text);
            if (max_length != 0 && text.size() > max_length) text.resize(max_length);
            Push(window);
            return true;
        }
        if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
            const bool down = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
            const auto direction = [&](int dx, int dy) {
                if (down) {
                    dpad_x = dx != 0 ? dx : dpad_x;
                    dpad_y = dy != 0 ? dy : dpad_y;
                    MoveCursor(dx * kStep, dy * kStep);
                    next_dpad_ms = SDL_GetTicks() + 320;
                } else {
                    if (dx != 0 && dpad_x == dx) dpad_x = 0;
                    if (dy != 0 && dpad_y == dy) dpad_y = 0;
                }
            };
            switch (e.gbutton.button) {
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT: direction(-1, 0); return true;
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: direction(1, 0); return true;
            case SDL_GAMEPAD_BUTTON_DPAD_UP: direction(0, -1); return true;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN: direction(0, 1); return true;
            case SDL_GAMEPAD_BUTTON_SOUTH:  // A: a press of the touch screen where the selector is
                a_held = down;
                g_cursor.pressed.store(down);
                PadTouch(down, cursor_x, cursor_y);
                return true;
            case SDL_GAMEPAD_BUTTON_EAST:  // B: delete the last letter
                if (down) {
                    if (!text.empty()) text.pop_back();
                    Push(window);
                }
                return true;
            case SDL_GAMEPAD_BUTTON_START:
                if (down) Port::Keyboard::Submit(serial, text);
                return true;
            case SDL_GAMEPAD_BUTTON_BACK:
                if (down) Port::Keyboard::Cancel(serial);
                return true;
            default:
                return true;  // no other button does anything while the keyboard is open
            }
        }
        if (e.type == SDL_EVENT_KEY_DOWN) {
            std::fprintf(stderr, "mh4u-win: keyboard key %s (mods 0x%x)\n", SDL_GetKeyName(e.key.key),
                         static_cast<unsigned>(e.key.mod));
            if (e.key.key == SDLK_BACKSPACE) {
                if (!text.empty()) text.pop_back();
                Push(window);
                return true;
            }
            if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) {
                Port::Keyboard::Submit(serial, text);
                return true;
            }
            if (e.key.key == SDLK_ESCAPE) {
                Port::Keyboard::Cancel(serial);
                return true;
            }
        }
        return false;
    }
};


// Whether the game waits for something on the touch screen, from what the port knows of the game (hooks.h GameState): the
// intro, the title and the character's creation, and the pages of a town's touch screen that the player opened (Hunters for
// Hire, Guild Cards, Gestures, Friends). The game's own keyboard is handled by the caller. Tuned by playing: set
// MH4U_LOG_STATE=1 to print the state as it changes.
bool TouchWanted(const Port::Hooks::GameState& gs) {
    // (a touch screen the game draws nothing on, as at the title, has nothing to wait for)
    if (gs.front_end == 1) return Port::Hooks::TouchScreenObjects() > 0;
    if (gs.place == Port::Hooks::GameState::PLACE_TOWN) {
        switch (gs.town_panel) {
        case 5: case 8: case 11: case 18: return true;
        default: break;
        }
    }
    return false;
}

// The refresh rate of the display a window is on, in whole hertz (60 when it can't be read).
int DisplayHz(SDL_Window* window) {
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
    return mode != nullptr && mode->refresh_rate >= 20.0f ? static_cast<int>(std::lround(mode->refresh_rate)) : 60;
}

// A call through a null function pointer leaves no frame the unwinder can follow: at that fault the stack's own words
// are the return addresses. Writes the ones that lie in this program's image as "image+offset" lines to stderr, for
// llvm-symbolizer. Only a first look at a crash; the crash file (mh4n::app::report) is the real one.
LONG CALLBACK StackWords(PEXCEPTION_POINTERS info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    const char* base = reinterpret_cast<const char*>(GetModuleHandleA(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const char* end = base + nt->OptionalHeader.SizeOfImage;
    std::fprintf(stderr, "mh4u-win: access violation, rip=%p, image base %p\n",
                 reinterpret_cast<void*>(info->ContextRecord->Rip), static_cast<const void*>(base));
    const auto* stack = reinterpret_cast<const char* const*>(info->ContextRecord->Rsp);
    int shown = 0;
    for (int i = 0; i < 4096 && shown < 24; ++i) {
        if (IsBadReadPtr(stack + i, sizeof(void*))) break;
        const char* word = stack[i];
        if (word >= base && word < end) {
            std::fprintf(stderr, "mh4u-win:   stack[%d] = image+0x%llx\n", i, static_cast<unsigned long long>(word - base));
            ++shown;
        }
    }
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}


// The settings of the Windows shell: Caravan4URecomp.ini beside the executable (portable: nothing goes to the registry or to
// %APPDATA%). "key = value" lines, "#" starts a comment; the command line overrides it. Keys: game, code, user (the game
// folder, the code library and the user folder), options (more runner options, e.g. "--room-host 24872 --nick Hunter"; "%EXEDIR%" is the program's folder),
// scale, volume (0 to 100, 50 by default), vsync (on|off), fullscreen (on|off), width, height, display (-1: the primary display; the first
// one that is not the primary in a test run),
// hd_textures (on|off: off reads no HD texture pack), skip_key, circle_pad_pro (on|off), mouse_camera (on|off), fps_counter (on|off: the frame rate over the game, off by default). The shell writes fullscreen, width and height back when it
// closes.
struct Ini {
    std::string path;
    std::string exedir;  // the folder of the settings file (that of the program): what %EXEDIR% stands for in a value
    std::vector<std::string> lines;

    static std::string Trim(std::string t) {
        const char* ws = " \t\r\n";
        t.erase(0, t.find_first_not_of(ws));
        t.erase(t.find_last_not_of(ws) + 1);
        return t;
    }
    void Load(const std::string& file) {
        path = file;
        const std::size_t slash = file.find_last_of("\\/");
        exedir = slash == std::string::npos ? std::string(".") : file.substr(0, slash);
        if (FILE* f = std::fopen(file.c_str(), "rb")) {
            std::string all;
            char buf[4096];
            std::size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) all.append(buf, n);
            std::fclose(f);
            if (all.compare(0, 3, "\xEF\xBB\xBF") == 0) all.erase(0, 3);  // a byte order mark
            std::size_t pos = 0;
            while (pos <= all.size()) {
                const std::size_t end = all.find('\n', pos);
                const std::string line = Trim(all.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
                if (!line.empty() && line.rfind("# -- ", 0) != 0) lines.push_back(line);
                if (end == std::string::npos) break;
                pos = end + 1;
            }
        }
    }
    static bool KeyOf(const std::string& line, std::string* key, std::string* value) {
        if (line.empty() || line[0] == '#') return false;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) return false;
        *key = Trim(line.substr(0, eq));
        *value = Trim(line.substr(eq + 1));
        return !key->empty();
    }
    std::string Get(const std::string& key, const std::string& fallback = "") const {
        for (const auto& line : lines) {
            std::string k, v;
            if (KeyOf(line, &k, &v) && k == key) return Expand(v);
        }
        return fallback;
    }
    // %EXEDIR% is the program's folder: a path in the settings that does not depend on where the program was started from
    std::string Expand(std::string v) const {
        for (std::size_t at = v.find("%EXEDIR%"); at != std::string::npos; at = v.find("%EXEDIR%", at + exedir.size())) {
            v.replace(at, 8, exedir);
        }
        return v;
    }
    void Set(const std::string& key, const std::string& value) {
        for (auto& line : lines) {
            std::string k, v;
            if (KeyOf(line, &k, &v) && k == key) {
                line = key + " = " + value;
                return;
            }
        }
        lines.push_back(key + " = " + value);
    }
    // The file as it is written: the header comment, then the settings in groups ("# -- Video --"...), one blank line between groups,
    // and whatever is not known (other keys, other comments) at the end in its own order.
    std::vector<std::string> Tidy() const {
        struct Group { const char* title; std::vector<std::string> keys; };
        static const Group kGroups[] = {
            {"Game", {"game", "code", "user", "options"}},
            {"Video", {"scale", "fullscreen", "width", "height", "display", "vsync", "fps", "steady", "effects", "aspect", "mode",
                       "touch_show", "popup_size", "popup_corner", "fps_counter", "pace", "gpu_thread", "hd_textures"}},
            {"Audio", {"volume"}},
            {"Controls", {"skip_key", "skip_pad", "circle_pad_pro", "mouse_camera", "hid_pads", "hidhide_ask"}},
            {"Online", {"nick", "join_address", "host_port", "lobby", "online_touch"}},
            {"Other", {"language", "discord_presence", "auto_backup", "trace_panels"}},
        };
        std::vector<std::string> out;
        std::size_t first = 0;
        while (first < lines.size() && lines[first][0] == '#') out.push_back(lines[first++]);
        std::vector<bool> used(lines.size(), false);
        for (std::size_t i = 0; i < first; ++i) used[i] = true;
        const auto group_of = [&](const std::string& key) -> int {
            for (std::size_t g = 0; g < std::size(kGroups); ++g) {
                if (std::find(kGroups[g].keys.begin(), kGroups[g].keys.end(), key) != kGroups[g].keys.end()) return static_cast<int>(g);
            }
            return key.rfind("key_", 0) == 0 || key.rfind("pad_", 0) == 0 ? 3 : -1;  // the changed controls go with the controls
        };
        for (std::size_t g = 0; g < std::size(kGroups); ++g) {
            std::vector<std::string> block;
            for (const std::string& key : kGroups[g].keys) {  // in the order of the list
                for (std::size_t i = first; i < lines.size(); ++i) {
                    std::string k, v;
                    if (!used[i] && KeyOf(lines[i], &k, &v) && k == key) { block.push_back(lines[i]); used[i] = true; }
                }
            }
            for (std::size_t i = first; i < lines.size(); ++i) {  // the rest of the group (key_*, pad_*)
                std::string k, v;
                if (!used[i] && KeyOf(lines[i], &k, &v) && group_of(k) == static_cast<int>(g)) { block.push_back(lines[i]); used[i] = true; }
            }
            if (block.empty()) continue;
            out.push_back("");
            out.push_back(std::string("# -- ") + kGroups[g].title + " --");
            out.insert(out.end(), block.begin(), block.end());
        }
        bool any = false;
        for (std::size_t i = first; i < lines.size(); ++i) {
            if (used[i]) continue;
            if (!any) { out.push_back(""); out.push_back("# -- More --"); any = true; }
            out.push_back(lines[i]);
        }
        return out;
    }
    void Save() const {
        if (path.empty()) return;
        const std::string temp = path + ".tmp";
        if (FILE* f = std::fopen(temp.c_str(), "wb")) {
            for (const auto& line : Tidy()) std::fprintf(f, "%s\r\n", line.c_str());
            const bool ok = std::fflush(f) == 0;
            std::fclose(f);
            if (ok && MoveFileExA(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return;
            std::remove(temp.c_str());
        }
    }
};


// ---- the credits (the licence's additional terms keep them: shown when the program opens and with --credits) ---------

constexpr const char* kCreditsTitle =
    "Caravan4URecomp (MH4U for Windows) - a MODIFIED version of 4u-native by khorne-hub (github.com/khorne-hub/4u-native) - based on "
    "MH4UltimateDC by CompaLarios18 and Azahar/Citra - GPL-3.0 - not the official version - the licenses are in the LICENSES folder";
constexpr const char* kCreditsText =
    "Caravan4URecomp (MH4U for Windows)\n"
    "A MODIFIED version of 4u-native (MH4U for Android) by khorne-hub.\n"
    "Official project: https://github.com/khorne-hub/4u-native\n"
    "This is not the official version, and is not made or endorsed by khorne-hub.\n\n"
    "Based on MH4UltimateDC by CompaLarios18 and its contributors\n"
    "(https://github.com/CompaLarios18/MH4UltimateDC).\n"
    "Parts of the runtime are derived from Azahar and Citra (https://github.com/azahar-emu/azahar).\n"
    "Libraries: SDL3, Dear ImGui, plume, volk, Vulkan Memory Allocator, Vulkan-Headers, glslang, {fmt}, xxHash,\n"
    "ENet, inih, LodePNG, bc7enc, and llvm-mingw's runtime. Each keeps its own license.\n\n"
    "Free software under the GNU General Public License, version 3 or (at your option) any later version,\n"
    "with the additional terms of ADDITIONAL-TERMS. The license texts, the third-party notices and the\n"
    "README's Credits and License sections are in the LICENSES folder beside this program.\n"
    "The complete corresponding source code is offered with every copy of this program.\n\n"
    "The game's code is not part of this program: you build your own code library from your own copy of the game.\n"
    "Monster Hunter is a trademark of its owner and the 3DS is a trademark of Nintendo; this project is not\n"
    "affiliated with, endorsed by or sponsored by either of them.\n";



// ---- the first start: building the player's own code library ------------------------------------------------------------
// The game's code is not part of this program: each player builds a code library from their own decrypted copy of the game.
// On the first start (no game set up in the settings) the program asks for the two files, runs PrepareGame.ps1 (which uses the
// Python and the compiler that come with the program, or download once) in a console window that shows its progress, and
// goes on to the game when it is done.

bool PathExists(const std::string& path) {
    return !path.empty() && GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

struct PickState {
    std::atomic<bool> done{false};
    std::string path;
};
void SDLCALL OnPicked(void* userdata, const char* const* files, int /*filter*/) {
    auto* state = static_cast<PickState*>(userdata);
    if (files != nullptr && files[0] != nullptr) state->path = files[0];
    state->done.store(true);
}
std::string PickGameFile(const char* title) {
    static const SDL_DialogFileFilter filters[] = {{"Nintendo 3DS game files", "cia;3ds;cci;cxi"}, {"All files", "*"}};
    PickState state;
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, title);
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, const_cast<SDL_DialogFileFilter*>(filters));
    SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, 2);
    SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFILE, OnPicked, &state, props);
    while (!state.done.load()) {
        SDL_PumpEvents();
        SDL_Delay(20);
    }
    SDL_DestroyProperties(props);
    return state.path;
}

std::wstring WideFromUtf8(const std::string& text) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(std::max(n, 1)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), n);
    wide.resize(static_cast<std::size_t>(std::max(n - 1, 0)));
    return wide;
}

// Asks for the two files (unless they were given), runs PrepareGame.ps1 and waits. False: cancelled or failed (said to the player).
bool RunSetup(const std::string& exedir, std::string base_game, std::string update_game) {
    if (base_game.empty() || update_game.empty()) {
        SDL_ShowSimpleMessageBox(
            SDL_MESSAGEBOX_INFORMATION, "Caravan4URecomp - first start",
            "Caravan4URecomp does not include the game. It needs YOUR OWN decrypted copy of Monster Hunter 4 Ultimate (USA):\n\n"
            "  1) the base game (.cia, .3ds or .cci)\n  2) its v1.3.0 update (.cia)\n\n"
            "Choose them in the next two windows. A window will then show the progress of building the game's code for this PC "
            "(a few minutes; the first time it also downloads a compiler of about 150 MB). It is done once, and what it makes "
            "stays on this PC: do not share it.",
            nullptr);
        if (base_game.empty()) base_game = PickGameFile("1/2 - Choose the decrypted BASE game (USA)");
        if (base_game.empty()) return false;
        if (update_game.empty()) update_game = PickGameFile("2/2 - Choose the decrypted v1.3.0 UPDATE (USA, .cia)");
        if (update_game.empty()) return false;
    }
    char system_dir[MAX_PATH] = {};
    GetSystemDirectoryA(system_dir, MAX_PATH);
    const std::string shell = std::string(system_dir) + "\\WindowsPowerShell\\v1.0\\powershell.exe";
    const std::string command = "\"" + shell + "\" -NoProfile -ExecutionPolicy Bypass -File \"" + exedir + "\\PrepareGame.ps1\" -Base \"" +
                                base_game + "\" -Update \"" + update_game + "\" -Wait";
    std::wstring wide_command = WideFromUtf8(command);
    STARTUPINFOW startup = {};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION info = {};
    if (!CreateProcessW(nullptr, wide_command.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr,
                        WideFromUtf8(exedir).c_str(), &startup, &info)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Caravan4URecomp", "The game could not be prepared: PowerShell did not start.", nullptr);
        return false;
    }
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    CloseHandle(info.hThread);
    if (code != 0) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Caravan4URecomp",
                                 "The game could not be prepared. The messages of the window that just closed say why (a file that is "
                                 "encrypted, or not the USA game with its v1.3.0 update, are the usual causes).",
                                 nullptr);
        return false;
    }
    return true;
}

bool Has(const std::vector<std::string>& args, const char* name) {
    for (const auto& a : args) {
        if (a == name) return true;
    }
    return false;
}

int Fail(const char* what, const std::string& why) {
    std::fprintf(stderr, "mh4u-win: %s: %s\n", what, why.c_str());
    return 3;
}

}  // namespace

int main(int argc, char** argv) {
    // A windows-subsystem program (no console window of its own): where do the log and the messages go? Redirected by
    // whoever started it: there. Started from a terminal: to that terminal. Started by a double click: to
    // Caravan4URecomp.log beside the executable (overwritten at each start).
    {
        HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
        if (err == nullptr || err == INVALID_HANDLE_VALUE) {
            if (AttachConsole(ATTACH_PARENT_PROCESS)) {
                std::freopen("CONOUT$", "w", stdout);
                std::freopen("CONOUT$", "w", stderr);
            } else {
                const char* base = SDL_GetBasePath();
                const std::string log = std::string(base ? base : "") + "Caravan4URecomp.log";
                std::freopen(log.c_str(), "w", stdout);
                std::freopen(log.c_str(), "w", stderr);
            }
        }
        std::setvbuf(stderr, nullptr, _IONBF, 0);
    }
    Ini ini;
    {
        const char* base = SDL_GetBasePath();
        ini.Load(std::string(base ? base : "") + "Caravan4URecomp.ini");
    }
    const auto on = [](const std::string& v, bool fallback) {
        return v.empty() ? fallback : (v != "off" && v != "0" && v != "false");
    };
    double seconds = 0;  // 0: until the window is closed
    bool vsync = on(ini.Get("vsync"), true);
    bool mouse_camera = on(ini.Get("mouse_camera"), false);
    bool log_input = false;
    bool fullscreen = on(ini.Get("fullscreen"), false);
    int window_w = std::max(320, std::atoi(ini.Get("width", "1200").c_str()));
    int window_h = std::max(240, std::atoi(ini.Get("height", "720").c_str()));
    std::string skip_key_name = ini.Get("skip_key", "Tab");  // "none": no skip key
    std::string scale_value = ini.Get("scale", "3");
    bool show_fps = on(ini.Get("fps_counter"), false);  // the counter over the game: off unless the settings say so
    std::string volume_value = ini.Get("volume", "50");  // percent, 0 to 100
    int display_index = std::atoi(ini.Get("display", "-1").c_str());  // -1: the first display that is not the primary
    bool force_setup = false;
    std::string base_game, update_game;  // --base-game / --update: the files for the setup, without asking
    bool popup = ini.Get("mode").empty() || ini.Get("mode") == "popup";  // the default: a full top screen and a touch popup
    sl::Mode mode = popup ? sl::Mode::Stacked : sl::ParseMode(ini.Get("mode"));
    std::string touch_show = ini.Get("touch_show", "auto");  // auto | always | hidden: when the popup shows
    g_popup_size.store(std::clamp(std::atoi(ini.Get("popup_size", "38").c_str()), 15, 80));
    for (int i = 0; i < 6; ++i) {
        if (ini.Get("aspect") == kAspects[i]) g_aspect.store(i);
    }
    {
        const std::string corner = ini.Get("popup_corner", "bottom-right");
        g_popup_corner.store(corner == "top-left" ? 0 : corner == "top-right" ? 1 : corner == "bottom-left" ? 2 : 3);
    }
    std::vector<std::string> online_args;  // --join / --host / --nick / --room-password, as the runner's own options
    std::vector<std::string> game_args;  // after "--": the runner's arguments (PROGRAM --code LIB ...)
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--") {
            for (int j = i + 1; j < argc; ++j) game_args.emplace_back(argv[j]);
            break;
        }
        const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (arg == "--seconds") {
            seconds = std::atof(next().c_str());
        } else if (arg == "--vsync") {
            vsync = next() != "off";
        } else if (arg == "--mode") {
            const std::string name = next();
            popup = name == "popup";
            mode = sl::ParseMode(name);
        } else if (arg == "--skip-key") {
            skip_key_name = next();
        } else if (arg == "--setup") {
            force_setup = true;
        } else if (arg == "--base-game") {
            base_game = next();
            force_setup = true;
        } else if (arg == "--update") {
            update_game = next();
            force_setup = true;
        } else if (arg == "--credits") {
            std::fputs(kCreditsText, stdout);
            return 0;
        } else if (arg == "--list-rooms") {
            // Public rooms of a lobby (Azahar's lobby web API, plain HTTP only): prints them and exits. The optional
            // address, else `lobby` of the ini, else the community lobby the upstream app offers.
            std::string url = (i + 1 < argc && std::strncmp(argv[i + 1], "--", 2) != 0) ? argv[++i] : ini.Get("lobby");
            if (url.empty()) url = "http://88.198.47.46:5000";
            const mh4n::rooms::LobbyList list = mh4n::rooms::FetchLobby(url, mh4n::rooms::kMh4uUsa, 10000);
            if (!list.ok) {
                std::printf("lobby %s: %s\n", url.c_str(), list.error.c_str());
                return 1;
            }
            std::printf("lobby %s: %zu room(s); * = set up for MH4U. Join: options = --room-join HOST:PORT --nick NAME [--room-password TEXT]\n",
                        url.c_str(), list.rooms.size());
            for (const mh4n::rooms::LobbyRoom& room : list.rooms) {
                std::string players;
                for (const std::string& p : room.players) players += (players.empty() ? "" : ", ") + p;
                std::printf("%s \"%s\"  %s:%u  %zu/%u  %s%s  [%s]\n", room.preferred ? "*" : " ", room.name.c_str(),
                            room.address.c_str(), room.port, room.players.size(), room.max_players, room.game.c_str(),
                            room.password ? "  (password)" : "", players.c_str());
            }
            return 0;
        } else if (arg == "--log-input") {
            log_input = true;
        } else if (arg == "--mouse-camera") {
            mouse_camera = true;
        } else if (arg == "--display") {
            display_index = std::atoi(next().c_str());
        } else if (arg == "--join" || arg == "--host" || arg == "--nick" || arg == "--room-password") {
            // network rooms before the game starts (the in-game menu, F1, does the same while it runs)
            online_args.push_back(arg == "--join" ? "--room-join" : arg == "--host" ? "--room-host" : arg);
            online_args.push_back(next());
        } else {
            std::fprintf(stderr, "usage: Caravan4URecomp [--seconds N] [--vsync on|off] [--mode popup|overlay|stacked|single|side] "
                                 "[--display N] [--credits] [--list-rooms [URL]] [--join HOST:PORT | --host PORT] [--nick NAME] [--room-password TEXT] [--setup] [--base-game FILE --update FILE] [--mouse-camera] [--skip-key NAME|none] [-- PROGRAM --code LIB [runner options]]\n"
                                 "without a program it shows a test pattern\n");
            return arg == "--help" ? 0 : 2;
        }
    }

    const bool args_from_command_line = !game_args.empty();
    const auto build_game_args = [&]() {
        std::vector<std::string> game_args;
        if (ini.Get("game").empty()) return game_args;
        game_args = {ini.Get("game")};
        if (!ini.Get("code").empty()) { game_args.push_back("--code"); game_args.push_back(ini.Get("code")); }
        if (!ini.Get("user").empty()) { game_args.push_back("--user"); game_args.push_back(ini.Get("user")); }
        if (!on(ini.Get("circle_pad_pro"), true)) { game_args.push_back("--circle-pad-pro"); game_args.push_back("off"); }
        // more runner options, split at spaces outside double quotes (a path may hold spaces: --perf-log "%EXEDIR%\x.csv")
        const std::string extra = ini.Get("options");
        std::string word;
        bool quoted = false, any = false;
        for (const char c : extra) {
            if (c == '"') {
                quoted = !quoted;
                any = true;
            } else if (c == ' ' && !quoted) {
                if (any) game_args.push_back(word);
                word.clear();
                any = false;
            } else {
                word.push_back(c);
                any = true;
            }
        }
        if (any) game_args.push_back(word);
        return game_args;
    };
    if (game_args.empty()) game_args = build_game_args();
    const auto add_extras = [&] {
    if (!game_args.empty()) {
        // the settings the menu keeps in the settings file and the game reads as it runs: applied from the first frame
        for (const char* key : {"fps", "steady", "effects"}) {
            const std::string v = ini.Get(key);
            if (!v.empty() && v != "default") {
                game_args.push_back("--set");
                game_args.push_back(std::string("0:") + key + "=" + v);
            }
        }
        // pace: frames start on the emulated VBlank (the game's own limiter alone drifts against the VBlank and the
        // display's vsync: now and then a picture is shown twice or never, which feels like stutter at any counter value)
        // (a start-up option: the environment variable the hooks read; ini `pace = limiter` turns it off)
        if (ini.Get("trace_panels") == "on" && !std::getenv("MH4U_TRACE_PANELS")) _putenv_s("MH4U_TRACE_PANELS", "1");
        // hd_textures = off: no texture pack is read (the folder stays as it is), to compare with and without
        if (!on(ini.Get("hd_textures"), true) && !std::getenv("MH4N_HD_TEXTURES")) _putenv_s("MH4N_HD_TEXTURES", "0");
        if (!std::getenv("MH4U_PACE")) _putenv_s("MH4U_PACE", ini.Get("pace").empty() ? "vblank" : ini.Get("pace").c_str());
        if (popup) {  // the touch screen's menus go to the top screen (the game's own single-screen mode)
            game_args.push_back("--set");
            game_args.push_back("0:single_screen=on");
        }
        if (!online_args.empty()) {
            const bool has_nick = std::find(online_args.begin(), online_args.end(), "--nick") != online_args.end();
            game_args.insert(game_args.end(), online_args.begin(), online_args.end());
            if (!has_nick && !ini.Get("nick").empty()) {
                game_args.push_back("--nick");
                game_args.push_back(ini.Get("nick"));
            }
        }
    }
    };
    add_extras();
    AddVectoredExceptionHandler(0, StackWords);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        return Fail("SDL_Init", SDL_GetError());
    }
    // The first start: no game set up yet (or --setup): the window asks for the files and shows the building of the player's
    // own code library (below, once it exists).
    bool need_setup = false;
    {
        const bool have_setup = PathExists(ini.exedir + "\\PrepareGame.ps1");
        const bool configured = PathExists(ini.Get("game")) && PathExists(ini.Get("code"));
        need_setup = !args_from_command_line && have_setup && (force_setup || !configured);
    }
    // The window opens on the primary display, centred there. A test run (MH4U_TEST_SCRIPT or MH4U_SECONDARY set) opens on the
    // first display that is not the primary instead; a machine with one display uses it. --display N picks display N (0 is the
    // first SDL lists) instead.
    const bool on_secondary = std::getenv("MH4U_TEST_SCRIPT") || std::getenv("MH4U_SECONDARY");
    SDL_DisplayID display = 0;
    {
        int count = 0;
        SDL_DisplayID* displays = SDL_GetDisplays(&count);
        const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
        if (displays && count > 0) {
            display = primary ? primary : displays[0];
            if (display_index >= 0 && display_index < count) {
                display = displays[display_index];
            } else if (on_secondary) {
                for (int i = 0; i < count; ++i) {
                    if (displays[i] != primary) {
                        display = displays[i];
                        break;
                    }
                }
            }
        }
        SDL_free(displays);
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, kCreditsTitle);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, window_w);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, window_h);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, fullscreen);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
    SDL_Window* window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (!window) {
        return Fail("SDL_CreateWindow", SDL_GetError());
    }
    std::printf("mh4u-win: display %u (%s)\n", display, SDL_GetDisplayName(display));

    // The Vulkan instance gets the surface extensions SDL lists for this window (VK_KHR_win32_surface on Windows).
    rhi::InstanceOptions instance_options;
    Uint32 extension_count = 0;
    if (const char* const* names = SDL_Vulkan_GetInstanceExtensions(&extension_count)) {
        for (Uint32 i = 0; i < extension_count; ++i) {
            instance_options.extensions.emplace_back(names[i]);
        }
    }
    auto instance = rhi::CreateInstance(instance_options);
    if (!instance.ok()) {
        return Fail("CreateInstance", instance.error());
    }
    VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(window, rhi::NativeInstance(**instance), nullptr, &vk_surface)) {
        return Fail("SDL_Vulkan_CreateSurface", SDL_GetError());
    }
    // The 32 MiB allocator blocks are for phones: at 8x and above the screens' images outgrow them and the picture stays
    // black. A desktop card uses the allocator's own (256 MiB) blocks.
    if (!std::getenv("MH4N_VMA_BLOCK_MB")) _putenv_s("MH4N_VMA_BLOCK_MB", "0");
    rhi::BackendOptions backend_options;
    backend_options.surface = rhi::SurfaceFrom(vk_surface);
    auto backend = rhi::CreateBackend(*instance, backend_options);
    if (!backend.ok()) {
        return Fail("CreateBackend", backend.error());
    }

    int pixel_w = 0, pixel_h = 0;
    SDL_GetWindowSizeInPixels(window, &pixel_w, &pixel_h);
    g_layout_mode.store(static_cast<int>(mode));
    g_popup.store(popup);
    sc::LayoutSource layout = [] {
        sc::LayoutRequest request;
        request.inputs.mode = static_cast<sl::Mode>(g_layout_mode.load());
        request.inputs.aspect_ratio = kAspects[std::clamp(g_aspect.load(), 0, 5)];
        if (g_popup.load()) {  // a full top screen; the touch screen over it in a corner while it is wanted
            request.inputs.mode = g_touch_visible.load() ? sl::Mode::Overlay : sl::Mode::Single;
            Port::ScreenOverlay overlay;
            overlay.height = static_cast<float>(g_popup_size.load()) / 100.0f;
            overlay.opacity = 1.0f;
            overlay.corner = static_cast<Port::ScreenOverlay::Corner>(g_popup_corner.load());
            request.inputs.overlay = overlay;
        } else if (request.inputs.mode == sl::Mode::Overlay) {  // MODIFIED: "overlay" = the full top screen with the touch screen always over it (popup_size, popup_corner); popup shows it only when needed
            Port::ScreenOverlay overlay;
            overlay.height = static_cast<float>(g_popup_size.load()) / 100.0f;
            overlay.opacity = 1.0f;
            overlay.corner = static_cast<Port::ScreenOverlay::Corner>(g_popup_corner.load());
            request.inputs.overlay = overlay;
        }
        request.options.linear_filter = true;  // bilinear when the window is not a whole multiple of the picture
        return request;
    };
    sc::Options presenter_options;
    presenter_options.vsync = vsync;
    auto presenter = sc::Presenter::Create(*backend, sc::Target::Window(backend_options.surface, pixel_w, pixel_h),
                                           layout, presenter_options);
    if (!presenter.ok()) {
        return Fail("Presenter::Create", presenter.error());
    }

    g_cursor.presenter.store(presenter->get());
    (*presenter)->SetUiLayer(&g_cursor);
    g_cursor.show_fps.store(show_fps);
    if (std::getenv("MH4U_TEST_CURSOR")) g_cursor.visible.store(true);  // draws the selector without a game (a check)
    std::printf("mh4u-win: window %dx%d, vsync %s\n", pixel_w, pixel_h, vsync ? "on" : "off");

    // The input: the keyboard, the mouse and every controller as one 3DS pad (the app's InputMapper), which feeds the
    // runtime's live input source. Audio is the runtime's own SDL sink (--audio sdl).
    bool game = !game_args.empty() && !need_setup;
    auto pads = std::make_unique<HidAugmentedPads>(mh4n::app::MakeSdlGamepads());
    const char* hid_env = std::getenv("MH4U_HID_PADS");
    const bool hid_on = on(ini.Get("hid_pads"), true) && !(hid_env && std::string(hid_env) == "0");
    if (hid_on) hidpads::Start();
    // HidHide (DSX's leftover) hides a PlayStation pad from every program not on its list: offer to add this one (once; "No" is kept)
    if (hid_on && game && !std::getenv("MH4U_TEST_SCRIPT") && on(ini.Get("hidhide_ask"), true)) {
        wchar_t exe_buffer[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe_buffer, MAX_PATH);
        const std::filesystem::path exe_path(exe_buffer);
        const std::filesystem::path cli = hidhide::HidingFrom(exe_path, hidpads::XInputConnected());
        if (!cli.empty()) {
            std::string lang = ini.Get("language", "auto");
            if (lang != "es" && lang != "en") {
                wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
                GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH);
                lang = name[0] == L'e' && name[1] == L's' ? "es" : "en";
            }
            const bool es = lang == "es";
            const std::string message =
                es ? "HidHide (de DSX o DS4Windows) esta ocultando tu mando de PlayStation a los programas que no estan en su lista, "
                     "asi que Caravan4URecomp no puede leerlo.\n\nPuedo anadir Caravan4URecomp a la lista de HidHide (pide permiso de "
                     "administrador; se deshace con HidHideCLI --app-unreg). No toco nada mas de HidHide.\n\nSi usas DSX o DS4Windows abiertos "
                     "no hace falta."
                   : "HidHide (from DSX or DS4Windows) is hiding your PlayStation controller from programs that are not on its list, so "
                     "Caravan4URecomp cannot read it.\n\nI can add Caravan4URecomp to HidHide's list (it asks for administrator permission; "
                     "undo with HidHideCLI --app-unreg). Nothing else of HidHide is touched.\n\nIf you run DSX or DS4Windows it is not needed.";
            SDL_MessageBoxButtonData buttons[2] = {{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, es ? "Permitir" : "Allow"},
                                                   {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, es ? "No mas" : "Not again"}};
            SDL_MessageBoxData box{};
            box.flags = SDL_MESSAGEBOX_INFORMATION;
            box.window = window;
            box.title = "Caravan4URecomp - HidHide";
            box.message = message.c_str();
            box.numbuttons = 2;
            box.buttons = buttons;
            int chosen = 0;
            if (SDL_ShowMessageBox(&box, &chosen) && chosen == 1) {
                const bool done = hidhide::Allow(cli, exe_path);
                std::fprintf(stderr, "mh4u-win: HidHide: %s\n", done ? "Caravan4URecomp added to its list" : "could not add it (permission refused?)");
            } else {
                ini.Set("hidhide_ask", "off");
                ini.Save();
            }
        }
    }
    mh4n::app::InputMapper mapper(pads.get());
    // The mouse is the stylus (the touch screen) by default. The Azahar app's "auto" mode turns it into the camera as soon
    // as the keyboard is used, and then a click does nothing on the touch screen: --mouse-camera asks for that mode.
    // mouse_camera = on: the mouse is the camera (captured by the window; Left Alt held = the stylus, F3 toggles).
    App::Config input_config = App::Defaults();
    const auto apply_mouse_mode = [&] {
        input_config.input_mode = mouse_camera ? "keyboard_mouse" : "controller";
        mapper.Configure(input_config);
    };
    // The controls the player changed in the menu: for each 3DS button of ingame::kBindNames, a key (settings key_<name>:
    // an SDL key name, empty = the default keys) and a controller bind (pad_<name>: b<n> a button, a<n>+ or a<n>- an axis,
    // none, empty = the default).
    static const int kBindIds[ingame::kBindCount] = {App::Button::A,    App::Button::B,    App::Button::X,    App::Button::Y,
                                                     App::Button::L,    App::Button::R,    App::Button::ZL,   App::Button::ZR,
                                                     App::Button::Start, App::Button::Select, App::Button::Up, App::Button::Down,
                                                     App::Button::Left, App::Button::Right};
    const App::Config default_input = App::Defaults();
    const auto bind_setting = [&](int i, const char* what) { return ini.Get(std::string(what) + "_" + ingame::Lower(ingame::kBindNames[i])); };
    const auto apply_binds = [&] {
        mapper.ClearPadBinds();
        for (int i = 0; i < ingame::kBindCount; ++i) {
            const auto id = static_cast<std::size_t>(kBindIds[i]);
            const std::string key = bind_setting(i, "key");
            input_config.keys[id] = key.empty() ? default_input.keys[id] : key;
            const std::string pad = bind_setting(i, "pad");
            if (pad == "none") {
                mapper.SetPadBind(kBindIds[i], 0, -1, true);
            } else if (pad.size() >= 2 && pad[0] == 'b') {
                mapper.SetPadBind(kBindIds[i], 1, std::atoi(pad.c_str() + 1), true);
            } else if (pad.size() >= 3 && pad[0] == 'a' && (pad.back() == '+' || pad.back() == '-')) {
                mapper.SetPadBind(kBindIds[i], 2, std::atoi(pad.c_str() + 1), pad.back() == '+');
            }
        }
        mapper.Configure(input_config);
    };
    apply_mouse_mode();
    apply_binds();
    mapper.on_state = [log_input](const mh4n::InputState& state) {
        if (log_input) {
            std::fprintf(stderr, "mh4u-win: input buttons 0x%08x\n", static_cast<unsigned>(state.buttons));
        }
        SendInput(state);
    };

    std::thread game_thread;
    const auto user_folder = [&]() -> std::string {  // the --user of the game's arguments ("" when none)
        for (std::size_t i = 0; i + 1 < game_args.size(); ++i) {
            if (game_args[i] == "--user") return game_args[i + 1];
        }
        return {};
    };
    const auto user_path = [&] { return std::filesystem::path(WideFromUtf8(user_folder())); };
    const auto start_game = [&] {
        if (!user_folder().empty()) {  // the saves: a restore that was asked for, then a backup of them as they are
            const std::string restored = savemgr::ApplyPendingRestore(user_path());
            if (!restored.empty()) std::printf("mh4u-win: saves: %s\n", restored.c_str());
            for (const std::string& n : modmgr::DisableCodeMods(user_folder())) {
                std::printf("mh4u-win: mods: %s changes the game's code, which this program can't use: turned off\n", n.c_str());
            }
            if (ini.Get("auto_backup") != "off") std::printf("mh4u-win: saves: %s\n", savemgr::AutoBackup(user_path(), 5).c_str());
        }
        // crash files in <user>/log/crash-<pid>.txt (installed before Boot, so that the memory module's fault handler
        // chains to it)
        for (std::size_t i = 0; i + 1 < game_args.size(); ++i) {
            if (game_args[i] == "--user") mh4n::app::report::Install(game_args[i + 1]);
        }
        g_presenter.store(presenter->get());
        pr::SetSharedBackend(*backend);  // the renderer uses this device (N2b rule 4)
        std::vector<std::string> args = {"mh4u-win"};
        args.insert(args.end(), game_args.begin(), game_args.end());
        const auto give = [&](const char* name, const char* value) {
            if (!Has(args, name)) {
                args.emplace_back(name);
                args.emplace_back(value);
            }
        };
        {   // The GPU on a thread of its own (ini gpu_thread = on|off, or MH4U_GPU_THREAD=1): the game thread only queues
            // the GPU's work. Off by default until it is proven on every GPU (the MH3U port saw AMD drivers keep memory).
            const char* env = std::getenv("MH4U_GPU_THREAD");
            const bool threaded = env != nullptr ? *env == '1' : ini.Get("gpu_thread") == "on";
            give("--gpu", threaded ? "native-display-queue" : "native-display");
            give("--async-screens", "on");
        }
        for (std::string& a : args) {  // fps = display: the refresh rate of the display the window is on
            if (a == "0:fps=display") a = "0:fps=" + std::to_string(DisplayHz(window));
            if (a == "0:fps=unlimited") a = "0:fps=1000";  // no limit in practice: the game runs as fast as it can
        }
        give("--audio", "sdl");
        give("--input-source", "live");
        give("--input-latch", "none");
        // "auto": a whole number of times the 3DS's 240 rows that is nearest to the window's height (3 at 720, 5 at 1080)
        const std::string scale_arg = scale_value == "auto" ? std::to_string(std::clamp((pixel_h + 120) / 240, 1, 8))
                                                           : std::to_string(std::clamp(std::atoi(scale_value.c_str()), 1, 8));
        give("--scale", scale_arg.c_str());
        {   // Every emulated VBlank reads the two scaled screens back (CPU copy), so the VBlank rate (= an fps cap above 60) is
            // limited by how many pixels that is: ~1e9 pixels a second were measured to be fine on this PC; 2000 reads a second
            // of 3.4 MB (fps unlimited at 3x) starved the game thread and the picture stayed black. Never below 60.
            const int s = std::atoi(scale_arg.c_str());
            // (2026-11-20: with the direct screens - the renderer's own images given to the presenter, nothing read back - the
            // VBlank costs the GPU a copy only, so the ceiling is four times higher; MH4N_SCREEN_DIRECT=0 keeps the old one.)
            const char* direct_env = std::getenv("MH4N_SCREEN_DIRECT");
            const bool direct = direct_env == nullptr || *direct_env != '0';
            const double pixels_per_second = direct ? 4.0e9 : 1.0e9;
            const int max_hz = std::max(60, std::min(1000, static_cast<int>(pixels_per_second / (172800.0 * s * s))));
            args.emplace_back("--set");
            args.emplace_back("0:max_refresh=" + std::to_string(max_hz));
        }
        give("--volume", volume_value.c_str());  // the runner's own option of the SDL audio device (app/audio)  // internal resolution: the runner's default is the 3DS's own (1), which looks soft on a PC
        // the runner stops after 10 s of game time unless told otherwise; --frames alone means no time limit
        // (what the Android app does)
        if (!Has(args, "--ms") && !Has(args, "--frames")) give("--frames", "4000000000");
        game_thread = std::thread([args] {
            std::vector<const char*> argv_game;
            for (const auto& a : args) argv_game.push_back(a.c_str());
            const mh4n::Options options = mh4n::ParseOptions("runner", static_cast<int>(argv_game.size()), argv_game.data());
            int status = 2;
            if (options.ok()) {
                mh4n::Runtime runtime;
                status = runtime.Boot(options);
                const std::lock_guard<std::recursive_mutex> guard(g_runtime_mutex);
                g_runtime.store(nullptr);  // (before the runtime is destroyed: the shell may still be using it)
            }
            std::printf("mh4u-win: the game ended with status %d\n", status);
            g_game_status.store(status);
            g_runtime.store(nullptr);
            g_game_done.store(true);
        });
    };
    if (game) start_game();

    KeyboardTyping typing;
    SDL_Keycode skip_key = skip_key_name == "none" ? SDLK_UNKNOWN : SDL_GetKeyFromName(skip_key_name.c_str());
    // MODIFIED: skip_pad = "b<N>" (a controller button) or "a<N>+" / "a<N>-" (a trigger or stick axis), chosen in the menu's Controls page; empty = none
    int skip_pad_kind = 0, skip_pad_index = 0;
    bool skip_pad_positive = true;
    const auto set_skip_pad = [&](const std::string& text) {
        skip_pad_kind = 0;
        g_skip_pad_held = false;
        if (text.size() >= 2 && (text[0] == 'b' || text[0] == 'a')) {
            skip_pad_kind = text[0] == 'b' ? 1 : 2;
            skip_pad_index = std::atoi(text.c_str() + 1);
            skip_pad_positive = text.back() != '-';
        }
    };
    set_skip_pad(ini.Get("skip_pad"));
    if (game && skip_key == SDLK_UNKNOWN && skip_key_name != "none") {
        std::fprintf(stderr, "mh4u-win: --skip-key %s is not a key name; the skip key is off\n", skip_key_name.c_str());
    }
    sl::TouchTracker tracker;
    bool touching = false;
    // the frame rates in the window's title, once a second: what the game publishes and what the window shows
    auto counter_at = std::chrono::steady_clock::now();
    std::uint64_t counter_published = 0;
    auto rate_at = std::chrono::steady_clock::now() + std::chrono::seconds(8);  // (the credits show in the title until then)
    std::uint64_t rate_published = 0, rate_presented = 0;
    // Discord Rich Presence (ini discord_presence = off to turn it off): the game name and the time played, nothing else
    discord::Presence discord_presence;
    if (game && on(ini.Get("discord_presence"), true)) discord_presence.Start("Monster Hunter 4 Ultimate", "Playing on Caravan4URecomp");
    const auto start = std::chrono::steady_clock::now();
    const auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); };
    std::uint64_t sequence = 0;
    bool running = true;

    // The in-game settings menu (F1, or L3 + R3 on a controller): what each setting is now and how a change is
    // applied to the running game and kept in the settings file.
    ingame::Overlay menu_overlay;
    {   // the menu's look is drawn on the present thread with Dear ImGui over this device, in the swapchain's format
        int surface_format = static_cast<int>(VK_FORMAT_B8G8R8A8_UNORM);
        const rhi::VulkanHandles handles = rhi::Handles(**backend);
        std::uint32_t count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(handles.physical_device, vk_surface, &count, nullptr);
        std::vector<VkSurfaceFormatKHR> offered(count);
        if (count > 0) vkGetPhysicalDeviceSurfaceFormatsKHR(handles.physical_device, vk_surface, &count, offered.data());
        if (const auto chosen = mh4n::swapchain::ChooseSurfaceFormat(offered)) surface_format = static_cast<int>(chosen->format);
        sc::Presenter* present_thread = presenter->get();
        std::shared_ptr<rhi::Backend> shared_backend = *backend;
        menu_overlay.UseGui([shared_backend, surface_format, present_thread] {
            return ingame::MakeGuiRenderer(shared_backend, surface_format, [present_thread] { present_thread->RequestRedraw(); });
        });
    }
    if (ini.Get("nick").empty()) {  // a nickname for the rooms that is unlikely to be taken
        ini.Set("nick", "Hunter" + std::to_string(1000 + static_cast<int>(SDL_GetPerformanceCounter() % 9000)));
        ini.Save();
    }
    ingame::Host menu_host;
    menu_host.value = [&](const std::string& key) -> std::string {
        if (key == "skip_binding") {  // the keyboard key and the controller's button / axis that skip a cutscene
            std::string pad = "-";
            if (skip_pad_kind == 1) {
                const char* n = SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(skip_pad_index));
                pad = n ? n : "?";
            }
            if (skip_pad_kind == 2) {
                const char* n = SDL_GetGamepadStringForAxis(static_cast<SDL_GamepadAxis>(skip_pad_index));
                pad = std::string(n ? n : "?") + (skip_pad_positive ? "+" : "-");
            }
            return (skip_key == SDLK_UNKNOWN ? std::string("-") : std::string(SDL_GetKeyName(skip_key))) + "   |   " + pad;
        }
        if (key == "fullscreen") return fullscreen ? "on" : "off";
        if (key == "vsync") return vsync ? "on" : "off";
        if (key == "mode") return g_popup.load() ? "popup" : sl::ModeName(static_cast<sl::Mode>(g_layout_mode.load()));
        if (key == "touch_show") return touch_show;
        if (key == "aspect") return kAspects[std::clamp(g_aspect.load(), 0, 5)];
        if (key == "popup_size") return std::to_string(g_popup_size.load());
        if (key == "popup_corner") {
            static const char* const kCorners[] = {"top-left", "top-right", "bottom-left", "bottom-right"};
            return kCorners[std::clamp(g_popup_corner.load(), 0, 3)];
        }
        if (key == "fps_counter") return show_fps ? "on" : "off";
        if (key == "scale") return scale_value == "auto" ? scale_value : std::to_string(std::clamp(std::atoi(scale_value.c_str()), 1, 8));  // (9 and 10 of an older settings file read as 8)
        if (key == "volume") return volume_value;
        if (key == "system_language") {  // the language of Windows: "es" or "en"
            wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
            GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH);
            return name[0] == L'e' && name[1] == L's' ? "es" : "en";
        }
        if (key == "language") return ini.Get("language", "auto");
        if (key == "scale_auto") {  // the multiple "auto" picks for this window
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            return std::to_string(std::clamp((h + 120) / 240, 1, 8));
        }
        if (key == "lobby") return ini.Get("lobby").empty() ? "http://88.198.47.46:5000" : ini.Get("lobby");
        if (key.rfind("bind:", 0) == 0) {  // "A button: its keys, and its controller's button"
            const int i = std::atoi(key.c_str() + 5);
            if (i < 0 || i >= ingame::kBindCount) return "";
            const auto id = static_cast<std::size_t>(kBindIds[i]);
            std::string keys = bind_setting(i, "key");
            if (keys.empty()) keys = default_input.keys[id];
            std::string pad = bind_setting(i, "pad");
            std::string pad_text;
            const auto pad_name = [](int kind, int index, bool greater) -> std::string {
                static const char* const kButtons[] = {"Pad A", "Pad B", "Pad X", "Pad Y", "Back", "Guide", "Start", "L3", "R3", "LB", "RB", "Pad up", "Pad down", "Pad left", "Pad right"};
                if (kind == 1) return index >= 0 && index < 15 ? kButtons[index] : "?";
                if (kind == 2) {
                    static const char* const kAxes[] = {"Left stick right", "Left stick left", "Left stick down", "Left stick up", "Right stick right", "Right stick left", "Right stick down", "Right stick up", "LT", "LT", "RT", "RT"};
                    const int at = index * 2 + (greater ? 0 : 1);
                    return index >= 0 && index < 6 ? kAxes[at] : "?";
                }
                return "none";
            };
            if (pad == "none") {
                pad_text = "none";
            } else if (pad.size() >= 2 && pad[0] == 'b') {
                pad_text = pad_name(1, std::atoi(pad.c_str() + 1), true);
            } else if (pad.size() >= 3 && pad[0] == 'a') {
                pad_text = pad_name(2, std::atoi(pad.c_str() + 1), pad.back() == '+');
            } else {
                const mh4n::app::azahar::PadBind bind = mh4n::app::azahar::DefaultButtonBind(kBindIds[i]);
                pad_text = pad_name(bind.kind == mh4n::app::azahar::PadBind::Kind::Button ? 1 : bind.kind == mh4n::app::azahar::PadBind::Kind::Axis ? 2 : 0,
                                    bind.index, bind.greater);
            }
            return keys + "   |   " + pad_text;
        }
        return ini.Get(key, key == "fps" || key == "steady" || key == "effects" ? "default" : "");
    };
    menu_host.apply = [&](const std::string& key, const std::string& value) {
        if (key == "skip_key" || key == "skip_pad") {  // MODIFIED: the skip control, changed in the menu (Backspace there = key Tab, no controller button)
            if (key == "skip_key") {
                const std::string name = value.empty() ? "Tab" : value;
                ini.Set("skip_key", name);
                skip_key = SDL_GetKeyFromName(name.c_str());
                g_skip_held = false;
            } else {
                ini.Set("skip_pad", value);
                set_skip_pad(value);
            }
            ini.Save();
            return;
        }
        if (key.rfind("key_", 0) == 0 || key.rfind("pad_", 0) == 0) {  // a control changed in the menu
            ini.Set(key, value);
            ini.Save();
            apply_binds();
            return;
        }
        if (key == "fullscreen") {
            fullscreen = value == "on";
            SDL_SetWindowFullscreen(window, fullscreen);
        } else if (key == "vsync") {
            vsync = value == "on";
            (*presenter)->SetVsync(vsync);
        } else if (key == "mode") {
            const bool was_popup = g_popup.load();
            g_popup.store(value == "popup");
            g_layout_mode.store(static_cast<int>(sl::ParseMode(value)));
            if (was_popup != g_popup.load()) {  // the game's menus follow: on the top screen in popup mode
                const std::lock_guard<std::recursive_mutex> rt_guard(g_runtime_mutex);
                if (mh4n::Runtime* rt = g_runtime.load()) {
                    const std::string v = g_popup.load() ? "on" : "off";
                    rt->GetKernel().Post([v] { Port::Hooks::SetOption("single_screen", v); });
                }
            }
            (*presenter)->RequestRedraw();
        } else if (key == "aspect") {
            for (int i = 0; i < 6; ++i) {
                if (value == kAspects[i]) g_aspect.store(i);
            }
            (*presenter)->RequestRedraw();
        } else if (key == "touch_show") {
            touch_show = value;
        } else if (key == "popup_size") {
            g_popup_size.store(std::clamp(std::atoi(value.c_str()), 15, 80));
            (*presenter)->RequestRedraw();
        } else if (key == "popup_corner") {
            g_popup_corner.store(value == "top-left" ? 0 : value == "top-right" ? 1 : value == "bottom-left" ? 2 : 3);
            (*presenter)->RequestRedraw();
        } else if (key == "fps_counter") {
            show_fps = value == "on";
            g_cursor.show_fps.store(show_fps);
        } else if (key == "scale") {
            scale_value = value;  // read when the game starts
        } else if (key == "volume") {
            volume_value = value;
            const std::lock_guard<std::recursive_mutex> rt_guard(g_runtime_mutex);
            if (mh4n::Runtime* rt = g_runtime.load()) mh4n::app_audio::SetVolume(*rt, std::atoi(value.c_str()));
        } else if (key == "fps" || key == "steady" || key == "effects") {
            // the hooks read these as the game runs; "default" for the other two takes effect at the next start
            const std::lock_guard<std::recursive_mutex> rt_guard(g_runtime_mutex);
            if (mh4n::Runtime* rt = g_runtime.load()) {
                if (key == "fps" || value != "default") {
                    const std::string k = key;
                    const std::string v = key == "fps" ? (value == "default" ? "0" : value == "display" ? std::to_string(DisplayHz(window))
                                                      : value == "unlimited" ? "1000" : value)
                                      : value;
                    rt->GetKernel().Post([k, v] { Port::Hooks::SetOption(k, v); });
                }
            }
        }
        ini.Set(key, value);
        ini.Save();
    };
    menu_host.quit = [&] { running = false; };
    menu_host.action = [&](const std::string& name) -> std::string {
        if (name == "addresses") {  // the addresses a friend can use: "ip|where|vpn" a line, the virtual networks first
            std::string out;
            for (const netaddr::Address& a : netaddr::Find()) out += a.ip + "|" + a.where + "|" + (a.vpn ? "1" : "0") + '\n';
            return out;
        }
        if (name.rfind("copy:", 0) == 0) {
            SDL_SetClipboardText(name.c_str() + 5);
            return "Copied";
        }
        if (name == "paste") {
            char* text = SDL_GetClipboardText();
            std::string out = text ? text : "";
            SDL_free(text);
            return out;
        }
        if (name == "save_info") {  // "files|MB"
            const savemgr::Tree t = savemgr::MeasureSaves(user_path());
            if (t.files == 0) return "";
            char mb[32];
            std::snprintf(mb, sizeof mb, "%.1f", static_cast<double>(t.bytes) / 1048576.0);
            return std::to_string(t.files) + "|" + mb;
        }
        if (name == "save_list") {  // "name|stamp|label|MB" a line, the newest first
            std::string out;
            for (const savemgr::Backup& b : savemgr::List(user_path())) {
                char mb[32];
                std::snprintf(mb, sizeof mb, "%.1f", static_cast<double>(b.bytes) / 1048576.0);
                out += b.name + "|" + b.stamp + "|" + (b.label.empty() ? "-" : b.label) + "|" + mb + "\n";
            }
            return out;
        }
        if (name == "save_pending") return savemgr::Pending(user_path());
        if (name == "save_unstage") {
            savemgr::CancelRestore(user_path());
            return "";
        }
        if (name == "save_backup") {
            std::string error;
            const std::string made = savemgr::MakeBackup(user_path(), "", &error);
            return made.empty() ? error : "Backed up as " + made;
        }
        if (name.rfind("save_stage:", 0) == 0) {
            return savemgr::StageRestore(user_path(), name.substr(11)) ? "It will be put back the next time the game starts." : "That backup is not there.";
        }
        if (name.rfind("save_delete:", 0) == 0) {
            return savemgr::Delete(user_path(), name.substr(12)) ? "The backup was deleted." : "The backup could not be deleted.";
        }
        if (name == "open_saves" || name == "open_backups" || name == "open_mods") {
            if (user_folder().empty()) return "No user folder is set";
            std::filesystem::path dir = user_path();
            dir /= name == "open_saves" ? "sdmc" : name == "open_backups" ? "backups" : "load\\mods";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            if (!std::getenv("MH4U_TEST_SCRIPT")) ShellExecuteW(nullptr, L"open", dir.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return "The folder is open";
        }
        if (name == "mods_list") {  // "name|flags|on" a line, in load order (flags: r romfs, c code)
            std::string out;
            for (const auto& m : modmgr::List(user_folder())) {
                out += m.name + "|" + (m.romfs ? "r" : "") + (m.code ? "c" : "") + "|" + (m.on ? "1" : "0") + "\n";
            }
            return out;
        }
        if (name.rfind("mods_set:", 0) == 0) {  // mods_set:<name>:<on|off>
            const std::string rest = name.substr(9);
            const std::size_t colon = rest.rfind(':');
            if (colon != std::string::npos) modmgr::SetOn(user_folder(), rest.substr(0, colon), rest.substr(colon + 1) == "on");
            return "";
        }
        if (name.rfind("mods_move:", 0) == 0) {  // mods_move:<name>:<-1|1>
            const std::string rest = name.substr(10);
            const std::size_t colon = rest.rfind(':');
            if (colon != std::string::npos) modmgr::Move(user_folder(), rest.substr(0, colon), std::atoi(rest.c_str() + colon + 1));
            return "";
        }
        if (name == "reset_controls") {
            for (int i = 0; i < ingame::kBindCount; ++i) {
                ini.Set("key_" + ingame::Lower(ingame::kBindNames[i]), "");
                ini.Set("pad_" + ingame::Lower(ingame::kBindNames[i]), "");
            }
            ini.Save();
            apply_binds();
            return "Every control is back as it was.";
        }
        if (name == "open_textures") {  // <user>\load\textures, where the runtime reads HD texture packs from at start
            std::string user;
            for (std::size_t i = 0; i + 1 < game_args.size(); ++i) {
                if (game_args[i] == "--user") user = game_args[i + 1];
            }
            if (user.empty()) return "No user folder is set";
            const std::string dir = user + "\\load\\textures";
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(WideFromUtf8(dir)), ec);
            if (!std::getenv("MH4U_TEST_SCRIPT")) {  // (a test doesn't open a window of Explorer)
                ShellExecuteW(nullptr, L"open", WideFromUtf8(dir).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            return "The textures folder is open";
        }
        return "";
    };
    ingame::Menu menu(menu_host, menu_overlay);
    g_cursor.menu = &menu_overlay;
    if (std::getenv("MH4U_TEST_MENU")) menu.Toggle(window);  // draws the menu without a keyboard (a check)
    bool menu_was_open = false;

    // The first start in the window: the files, then the progress. When the look can't be drawn, the console's version.
    ingame::Setup setup(ini.exedir);
    bool setup_active = need_setup;
    if (setup_active) {
        setup.Begin(base_game, update_game, !base_game.empty() && !update_game.empty());
        SDL_SetWindowTitle(window, "Caravan4URecomp - first start");
    }
    const auto finish_setup = [&] {  // the game is ready: its settings are read again and it starts
        setup_active = false;
        menu_overlay.SetSetup(false, {});
        ini.lines.clear();
        ini.Load(ini.path);
        game_args = build_game_args();
        add_extras();
        if (game_args.empty()) {
            running = false;
            return;
        }
        game = true;
        SDL_SetWindowTitle(window, kCreditsTitle);
        start_game();
    };
    bool size_dirty = false;  // the window was resized: its size is saved when it has been still for a second
    auto size_changed_at = std::chrono::steady_clock::now();
    bool touch_forced = false;  // F2: the popup shows whatever the game does
    auto touch_needed_at = std::chrono::steady_clock::now();
    auto touch_need_since = std::chrono::steady_clock::time_point{};  // when the (auto) need began; the popup waits for it to last a moment
    auto room_checked_at = std::chrono::steady_clock::now();
    bool in_room = false;  // connected to a room (Joined or Moderator), looked at twice a second
    const bool log_state = std::getenv("MH4U_LOG_STATE") != nullptr;  // prints the game's state when it changes (tuning)
    Port::Hooks::GameState last_state;
    int last_objects = -1;

    // A test harness (MH4U_TEST_SCRIPT=file): synthetic events pushed into the program's own SDL queue at set times, so
    // a test never touches the real keyboard, mouse or focus. One line per step: "SECONDS COMMAND [ARGS]" with
    //   key NAME | text STRING | move X Y | click X Y [left|right] | wheel DY | pad NAME (a b start guide up down left right lb rb)
    // (times in seconds since the loop started; '#' starts a comment).
    struct ScriptStep {
        double at;
        std::vector<SDL_Event> events;
    };
    std::vector<ScriptStep> script;
    std::size_t script_next = 0;
    if (const char* path = std::getenv("MH4U_TEST_SCRIPT")) {
        if (FILE* f = std::fopen(path, "rb")) {
            char text[512];
            const SDL_WindowID window_id = SDL_GetWindowID(window);
            while (std::fgets(text, sizeof text, f)) {
                char cmd[32] = {}, a1[256] = {}, a2[64] = {}, a3[64] = {};
                double at = 0;
                if (text[0] == '#' || std::sscanf(text, "%lf %31s %255s %63s %63s", &at, cmd, a1, a2, a3) < 2) continue;
                ScriptStep step{at, {}};
                const std::string c = cmd, n1 = a1;
                const auto key = [&](SDL_Keycode code) {
                    for (const bool down : {true, false}) {
                        SDL_Event e = {};
                        e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                        e.key.windowID = window_id;
                        e.key.key = code;
                        e.key.scancode = SDL_GetScancodeFromKey(code, nullptr);
                        e.key.down = down;
                        step.events.push_back(e);
                    }
                };
                const auto mouse_at = [&](float x, float y) {
                    SDL_Event e = {};
                    e.type = SDL_EVENT_MOUSE_MOTION;
                    e.motion.windowID = window_id;
                    e.motion.x = x;
                    e.motion.y = y;
                    step.events.push_back(e);
                };
                if (c == "key") {
                    key(SDL_GetKeyFromName(a1));
                } else if (c == "text") {
                    SDL_Event e = {};
                    e.type = SDL_EVENT_TEXT_INPUT;
                    e.text.windowID = window_id;
                    e.text.text = SDL_strdup(a1);
                    step.events.push_back(e);
                } else if (c == "move") {
                    mouse_at(static_cast<float>(std::atof(a1)), static_cast<float>(std::atof(a2)));
                } else if (c == "click") {
                    mouse_at(static_cast<float>(std::atof(a1)), static_cast<float>(std::atof(a2)));
                    for (const bool down : {true, false}) {
                        SDL_Event e = {};
                        e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
                        e.button.windowID = window_id;
                        e.button.button = std::string(a3) == "right" ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
                        e.button.down = down;
                        e.button.x = static_cast<float>(std::atof(a1));
                        e.button.y = static_cast<float>(std::atof(a2));
                        step.events.push_back(e);
                    }
                } else if (c == "drop") {  // a file dropped on the window
                    SDL_Event e = {};
                    e.type = SDL_EVENT_DROP_FILE;
                    e.drop.windowID = window_id;
                    e.drop.data = SDL_strdup(a1);
                    step.events.push_back(e);
                } else if (c == "wheel") {
                    SDL_Event e = {};
                    e.type = SDL_EVENT_MOUSE_WHEEL;
                    e.wheel.windowID = window_id;
                    e.wheel.integer_y = std::atoi(a1);
                    e.wheel.y = static_cast<float>(e.wheel.integer_y);
                    step.events.push_back(e);
                } else if (c == "pad" || c == "padhold" || c == "padrelease") {  // padhold / padrelease: only the press or the release
                    static const std::pair<const char*, SDL_GamepadButton> kButtons[] = {
                        {"a", SDL_GAMEPAD_BUTTON_SOUTH}, {"b", SDL_GAMEPAD_BUTTON_EAST}, {"start", SDL_GAMEPAD_BUTTON_START},
                        {"l3", SDL_GAMEPAD_BUTTON_LEFT_STICK}, {"r3", SDL_GAMEPAD_BUTTON_RIGHT_STICK}, {"up", SDL_GAMEPAD_BUTTON_DPAD_UP}, {"down", SDL_GAMEPAD_BUTTON_DPAD_DOWN},
                        {"left", SDL_GAMEPAD_BUTTON_DPAD_LEFT}, {"right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
                        {"lb", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {"rb", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}};
                    for (const auto& [name, button] : kButtons) {
                        if (n1 != name) continue;
                        for (const bool down : {true, false}) {
                            if ((c == "padhold" && !down) || (c == "padrelease" && down)) continue;
                            SDL_Event e = {};
                            e.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
                            e.gbutton.which = 0;
                            e.gbutton.button = static_cast<Uint8>(button);
                            e.gbutton.down = down;
                            step.events.push_back(e);
                        }
                    }
                }
                // a press is held for a moment: the game samples its input once a frame, so a press and its release in
                // the same instant would never be seen
                ScriptStep release{at + 0.12, {}};
                std::vector<SDL_Event> downs;
                for (const SDL_Event& e : step.events) {
                    const bool up = e.type == SDL_EVENT_KEY_UP || e.type == SDL_EVENT_MOUSE_BUTTON_UP || e.type == SDL_EVENT_GAMEPAD_BUTTON_UP;
                    (up ? release.events : downs).push_back(e);
                }
                step.events = std::move(downs);
                script.push_back(std::move(step));
                if (!release.events.empty()) script.push_back(std::move(release));
            }
            std::fclose(f);
        }
        std::stable_sort(script.begin(), script.end(), [](const ScriptStep& a, const ScriptStep& b) { return a.at < b.at; });
    }

    // The moments the player marks (F8, or Share + R3 on a controller): a numbered line in the log with the clock, to find a stutter later
    auto mark_until = std::chrono::steady_clock::now();
    bool pad_back_held = false, pad_rs_held = false;
    const auto do_mark = [&](const char* how) {
        const std::string text = mh4n::display_mem::Mark(how);
        std::fprintf(stderr, "mh4u-win: %s (%s)\n", text.c_str(), how);
        SDL_SetWindowTitle(window, ("Caravan4URecomp - " + text).c_str());
        mark_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    };
    // The HID pads (hid_pads.h) the shell uses: not while an XInput pad is connected nor when SDL already has a gamepad of the same
    // vendor and product. Each one is announced to the mapper as an SDL gamepad event, and its changes are pushed as button and axis events.
    std::map<int, hidpads::Snapshot> hid_sent;
    std::vector<std::pair<std::uint16_t, std::uint16_t>> sdl_pads;
    bool hid_blocked = false;
    auto hid_check_at = std::chrono::steady_clock::now();
    const auto push_pad_event = [](SDL_EventType type, int id, int index, int value) {
        SDL_Event e;
        SDL_zero(e);
        e.type = type;
        if (type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
            e.gbutton.which = static_cast<SDL_JoystickID>(id);
            e.gbutton.button = static_cast<Uint8>(index);
            e.gbutton.down = type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        } else if (type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
            e.gaxis.which = static_cast<SDL_JoystickID>(id);
            e.gaxis.axis = static_cast<Uint8>(index);
            e.gaxis.value = static_cast<Sint16>(value);
        } else {
            e.gdevice.which = static_cast<SDL_JoystickID>(id);
        }
        SDL_PushEvent(&e);
    };
    const auto pump_hid = [&] {
        if (!hid_on) return;
        const auto now = std::chrono::steady_clock::now();
        if (now >= hid_check_at) {
            hid_check_at = now + std::chrono::seconds(1);
            hid_blocked = hidpads::XInputConnected();
            sdl_pads.clear();
            int count = 0;
            if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
                for (int i = 0; i < count; ++i) sdl_pads.emplace_back(SDL_GetGamepadVendorForID(ids[i]), SDL_GetGamepadProductForID(ids[i]));
                SDL_free(ids);
            }
        }
        std::vector<hidpads::Snapshot> use;
        for (hidpads::Snapshot& snap : hidpads::Get()) {
            const bool seen_by_sdl = std::find(sdl_pads.begin(), sdl_pads.end(), std::make_pair(snap.vendor, snap.product)) != sdl_pads.end();
            if (!hid_blocked && !seen_by_sdl) use.push_back(std::move(snap));
        }
        bool changed = false;
        for (auto it = hid_sent.begin(); it != hid_sent.end();) {  // a pad that is gone or no longer used
            if (std::none_of(use.begin(), use.end(), [&](const hidpads::Snapshot& s) { return s.id == it->first; })) {
                std::fprintf(stderr, "mh4u-win: HID pad %04x:%04x no longer used\n", it->second.vendor, it->second.product);
                for (int b = 0; b < 15; ++b) {
                    if (it->second.buttons & (1u << b)) push_pad_event(SDL_EVENT_GAMEPAD_BUTTON_UP, it->first, b, 0);
                }
                push_pad_event(SDL_EVENT_GAMEPAD_REMOVED, it->first, 0, 0);
                it = hid_sent.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
        std::vector<int> added;
        for (const hidpads::Snapshot& snap : use) {
            if (hid_sent.count(snap.id)) continue;
            std::fprintf(stderr, "mh4u-win: using the HID pad %s (%04x:%04x): SDL does not give it to the game\n", snap.name.c_str(), snap.vendor,
                         snap.product);
            hidpads::Snapshot blank = snap;
            blank.buttons = 0;
            std::fill(blank.axes, blank.axes + hidpads::kAxisCount, std::int16_t{0});
            hid_sent[snap.id] = blank;
            added.push_back(snap.id);
            changed = true;
        }
        if (changed) {  // the list first, then the events: the mapper reads the list when it gets them
            pads->active.clear();
            for (const auto& entry : hid_sent) pads->active.push_back(entry.second);
            for (int id : added) push_pad_event(SDL_EVENT_GAMEPAD_ADDED, id, 0, 0);
        }
        for (const hidpads::Snapshot& snap : use) {
            hidpads::Snapshot& sent = hid_sent[snap.id];
            for (int b = 0; b < 15; ++b) {
                const bool now_held = snap.buttons & (1u << b), was = sent.buttons & (1u << b);
                if (now_held != was) push_pad_event(now_held ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP, snap.id, b, 0);
            }
            for (int a = 0; a < hidpads::kAxisCount; ++a) {
                if (std::abs(snap.axes[a] - sent.axes[a]) >= 256 || (snap.axes[a] == 0) != (sent.axes[a] == 0)) {
                    push_pad_event(SDL_EVENT_GAMEPAD_AXIS_MOTION, snap.id, a, snap.axes[a]);
                    sent.axes[a] = snap.axes[a];
                }
            }
            sent.buttons = snap.buttons;
        }
    };

    while (running) {
        pump_hid();
        while (script_next < script.size() && elapsed() >= script[script_next].at) {
            for (SDL_Event e : script[script_next].events) SDL_PushEvent(&e);
            ++script_next;
        }
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            const bool menu_open = menu.open;  // as the event found it
            if (setup_active) setup.OnEvent(event, window);
            else if (event.type == SDL_EVENT_DROP_FILE && game && !user_folder().empty()) {
                // a mod dropped on the window (a folder or a .zip): it is put in <user>\load\mods and the menu shows the Mods page
                const std::string dropped = event.drop.data != nullptr ? event.drop.data : "";
                menu.ShowMods(window, modmgr::Install(user_folder(), std::filesystem::path(WideFromUtf8(dropped))));
            } else if (!typing.active || menu.open) menu.OnEvent(event, window);
            App::InputEvent input;
            if (ToInputEvent(event, input)) {
                // While the game's keyboard is open the physical keyboard types (KeyboardTyping) and its keys press no
                // 3DS button, but a controller keeps working: its D-pad and A move over the game's own on-screen keyboard
                // and press its keys. A key let go always reaches the mapper, so none stays held.
                const bool typing_key = (typing.active || menu_open) && (input.type == App::InputEvent::Type::KeyDown ||
                                                                         (input.type == App::InputEvent::Type::PadButton && input.down));
                const bool not_a_touch = typing_key ? true : mapper.OnEvent(input);
                // the mouse is the stylus unless the keyboard-and-mouse mode makes it the camera: a press on the bottom
                // screen (or a view) of the layout is a touch, kept while it is held
                if (game && !not_a_touch && !menu_open) {
                    const float density = SDL_GetWindowPixelDensity(window);
                    const auto px = [&](int v) { return static_cast<std::uint32_t>(std::max(0.0f, v * density)); };
                    const sl::Layout current = (*presenter)->CurrentLayout();
                    if (input.type == App::InputEvent::Type::MouseButton && input.button == App::MouseButton::Left) {
                        if (input.down) {
                            touching = tracker.Press(current, px(input.x), px(input.y));
                        } else {
                            tracker.Release();
                            touching = false;
                        }
                    } else if (input.type == App::InputEvent::Type::MouseMotion && touching) {
                        tracker.Move(current, px(input.x), px(input.y));
                    } else if (input.type == App::InputEvent::Type::FocusLost) {
                        tracker.Release();
                        touching = false;
                    }
                    if (input.type == App::InputEvent::Type::MouseButton || input.type == App::InputEvent::Type::MouseMotion ||
                        input.type == App::InputEvent::Type::FocusLost) {
                        const std::lock_guard<std::recursive_mutex> rt_guard(g_runtime_mutex);
                        if (mh4n::Runtime* rt = g_runtime.load()) {
                            if (auto live = mh4n::input_live::Find(*rt)) {
                                const auto point = tracker.Point();
                                live->SetTouch(point ? std::optional<mh4n::input_live::TouchPoint>(mh4n::input_live::TouchPoint{
                                                           static_cast<mh4n::u16>(point->x), static_cast<mh4n::u16>(point->y)})
                                                     : std::nullopt);
                            }
                        }
                    }
                }
            }
            if (game && skip_pad_kind != 0 && !typing.active && !menu_open) {  // the controller's skip button: the game still gets it too
                bool now = g_skip_pad_held;
                bool seen = false;
                if (skip_pad_kind == 1 && (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP) &&
                    event.gbutton.button == skip_pad_index) {
                    now = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
                    seen = true;
                } else if (skip_pad_kind == 2 && event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && event.gaxis.axis == skip_pad_index) {
                    now = skip_pad_positive ? event.gaxis.value > 16000 : event.gaxis.value < -16000;
                    seen = true;
                }
                if (seen && now != g_skip_pad_held) {
                    g_skip_pad_held = now;
                    std::fprintf(stderr, "mh4u-win: skip pad %s\n", now ? "down" : "up");
                    SendInput(g_last_state);
                }
            }
            if (game && skip_key != SDLK_UNKNOWN && (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) &&
                event.key.key == skip_key && !event.key.repeat && !typing.active && !menu_open) {
                g_skip_held = event.type == SDL_EVENT_KEY_DOWN;
                std::fprintf(stderr, "mh4u-win: skip key %s\n", g_skip_held ? "down" : "up");
                SendInput(g_last_state);
            }
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && !typing.active &&
                (event.key.key == SDLK_F11 || (event.key.key == SDLK_RETURN && (event.key.mod & SDL_KMOD_ALT)))) {
                fullscreen = !fullscreen;  // borderless fullscreen on the display the window is on
                SDL_SetWindowFullscreen(window, fullscreen);
                ini.Set("fullscreen", fullscreen ? "on" : "off");
                ini.Save();
            }
            if (game && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && !typing.active && !menu_open && event.key.key == SDLK_F3) {
                mouse_camera = !mouse_camera;  // the mouse: stylus <-> camera
                apply_mouse_mode();
                ini.Set("mouse_camera", mouse_camera ? "on" : "off");
                ini.Save();
            }
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && !typing.active && !menu_open && event.key.key == SDLK_F2 &&
                g_popup.load()) {
                touch_forced = !touch_forced;  // show or let go of the touch popup by hand
            }
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.key == SDLK_F8) do_mark("F8");
            if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {  // Share (Back) + R3
                const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
                if (event.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) pad_back_held = down;
                if (event.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) pad_rs_held = down;
                if (down && pad_back_held && pad_rs_held &&
                    (event.gbutton.button == SDL_GAMEPAD_BUTTON_BACK || event.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_STICK)) {
                    do_mark("Share+R3");
                }
            }
            typing.OnEvent(event, window);
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                running = false;
            } else if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                size_changed_at = std::chrono::steady_clock::now();
                size_dirty = true;
                (*presenter)->Resize(static_cast<std::uint32_t>(event.window.data1),
                                     static_cast<std::uint32_t>(event.window.data2));
            } else if (event.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED && ini.Get("fps") == "display") {
                // fps = display follows the window to the screen it is on (a 144 Hz cap on a 165 Hz screen is uneven)
                const std::lock_guard<std::recursive_mutex> rt_guard(g_runtime_mutex);
                if (mh4n::Runtime* rt = g_runtime.load()) {
                    const std::string hz = std::to_string(DisplayHz(window));
                    rt->GetKernel().Post([hz] { Port::Hooks::SetOption("fps", hz); });
                }
            }
        }
        if (setup_active) {
            if (menu_overlay.GuiFailed()) {  // no look to show it with: the console version asks and shows the progress
                setup.Stop();
                menu_overlay.SetSetup(false, {});
                if (RunSetup(ini.exedir, setup.Base(), setup.UpdateFile())) finish_setup(); else running = false;
            } else {
                if (setup.Update()) {
                    menu_overlay.SetSetup(true, setup.MakeView());
                    (*presenter)->RequestRedraw();
                }
                if (setup.QuitRequested()) running = false;
                else if (setup.Finished()) finish_setup();
            }
        }
        if (game) typing.Update(window, mapper);
        {   // the camera takes the mouse (relative mode) only while the game has it: not under the menu or the game's keyboard
            const bool want = game && mouse_camera && !menu.open && !typing.active && !setup_active && mapper.WantsRelativeMouse();
            if (want != SDL_GetWindowRelativeMouseMode(window)) SDL_SetWindowRelativeMouseMode(window, want);
        }
        if (menu.open != menu_was_open) {  // while the menu is up the game reads no button, stick or touch
            menu_was_open = menu.open;
            mapper.SetMuted(menu.open);
            if (menu.open) {
                tracker.Release();
                touching = false;
                PadTouch(false, 0.0f, 0.0f);
                g_skip_held = false;
                g_skip_pad_held = false;
            }
        }
        if (size_dirty && std::chrono::steady_clock::now() - size_changed_at > std::chrono::seconds(1)) {
            size_dirty = false;
            if (!fullscreen && !(SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED)) {
                int w = 0, h = 0;
                SDL_GetWindowSize(window, &w, &h);
                if (w >= 320 && h >= 240) {
                    ini.Set("width", std::to_string(w));
                    ini.Set("height", std::to_string(h));
                    ini.Save();
                }
            }
        }
        if (menu.Update()) (*presenter)->RequestRedraw();
        if (g_popup.load()) {
            // The touch screen shows as a popup while the game waits for something on it: its own keyboard is open, or
            // (auto) the game's state says the touch screen is the way in; F2 forces it. It stays a moment after the need
            // is gone (so that it doesn't flicker) and while the mouse holds it.
            const auto now = std::chrono::steady_clock::now();
            bool need = touch_show == "always" || touch_forced || touching || typing.active;
            if (touch_show == "auto" && game) {
                const Port::Hooks::GameState gs = Port::Hooks::CurrentGameState();
                need = need || TouchWanted(gs);
                // connected to a room in a town (the Gathering Hall included): the touch screen stays up, it is how the
                // hall is used (chat, gestures, guild card, friends). ini online_touch = off turns this off.
                if (ini.Get("online_touch") != "off" && gs.place == Port::Hooks::GameState::PLACE_TOWN) {
                    if (now - room_checked_at > std::chrono::milliseconds(500)) {
                        room_checked_at = now;
                        const auto room_state = mh4n::rooms::GetStatus().state;
                        in_room = room_state == mh4n::rooms::State::Joined || room_state == mh4n::rooms::State::Moderator;
                    }
                    need = need || in_room;
                }
            }
            if (touch_show == "hidden" && !touch_forced && !typing.active) need = false;
            // MODIFIED: the game's state flickers for a few frames while it changes screens (the touch screen showed for an instant):
            // the popup comes up only once the need has lasted 300 ms, unless the player asks for it (mouse, F2, "always", the keyboard).
            const bool asked = touch_show == "always" || touch_forced || touching || typing.active;
            if (!need) {
                touch_need_since = {};
            } else if (touch_need_since == std::chrono::steady_clock::time_point{}) {
                touch_need_since = now;
            }
            const bool lasting = need && (asked || g_touch_visible.load() || now - touch_need_since >= std::chrono::milliseconds(300));
            if (lasting) touch_needed_at = now;
            const bool show = lasting || (now - touch_needed_at < std::chrono::milliseconds(1200) && g_touch_visible.load());
            if (show != g_touch_visible.load()) {
                g_touch_visible.store(show);
                (*presenter)->RequestRedraw();
                if (log_state) std::fprintf(stderr, "mh4u-win: touch popup %s\n", show ? "shown" : "hidden");
            }
        }
        if (log_state && game) {
            const Port::Hooks::GameState gs = Port::Hooks::CurrentGameState();
            const int objects = Port::Hooks::TouchScreenObjects() > 0 ? 1 : 0;
            if (gs.place != last_state.place || gs.town_panel != last_state.town_panel || gs.menu != last_state.menu ||
                gs.front_end != last_state.front_end || objects != last_objects) {
                std::fprintf(stderr, "mh4u-win: state place=%d town_panel=%d menu=%d front_end=%d touch_objects=%d\n", gs.place,
                             gs.town_panel, gs.menu, gs.front_end, objects);
                last_state = gs;
                last_objects = objects;
            }
        }
        if (show_fps && std::chrono::steady_clock::now() - counter_at >= std::chrono::seconds(1)) {
            const sc::Stats counter_stats = (*presenter)->GetStats();
            const double counter_dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - counter_at).count();
            (void)counter_stats;
            const std::uint64_t game_frames = Port::Hooks::GameFrameCount();  // MODIFIED: the game's own frames (a 30 fps cap shows 30; the screen refreshes 60)
            g_cursor.fps.store(static_cast<int>(std::lround(static_cast<double>(game_frames - counter_published) / counter_dt)));
            counter_at = std::chrono::steady_clock::now();
            counter_published = game_frames;
            (*presenter)->RequestRedraw();  // the counter changed while the game may show nothing new
        }
        if (game && !typing.active && std::chrono::steady_clock::now() >= rate_at + std::chrono::seconds(1) &&
            std::chrono::steady_clock::now() >= mark_until) {
            const sc::Stats now_stats = (*presenter)->GetStats();
            const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - rate_at).count();
            char title[128];
            std::snprintf(title, sizeof title, "Caravan4URecomp (modified 4u-native) - game %.0f fps, shown %.0f fps",
                          static_cast<double>(now_stats.published - rate_published) / dt,
                          static_cast<double>(now_stats.presented - rate_presented) / dt);
            SDL_SetWindowTitle(window, title);
            rate_at = std::chrono::steady_clock::now();
            rate_published = now_stats.published;
            rate_presented = now_stats.presented;
        }
        mapper.Tick(elapsed());
        if (game) {
            if (g_game_done.load()) running = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        } else {
            (*presenter)->Publish(TestFrame(sequence++));
            std::this_thread::sleep_for(std::chrono::milliseconds(16));  // about 60 frames a second
        }
        if (seconds > 0 && elapsed() >= seconds) {
            running = false;
        }
    }
    if (game_thread.joinable()) {
        {
            const std::lock_guard<std::recursive_mutex> rt_guard(g_runtime_mutex);
            if (mh4n::Runtime* rt = g_runtime.load()) {
                rt->RequestExit(0, "the window is closing");
            }
        }  // (released before the join: the game thread takes the same lock to let the runtime go)
        game_thread.join();
        pr::SetSharedBackend({});
        g_presenter.store(nullptr);
    }

    const sc::Stats stats = (*presenter)->GetStats();
    std::printf("mh4u-win: published %llu, presented %llu, dropped %llu, swapchains %u, out-of-date %u, "
                "present mode %d, extent %ux%u\n",
                static_cast<unsigned long long>(stats.published), static_cast<unsigned long long>(stats.presented),
                static_cast<unsigned long long>(stats.dropped), stats.swapchain_creations, stats.out_of_date,
                stats.present_mode, stats.extent_width, stats.extent_height);
    (*presenter)->SetUiLayer(nullptr);
    g_cursor.menu = nullptr;
    g_cursor.presenter.store(nullptr);
    (*presenter)->Stop();
    menu_overlay.ResetGui();  // the menu's renderer waits for the device and lets its Vulkan objects go
    presenter->reset();
    rhi::sync::ReleaseRetired(**backend);  // the images of the last frames still hold the device (WP89 M1)
    SDL_Vulkan_DestroySurface(rhi::NativeInstance(**instance), vk_surface, nullptr);
    backend->reset();
    if (!ini.path.empty()) {
        ini.Set("fullscreen", fullscreen ? "on" : "off");
        if (!fullscreen && !(SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED)) {
            int w = 0, h = 0;
            SDL_GetWindowSize(window, &w, &h);
            if (w >= 320 && h >= 240) {
                ini.Set("width", std::to_string(w));
                ini.Set("height", std::to_string(h));
            }
        }
        ini.Save();
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
