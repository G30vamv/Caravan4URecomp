// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by
// khorne-hub (https://github.com/khorne-hub/4u-native); see ADDITIONAL-TERMS and THIRD_PARTY_NOTICES.
//
// Bringing in saves from elsewhere: another emulator's user folder, a backup made on a console, loose files. The files are
// found by their names and sizes wherever they are under the chosen folder:
//   a hunter save: "system" and "user1".."user3" together. "system" says which hunters there are, so it is required (without
//       it the game offers a new hunter) and the set takes the place of the whole save folder.
//   the extra data (guild cards, quests, Palicoes): "card1".."card3", "cardbox", "otomo1", "quest1".."quest5": files of
//       their own, which replace those of the same names. Where there is no extra data yet, all of them are needed.
// Nothing is changed while a game runs: what is chosen is copied to <user>\.import-pending and put in at the next start,
// over the saves as they are then, by the same safe steps as restoring a backup (a backup of the saves now first).
// Only files of this game's sizes are taken (the USA version's, the one this program runs).
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "save_manager.h"
#include "utf8_path.h"

namespace saveimport {

namespace fs = std::filesystem;

inline const char* const kPending = ".import-pending";
inline const char* const kHunter = "hunter";
inline const char* const kExtra = "extra";

inline fs::path Card() {  // the emulated SD card's folder of this console, in the user folder
    const char* id = "00000000000000000000000000000000";
    return fs::path("sdmc") / "Nintendo 3DS" / id / id;
}
inline fs::path HunterDir(const fs::path& user) { return user / Card() / "title" / "00040000" / "00126300" / "data" / "00000001"; }
inline fs::path ExtraRoot(const fs::path& user) { return user / Card() / "extdata" / "00000000" / "00001263"; }
inline fs::path ExtraDir(const fs::path& user) { return ExtraRoot(user) / "user"; }

// The size a save file of this name has (0: not the name of one), and which kind it belongs to.
inline std::uintmax_t SizeOf(const std::string& name, bool* hunter) {
    *hunter = true;
    if (name == "system") return 512;
    if (name == "user1" || name == "user2" || name == "user3") return 81408;
    *hunter = false;
    if (name == "card1" || name == "card2" || name == "card3") return 494592;
    if (name == "cardbox") return 270336;
    if (name == "otomo1") return 8704;
    if (name.size() == 6 && name.compare(0, 5, "quest") == 0 && name[5] >= '1' && name[5] <= '5') return 328704;
    return 0;
}
constexpr std::size_t kExtraFiles = 10;

struct Set {
    bool hunter = true;
    fs::path dir;                    // where the files are
    std::vector<std::string> files;  // their names, sorted
    fs::file_time_type newest{};
    bool Has(const char* name) const { return std::find(files.begin(), files.end(), name) != files.end(); }
    bool Complete() const { return hunter ? Has("system") : files.size() == kExtraFiles; }
};

struct Found {
    std::vector<Set> sets;  // the newest first
    int odd = 0;            // files with a save's name and another size
    bool cut = false;       // the search stopped at its limit
};

// The saves under `root`. `skip` leaves folders out. The search is bounded: a whole drive chosen by mistake ends it.
inline Found Scan(const fs::path& root, const std::function<bool(const fs::path&)>& skip = {}, int most = 40000, int deepest = 12) {
    Found found;
    std::vector<std::pair<fs::path, int>> folders = {{root, 0}};
    int seen = 0;
    while (!folders.empty() && !found.cut) {
        const auto [folder, depth] = folders.back();
        folders.pop_back();
        Set hunter, extra;
        extra.hunter = false;
        hunter.dir = extra.dir = folder;
        std::error_code ec;
        for (fs::directory_iterator it(savemgr::Long(folder), fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (++seen > most) {
                found.cut = true;
                break;
            }
            std::error_code fe;
            const fs::path name = it->path().filename();
            if (it->is_symlink(fe)) continue;
            if (it->is_directory(fe)) {
                const fs::path child = folder / name;
                if (depth < deepest && !(skip && skip(child))) folders.push_back({child, depth + 1});
                continue;
            }
            bool is_hunter = true;
            const std::string text = u8p::Utf8(name);
            const std::uintmax_t size = SizeOf(text, &is_hunter);
            if (size == 0 || !it->is_regular_file(fe)) continue;
            if (it->file_size(fe) != size || fe) {
                ++found.odd;
                continue;
            }
            Set& set = is_hunter ? hunter : extra;
            set.files.push_back(text);
            const auto time = it->last_write_time(fe);
            if (!fe && (set.files.size() == 1 || time > set.newest)) set.newest = time;
        }
        for (Set* set : {&hunter, &extra}) {
            std::sort(set->files.begin(), set->files.end());
            // (a lone "system" is no hunter's save: other things are called so)
            const bool worth = set->hunter ? (set->Has("user1") || set->Has("user2") || set->Has("user3")) : !set->files.empty();
            if (worth) found.sets.push_back(std::move(*set));
        }
    }
    std::stable_sort(found.sets.begin(), found.sets.end(), [](const Set& a, const Set& b) { return a.newest > b.newest; });
    return found;
}

inline bool HasHunter(const fs::path& user) {
    std::error_code ec;
    for (const char* name : {"user1", "user2", "user3"}) {
        if (fs::is_regular_file(savemgr::Long(HunterDir(user) / name), ec)) return true;
    }
    return false;
}
inline bool HasExtra(const fs::path& user) {
    std::error_code ec;
    return fs::is_directory(savemgr::Long(ExtraDir(user)), ec);
}

// Why a set cannot be brought into this user folder: "" it can, "no-system" or "incomplete".
inline std::string WhyNot(const fs::path& user, const Set& set) {
    if (set.hunter) return set.Complete() ? "" : "no-system";
    return set.Complete() || HasExtra(user) ? "" : "incomplete";
}

inline std::vector<std::string> Pending(const fs::path& user) {  // the kinds waiting: "hunter", "extra"
    std::vector<std::string> kinds;
    std::error_code ec;
    for (const char* kind : {kHunter, kExtra}) {
        if (fs::is_directory(savemgr::Long(user / kPending / kind), ec)) kinds.push_back(kind);
    }
    return kinds;
}

inline void Cancel(const fs::path& user) {
    std::error_code ec;
    fs::remove_all(savemgr::Long(user / kPending), ec);
}

// Copies a set's files aside, to go in at the next start (the folder they came from can then go). False and *error: nothing
// is waiting that was not before.
inline bool Stage(const fs::path& user, const Set& set, std::string* error) {
    if (const std::string why = WhyNot(user, set); !why.empty()) {
        if (error) *error = why;
        return false;
    }
    const fs::path kind = user / kPending / (set.hunter ? kHunter : kExtra);
    const fs::path partial = user / kPending / (set.hunter ? ".hunter.partial" : ".extra.partial");
    std::error_code ec;
    const auto give_up = [&](const std::string& why) {
        std::error_code re;
        fs::remove_all(savemgr::Long(partial), re);
        if (error) *error = why;
        return false;
    };
    fs::remove_all(savemgr::Long(partial), ec);
    fs::create_directories(savemgr::Long(partial), ec);
    if (ec) return give_up(ec.message());
    std::vector<std::string> names = set.files;
    for (const std::string& name : names) {
        fs::copy_file(savemgr::Long(set.dir / u8p::PathOf(name)), savemgr::Long(partial / u8p::PathOf(name)), ec);
        if (ec) return give_up(ec.message());
        bool hunter = true;
        if (fs::file_size(savemgr::Long(partial / u8p::PathOf(name)), ec) != SizeOf(name, &hunter) || ec)
            return give_up("a file changed while it was being copied");
    }
    if (!set.hunter) {  // the icon beside the extra data's folder, when it is there
        std::error_code ie;
        const fs::path icon = set.dir.parent_path() / "icon";
        if (fs::is_regular_file(savemgr::Long(icon), ie) && fs::file_size(savemgr::Long(icon), ie) <= 0x10000)
            fs::copy_file(savemgr::Long(icon), savemgr::Long(partial / ".icon"), ie);
    }
    fs::remove_all(savemgr::Long(kind), ec);
    if (ec) return give_up(ec.message());
    fs::rename(savemgr::Long(partial), savemgr::Long(kind), ec);
    return ec ? give_up(ec.message()) : true;
}

// The 16 bytes the game's own format leaves beside each folder (size, directories, files, duplicate), written when the saves
// have none.
inline bool WriteFormat(const fs::path& file, const std::array<unsigned char, 16>& bytes) {
    std::error_code ec;
    if (fs::exists(savemgr::Long(file), ec)) return true;
    std::ofstream out(savemgr::Long(file), std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.flush();
    return static_cast<bool>(out);
}

// What is waiting goes into `saves` (a copy of the saves, not the ones in use).
inline bool PutInto(const fs::path& user, const fs::path& saves, std::string* error) {
    std::error_code ec;
    for (const std::string& kind : Pending(user)) {
        const bool hunter = kind == kHunter;
        const fs::path from = user / kPending / kind;
        const fs::path to = hunter ? HunterDir(saves) : ExtraDir(saves);
        if (hunter) fs::remove_all(savemgr::Long(to), ec);  // (the set is the whole folder)
        fs::create_directories(savemgr::Long(to), ec);
        if (ec) {
            *error = ec.message();
            return false;
        }
        for (fs::directory_iterator it(savemgr::Long(from), ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = u8p::Utf8(it->path().filename());
            bool ignored = true;
            if (SizeOf(name, &ignored) == 0) continue;
            std::error_code ce;
            fs::copy_file(it->path(), savemgr::Long(to / u8p::PathOf(name)), fs::copy_options::overwrite_existing, ce);
            if (ce) {
                *error = ce.message();
                return false;
            }
        }
        if (ec) {
            *error = ec.message();
            return false;
        }
        const bool formatted =
            hunter ? WriteFormat(to.parent_path() / "00000001.metadata", {0, 0, 4, 0, 0, 0, 0, 0, 4, 0, 0, 0, 1, 0, 0, 0})
                   : WriteFormat(ExtraRoot(saves) / "metadata", {0xff, 0xff, 0xff, 0xff, 0x40, 0, 0, 0, 0x40, 0, 0, 0, 0, 0, 0, 0});
        if (!formatted) {
            *error = "the folder could not be written";
            return false;
        }
        if (!hunter && fs::is_regular_file(savemgr::Long(from / ".icon"), ec) && !fs::exists(savemgr::Long(ExtraRoot(saves) / "icon"), ec))
            fs::copy_file(savemgr::Long(from / ".icon"), savemgr::Long(ExtraRoot(saves) / "icon"), ec);
    }
    return true;
}

// Before the game starts, after any restore (savemgr::ApplyPendingRestore): what is waiting is made into a backup of its own
// ("imported": the saves as they are with what was chosen put in) and that backup is restored. Returns what it did ("" when
// nothing was waiting). Asked once: a failure does not repeat at every start.
inline std::string ApplyPending(const fs::path& user) {
    if (Pending(user).empty() || savemgr::RestoreUnfinished(user)) return {};
    std::string error;
    const std::string name = savemgr::NewBackup(
        user, "imported",
        [&](const fs::path& to, std::uint64_t* signature, std::string* why) {
            if (!savemgr::CopySaves(user, to, why) || !PutInto(user, to, why)) return false;
            *signature = savemgr::MeasureSaves(to).signature;
            return true;
        },
        &error);
    if (name.empty() || !savemgr::StageRestore(user, name)) {
        Cancel(user);
        return "the saves to import could not be put together" + (error.empty() ? std::string() : " (" + error + ")") +
               ": your saves are as they were";
    }
    Cancel(user);  // (from here the restore that waits carries them)
    return "importing: " + savemgr::ApplyPendingRestore(user);
}

}  // namespace saveimport
