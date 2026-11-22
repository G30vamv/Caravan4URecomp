// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: mh4n/ipc.h, mh4n/kernel.h, ipc_test.cpp and boot-village.ipc.trace.
#include "mh4n/ipc.h"

#include <algorithm>

namespace mh4n {
namespace {

constexpr auto kLog = "Service.IPC";
constexpr u32 kCommandOffset = 0x80;

std::size_t WordCount(std::size_t bytes) {
    return bytes / sizeof(u32) + (bytes % sizeof(u32) != 0);
}

u32 ByteMask(std::size_t bytes) {
    return bytes == sizeof(u32) ? ~u32{0} : (u32{1} << (bytes * 8)) - 1;
}

bool InMappedRange(u32 address, u32 capacity, u32 offset, u32 size) {
    return offset <= capacity && size <= capacity - offset &&
           static_cast<u64>(address) + offset + size <= (u64{1} << 32);
}

}  // namespace

MappedBuffer::MappedBuffer(GuestMemory& memory, u32 descriptor, u32 address, u32 id)
    : memory_(&memory), id_(id), address_(address), size_(ipc::MappedBufferDescSize(descriptor)),
      perms_(ipc::MappedBufferDescPerms(descriptor)) {}

bool MappedBuffer::Read(void* dst, u32 offset, u32 size) const {
    if (!Readable()) {
        MH4N_LOG(Warning, kLog, "mapped buffer {} has no read permission", id_);
        return false;
    }
    if (!InMappedRange(address_, size_, offset, size)) {
        MH4N_LOG(Warning, kLog, "read at {} of {} bytes is outside mapped buffer {}", offset, size, id_);
        return false;
    }
    if (size != 0 && !memory_->ReadBlock(dst, address_ + offset, size)) {
        MH4N_LOG(Warning, kLog, "mapped buffer {} is not mapped for read", id_);
        return false;
    }
    return true;
}

bool MappedBuffer::Write(const void* src, u32 offset, u32 size) const {
    if (!Writable()) {
        MH4N_LOG(Warning, kLog, "mapped buffer {} has no write permission", id_);
        return false;
    }
    if (!InMappedRange(address_, size_, offset, size)) {
        MH4N_LOG(Warning, kLog, "write at {} of {} bytes is outside mapped buffer {}", offset, size, id_);
        return false;
    }
    if (size != 0 && !memory_->WriteBlock(address_ + offset, src, size)) {
        MH4N_LOG(Warning, kLog, "mapped buffer {} is not mapped for write", id_);
        return false;
    }
    return true;
}

Reply::Reply(u16 command_id, u32 normal_params_size, u32 translate_params_size) {
    words_[0] = ipc::MakeHeader(command_id, normal_params_size, translate_params_size);
}

bool Reply::Validate() const {
    if (overflow_) {
        MH4N_LOG(Warning, kLog, "more than 64 words pushed into IPC reply");
    }
    if (CommandSize() != index_) {
        MH4N_LOG(Warning, kLog, "IPC reply declares {} words and {} were pushed", CommandSize(), index_);
    }
    return !overflow_ && CommandSize() == index_ && index_ <= words_.size();
}

void Reply::PushWord(u32 word) {
    PushPartial(word, sizeof(word));
}

void Reply::PushPartial(u32 value, std::size_t size) {
    const auto index = index_++;
    if (index >= words_.size()) {
        overflow_ = true;
        return;
    }
    const u32 mask = ByteMask(size);
    words_[index] = (words_[index] & ~mask) | (value & mask);
    written_[index] = mask;
}

void Reply::PushBytes(const void* data, std::size_t size) {
    auto bytes = static_cast<const u8*>(data);
    for (std::size_t offset = 0; offset < size; offset += sizeof(u32)) {
        u32 word = 0;
        const auto count = std::min(sizeof(word), size - offset);
        std::memcpy(&word, bytes + offset, count);
        PushPartial(word, count);
    }
}

void Reply::PushHandles(u32 descriptor, std::vector<std::shared_ptr<Object>> objects) {
    PushWord(descriptor);
    for (auto& object : objects) {
        PushWord(static_cast<u32>(objects_.size()));
        objects_.push_back(std::move(object));
    }
}

void Reply::PushStaticBuffer(std::vector<u8> buffer, u8 buffer_id) {
    const u8 id = buffer_id & 0xF;
    PushWord(ipc::StaticBufferDesc(buffer.size(), id));
    PushWord(0xDEADC0DE);
    static_buffers_[id] = std::move(buffer);
}

void Reply::PushMappedBuffer(const MappedBuffer& buffer) {
    PushWord(buffer.GenerateDescriptor());
    PushWord(buffer.Id());
}

void Reply::PushPid() {
    PushWord(ipc::CallingPidDesc());
    PushWord(0);
}

std::array<u32, ipc::kCommandBufferLength> Reply::SeededWords(
    std::span<const u32> request_words) const {
    auto words = words_;
    if (!seeded_) {
        const auto end = std::min(request_words.size(),
                                  std::size_t{1} + ipc::Header{HeaderWord()}.NormalParamsSize());
        for (std::size_t index = 1; index < end; ++index) {
            words[index] |= request_words[index] & ~written_[index];
        }
    }
    return words;
}

void Reply::SeedFrom(std::span<const u32> request_words) {
    words_ = SeededWords(request_words);
    seeded_ = true;
}

Request::Request(Kernel& kernel, std::shared_ptr<Thread> thread,
                 std::shared_ptr<ClientSession> session, u32 sequence)
    : kernel_(kernel), thread_(std::move(thread)), process_(thread_->owner_process.lock()),
      session_(std::move(session)), thread_id_(thread_->thread_id), sequence_(sequence),
      pid_(process_ ? process_->process_id : 0) {}

Request::~Request() = default;

Result Request::Translate(std::span<const u32> area) {
    if (area.empty()) {
        return kResultInvalidBufferDescriptor;
    }
    const ipc::Header header{area.front()};
    const std::size_t first_descriptor = 1 + header.NormalParamsSize();
    const std::size_t count = first_descriptor + header.TranslateParamsSize();
    if (count > cmd_.size() || count > area.size()) {
        return kResultInvalidBufferDescriptor;
    }
    command_size_ = count;
    std::copy_n(area.begin(), count, untranslated_.begin());
    cmd_ = untranslated_;

    std::vector<Handle> moved;
    for (std::size_t index = first_descriptor; index < count;) {
        ipc::Descriptor desc;
        desc.index = static_cast<u32>(index);
        desc.word = cmd_[index++];
        desc.type = ipc::GetDescriptorType(desc.word);
        const bool handles = desc.type == ipc::CopyHandle || desc.type == ipc::MoveHandle;
        const u32 payload = handles ? ipc::HandleNumberFromDesc(desc.word) : 1;
        if (payload > count - index) {
            return kResultInvalidBufferDescriptor;
        }

        switch (desc.type) {
        case ipc::CopyHandle:
        case ipc::MoveHandle:
            desc.size = payload;
            for (u32 part = 0; part < payload; ++part) {
                const Handle handle = cmd_[index + part];
                std::shared_ptr<Object> object;
                if (handle != 0) {
                    if (process_ && std::find(moved.begin(), moved.end(), handle) == moved.end()) {
                        object = process_->handle_table.GetGeneric(handle);
                    }
                    if (!object) {
                        return kResultInvalidHandleInCommand;
                    }
                    if (desc.type == ipc::MoveHandle) {
                        moved.push_back(handle);
                    }
                }
                cmd_[index + part] = AddIncoming(object);
                desc.objects.push_back(std::move(object));
            }
            break;
        case ipc::CallingPid:
            desc.size = 1;
            cmd_[index] = pid_;
            break;
        case ipc::StaticBuffer: {
            desc.address = cmd_[index];
            desc.size = ipc::StaticBufferDescSize(desc.word);
            StaticBuffer buffer{ipc::StaticBufferDescId(desc.word), desc.address, desc.size,
                                std::vector<u8>(desc.size)};
            if (desc.size && !Memory().ReadBlock(buffer.data.data(), desc.address, desc.size)) {
                MH4N_LOG(Warning, kLog, "static buffer {} is not mapped", buffer.id);
                std::fill(buffer.data.begin(), buffer.data.end(), 0);
            }
            static_in_.push_back(std::move(buffer));
            break;
        }
        case ipc::MappedBuffer:
            desc.address = cmd_[index];
            desc.size = ipc::MappedBufferDescSize(desc.word);
            desc.perms = ipc::MappedBufferDescPerms(desc.word);
            desc.mapped_id = static_cast<u32>(mapped_.size());
            mapped_.emplace_back(Memory(), desc.word, desc.address, desc.mapped_id);
            cmd_[index] = desc.mapped_id;
            break;
        default:
            return kResultInvalidBufferDescriptor;
        }
        descriptors_.push_back(std::move(desc));
        index += payload;
    }
    for (Handle handle : moved) {
        process_->handle_table.Close(handle);
    }
    return kResultSuccess;
}

u32 Request::AddIncoming(std::shared_ptr<Object> object) {
    const auto index = static_cast<u32>(incoming_.size());
    incoming_.push_back(std::move(object));
    return index;
}

std::shared_ptr<Object> Request::GetIncomingHandle(u32 id) const {
    return id < incoming_.size() ? incoming_[id] : nullptr;
}

u32 Request::NextWord() {
    if (cursor_ < command_size_) {
        return cmd_[cursor_++];
    }
    ++cursor_;
    if (!cursor_warned_) {
        MH4N_LOG(Warning, kLog, "IPC read past the end of command 0x{:08x}", Header());
        cursor_warned_ = true;
    }
    return 0;
}

void Request::PopBytes(void* dst, std::size_t size) {
    auto bytes = static_cast<u8*>(dst);
    for (std::size_t index = 0; index < WordCount(size); ++index) {
        const u32 word = NextWord();
        const auto offset = index * sizeof(word);
        std::memcpy(bytes + offset, &word, std::min(sizeof(word), size - offset));
    }
}

std::vector<std::shared_ptr<Object>> Request::PopHandleDescriptor(std::size_t expected) {
    const u32 word = NextWord();
    const auto type = ipc::GetDescriptorType(word);
    const u32 count = ipc::HandleNumberFromDesc(word);
    if ((type != ipc::CopyHandle && type != ipc::MoveHandle) || count != expected ||
        cursor_ > command_size_ || count > command_size_ - cursor_) {
        MH4N_LOG(Warning, kLog, "expected an IPC descriptor with {} handles", expected);
        return {};
    }
    std::vector<std::shared_ptr<Object>> objects;
    objects.reserve(count);
    for (u32 index = 0; index < count; ++index) {
        objects.push_back(GetIncomingHandle(NextWord()));
    }
    return objects;
}

u32 Request::PopPid() {
    const u32 descriptor = NextWord();
    const u32 value = NextWord();
    if (descriptor != ipc::CallingPidDesc()) {
        MH4N_LOG(Warning, kLog, "expected an IPC calling PID descriptor");
        return 0;
    }
    return value;
}

const StaticBuffer* Request::PopStaticBuffer() {
    const u32 descriptor = NextWord();
    const u32 address = NextWord();
    if (ipc::GetDescriptorType(descriptor) == ipc::StaticBuffer) {
        const auto id = ipc::StaticBufferDescId(descriptor);
        const auto size = ipc::StaticBufferDescSize(descriptor);
        const auto found = std::find_if(static_in_.begin(), static_in_.end(),
                                        [=](const auto& buffer) {
            return buffer.id == id && buffer.address == address && buffer.size == size;
        });
        if (found != static_in_.end()) {
            return &*found;
        }
    }
    MH4N_LOG(Warning, kLog, "expected an IPC static buffer descriptor");
    return nullptr;
}

const MappedBuffer* Request::PopMappedBuffer() {
    const u32 descriptor = NextWord();
    const u32 id = NextWord();
    if (ipc::GetDescriptorType(descriptor) == ipc::MappedBuffer && id < mapped_.size()) {
        return &mapped_[id];
    }
    MH4N_LOG(Warning, kLog, "expected an IPC mapped buffer descriptor");
    return nullptr;
}

Reply Request::NewReply(u32 normal_params_size, u32 translate_params_size) const {
    Reply reply(CommandId(), normal_params_size, translate_params_size);
    reply.SeedFrom(Words());
    return reply;
}

Reply& Request::MakeReply(u32 normal_params_size, u32 translate_params_size) {
    return reply_.emplace(NewReply(normal_params_size, translate_params_size));
}

std::vector<u32> Request::WriteReply(const Reply& reply) {
    reply.Validate();
    const auto seeded = reply.SeededWords(Words());
    const auto count = std::min<std::size_t>(reply.CommandSize(), seeded.size());
    std::vector<u32> output(seeded.begin(), seeded.begin() + count);
    const u32 address = thread_->tls_address + kCommandOffset;

    for (std::size_t index = 1 + ipc::Header{output.front()}.NormalParamsSize();
         index < output.size();) {
        const u32 descriptor = output[index++];
        const auto type = ipc::GetDescriptorType(descriptor);
        const bool handles = type == ipc::CopyHandle || type == ipc::MoveHandle;
        const u32 payload = handles ? ipc::HandleNumberFromDesc(descriptor) : 1;
        if (payload > output.size() - index) {
            MH4N_LOG(Warning, kLog, "truncated IPC reply descriptor");
            break;
        }

        switch (type) {
        case ipc::CopyHandle:
        case ipc::MoveHandle:
            for (u32 part = 0; part < payload; ++part) {
                u32& word = output[index + part];
                const auto id = std::exchange(word, 0);
                if (process_ && id < reply.Objects().size() && reply.Objects()[id]) {
                    const auto result = process_->handle_table.Create(&word, reply.Objects()[id]);
                    if (result != kResultSuccess) {
                        word = 0;
                        MH4N_LOG(Warning, kLog, "could not create IPC reply handle: 0x{:08x}", result);
                    }
                }
            }
            break;
        case ipc::CallingPid:
            output[index] = pid_;
            break;
        case ipc::StaticBuffer: {
            const auto id = ipc::StaticBufferDescId(descriptor);
            const u32 slot = address + static_cast<u32>(ipc::StaticBufferDescriptorWord(id) * 4);
            const u32 capacity = ipc::StaticBufferDescSize(Memory().Read<u32>(slot).value_or(0));
            const u32 destination = Memory().Read<u32>(slot + 4).value_or(0);
            output[index] = destination;
            const auto& data = reply.StaticData(id);
            if (data && data->size() > capacity) {
                MH4N_LOG(Warning, kLog, "static buffer {} has {} bytes and the client's receive buffer {}",
                         id, data->size(), capacity);
            }
            const u32 size = data ? static_cast<u32>(std::min<std::size_t>(capacity, data->size())) : 0;
            if (size && !Memory().WriteBlock(destination, data->data(), size)) {
                MH4N_LOG(Warning, kLog, "static receive buffer {} is not mapped", id);
            }
            break;
        }
        case ipc::MappedBuffer: {
            const u32 id = output[index];
            output[index] = id < mapped_.size() ? mapped_[id].Address() : 0;
            break;
        }
        default:
            MH4N_LOG(Warning, kLog, "unsupported IPC reply descriptor 0x{:08x}", descriptor);
            break;
        }
        index += payload;
    }
    if (!Memory().WriteBlock(address, output.data(), static_cast<u32>(output.size() * sizeof(u32)))) {
        MH4N_LOG(Warning, kLog, "IPC reply command buffer is not mapped");
    }
    return output;
}

void Request::ReleaseKernelObjects() {
    replied_hook_ = {};
    reply_.reset();
    for (auto& descriptor : descriptors_) {
        descriptor.objects.clear();
    }
    incoming_.clear();
    session_.reset();
    process_.reset();
    thread_.reset();
}

void Request::Cancel() {
    ReleaseKernelObjects();
    cancelled_ = true;
}

void Request::Deliver(Reply reply) {
    if (Finished()) {
        MH4N_LOG(Warning, kLog, "IPC request {} already completed", sequence_);
        return;
    }
    if (!Cancelled() && thread_ && thread_->status == ThreadStatus::WaitIPC) {
        const auto words = WriteReply(reply);
        if (replied_hook_) {
            replied_hook_(*this, words);
        }
        thread_->ResumeFromWait(kResultSuccess);
    }
    ReleaseKernelObjects();
    MarkFinished();
}

void Complete(const std::shared_ptr<Request>& request, Reply reply) {
    auto pending = std::make_shared<Reply>(std::move(reply));
    request->GetKernel().Post([request, pending] { request->Deliver(std::move(*pending)); });
}

}  // namespace mh4n
