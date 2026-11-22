// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: Runtime, HID and IR contract headers, and the MHID format in hid_test.cpp.

#include "hid_state.h"

#include <array>
#include <fstream>
#include <string>

#include "mh4n/registry.h"
#include "mh4n/runtime.h"
#include "services/ir/ir_state.h"

namespace mh4n::hid {
namespace {

constexpr u32 kHidRecord = 1;
constexpr u32 kIrRecord = 2;
constexpr u32 kIrCaptureSize = 0x200;

class Capture {
public:
    explicit Capture(const std::string& path)
        : data_(path, std::ios::binary), manifest_(path + ".json", std::ios::binary) {
        data_.write("MHID", 4);
        Write(u32{1});
    }

    bool Good() const { return data_.good() && manifest_.good(); }

    void Record(u32 type, u32 frame, u64 tick, const u8* bytes, u32 size) {
        Write(type);
        Write(frame);
        Write(tick);
        Write(size);
        data_.write(reinterpret_cast<const char*>(bytes), size);
    }

    void Finish() {
        data_.close();
        manifest_ << "{\n  \"format\": \"MHID\",\n  \"version\": 1\n}\n";
        manifest_.close();
    }

private:
    template <typename T>
    void Write(T value) {
        std::array<char, sizeof(T)> bytes{};
        for (auto& byte : bytes) {
            byte = static_cast<char>(value & 0xFF);
            value >>= 8;
        }
        data_.write(bytes.data(), bytes.size());
    }

    std::ofstream data_;
    std::ofstream manifest_;
};

void StartCapture(Runtime& runtime) {
    const auto path = runtime.GetOptions().Get("--capture-hid");
    if (!path) {
        return;
    }
    auto* manager = FindServiceManager(runtime);
    if (!manager) {
        runtime.RequestExit(2, "--capture-hid requires the service manager");
        return;
    }

    auto capture = std::make_shared<Capture>(*path);
    if (!capture->Good()) {
        runtime.RequestExit(2, "--capture-hid could not open " + *path);
        return;
    }
    auto hid = GetShared(manager->Context());
    auto ir = ir::GetShared(manager->Context());
    runtime.OnFrame([capture, hid, ir](u32 frame, u64 tick) {
        const std::array<u8, kReadableSize> empty{};
        const auto& memory = hid->Memory();
        const u8* bytes = memory && memory->IsMapped() ? memory->GetPointer() : nullptr;
        capture->Record(kHidRecord, frame, tick, bytes ? bytes : empty.data(), kReadableSize);
        const auto ir_memory = ir->capture_memory.lock();
        if (ir_memory && ir_memory->Size() >= kIrCaptureSize) {
            if (const u8* ir_bytes = ir_memory->GetPointer()) {
                capture->Record(kIrRecord, frame, tick, ir_bytes, kIrCaptureSize);
            }
        }
    });
    runtime.OnFinish([capture, &runtime] {
        capture->Finish();
        if (!capture->Good()) {
            runtime.RequestExit(2, "--capture-hid could not finish writing the capture");
        }
    });
}

MH4N_OPTION("runner", "--capture-hid", "FILE", "Capture HID shared memory at each game frame");
MH4N_REGISTER(BootStep, "tracing.hid", BootStep{Phase::Tracing, StartCapture});

}  // namespace
}  // namespace mh4n::hid
