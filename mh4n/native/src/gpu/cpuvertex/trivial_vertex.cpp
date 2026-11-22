// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: cpuvertex and RHI contract headers, gpu_cpuvertex and gpu_cpuvertex_batch unit tests.

#include "trivial_vertex.h"

#ifdef __ANDROID__
#include "trivial_vertex_spv_v11.inc"
#else
#include "trivial_vertex_spv.inc"
#endif

namespace mh4n::gpu::cpuvertex {

std::span<const u32> TrivialVertexShader(bool use_clip_planes) {
#ifdef __ANDROID__
    if (use_clip_planes) {
        return spv_trivial_vertex_clip_v11;
    }
    return spv_trivial_vertex_noclip_v11;
#else
    if (use_clip_planes) {
        return spv_trivial_vertex_clip;
    }
    return spv_trivial_vertex_noclip;
#endif
}

} // namespace mh4n::gpu::cpuvertex
