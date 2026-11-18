// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by
// khorne-hub (https://github.com/khorne-hub/4u-native); see ADDITIONAL-TERMS and THIRD_PARTY_NOTICES.
//
// The player's saves: backups, and putting one back. The game's saves live in the user folder (the `user` of the settings):
//   <user>\sdmc   the 3DS's SD card (the hunter's save data and the quests' extra data)
//   <user>\nand   the system's extra data
// A backup is a folder <user>\backups\<yyyymmdd-hhmmss>[-<label>] with copies of those two folders. Putting one back is never done
// while the game runs (it holds its files): it is asked for, and done by ApplyPendingRestore the next time the game starts, after
// a backup of what is there ("before-restore"), so a mistake can be undone. Nothing here touches the game's files, only saves.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace savemgr {

namespace fs = std::filesystem;

inline const char* const kFolders[] = {"sdmc", "nand"};  // what holds the saves, in the user folder
inline const char* const kBackups = "backups";
inline const char* const kPendingFile = "restore_pending.txt";  // in the backups folder: the name of the backup to put back

struct Backup {
    std::string name;      // the folder's name
    std::string stamp;     // "2026-11-17 18:30:15"
    std::string label;     // "", "auto", "before-restore" or what the player gave
    std::uint64_t bytes = 0;
    std::uint32_t files = 0;
};

struct Tree {
    std::uint64_t bytes = 0;
    std::uint32_t files = 0;
    std::uint64_t signature = 0;  // size and write time of every file, mixed: the same saves give the same number
};

inline Tree Measure(const fs::path& root) {
    Tree t;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return t;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code fe;
        if (!it->is_regular_file(fe) || it->path().filename().string().rfind('.', 0) == 0) continue;  // (a backup's .signature is not a save)
        const std::uint64_t size = it->file_size(fe);
        const auto time = it->last_write_time(fe).time_since_epoch().count();
        t.bytes += size;
        ++t.files;
        t.signature = t.signature * 1099511628211ull + size * 31u + static_cast<std::uint64_t>(time);
    }
    return t;
}

inline Tree MeasureSaves(const fs::path& user) {
    Tree total;
    for (const char* f : kFolders) {
        const Tree t = Measure(user / f);
        total.bytes += t.bytes;
        total.files += t.files;
        total.signature = total.signature * 1469598103934665603ull + t.signature;
    }
    return total;
}

inline fs::path BackupsFolder(const fs::path& user) { return user / kBackups; }

inline std::string NowName(const std::string& label) {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
    return std::string(buf) + (label.empty() ? "" : "-" + label);
}

// "20261117-183015-auto" -> stamp "2026-11-17 18:30:15", label "auto"
inline void SplitName(const std::string& name, std::string* stamp, std::string* label) {
    *stamp = name;
    label->clear();
    if (name.size() >= 15 && name[8] == '-') {
        *stamp = name.substr(0, 4) + "-" + name.substr(4, 2) + "-" + name.substr(6, 2) + " " + name.substr(9, 2) + ":" +
                 name.substr(11, 2) + ":" + name.substr(13, 2);
        if (name.size() > 16) *label = name.substr(16);
    }
}

// The backups, the newest first.
inline std::vector<Backup> List(const fs::path& user) {
    std::vector<Backup> out;
    std::error_code ec;
    const fs::path dir = BackupsFolder(user);
    if (!fs::is_directory(dir, ec)) return out;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::error_code de;
        if (!e.is_directory(de)) continue;
        Backup b;
        b.name = e.path().filename().string();
        if (b.name.empty() || b.name[0] == '.') continue;
        SplitName(b.name, &b.stamp, &b.label);
        const Tree t = Measure(e.path());
        b.bytes = t.bytes;
        b.files = t.files;
        out.push_back(std::move(b));
    }
    std::sort(out.begin(), out.end(), [](const Backup& a, const Backup& c) { return a.name > c.name; });
    return out;
}

inline bool CopyFolder(const fs::path& from, const fs::path& to, std::string* error) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        if (error) *error = ec.message();
        return false;
    }
    return true;
}

// Makes a backup of the saves now. Returns its name, or "" (and *error): nothing to back up, or a copy failed (then the
// half-made folder is removed).
inline std::string MakeBackup(const fs::path& user, const std::string& label, std::string* error) {
    const Tree now = MeasureSaves(user);
    if (now.files == 0) {
        if (error) *error = "There are no saves yet.";
        return {};
    }
    std::string name = NowName(label);
    const fs::path dir = BackupsFolder(user) / name;
    std::error_code ec;
    for (int n = 2; fs::exists(dir, ec) && n < 100; ++n) name = NowName(label) + "-" + std::to_string(n);  // (two in a second)
    const fs::path target = BackupsFolder(user) / name;
    for (const char* f : kFolders) {
        if (!fs::is_directory(user / f, ec)) continue;
        if (!CopyFolder(user / f, target / f, error)) {
            fs::remove_all(target, ec);
            return {};
        }
    }
    std::ofstream(target / ".signature") << now.signature << "\n";
    return name;
}

inline std::uint64_t LastSignature(const fs::path& user, const std::string& label_filter) {
    for (const Backup& b : List(user)) {  // (the newest first)
        if (!label_filter.empty() && b.label != label_filter) continue;
        std::ifstream in(BackupsFolder(user) / b.name / ".signature");
        std::uint64_t s = 0;
        if (in >> s) return s;
        return 0;
    }
    return 0;
}

// At the start of the game: a backup called "auto" unless nothing changed since the last backup of any kind, keeping the newest
// `keep` of the "auto" ones (the player's own are never removed). Returns what it did, for the log.
inline std::string AutoBackup(const fs::path& user, int keep) {
    const Tree now = MeasureSaves(user);
    if (now.files == 0) return "no saves to back up";
    if (LastSignature(user, "") == now.signature) return "the saves are the same as in the last backup";
    std::string error;
    const std::string name = MakeBackup(user, "auto", &error);
    if (name.empty()) return "automatic backup failed: " + error;
    int seen = 0;
    for (const Backup& b : List(user)) {
        if (b.label != "auto") continue;
        if (++seen > keep) {
            std::error_code ec;
            fs::remove_all(BackupsFolder(user) / b.name, ec);
        }
    }
    return "backed up the saves as " + name;
}

inline bool Delete(const fs::path& user, const std::string& name) {
    if (name.empty() || name.find_first_of("\\/") != std::string::npos || name[0] == '.') return false;
    std::error_code ec;
    return fs::remove_all(BackupsFolder(user) / name, ec) > 0 && !ec;
}

// ---- putting one back: asked for now, done at the next start ----

inline std::string Pending(const fs::path& user) {
    std::ifstream in(BackupsFolder(user) / kPendingFile);
    std::string name;
    std::getline(in, name);
    while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
    return name;
}

inline bool StageRestore(const fs::path& user, const std::string& name) {
    std::error_code ec;
    if (name.empty() || name.find_first_of("\\/") != std::string::npos || !fs::is_directory(BackupsFolder(user) / name, ec)) return false;
    fs::create_directories(BackupsFolder(user), ec);
    std::ofstream out(BackupsFolder(user) / kPendingFile, std::ios::trunc);
    out << name << "\n";
    return static_cast<bool>(out);
}

inline void CancelRestore(const fs::path& user) {
    std::error_code ec;
    fs::remove(BackupsFolder(user) / kPendingFile, ec);
}

// Before the game starts: if a restore was asked for, a backup of the saves now ("before-restore"), then the saves of the chosen
// backup take their place. Returns what it did ("" when nothing was asked for).
inline std::string ApplyPendingRestore(const fs::path& user) {
    const std::string name = Pending(user);
    if (name.empty()) return {};
    std::error_code ec;
    const fs::path from = BackupsFolder(user) / name;
    CancelRestore(user);  // (once: a failure does not repeat at every start)
    if (!fs::is_directory(from, ec)) return "the backup " + name + " is not there any more: nothing was restored";
    std::string error;
    if (MeasureSaves(user).files != 0 && MakeBackup(user, "before-restore", &error).empty()) {
        return "the saves now could not be backed up (" + error + "): nothing was restored";
    }
    for (const char* f : kFolders) {
        fs::remove_all(user / f, ec);
        if (fs::is_directory(from / f, ec) && !CopyFolder(from / f, user / f, &error)) return "restoring failed: " + error;
    }
    return "restored the saves of " + name;
}

}  // namespace savemgr
