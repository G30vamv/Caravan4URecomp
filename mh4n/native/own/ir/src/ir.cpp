// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: 3dbrew IR Services, IRUSER Shared Memory and Circle Pad Pro, boot-village traces and MH4U's IR client.

#include "ir_packets.h"

#include <optional>

#include "mh4n/clock.h"
#include "mh4n/registry.h"
#include "mh4n/service.h"

namespace mh4n {
namespace {

using namespace ir_user;

constexpr Result kNoData = MakeResult(1007, 67, 4, 25);
constexpr Result kInvalidSize = MakeResult(1004, 67, 7, 28);
constexpr Result kInvalidState = MakeResult(1023, 67, 5, 25);
constexpr Result kBufferFull = MakeResult(1003, 67, 3, 25);
constexpr u8 kCirclePadProTarget = 1;

// MH4U checks for shared connection status 2 at 0x0047d070.
enum class Connection : u8 { Disconnected = 0, Connected = 2 };

void ReplyResult(Request& request, Result result = kResultSuccess) {
    request.MakeReply(1, 0).Push(result);
}

void ReplyEvent(Request& request, const std::shared_ptr<Event>& event) {
    auto& reply = request.MakeReply(1, 2);
    reply.Push(kResultSuccess);
    reply.PushCopyObjects(event);
}

class IrUser final : public Service {
public:
    explicit IrUser(ServiceContext& context)
        : Service(context, "ir:USER"),
          connection_event_(context.GetKernel().NewEvent(ResetType::OneShot, "IR connection")),
          receive_event_(context.GetKernel().NewEvent(ResetType::OneShot, "IR receive")),
          send_event_(context.GetKernel().NewEvent(ResetType::OneShot, "IR send")) {
        // 3dbrew IR Services supplies the command IDs and names.
        Register(0x0001, "InitializeIrnop");
        Register(0x0002, "FinalizeIrnop", &IrUser::Finalize);  // seq=2108
        Register(0x0003, "ClearReceiveBuffer");
        Register(0x0004, "ClearSendBuffer");
        Register(0x0005, "WaitConnection");
        Register(0x0006, "RequireConnection", &IrUser::RequireConnection);  // seq=2103,2112
        Register(0x0007, "AutoConnection");
        Register(0x0008, "AnyConnection");
        Register(0x0009, "Disconnect", &IrUser::Disconnect);  // seq=2107
        Register(0x000a, "GetReceiveEvent", &IrUser::GetReceiveEvent);  // seq=2104,2113
        Register(0x000b, "GetSendEvent", &IrUser::GetSendEvent);  // seq=2114
        Register(0x000c, "GetConnectionStatusEvent", &IrUser::GetConnectionEvent);  // seq=2102,2111
        Register(0x000d, "SendIrnop", &IrUser::Send);  // seq=2105,2115
        Register(0x000e, "SendIrnopLarge");
        Register(0x000f, "ReceiveIrnop");
        Register(0x0010, "ReceiveIrnopLarge");
        Register(0x0011, "GetLatestReceiveErrorResult");
        Register(0x0012, "GetLatestSendErrorResult");
        Register(0x0013, "GetConnectionStatus");
        Register(0x0014, "GetTryingToConnectStatus");
        Register(0x0015, "GetReceiveSizeFreeAndUsed");
        Register(0x0016, "GetSendSizeFreeAndUsed");
        Register(0x0017, "GetConnectionRole");
        Register(0x0018, "InitializeIrnopShared", &IrUser::Initialize);  // seq=2101,2110
        Register(0x0019, "ReleaseReceivedData", &IrUser::Release);  // seq=2106,2117
        Register(0x001a, "SetOwnMachineId");
    }

    ~IrUser() override { StopPolling(); }

private:
    std::span<u8> Shared() {
        // A guest Map or Unmap can move the backing store between requests.
        return {shared_->GetPointer(), shared_size_};
    }

    void Initialize(Request& request) {
        const u32 shared_size = request.Pop<u32>();
        const u32 receive_size = request.Pop<u32>();
        const u32 receive_slots = request.Pop<u32>();
        const u32 send_size = request.Pop<u32>();
        const u32 send_slots = request.Pop<u32>();
        request.Pop<u8>();  // Bitrate has no effect on the virtual link.
        auto shared = request.PopObject<SharedMemory>();
        if (!shared) {
            ReplyResult(request, kResultInvalidHandle);
            return;
        }
        const u64 layout_size = kStatusSize + 2 * kBufferInfoSize + u64{receive_size} + send_size;
        if (layout_size > shared_size || shared_size > shared->Size() ||
            !PacketQueue::ValidLayout(receive_size, receive_slots) ||
            !PacketQueue::ValidLayout(send_size, send_slots)) {
            ReplyResult(request, kInvalidSize);
            return;
        }

        Reset();
        shared_ = std::move(shared);
        shared_size_ = shared_size;
        receive_ = PacketQueue(kStatusSize, receive_size, receive_slots);
        send_ = PacketQueue(kStatusSize + kBufferInfoSize + receive_size, send_size, send_slots);
        auto bytes = Shared();
        std::fill_n(bytes.begin(), static_cast<u32>(layout_size), u8{0});
        bytes[0x0e] = 1;
        ReplyResult(request);
    }

    void Finalize(Request& request) {
        Reset();
        ReplyResult(request);
    }

    void Reset() {
        CloseConnection();
        if (shared_) {
            Shared()[0x0e] = 0;
            shared_.reset();
            receive_ = {};
            send_ = {};
        }
        connection_event_->Clear();
    }

    void RequireConnection(Request& request) {
        const u8 target = request.Pop<u8>();
        if (!shared_) {
            ReplyResult(request, kInvalidState);
            return;
        }
        if (target != kCirclePadProTarget) {
            ReplyResult(request, kNoData);
            return;
        }

        if (connection_ != Connection::Connected) {
            connection_ = Connection::Connected;
            auto bytes = Shared();
            bytes[0x08] = static_cast<u8>(connection_);
            bytes[0x0a] = 1;
            bytes[0x0c] = target;
            bytes[0x0d] = kNetworkId;
            connection_event_->Signal();
        }
        ReplyResult(request);
    }

    void Disconnect(Request& request) {
        CloseConnection();
        ReplyResult(request);
    }

    void CloseConnection() {
        StopPolling();
        if (shared_) {
            auto bytes = Shared();
            bytes[0x08] = static_cast<u8>(Connection::Disconnected);
            bytes[0x0a] = bytes[0x0c] = bytes[0x0d] = 0;
            receive_.Clear(bytes);
            send_.Clear(bytes);
        }
        receive_event_->Clear();
        send_event_->Clear();
        if (connection_ != Connection::Disconnected) {
            connection_ = Connection::Disconnected;
            connection_event_->Signal();
        }
    }

    void GetConnectionEvent(Request& request) { ReplyEvent(request, connection_event_); }
    void GetReceiveEvent(Request& request) { ReplyEvent(request, receive_event_); }
    void GetSendEvent(Request& request) { ReplyEvent(request, send_event_); }

    void Send(Request& request) {
        const u32 length = request.Pop<u32>();
        const StaticBuffer* buffer = request.PopStaticBuffer();
        if (connection_ != Connection::Connected) {
            ReplyResult(request, kInvalidState);
            return;
        }
        if (!buffer || length == 0 || length > 0xfc || length > buffer->data.size()) {
            ReplyResult(request, kInvalidSize);
            return;
        }

        const auto payload = std::span{buffer->data}.first(length);
        auto bytes = Shared();
        if (!send_.Push(bytes, Frame(payload))) {
            ReplyResult(request, kBufferFull);
            return;
        }
        send_.Release(bytes, 1);
        send_event_->Signal();

        // 3dbrew Circle Pad Pro. Both request payloads occur in boot-village.
        if (payload[0] == 0x01 && payload.size() == 3 && payload[1] != 0) {
            StartPolling(payload[1]);
        } else if (payload[0] == 0x02 && payload.size() == 6) {
            const auto calibration = CalibrationPayload(payload.first<6>());
            if (!calibration.empty()) {
                Receive(calibration);
            }
        }
        ReplyResult(request);
    }

    void Release(Request& request) {
        const u32 count = request.Pop<u32>();
        if (!shared_) {
            ReplyResult(request, kInvalidState);
            return;
        }
        ReplyResult(request, receive_.Release(Shared(), count) ? kResultSuccess : kNoData);
    }

    void Receive(std::span<const u8> payload) {
        if (receive_.Push(Shared(), Frame(payload))) {
            receive_event_->Signal();
        }
    }

    void StartPolling(u8 milliseconds) {
        const u64 period = kTicksPerSecond * milliseconds / 1000;
        // Repeating a request for the same period leaves the current sample deadline in place.
        if (period_ != period) {
            StopPolling();
            period_ = period;
            ScheduleSample(Context().GetKernel().NowTicks() + period);
        }
    }

    void ScheduleSample(u64 deadline) {
        auto& kernel = Context().GetKernel();
        poll_ = kernel.ScheduleAt(deadline, [this, deadline] {
            const u64 now = Context().GetKernel().NowTicks();
            auto* input = Context().Interfaces().input;
            Receive(InputPayload(input ? input->Sample(now) : InputState{}));
            // Keep the phase when the scheduler is late and skip periods already missed.
            const u64 late = now > deadline ? now - deadline : 0;
            ScheduleSample(deadline + (late / period_ + 1) * period_);
        });
    }

    void StopPolling() {
        if (poll_) {
            Context().GetKernel().Cancel(*poll_);
            poll_.reset();
        }
        period_ = 0;
    }

    std::shared_ptr<Event> connection_event_;
    std::shared_ptr<Event> receive_event_;
    std::shared_ptr<Event> send_event_;
    std::shared_ptr<SharedMemory> shared_;
    u32 shared_size_ = 0;
    PacketQueue receive_;
    PacketQueue send_;
    Connection connection_ = Connection::Disconnected;
    std::optional<u64> poll_;
    u64 period_ = 0;
};

}  // namespace

MH4N_REGISTER(ServiceFactory, "new/ir:USER", ServiceFactory{
    [](ServiceContext& context) { return std::make_unique<IrUser>(context); }, 2});

}  // namespace mh4n
