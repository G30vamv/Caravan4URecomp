// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: native_gpu.h, the RHI and PICA contracts, and the native GPU tests.
#pragma once

#include <map>
#include <optional>
#include <utility>

#include "gpu/backend/shared/shared_images.h"
#include "gpu/cpuvertex/primitive_assembly.h"
#include "gpu/cpuvertex/triangle_batch.h"
#include "gpu/fragment/sampling/sampling.h"
#include "gpu/gx/blitter.h"
#include "gpu/texture/cache.h"
#include "gpu/texture/texture_regs.h"
#include "hd_textures.h"
#include "native_gpu.h"
#include "pipeline_builder.h"
#include "scaled_screens.h"

namespace mh4n::gpu::native {

struct MemoryRange {
    u32 address;
    u64 size;

    u64 End() const { return u64{address} + size; }
    bool Overlaps(MemoryRange other) const {
        return size && other.size && address < other.End() && other.address < End();
    }
};

class GuestReadHost final : public Host {
public:
    GuestReadHost(Host& host, std::function<void(MemoryRange)> prepare) : host_(host), prepare_(std::move(prepare)) {}
    u8* PhysicalPointer(u32 address, u32 size) override {
        if (size) prepare_({address, size});
        return host_.PhysicalPointer(address, size);
    }
    std::optional<u32> VirtualToPhysical(u32 address) override { return host_.VirtualToPhysical(address); }
    u8* GuestPointer(u32 address, u32 size) override { return host_.GuestPointer(address, size); }
    void SignalInterrupt(Interrupt id, u64 delay_ns) override { host_.SignalInterrupt(id, delay_ns); }
    u64 InterruptDelayNs() const override { return host_.InterruptDelayNs(); }
    void Post(std::function<void()> work) override { host_.Post(std::move(work)); }
    void* CodeSymbol(const char* name) override { return host_.CodeSymbol(name); }

private:
    Host& host_;
    std::function<void(MemoryRange)> prepare_;
};

struct SurfaceKey {
    u32 address;
    u32 width;
    u32 height;
    rhi::PicaFormat format;
    bool shadow = false;

    auto operator<=>(const SurfaceKey&) const = default;
    u32 StorageWidth() const { return (width + 7) & ~7u; }
    u32 StorageHeight() const { return (height + 7) & ~7u; }
    u32 BytesPerPixel() const;
    u32 StripBytes() const { return StorageWidth() * 8 * BytesPerPixel(); }
    bool IsDepth() const { return format >= rhi::PicaFormat::D16; }
    MemoryRange Range() const { return {address, u64{StripBytes()} * (StorageHeight() / 8)}; }
};

struct Surface {
    enum class Strip : u8 { Stale, Current, Dirty };

    SurfaceKey key;
    std::shared_ptr<rhi::Image> image;
    std::shared_ptr<rhi::Image> feedback;
    std::vector<Strip> strips;
    std::vector<bool> feedback_stale;
    bool initialized = false;
    bool sample_after_write = true;
    bool shadow_cleared = false;

    MemoryRange StripRange(u32 first, u32 count = 1) const {
        return {key.address + first * key.StripBytes(), u64{count} * key.StripBytes()};
    }
};

using CounterMap = std::map<std::string, u64>;

class Surfaces {
public:
    Surfaces(Host& host, std::shared_ptr<rhi::Backend> backend, u32 scale, CounterMap& counters)
        : host_(host), backend_(std::move(backend)), scale_(scale), counters_(counters) {}

    Surface* Get(const SurfaceKey& key);
    Surface* Find(const SurfaceKey& key);
    Surface* FindTexture(const SurfaceKey& key);
    Surface* FindTransferSource(const gx::TransferRegs& transfer, u32& first_row);
    bool Upload(Surface& surface, const rhi::Rect& area, rhi::Recorder& recorder, rhi::Ring& staging,
                const std::function<void()>& outside_pass);
    const rhi::Image* Sample(Surface& surface, bool feedback, rhi::Recorder& recorder,
                             const std::function<void()>& outside_pass);
    u64 TakeFeedbackCopies() { return std::exchange(feedback_copies_, 0); }
    void PrepareDraw(Surface* color, Surface* depth, const rhi::Rect& area, const std::function<void()>& before_read);
    void Written(Surface& surface, const rhi::Rect& area);
    void WriteBack(MemoryRange range, const Surface* except = nullptr, bool preserve_guest = false,
                   const std::function<void()>& before_read = {}, bool use_aliases = false);
    void Invalidate(MemoryRange range, const Surface* except = nullptr);
    void Clear();
    rhi::Format ColorFormat(const Surface& surface) const;
    rhi::Format DepthFormat(const Surface& surface) const;

private:
    std::pair<u32, u32> StripInterval(const Surface& surface, const rhi::Rect& area) const;
    void CopyAliases(Surface& surface, u32 first, u32 end, rhi::Recorder& recorder);
    void WriteBackStrips(Surface& surface, u32 first, u32 end, MemoryRange keep);
    bool ConvertForSampling(const Surface& surface, u32 bits, rhi::Recorder& recorder,
                            std::span<const rhi::ImageCopyRegion> regions);

    Host& host_;
    std::shared_ptr<rhi::Backend> backend_;
    u32 scale_;
    CounterMap& counters_;
    std::map<SurfaceKey, Surface> cache_;
    u64 feedback_copies_ = 0;
    std::shared_ptr<rhi::Pipeline> sample_conversion_;
    std::unique_ptr<rhi::BindingArena> sample_bindings_;
};

class TextureBindings {
public:
    TextureBindings(Host& host, std::shared_ptr<rhi::Backend> backend, Surfaces& surfaces,
                    const NativeOptions& options, CounterMap& counters);

    void Prepare(const pica::PicaState& state, const fragment::Needs& needs,
                 const std::function<void(MemoryRange, const Surface*)>& prepare_memory);
    bool Bind(const pica::PicaState& state, DrawDesc& draw, Surface* output, rhi::Recorder& recorder,
              rhi::Ring& staging, const std::function<void()>& outside_pass);
    void Notify(MemoryRange range);
    void Submitted(rhi::TimelineValue value);
    void NextFrame();
    void Reset();
    // The guest-texture images kept (count and bytes), for the memory line; the budget in MB (env MH4N_TEXTURE_BUDGET_MB).
    void Stats(u64& images, u64& bytes) const;

private:
    struct Upload {
        std::shared_ptr<rhi::Image> image;
        std::array<u32, 6> generations{};
        u64 last_used = 0;
        u64 bytes = 0;
        std::optional<HdTextures::Replacement> hd_pending;  // the pack's texture is still being read: this is the game's own
    };
    struct ImageKey {
        std::array<u32, 6> addresses{};
        texture::Format format = texture::Format::RGBA8;
        u32 width = 0;
        u32 height = 0;
        bool cube = false;

        auto operator<=>(const ImageKey&) const = default;
    };

    Surface* AsTarget(const texture::TextureRef& ref, bool shadow);
    const rhi::Image* NullImage(rhi::Recorder& recorder, bool cube, bool shadow, const std::function<void()>& outside_pass);
    std::shared_ptr<rhi::Image> GuestImage(std::span<const texture::TextureRef> refs, rhi::Recorder& recorder,
                                          rhi::Ring& staging, const std::function<void()>& outside_pass);

    Host& host_;
    std::shared_ptr<rhi::Backend> backend_;
    Surfaces& surfaces_;
    bool target_textures_;
    CounterMap& counters_;
    texture::TextureCache cache_;
    fragment::sampling::SamplerCache samplers_;
    HdTextures hd_;
    u32 slow_texture_logs_ = 0;
    u64 hd_upload_bytes_ = 0;  // what was uploaded from the pack this frame (a few big ones at a time, not all at once)
    std::vector<texture::TextureRef> refs_;
    std::array<Surface*, 3> targets_{};
    std::map<ImageKey, Upload> images_;
    std::vector<std::shared_ptr<rhi::Image>> retired_;
    std::array<std::shared_ptr<rhi::Image>, 3> null_images_;
};

struct DisplayBuffers {
    u32 selected = 0;
    u32 alternate = 0;
    u32 format = 0;
    u32 stride = 0;
    bool fill = false;
    std::array<u32, 2> right{};

    bool Contains(u32 address) const {
        return address && (address == selected || address == alternate);
    }
};

class ScreenPictures {
public:
    explicit ScreenPictures(std::shared_ptr<rhi::Backend> backend) : backend_(std::move(backend)) {}
    std::shared_ptr<const present::SharedPicture> Make(const std::shared_ptr<rhi::Image>& source, u32 format, bool flip);
    void Reset();

private:
    bool Init();

    std::shared_ptr<rhi::Backend> backend_;
    std::unique_ptr<rhi::SharedImages> shared_;
    std::vector<std::shared_ptr<rhi::SharedImage>> shared_pool_;
    std::shared_ptr<rhi::Pipeline> pipeline_;
    std::unique_ptr<rhi::BindingArena> bindings_;
    bool initialized_ = false;
};

class Renderer {
public:
    static std::unique_ptr<Renderer> Create(Host& host, const NativeOptions& options, CounterMap& counters);
    ~Renderer();

    void Draw(const pica::DrawCall& draw, bool touch_shown);
    void SubmitDraws();
    void PrepareMemory(MemoryRange range);
    void InvalidateGuestRange(MemoryRange range);
    void NotifyWrite(MemoryRange range, bool cpu_flush = false);
    void WriteBack();
    void Reset();
    void FinishFrame(std::vector<Counter>& counters);
    // Once per frame (the top screen's vblank): ages the texture caches, keeps them inside their budget, and now and
    // then writes the memory line.
    void FrameTick();
    std::optional<u64> TakeGpuTime();
    u32 Scale() const { return scale_; }
    bool CaptureTransfer(const gx::TransferRegs& transfer, MemoryRange output);
    void SetSharedScreens(bool on) { shared_screens_ = on; }
    void SetDirectScreens(bool on) { direct_screens_ = on; }
    ScaledScreens::ScreenRead ReadBackendScreen(u32 address, u32 format, u32 stride, u32 rows, ScaledScreens::BackendScreen& out);
    void EndDisplayFrame(const std::array<DisplayBuffers, 2>& buffers, bool touch_shown);
    ScaledScreens::ScreenRead ReadScreen(u32 address, u32 format, u32 stride, u32 rows, std::vector<u8>& rgba,
                                        bool wait, u64* sequence);
    ScaledScreens::ScreenRead ReadSharedScreen(u32 address, u32 format, u32 stride, u32 rows,
                                               ScaledScreens::SharedScreen& out);
    void PrepareDrawMemory(MemoryRange range);

private:
    Renderer(Host& host, const NativeOptions& options, CounterMap& counters, std::shared_ptr<rhi::Backend> backend,
             u32 scale);

    bool Init();
    void RecordDraw(const pica::DrawCall& source, DrawDesc& draw, Surface* color, Surface* depth,
                    bool flip_y, bool uniforms_changed);
    rhi::Recorder& Recording();
    void ReserveVertexSpace(u64 bytes);
    bool UploadVertices(DrawDesc& draw, const vertex::LoadedDraw& loaded,
                        std::array<rhi::VertexBufferBinding, rhi::kMaxVertexBindings>& buffers);
    bool PreparePull(DrawDesc& draw, const pica::DrawCall& source, rhi::Recorder& recorder);
    bool UploadUniforms(DrawDesc& draw, const pica::PicaState& state, bool flip_y, bool changed);
    void Drop(std::string_view reason);
    void InvalidateScreens(MemoryRange range);
    void MaterializeScreens(MemoryRange range, bool preserve_guest = false,
                            const std::function<void()>& before_read = {});

    struct TransferTarget {
        SurfaceKey key;
        u32 first_row;
        u32 width;
        u32 rows;
    };
    struct Screen {
        MemoryRange range{};
        u32 format = 0, row_pixels = 0, rows = 0;
        bool flip = false;
        u64 sequence = 0;
        rhi::TimelineValue ready = 0;
        std::shared_ptr<rhi::Image> image;
        std::shared_ptr<rhi::Buffer> readback;
        std::optional<gx::TransferRegs> owed;
        TransferTarget source;
        std::shared_ptr<const present::SharedPicture> picture;
        bool complete = true;
        bool published_direct = false;  // handed to the display as it is (ReadBackendScreen): a late CPU readback leaves its layout alone
    };
    bool SkipTouchDraw(const SurfaceKey& key, const rhi::Rect& area) const;
    Screen* FindScreen(u32 address, u32 format, u32 stride, u32 rows);
    bool RecordScreenReadback(Screen& screen);
    void LearnDisplayTargets(const std::array<DisplayBuffers, 2>& buffers);
    bool Materialize(Screen& screen, MemoryRange preserve);

    Host& host_;
    GuestReadHost vertex_host_;
    const NativeOptions& options_;
    CounterMap& counters_;
    std::shared_ptr<rhi::Backend> backend_;
    u32 scale_;
    PipelineBuilder builder_;
    std::unique_ptr<PipelineProvider> provider_;
    std::unique_ptr<DrawComposer> composer_;
    fragment::FragmentState fragment_;
    std::unique_ptr<fragment::TableRings> tables_;
    std::unique_ptr<fragment::FragmentUniformRing> fragment_uniforms_;
    std::unique_ptr<rhi::Ring> uniforms_, vertices_, staging_;
    std::unique_ptr<rhi::BindingArena> fixup_sets_;
    std::shared_ptr<rhi::Pipeline> flip_pipeline_;
    std::vector<std::shared_ptr<rhi::Pipeline>> recorded_quaternions_;
    Surfaces surfaces_;
    TextureBindings textures_;
    vertex::VertexLoader loader_;
    vertex::LoadedDraw loaded_;
    cpuvertex::CpuVertexRunner cpu_;
    cpuvertex::PrimitiveAssembler assembler_;
    cpuvertex::TriangleBatch batch_;
    ScreenPictures screen_pictures_;
    std::map<u32, std::shared_ptr<Screen>> screens_;
    std::array<DisplayBuffers, 2> display_buffers_{};
    std::optional<TransferTarget> touch_target_;
    std::array<std::optional<TransferTarget>, 4> top_targets_{};
    std::array<std::shared_ptr<Screen>, 2> complete_screens_{};
    u64 last_vblank_sequence_ = 0;
    bool shared_screens_ = false;
    bool direct_screens_ = false;  // the display takes the screens images as they are (ReadBackendScreen): no readback
    bool pending_draw_ = false;
    bool holding_screens_ = false;
    std::optional<rhi::Recorder> recording_;
    u64 vertex_bytes_ = 0;
    u32 attempted_draws_ = 0, recorded_draws_ = 0;
    u64 frame_ticks_ = 0;
    std::vector<std::shared_ptr<rhi::Buffer>> readback_pool_;
    // The screens conversion to upright RGBA runs on worker threads, one picture behind (render_screens.cpp, ReadScreen)
    struct ScreenJob;
    std::map<u64, std::shared_ptr<ScreenJob>> screen_jobs_;
    u64 screen_sequence_ = 0;
    rhi::TimelineValue last_submission_ = 0;
    u64 gpu_time_total_ = 0;
    bool geometry_fixup_ = false;
    bool pull_fixup_ = false;
    u32 logged_drops_ = 0;
};

} // namespace mh4n::gpu::native
