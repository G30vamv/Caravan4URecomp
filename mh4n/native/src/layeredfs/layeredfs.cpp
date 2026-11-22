// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: 3dbrew RomFS, layeredfs.h, positioned_file.h and layeredfs_test.cpp.

#include "layeredfs.h"
#include "patch.h"

#include "../common/utf8.h"
#include "mh4n/log.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <deque>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_set>
#include <variant>

namespace mh4n::layeredfs {
namespace {

namespace host = std::filesystem;
using Bytes = std::vector<u8>;

constexpr u32 kEnd = 0xFFFFFFFF;
constexpr u32 kHeaderSize = 0x28;
constexpr u32 kDirectoryRecord = 0x18;
constexpr u32 kFileRecord = 0x20;
constexpr u64 kMaxOffset = std::numeric_limits<u64>::max();
constexpr char kLog[] = "LayeredFS";

u64 ReadLe(const u8* data, unsigned width = 4) {
    u64 value = 0;
    for (unsigned i = 0; i < width; ++i) {
        value |= u64{data[i]} << (8 * i);
    }
    return value;
}

void WriteLe(u8* data, u64 value, unsigned width = 4) {
    for (unsigned i = 0; i < width; ++i) {
        data[i] = static_cast<u8>(value >> (8 * i));
    }
}

class FileSlice final : public BaseReader {
public:
    FileSlice(std::shared_ptr<fs::PositionedFile> file, u64 offset, u64 size)
        : file_(std::move(file)), offset_(offset), size_(size) {}

    std::size_t ReadAt(u64 offset, void* buffer, std::size_t length) override {
        if (offset >= size_) {
            return 0;
        }
        return file_->ReadAt(offset_ + offset, buffer, static_cast<std::size_t>(std::min<u64>(length, size_ - offset)));
    }
    u64 Size() const override { return size_; }

private:
    std::shared_ptr<fs::PositionedFile> file_;
    u64 offset_;
    u64 size_;
};

struct HostFile {
    explicit HostFile(std::string name) : path(std::move(name)) {}

    std::size_t Read(u64 offset, u8* buffer, std::size_t size) {
        std::call_once(opened, [&] { file = fs::PositionedFile::Open(path, nullptr); });
        return file ? file->ReadAt(offset, buffer, size) : 0;
    }

    std::string path;
    std::once_flag opened;
    std::shared_ptr<fs::PositionedFile> file;
};

using Source = std::variant<u64, std::shared_ptr<HostFile>, Bytes>;

struct File {
    std::string name;
    u64 size;
    Source source;
    u32 record = kEnd;
};

struct Directory {
    std::string name;
    std::size_t parent = 0;
    std::vector<std::size_t> children;
    std::vector<File> files;
    u32 record = kEnd;
};

using Tree = std::deque<Directory>;

std::size_t ReadSource(BaseReader& base, const Source& source, u64 offset, std::span<u8> output) {
    if (const auto* original = std::get_if<u64>(&source)) {
        return base.ReadAt(*original + offset, output.data(), output.size());
    }
    if (const auto* file = std::get_if<std::shared_ptr<HostFile>>(&source)) {
        return (*file)->Read(offset, output.data(), output.size());
    }
    const auto& bytes = std::get<Bytes>(source);
    if (!output.empty()) {
        std::memcpy(output.data(), bytes.data() + offset, output.size());
    }
    return output.size();
}

std::optional<std::string> ReadName(const Bytes& table, u32 at, u32 fixed) {
    if ((at & 3) != 0 || at > table.size() || fixed > table.size() - at) {
        return std::nullopt;
    }
    const u8* entry = table.data() + at;
    const auto length = static_cast<std::size_t>(ReadLe(entry + fixed - 4));
    if ((length & 1) != 0 || length > table.size() - at - fixed) {
        return std::nullopt;
    }
    std::u16string name;
    name.reserve(length / 2);
    for (std::size_t i = 0; i < length; i += 2) {
        name.push_back(static_cast<char16_t>(ReadLe(entry + fixed + i, 2)));
    }
    return utf8::FromUtf16(name);
}

bool ParseTree(BaseReader& base, Tree& tree) {
    std::array<u8, kHeaderSize> header{};
    if (base.ReadAt(0, header.data(), header.size()) != header.size() ||
        ReadLe(header.data()) != kHeaderSize) {
        return false;
    }
    const u64 data_start = ReadLe(header.data() + 0x24);
    if (data_start < kHeaderSize || data_start > base.Size()) {
        return false;
    }

    const auto table_range = [&](unsigned field) {
        const u64 offset = ReadLe(header.data() + field);
        const u64 length = ReadLe(header.data() + field + 4);
        return std::pair{offset, length};
    };
    u64 end = kHeaderSize;
    for (const unsigned field : {0x04, 0x0C, 0x14, 0x1C}) {
        const auto [offset, length] = table_range(field);
        if (offset < end || offset + length > data_start || ((offset | length) & 3) != 0) {
            return false;
        }
        end = offset + length;
    }

    const auto read_table = [&](unsigned field, Bytes& table) {
        const auto [offset, length] = table_range(field);
        table.resize(static_cast<std::size_t>(length));
        return table.empty() || base.ReadAt(offset, table.data(), table.size()) == table.size();
    };
    Bytes directories, files;
    if (!read_table(0x0C, directories) || !read_table(0x1C, files)) {
        return false;
    }

    if (const auto root = ReadName(directories, 0, kDirectoryRecord); !root || !root->empty()) {
        return false;
    }
    std::vector<u32> directory_records{0};
    std::unordered_set<u32> visited_dirs{0}, visited_files;
    tree.emplace_back();
    for (std::size_t index = 0; index < tree.size(); ++index) {
        auto& directory = tree[index];
        const u32 dir_at = directory_records[index];
        const u8* entry = directories.data() + dir_at;
        auto child = static_cast<u32>(ReadLe(entry + 8));
        while (child != kEnd) {
            auto name = ReadName(directories, child, kDirectoryRecord);
            if (!name || !visited_dirs.insert(child).second ||
                ReadLe(directories.data() + child) != dir_at) {
                return false;
            }
            directory.children.push_back(tree.size());
            directory_records.push_back(child);
            auto& added = tree.emplace_back();
            added.name = std::move(*name);
            added.parent = index;
            child = static_cast<u32>(ReadLe(directories.data() + child + 4));
        }
        auto file = static_cast<u32>(ReadLe(entry + 12));
        while (file != kEnd) {
            auto name = ReadName(files, file, kFileRecord);
            if (!name || !visited_files.insert(file).second || ReadLe(files.data() + file) != dir_at) {
                return false;
            }
            const u8* record = files.data() + file;
            const u64 start = ReadLe(record + 8, 8);
            const u64 size = ReadLe(record + 16, 8);
            if (start > kMaxOffset - data_start || size > kMaxOffset - data_start - start) {
                return false;
            }
            directory.files.push_back({std::move(*name), size, data_start + start});
            file = static_cast<u32>(ReadLe(record + 4));
        }
    }
    return true;
}

char FoldAscii(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}

template <class Range, class Name>
auto FindName(Range& entries, std::string_view name, Name name_of) {
    auto match = entries.end();
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        const std::string_view candidate = name_of(*it);
        if (candidate == name) {
            return it;
        }
        if (match == entries.end() &&
            std::equal(candidate.begin(), candidate.end(), name.begin(), name.end(),
                       [](char a, char b) { return FoldAscii(a) == FoldAscii(b); })) {
            match = it;
        }
    }
    return match;
}

std::vector<host::directory_entry> List(const host::path& path) {
    std::vector<host::directory_entry> entries;
    std::error_code error;
    for (auto it = host::directory_iterator(path, error); !error && it != host::directory_iterator{};
         it.increment(error)) {
        entries.push_back(*it);
    }
    std::sort(entries.begin(), entries.end(), [](const auto& first, const auto& second) {
        return utf8::PathToUtf8(first.path().filename()) < utf8::PathToUtf8(second.path().filename());
    });
    return entries;
}

void ApplyPatch(BaseReader& base, File& file, const host::path& path, bool ips) {
    const auto name = utf8::PathToUtf8(path);
    const auto patch_file = fs::PositionedFile::Open(name, nullptr);
    Bytes patch_bytes, contents;
    if (!patch_file || patch_file->Size() > patch_bytes.max_size() || file.size > contents.max_size()) {
        MH4N_LOG(Warning, kLog, "Cannot read patch {}", name);
        return;
    }
    patch_bytes.resize(static_cast<std::size_t>(patch_file->Size()));
    if (!patch_bytes.empty() && patch_file->ReadAt(0, patch_bytes.data(), patch_bytes.size()) != patch_bytes.size()) {
        MH4N_LOG(Warning, kLog, "Cannot read patch {}", name);
        return;
    }
    contents.resize(static_cast<std::size_t>(file.size));
    // Unavailable bytes in the original image are zero-filled, just as on a normal read.
    ReadSource(base, file.source, 0, contents);
    const auto status = ips ? patch::ApplyIps(patch_bytes, contents) : patch::ApplyBps(patch_bytes, contents);
    if (status == patch::Status::Success) {
        file.size = contents.size();
        file.source = std::move(contents);
        MH4N_LOG(Debug, kLog, "Patched {}", name);
    } else {
        MH4N_LOG(Warning, kLog, "Could not apply patch {}", name);
    }
}

void ApplyLayer(BaseReader& base, Tree& tree, const std::string& path, bool extension) {
    if (path.empty()) {
        return;
    }
    std::vector<std::pair<std::size_t, host::path>> pending{{0, utf8::PathFromUtf8(path)}};
    while (!pending.empty()) {
        auto [index, folder] = std::move(pending.back());
        pending.pop_back();
        const auto previous_count = pending.size();
        auto& directory = tree[index];
        for (const auto& entry : List(folder)) {
            auto name = utf8::PathToUtf8(entry.path().filename());
            std::error_code error;
            const auto status = entry.symlink_status(error);
            if (host::is_directory(status)) {
                auto it = FindName(directory.children, name, [&](std::size_t i) -> const std::string& {
                    return tree[i].name;
                });
                if (it != directory.children.end()) {
                    pending.emplace_back(*it, entry.path());
                } else if (!extension) {
                    const auto child = tree.size();
                    directory.children.push_back(child);
                    Directory& added = tree.emplace_back();
                    added.name = std::move(name);
                    added.parent = index;
                    pending.emplace_back(child, entry.path());
                }
                continue;
            }
            if (!host::is_regular_file(status)) {
                continue;
            }
            std::string suffix;
            if (extension) {
                if (name.ends_with(".stub")) {
                    suffix = ".stub";
                } else if (name.size() > 4) {
                    suffix = name.substr(name.size() - 4);
                } else {
                    continue;
                }
                name.resize(name.size() - suffix.size());
            }
            auto& files = directory.files;
            const auto it = FindName(files, name, [](const File& file) -> const std::string& { return file.name; });
            if (!extension) {
                const auto size = entry.file_size(error);
                if (error) {
                    continue;
                }
                auto source = std::make_shared<HostFile>(utf8::PathToUtf8(entry.path()));
                if (it == files.end()) {
                    files.push_back({std::move(name), size, std::move(source)});
                } else {
                    it->size = size;
                    it->source = std::move(source);
                }
                MH4N_LOG(Debug, kLog, "Overlay file {}", utf8::PathToUtf8(entry.path()));
            } else if (it != files.end()) {
                if (suffix == ".stub") {
                    files.erase(it);
                } else {
                    ApplyPatch(base, *it, entry.path(), suffix == ".ips");
                }
            }
        }
        // Children were added in ascending host name order. Process that order even on a case-sensitive host
        // where two host directories can resolve to the same directory in the image.
        std::reverse(pending.begin() + static_cast<std::ptrdiff_t>(previous_count), pending.end());
    }
}

struct Extent {
    u64 offset;
    u64 size;
    Source source;
};

class Image final : public fs::FileBackend {
public:
    Image(std::shared_ptr<BaseReader> base, Bytes metadata, std::vector<Extent> extents, u64 size)
        : base_(std::move(base)), metadata_(std::move(metadata)), extents_(std::move(extents)), size_(size) {}

    u64 GetSize() override { return size_; }

    Result Read(u64 offset, std::size_t length, u8* buffer, std::size_t* read) override {
        *read = 0;
        if (offset >= size_ || length == 0) {
            return kResultSuccess;
        }
        *read = static_cast<std::size_t>(std::min<u64>(length, size_ - offset));
        std::memset(buffer, 0, *read);
        const u64 end = offset + *read;
        if (offset < metadata_.size()) {
            const auto count = static_cast<std::size_t>(std::min<u64>(end, metadata_.size()) - offset);
            std::memcpy(buffer, metadata_.data() + offset, count);
        }
        auto it = std::lower_bound(extents_.begin(), extents_.end(), offset,
                                   [](const Extent& extent, u64 pos) { return extent.offset + extent.size <= pos; });
        for (; it != extents_.end() && it->offset < end; ++it) {
            const u64 start = std::max(offset, it->offset);
            const auto size = static_cast<std::size_t>(std::min(end, it->offset + it->size) - start);
            ReadSource(*base_, it->source, start - it->offset, {buffer + (start - offset), size});
        }
        return kResultSuccess;
    }

private:
    std::shared_ptr<BaseReader> base_;
    Bytes metadata_;
    std::vector<Extent> extents_;
    u64 size_;
};

u64 BucketCount(u64 count) {
    if (count < 19) {
        return std::max<u64>(3, count | 1);
    }
    for (;; ++count) {
        constexpr std::array factors{2, 3, 5, 7, 11, 13, 17};
        if (std::none_of(factors.begin(), factors.end(), [=](int factor) { return count % factor == 0; })) {
            return count;
        }
    }
}

bool AssignRecord(u32& offset, u64& total, u32 fixed, std::size_t units) {
    const u64 size = (fixed + u64{units} * 2 + 3) & ~u64{3};
    if (total + size > kEnd) {
        return false;
    }
    offset = static_cast<u32>(total);
    total += size;
    return true;
}

void WriteNameAndHash(u8* record, u32 fixed, const std::u16string& name, u32 parent, u32 offset,
                      std::span<u8> table) {
    u32 hash = parent ^ 123456789u;
    for (std::size_t i = 0; i < name.size(); ++i) {
        hash = std::rotr(hash, 5) ^ name[i];
        WriteLe(record + fixed + 2 * i, name[i], 2);
    }
    u8* bucket = table.data() + 4 * (hash % (table.size() / 4));
    WriteLe(record + fixed - 8, ReadLe(bucket));
    WriteLe(record + fixed - 4, name.size() * 2);
    WriteLe(bucket, offset);
}

std::shared_ptr<fs::FileBackend> Serialize(std::shared_ptr<BaseReader> base, Tree& tree) {
    std::vector<std::size_t> order, pending{0};
    u64 dm_size = 0, fm_size = 0, file_count = 0;
    while (!pending.empty()) {
        const auto index = pending.back();
        pending.pop_back();
        order.push_back(index);
        auto& directory = tree[index];
        if (!AssignRecord(directory.record, dm_size, kDirectoryRecord, utf8::ToUtf16(directory.name).size())) {
            return nullptr;
        }
        for (auto& file : directory.files) {
            if (!AssignRecord(file.record, fm_size, kFileRecord, utf8::ToUtf16(file.name).size())) {
                return nullptr;
            }
            ++file_count;
        }
        pending.insert(pending.end(), directory.children.rbegin(), directory.children.rend());
    }

    const u64 dh_size = BucketCount(order.size()) * 4;
    const u64 fh_size = BucketCount(file_count) * 4;
    const u64 dm_at = kHeaderSize + dh_size;
    const u64 fh_at = dm_at + dm_size;
    const u64 fm_at = fh_at + fh_size;
    const u64 data_at = (fm_at + fm_size + 15) & ~u64{15};
    if (data_at > kEnd) {
        return nullptr;
    }

    Bytes metadata(static_cast<std::size_t>(data_at));
    const std::array<u64, 10> header{kHeaderSize, kHeaderSize, dh_size, dm_at, dm_size, fh_at, fh_size,
                                      fm_at, fm_size, data_at};
    for (std::size_t i = 0; i < header.size(); ++i) {
        WriteLe(metadata.data() + 4 * i, header[i]);
    }
    std::span<u8> dir_hash(metadata.data() + kHeaderSize, static_cast<std::size_t>(dh_size));
    std::span<u8> file_hash(metadata.data() + fh_at, static_cast<std::size_t>(fh_size));
    std::fill(dir_hash.begin(), dir_hash.end(), 0xFF);
    std::fill(file_hash.begin(), file_hash.end(), 0xFF);

    std::vector<Extent> extents;
    u64 cursor = data_at;
    WriteLe(metadata.data() + dm_at + 4, kEnd);  // root sibling
    for (const auto index : order) {
        auto& directory = tree[index];
        const u32 parent = tree[directory.parent].record;
        u8* record = metadata.data() + dm_at + directory.record;
        WriteLe(record, parent);
        WriteLe(record + 8, directory.children.empty() ? kEnd : tree[directory.children.front()].record);
        WriteLe(record + 12, directory.files.empty() ? kEnd : directory.files.front().record);
        WriteNameAndHash(record, kDirectoryRecord, utf8::ToUtf16(directory.name), parent, directory.record, dir_hash);

        for (std::size_t i = 0; i < directory.children.size(); ++i) {
            const u32 child = tree[directory.children[i]].record;
            const u32 next = i + 1 < directory.children.size() ? tree[directory.children[i + 1]].record : kEnd;
            WriteLe(metadata.data() + dm_at + child + 4, next);
        }
        for (std::size_t i = 0; i < directory.files.size(); ++i) {
            auto& file = directory.files[i];
            if (file.size > kMaxOffset - cursor - 15) {
                return nullptr;
            }
            u8* item = metadata.data() + fm_at + file.record;
            const u32 next = i + 1 < directory.files.size() ? directory.files[i + 1].record : kEnd;
            WriteLe(item, directory.record);
            WriteLe(item + 4, next);
            WriteLe(item + 8, cursor - data_at, 8);
            WriteLe(item + 16, file.size, 8);
            WriteNameAndHash(item, kFileRecord, utf8::ToUtf16(file.name), directory.record, file.record, file_hash);
            if (file.size != 0) {
                extents.push_back({cursor, file.size, std::move(file.source)});
            }
            cursor += (file.size + 15) & ~u64{15};
        }
    }
    MH4N_LOG(Info, kLog, "Built image with {} directories and {} files", order.size(), file_count);
    return std::make_shared<Image>(std::move(base), std::move(metadata), std::move(extents), cursor);
}

}  // namespace

std::shared_ptr<BaseReader> MakeBaseReader(std::shared_ptr<fs::PositionedFile> file, u64 offset, u64 size) {
    if (!file || offset > file->Size()) {
        return nullptr;
    }
    size = std::min(size, file->Size() - offset);
    return std::make_shared<FileSlice>(std::move(file), offset, size);
}

std::shared_ptr<fs::FileBackend> Build(std::shared_ptr<BaseReader> base, const std::vector<Layer>& layers,
                                       std::string* error) {
    Tree tree;
    if (!base || !ParseTree(*base, tree)) {
        if (error) {
            *error = "invalid or truncated RomFS metadata";
        }
        return nullptr;
    }
    for (const auto& layer : layers) {
        ApplyLayer(*base, tree, layer.romfs_dir, false);
        ApplyLayer(*base, tree, layer.romfs_ext_dir, true);
    }
    auto image = Serialize(std::move(base), tree);
    if (!image && error) {
        *error = "layered RomFS exceeds the format's size limits";
    }
    return image;
}

std::shared_ptr<fs::FileBackend> Build(std::shared_ptr<BaseReader> base, const std::string& romfs_dir,
                                       const std::string& romfs_ext_dir, std::string* error) {
    return Build(std::move(base), std::vector<Layer>{{romfs_dir, romfs_ext_dir}}, error);
}

std::shared_ptr<fs::FileBackend> Build(std::shared_ptr<fs::PositionedFile> base, u64 offset, u64 size,
                                       const std::vector<Layer>& layers, std::string* error) {
    return Build(MakeBaseReader(std::move(base), offset, size), layers, error);
}

std::shared_ptr<fs::FileBackend> Build(std::shared_ptr<fs::PositionedFile> base, u64 offset, u64 size,
                                       const std::string& romfs_dir, const std::string& romfs_ext_dir,
                                       std::string* error) {
    return Build(MakeBaseReader(std::move(base), offset, size), romfs_dir, romfs_ext_dir, error);
}

}  // namespace mh4n::layeredfs
