// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by
// khorne-hub (https://github.com/khorne-hub/4u-native); see ADDITIONAL-TERMS and THIRD_PARTY_NOTICES.
//
// Putting a mod in place: a folder, or a .zip, dropped on the window becomes <user>\load\mods\<name>\ (what the runtime reads, see
// mods/mod_list.h). A zip is unpacked with the tar that comes with Windows 10 and 11. A mod that changes the game's code (a code
// mod) is installed turned off: the code library would have to be built with it, which this program does not do, and the runtime
// refuses to start the game with a library made without a code mod that is on.
#pragma once

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "mods/mod_list.h"

namespace modmgr {

namespace fs = std::filesystem;

inline constexpr unsigned long long kProgramId = 0x0004000000126300ull;

// What makes a folder a mod (mods/mod_list.h): romfs/, romfs_ext/, exefs/, an exheader.bin or a code file at its top.
inline bool LooksLikeMod(const fs::path& dir) {
    std::error_code ec;
    for (const char* sub : {"romfs", "romfs_ext", "exefs"}) {
        if (fs::is_directory(dir / sub, ec)) return true;
    }
    for (const char* file : {"exheader.bin", "code.ips", "code.bps", "code.bin"}) {
        if (fs::is_regular_file(dir / file, ec)) return true;
    }
    return false;
}

// The folder that holds the mod: `dir` itself, or the only folder inside it (zips often wrap the mod in a folder), a few levels deep.
inline fs::path FindModRoot(const fs::path& dir, int depth = 3) {
    if (LooksLikeMod(dir)) return dir;
    if (depth <= 0) return {};
    std::error_code ec;
    std::vector<fs::path> subs;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::error_code de;
        if (e.is_directory(de)) subs.push_back(e.path());
    }
    if (subs.size() == 1) return FindModRoot(subs[0], depth - 1);
    // Emulator-style packages hold one folder per title id (0004000000126300 = the game, 0004000E00126300 = its update):
    // the update's files are the ones that apply on top of the game as this program runs it, so take that one.
    auto is_title_id = [](const fs::path& p) {
        const std::string n = p.filename().string();
        return n.size() == 16 && std::all_of(n.begin(), n.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
    };
    fs::path game_folder;
    for (const fs::path& sub : subs) {
        if (!is_title_id(sub) || !LooksLikeMod(sub)) continue;
        std::string n = sub.filename().string();
        for (char& c : n) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (n.rfind("0004000E", 0) == 0) return sub;
        game_folder = sub;
    }
    return game_folder;
}

inline std::string SafeName(std::string name) {
    for (char& c : name) {
        if (std::strchr("<>:\"/\\|?*", c) != nullptr || static_cast<unsigned char>(c) < 0x20) c = '_';
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
    while (!name.empty() && (name.front() == '.' || name.front() == '_' || name.front() == ' ')) name.erase(name.begin());  // (the runtime skips those)
    return name;
}

// tar -xf <zip> -C <dest>, without a window. True when it succeeded.
inline bool Unzip(const fs::path& zip, const fs::path& dest) {
    std::error_code ec;
    fs::create_directories(dest, ec);
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring tar = std::wstring(sys) + L"\\tar.exe";
    std::wstring command = L"\"" + tar + L"\" -xf \"" + zip.wstring() + L"\" -C \"" + dest.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info)) return false;
    WaitForSingleObject(info.hProcess, 120000);
    DWORD code = 1;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    CloseHandle(info.hThread);
    return code == 0;
}

// The mods now, in load order, as the runtime reads them.
inline std::vector<mh4n::mods::Mod> List(const std::string& user_utf8) {
    return mh4n::mods::List(mh4n::mods::Folder(user_utf8), kProgramId);
}

inline bool SetOn(const std::string& user_utf8, const std::string& name, bool on) {
    std::vector<mh4n::mods::Mod> mods = List(user_utf8);
    bool found = false;
    for (auto& m : mods) {
        if (m.name == name) {
            m.on = on;
            found = true;
        }
    }
    return found && mh4n::mods::SaveOrder(mh4n::mods::Folder(user_utf8), mods);
}

inline bool Move(const std::string& user_utf8, const std::string& name, int by) {
    std::vector<mh4n::mods::Mod> mods = List(user_utf8);
    const auto it = std::find_if(mods.begin(), mods.end(), [&](const mh4n::mods::Mod& m) { return m.name == name; });
    if (it == mods.end()) return false;
    const int from = static_cast<int>(it - mods.begin());
    const int to = std::clamp(from + by, 0, static_cast<int>(mods.size()) - 1);
    if (to == from) return true;
    std::swap(mods[static_cast<std::size_t>(from)], mods[static_cast<std::size_t>(to)]);
    return mh4n::mods::SaveOrder(mh4n::mods::Folder(user_utf8), mods);
}

// At the start of the game: a code mod that is on would keep the game from starting (the code library is made without any), so the
// ones that are on are turned off. Returns their names.
inline std::vector<std::string> DisableCodeMods(const std::string& user_utf8) {
    std::vector<std::string> names;
    std::vector<mh4n::mods::Mod> mods = List(user_utf8);
    for (auto& m : mods) {
        if (m.code && m.on) {
            m.on = false;
            names.push_back(m.name);
        }
    }
    if (!names.empty()) mh4n::mods::SaveOrder(mh4n::mods::Folder(user_utf8), mods);
    return names;
}

// Installs a dropped folder or .zip. The answer is the line the menu shows ("" never: a failure says why).
inline std::string Install(const std::string& user_utf8, const fs::path& dropped) {
    std::error_code ec;
    const fs::path mods_dir = fs::path(mh4n::mods::Folder(user_utf8));
    fs::create_directories(mods_dir, ec);
    fs::path source = dropped;
    fs::path temp;
    std::string name = dropped.stem().string();
    if (fs::is_regular_file(dropped, ec)) {
        std::string ext = dropped.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".zip") return "A mod is a folder or a .zip: " + dropped.filename().string() + " is neither.";
        temp = mods_dir / ".installing";
        fs::remove_all(temp, ec);
        if (!Unzip(dropped, temp)) {
            fs::remove_all(temp, ec);
            return "The .zip could not be unpacked.";
        }
        source = temp;
    } else if (fs::is_directory(dropped, ec)) {
        name = dropped.filename().string();
    } else {
        return "That is not a folder or a .zip.";
    }
    const fs::path root = FindModRoot(source);
    if (root.empty()) {
        if (!temp.empty()) fs::remove_all(temp, ec);
        return "That is not a mod: it has no romfs, romfs_ext or exefs folder.";
    }
    name = SafeName(name);
    if (name.empty()) name = "mod";
    std::string unique = name;
    for (int n = 2; fs::exists(mods_dir / unique, ec) && n < 100; ++n) unique = name + " (" + std::to_string(n) + ")";
    fs::copy(root, mods_dir / unique, fs::copy_options::recursive, ec);
    if (!temp.empty()) fs::remove_all(temp, ec);
    if (ec) return "The mod could not be copied: " + ec.message();
    for (const auto& m : List(user_utf8)) {
        if (m.name == unique && m.code) {
            SetOn(user_utf8, unique, false);
            return "Installed " + unique + ", turned off: it changes the game's code, which this program can't use.";
        }
    }
    return "Installed " + unique + ". It applies the next time the game starts.";
}

}  // namespace modmgr
