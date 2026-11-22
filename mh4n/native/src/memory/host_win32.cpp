// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: host.h, memory_test.cpp, and Microsoft Learn Win32 virtual memory and exception handling documentation.

#ifdef _WIN32

#include "host.h"

#include <cstdlib>
#include <windows.h>

#include "mh4n/assert.h"

namespace mh4n::host {
namespace {

LONG CALLBACK OnException(EXCEPTION_POINTERS* details) {
    const EXCEPTION_RECORD& record = *details->ExceptionRecord;
    if ((record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
         record.ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        record.NumberParameters >= 2) {
        FaultAccess access = FaultAccess::Unknown;
        switch (record.ExceptionInformation[0]) {
        case 0:
            access = FaultAccess::Read;
            break;
        case 1:
            access = FaultAccess::Write;
            break;
        }
        fault::HandleFault(record.ExceptionInformation[1], access);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

std::size_t PageSize() {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return info.dwPageSize;
}

void* Reserve(std::size_t size) {
    return VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
}

void Release(void* base, std::size_t) {
    MH4N_ASSERT(VirtualFree(base, 0, MEM_RELEASE) != 0);
}

bool Commit(void* addr, std::size_t size) {
    return VirtualAlloc(addr, size, MEM_COMMIT, PAGE_READWRITE) == addr;
}

bool Decommit(void* addr, std::size_t size) {
    return VirtualFree(addr, size, MEM_DECOMMIT) != 0;
}

void InstallFaultHandler() {
    static std::once_flag installed;
    std::call_once(installed, [] {
        MH4N_ASSERT(AddVectoredExceptionHandler(1, OnException) != nullptr);
    });
}

void InstallThreadFaultStack() {}

} // namespace mh4n::host

#endif
