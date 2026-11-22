// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: 3dbrew Services, srv_test.cpp, service.h, boot-village.ipc.trace.

#include "mh4n/log.h"
#include "mh4n/registry.h"
#include "mh4n/service.h"

namespace mh4n {
namespace {

constexpr auto kLog = "Service.SRV";

class Srv final : public Service {
public:
    explicit Srv(ServiceContext& context) : Service(context, "srv:") {
        Register(0x01, "RegisterClient", &Srv::RegisterClient);
        Register(0x02, "EnableNotification", &Srv::EnableNotification);
        Register(0x03, "RegisterService");
        Register(0x04, "UnregisterService");
        Register(0x05, "GetServiceHandle", &Srv::GetServiceHandle);
        Register(0x06, "RegisterPort");
        Register(0x07, "UnregisterPort");
        Register(0x08, "GetPort");
        Register(0x09, "Subscribe", &Srv::Acknowledge);
        Register(0x0A, "Unsubscribe", &Srv::Acknowledge);
        Register(0x0B, "ReceiveNotification", &Srv::Acknowledge);
        Register(0x0C, "PublishToSubscriber", &Srv::PublishToSubscriber);
        Register(0x0D, "PublishAndGetSubscriber");
        Register(0x0E, "IsServiceRegistered");
    }

private:
    void RegisterClient(Request& request) {
        const auto& descriptors = request.Descriptors();
        const bool valid = !descriptors.empty() && descriptors.front().word == ipc::CallingPidDesc();
        request.MakeReply(1, 0).Push(valid ? kResultSuccess : kResultInvalidBufferDescriptor);
    }

    void EnableNotification(Request& request) {
        if (!notifications_) {
            const Result result = Context().GetKernel().NewSemaphore(&notifications_, 0, 16);
            if (result != kResultSuccess) {
                request.MakeReply(1, 0).Push(result);
                return;
            }
        }
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        reply.PushCopyObjects(notifications_);
    }

    Handled GetServiceHandle(Request& request) {
        const auto bytes = request.PopRaw<std::array<char, kMaxServiceNameLength>>();
        const u32 length = request.Pop<u32>();
        const u32 flags = request.Pop<u32>();
        if (length == 0 || length > bytes.size()) {
            request.MakeReply(1, 0).Push(kResultSrvInvalidNameSize);
            return Handled::Done;
        }
        std::string name(bytes.data(), length);
        auto& manager = Context().Services();
        std::shared_ptr<ClientSession> session;
        const Result result = manager.ConnectToService(name, &session);
        if (result == kResultSrvServiceNotRegistered && (flags & 1) == 0) {
            MH4N_LOG(Info, kLog, "GetServiceHandle service={} delayed", name);
            manager.NotifyWhenRegistered(name, [&manager, name, pending = request.Share()] {
                if (pending->Cancelled()) {
                    return;
                }
                std::shared_ptr<ClientSession> connected;
                const Result status = manager.ConnectToService(name, &connected);
                Reply reply = pending->NewReply(1, connected ? 2 : 0);
                ReplyWithSession(reply, status, std::move(connected));
                Complete(pending, std::move(reply));
            });
            return Handled::Deferred;
        }
        Reply& reply = request.MakeReply(1, session ? 2 : 0);
        ReplyWithSession(reply, result, std::move(session));
        return Handled::Done;
    }

    static void ReplyWithSession(Reply& reply, Result result, std::shared_ptr<ClientSession> session) {
        reply.Push(result);
        if (session) {
            reply.PushMoveObjects(std::move(session));
        }
    }

    void Acknowledge(Request& request) {
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    void PublishToSubscriber(Request& request) {
        const u32 notification = request.Pop<u32>();
        Acknowledge(request);
        if (notification == 0x203) {
            Context().RequestExit(0, "srv: power-off notification");
        }
    }

    std::shared_ptr<Semaphore> notifications_;
};

MH4N_REGISTER(ServiceFactory, "srv:",
              ServiceFactory::NamedPort([](ServiceContext& context) {
                  return std::make_unique<Srv>(context);
              }, 64));

}  // namespace
}  // namespace mh4n
