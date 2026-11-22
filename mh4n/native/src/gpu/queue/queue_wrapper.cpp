// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: the GpuWrapper contract and gpu_queue_test.

#include "queue_gpu.h"

namespace mh4n::gpu::queue {
namespace {

MH4N_OPTION("capture-player", "--queue", "", "Run the GPU on a renderer thread in lockstep");

MH4N_REGISTER(GpuWrapper, "queue", [](std::unique_ptr<Gpu> inner, Host&, const Options&) {
    return std::make_unique<QueueGpu>(std::move(inner), QueueConfig{.lockstep = true});
});

} // namespace
} // namespace mh4n::gpu::queue
