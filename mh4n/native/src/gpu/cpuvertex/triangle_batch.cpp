// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: cpuvertex and RHI contract headers, gpu_cpuvertex and gpu_cpuvertex_batch unit tests.

#include "triangle_batch.h"

#include <cstring>
#include <iterator>

namespace mh4n::gpu::cpuvertex {

rhi::VertexLayout MakeBatchVertexLayout() {
    using rhi::Format;
    return {{{0, kHardwareVertexStride, false}},
            {{0, 0, Format::Rgba32Float, offsetof(HardwareVertex, position)},
             {1, 0, Format::Rgba32Float, offsetof(HardwareVertex, color)},
             {2, 0, Format::Rg32Float, offsetof(HardwareVertex, tex_coord0)},
             {3, 0, Format::Rg32Float, offsetof(HardwareVertex, tex_coord1)},
             {4, 0, Format::Rg32Float, offsetof(HardwareVertex, tex_coord2)},
             {5, 0, Format::R32Float, offsetof(HardwareVertex, tex_coord0_w)},
             {6, 0, Format::Rgba32Float, offsetof(HardwareVertex, normquat)},
             {7, 0, Format::Rgb32Float, offsetof(HardwareVertex, view)}}};
}

HardwareVertex MakeHardwareVertex(const OutputVertex& v, bool flip_quaternion) {
    HardwareVertex result{
        {v.pos[0], v.pos[1], v.pos[2], v.pos[3]},
        {v.color[0], v.color[1], v.color[2], v.color[3]},
        {v.tc0[0], v.tc0[1]},
        {v.tc1[0], v.tc1[1]},
        {v.tc2[0], v.tc2[1]},
        v.tc0_w,
        {v.quat[0], v.quat[1], v.quat[2], v.quat[3]},
        {v.view[0], v.view[1], v.view[2]},
    };
    if (flip_quaternion) {
        for (float& lane : result.normquat) {
            lane = -lane;
        }
    }
    return result;
}

#if defined(__clang__)
#pragma clang fp contract(off)
#endif
bool AreQuaternionsOpposite(const float (&a)[4], const float (&b)[4]) {
    return ((a[0] * b[0] + a[1] * b[1]) + a[2] * b[2]) + a[3] * b[3] < 0.0f;
}

void TriangleBatch::AddTriangle(const OutputVertex& v0, const OutputVertex& v1, const OutputVertex& v2) {
    const HardwareVertex triangle[] = {
        MakeHardwareVertex(v0, false),
        MakeHardwareVertex(v1, AreQuaternionsOpposite(v0.quat, v1.quat)),
        MakeHardwareVertex(v2, AreQuaternionsOpposite(v0.quat, v2.quat)),
    };
    vertices_.insert(vertices_.end(), std::begin(triangle), std::end(triangle));
}

rhi::Result<StreamedBatch> UploadBatch(rhi::Ring& ring, const TriangleBatch& batch) {
    if (batch.Empty()) {
        return StreamedBatch{};
    }
    const u64 bytes = batch.VertexCount() * sizeof(HardwareVertex);
    auto allocation = ring.Map(bytes, 16);
    if (!allocation) {
        return rhi::Error{allocation.error()};
    }
    std::memcpy(allocation->data.data(), batch.Data(), bytes);
    ring.GetBuffer()->Flush(allocation->offset, bytes);
    const auto commit = ring.Commit(bytes);
    if (!commit) {
        return rhi::Error{commit.error()};
    }
    return StreamedBatch{allocation->offset, static_cast<u32>(batch.VertexCount())};
}

} // namespace mh4n::gpu::cpuvertex
