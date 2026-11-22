// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: 3dbrew ErrDisp and ERR:Throw, srv_test.cpp, service.h.

#include <array>
#include <ctime>
#include <string_view>

#include "mh4n/log.h"
#include "mh4n/registry.h"
#include "mh4n/service.h"

namespace mh4n {
namespace {

constexpr auto kLog = "Service.ERR";

constexpr std::array kErrorTypes{
    "Generic", "Corrupted", "CardRemoved", "Exception", "ResultFailure", "Logged",
};
constexpr std::array kExceptionTypes{
    "Prefetch Abort", "Data Abort", "Undefined Exception", "Vector Floating Point Exception",
};

struct ExceptionInfo {
    u32 type;
    u32 status;
    u32 address;
    u32 fpexc;
    u32 fpinst;
    u32 fpinst2;
    std::array<u32, 16> registers;
    u32 cpsr;
};

void LogException(const ExceptionInfo& info) {
    const char* type = info.type < kExceptionTypes.size()
                           ? kExceptionTypes[info.type] : "Unknown Exception";
    MH4N_LOG(Critical, kLog, "EXCEPTION TYPE: {}", type);
    switch (info.type) {
    case 0:
        MH4N_LOG(Critical, kLog, "IFSR: 0x{:08X}, r15: 0x{:08X}", info.status, info.address);
        break;
    case 1:
        MH4N_LOG(Critical, kLog, "DFSR: 0x{:08X}, DFAR: 0x{:08X}", info.status, info.address);
        break;
    case 3:
        MH4N_LOG(Critical, kLog, "FPEXC: 0x{:08X}, FINST: 0x{:08X}, FINST2: 0x{:08X}",
                 info.fpexc, info.fpinst, info.fpinst2);
        break;
    }
    MH4N_LOG(Critical, kLog, "ARM Registers:");
    for (unsigned i = 0; i < info.registers.size(); ++i) {
        MH4N_LOG(Critical, kLog, "r{}: 0x{:08X}", i, info.registers[i]);
    }
    MH4N_LOG(Critical, kLog, "SP=0x{:08X} LR=0x{:08X} PC=0x{:08X} CPSR=0x{:08X}",
             info.registers[13], info.registers[14], info.registers[15], info.cpsr);
    MH4N_LOG(Critical, kLog, "{} at PC=0x{:08X} LR=0x{:08X}",
             type, info.registers[15], info.registers[14]);
}

class Err final : public Service {
public:
    explicit Err(ServiceContext& context) : Service(context, "err:f") {
        Register(0x01, "Throw", &Err::Throw);
        Register(0x02, "SetUserString");
    }

private:
    void Throw(Request& request) {
        if (request.NormalParamsSize() != 32 || request.TranslateParamsSize() != 0) {
            request.MakeReply(1, 0).Push(kResultInvalidBufferDescriptor);
            return;
        }

        const u32 revision = request.Pop<u32>();
        const u32 type = revision & 0xFF;
        const Result result = request.Pop<Result>();
        const u32 address = request.Pop<u32>();
        const u32 pid = request.Pop<u32>();
        const u64 title = request.Pop<u64>();
        const u64 application = request.Pop<u64>();
        const char* name = type < kErrorTypes.size() ? kErrorTypes[type] : "Unknown Error Type";
        MH4N_LOG(Critical, kLog, "Fatal error type: {}", name);
        MH4N_LOG(Critical, kLog, "PID: 0x{:08X}", pid);
        MH4N_LOG(Critical, kLog, "REV: 0x{:08X}_0x{:08X}", (revision >> 8) & 0xFF, revision >> 16);
        MH4N_LOG(Critical, kLog, "TID: 0x{:08X}_0x{:08X}", title >> 32, static_cast<u32>(title));
        MH4N_LOG(Critical, kLog, "AID: 0x{:08X}_0x{:08X}", application >> 32,
                 static_cast<u32>(application));
        MH4N_LOG(Critical, kLog, "ADR: 0x{:08X}", address);
        MH4N_LOG(Critical, kLog, "RSL: 0x{:08X}", result);
        MH4N_LOG(Critical, kLog, "  Level: {}\n  Summary: {}\n  Module: {}\n  Desc: {}",
                 result >> 27, (result >> 21) & 0x3F, (result >> 10) & 0xFF, result & 0x3FF);
        if (type == 3) {
            LogException(request.PopRaw<ExceptionInfo>());
        } else if (type == 4) {
            const auto message = request.PopRaw<std::array<char, 0x60>>();
            const std::string_view text(message.data(), message.size());
            MH4N_LOG(Critical, kLog, "Failure Message: {}", text.substr(0, text.find('\0')));
        }
        if (type < kErrorTypes.size()) {
            const auto now = std::time(nullptr);
            std::array<char, 32> timestamp{};
            if (const auto* utc = std::gmtime(&now)) {
                std::strftime(timestamp.data(), timestamp.size(), "%Y-%m-%d %H:%M:%S UTC", utc);
            }
            MH4N_LOG(Critical, kLog, "Datetime: {}", timestamp.data());
        }

        request.MakeReply(1, 0).Push(kResultSuccess);
        Context().RequestExit(3, fmt::format("fatal error through err:f: type {}, result 0x{:08X}, "
                                             "address 0x{:08X}", name, result, address));
    }
};

MH4N_REGISTER(ServiceFactory, "err:f",
              ServiceFactory::NamedPort([](ServiceContext& context) {
                  return std::make_unique<Err>(context);
              }, 1));

}  // namespace
}  // namespace mh4n
