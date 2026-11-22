// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: 3dbrew CECD Services, CECDU:OpenAndRead and golden boot-village.ipc.trace.

#include "mh4n/registry.h"
#include "mh4n/service.h"

namespace mh4n {
namespace {

class CecdUser final : public Service {
public:
    explicit CecdUser(ServiceContext& context) : Service(context, "cecd:u") {
        Register(0x0001, "Open");
        Register(0x0002, "Read");
        Register(0x0003, "ReadMessage");
        Register(0x0004, "ReadMessageWithHMAC");
        Register(0x0005, "Write");
        Register(0x0006, "WriteMessage");
        Register(0x0007, "WriteMessageWithHMAC");
        Register(0x0008, "Delete");
        Register(0x0009, "SetData");
        Register(0x000A, "ReadData");
        Register(0x000B, "Start");
        Register(0x000C, "Stop");
        Register(0x000D, "GetCecInfoBuffer");
        Register(0x000E, "GetCecdState");
        Register(0x000F, "GetCecInfoEventHandle");
        Register(0x0010, "GetChangeStateEventHandle");
        Register(0x0011, "OpenAndWrite");
        Register(0x0012, "OpenAndRead", &CecdUser::OpenAndRead);
        Register(0x001E, "GetEventLog");
        Register(0x001F, "GetEventLogStart");
        Register(0x0020, "GetEventLogEntryCount");
    }

private:
    // Golden seq=38626 and 3dbrew CECDU:OpenAndRead: return the output mapping with zero bytes read.
    void OpenAndRead(Request& request) {
        constexpr Result kOpenError = 0xC8810BEF;
        request.Pop<u32>(); // Buffer size
        request.Pop<u32>(); // Program ID
        request.Pop<u32>(); // Path type
        request.Pop<u32>(); // Open flags
        request.PopPid();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        if (!buffer || !buffer->Writable()) {
            request.MakeReply(1, 0).Push(kResultInvalidBufferDescriptor);
            return;
        }

        Reply& reply = request.MakeReply(2, 2);
        reply.Push(kOpenError, u32{0});
        reply.PushMappedBuffer(*buffer);
    }
};

MH4N_REGISTER(ServiceFactory, "new/cecd:u", [](ServiceContext& context) {
    return std::make_unique<CecdUser>(context);
});

} // namespace
} // namespace mh4n
