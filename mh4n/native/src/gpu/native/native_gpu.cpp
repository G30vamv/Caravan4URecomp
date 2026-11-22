// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: gpu.h, gpu_debug.h, native_gpu.h, 3dbrew GPU/External Registers and GSP Shared Memory, and the GX and PICA tests.
#include "renderer.h"
#include "render_robust.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>

#include "gpu/gx/engine.h"
#include "gpu/pica/command_processor.h"
#include "mh4n/registry.h"

namespace mh4n::gpu::native {
namespace {

constexpr u32 kLcdBase = 0x1ED02000;
constexpr u32 kGpuBase = pica::kGpuMmioBase;
constexpr u32 kInternalBase = kGpuBase + 4 * pica::kExternalRegs;
constexpr std::array<u32, 2> kDisplayBase{0x1EF00400, 0x1EF00500};
constexpr std::array<u32, 2> kDisplayFill{0x1ED02204, 0x1ED02A04};
std::atomic<bool> shadows{true};

MemoryRange TransferOutput(const gx::TransferRegs& transfer);

CounterMap NewCounters() {
    CounterMap counters;
    for (const char* name : {
             "native_draws", "native_draws_dropped", "cpu_path_draws", "cpu_path_vertices",
             "cpu_path_unhandled", "cpu_path_load_failed", "translate_failures", "vertex_restart_widened",
             "texture_binds", "texture_uploads", "texture_unavailable", "texture_levels_missing",
             "sampler_creates", "fixup_draws", "pull_draws", "pull_vertices", "writebacks", "touch_draws_skipped",
             "vma_block_bytes", "vma_allocation_bytes", "vma_blocks", "vma_allocations",
             "stubbed:sampling", "stubbed:lighting", "stubbed:fog", "stubbed:proctex",
             "stubbed:shadow", "stubbed:gas"}) {
        counters.emplace(name, 0);
    }
    return counters;
}

u64 CopyFootprint(u32 size, u32 layout) {
    const u32 bytes = size & ~15u;
    const u32 line = (layout & 0xffff) * 16;
    const u32 gap = (layout >> 16) * 16;
    return line && bytes ? bytes + u64{(bytes - 1) / line} * gap : bytes;
}

class GpuFrontend final : public NativeGpu, public ScaledScreens, private pica::Backend {
public:
    GpuFrontend(Host& host, const NativeOptions& options)
        : host_(host), pica_host_(host, [this](MemoryRange range) {
              if (renderer_) renderer_->PrepareDrawMemory(range);
          }), options_(options), gx_(host), pica_(pica_host_, *this), counters_(NewCounters()) {
        options_.scale = std::clamp(options_.scale, 1u, kMaxScale);
        pica_.SetRegisterWriteLog(true);
        gx_.SetDisplayTransferHook([this](const gx::TransferRegs& transfer) {
            return renderer_ && renderer_->CaptureTransfer(transfer, TransferOutput(transfer));
        });
    }

    void Execute(const GxCommand& command) override;
    void WriteReg(u32 addr, u32 value) override;
    u32 ReadReg(u32 addr) override;
    void SetBufferSwap(u32 screen, const FrameBufferInfo& info) override { gx_.SetBufferSwap(screen, info); }
    void SetColorFill(u32 fill) override { gx_.SetColorFill(fill); }
    void FlushRegion(u32 address, u32 size) override;
    void InvalidateRegion(u32 address, u32 size) override;
    void VBlank(u32 screen) override;

    void LoadInitState(const InitState& state) override;
    std::optional<std::vector<RegisterWrite>> TakeRegisterWrites() override { return pica_.TakeRegisterWrites(); }
    void WriteBackColorBuffers() override {
        if (renderer_) renderer_->WriteBack();
    }
    std::vector<Counter> PeekCounters() const override {
        std::vector<Counter> result;
        result.reserve(counters_.size());
        for (const auto& [name, value] : counters_) result.push_back({name, value});
        return result;
    }
    std::vector<Counter> TakeFrameCounters() override {
        auto result = PeekCounters();
        if (renderer_) renderer_->FinishFrame(result);
        for (auto& [name, value] : counters_) {
            if (!name.starts_with("vma_") && name != kGpuInitFailed && name != kGpuTargetFailed) value = 0;
        }
        return result;
    }
    std::optional<u64> TakeFrameGpuTimeNs() override {
        return renderer_ ? renderer_->TakeGpuTime() : std::nullopt;
    }

    u32 ScreenScale() const override { return renderer_ ? renderer_->Scale() : options_.scale; }
    bool HasScreens() const override {
        return ScreenScale() > 1 || options_.async_screens || shared_screens_ || direct_screens_ || display_active_;
    }
    ScreenRead ReadScaledScreen(u32 paddr, u32 format, u32 stride, u32 rows, std::vector<u8>& rgba, bool wait,
                                u64* sequence) override {
        return renderer_ ? renderer_->ReadScreen(paddr, format, stride, rows, rgba, wait, sequence) : ScreenRead::None;
    }
    void PrepareMemory(u32 paddr, u32 size) override { Prepare({paddr, size}); }
    void SetSharedScreens(bool on) override {
        shared_screens_ = on;
        if (renderer_) renderer_->SetSharedScreens(on);
    }
    void SetDirectScreens(bool on) override {
        direct_screens_ = on;
        if (renderer_) renderer_->SetDirectScreens(on);
    }
    ScreenRead ReadBackendScreen(u32 paddr, u32 format, u32 stride, u32 rows, BackendScreen& out) override {
        return renderer_ ? renderer_->ReadBackendScreen(paddr, format, stride, rows, out) : ScreenRead::None;
    }
    void SetTouchScreenShown(bool shown) override { touch_shown_.store(shown, std::memory_order_relaxed); }
    ScreenRead ReadSharedScreen(u32 paddr, u32 format, u32 stride, u32 rows, SharedScreen& out) override {
        return renderer_ ? renderer_->ReadSharedScreen(paddr, format, stride, rows, out) : ScreenRead::None;
    }

private:
    void Draw(const pica::DrawCall& draw) override {
        if (!tried_renderer_) {
            tried_renderer_ = true;
            renderer_ = Renderer::Create(host_, options_, counters_);
            if (renderer_) {
                present::SetRendererCannotDraw(false);
                renderer_->SetSharedScreens(shared_screens_);
                renderer_->SetDirectScreens(direct_screens_);
            }
        }
        if (renderer_) renderer_->Draw(draw, touch_shown_.load(std::memory_order_relaxed));
        else ++counters_["native_draws_dropped"];
    }

    void Prepare(MemoryRange range) {
        if (renderer_ && range.size) renderer_->PrepareMemory(range);
    }
    void Changed(MemoryRange range, bool cpu_flush = false) {
        if (renderer_ && range.size) renderer_->NotifyWrite(range, cpu_flush);
    }
    MemoryRange FillRange(u32 start, u32 end, u32 control) const;
    void PrepareList(u32 index);
    gx::TransferRegs ReadTransfer();
    void LoadRegisters(std::span<const u8> block, u32 base, u32 count);

    Host& host_;
    GuestReadHost pica_host_;
    NativeOptions options_;
    gx::GxEngine gx_;
    pica::CommandProcessor pica_;
    CounterMap counters_;
    std::unique_ptr<Renderer> renderer_;
    std::atomic<bool> touch_shown_{true};
    bool shared_screens_ = false;
    bool direct_screens_ = false;
    bool display_active_ = false;
    bool tried_renderer_ = false;
};

void GpuFrontend::VBlank(u32 screen) {
    gx_.VBlank(screen);
    if (screen != 0 || !renderer_) return;
    renderer_->FrameTick();
    std::array<DisplayBuffers, 2> buffers{};
    for (size_t i = 0; i < buffers.size(); ++i) {
        const auto reg = [&](u32 offset) { return gx_.ReadReg(kDisplayBase[i] + offset); };
        const u32 selected = reg(0x78) & 1;
        buffers[i] = {reg(0x68 + 4 * selected), reg(0x68 + 4 * (1 - selected)), reg(0x70) & 7,
                      reg(0x90), (gx_.ReadReg(kDisplayFill[i]) & (1u << 24)) != 0, {reg(0x94), reg(0x98)}};
    }
    display_active_ = buffers[0].selected || buffers[1].selected;
    renderer_->EndDisplayFrame(buffers, touch_shown_.load(std::memory_order_relaxed));
}

MemoryRange GpuFrontend::FillRange(u32 start, u32 end, u32 control) const {
    if (!start || end <= start || !(control & 1)) return {};
    return {start, end - start};
}

gx::TransferRegs GpuFrontend::ReadTransfer() {
    const auto word = [this](u32 index) { return gx_.ReadReg(kGpuBase + 4 * (gx::kRegTransfer + index)); };
    return {word(0), word(1), word(2), word(3), word(4), word(5), word(6), word(7),
            word(8), word(9), word(10)};
}

MemoryRange TransferInput(const gx::TransferRegs& transfer) {
    const u64 size = transfer.IsTextureCopy()
        ? CopyFootprint(transfer.texture_copy_size, transfer.texture_copy_input_size)
        : u64{transfer.InputWidth()} * transfer.InputHeight() * gx::BytesPerPixel(transfer.InputFormat());
    return {transfer.InputAddress(), size};
}

MemoryRange TransferOutput(const gx::TransferRegs& transfer) {
    const u64 address = transfer.OutputAddress();
    if (transfer.IsTextureCopy()) {
        return {static_cast<u32>(address), CopyFootprint(transfer.texture_copy_size, transfer.texture_copy_output_size)};
    }
    const u32 scale = transfer.Scaling();
    if (scale > 2 || (scale && transfer.InputLinear())) return {};
    const u32 width = transfer.OutputWidth() >> (scale != 0);
    const u32 rows = transfer.OutputHeight() >> (scale == 2);
    const u32 bpp = gx::BytesPerPixel(transfer.OutputFormat());
    if (!width || !rows || !bpp) return {};
    u64 offset = 0;
    if (transfer.FlipVertically() && transfer.CropInputLines() && transfer.InputWidth() > width) {
        offset = u64{transfer.InputWidth() - width} * (rows - 1) * bpp;
    }
    if (address + offset > std::numeric_limits<u32>::max()) return {};
    return {static_cast<u32>(address + offset), u64{width} * rows * bpp};
}

void GpuFrontend::Execute(const GxCommand& command) {
    const u32* w = command.words;
    const auto id = static_cast<gx::CommandId>(w[0] & 0xff);
    if (id == gx::CommandId::SubmitCmdList) {
        const u32 address = gx_.ToPhysical(w[1]);
        Prepare({address, w[2]});
        pica_.SubmitGxCmdList(address, w[2]);
        if (renderer_) renderer_->SubmitDraws();
        return;
    }

    MemoryRange input{}, output{}, second{};
    bool transfer_hook_reads_input = false;
    switch (id) {
    case gx::CommandId::RequestDma:
        input = {gx_.ToPhysical(w[1]), w[3]};
        output = {gx_.ToPhysical(w[2]), w[3]};
        break;
    case gx::CommandId::MemoryFill:
        output = FillRange(gx_.ToPhysical(w[1]), gx_.ToPhysical(w[3]), w[7] & 0xffff);
        second = FillRange(gx_.ToPhysical(w[4]), gx_.ToPhysical(w[6]), w[7] >> 16);
        break;
    case gx::CommandId::DisplayTransfer: {
        const gx::TransferRegs transfer{gx_.ToPhysical(w[1]) >> 3, gx_.ToPhysical(w[2]) >> 3, w[4], w[3], w[5]};
        input = TransferInput(transfer);
        output = TransferOutput(transfer);
        transfer_hook_reads_input = !transfer.IsTextureCopy();
        break;
    }
    case gx::CommandId::TextureCopy: {
        gx::TransferRegs transfer{gx_.ToPhysical(w[1]) >> 3, gx_.ToPhysical(w[2]) >> 3};
        transfer.flags = w[6] | 8u;
        transfer.texture_copy_size = w[3];
        transfer.texture_copy_input_size = w[4];
        transfer.texture_copy_output_size = w[5];
        input = TransferInput(transfer);
        output = TransferOutput(transfer);
        break;
    }
    case gx::CommandId::CacheFlush:
        for (u32 i = 1; i < 7; i += 2) FlushRegion(w[i], w[i + 1]);
        break;
    default:
        break;
    }
    if (!transfer_hook_reads_input) {
        Prepare(input);
        Prepare(output);
        Changed(output);
    }
    Prepare(second);
    Changed(second);
    gx_.Execute(command);
}

void GpuFrontend::PrepareList(u32 index) {
    const auto& regs = pica_.state().regs;
    const u32 address = (regs[pica::reg::kCommandBufferAddress + index] & 0x0fffffff) * 8;
    const u32 size = (regs[pica::reg::kCommandBufferSize + index] & 0x000fffff) * 8;
    Prepare({address, size});
}

void GpuFrontend::WriteReg(u32 addr, u32 value) {
    if ((addr & 3) != 0) return;
    if (addr >= kInternalBase && addr < kInternalBase + 4 * pica::kNumRegs) {
        const u32 index = (addr - kInternalBase) / 4;
        if (value && index >= pica::reg::kCommandBufferTrigger && index < pica::reg::kCommandBufferTrigger + 2) {
            PrepareList(index - pica::reg::kCommandBufferTrigger);
        }
        pica_.WriteHardwareRegister(index, value);
        if (renderer_) renderer_->SubmitDraws();
        return;
    }

    MemoryRange output{};
    if (value & 1) {
        if (addr == kGpuBase + 4 * gx::kRegFillControl0 || addr == kGpuBase + 4 * gx::kRegFillControl1) {
            output = FillRange(gx_.ReadReg(addr - 12) * 8, gx_.ReadReg(addr - 8) * 8, value);
        } else if (addr == kGpuBase + 4 * gx::kRegTransferTrigger) {
            const gx::TransferRegs transfer = ReadTransfer();
            if (transfer.IsTextureCopy()) {
                Prepare(TransferInput(transfer));
                output = TransferOutput(transfer);
            }
        }
    }
    Prepare(output);
    Changed(output);
    gx_.WriteReg(addr, value);
}

u32 GpuFrontend::ReadReg(u32 addr) {
    if ((addr & 3) != 0) return 0;
    if (addr >= kInternalBase && addr < kInternalBase + 4 * pica::kNumRegs) {
        return pica_.state().regs[(addr - kInternalBase) / 4];
    }
    return gx_.ReadReg(addr);
}

void GpuFrontend::FlushRegion(u32 address, u32 size) {
    if (const auto physical = host_.VirtualToPhysical(address)) Changed({*physical, size}, true);
}

void GpuFrontend::InvalidateRegion(u32 address, u32 size) {
    if (!renderer_ || !size) return;
    if (const auto physical = host_.VirtualToPhysical(address)) renderer_->InvalidateGuestRange({*physical, size});
}

void GpuFrontend::LoadRegisters(std::span<const u8> block, u32 base, u32 count) {
    if (block.size() != count * sizeof(u32)) return;
    for (u32 i = 0; i < count; ++i) {
        u32 word;
        std::memcpy(&word, block.data() + i * sizeof(word), sizeof(word));
        if (base == kGpuBase && (i == gx::kRegFillControl0 || i == gx::kRegFillControl1 ||
                                 i == gx::kRegTransferTrigger)) {
            word &= ~1u;
        }
        gx_.WriteReg(base + i * 4, word);
    }
}

void GpuFrontend::LoadInitState(const InitState& state) {
    if (renderer_) renderer_->Reset();
    display_active_ = false;
    LoadRegisters(state.Block(InitBlock::GpuRegs), kGpuBase, pica::kExternalRegs);
    LoadRegisters(state.Block(InitBlock::LcdRegs), kLcdBase, gx::kLcdRegCount);
    pica_.LoadInitState(state);
}

} // namespace

void SetShadows(bool on) { shadows.store(on, std::memory_order_relaxed); }

bool Shadows() { return shadows.load(std::memory_order_relaxed); }

std::unique_ptr<NativeGpu> MakeNativeGpu(Host& host, const NativeOptions& options) {
    return std::make_unique<GpuFrontend>(host, options);
}

MH4N_REGISTER(GpuFactory, "native", [](Host& host, const Options& options) {
    NativeOptions native;
    native.options = options;
    native.force_ubershader = options.Has("--force-ubershader");
    native.scale = static_cast<u32>(std::clamp<u64>(options.GetUInt("--scale", 1), 1, kMaxScale));
    native.target_textures = options.GetString("--target-textures", "on") != "off";
    const auto gpu_name = options.GetString("--gpu", "");
    const bool display = gpu_name == "native-display" || gpu_name == "native-display-queue";
    native.async_screens = options.GetString("--async-screens", display ? "on" : "off") == "on";
    native.lighting_fixup = options.GetString("--lighting-fixup", "auto");
    return MakeNativeGpu(host, native);
});

MH4N_OPTION("capture-player", "--force-ubershader", "", "--gpu native: use the fragment ubershader");
MH4N_OPTION("capture-player", "--scale", "N", "--gpu native: internal resolution multiplier");
MH4N_OPTION("capture-player", "--target-textures", "on|off", "--gpu native: sample render targets directly");
MH4N_OPTION("capture-player", "--async-screens", "on|off", "--gpu native: prepare display transfers on the GPU");
MH4N_OPTION("capture-player", "--lighting-fixup", "MODE", "--gpu native: quaternion fix-up (gs, vs, off, auto)");
MH4N_OPTION("runner", "--scale", "N", "--gpu native-display: internal resolution multiplier");
MH4N_OPTION("runner", "--target-textures", "on|off", "--gpu native-display: sample render targets directly");
MH4N_OPTION("runner", "--async-screens", "on|off", "--gpu native-display: prepare display transfers on the GPU");
MH4N_OPTION("runner", "--lighting-fixup", "MODE", "--gpu native-display: quaternion fix-up (gs, vs, off, auto)");

} // namespace mh4n::gpu::native
