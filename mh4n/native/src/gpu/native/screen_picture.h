// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: present.h, gpu/backend/rhi.h and present_test.cpp.
#pragma once

#include <thread>

#include "mh4n/present.h"

namespace mh4n::gpu::rhi {
class Backend;
}

namespace mh4n::gpu::native {

// Convert this renderer's same-device shared picture to the Vulkan compositor's input.
// The producer calls this before handing the frame to the reader. A picture keeps the same reader thread.
std::optional<present::ScreenImage> VulkanScreenImage(const present::ScreenImage& screen, const rhi::Backend& backend,
                                                     std::thread::id reader);

} // namespace mh4n::gpu::native
