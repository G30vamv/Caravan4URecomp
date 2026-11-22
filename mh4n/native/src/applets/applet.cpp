// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: applet contracts, apt_test.cpp and applet_frontend_test.cpp black-box fixtures.

#include "applet.h"

#include <cstring>

#include "mh4n/log.h"

namespace mh4n::apt {

Result Applet::ReceiveParameter(const MessageParameter& parameter) {
    switch (parameter.signal) {
    case SignalType::Wakeup:
        return Start(parameter);
    case SignalType::WakeupByExit:
    case SignalType::WakeupByCancel:
    case SignalType::WakeupByCancelAll:
        return Finalize();
    default:
        return ReceiveParameterImpl(parameter);
    }
}

void Applet::SendParameter(const MessageParameter& parameter) {
    if (auto host = Host()) {
        host->CancelAndSendParameter(parameter);
    }
}

void Applet::CloseApplet(std::shared_ptr<Object> object, const std::vector<u8>& buffer) {
    is_running_ = false;
    is_active_ = false;
    if (auto host = Host()) {
        host->PrepareToCloseLibraryApplet(true, true, false);
        host->CloseLibraryApplet(std::move(object), buffer);
    }
}

Result Applet::AnswerFramebufferRequest(ServiceContext& context, const MessageParameter& parameter,
                                       std::shared_ptr<SharedMemory>& memory, const char* name) {
    constexpr const char* kLog = "Service.APT";
    if (parameter.signal != SignalType::Request) {
        MH4N_LOG(Warning, kLog, "{}: unsupported signal {:08X}", name,
                 static_cast<u32>(parameter.signal));
        return kResultUnknown;
    }
    if (parameter.buffer.size() != sizeof(CaptureBufferInfo)) {
        MH4N_LOG(Warning, kLog,
                 "{}: expected a Request parameter of {} bytes, but the Request parameter has {} bytes",
                 name, sizeof(CaptureBufferInfo), parameter.buffer.size());
        return kResultUnknown;
    }

    CaptureBufferInfo capture{};
    std::memcpy(&capture, parameter.buffer.data(), sizeof(capture));
    const Result result = context.GetKernel().NewSharedMemory(
        &memory, nullptr, capture.size, MemoryPermission::ReadWrite,
        MemoryPermission::ReadWrite, 0, FcramRegion::Base, name);
    if (!IsError(result)) {
        SendParameter({id_, parameter.sender_id, SignalType::Response, memory, {}});
    }
    return result;
}

}  // namespace mh4n::apt
