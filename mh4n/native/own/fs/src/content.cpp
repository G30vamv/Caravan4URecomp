// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: 3dbrew Filesystem Services, NCCH, CIA, ExeFS and RomFS, boot-village.ipc.trace and observed NGWord IPC metadata.

#include "content.h"

#include <algorithm>
#include <bit>
#include <fstream>
#include <string_view>

#include "mh4n/kernel.h"

namespace mh4n::wp30 {
namespace {

constexpr u64 NgWordTitle = 0x000400DB00010302;
constexpr u32 NoEntry = 0xFFFFFFFF;

u64 Align(u64 value, u64 alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

bool ReadExact(HostFile& file, u64 offset, std::span<u8> data) {
    u32 count = 0;
    return file.Read(offset, data, count) == kResultSuccess && count == data.size();
}

FileData Slice(const FileData& source, u64 offset, u64 size) {
    if (!source.length || offset > *source.length || size > *source.length - offset)
        return {};
    return {source.file, {}, source.base + offset, size};
}

FileData Whole(const fs::path& path) {
    std::error_code ec;
    if (path.empty() || !fs::is_regular_file(path, ec))
        return {};
    auto file = HostFile::Open(path, false);
    if (!file)
        return {};
    const auto size = file->Size();
    if (!size)
        return {};
    return {std::move(file), {}, 0, size};
}

// The CIA contains aligned header, certificate, ticket and TMD sections before content index zero.
u64 CiaContentOffset(HostFile& file) {
    std::array<u8, 0x20> header{};
    if (!ReadExact(file, 0, header) || Load32(header, 0) < 0x20)
        return 0;
    return Align(Load32(header, 0), 64) + Align(Load32(header, 8), 64) +
           Align(Load32(header, 12), 64) + Align(Load32(header, 16), 64);
}

struct Ncch {
    FileData romfs;
    FileData exefs;
    u64 title = 0;
};

Ncch ReadNcch(const fs::path& path) {
    const auto whole = Whole(path);
    if (!whole)
        return {};
    const u64 start = path.extension() == ".cia" ? CiaContentOffset(*whole.file) : 0;
    std::array<u8, 0x200> header{};
    if ((path.extension() == ".cia" && start == 0) || !ReadExact(*whole.file, start, header) ||
        Load32(header, 0x100) != 0x4843434E || !(header[0x18F] & 4) || header[0x18E] > 7)
        return {};
    const u64 unit = 0x200ull << header[0x18E];
    const auto content = Slice(whole, start, unit * Load32(header, 0x104));
    if (!content)
        return {};
    return {Slice(content, unit * Load32(header, 0x1B0), unit * Load32(header, 0x1B4)),
            Slice(content, unit * Load32(header, 0x1A0), unit * Load32(header, 0x1A4)),
            Load64(header, 0x118)};
}

fs::path GameSource(const fs::path& game, bool update) {
    std::ifstream manifest(game / "source.txt");
    const std::string_view key = update ? "update=" : "base=";
    for (std::string line; std::getline(manifest, line);) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.starts_with(key)) {
            const fs::path source(std::u8string(line.begin() + key.size(), line.end()));
            return source.is_absolute() ? source : game / source;
        }
    }
    return {};
}

FileData LevelThree(const FileData& romfs) {
    if (!romfs.length || *romfs.length < 0x28)
        return {};
    std::array<u8, 0x5C> header{};
    u32 count = 0;
    if (romfs.Read(0, header, count) != kResultSuccess || count < 0x28)
        return {};
    if (Load32(header, 0) == 0x28)
        return romfs;  // An already extracted level-3 image.
    const auto block_log = Load32(header, 0x4C);
    if (count != header.size() || Load32(header, 0) != 0x43465649 ||
        Load32(header, 4) != 0x10000 || block_log < 5 || block_log > 24)
        return {};
    const u64 master_end = 0x60ull + Load32(header, 8);
    return Slice(romfs, Align(master_end, 1ull << block_log), Load64(header, 0x44));
}

FileData ExeFile(const FileData& exefs, std::string_view name) {
    if (!exefs.length || *exefs.length < 0x200)
        return {};
    std::array<u8, 0x200> header{};
    u32 count = 0;
    if (exefs.Read(0, header, count) != kResultSuccess || count != header.size())
        return {};
    for (std::size_t i = 0; i < 10; ++i) {
        const auto entry = std::span(header).subspan(i * 16, 16);
        const auto end = std::find(entry.begin(), entry.begin() + 8, 0);
        const std::string_view entry_name(reinterpret_cast<const char*>(entry.data()),
                                          static_cast<std::size_t>(end - entry.begin()));
        if (entry_name == name)
            return Slice(exefs, 0x200ull + Load32(entry, 8), Load32(entry, 12));
    }
    return {};
}

FileData GameRomfs(const fs::path& game, bool update) {
    if (game.empty())
        return {};
    auto data = LevelThree(Whole(game / (update ? "romfs_update.bin" : "romfs.bin")));
    if (data)
        return data;
    return LevelThree(ReadNcch(GameSource(game, update)).romfs);
}

FileData GameExeFile(const fs::path& game, std::span<const u8> low) {
    const auto end = std::ranges::find(low, 0);
    const std::string_view name(reinterpret_cast<const char*>(low.data()),
                                static_cast<std::size_t>(end - low.begin()));
    if (game.empty() || (name != "icon" && name != "banner" && name != "logo"))
        return {};
    auto loose_file = Whole(game / (std::string(name) + ".bin"));
    if (loose_file)
        return loose_file;
    auto data = ExeFile(Whole(game / "exefs.bin"), name);
    if (data)
        return data;
    return ExeFile(ReadNcch(GameSource(game, false)).exefs, name);
}

// Installed system data is preferred. The firmware-less fallback exposes the observed
// NGWord filenames and the same level-3 layout with empty lists of our own.
FileData EmptyWordLists() {
    std::vector<std::u16string> names;
    for (unsigned i = 0; i < 17; ++i) {
        const auto number = std::to_string(i);
        names.emplace_back(number.begin(), number.end());
        names.back() += u".txt";
    }
    std::ranges::sort(names);
    names.emplace_back(u"version.dat");
    constexpr u32 header_size = 0x28;
    constexpr u32 dir_buckets = 3;
    constexpr u32 file_buckets = 19;
    constexpr u32 root_size = 24;
    constexpr u32 dir_hash = header_size;
    constexpr u32 dir_entries = dir_hash + 4 * dir_buckets;
    constexpr u32 file_hash = dir_entries + root_size;
    constexpr u32 file_entries = file_hash + 4 * file_buckets;
    u32 file_table_size = 0;
    for (const auto& name : names)
        file_table_size += 32 + static_cast<u32>(Align(name.size() * 2, 4));
    const auto data_offset = static_cast<u32>(Align(file_entries + file_table_size, 16));
    // One four-byte format version follows the lists.
    auto image = std::make_shared<std::vector<u8>>(data_offset + 4);
    auto bytes = std::span(*image);
    const std::array<u32, 10> header{header_size, dir_hash, 4 * dir_buckets,
        dir_entries, root_size, file_hash, 4 * file_buckets, file_entries,
        file_table_size, data_offset};
    for (std::size_t i = 0; i < header.size(); ++i)
        Store32(bytes, i * 4, header[i]);
    std::ranges::fill(bytes.subspan(dir_hash, dir_buckets * 4), 0xFF);
    std::ranges::fill(bytes.subspan(file_hash, file_buckets * 4), 0xFF);
    constexpr u32 salt = 123456789;
    Store32(bytes, dir_hash + 4 * (salt % dir_buckets), 0);
    Store32(bytes, dir_entries + 4, NoEntry);  // Sibling.
    Store32(bytes, dir_entries + 8, NoEntry);  // First subdirectory.
    Store32(bytes, dir_entries + 16, NoEntry);  // Hash chain.
    u32 record = 0;
    for (const auto& name : names) {
        const u32 at = file_entries + record;
        const auto name_bytes = static_cast<u32>(name.size() * 2);
        const auto next = record + 32 + static_cast<u32>(Align(name_bytes, 4));
        const bool version = next == file_table_size;
        Store32(bytes, at + 4, version ? NoEntry : next);
        Store32(bytes, at + 0x10, version ? 4 : 0);
        Store32(bytes, at + 0x1C, name_bytes);
        u32 hash = salt;
        for (std::size_t i = 0; i < name.size(); ++i) {
            bytes[at + 0x20 + i * 2] = static_cast<u8>(name[i]);
            hash = std::rotr(hash, 5) ^ name[i];
        }
        const u32 bucket = file_hash + 4 * (hash % file_buckets);
        Store32(bytes, at + 0x18, Load32(bytes, bucket));
        Store32(bytes, bucket, record);
        record = next;
    }
    return {{}, image, 0, image->size()};
}

FileData NgWordRomfs(const fs::path& user) {
    if (!user.empty()) {
        const auto dir = user / "nand" / "title" / "000400db" / "00010302" / "content";
        std::error_code ec;
        for (fs::directory_iterator entry(dir, ec), end; !ec && entry != end; entry.increment(ec)) {
            if (entry->path().extension() != ".app")
                continue;
            auto ncch = ReadNcch(entry->path());
            if (ncch.title == NgWordTitle) {
                auto data = LevelThree(ncch.romfs);
                if (data)
                    return data;
            }
        }
    }
    return EmptyWordLists();
}

}  // namespace

u64 GameTitle(const fs::path& game) {
    if (game.empty())
        return 0;
    const auto exheader = Whole(game / "exheader.bin");
    std::array<u8, 8> id{};
    if (exheader && ReadExact(*exheader.file, 0x200, id))
        return Load64(id, 0);
    return ReadNcch(GameSource(game, false)).title;
}

Result OpenContent(const fs::path& game, const fs::path& user, const Archive& archive,
                   LowPath path, FileData& data) {
    if (archive.id == ArchiveId::SelfNcch && path.IsBinary(12)) {
        const u32 type = Load32(path.bytes, 0);
        if (type == 0 || type == 5)
            data = GameRomfs(game, type == 5);
        else if (type == 2)
            data = GameExeFile(game, path.bytes.subspan(4));
        else
            return InvalidPath;
    } else if (archive.id == ArchiveId::Ncch && path.IsBinary(20)) {
        if (Load32(path.bytes, 0) != 0 || Load32(path.bytes, 4) != 0)
            return InvalidPath;
        const u32 section = Load32(path.bytes, 8);
        if (archive.title == NgWordTitle && section == 0)
            data = NgWordRomfs(user);
        else if (archive.title == GameTitle(game)) {
            if (section == 0)
                data = GameRomfs(game, false);
            else if (section == 2)
                data = GameExeFile(game, path.bytes.subspan(12));
        }
    } else {
        return InvalidPath;
    }
    return data ? kResultSuccess : Missing;
}

}  // namespace mh4n::wp30
