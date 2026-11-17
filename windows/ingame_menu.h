// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by
// khorne-hub (https://github.com/khorne-hub/4u-native); see ADDITIONAL-TERMS and THIRD_PARTY_NOTICES.
//
// The in-game settings menu of the Windows shell (F1, or L3 + R3 on a controller). It is drawn over the game
// with the presenter's UI layer, with raw clears only (vkCmdClearAttachments: no pipeline, no shader, no ImGui), in
// a 5 x 7 font made of squares. It has the graphics, game, audio and online settings, a browser of the public rooms
// of a lobby (Azahar's web API) and the hosting and joining of network rooms (mh4n/rooms.h) while the game runs.
//
// The menu knows nothing of the shell: win_main.cpp gives it a `Host` that answers "what is this setting now" and
// "apply this setting" (which also saves it to the settings file).
#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

#include "gpu/backend/vulkan_access.h"
#include "mh4n/rooms.h"

namespace ingame {

// ---- the font and the drawing ----------------------------------------------------------------------------------------

// 5 columns x 7 rows, '1' is a lit square; lower case is drawn as upper case; anything unknown is a question mark.
inline const char* Glyph(char c) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    switch (c) {
    case 'A': return "01110" "10001" "10001" "11111" "10001" "10001" "10001";
    case 'B': return "11110" "10001" "10001" "11110" "10001" "10001" "11110";
    case 'C': return "01110" "10001" "10000" "10000" "10000" "10001" "01110";
    case 'D': return "11110" "10001" "10001" "10001" "10001" "10001" "11110";
    case 'E': return "11111" "10000" "10000" "11110" "10000" "10000" "11111";
    case 'F': return "11111" "10000" "10000" "11110" "10000" "10000" "10000";
    case 'G': return "01110" "10001" "10000" "10111" "10001" "10001" "01111";
    case 'H': return "10001" "10001" "10001" "11111" "10001" "10001" "10001";
    case 'I': return "01110" "00100" "00100" "00100" "00100" "00100" "01110";
    case 'J': return "00111" "00010" "00010" "00010" "00010" "10010" "01100";
    case 'K': return "10001" "10010" "10100" "11000" "10100" "10010" "10001";
    case 'L': return "10000" "10000" "10000" "10000" "10000" "10000" "11111";
    case 'M': return "10001" "11011" "10101" "10101" "10001" "10001" "10001";
    case 'N': return "10001" "11001" "10101" "10011" "10001" "10001" "10001";
    case 'O': return "01110" "10001" "10001" "10001" "10001" "10001" "01110";
    case 'P': return "11110" "10001" "10001" "11110" "10000" "10000" "10000";
    case 'Q': return "01110" "10001" "10001" "10001" "10101" "10010" "01101";
    case 'R': return "11110" "10001" "10001" "11110" "10100" "10010" "10001";
    case 'S': return "01111" "10000" "10000" "01110" "00001" "00001" "11110";
    case 'T': return "11111" "00100" "00100" "00100" "00100" "00100" "00100";
    case 'U': return "10001" "10001" "10001" "10001" "10001" "10001" "01110";
    case 'V': return "10001" "10001" "10001" "10001" "10001" "01010" "00100";
    case 'W': return "10001" "10001" "10001" "10101" "10101" "11011" "10001";
    case 'X': return "10001" "10001" "01010" "00100" "01010" "10001" "10001";
    case 'Y': return "10001" "10001" "01010" "00100" "00100" "00100" "00100";
    case 'Z': return "11111" "00001" "00010" "00100" "01000" "10000" "11111";
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
    case '.': return "00000" "00000" "00000" "00000" "00000" "01100" "01100";
    case ',': return "00000" "00000" "00000" "00000" "01100" "00100" "01000";
    case ':': return "00000" "01100" "01100" "00000" "01100" "01100" "00000";
    case ';': return "00000" "01100" "01100" "00000" "01100" "00100" "01000";
    case '-': return "00000" "00000" "00000" "11111" "00000" "00000" "00000";
    case '_': return "00000" "00000" "00000" "00000" "00000" "00000" "11111";
    case '/': return "00001" "00001" "00010" "00100" "01000" "10000" "10000";
    case '\\': return "10000" "10000" "01000" "00100" "00010" "00001" "00001";
    case '(': return "00010" "00100" "01000" "01000" "01000" "00100" "00010";
    case ')': return "01000" "00100" "00010" "00010" "00010" "00100" "01000";
    case '[': return "01110" "01000" "01000" "01000" "01000" "01000" "01110";
    case ']': return "01110" "00010" "00010" "00010" "00010" "00010" "01110";
    case '+': return "00000" "00100" "00100" "11111" "00100" "00100" "00000";
    case '*': return "00000" "10101" "01110" "11111" "01110" "10101" "00000";
    case '%': return "11001" "11010" "00010" "00100" "01000" "01011" "10011";
    case '!': return "00100" "00100" "00100" "00100" "00100" "00000" "00100";
    case '\'': return "00100" "00100" "01000" "00000" "00000" "00000" "00000";
    case '"': return "01010" "01010" "00000" "00000" "00000" "00000" "00000";
    case '=': return "00000" "00000" "11111" "00000" "11111" "00000" "00000";
    case '<': return "00010" "00100" "01000" "10000" "01000" "00100" "00010";
    case '>': return "01000" "00100" "00010" "00001" "00010" "00100" "01000";
    case '#': return "01010" "01010" "11111" "01010" "11111" "01010" "01010";
    case '@': return "01110" "10001" "10111" "10101" "10111" "10000" "01110";
    case '&': return "01100" "10010" "10100" "01000" "10101" "10010" "01101";
    case '|': return "00100" "00100" "00100" "00100" "00100" "00100" "00100";
    case ' ': return nullptr;
    default: return "01110" "10001" "00001" "00010" "00100" "00000" "00100";  // ?
    }
}

// One line of the panel. Colours: 0 text, 1 title, 2 dim, 3 good, 4 bad.
struct Line {
    std::string text;
    int color = 0;
    bool selected = false;
    bool operator==(const Line& o) const { return text == o.text && color == o.color && selected == o.selected; }
};

constexpr int kCols = 64;      // characters across the panel
constexpr int kMaxRows = 22;    // lines of the panel

// The panel as the present thread draws it: the main thread hands over the lines, the present thread reads them.
class Overlay {
public:
    void Set(bool open, std::vector<Line> lines) {
        std::lock_guard lock(mutex_);
        open_ = open;
        lines_ = std::move(lines);
    }

    // Where the panel is in a target of this size (the drawing and the mouse use the same numbers).
    struct Geometry {
        int unit, pad, panel_w, panel_h, x0, y0;
    };
    static Geometry GeometryOf(std::uint32_t width, std::uint32_t height, std::size_t line_count) {
        Geometry g;
        g.unit = std::max(2, static_cast<int>(height) / 260);
        while (g.unit > 1 && (kCols * 6 + 6) * g.unit > static_cast<int>(width)) --g.unit;
        g.pad = 3 * g.unit;
        g.panel_w = kCols * 6 * g.unit + 2 * g.pad;
        g.panel_h = static_cast<int>(line_count) * 10 * g.unit + 2 * g.pad;
        g.x0 = std::max(0, (static_cast<int>(width) - g.panel_w) / 2);
        g.y0 = std::max(0, (static_cast<int>(height) - g.panel_h) / 2);
        return g;
    }

    void Draw(VkCommandBuffer commands, std::uint32_t width, std::uint32_t height) const {
        std::vector<Line> lines;
        {
            std::lock_guard lock(mutex_);
            if (!open_) return;
            lines = lines_;
        }
        struct Sq {
            int x, y, w, h;
        };
        const Geometry geometry = GeometryOf(width, height, lines.size());
        const int unit = geometry.unit, pad = geometry.pad, panel_w = geometry.panel_w, panel_h = geometry.panel_h;
        const int x0 = geometry.x0, y0 = geometry.y0;

        std::vector<Sq> border = {{x0 - unit, y0 - unit, panel_w + 2 * unit, panel_h + 2 * unit}};
        std::vector<Sq> fill = {{x0, y0, panel_w, panel_h}};
        std::vector<Sq> bars;
        std::vector<Sq> text[5];
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const int ty = y0 + pad + static_cast<int>(i) * 10 * unit;
            if (lines[i].selected) bars.push_back({x0 + unit, ty - unit, panel_w - 2 * unit, 9 * unit});
            int pen = x0 + pad;
            int shown = 0;
            for (const char c : lines[i].text) {
                if (shown++ >= kCols) break;
                if (const char* glyph = Glyph(c)) {
                    for (int row = 0; row < 7; ++row) {  // a lit run of a row is one rectangle
                        int col = 0;
                        while (col < 5) {
                            if (glyph[row * 5 + col] != '1') {
                                ++col;
                                continue;
                            }
                            int end = col;
                            while (end < 5 && glyph[row * 5 + end] == '1') ++end;
                            text[std::clamp(lines[i].color, 0, 4)].push_back({pen + col * unit, ty + row * unit, (end - col) * unit, unit});
                            col = end;
                        }
                    }
                }
                pen += 6 * unit;
            }
        }
        const auto fill_rects = [&](const std::vector<Sq>& squares, float r, float g, float b) {
            std::vector<VkClearRect> rects;
            for (const Sq& q : squares) {
                const int xa = std::max(0, q.x), ya = std::max(0, q.y);
                const int xb = std::min<int>(static_cast<int>(width), q.x + q.w), yb = std::min<int>(static_cast<int>(height), q.y + q.h);
                if (xb <= xa || yb <= ya) continue;
                VkClearRect rect = {};
                rect.rect = {{xa, ya}, {static_cast<std::uint32_t>(xb - xa), static_cast<std::uint32_t>(yb - ya)}};
                rect.layerCount = 1;
                rects.push_back(rect);
            }
            if (rects.empty()) return;
            VkClearAttachment attachment = {};
            attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            attachment.colorAttachment = 0;
            attachment.clearValue.color = {{r, g, b, 1.0f}};
            vkCmdClearAttachments(commands, 1, &attachment, static_cast<std::uint32_t>(rects.size()), rects.data());
        };
        fill_rects(border, 0.75f, 0.78f, 0.90f);
        fill_rects(fill, 0.04f, 0.06f, 0.14f);
        fill_rects(bars, 0.16f, 0.30f, 0.70f);
        fill_rects(text[0], 0.95f, 0.95f, 0.95f);
        fill_rects(text[1], 1.00f, 0.85f, 0.25f);
        fill_rects(text[2], 0.55f, 0.58f, 0.66f);
        fill_rects(text[3], 0.45f, 1.00f, 0.50f);
        fill_rects(text[4], 1.00f, 0.45f, 0.45f);
    }

private:
    mutable std::mutex mutex_;
    bool open_ = false;
    std::vector<Line> lines_;
};

// ---- the menu --------------------------------------------------------------------------------------------------------

// What the shell gives the menu. `value` answers with a setting's text now; `apply` changes it for the game that is
// running and saves it to the settings file.
struct Host {
    std::function<std::string(const std::string&)> value;
    std::function<void(const std::string&, const std::string&)> apply;
    std::function<void()> quit;
    /// A one-off action by name ("open_textures"); answers with the line the menu shows.
    std::function<std::string(const std::string&)> action;
};

enum class Page { Main, Graphics, Game, Audio, Online, Rooms, Password, Count };

struct Item {
    enum Kind { Info, Choice, Action, Text, Link };
    Kind kind = Info;
    std::string id;  // what keeps the selection on the same item when the rows around it change
    std::string label, value;
    int color = 0;
    std::function<void(int)> change;  // Choice: left (-1) and right (+1)
    std::function<void()> act;        // Choice, Action, Link: Enter or A
    std::string* target = nullptr;    // Text: the string typed into
    std::size_t max_length = 0;
    bool secret = false;
    std::function<void()> done;       // Text: after the text is accepted
    bool Selectable() const { return kind != Info; }
};

// A text with its bytes outside ASCII (the font has none) made safe to show: a UTF-8 character becomes one '?'.
inline std::string Plain(const std::string& in) {
    std::string out;
    for (std::size_t i = 0; i < in.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c < 0x80) {
            out.push_back(c < 0x20 ? ' ' : static_cast<char>(c));
        } else if (c >= 0xC0) {
            out.push_back('?');
        }  // (continuation bytes belong to the '?')
    }
    return out;
}

class Menu {
public:
    Menu(Host host, Overlay& overlay) : host_(std::move(host)), overlay_(overlay) {
        nick_ = host_.value("nick");
        addr_ = host_.value("join_address");
        host_port_ = host_.value("host_port");
        if (host_port_.empty()) host_port_ = "24872";
    }

    bool open = false;  // main thread

    bool Editing() const { return edit_ != nullptr; }

    void Toggle(SDL_Window* window) {
        open = !open;
        if (!open) EndEdit(window, false);
        dirty_ = true;
    }

    // Every SDL event, on the main thread. The menu opens and closes with F1 or L3 + R3 (both stick clicks). While it is open:
    //  - keyboard: arrows, Enter, Escape, Backspace, and the text of a field;
    //  - controller: D-pad or left stick (they repeat while held), A, B, the shoulder buttons to change a value, Start
    //    to close; a text field is typed with the D-pad (up / down changes the last letter, right adds one, left
    //    deletes) and A accepts, B cancels;
    //  - mouse: moving over a row selects it, a click does what Enter does (a click on a field starts typing), the right
    //    button goes back, the wheel moves.
    void OnEvent(const SDL_Event& e, SDL_Window* window) {
        window_ = window;
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F1) {
            Toggle(window);
            return;
        }
        if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {  // L3 + R3 together
            const bool down = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
            if (e.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_STICK) l3_ = down;
            if (e.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) r3_ = down;
            if (down && l3_ && r3_ && (e.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_STICK || e.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_STICK)) {
                Toggle(window);
                return;
            }
        }
        if (e.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
            if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP || e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) dpad_move_ = 0;
            if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_LEFT || e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) dpad_change_ = 0;
            return;
        }
        if (!open) return;
        if (e.type == SDL_EVENT_TEXT_INPUT && edit_ != nullptr) {
            for (const char* p = e.text.text; *p; ++p) {
                if (edit_->size() < edit_max_ && static_cast<unsigned char>(*p) >= 0x20 && static_cast<unsigned char>(*p) < 0x7F) edit_->push_back(*p);
            }
            dirty_ = true;
            return;
        }
        Act act;
        switch (e.type) {
        case SDL_EVENT_KEY_DOWN:
            switch (e.key.key) {
            case SDLK_UP: act.move = -1; break;
            case SDLK_DOWN: act.move = 1; break;
            case SDLK_LEFT: act.change = -1; break;
            case SDLK_RIGHT: act.change = 1; break;
            case SDLK_PAGEUP: act.move = -5; break;
            case SDLK_PAGEDOWN: act.move = 5; break;
            case SDLK_RETURN:
            case SDLK_KP_ENTER: act.enter = !e.key.repeat; break;
            case SDLK_ESCAPE: act.back = !e.key.repeat; break;
            case SDLK_BACKSPACE: act.erase = true; break;
            default: return;
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            act.pad = true;
            switch (e.gbutton.button) {
            case SDL_GAMEPAD_BUTTON_DPAD_UP: act.move = dpad_move_ = -1; break;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN: act.move = dpad_move_ = 1; break;
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT: act.change = dpad_change_ = -1; break;
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: act.change = dpad_change_ = 1; break;
            case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: act.change = -1; act.shoulder = true; break;
            case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: act.change = 1; act.shoulder = true; break;
            case SDL_GAMEPAD_BUTTON_SOUTH: act.enter = true; break;
            case SDL_GAMEPAD_BUTTON_EAST: act.back = true; break;
            case SDL_GAMEPAD_BUTTON_START: Toggle(window); return;
            default: return;
            }
            if (act.move != 0 || act.change != 0) next_repeat_ms_ = SDL_GetTicks() + 380;
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: {  // the left stick, like the D-pad (it repeats while held)
            if (e.gaxis.axis != SDL_GAMEPAD_AXIS_LEFTX && e.gaxis.axis != SDL_GAMEPAD_AXIS_LEFTY) return;
            const int dir = e.gaxis.value > 16000 ? 1 : (e.gaxis.value < -16000 ? -1 : 0);
            int& held = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY ? stick_move_ : stick_change_;
            if (dir == held) return;
            held = dir;
            if (dir == 0) return;
            act.pad = true;
            (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY ? act.move : act.change) = dir;
            next_repeat_ms_ = SDL_GetTicks() + 380;
            break;
        }
        case SDL_EVENT_MOUSE_MOTION:
            act.row = RowAt(window, e.motion.x, e.motion.y);
            if (act.row < 0) return;
            act.hover = true;
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (e.button.button == SDL_BUTTON_RIGHT || e.button.button == SDL_BUTTON_X1) {
                act.back = true;
            } else if (e.button.button == SDL_BUTTON_LEFT) {
                act.row = RowAt(window, e.button.x, e.button.y);
                if (edit_ != nullptr) EndEdit(window, true);
                if (act.row < 0) return;
                act.enter = true;
            } else {
                return;
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (e.wheel.integer_y == 0) return;
            act.move = e.wheel.integer_y > 0 ? -1 : 1;
            break;
        default:
            return;
        }
        Do(act, window);
    }

    // Once per loop, on the main thread. True when the picture of the menu changed.
    bool Update() {
        const auto now = std::chrono::steady_clock::now();
        if (open && window_ != nullptr) {  // D-pad and stick held: repeat
            const int move = dpad_move_ != 0 ? dpad_move_ : stick_move_;
            const int change = dpad_change_ != 0 ? dpad_change_ : stick_change_;
            const Uint64 ticks = SDL_GetTicks();
            if ((move != 0 || change != 0) && ticks >= next_repeat_ms_) {
                Act act;
                act.pad = true;
                act.move = move;
                act.change = change;
                next_repeat_ms_ = ticks + 90;
                Do(act, window_);
            }
        }
        if (!dirty_ && now < next_refresh_) return false;
        next_refresh_ = now + std::chrono::milliseconds(300);
        dirty_ = false;
        std::vector<Line> lines = open ? Render() : std::vector<Line>{};
        if (open == shown_open_ && lines == shown_) return false;
        shown_open_ = open;
        shown_ = lines;
        overlay_.Set(open, std::move(lines));
        return true;
    }

private:
    struct Act {
        int move = 0, change = 0, row = -1;
        bool enter = false, back = false, erase = false, pad = false, shoulder = false, hover = false;
    };

    // The row of the panel under a point of the window (in window units), as an index of the lines; -1: none.
    int RowAt(SDL_Window* window, float x, float y) const {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        const float density = SDL_GetWindowPixelDensity(window);
        const Overlay::Geometry g = Overlay::GeometryOf(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), line_count_);
        const int px = static_cast<int>(x * density), py = static_cast<int>(y * density);
        if (px < g.x0 || px >= g.x0 + g.panel_w) return -1;
        const int row = (py - g.y0 - g.pad + g.unit) / (10 * g.unit);
        if (py < g.y0 + g.pad - g.unit || row < 0 || row >= static_cast<int>(line_count_)) return -1;
        return row;
    }

    void Do(const Act& a, SDL_Window* window) {
        dirty_ = true;
        if (edit_ != nullptr) {  // typing into a field
            static const std::string kLetters = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .:-_";
            if (a.pad) {
                if (a.move != 0 && !edit_->empty()) {  // the last letter, one step through the list
                    const std::size_t at = kLetters.find(edit_->back());
                    const int n = static_cast<int>(kLetters.size());
                    const int next = at == std::string::npos ? 0 : (static_cast<int>(at) - a.move + n) % n;
                    edit_->back() = kLetters[static_cast<std::size_t>(next)];
                } else if (a.change > 0 && edit_->size() < edit_max_) {
                    edit_->push_back(edit_->empty() || std::isdigit(static_cast<unsigned char>(edit_->back())) ? '1' : 'a');
                } else if (a.change < 0 && !edit_->empty()) {
                    edit_->pop_back();
                } else if (a.move != 0 && edit_->empty() && edit_->size() < edit_max_) {
                    edit_->push_back('a');
                }
            } else if (a.erase && !edit_->empty()) {
                edit_->pop_back();
            }
            if (a.enter) EndEdit(window, true);
            if (a.back) EndEdit(window, false);
            return;
        }
        std::vector<Item> items = Build();
        int& sel = sel_[static_cast<int>(page_)];
        SyncSelection(items);
        const auto clamp_to_item = [&] {
            sel = std::clamp(sel, 0, std::max(0, static_cast<int>(items.size()) - 1));
            if (!items.empty()) sel_id_[static_cast<int>(page_)] = items[static_cast<std::size_t>(sel)].id;
        };
        if (a.row >= 0) {  // the mouse: the row under it
            const int index = a.row < static_cast<int>(row_item_.size()) ? row_item_[static_cast<std::size_t>(a.row)] : -1;
            if (index < 0 || index >= static_cast<int>(items.size()) || !items[static_cast<std::size_t>(index)].Selectable()) return;
            sel = index;
            if (a.hover) {
                clamp_to_item();
                return;
            }
        }
        if (a.move != 0 && !items.empty()) {
            const int dir = a.move < 0 ? -1 : 1;
            for (int steps = 0; steps < std::abs(a.move); ++steps) {
                for (std::size_t tries = 0; tries < items.size(); ++tries) {
                    sel = (sel + dir + static_cast<int>(items.size())) % static_cast<int>(items.size());
                    if (items[static_cast<std::size_t>(sel)].Selectable()) break;
                }
            }
        }
        clamp_to_item();
        if (!items.empty()) {
            Item& item = items[static_cast<std::size_t>(sel)];
            if (a.change != 0 && item.change) item.change(a.change);
            if (a.enter) {
                if (item.kind == Item::Text) {
                    BeginEdit(window, item);
                } else if (item.act) {
                    item.act();
                }
            }
        }
        if (a.back) {
            if (page_ == Page::Main) {
                open = false;
            } else {
                page_ = (page_ == Page::Rooms || page_ == Page::Password) ? Page::Online : Page::Main;
            }
        }
    }

    SDL_Window* window_ = nullptr;
    bool l3_ = false, r3_ = false;  // the stick clicks held: both open and close the menu
    int dpad_move_ = 0, dpad_change_ = 0, stick_move_ = 0, stick_change_ = 0;
    Uint64 next_repeat_ms_ = 0;
    std::size_t line_count_ = 0;
    std::vector<int> row_item_;  // the item each line of the panel shows (-1: none)

    Host host_;
    Overlay& overlay_;
    Page page_ = Page::Main;
    int sel_[static_cast<int>(Page::Count)] = {};
    std::string sel_id_[static_cast<int>(Page::Count)];
    bool dirty_ = true;
    std::chrono::steady_clock::time_point next_refresh_{};
    bool shown_open_ = false;
    std::vector<Line> shown_;

    // online
    std::string nick_, addr_, password_, host_port_, host_password_, join_password_;
    std::string message_;
    std::chrono::steady_clock::time_point message_until_{};
    mh4n::rooms::LobbyRoom pending_;

    struct Fetch {
        std::mutex mutex;
        bool busy = false, fetched = false;
        mh4n::rooms::LobbyList list;
    };
    std::shared_ptr<Fetch> fetch_ = std::make_shared<Fetch>();

    // typing
    std::string* edit_ = nullptr;
    std::string edit_backup_;
    std::size_t edit_max_ = 0;
    std::function<void()> edit_done_;

    // Settings saved that only the next start takes: the menu lists them until the game is closed.
    std::vector<std::string> restart_;
    void NeedRestart(const std::string& what) {
        if (std::find(restart_.begin(), restart_.end(), what) == restart_.end()) restart_.push_back(what);
        Say("SAVED. THIS CHANGE APPLIES THE NEXT TIME YOU START THE GAME");
    }

    void Say(const std::string& text) {
        message_ = text;
        message_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        dirty_ = true;
    }

    void BeginEdit(SDL_Window* window, const Item& item) {
        if (item.target == nullptr) return;
        edit_ = item.target;
        edit_backup_ = *edit_;
        edit_max_ = item.max_length;
        edit_done_ = item.done;
        SDL_StartTextInput(window);
    }
    void EndEdit(SDL_Window* window, bool accept) {
        if (edit_ == nullptr) return;
        if (!accept) *edit_ = edit_backup_;
        std::function<void()> done = std::move(edit_done_);
        edit_ = nullptr;
        edit_done_ = nullptr;
        SDL_StopTextInput(window);
        if (accept && done) done();
    }

    // ---- items -----------------------------------------------------------------------------------------------------------
    Item Choice(const std::string& label, const std::string& key, std::vector<std::string> options, bool restart = false,
                bool restart_to_default = false) {
        Item item;
        item.kind = Item::Choice;
        item.id = key;
        item.label = label + (restart ? " (RESTART)" : "");
        std::string now = host_.value(key);
        std::size_t index = 0;
        bool found = false;
        for (std::size_t i = 0; i < options.size(); ++i) {
            if (options[i] == now) {
                index = i;
                found = true;
            }
        }
        item.value = found ? options[index] : (now.empty() ? options[0] : now);
        const std::string name = label;
        auto step = [this, key, options, index, found, restart, restart_to_default, name](int dir) {
            const int n = static_cast<int>(options.size());
            const int next = (static_cast<int>(found ? index : 0) + dir + n) % n;
            host_.apply(key, options[static_cast<std::size_t>(next)]);
            // a change that the running game can't take is saved and said to wait: nothing restarts by itself
            if (restart || (restart_to_default && options[static_cast<std::size_t>(next)] == "default")) NeedRestart(name);
            dirty_ = true;
        };
        item.change = step;
        item.act = [step] { step(1); };
        return item;
    }
    static Item Info(const std::string& text, int color = 2) {
        Item item;
        item.label = text;
        item.color = color;
        return item;
    }
    static Item Action(const std::string& label, std::function<void()> act) {
        Item item;
        item.kind = Item::Action;
        item.id = label;
        item.label = label;
        item.act = std::move(act);
        return item;
    }
    static Item TextField(const std::string& label, std::string* target, std::size_t max_length, bool secret = false,
                          std::function<void()> done = nullptr) {
        Item item;
        item.kind = Item::Text;
        item.id = label;
        item.label = label;
        item.target = target;
        item.max_length = max_length;
        item.secret = secret;
        item.done = std::move(done);
        return item;
    }

    static std::vector<std::string> Numbers(int from, int to, int step) {
        std::vector<std::string> v;
        for (int n = from; n <= to; n += step) v.push_back(std::to_string(n));
        return v;
    }

    std::vector<Item> Build() {
        std::vector<Item> items;
        switch (page_) {
        case Page::Main: {
            const auto link = [&](const char* label, Page to) {
                Item i;
                i.kind = Item::Link;
                i.label = std::string(label) + " >";
                i.id = i.label;
                i.act = [this, to] { page_ = to; };
                items.push_back(i);
            };
            link("Graphics", Page::Graphics);
            link("Game", Page::Game);
            link("Audio", Page::Audio);
            link("Online", Page::Online);
            items.push_back(Info(""));
            items.push_back(Action("Resume the game", [this] { open = false; }));
            items.push_back(Action("Quit the game", [this] { host_.quit(); }));
            break;
        }
        case Page::Graphics:
            items.push_back(Choice("Fullscreen", "fullscreen", {"on", "off"}));
            items.push_back(Choice("VSync", "vsync", {"on", "off"}));
            items.push_back(Choice("Screen layout", "mode", {"popup", "stacked", "overlay", "single", "side"}));
            if (host_.value("mode") == "popup") {  // the top screen full, the touch screen over it when it is wanted
                items.push_back(Choice("Touch screen shows", "touch_show", {"auto", "always", "hidden"}));
                items.push_back(Choice("Touch popup size %", "popup_size", Numbers(20, 70, 5)));
                items.push_back(Choice("Touch popup corner", "popup_corner", {"bottom-right", "bottom-left", "top-right", "top-left"}));
                items.push_back(Info("F2 SHOWS OR HIDES THE TOUCH SCREEN BY HAND"));
            }
            items.push_back(Choice("Internal resolution", "scale", {"auto", "1", "2", "3", "4", "5", "6", "8", "10"}, true));
            items.push_back(Choice("FPS counter", "fps_counter", {"off", "on"}));
            items.push_back(Action("Open the HD textures folder", [this] {
                const std::string said = host_.action ? host_.action("open_textures") : "NOT AVAILABLE";
                NeedRestart("HD textures");
                Say(said + ". RESTART THE GAME TO LOAD THEM");
            }));
            items.push_back(Info("PACKS (AZAHAR/CITRA FORMAT) LOAD WHEN THE GAME STARTS"));
            break;
        case Page::Game:
            items.push_back(Choice("Frame rate limit", "fps", {"default", "display", "30", "60", "90", "120", "144", "165", "180", "240", "unlimited"}));
            items.push_back(Choice("Steady frame rate", "steady", {"default", "on", "off"}, false, true));
            items.push_back(Choice("Particle effects", "effects", {"default", "on", "off"}, false, true));
            items.push_back(Info("DEFAULT = THE GAME'S OWN SETTING   DISPLAY = YOUR SCREEN'S RATE"));
            items.push_back(Info("UNLIMITED: ALL THE CPU. VSYNC ON: SCREEN RATE IS THE MAX"));
            break;
        case Page::Audio:
            items.push_back(Choice("Volume", "volume", Numbers(0, 100, 5)));
            break;
        case Page::Online:
            BuildOnline(items);
            break;
        case Page::Rooms:
            BuildRooms(items);
            break;
        case Page::Password: {
            items.push_back(Info("ROOM: " + Plain(pending_.name), 1));
            items.push_back(TextField("Password", &join_password_, 32, true));
            items.push_back(Action("Join", [this] {
                JoinRoom(pending_.address, pending_.port, join_password_);
                join_password_.clear();
                page_ = Page::Online;
            }));
            break;
        }
        default: break;
        }
        return items;
    }

    static std::string StatusName(const mh4n::rooms::Status& st) {
        using mh4n::rooms::State;
        return st.state == State::Idle ? "NOT IN A ROOM" : (st.state == State::Joining ? "JOINING" : "IN A ROOM");
    }

    void BuildOnline(std::vector<Item>& items) {
        using mh4n::rooms::State;
        const mh4n::rooms::Status st = mh4n::rooms::GetStatus();
        if (st.state == State::Joined || st.state == State::Moderator) {
            items.push_back(Info("IN ROOM: " + Plain(st.room.name) + " (" + std::to_string(st.members.size()) + " MEMBERS)", 3));
            std::string who;
            for (const auto& m : st.members) who += (who.empty() ? "" : ", ") + Plain(m.nickname);
            items.push_back(Info("  " + who));
        } else if (st.state == State::Joining) {
            items.push_back(Info("JOINING " + Plain(st.host) + (st.waiting_for_console ? " (WAITING FOR THE GAME)" : "..."), 1));
        } else if (st.error != mh4n::rooms::Error::None) {
            items.push_back(Info(std::string("NOT IN A ROOM: ") + mh4n::rooms::ErrorMessage(st.error), 4));
        } else {
            items.push_back(Info("NOT IN A ROOM"));
        }
        if (st.hosting) items.push_back(Info("HOSTING ON UDP PORT " + std::to_string(st.hosted_port) + " (OPEN IT ON YOUR ROUTER)", 3));
        items.push_back(TextField("Nickname", &nick_, 20, false, [this] { host_.apply("nick", nick_); }));
        {
            Item i;
            i.kind = Item::Link;
            i.label = "Public rooms >";
            i.id = i.label;
            i.act = [this] {
                page_ = Page::Rooms;
                StartFetch(false);
            };
            items.push_back(i);
        }
        items.push_back(TextField("Address HOST:PORT", &addr_, 64, false, [this] { host_.apply("join_address", addr_); }));
        items.push_back(TextField("Room password", &password_, 32, true));
        items.push_back(Action("Join this address", [this] {
            std::string host = addr_;
            unsigned port = mh4n::rooms::kDefaultPort;
            const auto colon = host.rfind(':');
            if (colon != std::string::npos) {
                port = static_cast<unsigned>(std::atoi(host.c_str() + colon + 1));
                host.resize(colon);
            }
            if (host.empty() || port == 0 || port > 65535) {
                Say("TYPE THE ADDRESS AS HOST:PORT");
                return;
            }
            JoinRoom(host, static_cast<std::uint16_t>(port), password_);
        }));
        items.push_back(TextField("Host port (UDP)", &host_port_, 5, false, [this] { host_.apply("host_port", host_port_); }));
        items.push_back(TextField("Host password", &host_password_, 32, true));
        items.push_back(Action(st.hosting ? "Close the room I host" : "Host a room", [this, hosting = st.hosting] {
            if (hosting) {
                mh4n::rooms::Leave();
                mh4n::rooms::CloseRoom();
                Say("ROOM CLOSED");
                return;
            }
            const int port = std::atoi(host_port_.c_str());
            if (port <= 0 || port > 65535 || !mh4n::rooms::ValidNickname(nick_)) {
                Say(mh4n::rooms::ValidNickname(nick_) ? "THE PORT MUST BE 1 TO 65535" : "NICKNAME: 4 TO 20 LETTERS, DIGITS, SPACE . _ -");
                return;
            }
            mh4n::rooms::HostRequest room;
            room.port = static_cast<std::uint16_t>(port);
            room.name = "Caravan4URecomp room";
            room.slots = 16;
            room.password = host_password_;
            room.host_username = nick_;
            std::string error;
            if (!mh4n::rooms::Host(room, &error)) {
                Say(error);
                return;
            }
            JoinRoom("127.0.0.1", room.port, host_password_);
            Say("HOSTING: OTHERS JOIN YOUR PUBLIC IP, PORT " + std::to_string(port));
        }));
        items.push_back(Action("Leave the room", [this] {
            mh4n::rooms::Leave();
            Say("LEFT THE ROOM");
        }));
        for (Item& i : items) {
            if (i.label == "Host a room" || i.label == "Close the room I host") i.id = "host";
        }
        items.push_back(Info("THEN, IN THE GAME: GATHERING HALL > LOCAL PLAY"));
    }

    void StartFetch(bool force) {
        std::shared_ptr<Fetch> fetch = fetch_;
        {
            std::lock_guard lock(fetch->mutex);
            if (fetch->busy || (fetch->fetched && !force)) return;
            fetch->busy = true;
        }
        std::string url = host_.value("lobby");
        std::thread([fetch, url] {
            mh4n::rooms::LobbyList list = mh4n::rooms::FetchLobby(url, mh4n::rooms::kMh4uUsa, 10000);
            std::lock_guard lock(fetch->mutex);
            fetch->list = std::move(list);
            fetch->busy = false;
            fetch->fetched = true;
        }).detach();
    }

    void BuildRooms(std::vector<Item>& items) {
        std::shared_ptr<Fetch> fetch = fetch_;
        std::lock_guard lock(fetch->mutex);
        Item refresh = Action(fetch->busy ? "Refreshing..." : "Refresh the list", [this] { StartFetch(true); });
        refresh.id = "refresh";
        items.push_back(refresh);
        if (fetch->busy && !fetch->fetched) {
            items.push_back(Info("LOADING THE LIST OF ROOMS...", 1));
            return;
        }
        if (!fetch->list.ok) {
            items.push_back(Info(Plain(fetch->list.error), 4));
            return;
        }
        items.push_back(Info("* = SET UP FOR MH4U   (P) = PASSWORD   YOU NEED MH4U USA 1.3.0"));
        for (const mh4n::rooms::LobbyRoom& room : fetch->list.rooms) {
            Item i;
            i.kind = Item::Action;
            i.label = std::string(room.preferred ? "*" : " ") + (room.password ? "(P) " : "    ") +
                      std::to_string(room.players.size()) + "/" + std::to_string(room.max_players) + " " + Plain(room.name);
            i.color = room.preferred ? 0 : 2;
            i.id = room.address + ":" + std::to_string(room.port) + room.name;
            i.act = [this, room] {
                if (room.password) {
                    pending_ = room;
                    join_password_.clear();
                    page_ = Page::Password;
                } else {
                    JoinRoom(room.address, room.port, "");
                    page_ = Page::Online;
                }
            };
            items.push_back(i);
        }
    }

    void JoinRoom(const std::string& host, std::uint16_t port, const std::string& password) {
        if (!mh4n::rooms::ValidNickname(nick_)) {
            Say("NICKNAME: 4 TO 20 LETTERS, DIGITS, SPACE . _ -");
            return;
        }
        mh4n::rooms::JoinRequest request;
        request.host = host;
        request.port = port;
        request.nickname = nick_;
        request.password = password;
        std::string error;
        if (!mh4n::rooms::Join(request, &error)) {
            Say(error);
        } else {
            Say("JOINING " + host + ":" + std::to_string(port) + "...");
        }
    }

    // The selection follows its item by id when the rows change (a status line appears above it), else it is clamped to a
    // row that can be chosen.
    void SyncSelection(const std::vector<Item>& items) {
        int& sel = sel_[static_cast<int>(page_)];
        std::string& id = sel_id_[static_cast<int>(page_)];
        if (!id.empty()) {
            for (std::size_t i = 0; i < items.size(); ++i) {
                if (items[i].id == id && items[i].Selectable()) {
                    sel = static_cast<int>(i);
                    return;
                }
            }
        }
        sel = std::clamp(sel, 0, std::max(0, static_cast<int>(items.size()) - 1));
        if (!items.empty() && !items[static_cast<std::size_t>(sel)].Selectable()) {  // the first thing that can be chosen
            for (std::size_t i = 0; i < items.size(); ++i) {
                if (items[i].Selectable()) {
                    sel = static_cast<int>(i);
                    break;
                }
            }
        }
        if (!items.empty()) id = items[static_cast<std::size_t>(sel)].id;
    }

    // ---- the picture -----------------------------------------------------------------------------------------------------
    std::vector<Line> Render() {
        static const char* const kTitles[] = {"SETTINGS", "GRAPHICS", "GAME", "AUDIO", "ONLINE", "PUBLIC ROOMS", "ROOM PASSWORD"};
        std::vector<Item> items = Build();
        SyncSelection(items);
        const int sel = sel_[static_cast<int>(page_)];
        std::vector<Line> lines;
        row_item_.assign(2, -1);
        lines.push_back({std::string("CARAVAN4URECOMP - ") + kTitles[static_cast<int>(page_)], 1, false});
        lines.push_back({"", 0, false});
        const int rows = kMaxRows - 6;  // title, gap, gap, restart note, message, hint
        const int n = static_cast<int>(items.size());
        const int first = std::clamp(sel - rows / 2, 0, std::max(0, n - rows));
        for (int i = first; i < std::min(n, first + rows); ++i) {
            const Item& item = items[static_cast<std::size_t>(i)];
            const bool selected = i == sel;
            std::string text;
            if (item.kind == Item::Choice) {
                std::string label = item.label;
                label.resize(std::max<std::size_t>(label.size() + 1, 30), ' ');
                text = label + (selected ? "< " + item.value + " >" : item.value);
            } else if (item.kind == Item::Text) {
                std::string label = item.label;
                label.resize(std::max<std::size_t>(label.size(), 22), ' ');
                std::string shown = *item.target;
                if (item.secret) shown.assign(shown.size(), '*');
                if (edit_ == item.target && selected) shown += "_";
                text = label + shown;
            } else {
                text = item.label;
            }
            lines.push_back({Plain(text), selected ? 0 : item.color, selected});
            row_item_.push_back(i);
        }
        row_item_.resize(lines.size() + 4, -1);
        lines.push_back({"", 0, false});
        if (!restart_.empty()) {
            std::string what;
            for (const std::string& r : restart_) what += (what.empty() ? "" : ", ") + r;
            lines.push_back({"PENDING A RESTART OF THE GAME: " + Plain(what), 1, false});
        }
        const auto now = std::chrono::steady_clock::now();
        lines.push_back({now < message_until_ ? Plain(message_) : "", 1, false});
        lines.push_back({edit_ != nullptr ? "ENTER/A: OK  ESC/B: CANCEL  PAD: UP/DN=LETTER R=ADD L=DEL"
                                          : "ARROWS/DPAD/MOUSE: MOVE  ENTER/A/CLICK: OK  ESC/B/RCLICK: BACK",
                         2, false});
        line_count_ = lines.size();
        return lines;
    }
};

}  // namespace ingame
