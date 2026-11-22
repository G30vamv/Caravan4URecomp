// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: code_patch.h, layeredfs/patch.h, code_patch_test.cpp and the code_mods.py conflict contract.

#include "code_patch.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string_view>

#include "layeredfs/patch.h"

namespace mh4n::code_patch {
namespace {

namespace fs = std::filesystem;
namespace patch = layeredfs::patch;
using Bytes = std::vector<u8>;
using Range = std::pair<u32, u32>;

constexpr std::array kReplacements{"exefs/code.bin", "code.bin"};
constexpr std::array kPatches{"exefs/code.ips", "exefs/code.bps", "code.ips", "code.bps"};

bool RegularFile(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

const char* FirstPatch(const fs::path& folder) {
    for (auto name : kPatches) {
        std::error_code ec;
        if (fs::exists(folder / name, ec) || (ec && ec != std::errc::no_such_file_or_directory)) {
            return name;
        }
    }
    return nullptr;
}

std::optional<Bytes> Read(const fs::path& path, std::optional<std::size_t> prefix = std::nullopt) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || (prefix && size < *prefix)) {
        return std::nullopt;
    }
    const auto length = prefix ? *prefix : size;
    if (length > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max()) ||
        length > Bytes{}.max_size()) {
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    Bytes bytes(static_cast<std::size_t>(length));
    if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(length))) {
        return std::nullopt;
    }
    return bytes;
}

Outcome PatchError(const std::string& name, const char* detail) {
    return {false, "mods: " + name + ": patch error", detail, {}};
}

Outcome ApplyPatch(const fs::path& folder, const char* name, const std::string& label, Bytes& code) {
    const auto bytes = Read(folder / name);
    if (!bytes) {
        return PatchError(label, "unreadable");
    }
    const auto status = std::string_view(name).ends_with(".ips")
                            ? patch::ApplyIps(*bytes, code)
                            : patch::ApplyBps(*bytes, code);
    if (status != patch::Status::Success) {
        return PatchError(label, status == patch::Status::ErrorPatchesInvalidTitle
                                     ? "source CRC-32 differs" : "malformed");
    }
    Outcome result;
    result.applied.push_back(label);
    return result;
}

std::string Label(const std::string& folder, const char* name) {
    auto dir = fs::path(folder);
    while (!dir.has_filename() && dir.has_relative_path()) {
        dir = dir.parent_path();
    }
    return (dir.filename() / name).generic_string();
}

}  // namespace

u64 ModId(u64 program_id) {
    constexpr u64 category_mask = 0x000000FF00000000ull;
    constexpr u64 update_category = 0x0000000E00000000ull;
    return (program_id & category_mask) == update_category ? program_id & ~update_category : program_id;
}

std::string ModsPath(const std::string& load_dir, u64 program_id) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string title(16, '0');
    u64 id = ModId(program_id);
    for (auto digit = title.rbegin(); digit != title.rend(); ++digit) {
        *digit = digits[id & 0xF];
        id >>= 4;
    }
    return (fs::path(load_dir) / "mods" / title).generic_string();
}

u32 BssPages(u32 bss_size) {
    return static_cast<u32>((u64{bss_size} + kPageSize - 1) / kPageSize);
}

ModFolder::ModFolder(std::string path) : path_(std::move(path)) {}

bool ModFolder::Replace(Bytes& code, std::string* applied) const {
    for (auto name : kReplacements) {
        if (auto bytes = Read(fs::path(path_) / name)) {
            code = std::move(*bytes);
            if (applied) {
                *applied = name;
            }
            return true;
        }
    }
    return false;
}

Outcome ModFolder::Patch(Bytes& code) const {
    if (const auto* name = FirstPatch(path_)) {
        return ApplyPatch(path_, name, name, code);
    }
    return {};
}

Outcome ModFolder::ApplyToCode(Bytes& code, u32 bss_pages) const {
    std::string replacement;
    Replace(code, &replacement);
    code.resize(code.size() + std::size_t{bss_pages} * kPageSize);
    auto outcome = Patch(code);
    if (!replacement.empty()) {
        outcome.applied.insert(outcome.applied.begin(), std::move(replacement));
    }
    return outcome;
}

std::optional<Bytes> ModFolder::ExheaderOverride() const {
    return Read(fs::path(path_) / "exheader.bin", kExheaderSize);
}

std::optional<Bytes> ExheaderOverrideOf(const std::vector<std::string>& folders, std::string* which) {
    for (auto folder = folders.rbegin(); folder != folders.rend(); ++folder) {
        if (auto exheader = ModFolder(*folder).ExheaderOverride()) {
            if (which) {
                *which = *folder;
            }
            return exheader;
        }
    }
    return std::nullopt;
}

Outcome ApplyFolders(const std::vector<std::string>& folders, Bytes& code, u32 bss_pages) {
    Outcome result;
    for (auto folder = folders.rbegin(); folder != folders.rend(); ++folder) {
        std::string replacement;
        if (ModFolder(*folder).Replace(code, &replacement)) {
            result.applied.push_back(Label(*folder, replacement.c_str()));
            break;
        }
    }
    code.resize(code.size() + std::size_t{bss_pages} * kPageSize);
    for (const auto& folder : folders) {
        if (const auto* name = FirstPatch(folder)) {
            auto outcome = ApplyPatch(folder, name, Label(folder, name), code);
            if (!outcome.ok) {
                outcome.applied = std::move(result.applied);
                return outcome;
            }
            result.applied.push_back(std::move(outcome.applied.front()));
        }
    }
    return result;
}

std::vector<Range> IpsRanges(const Bytes& bytes) {
    if (bytes.size() < 8 || std::string_view(reinterpret_cast<const char*>(bytes.data()), 5) != "PATCH") {
        return {};
    }
    std::vector<Range> ranges;
    std::size_t cursor = 5;
    while (bytes.size() - cursor > 3) {
        const auto* record = bytes.data() + cursor;
        if (std::string_view(reinterpret_cast<const char*>(record), 3) == "EOF") {
            break;
        }
        if (bytes.size() - cursor < 5) {
            return {};
        }
        const u32 offset = (u32{record[0]} << 16) | (u32{record[1]} << 8) | record[2];
        u32 length = (u32{record[3]} << 8) | record[4];
        cursor += 5;
        if (length == 0) {
            if (bytes.size() - cursor < 3) {
                return {};
            }
            length = (u32{bytes[cursor]} << 8) | bytes[cursor + 1];
            cursor += 3;
        } else {
            if (length > bytes.size() - cursor) {
                return {};
            }
            cursor += length;
        }
        if (length != 0) {
            ranges.emplace_back(offset, offset + length);
        }
    }
    return ranges;
}

std::vector<CodeConflict> CodeConflicts(const std::vector<std::string>& folders) {
    using Kind = CodeConflict::Kind;
    std::vector<CodeConflict> conflicts;
    std::vector<std::pair<std::size_t, const char*>> patches;
    std::optional<std::size_t> replacement;
    std::optional<std::size_t> exheader;
    for (std::size_t i = 0; i < folders.size(); ++i) {
        const fs::path folder(folders[i]);
        if (std::ranges::any_of(kReplacements, [&](const char* name) { return RegularFile(folder / name); })) {
            if (replacement) {
                conflicts.push_back({Kind::Replaced, *replacement, i});
            }
            replacement = i;
        }
        if (RegularFile(folder / "exheader.bin")) {
            exheader = i;
        }
        if (const auto* name = FirstPatch(folder)) {
            patches.emplace_back(i, name);
        }
    }

    if (replacement) {
        for (const auto& [i, name] : patches) {
            if (i != *replacement) {
                conflicts.push_back({Kind::PatchOnReplacement, i, *replacement});
            }
        }
        if (exheader && *exheader != *replacement) {
            conflicts.push_back({Kind::Exheader, *exheader, *replacement});
        }
    }

    std::vector<std::pair<std::size_t, std::vector<Range>>> ips;
    std::optional<std::size_t> previous_patch;
    for (const auto& [i, name] : patches) {
        if (std::string_view(name).ends_with(".bps")) {
            if (previous_patch) {
                conflicts.push_back({Kind::Bps, i, *previous_patch});
            }
        } else if (auto bytes = Read(fs::path(folders[i]) / name)) {
            auto ranges = IpsRanges(*bytes);
            for (const auto& [other, earlier] : ips) {
                if (std::ranges::any_of(ranges, [&](const Range& range) {
                        return std::ranges::any_of(earlier, [&](const Range& prior) {
                            return range.first < prior.second && prior.first < range.second;
                        });
                    })) {
                    conflicts.push_back({Kind::Overlap, i, other});
                }
            }
            ips.emplace_back(i, std::move(ranges));
        }
        previous_patch = i;
    }
    return conflicts;
}

}  // namespace mh4n::code_patch
