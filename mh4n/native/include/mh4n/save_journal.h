// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
//
// The 3DS's save data counts from the game's commit (FS:ControlArchive): cut before it, the console keeps the save as last
// committed. The host's files are written in place, and MH4U saves by emptying a file (SetSize 0), writing it whole and then
// committing: cut in between, the save was left empty. So the committed save is kept aside from the first change to the commit:
//   <user>\.save-journal\data      the save folder as it was last committed
//   <user>\.save-journal\root.txt  which folder that is, relative to <user>
// The journal is there only while there are changes not committed. The file system makes it (Begin, before the first change)
// and drops it (Commit); the program puts the committed save back before a game starts (Recover, with the user folder taken
// for itself and before any backup is made), keeping what it takes out in <user>\.save-discarded: nothing is destroyed.
#pragma once

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace mh4n::save_journal {

namespace fs = std::filesystem;

inline fs::path Journal(const fs::path& user) { return user / ".save-journal"; }
inline fs::path Partial(const fs::path& user) { return user / ".save-journal.partial"; }  // being made: not whole yet
inline fs::path Done(const fs::path& user) { return user / ".save-journal.done"; }        // dropped: being removed
inline fs::path Discarded(const fs::path& user) { return user / ".save-discarded"; }

// Paths past 260 characters need the \\?\ prefix, which takes ".." as a name: resolved first.
inline fs::path Long(const fs::path& p) {
#if defined(_WIN32)
    std::error_code ec;
    fs::path abs = fs::absolute(p, ec);
    if (ec) return p;
    abs = abs.lexically_normal();
    const std::wstring w = abs.make_preferred().wstring();
    if (w.rfind(LR"(\\?\)", 0) == 0 || w.size() < 2 || w[1] != L':') return abs;
    return fs::path(std::wstring(LR"(\\?\)") + w);
#else
    return p;
#endif
}

// What was written to `file` is on the disk, not only in the system's memory.
inline bool Sync(const fs::path& file) {
#if defined(_WIN32)
    FILE* f = _wfopen(Long(file).c_str(), L"r+b");
    if (!f) return false;
    const bool ok = _commit(_fileno(f)) == 0;
#else
    FILE* f = std::fopen(file.c_str(), "r+b");
    if (!f) return false;
    const bool ok = fsync(fileno(f)) == 0;
#endif
    return std::fclose(f) == 0 && ok;
}

// A rename that is on the disk when it returns. Another program (an antivirus) can hold a file for a moment: tried again.
inline std::error_code Move(const fs::path& from, const fs::path& to) {
    std::error_code ec;
#if defined(_WIN32)
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (MoveFileExW(Long(from).c_str(), Long(to).c_str(), MOVEFILE_WRITE_THROUGH)) return {};
        ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        Sleep(25);
    }
#else
    fs::rename(from, to, ec);
#endif
    return ec;
}

// Before the first change after a commit: `root` (a folder inside `user`) as it is now is put aside. Nothing to do when it
// already was. On an error nothing must be changed: there would be no way back.
inline std::error_code Begin(const fs::path& user, const fs::path& root) {
    std::error_code ec;
    if (fs::exists(Long(Journal(user)), ec)) return {};
    const fs::path partial = Partial(user);
    fs::remove_all(Long(partial), ec);
    if (ec) return ec;
    fs::create_directories(Long(partial / "data"), ec);
    if (ec) return ec;
    const fs::path from = Long(root);
    for (fs::recursive_directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path to = partial / "data" / it->path().lexically_relative(from);
        if (it->is_directory(ec)) {
            fs::create_directories(Long(to), ec);
        } else if (!ec) {
            fs::copy_file(it->path(), Long(to), ec);
            if (!ec && !Sync(to)) ec = std::make_error_code(std::errc::io_error);
        }
        if (ec) break;
    }
    if (ec) return ec;
    const fs::path from_root = fs::absolute(root, ec);
    if (ec) return ec;
    const fs::path from_user = fs::absolute(user, ec);
    if (ec) return ec;
    const fs::path relative = from_root.lexically_normal().lexically_relative(from_user.lexically_normal());
    const std::u8string name = relative.generic_u8string();
    {
        std::ofstream note(Long(partial / "root.txt"), std::ios::binary);
        note.write(reinterpret_cast<const char*>(name.data()), static_cast<std::streamsize>(name.size()));
        note.flush();
        if (!note) return std::make_error_code(std::errc::io_error);
    }
    if (!Sync(partial / "root.txt")) return std::make_error_code(std::errc::io_error);
    return Move(partial, Journal(user));  // from here the way back is whole
}

// The game committed (its files flushed first): the changes count. One rename; what it leaves is removed after.
inline std::error_code Commit(const fs::path& user) {
    std::error_code ec;
    if (!fs::exists(Long(Journal(user)), ec)) return {};
    fs::remove_all(Long(Done(user)), ec);
    if (const std::error_code moved = Move(Journal(user), Done(user))) return moved;
    fs::remove_all(Long(Done(user)), ec);
    return {};
}

// Whether two folders hold the same files with the same bytes.
inline bool SameFolder(const fs::path& a, const fs::path& b) {
    const auto inside = [](const fs::path& one, const fs::path& other) {
        std::error_code ec;
        const fs::path from = Long(one);
        for (fs::recursive_directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
            const fs::path match = Long(other / it->path().lexically_relative(from));
            if (it->is_directory(ec)) {
                if (!fs::is_directory(match, ec)) return false;
                continue;
            }
            std::ifstream x(it->path(), std::ios::binary), y(match, std::ios::binary);
            if (!x || !y) return false;
            if (std::string(std::istreambuf_iterator<char>(x), {}) != std::string(std::istreambuf_iterator<char>(y), {})) return false;
        }
        return !ec;
    };
    return inside(a, b) && inside(b, a);
}

// A journal can outlive its cut: an older version of the program (an update undone) knows nothing of it, and the player goes
// on playing and saving. Whether the save in `root` is such later progress: every file the committed save has is there and
// none is empty, and one was written long after the journal was made (`note`). A save cut short is written within moments.
inline bool WrittenLongAfter(const fs::path& root, const fs::path& committed, const fs::path& note) {
    constexpr auto kLong = std::chrono::minutes(5);
    std::error_code ec;
    const auto made = fs::last_write_time(Long(note), ec);
    if (ec) return false;
    bool later = false;
    const fs::path from = Long(committed);
    for (fs::recursive_directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec)) continue;
        const fs::path live = Long(root / it->path().lexically_relative(from));
        std::error_code fe;
        if (!fs::is_regular_file(live, fe) || fs::file_size(live, fe) == 0 || fe) return false;
        const auto written = fs::last_write_time(live, fe);
        if (fe) return false;
        if (written - made > kLong) later = true;
    }
    return !ec && later;
}

struct Recovery {
    std::string message;  // for the log; "" when there was nothing to say
    bool stuck = false;   // the committed save could not be put back: the game must not run on what is there
};

// Before a game starts (nothing else using the user folder): a save cut short is put back as the game last committed it.
// Safe to cut at any point: the next start goes on from there.
inline Recovery Recover(const fs::path& user) {
    std::error_code ec;
    fs::remove_all(Long(Partial(user)), ec);  // (a journal not finished: nothing was changed yet)
    fs::remove_all(Long(Done(user)), ec);
    const fs::path journal = Journal(user);
    if (!fs::exists(Long(journal), ec)) return {};
    const auto stuck = [&](const std::string& why) {
        const std::u8string at = journal.u8string();
        return Recovery{"a save of the game that was cut short could not be put back yet (" + why + "): the save as the game last finished it is in " +
                            std::string(at.begin(), at.end()),
                        true};
    };
    std::string name;
    {
        std::ifstream note(Long(journal / "root.txt"), std::ios::binary);
        if (!note) return stuck("its note cannot be read");
        name.assign(std::istreambuf_iterator<char>(note), {});
    }
    fs::path relative;
    try {  // (a damaged note must not end the program: it is not acted on)
        relative = fs::path(std::u8string(name.begin(), name.end()));
    } catch (const std::exception&) {
        return stuck("its note is damaged");
    }
    if (name.find('\0') != std::string::npos || relative.empty() || relative.is_absolute()) return stuck("its note names no folder");
    for (const fs::path& part : relative) {
        if (part == "..") return stuck("its note names a folder outside the user folder");
    }
    const fs::path root = user / relative, data = journal / "data";
    std::string result;
    if (fs::is_directory(Long(data), ec) && WrittenLongAfter(root, data, journal / "root.txt")) {
        // later progress: the save stays as it is, the older copy is kept aside
        fs::remove_all(Long(Discarded(user)), ec);
        if (ec) return stuck("an older discarded save cannot be removed: " + ec.message());
        if (const std::error_code moved = Move(data, Discarded(user))) return stuck(moved.message());
        const std::u8string at = Discarded(user).u8string();
        result = "a copy left by an old cut was found, but the save was written long after it: the save stays as it is (the old copy is in " +
                 std::string(at.begin(), at.end()) + ")";
    } else if (fs::is_directory(Long(data), ec)) {
        const bool there = fs::exists(Long(root), ec);
        if (!there || !SameFolder(root, data)) {
            if (there) {
                fs::remove_all(Long(Discarded(user)), ec);
                if (ec) return stuck("an older discarded save cannot be removed: " + ec.message());
                if (const std::error_code moved = Move(root, Discarded(user))) return stuck(moved.message());
            }
            fs::create_directories(Long(root.parent_path()), ec);
            if (const std::error_code moved = Move(data, root)) return stuck(moved.message());
            const std::u8string at = Discarded(user).u8string();
            result = "a save of the game was cut short: the save is back as the game last finished it (what was there is in " +
                     std::string(at.begin(), at.end()) + ")";
        }
    }
    if (const std::error_code dropped = Commit(user)) return stuck(dropped.message());
    return {result, false};
}

}  // namespace mh4n::save_journal
