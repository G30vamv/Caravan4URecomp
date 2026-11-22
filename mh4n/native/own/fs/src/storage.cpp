// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: 3dbrew Filesystem Services, boot-village.ipc.trace and the game/user_test directory layout.

#include "storage.h"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <string_view>

#include "mh4n/kernel.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace mh4n::wp30 {
namespace {

constexpr char ConsoleId[] = "00000000000000000000000000000000";

Result HostError(const std::error_code& ec, Result missing = Missing) {
    if (ec == std::errc::permission_denied || ec == std::errc::read_only_file_system)
        return Permission;
    if (ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory)
        return missing;
    if (ec == std::errc::file_exists)
        return AlreadyExists;
    if (ec == std::errc::no_space_on_device)
        return 0xC86044D2; // 3dbrew FSPXI: partition full.
    if (ec == std::errc::file_too_large)
        return 0xE0E046D1; // 3dbrew FSPXI: exceeds the file size limit.
    return IoError;
}

std::FILE* OpenStream(const fs::path& path, const char* mode) {
#ifdef _WIN32
    std::wstring wide_mode;
    for (const char* letter = mode; *letter; ++letter)
        wide_mode += static_cast<wchar_t>(*letter);
    return _wfopen(path.c_str(), wide_mode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

bool Seek(std::FILE* file, u64 offset, int origin) {
    if (offset > static_cast<u64>(std::numeric_limits<s64>::max()))
        return false;
#ifdef _WIN32
    return _fseeki64(file, static_cast<s64>(offset), origin) == 0;
#else
    return fseeko(file, static_cast<off_t>(offset), origin) == 0;
#endif
}

bool AllowedComponent(std::u16string_view part) {
    if (part.empty() || part == u"." || part.back() == u'.' || part.back() == u' ')
        return false;
    if (std::any_of(part.begin(), part.end(), [](char16_t c) {
            return c < 32 || std::u16string_view(u"\\:\"<>|?*").find(c) != std::u16string_view::npos;
        }))
        return false;
    auto stem = part.substr(0, part.find(u'.'));
    std::u16string upper(stem);
    for (char16_t& c : upper) {
        if (c >= u'a' && c <= u'z')
            c -= u'a' - u'A';
    }
    if (upper == u"CON" || upper == u"PRN" || upper == u"AUX" || upper == u"NUL")
        return false;
    return !(upper.size() == 4 &&
             (upper.starts_with(u"COM") || upper.starts_with(u"LPT")) &&
             std::u16string_view(u"0123456789\u00b9\u00b2\u00b3").find(upper.back()) != std::u16string_view::npos);
}

bool Confined(const fs::path& root, const fs::path& path) {
    std::error_code ec;
    const auto base = fs::weakly_canonical(root, ec);
    if (ec)
        return false;
    const auto candidate = fs::weakly_canonical(path, ec);
    return !ec && std::ranges::mismatch(base, candidate).in1 == base.end();
}

void CopyShortName(std::span<u8> field, std::u16string_view name) {
    std::ranges::fill(field, ' ');
    for (std::size_t i = 0; i < std::min(field.size(), name.size()); ++i) {
        const char16_t c = name[i];
        if (c >= u'a' && c <= u'z')
            field[i] = static_cast<u8>(c - u'a' + 'A');
        else if (c > u' ' && c < 127)
            field[i] = static_cast<u8>(c);
        else
            field[i] = '_';
    }
}

DirEntry EncodeEntry(const std::u16string& name, bool directory, u64 size) {
    DirEntry bytes{};
    for (std::size_t i = 0; i < std::min<std::size_t>(name.size(), 0x105); ++i) {
        bytes[2 * i] = static_cast<u8>(name[i]);
        bytes[2 * i + 1] = static_cast<u8>(name[i] >> 8);
    }
    const auto dot = name.rfind(u'.');
    auto wire = std::span(bytes);
    CopyShortName(wire.subspan(0x20C, 8), std::u16string_view(name).substr(0, dot));
    CopyShortName(wire.subspan(0x216, 3), dot == name.npos ? std::u16string_view{} :
                                                       std::u16string_view(name).substr(dot + 1));
    // The captured implementation uses space-padded 8.3 fields and leaves byte 0x21a zero.
    bytes[directory ? 0x21C : 0x21E] = 1;
    Store32(wire, 0x220, static_cast<u32>(size));
    Store32(wire, 0x224, static_cast<u32>(size >> 32));
    return bytes;
}

}  // namespace

u32 Load32(std::span<const u8> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4)
        return 0;
    const auto slice = bytes.subspan(offset, 4);
    return static_cast<u32>(slice[0]) | (static_cast<u32>(slice[1]) << 8) |
           (static_cast<u32>(slice[2]) << 16) | (static_cast<u32>(slice[3]) << 24);
}

u64 Load64(std::span<const u8> bytes, std::size_t offset) {
    return Load32(bytes, offset) | (static_cast<u64>(Load32(bytes, offset + 4)) << 32);
}

void Store32(std::span<u8> bytes, std::size_t offset, u32 word) {
    for (unsigned i = 0; i < 4; ++i)
        bytes[offset + i] = static_cast<u8>(word >> (8 * i));
}

std::string Hex32(u32 word, bool uppercase) {
    const std::string_view digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    std::string text(8, '0');
    for (char& c : text) {
        c = digits[word >> 28];
        word <<= 4;
    }
    return text;
}

bool LowPath::IsBinary(std::size_t size) const {
    return type == PathType::Binary && bytes.size() == size;
}

bool LowPath::IsEmpty() const {
    return type == PathType::Empty && bytes.size() == 1;
}

std::optional<fs::path> LowPath::Relative() const {
    std::u16string text;
    if (type == PathType::Ascii && !bytes.empty() && bytes.back() == 0) {
        text.assign(bytes.begin(), bytes.end() - 1);
    } else if (type == PathType::Utf16 && bytes.size() >= 2 && bytes.size() % 2 == 0 &&
               bytes[bytes.size() - 2] == 0 && bytes.back() == 0) {
        for (std::size_t i = 0; i + 2 < bytes.size(); i += 2)
            text += static_cast<char16_t>(bytes[i] | (static_cast<u16>(bytes[i + 1]) << 8));
    } else {
        return std::nullopt;
    }
    if (text.empty() || text.front() != u'/' || text.size() > 0x106)
        return std::nullopt;
    fs::path path;
    for (std::size_t start = 1; start < text.size();) {
        const auto end = text.find(u'/', start);
        const auto part = std::u16string_view(text).substr(start, end - start);
        if (!AllowedComponent(part))
            return std::nullopt;
        try {
            path /= fs::path(part);
        } catch (const std::exception&) {
            return std::nullopt;
        }
        if (end == text.npos)
            break;
        start = end + 1;
    }
    return path;
}

std::shared_ptr<HostFile> HostFile::Open(const fs::path& path, bool write) {
    auto* stream = OpenStream(path, write ? "r+b" : "rb");
    if (!stream)
        return {};
    std::setvbuf(stream, nullptr, _IONBF, 0);
    return std::shared_ptr<HostFile>(new HostFile(stream));
}

Result HostFile::Create(const fs::path& path, u64 size) {
    errno = 0;
    auto* stream = OpenStream(path, "wbx");
    if (!stream)
        return HostError(std::error_code(errno, std::generic_category()), MissingParent);
    Result result;
    {
        HostFile file(stream);
        result = size ? file.Resize(size) : kResultSuccess;
    }
    if (result != kResultSuccess) {
        std::error_code ec;
        fs::remove(path, ec);
    }
    return result;
}

HostFile::~HostFile() {
    std::fclose(file_);
}

std::optional<u64> HostFile::Size() const {
    if (!Seek(file_, 0, SEEK_END))
        return std::nullopt;
#ifdef _WIN32
    const auto size = _ftelli64(file_);
#else
    const auto size = ftello(file_);
#endif
    if (size < 0)
        return std::nullopt;
    return static_cast<u64>(size);
}

Result HostFile::Read(u64 offset, std::span<u8> bytes, u32& count) {
    count = 0;
    std::clearerr(file_);
    if (bytes.empty())
        return kResultSuccess;
    if (!Seek(file_, offset, SEEK_SET))
        return IoError;
    count = static_cast<u32>(std::fread(bytes.data(), 1, bytes.size(), file_));
    return std::ferror(file_) ? IoError : kResultSuccess;
}

Result HostFile::Write(u64 offset, std::span<const u8> bytes, u32& count, bool flush) {
    count = 0;
    std::clearerr(file_);
    if (!bytes.empty()) {
        if (!Seek(file_, offset, SEEK_SET))
            return IoError;
        count = static_cast<u32>(std::fwrite(bytes.data(), 1, bytes.size(), file_));
        if (count != bytes.size())
            return HostError(std::error_code(errno, std::generic_category()));
    }
    return flush ? Flush() : kResultSuccess;
}

Result HostFile::Resize(u64 size) {
    if (size > static_cast<u64>(std::numeric_limits<s64>::max()))
        return InvalidArgument;
#ifdef _WIN32
    const int error = _chsize_s(_fileno(file_), size);
#else
    const int error = ftruncate(fileno(file_), static_cast<off_t>(size)) != 0 ? errno : 0;
#endif
    return error ? HostError(std::error_code(error, std::generic_category())) : kResultSuccess;
}

Result HostFile::Flush() {
    if (std::fflush(file_) != 0)
        return IoError;
#ifdef _WIN32
    return _commit(_fileno(file_)) == 0 ? kResultSuccess : IoError;
#else
    return fsync(fileno(file_)) == 0 ? kResultSuccess : IoError;
#endif
}

std::optional<u64> FileData::Size() const {
    if (length)
        return length;
    return file ? file->Size() : std::nullopt;
}

Result FileData::Read(u64 offset, std::span<u8> bytes, u32& count) const {
    count = 0;
    const auto size = Size();
    if (!size)
        return IoError;
    if (offset >= *size)
        return kResultSuccess;
    bytes = bytes.first(static_cast<std::size_t>(std::min<u64>(bytes.size(), *size - offset)));
    if (memory) {
        std::copy_n(memory->data() + base + offset, bytes.size(), bytes.data());
        count = static_cast<u32>(bytes.size());
        return kResultSuccess;
    }
    return file->Read(base + offset, bytes, count);
}

Result DescribeArchive(const fs::path& user, u64 title, ArchiveId id, LowPath low, Archive& archive) {
    archive.id = id;
    archive.title = title;
    const auto sd = user / "sdmc" / "Nintendo 3DS" / ConsoleId / ConsoleId;
    switch (id) {
    case ArchiveId::SelfNcch:
        return low.IsEmpty() ? kResultSuccess : InvalidPath;
    case ArchiveId::Ncch:
        if (!low.IsBinary(16) || low.bytes[8] > 2)
            return InvalidPath;
        archive.title = Load64(low.bytes, 0);
        return kResultSuccess;
    case ArchiveId::Save:
        if (!low.IsEmpty() || title == 0)
            return InvalidPath;
        archive.root = sd / "title" / Hex32(static_cast<u32>(title >> 32)) /
                       Hex32(static_cast<u32>(title)) / "data" / "00000001";
        archive.metadata = archive.root.parent_path() / "00000001.metadata";
        break;
    case ArchiveId::Ext:
    case ArchiveId::SharedExt: {
        const bool shared = id == ArchiveId::SharedExt;
        if (!low.IsBinary(12) || Load32(low.bytes, 0) != (shared ? 0u : 1u))
            return InvalidPath;
        const auto media = shared ? user / "nand" / "data" / ConsoleId : sd;
        const auto high = shared ? "00048000" : Hex32(Load32(low.bytes, 8), true);
        const auto container = media / "extdata" / high / Hex32(Load32(low.bytes, 4), true);
        archive.root = container / "user";
        archive.metadata = container / "metadata";
        break;
    }
    default:
        return UnknownArchive;
    }
    if (user.empty() || !Confined(user, archive.root) || !Confined(user, archive.metadata))
        return Permission;
    return kResultSuccess;
}

Result CheckArchive(const Archive& archive) {
    if (archive.IsContent())
        return kResultSuccess;
    std::error_code ec;
    if (fs::is_directory(archive.root, ec))
        return kResultSuccess;
    const Result absent = archive.id == ArchiveId::Save ? NotFormatted : MissingExtdata;
    return ec ? HostError(ec, absent) : absent;
}

Result ResolvePath(const fs::path& user, const Archive& archive, LowPath low, fs::path& target) {
    if (archive.IsContent())
        return UnsupportedOpenFlags;
    auto relative = low.Relative();
    if (!relative)
        return InvalidPath;
    target = relative->empty() ? archive.root : archive.root / *relative;
    if (!Confined(user, target) || !Confined(archive.root, target))
        return Permission;
    return kResultSuccess;
}

Result OpenSaveFile(const fs::path& target, u32 flags, FileData& data) {
    std::error_code ec;
    const auto status = fs::status(target, ec);
    if (ec && ec != std::errc::no_such_file_or_directory)
        return HostError(ec);
    if (!fs::exists(status)) {
        if (!(flags & CreateFlag))
            return fs::is_directory(target.parent_path(), ec) ? Missing : MissingParent;
        const Result result = HostFile::Create(target, 0);
        if (result != kResultSuccess && result != AlreadyExists)
            return result;
    } else if (!fs::is_regular_file(status)) {
        return Permission;
    }
    data.file = HostFile::Open(target, (flags & WriteFlag) != 0);
    return data ? kResultSuccess : HostError(std::error_code(errno, std::generic_category()));
}

Result ReadDirectory(const fs::path& target, std::vector<DirEntry>& entries) {
    std::error_code ec;
    std::vector<std::pair<std::u16string, DirEntry>> snapshot;
    for (fs::directory_iterator item(target, ec), end; !ec && item != end; item.increment(ec)) {
        const auto status = item->symlink_status(ec);
        if (ec)
            break;
        if (!fs::is_directory(status) && !fs::is_regular_file(status))
            continue;
        const auto size = fs::is_directory(status) ? 0 : item->file_size(ec);
        if (ec)
            break;
        const auto name = item->path().filename().u16string();
        snapshot.emplace_back(name, EncodeEntry(name, fs::is_directory(status), size));
    }
    if (ec)
        return HostError(ec, MissingParent);
    std::ranges::sort(snapshot, {}, &decltype(snapshot)::value_type::first);
    entries.reserve(snapshot.size());
    for (const auto& entry : snapshot)
        entries.push_back(entry.second);
    return kResultSuccess;
}

Result CreateSaveDirectory(const fs::path& target) {
    std::error_code ec;
    if (fs::create_directory(target, ec))
        return kResultSuccess;
    return ec ? HostError(ec, MissingParent) : DirectoryExists;
}

Result DeleteSaveFile(const fs::path& target) {
    std::error_code ec;
    if (!fs::is_regular_file(fs::symlink_status(target, ec)))
        return ec ? HostError(ec) : Missing;
    if (fs::remove(target, ec))
        return kResultSuccess;
    return ec ? HostError(ec) : Missing;
}

Result ReadFormatInfo(const Archive& archive, FormatInfo& info) {
    if (archive.IsContent())
        return Permission;
    auto file = HostFile::Open(archive.metadata, false);
    if (!file)
        return NotFormatted;
    std::array<u8, 16> bytes{};
    u32 read = 0;
    if (file->Read(0, bytes, read) != kResultSuccess || read != bytes.size())
        return IoError;
    info = {Load32(bytes, 0), Load32(bytes, 4), Load32(bytes, 8), bytes[12] != 0};
    return kResultSuccess;
}

}  // namespace mh4n::wp30
