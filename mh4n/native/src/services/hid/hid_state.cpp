// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: HID and kernel contract headers, hid_test.cpp, kernel_objects_test.cpp
// and boot-village IPC and SVC traces.

#include "hid_state.h"

#include "mh4n/scheduler.h"

namespace mh4n::hid {

void Shared::Ensure(ServiceContext& context) {
    if (initialised_) {
        return;
    }

    auto& kernel = context.GetKernel();
    const Result result = kernel.NewSharedMemory(&memory_, nullptr, kSharedMemorySize,
                                                 MemoryPermission::ReadWrite, MemoryPermission::Read,
                                                 0, FcramRegion::Base, "HID");
    if (!IsSuccess(result)) {
        context.RequestExit(2, "HID: could not allocate shared memory");
        return;
    }

    pad_or_touch_1_ = kernel.NewEvent(ResetType::OneShot, "HID pad or touch 1");
    pad_or_touch_2_ = kernel.NewEvent(ResetType::OneShot, "HID pad or touch 2");
    accelerometer_event_ = kernel.NewEvent(ResetType::OneShot, "HID accelerometer");
    gyroscope_event_ = kernel.NewEvent(ResetType::OneShot, "HID gyroscope");
    debug_pad_event_ = kernel.NewEvent(ResetType::OneShot, "HID debug pad");
    context_ = &context;
    scheduler_ = &context.Scheduler();
    origin_ = scheduler_->NowTicks();
    initialised_ = true;
    SchedulePad(origin_ + kPadUpdateTicks);
}

u8* Shared::MemoryPointer() const {
    return memory_ ? memory_->GetPointer() : nullptr;
}

InputState Shared::Sample(u64 tick) const {
    if (context_ && context_->Interfaces().input) {
        return context_->Interfaces().input->Sample(tick);
    }
    return {};
}

void Shared::RunPadUpdate(u64 tick) {
    if (u8* memory = MemoryPointer()) {
        module_.UpdatePad(memory, Sample(tick), tick);
        pad_or_touch_1_->Signal();
        pad_or_touch_2_->Signal();
    }
}

void Shared::RunAccelerometerUpdate(u64 tick) {
    if (u8* memory = MemoryPointer()) {
        module_.UpdateAccelerometer(memory, Sample(tick), tick);
        accelerometer_event_->Signal();
    }
}

void Shared::RunGyroscopeUpdate(u64 tick) {
    if (u8* memory = MemoryPointer()) {
        module_.UpdateGyroscope(memory, Sample(tick), tick);
        gyroscope_event_->Signal();
    }
}

void Shared::SchedulePad(u64 due) {
    pad_timer_ = scheduler_->ScheduleAt(due, [weak = weak_from_this(), due] {
        if (auto hid = weak.lock()) {
            hid->RunPadUpdate(due);
            hid->SchedulePad(due + kPadUpdateTicks);
        }
    });
}

void Shared::ScheduleAccelerometer(u64 due) {
    accelerometer_timer_ = scheduler_->ScheduleAt(due, [weak = weak_from_this(), due] {
        if (auto hid = weak.lock()) {
            hid->RunAccelerometerUpdate(due);
            hid->ScheduleAccelerometer(due + kAccelerometerUpdateTicks);
        }
    });
}

void Shared::ScheduleGyroscope(u64 due) {
    gyroscope_timer_ = scheduler_->ScheduleAt(due, [weak = weak_from_this(), due] {
        if (auto hid = weak.lock()) {
            hid->RunGyroscopeUpdate(due);
            hid->ScheduleGyroscope(due + kGyroscopeUpdateTicks);
        }
    });
}

void Shared::EnableAccelerometer() {
    if (initialised_ && module_.EnableAccelerometer()) {
        ScheduleAccelerometer(scheduler_->NowTicks() + kAccelerometerUpdateTicks);
    }
}

void Shared::DisableAccelerometer() {
    if (initialised_ && module_.DisableAccelerometer()) {
        scheduler_->Cancel(accelerometer_timer_);
        accelerometer_timer_ = 0;
    }
}

void Shared::EnableGyroscope() {
    if (initialised_ && module_.EnableGyroscope()) {
        ScheduleGyroscope(scheduler_->NowTicks() + kGyroscopeUpdateTicks);
    }
}

void Shared::DisableGyroscope() {
    if (initialised_ && module_.DisableGyroscope()) {
        scheduler_->Cancel(gyroscope_timer_);
        gyroscope_timer_ = 0;
    }
}

std::shared_ptr<Shared> GetShared(ServiceContext& context) {
    auto hid = context.State<Shared>("hid");
    hid->Ensure(context);
    return hid;
}

}  // namespace mh4n::hid
