// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: the comments of mh4n/kernel.h (Object, IsWaitable), 3dbrew "Kernel objects" and the
// black-box specification of tests/kernel_objects_test.cpp.
#include "mh4n/kernel.h"

namespace mh4n {

Object::Object(Kernel& kernel) : kernel_(kernel), object_id_(kernel.GenerateObjectId()) {}

bool Object::IsWaitable() const {
    // Event, Mutex, Thread, Semaphore, Timer, ServerPort and ServerSession; not Process
    switch (GetHandleType()) {
    case HandleType::Event:
    case HandleType::Mutex:
    case HandleType::Thread:
    case HandleType::Semaphore:
    case HandleType::Timer:
    case HandleType::ServerPort:
    case HandleType::ServerSession:
        return true;
    default:
        return false;
    }
}

}  // namespace mh4n
