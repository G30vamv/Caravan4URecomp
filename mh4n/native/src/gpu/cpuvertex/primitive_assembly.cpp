// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: cpuvertex and RHI contract headers, gpu_cpuvertex and gpu_cpuvertex_batch unit tests.

#include "primitive_assembly.h"

namespace mh4n::gpu::cpuvertex {

void PrimitiveAssembler::ApplyRegisterWrites(const pica::PicaState& state) {
    if (state.dirty.Test(pica::reg::kTriangleTopology)) {
        Reconfigure(static_cast<Topology>((state.regs[pica::reg::kTriangleTopology] >> 8) & 3u));
    } else if (state.dirty.Test(pica::reg::kRestartPrimitive)) {
        Reset();
    }
}

} // namespace mh4n::gpu::cpuvertex
