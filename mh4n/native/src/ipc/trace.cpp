// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: mh4n/runtime.h, mh4n/service.h, ipc_test.cpp and boot-village.ipc.trace.
#include "mh4n/crc32.h"
#include "mh4n/runtime.h"
#include "mh4n/service.h"

#include <chrono>
#include <cctype>
#include <fstream>

namespace mh4n {
namespace {

MH4N_OPTION("runner", "--trace-ipc", "FILE", "write the IPC trace");
MH4N_OPTION("runner", "--trace-ipc-full", "", "include IPC input buffer bytes");

std::string WordList(std::span<const u32> words) {
    std::string result;
    for (u32 word : words) {
        fmt::format_to(std::back_inserter(result), "{}0x{:08x}", result.empty() ? "" : ",", word);
    }
    return result.empty() ? "-" : result;
}

void AppendDescriptor(std::string& list, std::string_view kind, u32 index, u32 size,
                      std::string_view detail) {
    fmt::format_to(std::back_inserter(list), "{}{}:{}:{}:{}", list.empty() ? "" : ",", kind,
                   index, size, detail);
}

std::string_view MappedKind(ipc::MappedBufferPermissions perms) {
    switch (perms) {
    case ipc::R:
        return "mapped-r";
    case ipc::W:
        return "mapped-w";
    default:
        return "mapped-rw";
    }
}

std::string BufferCrc(std::span<const u8> bytes) {
    return fmt::format("0x{:08x}", Crc32(bytes.data(), bytes.size()));
}

struct TraceRecord {
    std::string prefix;
    std::string data;
    std::string session;
    u64 tick = 0;
};

class IpcTrace {
public:
    IpcTrace(Runtime& runtime, const std::string& path, bool full)
        : runtime_(runtime), output_(path), full_(full) {}

    bool Open() const { return output_.is_open(); }

    void Header() {
        const auto today = std::chrono::year_month_day{
            std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now())};
        const auto* image = runtime_.Image();
        output_ << fmt::format("# ipctrace v1 {:04d}-{:02d}-{:02d} game=0x{:016x} cpu=native new3ds={}\n",
                               static_cast<int>(today.year()), static_cast<unsigned>(today.month()),
                               static_cast<unsigned>(today.day()), image ? image->program_id : 0,
                               runtime_.GetOptions().Has("--new3ds") ? "on" : "off");
    }

    void MainThread() {
        if (const auto thread = runtime_.MainThread()) {
            ThreadCreated({.thread_id = thread->thread_id,
                           .entry = thread->entry_point,
                           .stack_top = thread->stack_top,
                           .priority = thread->nominal_priority,
                           .requested_processor = thread->processor_id,
                           .resolved_processor = thread->processor_id});
        }
    }

    void ThreadCreated(const SvcThreadCreated& thread) {
        output_ << fmt::format("# thread {} parent={} entry={:08X} arg={:08X} stack={:08X} prio={} core={}/{}\n",
                               thread.thread_id, thread.parent_id, thread.entry, thread.arg,
                               thread.stack_top, thread.priority, thread.requested_processor,
                               thread.resolved_processor);
    }

    void Sent(const Request& request, Result translation) {
        const auto client = request.Session();
        std::string session = client->PortName();
        if (session.empty()) {
            const auto found = objects_.find(client->ObjectId());
            session = found == objects_.end() ? client->Server()->Name() : found->second;
        }

        TraceRecord record;
        record.prefix = fmt::format("{} seq={} session={}", request.ClientThreadId(),
                                    request.Sequence(), session);
        record.tick = runtime_.GetKernel().NowTicks();
        if (translation != kResultSuccess) {
            output_ << record.prefix << " cmd=- in=- desc=- out=- outdesc=- t=" << record.tick << '\n';
            return;
        }

        std::string descriptors;
        auto static_buffer = request.StaticBuffers().begin();
        for (const auto& desc : request.Descriptors()) {
            switch (desc.type) {
            case ipc::MoveHandle:
            case ipc::CopyHandle: {
                std::string names;
                for (const auto& object : desc.objects) {
                    AppendObject(names, object.get(), session);
                }
                AppendDescriptor(descriptors, desc.type == ipc::MoveHandle ? "move" : "copy",
                                 desc.index, desc.size, names);
                break;
            }
            case ipc::CallingPid:
                AppendDescriptor(descriptors, "pid", desc.index, 1, "-");
                break;
            case ipc::StaticBuffer:
                AppendDescriptor(descriptors, "static", desc.index, desc.size,
                                 BufferCrc(static_buffer->data));
                AddData(record.data, desc.index, static_buffer->data);
                ++static_buffer;
                break;
            case ipc::MappedBuffer: {
                const auto bytes = (desc.perms & ipc::R)
                    ? ReadBytes(request, desc.address, desc.size) : std::nullopt;
                AppendDescriptor(descriptors, MappedKind(desc.perms), desc.index, desc.size,
                                 bytes ? BufferCrc(*bytes) : "-");
                if (bytes) {
                    AddData(record.data, desc.index, *bytes);
                }
                break;
            }
            default:
                break;
            }
        }
        fmt::format_to(std::back_inserter(record.prefix), " cmd=0x{:08x} in={} desc={}",
                       request.Header(), WordList(request.UntranslatedWords()),
                       descriptors.empty() ? "-" : descriptors);
        record.session = std::move(session);
        pending_.emplace(request.Sequence(), std::move(record));
    }

    void Replied(const Request& request, std::span<const u32> words) {
        auto record = pending_.extract(request.Sequence());
        if (record.empty()) {
            return;
        }
        auto descriptors = ReplyDescriptors(request, words, record.mapped().session);
        Write(record.mapped(), WordList(words), descriptors);
    }

    void Finish() {
        for (const auto& [sequence, record] : pending_) {
            Write(record, "-", {});
        }
        pending_.clear();
        output_.close();
    }

private:
    static std::optional<std::vector<u8>> ReadBytes(const Request& request, u32 address, u32 size) {
        if (size && !request.Memory().IsMapped(address, size)) {
            return std::nullopt;
        }
        std::vector<u8> bytes(size);
        if (size && !request.Memory().ReadBlock(bytes.data(), address, size)) {
            return std::nullopt;
        }
        return bytes;
    }

    void AddData(std::string& data, u32 index, std::span<const u8> bytes) const {
        if (!full_ || bytes.empty()) {
            return;
        }
        fmt::format_to(std::back_inserter(data), " data={}:", index);
        for (u8 byte : bytes) {
            fmt::format_to(std::back_inserter(data), "{:02x}", byte);
        }
    }

    void AppendObject(std::string& names, const Object* object, const std::string& session) {
        if (!names.empty()) {
            names += '+';
        }
        if (!object) {
            names += '-';
            return;
        }
        auto [found, added] = objects_.try_emplace(object->ObjectId());
        if (added) {
            std::string kind = object->TypeName();
            if (object->GetHandleType() == HandleType::ClientSession) {
                const auto* server = static_cast<const ClientSession*>(object)->Server().get();
                kind = server->Closed() ? "session" : server->GetService()->SessionKind();
            } else {
                for (char& letter : kind) {
                    letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
                }
            }
            auto category = fmt::format("{}/{}", session, kind);
            found->second = fmt::format("{}#{}", category, counts_[category]++);
        }
        names += found->second;
    }

    std::string ReplyDescriptors(const Request& request, std::span<const u32> words,
                                 const std::string& session) {
        std::map<u32, std::string> fields;
        const auto process = request.ClientThread().owner_process.lock();
        for (std::size_t index = 1 + ipc::Header{words.front()}.NormalParamsSize();
             index < words.size();) {
            const u32 position = static_cast<u32>(index);
            const u32 word = words[index++];
            const auto type = ipc::GetDescriptorType(word);
            const bool handles = type == ipc::MoveHandle || type == ipc::CopyHandle;
            const u32 payload = handles ? ipc::HandleNumberFromDesc(word) : 1;
            if (payload > words.size() - index) {
                break;
            }

            switch (type) {
            case ipc::MoveHandle:
            case ipc::CopyHandle: {
                std::string names;
                for (u32 i = 0; i < payload; ++i) {
                    const u32 handle = words[index + i];
                    AppendObject(names, process && handle ? process->handle_table.PeekGeneric(handle)
                                                           : nullptr, session);
                }
                AppendDescriptor(fields[position], type == ipc::MoveHandle ? "move" : "copy",
                                 position, payload, names);
                break;
            }
            case ipc::CallingPid:
                AppendDescriptor(fields[position], "pid", position, 1, "-");
                break;
            case ipc::StaticBuffer: {
                const auto id = ipc::StaticBufferDescId(word);
                const u32 slot = request.ClientThread().tls_address + 0x80 +
                                 static_cast<u32>(ipc::StaticBufferDescriptorWord(id) * 4);
                const auto capacity = request.Memory().Read<u32>(slot).value_or(0);
                const u32 size = std::min(ipc::StaticBufferDescSize(word),
                                          ipc::StaticBufferDescSize(capacity));
                const auto bytes = ReadBytes(request, words[index], size);
                AppendDescriptor(fields[position], "static", position, size,
                                 bytes ? BufferCrc(*bytes) : "-");
                break;
            }
            default:
                break;
            }
            index += payload;
        }
        for (const auto& desc : request.Descriptors()) {
            if (desc.type == ipc::MappedBuffer && (desc.perms & ipc::W)) {
                const auto bytes = ReadBytes(request, desc.address, desc.size);
                std::string mapped;
                AppendDescriptor(mapped, MappedKind(desc.perms), desc.index, desc.size,
                                 bytes ? BufferCrc(*bytes) : "-");
                auto& field = fields[desc.index];
                if (!field.empty()) {
                    field += ',';
                }
                field += mapped;
            }
        }
        std::string descriptors;
        for (const auto& [index, field] : fields) {
            if (!descriptors.empty()) {
                descriptors += ',';
            }
            descriptors += field;
        }
        return descriptors;
    }

    void Write(const TraceRecord& record, std::string_view words, std::string_view descriptors) {
        output_ << record.prefix << " out=" << words << " outdesc="
                << (descriptors.empty() ? "-" : descriptors) << " t="
                << record.tick << record.data << '\n';
    }

    Runtime& runtime_;
    std::ofstream output_;
    bool full_;
    std::map<u32, TraceRecord> pending_;
    std::map<u32, std::string> objects_;
    std::map<std::string, u32> counts_;
};

thread_local std::map<Runtime*, std::weak_ptr<IpcTrace>> traces;

void InstallTrace(Runtime& runtime) {
    const auto path = runtime.GetOptions().Get("--trace-ipc");
    const bool full = runtime.GetOptions().Has("--trace-ipc-full");
    if (!path) {
        if (full) {
            runtime.RequestExit(2, "--trace-ipc-full needs --trace-ipc FILE");
        }
        return;
    }
    auto trace = std::make_shared<IpcTrace>(runtime, *path, full);
    if (!trace->Open()) {
        runtime.RequestExit(2, fmt::format("cannot write IPC trace {}", *path));
        return;
    }
    trace->Header();
    if (auto* manager = FindServiceManager(runtime)) {
        manager->AddObserver({
            [trace](const Request& request, Result result) { trace->Sent(request, result); },
            [trace](const Request& request, std::span<const u32> words) {
                trace->Replied(request, words);
            },
        });
    }
    SvcHooks hooks;
    hooks.thread_created = [trace](const SvcThreadCreated& thread) { trace->ThreadCreated(thread); };
    runtime.AddSvcHooks(std::move(hooks));
    traces.insert_or_assign(&runtime, trace);
    runtime.OnFinish([trace, &runtime] {
        traces.erase(&runtime);
        trace->Finish();
    });
}

void TraceMainThread(Runtime& runtime) {
    // The tracing step precedes creation of the main thread; use a started-phase callback.
    const auto found = traces.find(&runtime);
    if (found != traces.end()) {
        if (const auto trace = found->second.lock()) {
            trace->MainThread();
        }
        traces.erase(found);
    }
}

MH4N_REGISTER(BootStep, "trace.ipc", BootStep{Phase::Tracing, InstallTrace});
MH4N_REGISTER(BootStep, "trace.ipc.main", BootStep{Phase::Started, TraceMainThread});

}  // namespace
}  // namespace mh4n
