// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: 3dbrew Filesystem Services, boot-village.ipc.trace and game/user_test.

#pragma once

#include <array>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "mh4n/types.h"

namespace mh4n::wp30 {

namespace fs = std::filesystem;

inline constexpr Result Missing = 0xC8804470;
inline constexpr Result ArchiveNotMounted = 0xC8804465;
inline constexpr Result UnknownArchive = 0xC8804478;
inline constexpr Result MissingExtdata = 0xC8A04478;
inline constexpr Result MissingParent = 0xC8804471;
inline constexpr Result AlreadyExists = 0xC82044B4;
inline constexpr Result DirectoryExists = 0xC82044B9;
inline constexpr Result InvalidPath = 0xE0E046BE;
inline constexpr Result Permission = 0xC92044E6;
inline constexpr Result UnsupportedOpenFlags = 0xE0C046F8;
inline constexpr Result InvalidArgument = 0xD9004676;
inline constexpr Result IoError = 0xC960454B;
inline constexpr Result NotFormatted = 0xC8A04554;

inline constexpr u32 ReadFlag = 1;
inline constexpr u32 WriteFlag = 2;
inline constexpr u32 CreateFlag = 4;

enum class PathType : u32 { Empty = 1, Binary = 2, Ascii = 3, Utf16 = 4 };

struct LowPath {
    PathType type{};
    std::span<const u8> bytes;

    bool IsBinary(std::size_t size) const;
    bool IsEmpty() const;
    std::optional<fs::path> Relative() const;
};

u32 Load32(std::span<const u8> bytes, std::size_t offset);
u64 Load64(std::span<const u8> bytes, std::size_t offset);
void Store32(std::span<u8> bytes, std::size_t offset, u32 word);
std::string Hex32(u32 word, bool uppercase = false);

class HostFile {
public:
    static std::shared_ptr<HostFile> Open(const fs::path& path, bool write);
    static Result Create(const fs::path& path, u64 size);

    ~HostFile();
    std::optional<u64> Size() const;
    Result Read(u64 offset, std::span<u8> bytes, u32& count);
    Result Write(u64 offset, std::span<const u8> bytes, u32& count, bool flush);
    Result Resize(u64 size);
    Result Flush();

private:
    explicit HostFile(std::FILE* file) : file_(file) {}
    std::FILE* file_;
};

struct FileData {
    std::shared_ptr<HostFile> file;
    std::shared_ptr<const std::vector<u8>> memory;
    u64 base = 0;
    std::optional<u64> length;

    explicit operator bool() const { return file || memory; }
    std::optional<u64> Size() const;
    Result Read(u64 offset, std::span<u8> bytes, u32& count) const;
};

enum class ArchiveId : u32 { SelfNcch = 3, Save = 4, Ext = 6, SharedExt = 7, Ncch = 0x2345678A };

struct Archive {
    ArchiveId id{};
    fs::path root;
    fs::path metadata;
    u64 title = 0;

    bool IsContent() const { return id == ArchiveId::SelfNcch || id == ArchiveId::Ncch; }
};

struct FormatInfo {
    u32 size;
    u32 directories;
    u32 files;
    bool duplicate;
};

// Each entry on the wire is 0x228 bytes (3dbrew DirectoryEntry and seq=298, 935).
using DirEntry = std::array<u8, 0x228>;

Result DescribeArchive(const fs::path& user, u64 title, ArchiveId id, LowPath low, Archive& archive);
Result CheckArchive(const Archive& archive);
Result ResolvePath(const fs::path& user, const Archive& archive, LowPath low, fs::path& target);
Result OpenSaveFile(const fs::path& target, u32 flags, FileData& data);
Result ReadDirectory(const fs::path& target, std::vector<DirEntry>& entries);
Result CreateSaveDirectory(const fs::path& target);
Result DeleteSaveFile(const fs::path& target);
Result ReadFormatInfo(const Archive& archive, FormatInfo& info);

}  // namespace mh4n::wp30
