// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: native_gpu.h, texture/cache.h, fragment/sampling/sampling.h, hd_textures.h and gpu_sampling_test.
#include "renderer.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>

#include "gpu/backend/targets/upload.h"
#include "hdpack/image.h"
#include "mh4n/log.h"

namespace mh4n::gpu::native {
namespace {

namespace sampling = fragment::sampling;

std::optional<rhi::PicaFormat> SurfaceFormat(texture::Format format) {
    switch (format) {
    case texture::Format::RGBA8: return rhi::PicaFormat::Rgba8;
    case texture::Format::RGB8: return rhi::PicaFormat::Rgb8;
    case texture::Format::RGB5A1: return rhi::PicaFormat::Rgb5A1;
    case texture::Format::RGB565: return rhi::PicaFormat::Rgb565;
    case texture::Format::RGBA4: return rhi::PicaFormat::Rgba4;
    default: return std::nullopt;
    }
}

rhi::Rect WholeImage(const rhi::Image& image) {
    return {0, 0, image.Width(), image.Height()};
}

bool UploadFace(rhi::Backend& backend, rhi::Recorder& recorder, rhi::Ring& staging, rhi::Image& image, u32 face,
                std::span<const u8> pixels) {
    const u32 width = image.Width(), height = image.Height();
    if (sampling::UploadRgba8(recorder, staging, image, 0, face, width, height, pixels)) return true;

    const size_t pitch = size_t{width} * 4;
    if (pixels.size() != pitch * height) return false;
    std::vector<u8> flipped(pixels.size());
    for (u32 y = 0; y < height; ++y) {
        std::memcpy(flipped.data() + pitch * y, pixels.data() + pitch * (height - 1 - y), pitch);
    }
    rhi::BufferImageRegion region{};
    region.layer = face;
    region.image_rect = WholeImage(image);
    return rhi::UploadToImage(backend, recorder, image, region, flipped).ok();
}

// MODIFIED for the Windows port: the levels of a converted HD texture (block-compressed, level 0 first) into an image made with
// that many levels. A level goes through the staging ring, or alone when the ring cannot hold it.
bool UploadBlocks(rhi::Backend& backend, rhi::Recorder& recorder, rhi::Ring& staging, rhi::Image& image,
                  hdpack::PixelFormat format, u32 mips, std::span<const u8> blob) {
    u64 offset = 0;
    for (u32 level = 0; level < mips; ++level) {
        const u32 width = std::max(1u, image.Width() >> level), height = std::max(1u, image.Height() >> level);
        const u64 bytes = hdpack::LevelBytes(format, width, height);
        if (offset + bytes > blob.size()) return false;
        const std::span<const u8> data = blob.subspan(offset, bytes);
        rhi::BufferImageRegion region{};
        region.mip_level = level;
        region.image_rect = {0, 0, width, height};
        bool done = false;
        if (const auto allocation = staging.Map(bytes, 16)) {
            std::memcpy(allocation->data.data(), data.data(), bytes);
            staging.GetBuffer()->Flush(allocation->offset, bytes);
            if (staging.Commit(bytes)) {
                region.buffer_offset = allocation->offset;
                recorder.CopyBufferToImage(*staging.GetBuffer(), image, std::span{&region, 1});
                done = true;
            }
        }
        if (!done && !rhi::UploadToImage(backend, recorder, image, region, data).ok()) return false;
        offset += bytes;
    }
    return true;
}

rhi::Format BlockFormat(hdpack::PixelFormat format) {
    switch (format) {
    case hdpack::PixelFormat::BC1: return rhi::Format::Bc1Unorm;
    case hdpack::PixelFormat::BC3: return rhi::Format::Bc3Unorm;
    case hdpack::PixelFormat::BC5: return rhi::Format::Bc5Unorm;
    default: return rhi::Format::Bc7Unorm;
    }
}

// MODIFIED for the Windows port: only the textures a notification touches are marked (cache.h says this is the intended way;
// the struct default marked all of them, so every flush rehashed every texture the frame drew). MH4N_TEXTURE_CHECK_ALL=1 brings
// the old behaviour back.
texture::TextureCache::Config CacheConfig() {
    texture::TextureCache::Config config;
    config.check_all_on_notify = false;
    const char* all = std::getenv("MH4N_TEXTURE_CHECK_ALL");
    if (all) config.check_all_on_notify = std::string_view{all} == "1";
    return config;
}

} // namespace

TextureBindings::TextureBindings(Host& host, std::shared_ptr<rhi::Backend> backend, Surfaces& surfaces,
                                 const NativeOptions& options, CounterMap& counters)
    : host_(host), backend_(std::move(backend)), surfaces_(surfaces), target_textures_(options.target_textures),
      counters_(counters), cache_(host, CacheConfig()), samplers_(backend_) {
    hd_.SetCompressedAllowed(backend_ && backend_->ChooseFormat(rhi::Format::Bc7Unorm, rhi::FormatUse::Sampled).conversion == rhi::Conversion::None);
    const char* enabled = std::getenv("MH4N_HD_TEXTURES");
    if (enabled && std::string_view{enabled} == "0") return;
    const char* packs = std::getenv("MH4N_TEXTURE_PACKS");
    const std::string user = options.options.GetString("--user");
    const std::filesystem::path root = user.empty() ? std::filesystem::path{} : std::filesystem::path(user) / "load" / "textures";
    hd_.Load(packs ? std::string(packs) : root.string());
}

Surface* TextureBindings::AsTarget(const texture::TextureRef& ref, bool shadow) {
    if (ref.cube || (!target_textures_ && !shadow)) return nullptr;
    const auto format = SurfaceFormat(ref.format);
    if (!format) return nullptr;
    SurfaceKey key{ref.paddr, ref.width, ref.height, *format, shadow};
    if (shadow && ref.format == texture::Format::RGBA8) return surfaces_.Get(key);
    return shadow ? surfaces_.Find(key) : surfaces_.FindTexture(key);
}

const rhi::Image* TextureBindings::NullImage(rhi::Recorder& recorder, bool cube, bool shadow,
                                             const std::function<void()>& outside_pass) {
    auto& image = null_images_[cube ? 1 : shadow ? 2 : 0];
    if (!image) {
        rhi::ImageDesc desc{};
        desc.width = desc.height = 1;
        desc.format = rhi::Format::Rgba8Unorm;
        desc.cube = cube;
        desc.layers = cube ? 6 : 1;
        desc.shadow_source = shadow;
        desc.name = "Unbound PICA texture";
        auto made = backend_->CreateImage(desc);
        if (!made) return nullptr;
        image = std::move(*made);
        outside_pass();
        for (u32 layer = 0; layer < desc.layers; ++layer) recorder.ClearImage(*image, {}, 0, layer);
    }
    return image.get();
}

void TextureBindings::Prepare(const pica::PicaState& state, const fragment::Needs& needs,
                              const std::function<void(MemoryRange, const Surface*)>& prepare_memory) {
    refs_.clear();
    targets_.fill(nullptr);
    if (needs.texture_units == 0) return;
    texture::TexturesOfDraw(state.regs.data(), refs_);
    const auto type = static_cast<texture::Type>(state.regs[texture::kTexture0 + 2] >> 28);
    // texture::TexturesOfDraw leaves shadow units out: a shadow receiver still reads its shadow map as a target.
    if (type == texture::Type::Shadow2D && (state.regs[texture::kMainConfig] & 1u) && needs.texture_units > 0) {
        const u32 size = state.regs[texture::kTexture0 + 1];
        const u32 format = state.regs[texture::kTexture0Format] & 0xf;
        texture::TextureRef shadow_ref{0, 0, false, (state.regs[texture::kTexture0 + 4] & 0x0fffffff) * 8,
                                       (size >> 16) & 0x7ff, size & 0x7ff, static_cast<texture::Format>(format)};
        if (shadow_ref.width && shadow_ref.height) refs_.insert(refs_.begin(), shadow_ref);
    }
    for (const auto& ref : refs_) {
        if (ref.unit >= needs.texture_units) continue;
        const bool shadow = ref.unit == 0 && type == texture::Type::Shadow2D;
        Surface* target = AsTarget(ref, shadow);
        if (shadow && !Shadows() && target && !target->shadow_cleared) target = nullptr;
        targets_[ref.unit] = target;
        const MemoryRange range{ref.paddr, texture::EncodedBytes(ref.format, ref.width, ref.height)};
        prepare_memory(range, target);
    }
}

std::shared_ptr<rhi::Image> TextureBindings::GuestImage(std::span<const texture::TextureRef> refs,
                                                       rhi::Recorder& recorder, rhi::Ring& staging,
                                                       const std::function<void()>& outside_pass) {
    const auto& first = refs.front();
    ImageKey key{.format = first.format, .width = first.width, .height = first.height, .cube = first.cube};
    const u32 layers = key.cube ? 6 : 1;
    if (refs.size() != layers) return {};
    if (key.cube && key.width != key.height) return {};

    std::array<u32, 6> generations{};
    std::array<texture::CacheKey, 6> guest_keys{};
    for (u32 face = 0; face < layers; ++face) {
        if (refs[face].face != face) return {};
        key.addresses[face] = refs[face].paddr;
        guest_keys[face] = {key.addresses[face], key.format, key.width, key.height};
        const auto* cached = cache_.Get(guest_keys[face]);
        if (!cached) return {};
        generations[face] = cached->generation;
    }
    auto old = images_.find(key);
    constexpr u64 kHdUploadPerFrame = HdTextures::kUploadPerFrame;
    std::optional<HdTextures::Replacement> pending_hd;
    if (old != images_.end() && old->second.generations == generations) {
        old->second.last_used = cache_.frame();
        auto& kept = old->second;
        if (!kept.hd_pending) return kept.image;
        // The pack's texture was being read by the workers: swap it in when it is ready (never making the game wait)
        const auto status = hd_.Status(*kept.hd_pending);
        if (status == HdTextures::State::Failed) {
            hd_.Refuse(*kept.hd_pending);
            kept.hd_pending.reset();
            return kept.image;
        }
        const u64 size = hd_.UploadBytes(*kept.hd_pending);
        if (status == HdTextures::State::Pending || (hd_upload_bytes_ != 0 && hd_upload_bytes_ + size > kHdUploadPerFrame)) {
            return kept.image;
        }
        pending_hd = kept.hd_pending;
    }
    outside_pass();

    rhi::ImageDesc desc{};
    desc.format = rhi::Format::Rgba8Unorm;
    desc.width = key.width;
    desc.height = key.height;
    desc.layers = layers;
    desc.cube = key.cube;
    desc.name = "PICA texture";
    std::optional<HdTextures::Replacement> hd;
    std::optional<HdTextures::Replacement> hd_wait;  // found, but its pixels are not ready yet
    std::vector<u8> hd_pixels;
    if (layers == 1 && hd_.active()) {
        std::optional<HdTextures::Replacement> found = pending_hd;
        if (!found) {
            const auto* pixels = cache_.Pixels(guest_keys[0]);
            const auto bytes = texture::EncodedBytes(key.format, key.width, key.height);
            const u8* memory = host_.PhysicalPointer(key.addresses[0], static_cast<u32>(bytes));
            if (pixels && memory) {
                hd_.Dump(pixels->content_hash, memory, bytes, key.width, key.height, key.format, pixels->rgba8);
                if (const auto* replacement = hd_.Find(pixels->content_hash, memory, bytes, key.width, key.height, key.format)) {
                    found = *replacement;
                }
            }
        }
        if (found) {
            const u64 size = hd_.UploadBytes(*found);
            const auto status = pending_hd ? HdTextures::State::Ready : hd_.Status(*found);
            // (a texture bigger than the cap goes up alone, first in its frame: the pack's 4096x4096 fonts are 64 MB)
            if (status == HdTextures::State::Ready && (hd_upload_bytes_ == 0 || hd_upload_bytes_ + size <= kHdUploadPerFrame)) {
                if (hd_.Take(*found, hd_pixels)) hd = found;
                else hd_.Refuse(*found);
            } else if (status == HdTextures::State::Failed) {
                hd_.Refuse(*found);
            } else {
                hd_wait = found;
            }
        }
    }
    if (hd) {
        desc.width = hd->width;
        desc.height = hd->height;
        const bool blocks = hd_.Compressed(*hd);
        if (blocks) {
            desc.format = BlockFormat(hd_.PackFormat(*hd));
            desc.mip_levels = std::min<u32>(hd_.Mips(*hd), std::bit_width(std::max(desc.width, desc.height)));
        }
        auto replacement = backend_->CreateImage(desc);
        const bool uploaded = replacement && (blocks ? UploadBlocks(*backend_, recorder, staging, **replacement, hd_.PackFormat(*hd),
                                                                    desc.mip_levels, hd_pixels)
                                                     : UploadFace(*backend_, recorder, staging, **replacement, 0, hd_pixels));
        desc.format = rhi::Format::Rgba8Unorm;
        desc.mip_levels = 1;
        if (uploaded) {
            const u64 vram = hd_.UploadBytes(*hd);
            if (old != images_.end()) retired_.push_back(std::move(old->second.image));
            hd_upload_bytes_ += vram;
            images_.insert_or_assign(key, Upload{*replacement, generations, cache_.frame(), vram, std::nullopt});
            cache_.ReleasePixels(guest_keys[0]);
            ++counters_["texture_uploads"];
            return *replacement;
        }
        if (replacement) retired_.push_back(std::move(*replacement));
        if (hd_.DelayUploadRetry(*hd)) hd_wait = hd;  // (once more later: a lack of GPU memory may pass)
        else hd_.Refuse(*hd);
        desc.width = key.width;
        desc.height = key.height;
    }
    auto image = backend_->CreateImage(desc);
    if (!image) return {};
    for (u32 face = 0; face < layers; ++face) {
        const auto* pixels = cache_.Pixels(guest_keys[face]);
        if (!pixels || !UploadFace(*backend_, recorder, staging, **image, face, pixels->rgba8)) {
            retired_.push_back(std::move(*image));
            return {};
        }
        cache_.ReleasePixels(guest_keys[face]);
    }
    if (old != images_.end()) retired_.push_back(std::move(old->second.image));
    images_.insert_or_assign(key, Upload{*image, generations, cache_.frame(), u64{key.width} * key.height * 4 * layers, hd_wait});
    ++counters_["texture_uploads"];
    return *image;
}

bool TextureBindings::Bind(const pica::PicaState& state, DrawDesc& draw, Surface* output,
                           rhi::Recorder& recorder, rhi::Ring& staging, const std::function<void()>& outside_pass) {
    using fragment::re::TextureType;
    const auto type = draw.fragment->texture.texture0_type();
    for (u32 unit = 0; unit < std::min<u32>(draw.needs.texture_units, 3); ++unit) {
        const bool shadow = unit == 0 && type == TextureType::Shadow2D;
        const bool cube = unit == 0 && (type == TextureType::TextureCube || type == TextureType::ShadowCube);
        if (!(state.regs[texture::kMainConfig] & (1u << unit))) {
            draw.units[unit].image = NullImage(recorder, cube, shadow, outside_pass);
            continue;
        }
        const auto first = std::find_if(refs_.begin(), refs_.end(), [unit](const auto& ref) { return ref.unit == unit; });
        const auto params = sampling::SamplerParamsOf(state, unit);
        counters_["texture_levels_missing"] += params.lod_max != 0;
        ++counters_["texture_binds"];
        if (first == refs_.end()) {
            ++counters_["texture_unavailable"];
            draw.units[unit].image = NullImage(recorder, cube, shadow, outside_pass);
            continue;
        }
        const auto previous = samplers_.creates();
        if (!shadow) {
            auto sampler = samplers_.Get(params);
            if (!sampler) return false;
            draw.units[unit].sampler = sampler->get();
        }
        counters_["sampler_creates"] += samplers_.creates() - previous;

        // Disabled shadow reads still need the dummy target that is cleared below.
        Surface* target = targets_[unit];
        if (shadow && !target) target = AsTarget(*first, true);
        if (target) {
            if (shadow && Shadows() && target->shadow_cleared) {
                std::ranges::fill(target->strips, Surface::Strip::Stale);
            }
            if (!surfaces_.Upload(*target, WholeImage(*target->image), recorder, staging, outside_pass)) return false;
            if (shadow && !Shadows() && !target->shadow_cleared) {
                rhi::ClearValue clear{};
                clear.color = {1, 1, 1, 1};
                outside_pass();
                recorder.ClearImage(*target->image, clear);
                target->shadow_cleared = true;
                target->sample_after_write = true;
                std::fill(target->feedback_stale.begin(), target->feedback_stale.end(), true);
            }
            draw.units[unit].image = surfaces_.Sample(*target, target == output, recorder, outside_pass);
            if (!draw.units[unit].image) return false;
            if (shadow) draw.units[unit].view = rhi::ImageView::ShadowStorage;
        } else {
            const auto end = std::find_if(first, refs_.end(), [unit](const auto& ref) { return ref.unit != unit; });
            const auto guest_start = std::chrono::steady_clock::now();
            const auto image = GuestImage(std::span{first, end}, recorder, staging, outside_pass);
            if (const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - guest_start).count();
                ms >= 15.0 && slow_texture_logs_ < 100) {
                ++slow_texture_logs_;
                MH4N_LOG(Info, "NativeGpu", "slow texture: {}x{} format {} took {:.1f} ms on the game thread (decode, upload)",
                         first->width, first->height, static_cast<u32>(first->format), ms);
            }
            if (image) {
                draw.units[unit].image = image.get();
            } else {
                ++counters_["texture_unavailable"];
                draw.units[unit] = {NullImage(recorder, cube, shadow, outside_pass), rhi::ImageView::Sampled, nullptr};
            }
        }
    }
    return true;
}

void TextureBindings::Notify(MemoryRange range) {
    const auto bytes = std::min(range.size, u64{std::numeric_limits<u32>::max()});
    cache_.OnGpuWrite(range.address, static_cast<u32>(bytes));
}

void TextureBindings::Submitted(rhi::TimelineValue value) {
    for (auto& image : retired_) backend_->Retire(std::move(image), value);
    retired_.clear();
}

void TextureBindings::Stats(u64& images, u64& bytes) const {
    images = images_.size();
    bytes = 0;
    for (const auto& [key, upload] : images_) bytes += upload.bytes;
}

void TextureBindings::NextFrame() {
    cache_.NextFrame();
    hd_.NextFrame();
    hd_upload_bytes_ = 0;
    if (hd_.TakeJustReady()) {
        // The pack is read: what was drawn meanwhile with the game's own textures is made again from it.
        for (auto& [key, upload] : images_) backend_->Retire(std::move(upload.image), backend_->LastSubmittedValue());
        images_.clear();
    }
    const u64 frame = cache_.frame();
    if (frame % 15 == 0) {
        // The budget: when the images kept pass it, the least recently used go first (never one used this frame).
        static const u64 budget = [] {
            const char* text = std::getenv("MH4N_TEXTURE_BUDGET_MB");
            const long mb = text ? std::atol(text) : 768;
            return u64{static_cast<u64>(mb > 0 ? mb : 768)} << 20;
        }();
        u64 images = 0, total = 0;
        Stats(images, total);
        while (total > budget) {
            auto oldest = images_.end();
            for (auto it = images_.begin(); it != images_.end(); ++it) {
                if (it->second.last_used >= frame) continue;
                if (oldest == images_.end() || it->second.last_used < oldest->second.last_used) oldest = it;
            }
            if (oldest == images_.end()) break;
            total -= std::min(total, oldest->second.bytes);
            backend_->Retire(std::move(oldest->second.image), backend_->LastSubmittedValue());
            images_.erase(oldest);
            ++counters_["textures_let_go"];
        }
    }
    if (frame % 60 != 0) return;
    for (auto it = images_.begin(); it != images_.end();) {
        if (frame - it->second.last_used > 120) {
            backend_->Retire(std::move(it->second.image), backend_->LastSubmittedValue());
            it = images_.erase(it);
        } else {
            ++it;
        }
    }
    cache_.Sweep(120, 5);
}

void TextureBindings::Reset() {
    images_.clear();
    cache_.Clear();
}

} // namespace mh4n::gpu::native
