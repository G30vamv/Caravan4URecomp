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
#include <functional>
#include <string>
#include <vector>

#include "utf8_path.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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

// Paths past the old 260-character limit (a deep install folder plus the saves' own depth) need the \?\ prefix, or the
// copy fails with "The filename or extension is too long" and no backup is made.
inline fs::path Long(const fs::path& p) {
#if defined(_WIN32)
    std::error_code ec;
    fs::path abs = fs::absolute(p, ec);
    if (ec) return p;
    abs = abs.lexically_normal();  // behind \\?\ Windows takes ".." as a name: a user folder written with it showed no saves
    const std::wstring w = abs.make_preferred().wstring();
    if (w.rfind(LR"(\\?\)", 0) == 0) return abs;
    if (w.size() < 2 || w[1] != L':') return abs;
    return fs::path(std::wstring(LR"(\\?\)") + w);
#else
    return p;
#endif
}

inline Tree Measure(const fs::path& root) {
    Tree t;
    std::error_code ec;
    if (!fs::is_directory(Long(root), ec)) return t;
    for (fs::recursive_directory_iterator it(Long(root), fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code fe;
        if (!it->is_regular_file(fe) || u8p::Utf8(it->path().filename()).rfind('.', 0) == 0) continue;  // (a backup's .signature is not a save)
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

// MODIFIED for the Windows port: `backup_dir` of the ini sends the backups to another folder (e.g. on another drive).
inline fs::path g_backup_dir;
inline fs::path BackupsFolder(const fs::path& user) { return g_backup_dir.empty() ? user / kBackups : g_backup_dir; }

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
        // "-2", "-3"... only tell two backups of one second apart: "auto-2" is an automatic one like any other
        for (const char* own : {"auto", "before-restore", "imported", ""}) {
            const std::string prefix = *own ? std::string(own) + "-" : std::string();
            if (label->rfind(prefix, 0) != 0) continue;
            const std::string number = label->substr(prefix.size());
            if (!number.empty() && number.size() <= 2 && number.find_first_not_of("0123456789") == std::string::npos) {
                *label = own;
                break;
            }
        }
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
        b.name = u8p::Utf8(e.path().filename());
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
    fs::create_directories(Long(to.parent_path()), ec);
    fs::copy(Long(from), Long(to), fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        if (error) *error = ec.message();
        return false;
    }
    return true;
}

// A copy holds the saves when it has as many files, with as many bytes, as what it was made from.
inline bool SameSaves(const Tree& a, const Tree& b) { return a.files == b.files && a.bytes == b.bytes; }

// Makes a new folder in the backups, filled by `fill(folder, &why)`, which also gives the signature to note with it. Returns
// its name, or "" (and *error). It is made in a folder the list does not show (".partial-<name>") and gets its name only when
// it is whole, so one cut short (the program closed, a power cut) is never offered. A finished one says in ".complete" what
// it holds.
inline std::string NewBackup(const fs::path& user, const std::string& label,
                             const std::function<bool(const fs::path&, std::uint64_t*, std::string*)>& fill, std::string* error) {
    const fs::path dir = BackupsFolder(user);
    std::error_code ec;
    fs::create_directories(Long(dir), ec);
    std::vector<fs::path> stale;  // copies cut short on an earlier day or hour (a newer one may be another copy of the game at work)
    for (fs::directory_iterator it(Long(dir), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code te;
        if (u8p::Utf8(it->path().filename()).rfind(".partial-", 0) != 0) continue;
        const auto age = fs::file_time_type::clock::now() - fs::last_write_time(it->path(), te);
        if (!te && age > std::chrono::hours(1)) stale.push_back(it->path());
    }
    for (const fs::path& p : stale) fs::remove_all(p, ec);
    // a name of its own: two backups in the same second get "-2", "-3"... (never one written over another)
    const std::string first = NowName(label);
    std::string name = first;
    const auto taken = [&](const std::string& n) {
        std::error_code te;
        return fs::exists(Long(dir / n), te) || fs::exists(Long(dir / (".partial-" + n)), te);
    };
    for (int n = 2; taken(name) && n < 100; ++n) name = first + "-" + std::to_string(n);
    if (taken(name)) {
        if (error) *error = "Too many backups in the same second.";
        return {};
    }
    const fs::path partial = dir / (".partial-" + name);
    const auto give_up = [&](const std::string& why) {
        std::error_code re;
        fs::remove_all(Long(partial), re);
        if (error) *error = why;
        return std::string{};
    };
    fs::create_directories(Long(partial), ec);
    std::uint64_t noted = 0;
    {
        std::string why;
        if (!fill(partial, &noted, &why)) return give_up(why);
    }
    const Tree copied = MeasureSaves(partial);
    {
        std::ofstream signature(Long(partial / ".signature"));
        signature << noted << "\n";
        std::ofstream complete(Long(partial / ".complete"));
        complete << copied.files << " " << copied.bytes << "\n";
        signature.flush();
        complete.flush();
        if (!signature || !complete) return give_up("The backup could not be finished (is the disk full?).");
    }
    fs::rename(Long(partial), Long(dir / name), ec);
    if (ec) return give_up(ec.message());
    return name;
}

// A copy of the save folders there are in `from` into `to`.
inline bool CopySaves(const fs::path& from, const fs::path& to, std::string* error) {
    std::error_code ec;
    for (const char* f : kFolders) {
        if (!fs::is_directory(Long(from / f), ec)) continue;
        if (!CopyFolder(from / f, to / f, error)) return false;
    }
    return true;
}

// Makes a backup of the saves now. Returns its name, or "" (and *error): nothing to back up, or a copy failed.
inline std::string MakeBackup(const fs::path& user, const std::string& label, std::string* error) {
    const Tree now = MeasureSaves(user);
    if (now.files == 0) {
        if (error) *error = "There are no saves yet.";
        return {};
    }
    return NewBackup(
        user, label,
        [&](const fs::path& to, std::uint64_t* signature, std::string* why) {
            if (!CopySaves(user, to, why)) return false;
            if (!SameSaves(MeasureSaves(to), now)) {
                *why = "The saves changed while they were being copied. Try again.";
                return false;
            }
            *signature = now.signature;
            return true;
        },
        error);
}

inline std::uint64_t LastSignature(const fs::path& user, const std::string& label_filter) {
    for (const Backup& b : List(user)) {  // (the newest first)
        if (!label_filter.empty() && b.label != label_filter) continue;
        std::ifstream in(BackupsFolder(user) / u8p::PathOf(b.name) / ".signature");
        std::uint64_t s = 0;
        if (in >> s) return s;
        return 0;
    }
    return 0;
}

inline bool RestoreUnfinished(const fs::path& user);  // (below, with the restore)

// At the start of the game: a backup called "auto" unless nothing changed since the last backup of any kind, keeping the newest
// `keep` of the "auto" ones (the player's own are never removed). Returns what it did, for the log.
inline std::string AutoBackup(const fs::path& user, int keep) {
    // half-restored saves are not worth a copy, and each copy of them would push a good one out
    if (RestoreUnfinished(user)) return "no automatic backup: a restore of the saves is not finished";
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
            fs::remove_all(Long(BackupsFolder(user) / u8p::PathOf(b.name)), ec);
        }
    }
    return "backed up the saves as " + name;
}

inline bool Delete(const fs::path& user, const std::string& name) {
    if (name.empty() || name.find_first_of("\\/") != std::string::npos || name[0] == '.') return false;
    std::error_code ec;
    return fs::remove_all(Long(BackupsFolder(user) / u8p::PathOf(name)), ec) > 0 && !ec;
}

// ---- putting one back: asked for now, done at the next start ----

inline std::string Pending(const fs::path& user) {
    std::ifstream in(BackupsFolder(user) / kPendingFile);
    std::string name;
    std::getline(in, name);
    while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
    return name;
}

// Why a backup folder must not take the place of the saves ("" when it may). One without saves, or not whole, would leave the
// player with less than before.
inline std::string WhyNotRestorable(const fs::path& from) {
    std::error_code ec;
    if (!fs::is_directory(Long(from), ec)) return "it is not there any more";
    const Tree has = MeasureSaves(from);
    if (has.files == 0) return "it has no saves in it";
    std::ifstream complete(Long(from / ".complete"));
    if (complete) {
        Tree said;
        if (!(complete >> said.files >> said.bytes) || !SameSaves(said, has)) return "some of its files are missing or have changed";
        return {};
    }
    // Made by an older version: it wrote ".signature" when the copy had ended. A folder the player made by hand has another
    // kind of name and is taken as it is.
    const std::string name = u8p::Utf8(from.filename());
    const bool ours = name.size() >= 15 && name[8] == '-' && name.substr(0, 8).find_first_not_of("0123456789") == std::string::npos &&
                      name.substr(9, 6).find_first_not_of("0123456789") == std::string::npos;
    if (ours && !fs::exists(Long(from / ".signature"), ec)) return "its copy was not finished";
    return {};
}

inline bool StageRestore(const fs::path& user, const std::string& name) {
    std::error_code ec;
    if (name.empty() || name[0] == '.' || name.find_first_of("\\/") != std::string::npos ||
        !WhyNotRestorable(BackupsFolder(user) / u8p::PathOf(name)).empty()) {
        return false;
    }
    fs::create_directories(BackupsFolder(user), ec);
    std::ofstream out(BackupsFolder(user) / kPendingFile, std::ios::trunc);
    out << name << "\n";
    return static_cast<bool>(out);
}

inline void CancelRestore(const fs::path& user) {
    std::error_code ec;
    fs::remove(BackupsFolder(user) / kPendingFile, ec);
}

// ---- one game at a time on a user folder ----
// Everything here (and the game's own writing) takes the saves to be its alone: a second game on the same folder would take the
// first one's half-done restore for one that was cut short, and remove it. The game holds <user>\.in-use.lock open, shared
// with nobody, for as long as it runs; Windows lets it go however the program ends. It is a file, so two spellings of one
// folder meet in the same lock.
inline const char* const kUserLock = ".in-use.lock";

class UserLock {
public:
    UserLock() = default;
    UserLock(const UserLock&) = delete;
    UserLock& operator=(const UserLock&) = delete;
    ~UserLock() { Release(); }

    // False only when another program holds the folder, after waiting `wait_ms` for it (a game that is still closing). A
    // folder where the file cannot be made at all does not keep the game from starting.
    bool Acquire(const fs::path& user, int wait_ms) {
#if defined(_WIN32)
        if (handle_ != INVALID_HANDLE_VALUE) return true;
        std::error_code ec;
        fs::create_directories(Long(user), ec);
        const std::wstring file = Long(user / kUserLock).wstring();
        for (int waited = 0;; waited += 100) {
            handle_ = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle_ != INVALID_HANDLE_VALUE || GetLastError() != ERROR_SHARING_VIOLATION) return true;
            if (waited >= wait_ms) return false;
            Sleep(100);
        }
#else
        (void)user;
        (void)wait_ms;
        return true;
#endif
    }

    void Release() {
#if defined(_WIN32)
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
#endif
    }

private:
#if defined(_WIN32)
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#endif
};

// A restore never works on the saves themselves until the new ones are whole beside them:
//   1. the backup is copied to <user>\.restore-new and checked; the saves are untouched, and a failure ends here
//   2. each save folder is moved to <user>\.restore-old and the new one moved into its place (moves, not copies)
//   3. when all are in place, .restore-old is renamed to .restore-trash (the restore counts from here) and removed
// A failure in 2 moves the old ones back. So does the next start, when the program ended in the middle of it: .restore-old
// holds "plan.txt" (written before the first move), the list of the save folders there were.
inline const char* const kRestoreNew = ".restore-new";
inline const char* const kRestoreOld = ".restore-old";
inline const char* const kRestoreTrash = ".restore-trash";
inline const char* const kRestorePlan = "plan.txt";
// What an undo takes out of a save folder's place is moved here, never removed: it is normally the backup's copy, but after an
// older version of the program (an update undone) it can be what the player has saved since.
inline const char* const kRestoreAside = ".restore-discarded";

// True while .restore-old still says how to put the saves back: they may then be part one thing and part another. The game must
// not run on them: it would write over them, and the undo still to come removes what is in the old ones' place.
inline bool RestoreUnfinished(const fs::path& user) {
    std::error_code ec;
    const fs::path old = user / kRestoreOld;
    if (fs::exists(Long(old / kRestorePlan), ec)) return true;
    for (const char* f : kFolders) {
        if (fs::exists(Long(old / f), ec)) return true;
    }
    return false;
}

// Undoes a restore that did not finish, and removes what a finished one left. Returns "" when there was nothing to undo.
inline std::string RecoverRestore(const fs::path& user) {
    std::error_code ec;
    fs::remove_all(Long(user / kRestoreTrash), ec);
    const fs::path old = user / kRestoreOld;
    std::string result;
    if (fs::is_directory(Long(old), ec)) {
        const std::string stuck = "an interrupted restore could not be undone: your saves of before it are in " + u8p::Utf8(old) +
                                  " and in the 'before-restore' backup";
        std::ifstream plan(Long(old / kRestorePlan));
        if (!plan) {  // the plan comes before the first move: without it nothing was moved
            if (fs::exists(Long(old / kRestorePlan), ec)) return stuck;  // it is there: another program holds it
            for (const char* f : kFolders) {
                if (fs::exists(Long(old / f), ec)) return stuck;
            }
        } else {
            // moves what is in a save folder's place aside (the one of an earlier undo goes). False: it is held.
            const auto put_aside = [&](const char* f) {
                std::error_code ae;
                if (!fs::exists(Long(user / f), ae)) return true;
                const fs::path aside = user / kRestoreAside / f;
                fs::remove_all(Long(aside), ae);
                if (ae) return false;
                fs::create_directories(Long(aside.parent_path()), ae);
                if (ae) return false;
                fs::rename(Long(user / f), Long(aside), ae);
                return !ae;
            };
            std::vector<std::string> had;
            for (std::string line; std::getline(plan, line);) had.push_back(line);
            plan.close();
            for (const char* f : kFolders) {
                if (std::find(had.begin(), had.end(), f) != had.end()) {
                    if (!fs::exists(Long(old / f), ec)) continue;  // not moved yet: the one in place is the old one
                    if (!put_aside(f)) return stuck;               // the new one
                    fs::rename(Long(old / f), Long(user / f), ec);
                    if (ec) return stuck;
                } else if (!put_aside(f)) {  // there was none before: what is there came from the backup
                    return stuck;
                }
            }
            fs::remove(Long(old / kRestorePlan), ec);  // the undo counts from here: no later start goes through it again
            if (ec) return stuck;
            result = "an unfinished restore was undone: your saves are as they were before it";
        }
        fs::remove_all(Long(old), ec);
    }
    fs::remove_all(Long(user / kRestoreNew), ec);
    return result;
}

// The three steps above. "" when the saves of `from` are in place; else why not (the saves are then as they were).
inline std::string ReplaceSaves(const fs::path& user, const fs::path& from) {
    std::error_code ec;
    const fs::path fresh = user / kRestoreNew, old = user / kRestoreOld, trash = user / kRestoreTrash;
    if (fs::exists(Long(old), ec) || fs::exists(Long(fresh), ec)) return "what an earlier restore left in the user folder is in the way";
    const auto drop_fresh = [&] {
        std::error_code re;
        fs::remove_all(Long(fresh), re);
    };
    fs::create_directories(Long(fresh), ec);
    for (const char* f : kFolders) {
        if (!fs::is_directory(Long(from / f), ec)) continue;
        std::string why;
        if (!CopyFolder(from / f, fresh / f, &why)) {
            drop_fresh();
            return why;
        }
    }
    if (!SameSaves(MeasureSaves(fresh), MeasureSaves(from))) {
        drop_fresh();
        return "the copy of the backup came out incomplete (is the disk full?)";
    }
    fs::create_directories(Long(old), ec);
    {
        std::ofstream plan(Long(old / "plan.tmp"), std::ios::trunc);
        for (const char* f : kFolders) {
            if (fs::exists(Long(user / f), ec)) plan << f << "\n";
        }
        plan.flush();
        const bool written = static_cast<bool>(plan);
        plan.close();
        if (written) fs::rename(Long(old / "plan.tmp"), Long(old / kRestorePlan), ec);
        if (!written || ec) {
            fs::remove_all(Long(old), ec);
            drop_fresh();
            return "the user folder cannot be written";
        }
    }
    const auto undo = [&](const std::string& why) {
        const std::string undone = RecoverRestore(user);
        return undone.rfind("an unfinished", 0) == 0 ? why : why + "; " + undone;
    };
    for (const char* f : kFolders) {
        if (fs::exists(Long(user / f), ec)) {
            fs::rename(Long(user / f), Long(old / f), ec);
            if (ec) return undo(std::string("the folder ") + f + " is in use by another program (" + ec.message() + ")");
        }
        if (fs::exists(Long(fresh / f), ec)) {
            fs::rename(Long(fresh / f), Long(user / f), ec);
            if (ec) return undo(std::string("the new ") + f + " folder could not be put in place (" + ec.message() + ")");
        }
    }
    fs::rename(Long(old), Long(trash), ec);
    if (ec) return undo("the restore could not be finished (" + ec.message() + ")");
    fs::remove_all(Long(trash), ec);
    drop_fresh();
    return {};
}

// Before the game starts: an unfinished restore is undone; then, if one was asked for, a backup of the saves now
// ("before-restore") and the saves of the chosen backup take their place. Returns what it did ("" when nothing).
inline std::string ApplyPendingRestore(const fs::path& user) {
    const std::string recovered = RecoverRestore(user);
    if (RestoreUnfinished(user)) return recovered;  // nothing else is done with the saves like that: what was asked for waits
    const std::string before = recovered.empty() ? std::string() : recovered + "; ";
    const std::string name = Pending(user);
    if (name.empty()) return recovered;
    const fs::path from = BackupsFolder(user) / u8p::PathOf(name);
    CancelRestore(user);  // (once: a failure does not repeat at every start)
    if (const std::string why = WhyNotRestorable(from); !why.empty()) {
        return before + "the backup " + name + " was not restored, " + why + ": your saves are as they were";
    }
    std::string error;
    std::string safety;
    if (MeasureSaves(user).files != 0) {
        safety = MakeBackup(user, "before-restore", &error);
        if (safety.empty()) return before + "the saves now could not be backed up (" + error + "): nothing was restored";
    }
    if (const std::string why = ReplaceSaves(user, from); !why.empty()) {
        return before + "restoring failed: " + why + (RestoreUnfinished(user) ? "" : ". Your saves were not replaced") +
               (safety.empty() ? "" : " (there is a copy of the saves of before: " + safety + ")");
    }
    return before + "restored the saves of " + name + (safety.empty() ? "" : " (the ones there were are in the backup " + safety + ")");
}

}  // namespace savemgr
