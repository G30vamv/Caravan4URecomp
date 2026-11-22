// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: mh4n/memory.h, host.h, memory_test.cpp, and POSIX write and _exit.

#include "host.h"

#include <string_view>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <cerrno>
#include <unistd.h>
#endif

namespace mh4n {
namespace {

struct FaultContext {
    const u32* registers;
    u32 thread_id;
};

constinit thread_local FaultContext context{};
constinit std::atomic<FatalHook> fatal_hook{};
static_assert(std::atomic<FatalHook>::is_always_lock_free);

struct Reservation {
    std::atomic<std::uintptr_t> base{};
    std::atomic<std::size_t> size{};
};

constinit std::array<Reservation, 64> reservations{};
std::mutex reservation_mutex;
static_assert(std::atomic<std::uintptr_t>::is_always_lock_free);
static_assert(std::atomic<std::size_t>::is_always_lock_free);

void WriteStderr(const char* text, std::size_t length) {
#ifdef _WIN32
    const HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(handle, text, static_cast<DWORD>(length), &written, nullptr);
    }
#else
    while (length != 0) {
        const auto written = ::write(STDERR_FILENO, text, length);
        if (written > 0) {
            text += written;
            length -= static_cast<std::size_t>(written);
        } else if (written == 0 || errno != EINTR) {
            break;
        }
    }
#endif
}

} // namespace

void SetFaultContext(const void* rctx, u32 thread_id) {
    context = {static_cast<const u32*>(rctx), thread_id};
}

void ClearFaultContext() {
    context = {};
}

void SetFatalHook(FatalHook hook) {
    fatal_hook.store(hook, std::memory_order_release);
}

std::size_t FormatFaultReport(char* out, std::size_t capacity, u32 guest_addr, FaultAccess access,
                              bool has_context, u32 thread_id, u32 lr) {
    std::size_t length = 0;
    const auto append = [&](std::string_view text) {
        for (char c : text) {
            if (length == capacity) {
                break;
            }
            out[length++] = c;
        }
    };
    const auto hex = [&](u32 value) {
        constexpr char digits[] = "0123456789abcdef";
        char buffer[8];
        for (unsigned i = 0; i < sizeof(buffer); ++i) {
            buffer[sizeof(buffer) - 1 - i] = digits[value & 0xf];
            value >>= 4;
        }
        append(std::string_view{buffer, sizeof(buffer)});
    };

    append("mh4n: bad guest memory access: ");
    switch (access) {
    case FaultAccess::Read:
        append("read");
        break;
    case FaultAccess::Write:
        append("write");
        break;
    default:
        append("access");
        break;
    }
    append(" at 0x");
    hex(guest_addr);
    if (has_context) {
        char decimal[10];
        auto* end = decimal + sizeof(decimal);
        auto* start = end;
        do {
            *--start = static_cast<char>('0' + thread_id % 10);
            thread_id /= 10;
        } while (thread_id != 0);
        append(" (guest thread ");
        append(std::string_view{start, static_cast<std::size_t>(end - start)});
        append(", lr 0x");
        hex(lr);
        append(")\n");
    } else {
        append(" (no guest thread)\n");
    }
    return length;
}

namespace fault {

bool RegisterReservation(std::uintptr_t base, std::size_t size) {
    if (base == 0 || size == 0) {
        return false;
    }
    const std::lock_guard lock(reservation_mutex);
    for (auto& reservation : reservations) {
        if (reservation.base.load() == 0) {
            reservation.size.store(size);
            reservation.base.store(base);
            return true;
        }
    }
    return false;
}

void UnregisterReservation(std::uintptr_t base) {
    const std::lock_guard lock(reservation_mutex);
    for (auto& reservation : reservations) {
        if (reservation.base.load() == base) {
            reservation.base.store(0);
            return;
        }
    }
}

bool HandleFault(std::uintptr_t host_addr, FaultAccess access) {
    for (const auto& reservation : reservations) {
        const auto base = reservation.base.load();
        if (base == 0 || host_addr < base || host_addr - base >= reservation.size.load() ||
            base != reservation.base.load()) {
            continue;
        }

        char report[112];
        const bool has_context = context.registers != nullptr;
        const u32 lr = has_context ? context.registers[14] : 0;
        const auto length = FormatFaultReport(report, sizeof(report), static_cast<u32>(host_addr - base),
                                              access, has_context, context.thread_id, lr);
        WriteStderr(report, length);
        if (const auto hook = fatal_hook.load(std::memory_order_acquire)) {
            hook();
        }
        ::_exit(3);
    }
    return false;
}

} // namespace fault
} // namespace mh4n
