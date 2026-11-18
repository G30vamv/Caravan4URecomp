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
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

#include "gpu/backend/vulkan_access.h"
#include "addr_parse.h"
#include "menu_gui.h"
#include "menu_text.h"
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
    // The first start's screen, drawn instead of the menu while it is open.
    void SetSetup(bool open, SetupView view) {
        std::lock_guard lock(mutex_);
        setup_open_ = open;
        setup_ = std::move(view);
    }
    bool GuiFailed() const { return gui_failed_.load(); }

    void Set(bool open, std::vector<Line> lines, View view) {
        std::lock_guard lock(mutex_);
        open_ = open;
        lines_ = std::move(lines);
        view_ = std::move(view);
    }

    // The look of the menu (menu_gui.h): made on the present thread when the first frame needs it. If it can't be made,
    // the menu is drawn with plain rectangles and the 5 x 7 font below.
    void UseGui(std::function<std::unique_ptr<GuiRenderer>()> make) { make_gui_ = std::move(make); }
    // On the present thread or after it stopped (the renderer waits for the device).
    void ResetGui() {
        gui_.reset();
        make_gui_ = nullptr;
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

    void Draw(VkCommandBuffer commands, void* recorder, std::uint32_t width, std::uint32_t height) const {
        std::vector<Line> lines;
        View view;
        SetupView setup;
        bool setup_open = false;
        {
            std::lock_guard lock(mutex_);
            setup_open = setup_open_;
            if (setup_open) setup = setup_;
            else if (!open_) return;
            lines = lines_;
            view = view_;
        }
        if (make_gui_ && !gui_failed_.load()) {
            if (!gui_) gui_ = make_gui_();
            if (!gui_) gui_failed_ = true;
        }
        if (setup_open) {  // (the first start without the look is the console's, which the shell falls back to)
            if (gui_) gui_->DrawSetup(recorder, setup, width, height);
            return;
        }
        if (gui_ && gui_->Draw(recorder, view, width, height)) return;
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
    View view_;
    std::function<std::unique_ptr<GuiRenderer>()> make_gui_;
    mutable std::unique_ptr<GuiRenderer> gui_;
    mutable std::atomic<bool> gui_failed_{false};
    bool setup_open_ = false;
    SetupView setup_;
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

// The pages: five tabs, and two that belong to the Online tab (the public rooms and a room's password).
enum class Page { Graphics, Game, Controls, Audio, Online, Saves, Mods, System, Rooms, Password, Count };
constexpr int kTabCount = 8;
inline int TabOf(Page page) { return page == Page::Rooms || page == Page::Password ? static_cast<int>(Page::Online) : static_cast<int>(page); }
inline const char* const kTabNames[kTabCount] = {"Graphics", "Game", "Controls", "Audio", "Online", "Saves", "Mods", "System"};

// The 3DS buttons the controls page lists, in the order of the page (the shell's settings are key_<lower case name> and
// pad_<lower case name>).
inline const char* const kBindNames[] = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "Start", "Select", "Up", "Down", "Left", "Right"};
constexpr int kBindCount = 14;
inline std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

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
    std::string help;                 // what the row says about itself when it is marked
    std::string cap;                  // Action: the word on its button
    bool toggle = false, on = false;  // Choice that is an on / off switch
    bool slider = false;              // Choice shown as a slider (numbers)
    float frac = 0.0f;
    bool room = false;                // a row of the list of public rooms
    bool locked = false;              // ... with a password
    bool restart = false;             // the change waits for a restart
    std::function<void(float)> set_frac;  // a slider set by the mouse
    std::function<void()> erase;      // Backspace or Delete on the row (a backup to delete)
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

    // The shell tells the menu something (a mod that was dropped on the window): shown on the menu's message line.
    void Notify(const std::string& text) { Say(text); }

    // Opens the menu on the Mods page with a line (a mod was dropped on the window).
    void ShowMods(SDL_Window* window, const std::string& text) {
        window_ = window;
        open = true;
        SetPage(Page::Mods);
        if (text.rfind("Installed", 0) == 0) NeedRestart("Mods");
        Say(text);
    }

    void Toggle(SDL_Window* window) {
        open = !open;
        if (!open) EndEdit(window, false);
        dirty_ = true;
    }

    // Every SDL event, on the main thread. The menu opens and closes with F1 or L3 + R3 (both stick clicks). While it is open:
    //  - keyboard: arrows, Enter, Escape, Backspace, Q and E (or Page Up and Down in a list) for the pages, and the text of
    //    a field;
    //  - controller: D-pad or left stick (they repeat while held), A, B, LB and RB for the pages, Start to close; a text field
    //    is typed with the D-pad (up / down changes the last letter, right adds one, left deletes) and A accepts, B cancels;
    //  - mouse: moving over a row selects it, a click does what Enter does (on an arrow, a step; on a slider, it sets the
    //    value; on a field, it starts typing), a click on a tab changes the page, the right button goes back, the wheel moves.
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
        if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
            drag_ = -1;
            return;
        }
        if (!open) return;
        if (capture_ >= 0 && Capture(e)) return;
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
            case SDLK_Q: act.tab_step = -1; break;
            case SDLK_E: act.tab_step = 1; break;
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
            case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: act.tab_step = -1; break;
            case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: act.tab_step = 1; break;
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
        case SDL_EVENT_MOUSE_MOTION: {
            const Hit hit = HitTest(window, e.motion.x, e.motion.y);
            const int tab = hit.kind == Hit::Tab ? hit.index : -1;
            const int row = hit.kind == Hit::Row ? hit.index : -1;
            if (tab != hover_tab_ || row != hover_row_) {
                hover_tab_ = tab;
                hover_row_ = row;
                dirty_ = true;
            }
            if (drag_ >= 0 && (e.motion.state & SDL_BUTTON_LMASK)) {  // a slider being dragged
                act.hit = drag_;
                act.slider = SliderFraction(window, e.motion.x, drag_);
            } else if (hit.kind == Hit::Row) {
                act.hit = hit.index;
                act.hover = true;
            } else {
                return;
            }
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (e.button.button == SDL_BUTTON_RIGHT || e.button.button == SDL_BUTTON_X1) {
                act.back = true;
            } else if (e.button.button == SDL_BUTTON_LEFT) {
                const Hit hit = HitTest(window, e.button.x, e.button.y);
                if (edit_ != nullptr) EndEdit(window, true);
                if (hit.kind == Hit::Tab) {
                    act.tab = hit.index;
                } else if (hit.kind == Hit::Row) {
                    act.hit = hit.index;
                    act.zone = hit.zone;
                    if (hit.frac >= 0.0f) {
                        act.slider = hit.frac;
                        drag_ = hit.index;
                    } else {
                        act.enter = true;
                    }
                } else {
                    return;
                }
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
        std::vector<Line> lines;
        View view;
        if (open) Render(&lines, &view);
        if (open == shown_open_ && lines == shown_ && view == shown_view_) return false;
        shown_open_ = open;
        shown_ = lines;
        shown_view_ = view;
        overlay_.Set(open, std::move(lines), std::move(view));
        return true;
    }

private:
    struct Act {
        int move = 0, change = 0;
        int tab = -1, tab_step = 0;    // a page chosen by name (the mouse) or stepped to (LB, RB, Q, E)
        int hit = -1, zone = 0;        // an item under the mouse, and its arrow (-1 left, 1 right)
        float slider = -1.0f;          // a slider set by the mouse
        bool enter = false, back = false, erase = false, pad = false, hover = false;
    };

    // What is under a point of the window (in window units): a tab, or a row (as an index of the page's items).
    struct Hit {
        enum Kind { None, Tab, Row } kind = None;
        int index = -1;
        int zone = 0;
        float frac = -1.0f;  // a press on a slider's bar
    };

    // The pixel size of the window and the layout the drawing uses in it.
    Layout CurrentLayout(SDL_Window* window, float* density) const {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        *density = SDL_GetWindowPixelDensity(window);
        return LayoutOf(static_cast<float>(w), static_cast<float>(h));
    }

    Hit HitTest(SDL_Window* window, float x, float y) const {
        Hit hit;
        float density = 1.0f;
        const Layout l = CurrentLayout(window, &density);
        const float px = x * density, py = y * density;
        if (l.tabs.Contains(px, py) && !view_.tabs.empty()) {
            hit.kind = Hit::Tab;
            hit.index = std::clamp(static_cast<int>((px - l.tabs.x0) / (l.tabs.W() / static_cast<float>(view_.tabs.size()))), 0,
                                   static_cast<int>(view_.tabs.size()) - 1);
            return hit;
        }
        if (!l.rows.Contains(px, py)) return hit;
        int first = 0, fit = 1;
        WindowOf(l, view_.rows.size(), view_.sel, &first, &fit);
        const int slot = static_cast<int>((py - l.rows.y0) / l.row_h);
        const int index = first + slot;
        if (slot < 0 || slot >= fit || index < 0 || index >= static_cast<int>(view_.rows.size())) return hit;
        const ViewRow& row = view_.rows[static_cast<std::size_t>(index)];
        if (row.kind == ViewRow::Info) return hit;
        hit.kind = Hit::Row;
        hit.index = index;
        const float u = l.u;
        const float value_left = l.rows.x0 + l.rows.W() * 0.50f, value_right = l.rows.x1 - 18.0f * u;
        if (row.kind == ViewRow::Choice) {
            if (std::fabs(px - (value_left + 12 * u)) < 22 * u) hit.zone = -1;
            else if (std::fabs(px - (value_right - 12 * u)) < 22 * u) hit.zone = 1;
        } else if (row.kind == ViewRow::Slider) {
            const float bar_x0 = value_left, bar_x1 = value_right - 74.0f * u - 12.0f * u;
            if (px >= bar_x0 - 10 * u && px <= bar_x1 + 10 * u) hit.frac = std::clamp((px - bar_x0) / std::max(1.0f, bar_x1 - bar_x0), 0.0f, 1.0f);
        }
        return hit;
    }

    float SliderFraction(SDL_Window* window, float x, int row) const {
        float density = 1.0f;
        const Layout l = CurrentLayout(window, &density);
        (void)row;
        const float u = l.u;
        const float bar_x0 = l.rows.x0 + l.rows.W() * 0.50f, bar_x1 = l.rows.x1 - 18.0f * u - 74.0f * u - 12.0f * u;
        return std::clamp((x * density - bar_x0) / std::max(1.0f, bar_x1 - bar_x0), 0.0f, 1.0f);
    }

    void SetPage(Page page) {
        page_ = page;
        hover_row_ = -1;
        dirty_ = true;
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
        if (a.tab >= 0) {  // a tab clicked
            SetPage(static_cast<Page>(std::clamp(a.tab, 0, kTabCount - 1)));
            return;
        }
        if (a.tab_step != 0) {  // LB, RB, Q, E
            SetPage(static_cast<Page>((TabOf(page_) + a.tab_step + kTabCount) % kTabCount));
            return;
        }
        std::vector<Item> items = Build();
        int& sel = sel_[static_cast<int>(page_)];
        SyncSelection(items);
        const auto clamp_to_item = [&] {
            sel = std::clamp(sel, 0, std::max(0, static_cast<int>(items.size()) - 1));
            if (!items.empty()) sel_id_[static_cast<int>(page_)] = items[static_cast<std::size_t>(sel)].id;
        };
        if (a.hit >= 0) {  // the mouse: the row under it
            if (a.hit >= static_cast<int>(items.size()) || !items[static_cast<std::size_t>(a.hit)].Selectable()) return;
            sel = a.hit;
            clamp_to_item();
            if (a.hover) return;
            Item& item = items[static_cast<std::size_t>(a.hit)];
            if (a.slider >= 0.0f) {
                if (item.set_frac) item.set_frac(a.slider);
                return;
            }
            if (a.zone != 0 && item.change) {
                item.change(a.zone);
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
            if (a.erase && item.erase) item.erase();
            if (a.enter) {
                if (item.kind == Item::Text) {
                    BeginEdit(window, item);
                } else if (item.act) {
                    item.act();
                }
            }
        }
        if (a.back) {
            if (page_ == Page::Rooms || page_ == Page::Password) {
                SetPage(Page::Online);
            } else {
                open = false;
            }
        }
    }

    SDL_Window* window_ = nullptr;
    bool l3_ = false, r3_ = false;  // the stick clicks held: both open and close the menu
    int dpad_move_ = 0, dpad_change_ = 0, stick_move_ = 0, stick_change_ = 0;
    Uint64 next_repeat_ms_ = 0;
    int drag_ = -1;                 // the slider (an item's index) the mouse holds
    int hover_tab_ = -1, hover_row_ = -1;
    View view_;                     // what the last frame of the menu showed (the mouse reads its layout)

    Host host_;
    Overlay& overlay_;
    Page page_ = Page::Graphics;
    int sel_[static_cast<int>(Page::Count)] = {};
    std::string sel_id_[static_cast<int>(Page::Count)];
    bool dirty_ = true;
    std::chrono::steady_clock::time_point next_refresh_{};
    bool shown_open_ = false;
    std::vector<Line> shown_;
    View shown_view_;

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
        Say("Saved. This change applies the next time you start the game.");
    }

    // The language of the menu: the setting, or (auto) the one of Windows.
    void RefreshLanguage() {
        const std::string l = host_.value("language");
        g_spanish = l == "es" || (l != "en" && host_.value("system_language") == "es");
    }

    void Say(const std::string& text) {
        message_ = Tr(text);
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
        item.label = label;
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
        item.toggle = options.size() == 2 && ((options[0] == "on" && options[1] == "off") || (options[0] == "off" && options[1] == "on"));
        item.on = item.value == "on";
        item.slider = key == "volume" || key == "popup_size";
        item.frac = options.size() > 1 ? static_cast<float>(found ? index : 0) / static_cast<float>(options.size() - 1) : 0.0f;
        if (item.slider && options.size() > 1) {  // where the number is between the first and the last, even if it is between steps
            const float from = static_cast<float>(std::atof(options.front().c_str())), to = static_cast<float>(std::atof(options.back().c_str()));
            if (to > from) item.frac = std::clamp((static_cast<float>(std::atof(item.value.c_str())) - from) / (to - from), 0.0f, 1.0f);
        }
        item.restart = restart;
        const std::string name = label;
        auto apply_at = [this, key, options, restart, restart_to_default, name](int next) {
            host_.apply(key, options[static_cast<std::size_t>(next)]);
            // a change that the running game can't take is saved and said to wait: nothing restarts by itself
            if (restart || (restart_to_default && options[static_cast<std::size_t>(next)] == "default")) NeedRestart(name);
            dirty_ = true;
        };
        auto step = [apply_at, options, index, found](int dir) {
            const int n = static_cast<int>(options.size());
            apply_at((static_cast<int>(found ? index : 0) + dir + n) % n);
        };
        item.change = step;
        item.act = [step] { step(1); };
        item.set_frac = [apply_at, options, index, found](float frac) {
            const int n = static_cast<int>(options.size());
            const int next = std::clamp(static_cast<int>(std::lround(frac * static_cast<float>(n - 1))), 0, n - 1);
            if (next != static_cast<int>(found ? index : 0)) apply_at(next);
        };
        return item;
    }
    static Item Info(const std::string& text, int color = 2) {
        Item item;
        item.label = text;
        item.color = color;
        return item;
    }
    static Item Action(const std::string& label, std::function<void()> act, const std::string& cap = "Accept") {
        Item item;
        item.kind = Item::Action;
        item.id = label;
        item.label = label;
        item.cap = cap;
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

    // What a row says about itself when it is marked.
    static std::string HelpFor(const std::string& id) {
        static const std::pair<const char*, const char*> kHelp[] = {
            {"fullscreen", "Borderless fullscreen on the display the window is on. F11 or Alt+Enter does the same."},
            {"vsync", "Shows each frame when your display refreshes. Off lets the game run above your display's rate, and the picture may tear."},
            {"mode", "Popup: the top screen fills the window and the touch screen pops up over it when the game needs it. The others show both screens all the time."},
            {"aspect", "Default keeps the 3DS's own 5:3 shape (black bars at the sides on a wide screen). Stretch fills the whole window. The ratios draw the picture in that shape."},
            {"touch_show", "When the touch screen's popup appears: when the game needs it (auto), always, or never. F2 shows or hides it by hand."},
            {"popup_size", "The height of the touch screen's popup, as a percentage of the window's height."},
            {"popup_corner", "The corner of the window where the touch screen's popup sits."},
            {"scale", "How sharp the 3D scene, the HUD and the text are drawn: a multiple of the 3DS top screen's 400 x 240. Auto picks the one closest to your window."},
            {"fps_counter", "Shows the game's frame rate in the top-left corner."},
            {"Open the HD textures folder", "Opens the folder for HD texture packs (Azahar or Citra format). They are read when the game starts."},
            {"fps", "Caps the game's frame rate. Default is the game's own, Display matches your screen, Unlimited uses all the CPU."},
            {"steady", "Keeps the frame rate even instead of letting it vary. Default leaves the game's own setting."},
            {"effects", "Turns the game's particle effects on or off. Default leaves the game's own setting."},
            {"volume", "The volume of the game."},
            {"language", "The language of this menu. Auto follows Windows."},
            {"Nickname", "Your name in network rooms: 4 to 20 letters, digits, spaces, dots, underscores or dashes."},
            {"Public rooms >", "Lists the rooms of the community lobby (the same rooms Azahar uses). Everyone needs Monster Hunter 4 Ultimate USA 1.3.0."},
            {"Address HOST:PORT", "The address of a room, like 203.0.113.7:24872."},
            {"Room password", "The password of the room you join, if it has one. It is not saved."},
            {"Join this address", "Joins the room at that address. Then, in the game, go to the Gathering Hall and choose Local Play."},
            {"Host port (UDP)", "The UDP port your room listens on. Open it on your router so others can join."},
            {"Host password", "An optional password for the room you host. It is not saved."},
            {"host", "Opens a room on your PC and copies the address to send to your friends: a Radmin VPN, Hamachi, ZeroTier or Tailscale address if you have one (no router to open), else your local network's."},
            {"paste_join", "Joins the room whose address you copied (Ctrl+C on your friend's message)."},
            {"copy_address", "Copies the address to send to your friends again."},
            {"Leave the room", "Leaves the room you are in."},
            {"auto_backup", "Backs up your saves each time the game starts, if they changed since the last backup. The newest 5 automatic backups are kept; yours are never deleted."},
            {"Back up my saves now", "Copies your saves to the backups folder."},
            {"Resume the game", "Closes the menu and goes back to the game."},
            {"Quit the game", "Closes the game."},
            {"refresh", "Gets the list of rooms again."},
            {"Password", "The password of this room."},
            {"Join", "Joins the room with that password."},
        };
        for (const auto& [key, text] : kHelp) {
            if (id == key) return text;
        }
        return {};
    }

    std::vector<Item> Build() {
        RefreshLanguage();
        std::vector<Item> items;
        switch (page_) {
        case Page::Graphics:
            items.push_back(Choice("Fullscreen", "fullscreen", {"on", "off"}));
            items.push_back(Choice("VSync", "vsync", {"on", "off"}));
            items.push_back(Choice("Screen layout", "mode", {"popup", "stacked", "overlay", "single", "side"}));
            if (host_.value("mode") == "popup") {  // the top screen full, the touch screen over it when it is wanted
                items.push_back(Choice("Touch screen shows", "touch_show", {"auto", "always", "hidden"}));
                items.push_back(Choice("Touch popup size", "popup_size", Numbers(20, 70, 5)));
                items.push_back(Choice("Touch popup corner", "popup_corner", {"bottom-right", "bottom-left", "top-right", "top-left"}));
            }
            items.push_back(Choice("Picture shape", "aspect", {"default", "stretch", "16:9", "16:10", "21:9", "4:3"}));
            items.push_back(Choice("Internal resolution", "scale", {"auto", "1", "2", "3", "4", "5", "6", "7", "8"}, true));
            items.push_back(Choice("FPS counter", "fps_counter", {"off", "on"}));
            items.push_back(Action("Open the HD textures folder", [this] {
                const std::string said = host_.action ? host_.action("open_textures") : "Not available";
                NeedRestart("HD textures");
                Say(Tr(said) + Tr(". Restart the game to load the packs."));
            }, "Open"));
            break;
        case Page::Game:
            items.push_back(Choice("Frame rate limit", "fps", {"default", "display", "30", "60", "120", "180", "240", "300", "360", "unlimited"}));
            items.push_back(Choice("Steady frame rate", "steady", {"default", "on", "off"}, false, true));
            items.push_back(Choice("Particle effects", "effects", {"default", "on", "off"}, false, true));
            break;
        case Page::Controls:
            BuildControls(items);
            break;
        case Page::Saves:
            BuildSaves(items);
            break;
        case Page::Mods:
            BuildMods(items);
            break;
        case Page::Audio:
            items.push_back(Choice("Volume", "volume", Numbers(0, 100, 5)));
            break;
        case Page::Online:
            BuildOnline(items);
            break;
        case Page::System:
            items.push_back(Choice("Language", "language", {"auto", "en", "es"}));
            items.push_back(Action("Resume the game", [this] { open = false; }, "Resume"));
            items.push_back(Action("Quit the game", [this] { host_.quit(); }, "Quit"));
            items.push_back(Info("Every change is saved as soon as you make it."));
            items.push_back(Info("Open this menu with F1, or L3 + R3 on a controller."));
            break;
        case Page::Rooms:
            BuildRooms(items);
            break;
        case Page::Password: {
            items.push_back(Info(Tr("Room: ") + Plain(pending_.name), 1));
            items.push_back(TextField("Password", &join_password_, 32, true));
            items.push_back(Action("Join", [this] {
                JoinRoom(pending_.address, pending_.port, join_password_);
                join_password_.clear();
                SetPage(Page::Online);
            }, "Join"));
            break;
        }
        default: break;
        }
        for (Item& i : items) {
            if (i.help.empty()) i.help = HelpFor(i.id);
        }
        return items;
    }

    // ---- the controls page: every 3DS button with its key and its controller button; Enter on a row, then press the key
    // or the controller's button to use (a keyboard key sets the key, a controller's button or trigger sets the pad's). ----
    int capture_ = -1;  // the button (an index of kBindNames) waiting for a key or a controller's button

    void BuildControls(std::vector<Item>& items) {
        items.push_back(Info(capture_ >= 0 ? "Now press a key or a controller button.  Esc or the right mouse button cancels, Backspace puts it back."
                                           : "Enter on a button, then press the key or the controller's button you want for it.",
                             capture_ >= 0 ? 1 : 2));
        for (int i = 0; i < kBindCount; ++i) {
            Item item;
            item.kind = Item::Action;
            item.room = true;  // (drawn as a label and a text on the right)
            item.id = std::string("bind_") + kBindNames[i];
            item.label = std::string(kBindNames[i]) + (i >= 10 ? " (D-pad)" : "");
            item.value = capture_ == i ? Tr("Press a key or a button...") : host_.value("bind:" + std::to_string(i));
            item.color = capture_ == i ? 1 : 0;
            item.act = [this, i] {
                capture_ = i;
                dirty_ = true;
            };
            item.help = Tr("The key and the controller's button of this 3DS button. A keyboard key you press replaces the key, a controller button or trigger replaces the controller's.");
            items.push_back(std::move(item));
        }
        items.push_back(Action("Put every control back as it was", [this] {
            capture_ = -1;
            if (host_.action) Say(host_.action("reset_controls"));
        }, "Reset"));
        items.push_back(Info("The sticks: W A S D and the left stick (circle pad), O L K ; and the right stick (C-stick)."));
        items.push_back(Info("F1 (the menu), F2, F3 and F11 are the program's own keys."));
    }

    // What a host action answers, said in the language: a known sentence, or a sentence that ends with a name.
    void SayHost(const std::string& text) {
        static const char* const kPrefixes[] = {"Backed up as "};
        for (const char* prefix : kPrefixes) {
            if (text.rfind(prefix, 0) == 0) {
                Say(Tr(prefix) + text.substr(std::strlen(prefix)));
                return;
            }
        }
        Say(text);
    }

    // ---- the saves page: backups of the hunter's saves, and putting one back (at the next start) ----
    std::string confirm_;  // the backup (or "delete:" and the backup) waiting for its second Enter

    static std::vector<std::string> SplitLines(const std::string& text, char sep = '\n') {
        std::vector<std::string> out;
        std::size_t from = 0;
        while (from <= text.size()) {
            const std::size_t to = text.find(sep, from);
            const std::string part = text.substr(from, to == std::string::npos ? std::string::npos : to - from);
            if (!part.empty()) out.push_back(part);
            if (to == std::string::npos) break;
            from = to + 1;
        }
        return out;
    }

    void BuildSaves(std::vector<Item>& items) {
        const std::vector<std::string> info = SplitLines(host_.action ? host_.action("save_info") : "", '|');  // files|MB
        if (info.size() >= 2) items.push_back(Info(Tr("Your saves: ") + info[0] + " " + Tr("files") + ", " + info[1] + " MB.", 3));
        else items.push_back(Info("There are no saves yet. They appear when the game saves.", 2));
        const std::string pending = host_.action ? host_.action("save_pending") : "";
        if (!pending.empty()) {
            items.push_back(Info(Tr("A restore is waiting: ") + pending + ". " + Tr("It is done the next time the game starts (the saves now are backed up first)."), 1));
            items.push_back(Action("Cancel the restore", [this] {
                if (host_.action) host_.action("save_unstage");
                Say("The restore was cancelled.");
            }, "Cancel"));
        }
        {
            Item item = Action("Back up my saves now", [this] { if (host_.action) SayHost(host_.action("save_backup")); }, "Back up");
            item.id = "save_backup";
            items.push_back(item);
        }
        items.push_back(Choice("Back up automatically at start", "auto_backup", {"on", "off"}));
        items.push_back(Action("Open the saves folder", [this] { if (host_.action) Say(host_.action("open_saves")); }, "Open"));
        items.push_back(Action("Open the backups folder", [this] { if (host_.action) Say(host_.action("open_backups")); }, "Open"));
        const std::vector<std::string> backups = SplitLines(host_.action ? host_.action("save_list") : "");
        items.push_back(Info(backups.empty() ? "No backups yet."
                                             : "Backups, newest first. Enter restores one; Backspace deletes it.", 2));
        for (const std::string& line : backups) {  // name|stamp|label|MB
            const std::vector<std::string> f = SplitLines(line, '|');
            if (f.size() < 4) continue;
            const std::string name = f[0];
            Item item;
            item.kind = Item::Action;
            item.room = true;
            item.id = "backup_" + name;
            item.label = f[1] + (f[2] == "-" ? "" : "  (" + Tr(f[2]) + ")");
            item.value = f[3] + " MB";
            item.color = confirm_ == name || confirm_ == "delete:" + name ? 1 : 0;
            item.act = [this, name] {
                if (confirm_ == name) {
                    confirm_.clear();
                    Say(host_.action ? host_.action("save_stage:" + name) : "");
                } else {
                    confirm_ = name;
                    Say("Press Enter again to put this backup back the next time the game starts.");
                }
            };
            item.erase = [this, name] {
                if (confirm_ == "delete:" + name) {
                    confirm_.clear();
                    Say(host_.action ? host_.action("save_delete:" + name) : "");
                } else {
                    confirm_ = "delete:" + name;
                    Say("Press Backspace again to delete this backup.");
                }
            };
            item.help = Tr("A copy of your saves. Enter twice puts it back the next time the game starts (the saves now are backed up first). Backspace twice deletes it.");
            items.push_back(std::move(item));
        }
    }

    // ---- the mods page: the folders of <user>\load\mods, on or off, in the order they are applied ----
    void BuildMods(std::vector<Item>& items) {
        const std::vector<std::string> mods = SplitLines(host_.action ? host_.action("mods_list") : "");  // name|flags|on
        items.push_back(Info("Mods change the game's files. A change applies the next time the game starts.", 2));
        if (mods.empty()) {
            items.push_back(Info("No mods yet. Drop a mod's folder or .zip on this window, or open the folder and copy it there.", 1));
        }
        for (std::size_t i = 0; i < mods.size(); ++i) {
            const std::vector<std::string> f = SplitLines(mods[i], '|');
            if (f.size() < 3) continue;
            const std::string name = f[0];
            const bool code = f[1].find('c') != std::string::npos, on = f[2] == "1";
            Item item;
            item.kind = Item::Choice;
            item.id = "mod_" + name;
            item.label = Plain(name) + (code ? " (code)" : "");
            item.toggle = true;
            item.on = on;
            item.value = on ? "on" : "off";
            const auto set = [this, name, code](bool want) {
                if (want && code) {
                    Say("This mod changes the game's code: it needs the game's library built with it, which this program does not do.");
                    return;
                }
                if (host_.action) host_.action(std::string("mods_set:") + name + ":" + (want ? "on" : "off"));
                NeedRestart("Mods");
                dirty_ = true;
            };
            item.change = [set, on](int dir) { set(dir > 0 ? true : dir < 0 ? false : !on); };
            item.act = [set, on] { set(!on); };
            item.help = code ? Tr("A code mod: it changes the game's code, so the game's library would have to be built with it. It can be turned off here, not on.")
                             : Tr("Turns this mod on or off. Where two mods change the same file, the one lower in the list wins.");
            items.push_back(std::move(item));
            if (i > 0) {
                Item up = Action("  " + Plain(name) + Tr(": move up"), [this, name] { if (host_.action) host_.action("mods_move:" + name + ":-1"); NeedRestart("Mods"); }, "Up");
                up.id = "modup_" + name;
                items.push_back(std::move(up));
            }
            if (i + 1 < mods.size()) {
                Item down = Action("  " + Plain(name) + Tr(": move down"), [this, name] { if (host_.action) host_.action("mods_move:" + name + ":1"); NeedRestart("Mods"); }, "Down");
                down.id = "moddown_" + name;
                items.push_back(std::move(down));
            }
        }
        items.push_back(Action("Open the mods folder", [this] { if (host_.action) Say(host_.action("open_mods")); }, "Open"));
    }

    // While a button waits for its key: true when the event was taken.
    bool Capture(const SDL_Event& e) {
        const std::string name = Lower(kBindNames[capture_]);
        const auto finish = [this] {
            capture_ = -1;
            dirty_ = true;
        };
        if (e.type == SDL_EVENT_KEY_DOWN) {
            if (e.key.repeat) return true;
            if (e.key.key == SDLK_ESCAPE) {
                finish();
            } else if (e.key.key == SDLK_BACKSPACE || e.key.key == SDLK_DELETE) {
                host_.apply("key_" + name, "");
                host_.apply("pad_" + name, "");
                Say("Put back as it was.");
                finish();
            } else if (e.key.key != SDLK_F1) {
                const char* key = SDL_GetScancodeName(e.key.scancode);
                if (key != nullptr && *key != ' ') {
                    host_.apply("key_" + name, key);
                    Say(Tr("Saved: ") + key);
                }
                finish();
            } else {
                return false;  // F1 closes the menu
            }
            return true;
        }
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            if (e.button.button == SDL_BUTTON_RIGHT) finish();
            return true;
        }
        if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
            if (e.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) {
                finish();  // (the Guide button cancels: a controller alone can get out)
            } else {
                host_.apply("pad_" + name, "b" + std::to_string(static_cast<int>(e.gbutton.button)));
                Say("Saved.");
                finish();
            }
            return true;
        }
        if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
            if (std::abs(static_cast<int>(e.gaxis.value)) > 26000) {
                host_.apply("pad_" + name, "a" + std::to_string(static_cast<int>(e.gaxis.axis)) + (e.gaxis.value > 0 ? "+" : "-"));
                Say("Saved.");
                finish();
            }
            return true;
        }
        return e.type == SDL_EVENT_GAMEPAD_BUTTON_UP || e.type == SDL_EVENT_TEXT_INPUT;
    }

    void BuildOnline(std::vector<Item>& items) {
        using mh4n::rooms::State;
        const mh4n::rooms::Status st = mh4n::rooms::GetStatus();
        if (st.state == State::Joined || st.state == State::Moderator) {
            items.push_back(Info(Tr("In room: ") + Plain(st.room.name) + " (" + std::to_string(st.members.size()) + " " + Tr("members") + ")", 3));
            std::string who;
            for (const auto& m : st.members) who += (who.empty() ? "" : ", ") + Plain(m.nickname);
            items.push_back(Info("  " + who));
        } else if (st.state == State::Joining) {
            items.push_back(Info(Tr("Joining ") + Plain(st.host) + (st.waiting_for_console ? Tr(" (waiting for the game)") : "..."), 1));
        } else if (st.error != mh4n::rooms::Error::None) {
            items.push_back(Info(Tr("Not in a room: ") + mh4n::rooms::ErrorMessage(st.error), 4));
        } else {
            items.push_back(Info("Not in a room."));
        }
        if (st.hosting) {
            const auto shared = ShareAddresses();
            items.push_back(Info(Tr("Hosting. Send one of these addresses to your friends:"), 3));
            for (const auto& [ip, where, vpn] : shared) {
                (void)vpn;
                items.push_back(Info("  " + ip + ":" + std::to_string(st.hosted_port) + "  (" + Tr(where) + ")", 3));
            }
            if (shared.empty()) items.push_back(Info(Tr("No VPN or local network found: friends need your public IP and the port open on your router."), 4));
            Item copy = Action("Copy the address", [this, port = st.hosted_port] { CopyAddress(port); }, "Copy");
            copy.id = "copy_address";
            items.push_back(copy);
        }
        items.push_back(TextField("Nickname", &nick_, 20, false, [this] { host_.apply("nick", nick_); }));
        {   // the easy way: one click to host (the address to send is copied), one click to join with a pasted address
            Item host = Action(st.hosting ? "Close the room I host" : "Host a room", [this, hosting = st.hosting] {
                if (hosting) {
                    mh4n::rooms::Leave();
                    mh4n::rooms::CloseRoom();
                    Say("Room closed.");
                    return;
                }
                const int port = std::atoi(host_port_.c_str());
                if (port <= 0 || port > 65535 || !mh4n::rooms::ValidNickname(nick_)) {
                    Say(mh4n::rooms::ValidNickname(nick_) ? "The port must be 1 to 65535." : "Nickname: 4 to 20 letters, digits, spaces, dots, underscores or dashes.");
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
                share_at_ = {};  // look at the adapters again
                CopyAddress(static_cast<unsigned>(port));
            }, st.hosting ? "Close" : "Host");
            host.id = "host";
            items.push_back(host);
        }
        {
            Item paste = Action("Join with the copied address", [this] {
                const std::string text = host_.action ? host_.action("paste") : std::string();
                std::string host;
                unsigned port = 0;
                if (!netaddr::ParseHostPort(text, host, port, mh4n::rooms::kDefaultPort)) {
                    Say("Copy the address your friend sent (like 26.1.2.3:24872) and try again.");
                    return;
                }
                addr_ = host + ":" + std::to_string(port);
                host_.apply("join_address", addr_);
                JoinRoom(host, static_cast<std::uint16_t>(port), password_);
            }, "Join");
            paste.id = "paste_join";
            items.push_back(paste);
        }
        {
            Item i = Action("Public rooms >", [this] {
                SetPage(Page::Rooms);
                StartFetch(false);
            }, "Open");
            items.push_back(i);
        }
        // by hand: an address typed, a port, passwords
        items.push_back(TextField("Address HOST:PORT", &addr_, 64, false, [this] { host_.apply("join_address", addr_); }));
        items.push_back(TextField("Room password", &password_, 32, true));
        items.push_back(Action("Join this address", [this] {
            std::string host;
            unsigned port = 0;
            if (!netaddr::ParseHostPort(addr_, host, port, mh4n::rooms::kDefaultPort)) {
                Say("Type the address as HOST:PORT.");
                return;
            }
            JoinRoom(host, static_cast<std::uint16_t>(port), password_);
        }, "Join"));
        items.push_back(TextField("Host port (UDP)", &host_port_, 5, false, [this] { host_.apply("host_port", host_port_); }));
        items.push_back(TextField("Host password", &host_password_, 32, true));
        items.push_back(Action("Leave the room", [this] {
            mh4n::rooms::Leave();
            Say("Left the room.");
        }, "Leave"));
        items.push_back(Info("Then, in the game: Gathering Hall, Local Play."));
    }

    // The addresses a friend can reach this PC on (ip, where, virtual network), asked of the shell and kept a few seconds.
    struct Shared {
        std::string ip, where;
        bool vpn = false;
    };
    std::vector<Shared> shared_;
    std::chrono::steady_clock::time_point share_at_{};
    std::vector<Shared> ShareAddresses() {
        const auto now = std::chrono::steady_clock::now();
        if (share_at_ == std::chrono::steady_clock::time_point{} || now - share_at_ > std::chrono::seconds(4)) {
            share_at_ = now;
            shared_.clear();
            const std::string text = host_.action ? host_.action("addresses") : std::string();
            std::size_t from = 0;
            while (from < text.size()) {
                std::size_t end = text.find('\n', from);
                if (end == std::string::npos) end = text.size();
                const std::string line = text.substr(from, end - from);
                from = end + 1;
                const auto p1 = line.find('|'), p2 = line.rfind('|');
                if (p1 == std::string::npos || p2 == p1) continue;
                shared_.push_back({line.substr(0, p1), line.substr(p1 + 1, p2 - p1 - 1), line.substr(p2 + 1) == "1"});
            }
        }
        return shared_;
    }

    // Copies the best address (a virtual network's, else the local network's) with the port, and says so.
    void CopyAddress(unsigned port) {
        const auto shared = ShareAddresses();
        if (shared.empty()) {
            Say(Tr("Hosting. No VPN or local network found: friends need your public IP and the port open on your router."));
            return;
        }
        const std::string text = shared.front().ip + ":" + std::to_string(port);
        if (host_.action) host_.action("copy:" + text);
        Say(Tr("Hosting. Address copied: ") + text + " (" + Tr(shared.front().where) + "). " + Tr("Send it to your friends."));
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
        Item refresh = Action(fetch->busy ? "Refreshing..." : "Refresh the list", [this] { StartFetch(true); }, "Refresh");
        refresh.id = "refresh";
        items.push_back(refresh);
        if (fetch->busy && !fetch->fetched) {
            items.push_back(Info("Loading the list of rooms...", 1));
            return;
        }
        if (!fetch->list.ok) {
            items.push_back(Info(Plain(fetch->list.error), 4));
            return;
        }
        items.push_back(Info("* marks the rooms set up for MH4U, and they come first. You need MH4U USA 1.3.0."));
        for (const mh4n::rooms::LobbyRoom& room : fetch->list.rooms) {
            Item i;
            i.kind = Item::Action;
            i.room = true;
            i.label = (room.preferred ? "* " : "") + Plain(room.name);  // * = a room set up for MH4U
            i.value = std::to_string(room.players.size()) + "/" + std::to_string(room.max_players);
            i.locked = room.password;
            i.color = room.preferred ? 0 : 2;
            i.id = room.address + ":" + std::to_string(room.port) + room.name;
            i.help = "Joins this room. Rooms with a password ask for it first.";
            i.act = [this, room] {
                if (room.password) {
                    pending_ = room;
                    join_password_.clear();
                    SetPage(Page::Password);
                } else {
                    JoinRoom(room.address, room.port, "");
                    SetPage(Page::Online);
                }
            };
            items.push_back(i);
        }
    }

    void JoinRoom(const std::string& host, std::uint16_t port, const std::string& password) {
        if (!mh4n::rooms::ValidNickname(nick_)) {
            Say("Nickname: 4 to 20 letters, digits, spaces, dots, underscores or dashes.");
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
            Say(Tr("Joining ") + host + ":" + std::to_string(port) + "...");
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

    // The text of an option's value: its name in the language, or the value with a capital.
    // "3x  (1200 x 720)": the multiple of the 3DS's top screen (400 x 240) and the size it comes to; auto says which it chose.
    static std::string ScaleText(const std::string& value, int auto_scale) {
        const bool automatic = value == "auto";
        const int n = automatic ? auto_scale : std::atoi(value.c_str());
        if (n < 1) return Tr("auto");
        const std::string size = std::to_string(n) + "x  (" + std::to_string(400 * n) + " \xC3\x97 " + std::to_string(240 * n) + ")";
        return automatic ? Tr("Auto") + "  " + size : size;
    }

    static std::string Shown(const Item& item, int auto_scale) {
        if (item.id == "language") return item.value == "en" ? "English" : item.value == "es" ? "Espa\xC3\xB1ol" : Tr("auto");
        if (item.id == "scale") return ScaleText(item.value, auto_scale);
        const std::string t = Tr(item.value);
        return t != item.value ? t : Capital(item.value);
    }

    static std::string Capital(std::string s) {
        if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
        return s;
    }

    // ---- the picture -----------------------------------------------------------------------------------------------------
    // The page as the look draws it (a View) and, for when that can't be drawn, as plain lines of text.
    void Render(std::vector<Line>* out_lines, View* out_view) {
        static const char* const kTitles[] = {"Graphics", "Game", "Controls", "Audio", "Online", "Saves", "Mods", "System", "Public rooms", "Room password"};
        RefreshLanguage();
        std::vector<Item> items = Build();
        SyncSelection(items);
        const int sel = sel_[static_cast<int>(page_)];
        const auto now = std::chrono::steady_clock::now();

        View view;
        view.open = true;
        view.title = Tr(kTitles[static_cast<int>(page_)]);
        for (int t = 0; t < kTabCount; ++t) view.tabs.push_back(Tr(kTabNames[t]));
        view.keys = {{"A", Tr("Accept  (Enter)")}, {"B", Tr("Back  (Esc)")}, {"LB RB", Tr("Pages  (Q / E)")}, {"L3 R3", Tr("Close  (F1)")}};
        view.tab = TabOf(page_);
        view.sel = sel;
        view.hover_tab = hover_tab_;
        view.hover_row = hover_row_;
        for (std::size_t i = 0; i < items.size(); ++i) {
            const Item& item = items[i];
            ViewRow r;
            r.label = Tr(item.label);
            r.help = Tr(item.help);
            r.selected = static_cast<int>(i) == sel && item.Selectable();
            switch (item.kind) {
            case Item::Info:
                r.kind = ViewRow::Info;
                r.color = item.color;
                break;
            case Item::Choice:
                if (item.toggle) {
                    r.kind = ViewRow::Toggle;
                    r.on = item.on;
                    r.value = Tr(item.on ? "On" : "Off");
                } else if (item.slider) {
                    r.kind = ViewRow::Slider;
                    r.frac = item.frac;
                    r.value = item.value + (item.id == "volume" || item.id == "popup_size" ? "%" : "");
                } else {
                    r.kind = ViewRow::Choice;
                    r.value = Shown(item, std::atoi(host_.value("scale_auto").c_str()));
                }
                if (item.restart) r.label += " *";
                break;
            case Item::Action:
            case Item::Link:
                if (item.room) {
                    r.kind = ViewRow::Room;
                    r.value = item.value + (item.locked ? "  (" + Tr("password") + ")" : "");
                    r.enabled = item.color != 2;
                } else {
                    r.kind = ViewRow::Button;
                    r.value = Tr(item.cap);
                }
                break;
            case Item::Text: {
                r.kind = ViewRow::Text;
                std::string shown = *item.target;
                if (item.secret) shown.assign(shown.size(), '*');
                r.value = shown;
                r.caret = edit_ == item.target;
                break;
            }
            }
            view.rows.push_back(std::move(r));
        }
        if (!restart_.empty()) {
            std::string what;
            for (const std::string& r : restart_) what += (what.empty() ? "" : ", ") + Tr(r);
            view.notice = Tr("Waiting for a restart of the game: ") + what + ".";
        }
        if (now < message_until_) view.message = message_;
        view_ = view;

        // the plain version: one line a row, as the first look of the menu
        std::vector<Line> lines;
        std::string tabs_line;
        for (int t = 0; t < kTabCount; ++t) tabs_line += (t == view.tab ? "[" : " ") + std::string(kTabNames[t]) + (t == view.tab ? "]" : " ");
        lines.push_back({"CARAVAN4URECOMP - " + view.title, 1, false});
        lines.push_back({tabs_line + "   (Q / E: pages)", 2, false});
        const int rows = kMaxRows - 7;
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
            } else if (item.room) {
                text = item.label + "  " + item.value;
            } else {
                text = item.label;
            }
            lines.push_back({Plain(text), selected ? 0 : item.color, selected});
        }
        lines.push_back({"", 0, false});
        if (!view.notice.empty()) lines.push_back({Plain(view.notice), 1, false});
        lines.push_back({Plain(view.message), 1, false});
        lines.push_back({edit_ != nullptr ? "ENTER/A: OK  ESC/B: CANCEL  PAD: UP/DN=LETTER R=ADD L=DEL"
                                          : "ARROWS/DPAD/MOUSE: MOVE  ENTER/A/CLICK: OK  ESC/B/RCLICK: BACK",
                         2, false});
        *out_lines = std::move(lines);
        *out_view = std::move(view);
    }
};

}  // namespace ingame
