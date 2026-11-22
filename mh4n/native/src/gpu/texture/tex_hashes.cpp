// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: tex_hashes.h, gpu_texture_test.cpp, and the capture and PICA contracts.

#include "tex_hashes.h"
#include "hash.h"
#include "texture_regs.h"
#include "gpu/pica/command_processor.h"
#include "gpu_capture/check.h"
#include "mh4n/log.h"

#include <fstream>
#include <iterator>
#include <set>
#include <tuple>
#include <fmt/format.h>

namespace mh4n::gpu::texture {
namespace {

constexpr auto kLog = "TexHashes";

// Replay command lists against the player's memory without signalling interrupts on its GPU.
class TextureCheckHost final : public gpu::Host {
public:
    gpu_capture::CaptureHost* capture = nullptr;

    u8* PhysicalPointer(u32 paddr, u32 size) override {
        return capture->PhysicalPointer(paddr, size);
    }

    std::optional<u32> VirtualToPhysical(u32 vaddr) override {
        return capture->VirtualToPhysical(vaddr);
    }

    void SignalInterrupt(gpu::Interrupt, u64) override {}

    void Post(std::function<void()> work) override {
        work();
    }
};

struct HashList {
    void Add(u64 hash, const TextureRef& texture) {
        if (seen.emplace(hash, texture.width, texture.height, texture.format).second) {
            lines.push_back({hash, texture.width, texture.height, texture.format});
        }
    }

    std::set<std::tuple<u64, u32, u32, Format>> seen;
    std::vector<TextureHashLine> lines;
};

class TextureHashCheck final : public gpu_capture::CaptureCheck, public pica::Backend {
public:
    bool Start(gpu_capture::CheckContext& context, std::string*) override {
        host_.capture = &context.host;
        processor_.emplace(host_, *this);
        output_ = context.options.GetString("--tex-hashes-out",
                                            context.options.positional().front() + ".tex-hashes.txt");
        return true;
    }

    std::vector<std::string> Columns() const override {
        return {"tex_new", "tex_old"};
    }

    std::string OnRecord(std::size_t, const gpu_capture::Record& record) override {
        std::string error;
        switch (record.type) {
        case gpu_capture::kInit: {
            gpu::InitState init;
            if (gpu_capture::MakeInitState(record, init, &error)) {
                processor_->LoadInitState(init);
            }
            break;
        }
        case gpu_capture::kGx: {
            gpu::GxCommand command;
            if (gpu_capture::ParseGx(record, command, &error) &&
                (command.words[0] & 0xff) == gpu_capture::kGxSubmitCmdList) {
                const auto physical = host_.VirtualToPhysical(command.words[1]);
                if (physical) {
                    processor_->SubmitGxCmdList(*physical, command.words[2]);
                }
            }
            break;
        }
        case gpu_capture::kMmio: {
            gpu_capture::MmioData mmio;
            constexpr u32 begin = pica::kGpuMmioBase + 4 * pica::kExternalRegs;
            if (gpu_capture::ParseMmio(record, mmio, &error) && mmio.address % 4 == 0 &&
                mmio.address >= begin && mmio.address < begin + 4 * pica::kNumRegs) {
                processor_->WriteHardwareRegister((mmio.address - begin) / 4, mmio.value);
            }
            break;
        }
        default:
            break;
        }
        return error;
    }

    std::string OnFrame(const gpu_capture::FrameInfo&, gpu_capture::FrameColumns& columns) override {
        columns.Set("tex_new", std::to_string(new_hashes_.lines.size()));
        columns.Set("tex_old", std::to_string(old_hashes_.lines.size()));
        return {};
    }

    std::string Finish() override {
        std::ofstream out(output_, std::ios::binary);
        out << FormatTexHashes(new_hashes_.lines, old_hashes_.lines);
        out.close();
        if (!out) {
            return fmt::format("could not write {}", output_);
        }
        MH4N_LOG(Info, kLog, "{} textures ({} draws, {} unreadable) written to {}",
                 new_hashes_.lines.size(), draws_, unreadable_, output_);
        return {};
    }

    void Draw(const pica::DrawCall& draw) override {
        ++draws_;
        TexturesOfDraw(draw.state->regs.data(), textures_);
        for (const auto& texture : textures_) {
            const auto bytes = EncodedBytes(texture.format, texture.width, texture.height);
            const auto* memory = host_.PhysicalPointer(texture.paddr, static_cast<u32>(bytes));
            const auto hashes =
                ComputeCustomHashes(memory, bytes, texture.width, texture.height, texture.format);
            if (!hashes) {
                ++unreadable_;
                continue;
            }
            new_hashes_.Add(hashes->new_hash, texture);
            old_hashes_.Add(hashes->old_hash, texture);
        }
    }

private:
    TextureCheckHost host_;
    std::optional<pica::CommandProcessor> processor_;
    std::string output_;
    std::vector<TextureRef> textures_;
    HashList new_hashes_;
    HashList old_hashes_;
    u64 draws_ = 0;
    u64 unreadable_ = 0;
};

MH4N_OPTION("capture-player", "--tex-hashes-out", "FILE", "write the tex-hashes list to FILE");
MH4N_REGISTER(gpu_capture::CheckFactory, "tex-hashes", [](const Options&) {
    return std::make_unique<TextureHashCheck>();
});

} // namespace

std::string FormatTexHashes(const std::vector<TextureHashLine>& new_mode,
                            const std::vector<TextureHashLine>& old_mode) {
    std::string out = "# mh4n tex-hashes 1\n";
    const auto append = [&](int mode, const std::vector<TextureHashLine>& lines) {
        fmt::format_to(std::back_inserter(out), "# use_new_hash={}\n", mode);
        for (const auto& line : lines) {
            fmt::format_to(std::back_inserter(out), "{:016X} {}x{} {}\n", line.hash, line.width,
                           line.height, static_cast<u32>(line.format));
        }
    };
    append(1, new_mode);
    append(0, old_mode);
    return out;
}

} // namespace mh4n::gpu::texture
