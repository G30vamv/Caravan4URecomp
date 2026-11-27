// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: 3dbrew Filesystem Services, libctru IPC layouts and boot-village.ipc.trace.

#include "content.h"
#include "storage.h"

#include <algorithm>
#include <map>

#include "mh4n/registry.h"
#include "mh4n/save_journal.h"
#include "mh4n/service.h"

namespace mh4n {
namespace {

using namespace wp30;

constexpr auto ServiceName = "fs:USER";
constexpr u64 SaveFreeBytes = 32 * 1024 * 1024;

void ReplyResult(Request& request, Result result) {
    request.MakeReply(1, 0).Push(result);
}

LowPath PopPath(Request& request, PathType type, u32 size) {
    const auto* buffer = request.PopStaticBuffer();
    if (!buffer || buffer->data.size() != size)
        return {};
    return {type, buffer->data};
}

void ReplyObject(Request& request, Result result, std::shared_ptr<ClientSession> session = {}) {
    auto& reply = request.MakeReply(1, 2);
    reply.Push(result);
    reply.PushMoveObjects(std::move(session));
}

template <typename Operation>
Handled DeferIo(Request& request, u64 ticks, Operation operation) {
    auto& kernel = request.GetKernel();
    kernel.ScheduleAt(kernel.NowTicks() + ticks,
                      [pending = request.Share(), operation = std::move(operation)] {
        if (!pending->Cancelled())
            Complete(pending, operation(*pending));
    });
    return Handled::Deferred;
}

u64 TransferTicks(u32 bytes, bool romfs) {
    // Seq=27–32 shows roughly 180k startup ticks and 26 ticks/byte for cartridge reads.
    return romfs ? 180000 + 26ull * bytes : 10500 + 12ull * bytes;
}

// MODIFIED for the Windows port: what keeps the game's save data as last committed until the next commit (mh4n/save_journal.h).
// Only the save data (ArchiveId::Save) has a commit; the extra data is written as before.
struct SaveGuard {
    fs::path user, root;  // `root` empty: nothing to keep (not the save data)

    static SaveGuard For(const fs::path& user, const Archive* archive) {
        return archive && archive->id == ArchiveId::Save ? SaveGuard{user, archive->root} : SaveGuard{};
    }
    // Before a change. When the way back cannot be kept, the change is refused: the save stays as it is.
    Result Prepare() const {
        if (root.empty()) return kResultSuccess;
        const auto error = save_journal::Begin(user, root);
        if (!error) return kResultSuccess;
        std::fprintf(stderr, "mh4n: the save data was not changed: it could not be put aside first (%s)\n", error.message().c_str());
        return IoError;
    }
};

class FileSession final : public Service {
public:
    FileSession(ServiceContext& context, FileData data, u32 flags, SaveGuard guard)
        : Service(context, ServiceName), data_(std::move(data)), flags_(flags), guard_(std::move(guard)) {
        Register(0x0401, "Control");
        Register(0x0801, "OpenSubFile");
        Register(0x0802, "Read", &FileSession::Read);       // seq=24, 308
        Register(0x0803, "Write", &FileSession::Write);     // seq=35492, 35647
        Register(0x0804, "GetSize", &FileSession::GetSize); // seq=316
        Register(0x0805, "SetSize", &FileSession::SetSize); // seq=35490, 35645
        Register(0x0806, "GetAttributes");
        Register(0x0807, "SetAttributes");
        Register(0x0808, "Close", &FileSession::Close);    // seq=25
        Register(0x0809, "Flush", &FileSession::Flush);    // 3dbrew FSFile:Flush
        Register(0x080A, "SetPriority");
        Register(0x080B, "GetPriority");
        Register(0x080C, "OpenLinkFile");
    }

    const char* SessionKind() const override { return "file"; }

private:
    Handled Read(Request& request) {
        const u64 offset = request.Pop<u64>();
        const u32 size = request.Pop<u32>();
        const auto* buffer = request.PopMappedBuffer();
        if (!buffer || !buffer->Writable() || size > buffer->Size()) {
            ReplyResult(request, InvalidArgument);
            return Handled::Done;
        }
        if (!data_ || !(flags_ & ReadFlag)) {
            auto& reply = request.MakeReply(2, 2);
            reply.Push(Permission, u32{0});
            reply.PushMappedBuffer(*buffer);
            return Handled::Done;
        }
        return DeferIo(request, TransferTicks(size, data_.length.has_value()),
                       [data = data_, offset, size, buffer](Request& pending) {
            u32 read = 0;
            Result result = IoError;
            if (auto length = data.Size()) {
                const u64 available = offset < *length ? *length - offset : 0;
                std::vector<u8> bytes(static_cast<std::size_t>(std::min<u64>(size, available)));
                result = data.Read(offset, bytes, read);
                if (read && !buffer->Write(bytes.data(), 0, read)) {
                    read = 0;
                    result = InvalidArgument;
                }
            }
            auto reply = pending.NewReply(2, 2);
            reply.Push(result, read);
            reply.PushMappedBuffer(*buffer);
            return reply;
        });
    }

    Handled Write(Request& request) {
        const u64 offset = request.Pop<u64>();
        const u32 size = request.Pop<u32>();
        const u32 options = request.Pop<u32>();
        const auto* buffer = request.PopMappedBuffer();
        if (!buffer || !buffer->Readable() || size > buffer->Size()) {
            ReplyResult(request, InvalidArgument);
            return Handled::Done;
        }
        if (!data_ || !(flags_ & WriteFlag)) {
            auto& reply = request.MakeReply(2, 2);
            reply.Push(Permission, u32{0});
            reply.PushMappedBuffer(*buffer);
            return Handled::Done;
        }
        std::vector<u8> bytes(size);
        if (size && !buffer->Read(bytes.data(), 0, size)) {
            ReplyResult(request, InvalidArgument);
            return Handled::Done;
        }
        return DeferIo(request, TransferTicks(size, false),
                       [file = data_.file, offset, bytes = std::move(bytes), buffer, options, guard = guard_]
                       (Request& pending) {
            u32 written = 0;
            Result result = guard.Prepare();
            if (result == kResultSuccess) result = file->Write(offset, bytes, written, (options & 1) != 0);
            auto reply = pending.NewReply(2, 2);
            reply.Push(result, written);
            reply.PushMappedBuffer(*buffer);
            return reply;
        });
    }

    void GetSize(Request& request) {
        const auto size = data_.Size();
        request.MakeReply(3, 0).Push(size ? kResultSuccess : IoError, size.value_or(0));
    }

    void SetSize(Request& request) {
        const auto size = request.Pop<u64>();
        Result result = data_ && (flags_ & WriteFlag) ? guard_.Prepare() : Permission;
        if (result == kResultSuccess) result = data_.file->Resize(size);
        ReplyResult(request, result);
    }

    void Close(Request& request) {
        const auto result = data_ && (flags_ & WriteFlag) ? data_.file->Flush() : kResultSuccess;
        data_ = {};
        ReplyResult(request, result);
    }

    void Flush(Request& request) {
        const auto result = data_ && (flags_ & WriteFlag) ? data_.file->Flush() : kResultSuccess;
        ReplyResult(request, result);
    }

    FileData data_;
    u32 flags_;
    SaveGuard guard_;
};

class DirectorySession final : public Service {
public:
    DirectorySession(ServiceContext& context, std::vector<DirEntry> entries)
        : Service(context, ServiceName), entries_(std::move(entries)) {
        Register(0x0401, "Control");
        Register(0x0801, "Read", &DirectorySession::Read);    // seq=264, 1081
        Register(0x0802, "Close", &DirectorySession::Close);  // seq=265
        Register(0x0803, "SetPriority");
        Register(0x0804, "GetPriority");
    }

    const char* SessionKind() const override { return "dir"; }

private:
    void Read(Request& request) {
        const auto count = request.Pop<u32>();
        const auto* buffer = request.PopMappedBuffer();
        if (!buffer || !buffer->Writable() || count > buffer->Size() / sizeof(DirEntry)) {
            ReplyResult(request, InvalidArgument);
            return;
        }
        const auto take = std::min<std::size_t>(count, entries_.size() - position_);
        u32 read = 0;
        Result result = kResultSuccess;
        if (take) {
            const auto* data = entries_.data() + position_;
            if (buffer->Write(data, 0, static_cast<u32>(take * sizeof(DirEntry)))) {
                position_ += take;
                read = static_cast<u32>(take);
            } else {
                result = InvalidArgument;
            }
        }
        auto& reply = request.MakeReply(2, 2);
        reply.Push(result, read);
        reply.PushMappedBuffer(*buffer);
    }

    void Close(Request& request) {
        entries_.clear();
        position_ = 0;
        ReplyResult(request, kResultSuccess);
    }

    std::vector<DirEntry> entries_;
    std::size_t position_ = 0;
};

struct FsState {
    u64 next_archive = 1;
    u32 priority = 0;
    std::map<u64, Archive> archives;
    std::multimap<fs::path, std::weak_ptr<HostFile>> writers;
};

class FsUser final : public Service {
public:
    explicit FsUser(ServiceContext& context)
        : Service(context, ServiceName), state_(context.State<FsState>("new/fs:USER")) {
        Register(0x0801, "Initialize", &FsUser::Initialize);        // 3dbrew FS:Initialize
        Register(0x0802, "OpenFile", &FsUser::OpenFile);            // seq=223, 307, 35486
        Register(0x0803, "OpenFileDirectly", &FsUser::OpenDirect);  // seq=23, 34, 260
        Register(0x0804, "DeleteFile", &FsUser::DeleteFile);         // 3dbrew FS:DeleteFile
        Register(0x0805, "RenameFile");
        Register(0x0806, "DeleteDirectory");
        Register(0x0807, "DeleteDirectoryRecursively");
        Register(0x0808, "CreateFile", &FsUser::CreateFile);         // seq=50
        Register(0x0809, "CreateDirectory", &FsUser::CreateDir);     // 3dbrew FS:CreateDirectory
        Register(0x080A, "RenameDirectory");
        Register(0x080B, "OpenDirectory", &FsUser::OpenDir);        // seq=262, 297
        Register(0x080C, "OpenArchive", &FsUser::OpenArchive);      // seq=49, 229, 1104, 38588
        Register(0x080D, "ControlArchive", &FsUser::Control);       // seq=279, 35504
        Register(0x080E, "CloseArchive", &FsUser::CloseArchive);    // seq=224
        Register(0x0812, "GetFreeBytes", &FsUser::GetFreeBytes);     // seq=35456, 35614
        Register(0x0817, "IsSdmcDetected", &FsUser::IsSdmcDetected); // seq=57
        Register(0x0845, "GetFormatInfo", &FsUser::GetFormatInfo);   // 3dbrew FS:GetFormatInfo
        Register(0x084C, "FormatSaveData");
        Register(0x0851, "CreateExtSaveData");
        Register(0x0861, "InitializeWithSdkVersion", &FsUser::InitializeVersion); // seq=21
        Register(0x0862, "SetPriority", &FsUser::SetPriority);      // seq=22
        Register(0x0863, "GetPriority", &FsUser::GetPriority);      // seq=33, 44, 266
    }

private:
    fs::path GameFolder() const { return fs::path(std::u8string(Context().GameFolder().begin(),
                                                             Context().GameFolder().end())); }
    fs::path UserFolder() const { return fs::path(std::u8string(Context().UserFolder().begin(),
                                                             Context().UserFolder().end())); }

    const Archive* Find(u64 handle) const {
        const auto entry = state_->archives.find(handle);
        return entry == state_->archives.end() ? nullptr : &entry->second;
    }

    Result Describe(ArchiveId id, LowPath path, Archive& archive) const {
        const u64 title = id == ArchiveId::Save ? GameTitle(GameFolder()) : 0;
        return DescribeArchive(UserFolder(), title, id, path, archive);
    }

    Result Target(u64 handle, LowPath low, fs::path& path) const {
        const auto* archive = Find(handle);
        return archive ? ResolvePath(UserFolder(), *archive, low, path) : ArchiveNotMounted;
    }

    void Initialize(Request& request) {
        request.PopPid();
        ReplyResult(request, kResultSuccess);
    }

    void InitializeVersion(Request& request) {
        request.Pop<u32>();
        Initialize(request);
    }

    void SetPriority(Request& request) {
        state_->priority = request.Pop<u32>();
        ReplyResult(request, kResultSuccess);
    }

    void GetPriority(Request& request) {
        request.MakeReply(2, 0).Push(kResultSuccess, state_->priority);
    }

    void IsSdmcDetected(Request& request) {
        request.MakeReply(2, 0).Push(kResultSuccess, !UserFolder().empty());
    }

    void OpenArchive(Request& request) {
        const auto id = request.Pop<ArchiveId>();
        const auto type = request.Pop<PathType>();
        const auto size = request.Pop<u32>();
        Archive archive;
        auto result = Describe(id, PopPath(request, type, size), archive);
        if (result == kResultSuccess)
            result = CheckArchive(archive);
        u64 handle = 0;
        if (result == kResultSuccess) {
            handle = state_->next_archive++;
            state_->archives.emplace(handle, std::move(archive));
        }
        request.MakeReply(3, 0).Push(result, handle);
    }

    void CloseArchive(Request& request) {
        const auto erased = state_->archives.erase(request.Pop<u64>());
        ReplyResult(request, erased ? kResultSuccess : ArchiveNotMounted);
    }

    void GetFreeBytes(Request& request) {
        const auto* archive = Find(request.Pop<u64>());
        if (!archive || archive->IsContent()) {
            ReplyResult(request, archive ? UnsupportedOpenFlags : ArchiveNotMounted);
            return;
        }
        request.MakeReply(3, 0).Push(kResultSuccess, SaveFreeBytes);
    }

    void GetFormatInfo(Request& request) {
        const auto id = request.Pop<ArchiveId>();
        const auto type = request.Pop<PathType>();
        const auto size = request.Pop<u32>();
        Archive archive;
        auto result = Describe(id, PopPath(request, type, size), archive);
        FormatInfo info{};
        if (result == kResultSuccess)
            result = ReadFormatInfo(archive, info);
        if (result != kResultSuccess) {
            ReplyResult(request, result);
            return;
        }
        request.MakeReply(5, 0).Push(kResultSuccess, info.size, info.directories,
                                     info.files, static_cast<u32>(info.duplicate));
    }

    void OpenFile(Request& request) {
        request.Pop<u32>();
        const auto* archive = Find(request.Pop<u64>());
        const auto type = request.Pop<PathType>();
        const auto size = request.Pop<u32>();
        const auto flags = request.Pop<u32>();
        request.Pop<u32>(); // Attributes.
        const auto path = PopPath(request, type, size);
        if (archive)
            Open(request, *archive, path, flags);
        else
            ReplyObject(request, ArchiveNotMounted);
    }

    void OpenDirect(Request& request) {
        request.Pop<u32>();
        const auto id = request.Pop<ArchiveId>();
        const auto archive_type = request.Pop<PathType>();
        const auto archive_size = request.Pop<u32>();
        const auto file_type = request.Pop<PathType>();
        const auto file_size = request.Pop<u32>();
        const auto flags = request.Pop<u32>();
        request.Pop<u32>(); // Attributes.
        const auto archive_path = PopPath(request, archive_type, archive_size);
        const auto file_path = PopPath(request, file_type, file_size);
        Archive archive;
        auto result = Describe(id, archive_path, archive);
        if (result == kResultSuccess)
            result = CheckArchive(archive);
        if (result != kResultSuccess) {
            ReplyObject(request, result);
            return;
        }
        // A direct open allocates a transient archive handle (seq=23–49, 260–281).
        ++state_->next_archive;
        Open(request, archive, file_path, flags);
    }

    void Open(Request& request, const Archive& archive, LowPath path, u32 flags) {
        Result result = UnsupportedOpenFlags;
        FileData data;
        if (flags && !(flags & ~(ReadFlag | WriteFlag | CreateFlag)) &&
            (!(flags & CreateFlag) || (flags & WriteFlag))) {
            if (archive.IsContent()) {
                result = flags == ReadFlag ? OpenContent(GameFolder(), UserFolder(), archive, path, data)
                                           : UnsupportedOpenFlags;
            } else {
                fs::path target;
                result = ResolvePath(UserFolder(), archive, path, target);
                std::error_code ignored;
                if (result == kResultSuccess && (flags & CreateFlag) && !fs::exists(target, ignored))
                    result = SaveGuard::For(UserFolder(), &archive).Prepare();  // (opening it makes the file)
                if (result == kResultSuccess)
                    result = OpenSaveFile(target, flags, data);
            }
        }
        if (result != kResultSuccess) {
            ReplyObject(request, result);
            return;
        }
        if (flags & WriteFlag) {
            std::erase_if(state_->writers, [](const auto& entry) { return entry.second.expired(); });
            state_->writers.emplace(archive.root, data.file);
        }
        ReplyObject(request, kResultSuccess, Context().CreateSession(
            std::make_unique<FileSession>(Context(), std::move(data), flags, SaveGuard::For(UserFolder(), &archive))));
    }

    void OpenDir(Request& request) {
        const u64 handle = request.Pop<u64>();
        const auto type = request.Pop<PathType>();
        const auto size = request.Pop<u32>();
        fs::path path;
        auto result = Target(handle, PopPath(request, type, size), path);
        std::vector<DirEntry> entries;
        if (result == kResultSuccess)
            result = ReadDirectory(path, entries);
        if (result != kResultSuccess) {
            ReplyObject(request, result);
            return;
        }
        ReplyObject(request, result, Context().CreateSession(
            std::make_unique<DirectorySession>(Context(), std::move(entries))));
    }

    void CreateFile(Request& request) {
        request.Pop<u32>();
        const u64 handle = request.Pop<u64>();
        const auto type = request.Pop<PathType>();
        const auto size = request.Pop<u32>();
        request.Pop<u32>(); // Attributes.
        const auto file_size = request.Pop<u64>();
        fs::path path;
        auto result = Target(handle, PopPath(request, type, size), path);
        if (result == kResultSuccess) result = SaveGuard::For(UserFolder(), Find(handle)).Prepare();
        ReplyResult(request, result == kResultSuccess ? HostFile::Create(path, file_size) : result);
    }

    void CreateDir(Request& request) {
        request.Pop<u32>();
        const u64 handle = request.Pop<u64>();
        const auto type = request.Pop<PathType>();
        const auto size = request.Pop<u32>();
        request.Pop<u32>(); // Attributes.
        fs::path path;
        auto result = Target(handle, PopPath(request, type, size), path);
        if (result == kResultSuccess) result = SaveGuard::For(UserFolder(), Find(handle)).Prepare();
        ReplyResult(request, result == kResultSuccess ? CreateSaveDirectory(path) : result);
    }

    void DeleteFile(Request& request) {
        request.Pop<u32>();
        const u64 handle = request.Pop<u64>();
        const auto type = request.Pop<PathType>();
        const auto size = request.Pop<u32>();
        fs::path path;
        auto result = Target(handle, PopPath(request, type, size), path);
        if (result == kResultSuccess) result = SaveGuard::For(UserFolder(), Find(handle)).Prepare();
        ReplyResult(request, result == kResultSuccess ? DeleteSaveFile(path) : result);
    }

    void Control(Request& request) {
        const auto* archive = Find(request.Pop<u64>());
        const auto action = request.Pop<u32>();
        if (!archive || archive->IsContent() || action != 0) {
            ReplyResult(request, archive ? InvalidArgument : ArchiveNotMounted);
            return;
        }
        // Commit has no output payload (3dbrew FS:ControlArchive). Open writers may still be dirty.
        const auto [first, last] = state_->writers.equal_range(archive->root);
        for (auto entry = first; entry != last; ++entry) {
            if (auto writer = entry->second.lock()) {
                const auto result = writer->Flush();
                if (result != kResultSuccess) {
                    ReplyResult(request, result);
                    return;
                }
            }
        }
        std::error_code ignored;
        if (archive->id == ArchiveId::Save && fs::exists(save_journal::Journal(UserFolder()), ignored)) {  // MODIFIED: from here the changes count
            if (const auto error = save_journal::Commit(UserFolder())) {
                // (the game is told: the next start would put the save back as before this commit)
                std::fprintf(stderr, "mh4n: the save data's commit failed (%s)\n", error.message().c_str());
                ReplyResult(request, IoError);
                return;
            }
            std::fprintf(stderr, "mh4n: save data committed\n");
        }
        ReplyResult(request, kResultSuccess);
    }

    std::shared_ptr<FsState> state_;
};

MH4N_REGISTER(ServiceFactory, "new/fs:USER",
              [](ServiceContext& context) { return std::make_unique<FsUser>(context); });

}  // namespace
}  // namespace mh4n
