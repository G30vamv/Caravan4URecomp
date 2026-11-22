// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: the GPU and capture contracts, gpu_pica_test, and 3dbrew GPU/External Registers.

#include "command_processor.h"

#include <cstdlib>
#include <cstring>

#include "mh4n/log.h"

namespace mh4n::gpu::pica {
namespace {

constexpr char kLog[] = "GPU.PICA";
constexpr u32 kLcdBase = 0x1ED02000;

class DiscardDraws final : public Backend {
public:
    void Draw(const DrawCall&) override {}
};

template <size_t N>
void LoadRegisters(std::array<u32, N>& registers, const InitState& init, InitBlock block) {
    registers.fill(0);
    const auto bytes = init.Block(block);
    if (bytes.size() == sizeof(registers)) {
        std::memcpy(registers.data(), bytes.data(), bytes.size());
    } else if (!bytes.empty()) {
        MH4N_LOG(Warning, kLog, "INIT block {} has {} bytes, expected {}",
                 static_cast<u32>(block), bytes.size(), sizeof(registers));
    }
}

class PicaOnlyGpu final : public Gpu, public GpuDebug {
public:
    explicit PicaOnlyGpu(Host& host) : host_(host), processor_(host, backend_) {
        processor_.SetRegisterWriteLog(true);
    }

    void Execute(const GxCommand& command) override {
        if ((command.words[0] & 0xFF) == 1) {
            if (const auto address = host_.VirtualToPhysical(command.words[1])) {
                processor_.SubmitGxCmdList(*address, command.words[2]);
            }
        }
    }

    void WriteReg(u32 addr, u32 value) override {
        if (addr >= kInternalBase && addr - kInternalBase < kNumRegs * 4 && addr % 4 == 0) {
            processor_.WriteHardwareRegister((addr - kInternalBase) / 4, value);
        } else if (auto* word = ExternalRegister(addr)) {
            *word = value;
        }
    }

    u32 ReadReg(u32 addr) override {
        if (addr >= kInternalBase && addr - kInternalBase < kNumRegs * 4 && addr % 4 == 0) {
            return processor_.state().regs[(addr - kInternalBase) / 4];
        }
        const auto* word = ExternalRegister(addr);
        return word == nullptr ? 0 : *word;
    }

    void SetBufferSwap(u32, const FrameBufferInfo&) override {}
    void SetColorFill(u32) override {}
    void FlushRegion(u32, u32) override {}
    void InvalidateRegion(u32, u32) override {}
    void VBlank(u32) override {}

    void LoadInitState(const InitState& init) override {
        LoadRegisters(external_, init, InitBlock::GpuRegs);
        LoadRegisters(lcd_, init, InitBlock::LcdRegs);
        processor_.LoadInitState(init);
    }

    std::optional<std::vector<RegisterWrite>> TakeRegisterWrites() override {
        return processor_.TakeRegisterWrites();
    }

    std::vector<Counter> TakeFrameCounters() override {
        const u32 draws = processor_.draws_issued();
        const u32 lists = processor_.command_lists_processed();
        const u64 gas = processor_.gas_lut_writes();
        std::vector<Counter> counters{
            {"pica_draws", draws - previous_draws_},
            {"pica_cmdlists", lists - previous_lists_},
            {"pica_gas_lut_writes", gas - previous_gas_},
        };
        previous_draws_ = draws;
        previous_lists_ = lists;
        previous_gas_ = gas;
        return counters;
    }

private:
    u32* ExternalRegister(u32 addr) {
        if (addr % 4 != 0) {
            return nullptr;
        }
        if (addr >= kGpuMmioBase && addr < kInternalBase) {
            return &external_[(addr - kGpuMmioBase) / 4];
        }
        if (addr >= kLcdBase && addr - kLcdBase < lcd_.size() * 4) {
            return &lcd_[(addr - kLcdBase) / 4];
        }
        return nullptr;
    }

    static constexpr u32 kInternalBase = kGpuMmioBase + kExternalRegs * 4;
    Host& host_;
    DiscardDraws backend_;
    CommandProcessor processor_;
    std::array<u32, kExternalRegs> external_{};
    std::array<u32, 0x400> lcd_{};
    u32 previous_draws_ = 0;
    u32 previous_lists_ = 0;
    u64 previous_gas_ = 0;
};

MH4N_REGISTER(GpuFactory, "pica-only", [](Host& host, const Options&) {
    return std::make_unique<PicaOnlyGpu>(host);
});

} // namespace
} // namespace mh4n::gpu::pica
