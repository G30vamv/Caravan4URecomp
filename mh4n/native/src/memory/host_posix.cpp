// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: host.h, memory_test.cpp, and POSIX mmap, mprotect, sigaction, and sigaltstack.

#ifndef _WIN32

#include "host.h"

#include <algorithm>
#include <cstdlib>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include "mh4n/assert.h"

namespace mh4n::host {
namespace {

constexpr int kReserveFlags = MAP_PRIVATE | MAP_ANONYMOUS
#ifdef MAP_NORESERVE
                              | MAP_NORESERVE
#endif
    ;

struct PreviousAction {
    struct sigaction action{};
    std::atomic_flag called{};
};

PreviousAction previous_segv;
PreviousAction previous_bus;

#if (defined(__linux__) && defined(__x86_64__) && defined(REG_ERR)) || \
    (defined(__APPLE__) && defined(__x86_64__))
FaultAccess DecodePageFault(unsigned error) {
    constexpr unsigned kWrite = 1u << 1;
    constexpr unsigned kInstruction = 1u << 4;
    if ((error & kInstruction) != 0) {
        return FaultAccess::Unknown;
    }
    return (error & kWrite) != 0 ? FaultAccess::Write : FaultAccess::Read;
}
#endif

FaultAccess DecodeAccess(void* opaque) {
#if defined(__linux__) && defined(__x86_64__) && defined(REG_ERR)
    return DecodePageFault(static_cast<ucontext_t*>(opaque)->uc_mcontext.gregs[REG_ERR]);
#elif defined(__APPLE__) && defined(__x86_64__)
    return DecodePageFault(static_cast<ucontext_t*>(opaque)->uc_mcontext->__es.__err);
#else
    (void)opaque;
    return FaultAccess::Unknown;
#endif
}

[[noreturn]] void DeliverDefault(int signal) {
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    sigaction(signal, &action, nullptr);
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, signal);
    pthread_sigmask(SIG_UNBLOCK, &signals, nullptr);
    raise(signal);
    _exit(128 + signal);
}

void OnSignal(int signal, siginfo_t* info, void* opaque) {
    if (info->si_code > 0) {
        fault::HandleFault(reinterpret_cast<std::uintptr_t>(info->si_addr), DecodeAccess(opaque));
    }

    auto& previous = signal == SIGSEGV ? previous_segv : previous_bus;
    const auto& action = previous.action;
    if (action.sa_handler == SIG_IGN) {
        return;
    }
    if (action.sa_handler == SIG_DFL ||
        ((action.sa_flags & SA_RESETHAND) != 0 && previous.called.test_and_set())) {
        DeliverDefault(signal);
    }
    if ((action.sa_flags & SA_SIGINFO) != 0) {
        action.sa_sigaction(signal, info, opaque);
    } else {
        action.sa_handler(signal);
    }
}

void InstallAction(int signal, PreviousAction& previous) {
    MH4N_ASSERT(sigaction(signal, nullptr, &previous.action) == 0);
    struct sigaction action{};
    action.sa_sigaction = OnSignal;
    action.sa_mask = previous.action.sa_mask;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK |
                      (previous.action.sa_flags & (SA_RESTART | SA_NODEFER));
    MH4N_ASSERT(sigaction(signal, &action, nullptr) == 0);
}

class ThreadFaultStack {
public:
    void EnsureInstalled() {
        stack_t current{};
        MH4N_ASSERT(sigaltstack(nullptr, &current) == 0);
        if ((current.ss_flags & SS_DISABLE) == 0) {
            return;
        }
        if (!storage_) {
            size_ = std::max<std::size_t>(SIGSTKSZ, 64 * 1024);
            storage_ = std::make_unique<u8[]>(size_);
        }
        stack_t stack{};
        stack.ss_sp = storage_.get();
        stack.ss_size = size_;
        MH4N_ASSERT(sigaltstack(&stack, nullptr) == 0);
    }

    ~ThreadFaultStack() {
        stack_t current{};
        if (storage_ && sigaltstack(nullptr, &current) == 0 &&
            (current.ss_flags & SS_DISABLE) == 0 && current.ss_sp == storage_.get()) {
            stack_t disabled{};
            disabled.ss_flags = SS_DISABLE;
            MH4N_ASSERT(sigaltstack(&disabled, nullptr) == 0);
        }
    }

private:
    std::unique_ptr<u8[]> storage_;
    std::size_t size_ = 0;
};

} // namespace

std::size_t PageSize() {
    const long page_size = sysconf(_SC_PAGESIZE);
    return page_size > 0 ? static_cast<std::size_t>(page_size) : 0;
}

void* Reserve(std::size_t size) {
    void* base = mmap(nullptr, size, PROT_NONE, kReserveFlags, -1, 0);
    return base == MAP_FAILED ? nullptr : base;
}

void Release(void* base, std::size_t size) {
    MH4N_ASSERT(munmap(base, size) == 0);
}

bool Commit(void* addr, std::size_t size) {
    return mprotect(addr, size, PROT_READ | PROT_WRITE) == 0;
}

bool Decommit(void* addr, std::size_t size) {
    // Replace the pages in place so their addresses stay reserved and their old data is gone.
    return mmap(addr, size, PROT_NONE, kReserveFlags | MAP_FIXED, -1, 0) == addr;
}

void InstallFaultHandler() {
    static std::once_flag installed;
    std::call_once(installed, [] {
        InstallAction(SIGSEGV, previous_segv);
        InstallAction(SIGBUS, previous_bus);
    });
}

void InstallThreadFaultStack() {
    thread_local ThreadFaultStack stack;
    stack.EnsureInstalled();
}

} // namespace mh4n::host

#endif
