// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: the GX and GPU host contracts, gpu_gx_test, WP68 GSP state, and 3dbrew GSP Shared Memory and GPU/External Registers.

#include "engine.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "mh4n/log.h"

namespace mh4n::gpu::gx {
namespace {

constexpr char kLog[] = "GPU.GX";
constexpr u32 kGpuBase = 0x1EF00000;
constexpr u32 kLcdBase = 0x1ED02000;
constexpr u32 kFillWords = 4;

std::span<u8> DmaWindow(Host& host, u32 address, std::optional<u32> physical, u32 size) {
    if (auto* bytes = host.GuestPointer(address, size)) {
        return {bytes, size};
    }
    return physical ? Window(host, *physical, size) : std::span<u8>{};
}

} // namespace

CopyMode GetCopyMode(bool input_vram, bool output_vram) {
    return static_cast<CopyMode>((input_vram ? 2u : 0u) | (output_vram ? 1u : 0u));
}

u64 CalculateDelayNanoseconds(CopyMode mode, bool is_texture, size_t size) {
    constexpr std::array<u32, 4> dma{190, 310, 380, 380};
    constexpr std::array<u32, 4> texture{450, 3100, 5400, 5400};
    const u32 index = static_cast<u32>(mode);
    const u32 speed = (is_texture ? texture : dma)[index];
    return static_cast<u64>(static_cast<double>(size) * 1e9 / (speed * 1048576.0));
}

u32 GxEngine::ToPhysical(u32 vaddr) {
    if (vaddr == 0) {
        return 0;
    }
    const auto address = host_.VirtualToPhysical(vaddr);
    if (!address) {
        MH4N_LOG(Debug, kLog, "Untranslated GX address {:#010x}", vaddr);
    }
    return address.value_or(vaddr);
}

void GxEngine::Execute(const GxCommand& command) {
    const u32* words = command.words;
    const auto id = static_cast<CommandId>(words[0] & 0xFF);
    switch (id) {
    case CommandId::RequestDma:
        Dma(command);
        break;
    case CommandId::MemoryFill: {
        const bool first = words[1] != 0;
        const bool second = words[4] != 0;
        for (u32 i = 0; i < 2; ++i) {
            const u32 argument = 1 + 3 * i;
            const u32 base = kRegFill0 + kFillWords * i;
            if (words[argument] == 0) {
                continue;
            }
            regs_[base] = ToPhysical(words[argument]) >> 3;
            regs_[base + 1] = ToPhysical(words[argument + 2]) >> 3;
            regs_[base + 2] = words[argument + 1];
            regs_[base + 3] = (words[7] >> (16 * i)) & 0xFFFF;
            const u32 interrupt = i == 0 ? (second ? 2 : 0) : (first ? 0 : 1);
            MemoryFill(i, interrupt);
        }
        break;
    }
    case CommandId::DisplayTransfer:
    case CommandId::TextureCopy:
        regs_[kRegTransfer] = ToPhysical(words[1]) >> 3;
        regs_[kRegTransfer + 1] = ToPhysical(words[2]) >> 3;
        if (id == CommandId::TextureCopy) {
            regs_[kRegTransfer + 8] = words[3];
            regs_[kRegTransfer + 9] = words[4];
            regs_[kRegTransfer + 10] = words[5];
            regs_[kRegTransfer + 4] = words[6];
        } else {
            regs_[kRegTransfer + 2] = words[4];
            regs_[kRegTransfer + 3] = words[3];
            regs_[kRegTransfer + 4] = words[5];
            regs_[kRegTransfer + 5] = 0;
        }
        regs_[kRegTransferTrigger] = 1;
        MemoryTransfer();
        break;
    default:
        break;
    }
}

void GxEngine::WriteReg(u32 addr, u32 value) {
    if (addr % 4 != 0) {
        return;
    }
    if (addr >= kLcdBase && addr - kLcdBase < kLcdRegCount * 4) {
        lcd_[(addr - kLcdBase) / 4] = value;
    } else if (addr >= kGpuBase && addr - kGpuBase < kGpuRegCount * 4) {
        const u32 index = (addr - kGpuBase) / 4;
        regs_[index] = value;
        switch (index) {
        case kRegFillControl0: MemoryFill(0, 0); break;
        case kRegFillControl1: MemoryFill(1, 1); break;
        case kRegTransferTrigger: MemoryTransfer(); break;
        default: break;
        }
    }
}

u32 GxEngine::ReadReg(u32 addr) {
    if (addr % 4 == 0) {
        if (addr >= kGpuBase && addr - kGpuBase < kGpuRegCount * 4) {
            return regs_[(addr - kGpuBase) / 4];
        }
        if (addr >= kLcdBase && addr - kLcdBase < kLcdRegCount * 4) {
            return lcd_[(addr - kLcdBase) / 4];
        }
    }
    return 0;
}

void GxEngine::SetBufferSwap(u32 screen, const FrameBufferInfo& info) {
    if (screen >= 2) {
        return;
    }
    const u32 base = kRegFramebuffer + 0x40 * screen;
    const u32 slot = info.active_fb & 1;
    regs_[base + 0x1A + slot] = ToPhysical(info.address_left);
    regs_[base + 0x25 + slot] = ToPhysical(info.address_right);
    regs_[base + 0x1C] = info.format;
    regs_[base + 0x1E] = (regs_[base + 0x1E] & ~1u) | (info.shown_fb & 1);
    regs_[base + 0x24] = info.stride;
}

void GxEngine::SetColorFill(u32 fill) {
    lcd_[kLcdColorFillTop] = fill;
    lcd_[kLcdColorFillBottom] = fill;
}

void GxEngine::Dma(const GxCommand& command) {
    const u32* words = command.words;
    const u32 size = words[3];
    const auto source = host_.VirtualToPhysical(words[1]);
    const auto dest = host_.VirtualToPhysical(words[2]);
    const auto input = DmaWindow(host_, words[1], source, size);
    const auto output = DmaWindow(host_, words[2], dest, size);
    const size_t bytes = std::min(input.size(), output.size());
    if (bytes != 0) {
        std::memmove(output.data(), input.data(), bytes);
    }
    if (bytes != size && dma_reports_ < 8) {
        ++dma_reports_;
        MH4N_LOG(Debug, kLog, "DMA copied {} of {} bytes", bytes, size);
    }
    const auto vram = [](std::optional<u32> address) {
        return address && (*address < 0x20000000 || *address >= 0x28000000);
    };
    host_.SignalInterrupt(Interrupt::DMA,
                          CalculateDelayNanoseconds(GetCopyMode(vram(source), vram(dest)), false,
                                                    size));
}

FillRegs GxEngine::LoadFill(u32 index) const {
    const u32* words = &regs_[kRegFill0 + kFillWords * index];
    return {words[0], words[1], words[2], words[3]};
}

TransferRegs GxEngine::LoadTransfer() const {
    const u32* words = &regs_[kRegTransfer];
    return {words[0], words[1], words[2], words[3], words[4], words[5],
            words[6], words[7], words[8], words[9], words[10]};
}

void GxEngine::MemoryFill(u32 index, u32 interrupt_index) {
    const FillRegs fill = LoadFill(index);
    if (!fill.Trigger()) {
        return;
    }
    if (fill.StartAddress() != 0) {
        gx::MemoryFill(host_, fill);
        if (interrupt_index < 2) {
            const auto interrupt = interrupt_index == 0 ? Interrupt::PSC0 : Interrupt::PSC1;
            const u32 bytes = fill.EndAddress() - fill.StartAddress();
            host_.SignalInterrupt(interrupt,
                                  CalculateDelayNanoseconds(CopyMode::VramToVram, true, bytes));
        }
    }
    regs_[kRegFillControl0 + kFillWords * index] = (fill.control & ~1u) | 2;
}

void GxEngine::MemoryTransfer() {
    const TransferRegs transfer = LoadTransfer();
    if (!transfer.Triggered()) {
        return;
    }
    u64 bytes;
    if (transfer.IsTextureCopy()) {
        TextureCopy(host_, transfer);
        bytes = transfer.texture_copy_size;
    } else {
        if (!display_transfer_hook_ || !display_transfer_hook_(transfer)) {
            DisplayTransfer(host_, transfer);
        }
        bytes = u64{transfer.InputWidth()} * transfer.InputHeight() *
                BytesPerPixel(transfer.InputFormat());
    }
    regs_[kRegTransferTrigger] &= ~1u;
    const auto mode = GetCopyMode(transfer.IsInputVram(), transfer.IsOutputVram());
    host_.SignalInterrupt(Interrupt::PPF, CalculateDelayNanoseconds(mode, true, bytes));
}

MH4N_REGISTER(GpuFactory, "cpu-gx", [](Host& host, const Options&) {
    return std::make_unique<GxEngine>(host);
});

} // namespace mh4n::gpu::gx
