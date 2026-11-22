// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: WP83C's Intel UHD failure report and the RHI and presentation contracts.
#pragma once

#include <algorithm>
#include <string>
#include <string_view>

#include "mh4n/log.h"
#include "mh4n/present.h"

namespace mh4n::gpu::native {

inline constexpr char kGpuInitFailed[] = "gpu_init_failed";
inline constexpr char kGpuTargetFailed[] = "gpu_target_failed";
inline constexpr u64 kMiB = 1u << 20;

template <typename Create>
auto CreateSmallerRing(std::string_view name, u64 requested, u64 minimum, Create create) {
    for (u64 bytes = requested;; bytes = std::max(bytes / 2, minimum)) {
        auto resource = create(bytes);
        if (resource) {
            if (bytes != requested) {
                MH4N_LOG(Warning, "NativeGpu", "{}: using {} bytes instead of {} after allocation failure",
                         name, bytes, requested);
            }
            return resource;
        }
        if (bytes <= minimum) return resource;
    }
}

inline u32 LowerTargetScale(u32 scale, bool integrated) {
    return integrated && scale > 1 ? scale - 1 : 0;
}

inline void PublishGpuFailure(std::string_view operation, std::string_view resource, std::string_view cause) {
    present::SetRendererCannotDraw(true);
    std::string message(cause);
    std::ranges::replace(message, '\r', ' ');
    std::ranges::replace(message, '\n', ' ');
    MH4N_LOG(Error, "NativeGpu", "{} ({}): {}. Update the graphics driver from Intel or your GPU manufacturer "
             "and set scale = 1.", operation, resource, message);
}

inline void PublishGpuInitFailure(u64& failures, std::string_view resource, std::string_view cause) {
    ++failures;
    PublishGpuFailure("Renderer initialization failed", resource, cause);
}

} // namespace mh4n::gpu::native
