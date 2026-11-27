// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
//
// HidHide (installed by DSX / DS4Windows / ViGEm setups) hides physical controllers from every program that is not on its
// application list, so Windows answers "access denied" when this program opens a DualSense and the game never sees it. DSX and
// DS4Windows are on the list and offer a virtual Xbox pad instead; when that pad exists nothing is needed. When it does not (DSX
// closed), the fix is to add this program to HidHide's list with `HidHideCLI --app-reg` (needs administrator; undo with
// `--app-unreg`). Adapted from the author's other port (launcher/hidhide.py). Nothing else of HidHide's setup is touched.
#pragma once

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

#include "utf8_path.h"

namespace hidhide {

inline std::filesystem::path FindCli() {
    for (const char* var : {"ProgramFiles", "ProgramW6432"}) {
        if (const char* root = std::getenv(var)) {
            const std::filesystem::path cli = std::filesystem::path(root) / "Nefarius Software Solutions" / "HidHide" / "x64" / "HidHideCLI.exe";
            if (std::filesystem::exists(cli)) return cli;
        }
    }
    return {};
}

/// Runs HidHideCLI with one argument, no window, and returns what it printed ("" on any failure).
inline std::string Run(const std::filesystem::path& cli, const wchar_t* arg) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &sa, 0)) return {};
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = write;
    si.hStdError = write;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring command = L"\"" + cli.wstring() + L"\" " + arg;
    std::string out;
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(write);
        write = nullptr;
        char buffer[4096];
        DWORD n = 0;
        while (ReadFile(read, buffer, sizeof buffer, &n, nullptr) && n > 0) out.append(buffer, n);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    if (write) CloseHandle(write);
    CloseHandle(read);
    return out;
}

inline std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// Whether `exe` is on HidHide's application list.
inline bool OnList(const std::string& app_list, const std::filesystem::path& exe) {
    const std::string want = Lower(u8p::Utf8(exe.lexically_normal()));
    std::size_t at = 0;
    while ((at = app_list.find("--app-reg", at)) != std::string::npos) {
        const std::size_t a = app_list.find('"', at), b = a == std::string::npos ? a : app_list.find('"', a + 1);
        if (b == std::string::npos) break;
        if (Lower(u8p::Utf8(u8p::PathOf(app_list.substr(a + 1, b - a - 1)).lexically_normal())) == want) return true;
        at = b;
    }
    return false;
}

/// The HidHideCLI path when HidHide's cloak hides a Sony or Nintendo gamepad from `exe` (it is not on the list); empty otherwise.
/// The caller says whether an XInput pad stands in (DSX or DS4Windows running), in which case nothing is needed.
inline std::filesystem::path HidingFrom(const std::filesystem::path& exe, bool xinput_pad) {
    if (xinput_pad) return {};
    const std::filesystem::path cli = FindCli();
    if (cli.empty()) return {};
    if (Run(cli, L"--cloak-state").find("--cloak-on") == std::string::npos) return {};
    if (Run(cli, L"--inv-state").find("--inv-on") != std::string::npos) return {};
    const std::string hidden = Lower(Run(cli, L"--dev-list"));
    bool pad = false;
    for (const char* vendor : {"vid_054c", "vid&0002054c", "vid_057e", "vid&0002057e"}) pad = pad || hidden.find(vendor) != std::string::npos;
    if (!pad || OnList(Run(cli, L"--app-list"), exe)) return {};
    return cli;
}

/// Adds `exe` to HidHide's application list through an administrator prompt (UAC); true when the command ran and succeeded.
inline bool Allow(const std::filesystem::path& cli, const std::filesystem::path& exe) {
    const std::wstring params = L"--app-reg \"" + exe.wstring() + L"\"";
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = cli.c_str();
    info.lpParameters = params.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info) || !info.hProcess) return false;
    WaitForSingleObject(info.hProcess, 15000);
    DWORD code = 1;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    return code == 0;
}

}  // namespace hidhide
