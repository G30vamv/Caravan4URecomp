// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by
// khorne-hub (https://github.com/khorne-hub/4u-native); see ADDITIONAL-TERMS and THIRD_PARTY_NOTICES.
//
// The first start, in the program's own window: it asks for the two files of the game (a button, or drag them onto the
// window), then runs PrepareGame.ps1 with no console window and shows the progress as it happens. What the script says is
// read from a pipe; the lines that mean a step (the download of the compiler and its MB, the translation, "compiled 12/211")
// move the bar. The picture of it is menu_gui.cpp's, in the menu's design.
#pragma once

#include <windows.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "menu_gui.h"
#include "menu_text.h"

namespace ingame {

class Setup {
public:
    enum class Stage { Choose, Work, Failed, Done };

    explicit Setup(std::string exedir) : exedir_(std::move(exedir)) {}
    ~Setup() { Stop(); }

    // Starts at the choice of the files, or at once when both are given.
    void Begin(const std::string& base, const std::string& update, bool start_now) {
        base_ = base;
        update_ = update;
        stage_ = Stage::Choose;
        focus_ = 0;
        changed_ = true;
        if (start_now && Ready()) Start();
    }

    Stage stage() const { return stage_; }
    bool QuitRequested() const { return quit_; }
    // The setup is over and the game may start (a moment after "Ready", so that it can be read).
    bool Finished() const {
        return stage_ == Stage::Done && std::chrono::steady_clock::now() - done_at_ > std::chrono::milliseconds(1400);
    }
    const std::string& Base() const { return base_; }
    const std::string& UpdateFile() const { return update_; }

    // Stops the script and everything it started.
    void Stop() {
        if (job_ != nullptr) TerminateJobObject(job_, 1);  // (the pipe closes, and the reader ends)
        if (reader_.joinable()) reader_.join();
        if (job_ != nullptr) {
            CloseHandle(job_);
            job_ = nullptr;
        }
        if (process_ != nullptr) {
            CloseHandle(process_);
            process_ = nullptr;
        }
        if (read_pipe_ != nullptr) {
            CloseHandle(read_pipe_);
            read_pipe_ = nullptr;
        }
    }

    void OnEvent(const SDL_Event& e, SDL_Window* window) {
        window_ = window;
        switch (e.type) {
        case SDL_EVENT_DROP_FILE:
            if (stage_ == Stage::Choose && e.drop.data != nullptr) Assign(e.drop.data);
            break;
        case SDL_EVENT_MOUSE_MOTION: {
            const int hit = Hit(window, e.motion.x, e.motion.y);
            if (hit != hover_) {
                hover_ = hit;
                changed_ = true;
            }
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (e.button.button == SDL_BUTTON_LEFT) Activate(Hit(window, e.button.x, e.button.y));
            break;
        case SDL_EVENT_KEY_DOWN:
            switch (e.key.key) {
            case SDLK_TAB:
            case SDLK_DOWN: MoveFocus(1); break;
            case SDLK_UP: MoveFocus(-1); break;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
            case SDLK_SPACE: Activate(focus_); break;
            case SDLK_ESCAPE:
                if (stage_ == Stage::Work) Activate(3);
                break;
            default: break;
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) MoveFocus(1);
            else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP) MoveFocus(-1);
            else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) Activate(focus_);
            break;
        default: break;
        }
    }

    // Once per loop, on the main thread. True when what the screen shows changed.
    bool Update() {
        {   // a file chosen in the dialog
            std::lock_guard lock(pick_mutex_);
            if (!picked_.empty()) {
                Assign(picked_);
                picked_.clear();
            }
        }
        const bool script_ended = exited_.load();  // (before the lines: the last of them are in by then)
        std::vector<std::string> lines;
        {
            std::lock_guard lock(mutex_);
            lines.swap(pending_);
        }
        for (const std::string& line : lines) Parse(line);
        if (stage_ == Stage::Work) {
            const auto now = std::chrono::steady_clock::now();
            if (phase_ == Phase::Translate) {  // no count while it translates: the bar creeps towards the end of that step
                const float t = std::chrono::duration<float>(now - phase_at_).count();
                target_ = std::max(target_, 0.20f + 0.14f * (1.0f - std::exp(-t / 70.0f)));
                changed_ = true;
            }
            if (script_ended) {
                DWORD code = 1;
                GetExitCodeProcess(process_, &code);
                Stop();
                if (code == 0) {
                    target_ = 1.0f;
                    step_ = "Done.";
                    eta_.clear();
                    stage_ = Stage::Done;
                    done_at_ = now;
                } else {
                    stage_ = Stage::Failed;
                    focus_ = 2;
                }
                changed_ = true;
            }
            if (std::fabs(shown_ - target_) > 0.0005f) {
                shown_ += (target_ - shown_) * 0.15f;
                changed_ = true;
            }
        }
        const bool was = changed_;
        changed_ = false;
        return was;
    }

    SetupView MakeView() const {
        SetupView v;
        v.open = true;
        v.stage = static_cast<int>(stage_);
        v.base_path = base_;
        v.update_path = update_;
        v.focus = focus_;
        v.hover = hover_;
        v.placeholder = Tr("Choose a file, or drop it here");
        switch (stage_) {
        case Stage::Choose:
            v.title = Tr("First start");
            v.intro = Tr("Caravan4URecomp does not include the game. Choose YOUR OWN decrypted copy of Monster Hunter 4 Ultimate (USA): the base game and its v1.3.0 update. This is done once and takes a few minutes; the first time it also downloads the part of a compiler that it needs, about 50 MB (it needs Internet). What it builds stays on this PC: please don't share it.");
            v.base_label = Tr("The base game (.cia, .3ds or .cci)");
            v.update_label = Tr("The v1.3.0 update (.cia)");
            v.hint = Tr("You can also drag the files onto this window.");
            v.browse = Tr("Browse");
            v.primary = Tr("Start");
            v.primary_enabled = Ready();
            v.secondary = Tr("Quit");
            break;
        case Stage::Work:
            v.title = Tr("Preparing your game");
            v.step = Tr(step_) + stepdetail_;
            v.progress = shown_;
            v.eta = eta_.empty() ? Tr("You can leave this window open: it keeps working.") : eta_;
            v.log = log_;
            v.secondary = Tr("Cancel");
            v.focus = 3;
            break;
        case Stage::Done:
            v.title = Tr("Preparing your game");
            v.step = Tr("Ready! Starting the game...");
            v.progress = 1.0f;
            v.log = log_;
            break;
        case Stage::Failed:
            v.title = Tr("First start");
            v.step = Tr("Something went wrong");
            v.error = error_.empty() ? Tr("The game could not be prepared.") : Tr(error_);
            v.log = log_;
            v.primary = Tr("Try again");
            v.primary_enabled = true;
            v.secondary = Tr("Quit");
            break;
        }
        return v;
    }

private:
    enum class Phase { None, Download, Translate, Compile };

    std::string exedir_;
    std::string base_, update_;
    Stage stage_ = Stage::Choose;
    SDL_Window* window_ = nullptr;
    int focus_ = 0, hover_ = -1;
    bool changed_ = true, quit_ = false;
    std::chrono::steady_clock::time_point done_at_{}, phase_at_{}, started_at_{};

    // progress
    float target_ = 0.0f, shown_ = 0.0f;
    std::string step_, stepdetail_, eta_, error_;
    std::vector<std::string> log_;
    Phase phase_ = Phase::None;

    // the script
    HANDLE process_ = nullptr, job_ = nullptr, read_pipe_ = nullptr;
    std::thread reader_;
    std::mutex mutex_;
    std::vector<std::string> pending_;
    std::atomic<bool> exited_{false};

    // a file chosen in the dialog (its callback runs on another thread)
    std::mutex pick_mutex_;
    std::string picked_;
    int picking_ = -1;

    bool Ready() const {
        return !base_.empty() && !update_.empty() && GetFileAttributesW(Wide(base_).c_str()) != INVALID_FILE_ATTRIBUTES &&
               GetFileAttributesW(Wide(update_).c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    static std::wstring Wide(const std::string& text) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(std::max(n, 1)), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), n);
        wide.resize(static_cast<std::size_t>(std::max(n - 1, 0)));
        return wide;
    }

    // A file given (dropped or chosen): the update when its name says so, else the first one still empty.
    void Assign(const std::string& path) {
        std::string name = path;
        const auto slash = name.find_last_of("\\/");
        if (slash != std::string::npos) name.erase(0, slash + 1);
        std::string low = name;
        std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const bool looks_update = low.find("0004000e") != std::string::npos || low.find("patch") != std::string::npos ||
                                  low.find("update") != std::string::npos || low.find("v1.3") != std::string::npos;
        if (picking_ == 0) base_ = path;
        else if (picking_ == 1) update_ = path;
        else if (looks_update) update_ = path;
        else if (base_.empty()) base_ = path;
        else update_ = path;
        picking_ = -1;
        changed_ = true;
    }

    static void SDLCALL OnPicked(void* user, const char* const* files, int) {
        Setup* self = static_cast<Setup*>(user);
        std::lock_guard lock(self->pick_mutex_);
        if (files != nullptr && files[0] != nullptr) self->picked_ = files[0];
    }

    void Browse(int which) {
        static const SDL_DialogFileFilter filters[] = {{"Nintendo 3DS game files", "cia;3ds;cci;cxi"}, {"All files", "*"}};
        picking_ = which;
        SDL_ShowOpenFileDialog(&Setup::OnPicked, this, window_, filters, 2, nullptr, false);
    }

    // What is under a point of the window: 0, 1 a browse button, 2 the primary button, 3 the secondary one; -1 none.
    int Hit(SDL_Window* window, float x, float y) const {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        const float density = SDL_GetWindowPixelDensity(window);
        const SetupLayout s = SetupLayoutOf(static_cast<float>(w), static_cast<float>(h));
        const float px = x * density, py = y * density;
        if (stage_ == Stage::Choose) {
            if (s.base_browse.Contains(px, py)) return 0;
            if (s.update_browse.Contains(px, py)) return 1;
            if (s.base_field.Contains(px, py)) return 0;
            if (s.update_field.Contains(px, py)) return 1;
        }
        if ((stage_ == Stage::Choose || stage_ == Stage::Failed) && s.primary.Contains(px, py)) return 2;
        if (stage_ != Stage::Done && s.secondary.Contains(px, py)) return 3;
        return -1;
    }

    std::vector<int> Order() const {
        switch (stage_) {
        case Stage::Choose: return Ready() ? std::vector<int>{0, 1, 2, 3} : std::vector<int>{0, 1, 3};
        case Stage::Failed: return {2, 3};
        case Stage::Work: return {3};
        default: return {};
        }
    }

    void MoveFocus(int dir) {
        const std::vector<int> order = Order();
        if (order.empty()) return;
        auto it = std::find(order.begin(), order.end(), focus_);
        std::size_t i = it == order.end() ? 0 : static_cast<std::size_t>(it - order.begin());
        i = (i + order.size() + (dir < 0 ? order.size() - 1 : 1)) % order.size();
        focus_ = order[i];
        changed_ = true;
    }

    void Activate(int button) {
        if (button < 0) return;
        changed_ = true;
        if (stage_ == Stage::Choose) {
            if (button == 0 || button == 1) Browse(button);
            else if (button == 2 && Ready()) Start();
            else if (button == 3) quit_ = true;
        } else if (stage_ == Stage::Work) {
            if (button == 3) {  // cancel: what was started is stopped, the files stay chosen
                Stop();
                stage_ = Stage::Choose;
                focus_ = 2;
            }
        } else if (stage_ == Stage::Failed) {
            if (button == 2) {
                stage_ = Stage::Choose;
                focus_ = 2;
            } else if (button == 3) {
                quit_ = true;
            }
        }
    }

    // ---- the script --------------------------------------------------------------------------------------------------
    void Start() {
        Stop();
        log_.clear();
        pending_.clear();
        exited_ = false;
        error_.clear();
        step_ = "Starting...";
        stepdetail_.clear();
        eta_.clear();
        target_ = shown_ = 0.0f;
        phase_ = Phase::None;
        started_at_ = std::chrono::steady_clock::now();
        stage_ = Stage::Work;
        focus_ = 3;

        SetEnvironmentVariableW(L"PYTHONUNBUFFERED", L"1");  // so that what the tool says arrives as it says it
        SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
        HANDLE read = nullptr, write = nullptr;
        if (!CreatePipe(&read, &write, &sa, 0)) return Fail("The script could not be started.");
        SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
        HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        char system_dir[MAX_PATH] = {};
        GetSystemDirectoryA(system_dir, MAX_PATH);
        const std::string command = "\"" + std::string(system_dir) + "\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoProfile -ExecutionPolicy Bypass -File \"" +
                                    exedir_ + "\\PrepareGame.ps1\" -Base \"" + base_ + "\" -Update \"" + update_ + "\"";
        std::wstring wide = Wide(command);
        STARTUPINFOW startup = {};
        startup.cb = sizeof startup;
        startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        startup.hStdInput = nul;
        startup.hStdOutput = write;
        startup.hStdError = write;
        PROCESS_INFORMATION info = {};
        const BOOL ok = CreateProcessW(nullptr, wide.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                                       Wide(exedir_).c_str(), &startup, &info);
        CloseHandle(write);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        if (!ok) {
            CloseHandle(read);
            return Fail("PowerShell did not start.");
        }
        // everything the script starts dies with it (Cancel, or the window closing)
        job_ = CreateJobObjectW(nullptr, nullptr);
        if (job_ != nullptr) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof limits);
            AssignProcessToJobObject(job_, info.hProcess);
        }
        ResumeThread(info.hThread);
        CloseHandle(info.hThread);
        process_ = info.hProcess;
        read_pipe_ = read;
        reader_ = std::thread([this] { Read(); });
    }

    void Fail(const std::string& why) {
        error_ = why;
        stage_ = Stage::Failed;
        focus_ = 2;
        changed_ = true;
    }

    // The reader thread: what the script prints, cut into lines at the line ends and at the carriage returns of a progress.
    void Read() {
        char buffer[4096];
        std::string line;
        DWORD got = 0;
        const auto push = [&] {
            if (line.empty()) return;
            std::lock_guard lock(mutex_);
            pending_.push_back(line);
            line.clear();
        };
        while (ReadFile(read_pipe_, buffer, sizeof buffer, &got, nullptr) && got > 0) {
            for (DWORD i = 0; i < got; ++i) {
                if (buffer[i] == '\n' || buffer[i] == '\r') push();
                else line.push_back(buffer[i]);
            }
        }
        push();
        if (process_ != nullptr) WaitForSingleObject(process_, 20000);
        exited_ = true;
    }

    void Parse(const std::string& raw) {
        std::string line = raw;
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(0, 1);
        if (line.empty()) return;
        const auto starts = [&](const char* prefix) { return line.rfind(prefix, 0) == 0; };
        const auto now = std::chrono::steady_clock::now();
        // the lines that are only a number of MB / of objects are the bar's, not the log's
        unsigned a = 0, b = 0;
        char tail[8] = {};
        const bool counter = std::sscanf(line.c_str(), "%u/%u %7s", &a, &b, tail) >= 2;
        if (counter && std::string(tail) == "MB" && b > 0) {
            phase_ = Phase::Download;
            step_ = "Downloading the compiler";
            stepdetail_ = " (" + std::to_string(a) + " / " + std::to_string(b) + " MB)";
            // (the compiler comes after the translation of the code, or before it when the translation was already made)
            const float base = target_ >= 0.34f ? 0.35f : 0.01f, span = target_ >= 0.34f ? 0.03f : 0.12f;
            target_ = std::max(target_, base + span * static_cast<float>(a) / static_cast<float>(b));
            changed_ = true;
            return;
        }
        if (starts("compiled ") && std::sscanf(line.c_str(), "compiled %u/%u", &a, &b) == 2 && b > 0) {
            phase_ = Phase::Compile;
            step_ = "Compiling the game's code for this PC";
            stepdetail_ = " (" + std::to_string(a) + " / " + std::to_string(b) + ")";
            const float f = static_cast<float>(a) / static_cast<float>(b);
            target_ = std::max(target_, 0.38f + 0.58f * f);
            const float elapsed = std::chrono::duration<float>(now - phase_at_).count();
            if (f > 0.03f && elapsed > 5.0f) {
                const int minutes = static_cast<int>(std::ceil(elapsed * (1.0f - f) / f / 60.0f));
                eta_ = minutes <= 1 ? Tr("Less than a minute left.") : Tr("About ") + std::to_string(minutes) + Tr(" minutes left.");
            }
            changed_ = true;
            return;
        }
        if (starts("analysed ") && std::sscanf(line.c_str(), "analysed %u/%u", &a, &b) == 2 && b > 0) {  // the bar's, not the log's
            phase_ = Phase::Translate;
            step_ = "Translating the game's code";
            stepdetail_.clear();
            target_ = std::max(target_, 0.20f + 0.14f * static_cast<float>(a) / static_cast<float>(b));
            changed_ = true;
            return;
        }
        Log(line);
        if (starts("downloading")) {
            step_ = "Downloading the compiler";
            stepdetail_.clear();
        } else if (starts("No C compiler found")) {
            step_ = "Downloading the compiler";
            stepdetail_.clear();
            target_ = std::max(target_, 0.01f);
        } else if (starts("unpacking")) {
            step_ = "Unpacking the compiler";
            stepdetail_.clear();
            target_ = std::max(target_, 0.14f);
        } else if (starts("Building the code library")) {
            step_ = "Reading your game files";
            stepdetail_.clear();
            target_ = std::max(target_, 0.16f);
        } else if (starts("extracting")) {
            step_ = "Reading your game files";
            target_ = std::max(target_, 0.17f);
        } else if (starts("recompiling")) {
            step_ = "Translating the game's code";
            stepdetail_.clear();
            phase_ = Phase::Translate;
            phase_at_ = now;
            target_ = std::max(target_, 0.20f);
        } else if (line.find("functions,") != std::string::npos) {
            phase_ = Phase::None;
            step_ = "Translating the game's code";
            target_ = std::max(target_, 0.35f);
        } else if (starts("geometry shaders")) {
            target_ = std::max(target_, 0.37f);
        } else if (starts("compiling with")) {
            phase_ = Phase::Compile;
            phase_at_ = now;
            step_ = "Compiling the game's code for this PC";
            stepdetail_.clear();
            target_ = std::max(target_, 0.38f);
        } else if (starts("linked ") || starts("code library:")) {
            phase_ = Phase::None;
            step_ = "Linking the code library";
            stepdetail_.clear();
            eta_.clear();
            target_ = std::max(target_, 0.96f);
        } else if (starts("Preparing the game folder")) {
            step_ = "Preparing the game folder";
            target_ = std::max(target_, 0.97f);
        } else if (starts("ERROR:")) {
            error_ = line.substr(6);
            while (!error_.empty() && error_.front() == ' ') error_.erase(0, 1);
        }
        changed_ = true;
    }

    void Log(const std::string& line) {
        log_.push_back(line.size() > 160 ? line.substr(0, 157) + "..." : line);
        if (log_.size() > 40) log_.erase(log_.begin(), log_.begin() + 10);
    }
};

}  // namespace ingame
