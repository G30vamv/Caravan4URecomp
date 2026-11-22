// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: 3dbrew BOSS Services, other_services_test.cpp, IPC replay observations and the service contracts.

#include <algorithm>
#include <array>
#include <map>
#include <vector>

#include "mh4n/registry.h"
#include "mh4n/service.h"

namespace mh4n {
namespace {

constexpr Result kUnknown = 0xFFFFFFFF;
constexpr Result kTaskMissing = MakeResult(51, 62, 5, 25);
constexpr Result kNoNsData = MakeResult(64, 62, 5, 25);
constexpr Result kInvalidProperty = MakeResult(77, 62, 11, 25);

using TaskId = std::array<u8, 8>;
using TaskList = std::array<TaskId, 128>;
constexpr u16 kTaskCountProperty = 0x35;
constexpr u16 kTaskListProperty = 0x36;

u32 WritablePropertySize(u16 property) {
    constexpr std::array<u32, 0x1D> sizes{
        1, 1, 4, 4, 4, 1, 1, 0x200, 4, 1, 0x100, 0x200, 0, 0x360, 4, 12,
        1, 1, 1, 4, 4, 0x40, 4, 0, 1, 1, 1, 4, 4,
    };
    if (property < sizes.size()) {
        return sizes[property];
    }
    switch (property) {
    case 0x3B: return 4;
    case 0x3E: return 0x200;
    case 0x3F: return 1;
    default: return 0;
    }
}

struct BossFlags {
    u8 optout = 0;
    u8 new_flag = 0;
    u8 privileged_new_flag = 0;
};

class Boss final : public Service {
public:
    Boss(ServiceContext& context, bool privileged)
        : Service(context, privileged ? "boss:P" : "boss:U"),
          flags_(context.State<BossFlags>("boss")) {
        Register(0x01, "InitializeSession", &Boss::Initialize);
        Register(0x02, "SetStorageInfo", &Boss::Acknowledge);
        Register(0x03, "UnregisterStorage", &Boss::Acknowledge);
        Register(0x04, "GetStorageInfo", &Boss::ZeroValue);
        Register(0x05, "RegisterPrivateRootCa", &Boss::OfflineBuffers<0>);
        Register(0x06, "RegisterPrivateClientCert", &Boss::OfflineBuffers<0>);
        Register(0x07, "GetNewArrivalFlag", &Boss::ZeroValue);
        Register(0x08, "RegisterNewArrivalEvent", &Boss::Acknowledge);
        Register(0x09, "SetOptoutFlag", &Boss::SetOptout);
        Register(0x0A, "GetOptoutFlag", &Boss::GetOptout);
        Register(0x0B, "RegisterTask", &Boss::ChangeTasks);
        Register(0x0C, "UnregisterTask", &Boss::ChangeTasks);
        Register(0x0D, "ReconfigureTask", &Boss::OfflineBuffers<0>);
        Register(0x0E, "GetTaskIdList", &Boss::GetTaskIdList);
        Register(0x0F, "GetStepIdList", &Boss::OfflineBuffers<0>);
        Register(0x10, "GetNsDataIdList", &Boss::GetNsDataIdList);
        Register(0x11, "GetNsDataIdList1", &Boss::GetNsDataIdList);
        Register(0x12, "GetNsDataIdList2", &Boss::GetNsDataIdList);
        Register(0x13, "GetNsDataIdList3", &Boss::GetNsDataIdList);
        Register(0x14, "SendProperty", &Boss::SendProperty);
        Register(0x15, "SendPropertyHandle", &Boss::Acknowledge);
        Register(0x16, "ReceiveProperty", &Boss::ReceiveProperty);
        Register(0x17, "UpdateTaskInterval", &Boss::OfflineBuffers<0>);
        Register(0x18, "UpdateTaskCount", &Boss::OfflineBuffers<0>);
        Register(0x19, "GetTaskInterval", &Boss::OfflineBuffers<1>);
        Register(0x1A, "GetTaskCount", &Boss::OfflineBuffers<1>);
        Register(0x1B, "GetTaskServiceStatus", &Boss::OfflineBuffers<1, 1>);
        Register(0x1C, "StartTask", &Boss::OfflineBuffers<0>);
        Register(0x1D, "StartTaskImmediate", &Boss::OfflineBuffers<0>);
        Register(0x1E, "CancelTask", &Boss::OfflineBuffers<0>);
        Register(0x1F, "GetTaskFinishHandle", &Boss::GetTaskFinishHandle);
        Register(0x20, "GetTaskState", &Boss::OfflineBuffers<3>);
        Register(0x21, "GetTaskResult", &Boss::OfflineBuffers<3>);
        Register(0x22, "GetTaskCommErrorCode", &Boss::OfflineBuffers<3>);
        Register(0x23, "GetTaskStatus", &Boss::OfflineBuffers<1>);
        Register(0x24, "GetTaskError", &Boss::OfflineBuffers<1>);
        Register(0x25, "GetTaskInfo", &Boss::OfflineBuffers<0>);
        Register(0x26, "DeleteNsData", &Boss::Acknowledge);
        Register(0x27, "GetNsDataHeaderInfo", &Boss::GetNsDataHeaderInfo);
        Register(0x28, "ReadNsData", &Boss::NoNsData);
        Register(0x29, "SetNsDataAdditionalInfo", &Boss::Acknowledge);
        Register(0x2A, "GetNsDataAdditionalInfo", &Boss::ZeroValue);
        Register(0x2B, "SetNsDataNewFlag", &Boss::SetNewFlag);
        Register(0x2C, "GetNsDataNewFlag", &Boss::GetNewFlag);
        Register(0x2D, "GetNsDataLastUpdate", &Boss::Unknown);
        Register(0x2E, "GetErrorCode", &Boss::ZeroValue);
        Register(0x2F, "RegisterStorageEntry", &Boss::Acknowledge);
        Register(0x30, "GetStorageEntryInfo", &Boss::OfflineBuffers<2>);
        Register(0x31, "SetStorageOption", &Boss::Acknowledge);
        Register(0x32, "GetStorageOption", &Boss::OfflineBuffers<4>);
        Register(0x33, "StartBgImmediate", &Boss::OfflineBuffers<0>);
        Register(0x34, "GetTaskPriority", &Boss::OfflineBuffers<1>);
        Register(0x35, "RegisterImmediateTask", &Boss::OfflineBuffers<0>);
        Register(0x36, "SetTaskQuery", &Boss::OfflineBuffers<0>);
        Register(0x37, "GetTaskQuery", &Boss::OfflineBuffers<0>);

        if (privileged) {
            Register(0x401, "InitializeSessionPrivileged", &Boss::Initialize);
            Register(0x404, "GetAppNewFlag", &Boss::ZeroValue);
            Register(0x406, "SetOptoutFlagPrivileged");
            Register(0x407, "GetOptoutFlagPrivileged");
            Register(0x40D, "GetNsDataIdListPrivileged", &Boss::GetNsDataIdList);
            Register(0x40E, "GetNsDataIdListPrivileged1", &Boss::GetNsDataIdList);
            Register(0x413, "SendPropertyPrivileged", &Boss::OfflineBuffers<0>);
            Register(0x414, "ReceivePropertyPrivileged");
            Register(0x415, "DeleteNsDataPrivileged", &Boss::Acknowledge);
            Register(0x416, "GetNsDataHeaderInfoPrivileged", &Boss::GetNsDataHeaderInfo);
            Register(0x417, "ReadNsDataPrivileged", &Boss::ReadNsDataPrivileged);
            Register(0x41A, "SetNsDataNewFlagPrivileged", &Boss::SetNewFlag);
            Register(0x41B, "GetNsDataNewFlagPrivileged", &Boss::GetNewFlag);
        }
    }

private:
    void Acknowledge(Request& request) {
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    template <u32 Values, u32 First = 0>
    void OfflineBuffers(Request& request) {
        const auto& buffers = request.MappedBuffers();
        Reply& reply = request.MakeReply(1 + Values, static_cast<u32>(2 * buffers.size()));
        reply.Push(kResultSuccess);
        for (u32 i = 0; i < Values; ++i) {
            reply.Push(i == 0 ? First : 0u);
        }
        for (const auto& buffer : buffers) {
            reply.PushMappedBuffer(buffer);
        }
    }

    static void ReplyWithBuffer(Request& request, Result result, const MappedBuffer* buffer) {
        Reply& reply = request.MakeReply(1, buffer ? 2 : 0);
        reply.Push(result);
        if (buffer) {
            reply.PushMappedBuffer(*buffer);
        }
    }

    void Initialize(Request& request) {
        request.Pop<u64>();
        if (request.PopPid() == 0) {
            request.MakeReply(1, 0).Push(kResultInvalidBufferDescriptor);
            return;
        }
        initialized_ = true;
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    void ZeroValue(Request& request) {
        request.MakeReply(2, 0).Push(kResultSuccess, 0u);
    }

    void SetOptout(Request& request) {
        flags_->optout = request.Pop<u8>();
        Acknowledge(request);
    }

    void GetOptout(Request& request) {
        request.MakeReply(2, 0).Push(kResultSuccess, flags_->optout);
    }

    void SetNewFlag(Request& request) {
        const bool privileged = request.CommandId() == 0x41A;
        if (privileged) {
            request.Pop<u64>();
        }
        request.Pop<u32>();
        (privileged ? flags_->privileged_new_flag : flags_->new_flag) = request.Pop<u8>();
        Acknowledge(request);
    }

    void GetNewFlag(Request& request) {
        const u8 value = request.CommandId() == 0x41B ? flags_->privileged_new_flag : flags_->new_flag;
        request.MakeReply(2, 0).Push(kResultSuccess, value);
    }

    void ChangeTasks(Request& request) {
        if (!initialized_) {
            Unknown(request);
            return;
        }
        const bool adding = request.CommandId() == 0x0B;
        const u32 size = request.Pop<u32>();
        request.Pop<u8>();
        if (adding) {
            request.Pop<u8>();
        }

        const auto* buffer = request.PopMappedBuffer();
        TaskId id{};
        if (!buffer || size > id.size() || !buffer->Read(id.data(), 0, size)) {
            ReplyWithBuffer(request, adding && buffer ? kResultSuccess : kUnknown, buffer);
            return;
        }

        const auto existing = std::find(tasks_.begin(), tasks_.end(), id);
        Result result = kResultSuccess;
        if (adding) {
            if (existing == tasks_.end() && tasks_.size() < task_list_.size()) {
                tasks_.push_back(id);
            }
        } else if (existing != tasks_.end()) {
            tasks_.erase(existing);
        } else {
            result = kTaskMissing;
        }
        ReplyWithBuffer(request, result, buffer);
    }

    void GetTaskIdList(Request& request) {
        if (initialized_) {
            task_list_.fill({});
            std::copy(tasks_.begin(), tasks_.end(), task_list_.begin());
            task_count_ = static_cast<u16>(tasks_.size());
        }
        request.MakeReply(1, 0).Push(initialized_ ? kResultSuccess : kUnknown);
    }

    void SendProperty(Request& request) {
        if (!initialized_) {
            Unknown(request);
            return;
        }
        const u16 id = request.Pop<u16>();
        const u32 size = request.Pop<u32>();
        const auto* buffer = request.PopMappedBuffer();
        const u32 capacity = WritablePropertySize(id);
        if (capacity == 0 || !buffer) {
            ReplyWithBuffer(request, kInvalidProperty, buffer);
            return;
        }
        std::vector<u8> bytes(capacity);
        const bool copied = buffer->Read(bytes.data(), 0, std::min(size, capacity));
        if (copied) {
            properties_[id] = std::move(bytes);
        }
        ReplyWithBuffer(request, copied ? kResultSuccess : kUnknown, buffer);
    }

    void ReceiveProperty(Request& request) {
        if (!initialized_) {
            Unknown(request);
            return;
        }
        const u16 id = request.Pop<u16>();
        const u32 size = request.Pop<u32>();
        const auto* buffer = request.PopMappedBuffer();
        Result result = kInvalidProperty;
        if (buffer) {
            if (id == kTaskCountProperty) {
                result = WriteProperty(*buffer, size, &task_count_, sizeof(task_count_));
            } else if (id == kTaskListProperty) {
                result = WriteProperty(*buffer, size, task_list_.data(), sizeof(task_list_));
            } else if (const u32 capacity = WritablePropertySize(id)) {
                const auto saved = properties_.find(id);
                if (saved != properties_.end()) {
                    result = WriteProperty(*buffer, size, saved->second.data(), capacity);
                } else {
                    const std::vector<u8> zero(capacity);
                    result = WriteProperty(*buffer, size, zero.data(), capacity);
                }
            }
        }
        Reply& reply = request.MakeReply(2, buffer ? 2 : 0);
        reply.Push(result, size);
        if (buffer) {
            reply.PushMappedBuffer(*buffer);
        }
    }

    static Result WriteProperty(const MappedBuffer& buffer, u32 size, const void* bytes, u32 capacity) {
        return buffer.Write(bytes, 0, std::min(size, capacity)) ? kResultSuccess : kUnknown;
    }

    void GetTaskFinishHandle(Request& request) {
        if (!finish_) {
            finish_ = Context().GetKernel().NewEvent(ResetType::OneShot);
        }
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        reply.PushCopyObjects(finish_);
    }

    void NoNsData(Request& request) {
        request.MakeReply(1, 0).Push(kNoNsData);
    }

    void Unknown(Request& request) {
        request.MakeReply(1, 0).Push(kUnknown);
    }

    void GetNsDataIdList(Request& request) {
        const auto& buffers = request.MappedBuffers();
        if (buffers.empty()) {
            Unknown(request);
            return;
        }
        Reply& reply = request.MakeReply(3, 2);
        reply.Push(kResultSuccess, u16{0}, u16{0});
        reply.PushMappedBuffer(buffers.front());
    }

    void GetNsDataHeaderInfo(Request& request) {
        const auto& buffers = request.MappedBuffers();
        ReplyWithBuffer(request, request.CommandId() == 0x416 ? kResultSuccess : kNoNsData,
                        buffers.empty() ? nullptr : &buffers.front());
    }

    void ReadNsDataPrivileged(Request& request) {
        request.Pop<u64>();
        request.Pop<u32>();
        request.Pop<u64>();
        const u32 size = request.Pop<u32>();
        const auto* buffer = request.PopMappedBuffer();
        if (!buffer) {
            Unknown(request);
            return;
        }
        Reply& reply = request.MakeReply(3, 2);
        reply.Push(kResultSuccess, size, 0u);
        reply.PushMappedBuffer(*buffer);
    }

    std::shared_ptr<BossFlags> flags_;
    std::shared_ptr<Event> finish_;
    bool initialized_ = false;
    std::vector<TaskId> tasks_;
    TaskList task_list_{};
    u16 task_count_ = 0;
    std::map<u16, std::vector<u8>> properties_;
};

MH4N_REGISTER(ServiceFactory, "boss:U", [](ServiceContext& context) {
    return std::make_unique<Boss>(context, false);
});
MH4N_REGISTER(ServiceFactory, "boss:P", [](ServiceContext& context) {
    return std::make_unique<Boss>(context, true);
});

}  // namespace
}  // namespace mh4n
