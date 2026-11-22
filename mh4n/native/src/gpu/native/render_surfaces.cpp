// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: 3dbrew GPU/Internal Registers, native_gpu.h, the texture and RHI contracts, and gpu_backend_readback_test.
#include "renderer.h"

#include <algorithm>
#include <cstring>

#include "gpu/backend/targets/upload.h"
#include "gpu/fragment/generator.h"
#include "gpu/shader/vertex_shader.h"
#include "gpu/texture/decode.h"
#include "mh4n/log.h"

namespace mh4n::gpu::native {
namespace {

using F = rhi::Format;
using P = rhi::PicaFormat;
using Strip = Surface::Strip;

rhi::Format ColorStorage(P format) {
    switch (format) {
    case P::Rgba8: return F::Rgba8Unorm;
    case P::Rgb8: return F::Rgb8;
    case P::Rgb5A1: return F::Rgb5A1;
    case P::Rgb565: return F::Rgb565;
    case P::Rgba4: return F::Rgba4;
    default: return F::Undefined;
    }
}

rhi::Format DepthStorage(P format) {
    switch (format) {
    case P::D16: return F::D16Unorm;
    case P::D24: return F::D32Float;
    case P::D24S8: return F::D24S8;
    default: return F::Undefined;
    }
}

texture::Format TextureFormat(P format) {
    switch (format) {
    case P::Rgb8: return texture::Format::RGB8;
    case P::Rgb5A1: return texture::Format::RGB5A1;
    case P::Rgb565: return texture::Format::RGB565;
    case P::Rgba4: return texture::Format::RGBA4;
    default: return texture::Format::RGBA8;
    }
}

void StoreColor(u8* out, F format, texture::Rgba c) {
    if (format == F::Rgba8Unorm) {
        out[0] = c.r;
        out[1] = c.g;
        out[2] = c.b;
        out[3] = c.a;
        return;
    }
    if (format == F::B5G6R5 || format == F::B5G5R5A1 || format == F::B4G4R4A4) {
        std::swap(c.r, c.b);
    }
    const u32 r = c.r, g = c.g, b = c.b, a = c.a;
    u32 pixel;
    switch (format) {
    case F::R5G6B5:
    case F::B5G6R5:
        pixel = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        break;
    case F::R5G5B5A1:
    case F::B5G5R5A1:
        pixel = ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a >> 7);
        break;
    case F::A1R5G5B5:
        pixel = ((a >> 7) << 15) | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
        break;
    default:
        pixel = ((r >> 4) << 12) | ((g >> 4) << 8) | ((b >> 4) << 4) | (a >> 4);
        break;
    }
    out[0] = static_cast<u8>(pixel);
    out[1] = static_cast<u8>(pixel >> 8);
}

bool CanCopyStrips(const Surface& from, const Surface& to) {
    const auto& a = from.key;
    const auto& b = to.key;
    const u32 distance = std::max(a.address, b.address) - std::min(a.address, b.address);
    return !a.IsDepth() && !from.shadow_cleared && a.format == b.format && a.StorageWidth() == b.StorageWidth() &&
           from.image->GetFormat() == to.image->GetFormat() && distance % a.StripBytes() == 0;
}

u32 SampleBits(P format) {
    switch (format) {
    case P::Rgb8: return 0x0888;
    case P::Rgb5A1: return 0x1555;
    case P::Rgb565: return 0x0565;
    case P::Rgba4: return 0x4444;
    default: return 0;
    }
}

const char* kSampleVertex = R"glsl(#version 450
layout(location = 0) flat out uint bits;
void main() {
    vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
    bits = uint(gl_InstanceIndex);
}
)glsl";

const char* kSampleFragment = R"glsl(#version 450
#extension GL_EXT_samplerless_texture_functions : require
layout(set = 0, binding = 0) uniform texture2D source;
layout(location = 0) flat in uint bits;
layout(location = 0) out vec4 color;
void main() {
    color = texelFetch(source, ivec2(gl_FragCoord.xy), 0);
    for (uint channel = 0u; channel < 4u; ++channel) {
        uint size = (bits >> (4u * channel)) & 15u;
        if (size == 0u) {
            color[channel] = 1.0;
        } else if (size < 8u) {
            uint value = uint(round(color[channel] * 255.0)) >> (8u - size);
            uint expanded = size == 1u ? value * 255u : (value << (8u - size)) | (value >> (2u * size - 8u));
            color[channel] = float(expanded) / 255.0;
        }
    }
}
)glsl";

} // namespace

u32 SurfaceKey::BytesPerPixel() const {
    switch (format) {
    case P::Rgba8:
    case P::D24S8: return 4;
    case P::Rgb8:
    case P::D24: return 3;
    default: return 2;
    }
}

rhi::Format Surfaces::ColorFormat(const Surface& surface) const {
    return ColorStorage(surface.key.format);
}

rhi::Format Surfaces::DepthFormat(const Surface& surface) const {
    return DepthStorage(surface.key.format);
}

Surface* Surfaces::Find(const SurfaceKey& key) {
    const auto it = cache_.find(key);
    return it == cache_.end() ? nullptr : &it->second;
}

Surface* Surfaces::FindTexture(const SurfaceKey& key) {
    if (Surface* exact = Find(key)) return exact;

    const auto range = key.Range();
    const u32 strip_bytes = key.StripBytes();
    bool has_alias = false;
    for (auto& [other, surface] : cache_) {
        const auto source_range = other.Range();
        if (other.format != key.format || other.StorageWidth() != key.StorageWidth() ||
            surface.shadow_cleared || !source_range.Overlaps(range)) continue;
        if (other.address == key.address && other.StorageHeight() == key.StorageHeight()) return &surface;

        // One texture row of tiles must map to one framebuffer row of tiles.
        const u32 first_address = std::max(other.address, key.address);
        if ((first_address - std::min(other.address, key.address)) % strip_bytes != 0) continue;
        const u32 first = (first_address - other.address) / strip_bytes;
        const auto end = (std::min(source_range.End(), range.End()) - other.address) / strip_bytes;
        if (std::any_of(surface.strips.begin() + first, surface.strips.begin() + end,
                        [](Strip strip) { return strip != Strip::Stale; })) {
            has_alias = true;
        }
    }
    return has_alias ? Get(key) : nullptr;
}

Surface* Surfaces::Get(const SurfaceKey& key) {
    if (Surface* old = Find(key)) return old;
    if (!key.address || !key.width || !key.height || key.Range().End() > (u64{1} << 32)) return nullptr;

    rhi::ImageDesc desc{};
    desc.width = key.StorageWidth() * scale_;
    desc.height = key.StorageHeight() * scale_;
    desc.format = key.IsDepth() ? DepthStorage(key.format) : ColorStorage(key.format);
    desc.shadow_source = key.shadow;
    desc.name = key.IsDepth() ? "PICA depth" : "PICA color";
    auto image = backend_->CreateImage(desc);
    if (!image) {
        MH4N_LOG(Warning, "NativeGpu", "surface {:#x}: {}", key.address, image.error());
        return nullptr;
    }
    Surface surface{};
    surface.key = key;
    surface.image = std::move(*image);
    surface.strips.resize(key.StorageHeight() / 8, Strip::Stale);
    surface.feedback_stale.resize(surface.strips.size(), true);
    return &cache_.emplace(key, std::move(surface)).first->second;
}

std::pair<u32, u32> Surfaces::StripInterval(const Surface& surface, const rhi::Rect& area) const {
    const u32 image_height = surface.key.StorageHeight() * scale_;
    const u32 top = static_cast<u32>(area.y);
    const u32 strip_height = 8 * scale_;
    return {(image_height - top - area.height) / strip_height,
            (image_height - top + strip_height - 1) / strip_height};
}

void Surfaces::CopyAliases(Surface& surface, u32 first, u32 end, rhi::Recorder& recorder) {
    if (std::none_of(surface.strips.begin() + first, surface.strips.begin() + end,
                     [](Strip strip) { return strip == Strip::Stale; })) {
        return;
    }
    const auto range = surface.StripRange(first, end - first);
    const u32 step = 8 * scale_;
    std::map<const rhi::Image*, std::vector<rhi::ImageCopyRegion>> copies;
    for (auto& [key, other] : cache_) {
        if (&other == &surface || !key.Range().Overlaps(range) || !CanCopyStrips(other, surface)) continue;
        for (u32 i = first; i < end;) {
            const auto strip = surface.StripRange(i);
            if (surface.strips[i] != Strip::Stale || strip.address < key.address || strip.End() > key.Range().End()) {
                ++i;
                continue;
            }
            const u32 source = (strip.address - key.address) / key.StripBytes();
            u32 count = 0;
            while (i + count < end && source + count < other.strips.size() &&
                   surface.strips[i + count] == Strip::Stale && other.strips[source + count] != Strip::Stale) {
                ++count;
            }
            if (!count) {
                ++i;
                continue;
            }

            rhi::ImageCopyRegion copy{};
            copy.src_y = other.image->Height() - (source + count) * step;
            copy.dst_y = surface.image->Height() - (i + count) * step;
            copy.width = surface.image->Width();
            copy.height = count * step;
            copies[other.image.get()].push_back(copy);
            std::fill_n(surface.strips.begin() + i, count, Strip::Current);
            std::fill_n(surface.feedback_stale.begin() + i, count, true);
            surface.sample_after_write = true;
            i += count;
        }
    }
    if (copies.empty()) return;
    std::vector<rhi::ImageCopyJob> jobs;
    jobs.reserve(copies.size());
    for (const auto& [source, regions] : copies) jobs.push_back({source, surface.image.get(), regions});
    recorder.CopyImages(jobs);
}

bool Surfaces::Upload(Surface& surface, const rhi::Rect& area, rhi::Recorder& recorder, rhi::Ring& staging,
                       const std::function<void()>& outside_pass) {
    if (surface.key.IsDepth()) {
        if (!surface.initialized) {
            outside_pass();
            recorder.ClearImage(*surface.image, {});
            surface.initialized = true;
            std::ranges::fill(surface.strips, Strip::Current);
        }
        return true;
    }

    const u32 guest_width = surface.key.StorageWidth();
    const u32 guest_height = surface.key.StorageHeight();
    const u32 pixel_width = guest_width * scale_;
    const F format = surface.image->GetFormat();
    const u32 bpp = format == F::Rgba8Unorm ? 4 : 2;
    const auto [first, end] = StripInterval(surface, area);
    if (std::none_of(surface.strips.begin() + first, surface.strips.begin() + end,
                     [](Strip strip) { return strip == Strip::Stale; })) return true;
    outside_pass();
    CopyAliases(surface, first, end, recorder);
    const auto texture_format = TextureFormat(surface.key.format);
    for (u32 strip = first; strip < end;) {
        if (surface.strips[strip] != Strip::Stale) {
            ++strip;
            continue;
        }
        u32 count = 1;
        while (strip + count < end && surface.strips[strip + count] == Strip::Stale) ++count;
        const auto range = surface.StripRange(strip, count);
        const u8* guest = host_.PhysicalPointer(range.address, static_cast<u32>(range.size));
        const u32 pixel_height = count * 8 * scale_;
        const u64 bytes = u64{pixel_width} * pixel_height * bpp;
        auto upload = staging.Map(bytes, 4);
        std::vector<u8> spare;
        if (!upload) spare.resize(static_cast<size_t>(bytes));
        u8* pixels = upload ? upload->data.data() : spare.data();
        for (u32 y = 0; y < count * 8; ++y) {
            u8* row = pixels + u64{(count * 8 - 1 - y) * scale_} * pixel_width * bpp;
            for (u32 x = 0; x < guest_width; ++x) {
                const texture::Rgba color = guest ? texture::LookupTexture(guest, x, y, guest_width, texture_format)
                                                  : texture::Rgba{};
                for (u32 n = 0; n < scale_; ++n) StoreColor(row + (x * scale_ + n) * bpp, format, color);
            }
            const u32 row_bytes = pixel_width * bpp;
            for (u32 n = 1; n < scale_; ++n) std::memcpy(row + n * row_bytes, row, row_bytes);
        }

        rhi::BufferImageRegion region{};
        region.image_rect = {0, static_cast<s32>((guest_height - (strip + count) * 8) * scale_),
                             pixel_width, pixel_height};
        if (upload) {
            region.buffer_offset = upload->offset;
            if (!staging.Commit(bytes)) return false;
            recorder.CopyBufferToImage(*staging.GetBuffer(), *surface.image, std::span{&region, 1});
        } else if (!rhi::UploadToImage(*backend_, recorder, *surface.image, region, spare)) {
            return false;
        }
        std::fill_n(surface.strips.begin() + strip, count, Strip::Current);
        std::fill_n(surface.feedback_stale.begin() + strip, count, true);
        surface.sample_after_write = true;
        surface.shadow_cleared = false;
        strip += count;
    }
    return true;
}

bool Surfaces::ConvertForSampling(const Surface& surface, u32 bits, rhi::Recorder& recorder,
                                  std::span<const rhi::ImageCopyRegion> regions) {
    const rhi::BindingSetDesc layout{{{0, rhi::BindingType::SampledImage}}};
    if (!sample_conversion_) {
        std::vector<u32> vertex, fragment_code;
        std::string error;
        if (!shader::CompileGlslToSpirv(kSampleVertex, vertex, error) ||
            !fragment::CompileFragmentGlslToSpirv(kSampleFragment, fragment_code, error)) {
            MH4N_LOG(Warning, "NativeGpu", "Target texture shader: {}", error);
            return false;
        }
        rhi::GraphicsPipelineDesc desc{};
        desc.vertex.spirv = vertex;
        desc.fragment.spirv = fragment_code;
        desc.sets = {layout};
        desc.color_format = F::Rgba8Unorm;
        desc.name = "PICA target texture conversion";
        auto made = backend_->CreateGraphicsPipeline(desc);
        if (!made) return false;
        sample_conversion_ = std::move(*made);
        sample_bindings_ = std::make_unique<rhi::BindingArena>(backend_);
    }

    auto set = sample_bindings_->Acquire(layout);
    if (!set || !(*set)->SetSampledImage(0, *surface.image, rhi::ImageView::Sampled)) return false;
    const u32 width = surface.image->Width(), height = surface.image->Height();
    recorder.BeginTargetPass({.color = surface.feedback.get(), .area = {0, 0, width, height}});
    recorder.SetPipeline(*sample_conversion_);
    recorder.SetViewport({0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1});
    recorder.Bind(**set, 0);
    for (const auto& region : regions) {
        recorder.SetScissor({0, static_cast<s32>(region.dst_y), width, region.height});
        recorder.Draw(3, 1, 0, bits);
    }
    recorder.EndPass();
    return true;
}

const rhi::Image* Surfaces::Sample(Surface& surface, bool feedback, rhi::Recorder& recorder,
                                  const std::function<void()>& outside_pass) {
    // RGB8 and unsupported packed formats use RGBA8 attachments. Apply the guest
    // component sizes (including opaque alpha for RGB) before filtering.
    const F storage = surface.image->GetFormat();
    const u32 bits = storage == F::Rgba8Unorm ? SampleBits(surface.key.format) : 0;
    if (!feedback && !bits) {
        if (surface.sample_after_write) outside_pass();
        // The composer may reuse the whole descriptor set after an attachment write.
        if (!rhi::PrepareSampledImage(*surface.image)) return nullptr;
        surface.sample_after_write = false;
        return surface.image.get();
    }

    if (!surface.feedback) {
        rhi::ImageDesc desc{};
        desc.format = surface.image->GetFormat();
        desc.width = surface.image->Width();
        desc.height = surface.image->Height();
        desc.shadow_source = surface.key.shadow;
        desc.name = "PICA sampled target";
        auto image = backend_->CreateImage(desc);
        if (!image) return nullptr;
        surface.feedback = std::move(*image);
    }

    const u32 step = 8 * scale_;
    const u32 strips = static_cast<u32>(surface.strips.size());
    std::vector<rhi::ImageCopyRegion> copies;
    for (u32 first = 0; first < strips;) {
        if (!surface.feedback_stale[first]) {
            ++first;
            continue;
        }
        u32 end = first + 1;
        while (end < strips && surface.feedback_stale[end]) ++end;
        const u32 y = surface.image->Height() - end * step;
        copies.push_back({.src_y = y, .dst_y = y, .width = surface.image->Width(), .height = (end - first) * step});
        first = end;
    }
    if (!copies.empty()) {
        outside_pass();
        if (bits) {
            if (!ConvertForSampling(surface, bits, recorder, copies)) return nullptr;
        } else {
            recorder.CopyImage(*surface.image, *surface.feedback, copies);
        }
        std::fill(surface.feedback_stale.begin(), surface.feedback_stale.end(), false);
        feedback_copies_ += feedback;
    }
    return rhi::PrepareSampledImage(*surface.feedback) ? surface.feedback.get() : nullptr;
}

void Surfaces::PrepareDraw(Surface* color, Surface* depth, const rhi::Rect& area, const std::function<void()>& before_read) {
    for (Surface* surface : {color, depth}) {
        if (!surface) continue;
        const auto [first, end] = StripInterval(*surface, area);
        WriteBack(surface->StripRange(first, end - first), surface, false, before_read, true);
    }
}

void Surfaces::Written(Surface& surface, const rhi::Rect& area) {
    const auto [first, end] = StripInterval(surface, area);
    std::fill(surface.strips.begin() + first, surface.strips.begin() + end, Strip::Dirty);
    std::fill(surface.feedback_stale.begin() + first, surface.feedback_stale.begin() + end, true);
    surface.sample_after_write = true;
    Invalidate(surface.StripRange(first, end - first), &surface);
}

void Surfaces::WriteBackStrips(Surface& surface, u32 first, u32 end, MemoryRange keep) {
    const u32 rows = (end - first) * 8;
    const auto range = surface.StripRange(first, end - first);
    u8* guest = host_.PhysicalPointer(range.address, static_cast<u32>(range.size));
    if (!guest) return;
    const rhi::Rect rect{0, static_cast<s32>(surface.key.StorageHeight() - end * 8),
                         surface.key.StorageWidth(), rows};
    std::span<u8> dest(guest, static_cast<size_t>(range.size));
    std::vector<u8> preserve;
    u64 keep_offset = 0;
    if (range.Overlaps(keep)) {
        const u64 begin = std::max(keep.address, range.address);
        const u64 end_keep = std::min(keep.End(), range.End());
        keep_offset = begin - range.address;
        preserve.assign(guest + keep_offset, guest + keep_offset + (end_keep - begin));
    }
    auto written = backend_->ReadbackToGuest(*surface.image, rect, surface.key.format, dest, scale_);
    if (!preserve.empty()) std::memcpy(guest + keep_offset, preserve.data(), preserve.size());
    if (!written) {
        MH4N_LOG(Warning, "NativeGpu", "surface writeback {:#x}: {}", range.address, written.error());
        return;
    }
    std::fill(surface.strips.begin() + first, surface.strips.begin() + end, Strip::Current);
    ++counters_["writebacks"];
}

void Surfaces::WriteBack(MemoryRange range, const Surface* except, bool preserve_guest,
                         const std::function<void()>& before_read, bool use_aliases) {
    for (auto& [key, surface] : cache_) {
        if (&surface == except || !key.Range().Overlaps(range)) continue;
        if (use_aliases && except && CanCopyStrips(surface, *except)) continue;
        const auto dirty = [&](u32 strip) {
            return surface.strips[strip] == Strip::Dirty &&
                   (key.IsDepth() || surface.StripRange(strip).Overlaps(range));
        };
        const u32 strips = static_cast<u32>(surface.strips.size());
        for (u32 first = 0; first < strips;) {
            if (!dirty(first)) {
                ++first;
                continue;
            }
            u32 end = first + 1;
            while (end < strips && dirty(end)) ++end;
            if (before_read) before_read();
            WriteBackStrips(surface, first, end, preserve_guest ? range : MemoryRange{});
            first = end;
        }
    }
}

void Surfaces::Invalidate(MemoryRange range, const Surface* except) {
    for (auto& [key, surface] : cache_) {
        if (&surface == except || !key.Range().Overlaps(range)) continue;
        if (key.IsDepth()) surface.initialized = false;
        for (u32 strip = 0; strip < surface.strips.size(); ++strip) {
            if (surface.StripRange(strip).Overlaps(range)) surface.strips[strip] = Strip::Stale;
        }
    }
}

void Surfaces::Clear() {
    cache_.clear();
    feedback_copies_ = 0;
}

Surface* Surfaces::FindTransferSource(const gx::TransferRegs& transfer, u32& first_row) {
    constexpr std::array input_formats{P::Rgba8, P::Rgb8, P::Rgb565, P::Rgb5A1, P::Rgba4};
    if (transfer.InputFormat() >= input_formats.size()) return nullptr;
    for (auto& [key, surface] : cache_) {
        if (key.IsDepth() || key.shadow || key.width != transfer.InputWidth() ||
            key.format != input_formats[transfer.InputFormat()] ||
            transfer.InputAddress() < key.address) {
            continue;
        }
        const u32 byte_offset = transfer.InputAddress() - key.address;
        if (byte_offset % key.StripBytes() != 0) continue;
        const u32 strip = byte_offset / key.StripBytes();
        const u32 count = (transfer.OutputHeight() + 7) / 8;
        if (strip + count > surface.strips.size()) continue;
        const auto first = surface.strips.begin() + strip;
        if (std::any_of(first, first + count, [](Strip item) { return item == Strip::Stale; })) continue;
        first_row = strip * 8;
        return &surface;
    }
    return nullptr;
}

} // namespace mh4n::gpu::native
