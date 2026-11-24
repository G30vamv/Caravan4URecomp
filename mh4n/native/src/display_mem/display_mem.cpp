// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: 3dbrew GPU External Registers and LCD Registers, display_mem.h, display_mem_test.cpp and the ScaledScreens, DisplayShadow and Runtime contracts.

#include "display_mem.h"

#include <algorithm>
#include <chrono>
#include "mh4n/stall_stats.h"
#include <cstdio>
#include <ctime>
#include <string>
#include <array>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <utility>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "gpu/native/scaled_screens.h"
#if __has_include("../gpu_queue/queue_gpu.h")
#include "../gpu_queue/queue_gpu.h"
#else
#include "gpu/queue/queue_gpu.h"
#endif
#include "mh4n/log.h"
#include "mh4n/pacing_trace.h"
#include "mh4n/scheduler.h"
#if MH4N_DISPLAY_MEM_PLATFORM
#include "platform/platform_native.h"
#endif

namespace mh4n::display_mem {
namespace {

using Shadow = gpu_capture::DisplayShadow;
using Read = gpu::ScaledScreens::ScreenRead;
constexpr auto kLog = "display_mem";

// The player's marks (F8 / Share + R3): the same clock as "frame gap ... at t=" (t from the first VBlank) and the VBlank count.
std::atomic<std::int64_t> g_origin_ns{0};  // steady_clock ticks of the first VBlank (0 = none yet)
std::atomic<u64> g_vblanks{0};

constexpr std::array<u32, 2> kPdcBase{0x1EF00400, 0x1EF00500};
constexpr std::array<u32, 2> kLcdFill{0x1ED02204, 0x1ED02A04};
constexpr u32 kLeftFramebuffer = 0x68;
constexpr u32 kFormat = 0x70;
constexpr u32 kFramebufferSelect = 0x78;
constexpr u32 kStride = 0x90;
constexpr u32 kFillEnabled = 1u << 24;

DisplayMemGpu::FrameInfo SampleFrame(const RuntimeLink* link) {
    DisplayMemGpu::FrameInfo frame{};
    if (!link) return frame;
    frame.game_frame = link->game_frame.load(std::memory_order_relaxed);
    if (!link->runtime) return frame;
    frame.tick = link->runtime->Clock().Now();
#if MH4N_DISPLAY_MEM_PLATFORM
    if (const auto platform = platform::NativePlatform::Find(*link->runtime)) {
        if (const u32 rows = platform->TopScreenRows()) frame.top_columns = rows;
    }
#endif
    return frame;
}

u32 Fill(const Shadow& state, u32 screen) {
    return state.LcdReg((kLcdFill[screen] - Shadow::kLcdBase) / 4);
}

struct Framebuffer {
    u32 address;
    u32 other_address;
    u32 format;
    u32 stride;
    u32 bytes_per_pixel;
};

Framebuffer Buffer(const Shadow& state, u32 screen) {
    const auto reg = [&](u32 offset) {
        return state.GpuReg((kPdcBase[screen] + offset - Shadow::kGpuBase) / 4);
    };
    const u32 selected = reg(kFramebufferSelect) & 1u;
    const u32 format = reg(kFormat) & 7u;
    return {reg(kLeftFramebuffer + 4 * selected), reg(kLeftFramebuffer + 4 * (1 - selected)),
            format, reg(kStride), gpu_capture::BytesPerPixel(format)};
}

present::ScreenImage CpuImage(u32 width, u32 height, std::shared_ptr<std::vector<u8>> pixels) {
    present::ScreenImage image{};
    image.width = width;
    image.height = height;
    image.region = {0, 0, width, height};
    image.pixels = std::move(pixels);
    return image;
}

std::optional<std::size_t> RgbaSize(u32 width, u32 height, u32 scale) {
    const u64 w = u64{width} * scale;
    const u64 h = u64{height} * scale;
    constexpr auto max_size = std::numeric_limits<std::size_t>::max();
    if (!w || !h || w > std::numeric_limits<u32>::max() || h > std::numeric_limits<u32>::max() ||
        w > max_size / 4 / h) return std::nullopt;
    return static_cast<std::size_t>(w * h * 4);
}

void Store(u8* output, gpu_capture::Rgb color) {
    output[0] = color.r;
    output[1] = color.g;
    output[2] = color.b;
    output[3] = 255;
}

void ReadCpuScreen(const present::ScreenImage& screen, std::function<void(present::RgbImage)> done) {
    if (!screen.Valid() || !screen.IsCpu()) {
        done({});
        return;
    }
    present::RgbImage result{};
    result.width = screen.width;
    result.height = screen.height;
    const auto& pixels = *screen.pixels;
    result.rgb.resize(pixels.size() / 4 * 3);
    for (std::size_t src = 0, dst = 0; src < pixels.size(); src += 4, dst += 3) {
        std::copy_n(pixels.data() + src, 3, result.rgb.data() + dst);
    }
    done(std::move(result));
}

class QueuedDisplayTarget final : public DisplayMemGpu, public gpu::GpuDebug {
public:
    QueuedDisplayTarget(gpu::Host& host, std::unique_ptr<gpu::Gpu> inner)
        : DisplayMemGpu(host, std::move(inner)), debug_(dynamic_cast<gpu::GpuDebug*>(&Inner())) {}

    void RenderVBlank(u32 screen, const FrameInfo& frame) { PublishVBlank(screen, &frame); }

    void LoadInitState(const gpu::InitState& state) override {
        if (!debug_) return;
        debug_->LoadInitState(state);
        RefreshDisplayRegisters();
    }
    std::optional<std::vector<gpu::RegisterWrite>> TakeRegisterWrites() override {
        return debug_ ? debug_->TakeRegisterWrites() : std::nullopt;
    }
    void WriteBackColorBuffers() override {
        if (debug_) debug_->WriteBackColorBuffers();
    }
    std::vector<gpu::Counter> TakeFrameCounters() override {
        return debug_ ? debug_->TakeFrameCounters() : std::vector<gpu::Counter>{};
    }
    std::optional<u64> TakeFrameGpuTimeNs() override {
        return debug_ ? debug_->TakeFrameGpuTimeNs() : std::nullopt;
    }

private:
    gpu::GpuDebug* const debug_;
};

class QueuedDisplayGpu final : public gpu::queue::QueueGpu, public present::Display {
public:
    QueuedDisplayGpu(gpu::Host& host, const InnerMaker& make_inner)
        : QueueGpu(host, make_inner, {.lockstep = false, .max_frames_in_flight = 1, .max_frames_queued = 2}) {}

    void Attach(std::shared_ptr<RuntimeLink> link) { link_ = std::move(link); }

    u32 ReadReg(u32 addr) override {
        if ((addr & 3) != 0) return 0;
        // LCD controls other than the fill registers only change through explicit writes.
        // GX and PICA registers can also change as commands complete, so read the ordered value.
        constexpr u32 lcd_begin = 0x1ED02000;
        constexpr u32 lcd_end = lcd_begin + 0x1000;
        if (addr >= lcd_begin && addr < lcd_end && addr != kLcdFill[0] && addr != kLcdFill[1]) {
            return QueueGpu::ReadReg(addr);
        }
        u32 value = 0;
        RunSync([&] { value = ForwardTarget("ReadReg").ReadReg(addr); }, true);
        return value;
    }

    void InvalidateRegion(u32 addr, u32 size) override {
        if (size) QueueGpu::InvalidateRegion(addr, size);
    }

    void VBlank(u32 screen) override {
        if (finished_) return;
        const auto frame = screen == 0 ? SampleFrame(link_.get()) : DisplayMemGpu::FrameInfo{};
        EnqueueVBlank(screen, [screen, frame](gpu::Gpu& target) {
            static_cast<QueuedDisplayTarget&>(target).RenderVBlank(screen, frame);
        }, true);
    }

    void AddScreenSink(present::ScreenSink* sink) override {
        RunSync([&] { static_cast<QueuedDisplayTarget&>(ForwardTarget("AddScreenSink")).AddScreenSink(sink); });
    }
    void RemoveScreenSink(present::ScreenSink* sink) override {
        RunSync([&] { static_cast<QueuedDisplayTarget&>(ForwardTarget("RemoveScreenSink")).RemoveScreenSink(sink); });
    }
    void ReadScreen(const present::ScreenImage& screen,
                    std::function<void(present::RgbImage)> done) override {
        ReadCpuScreen(screen, std::move(done));
    }

    std::vector<gpu::Counter> TakeFrameCounters() override {
        auto counters = QueueGpu::TakeFrameCounters();
        const auto busy = std::find_if(counters.rbegin(), counters.rend(), [](const gpu::Counter& counter) {
            return counter.name == "translate_us";
        });
        if (busy != counters.rend() && std::none_of(counters.begin(), counters.end(), [](const gpu::Counter& counter) {
                return counter.name == "render_thread_busy_us";
            })) {
            counters.push_back({"render_thread_busy_us", busy->value});
        }
        return counters;
    }

    void Finish(Runtime& runtime) {
        finished_ = true;
        // Only the posted work, until none of it queues more render work (no timer fires: it could queue a new swap)
        do {
            WaitIdle();
        } while (runtime.Scheduler().RunPending(false));
    }

private:
    std::shared_ptr<RuntimeLink> link_;
    bool finished_ = false;
};

void AttachRuntime(Runtime& runtime) {
    auto* gpu = runtime.Interfaces().gpu;
    auto* queued = gpu::FindGpu<QueuedDisplayGpu>(gpu);
    auto* display = queued ? nullptr : gpu::FindGpu<DisplayMemGpu>(gpu);
    if (!queued && !display) return;
    auto link = std::make_shared<RuntimeLink>();
    link->runtime = &runtime;
    if (queued) {
        queued->Attach(link);
        runtime.OnFinish([&runtime] {
            if (auto* current = gpu::FindGpu<QueuedDisplayGpu>(runtime.Interfaces().gpu)) {
                current->Finish(runtime);
            }
        });
    } else {
        display->Attach(link);
    }
    runtime.OnFrame([link](u32 frame, u64) {
        link->game_frame.store(frame, std::memory_order_relaxed);
    });
}

std::unique_ptr<gpu::Gpu> MakeDisplay(std::string_view name, gpu::Host& host, const Options& options) {
    const auto* factory = Registry<gpu::GpuFactory>::Instance().Find(name);
    if (!factory) return nullptr;
    auto inner = (*factory)(host, options);
    if (!inner) return nullptr;
    return std::make_unique<DisplayMemGpu>(host, std::move(inner));
}

std::unique_ptr<gpu::Gpu> MakeQueuedDisplay(gpu::Host& host, const Options& options) {
    const auto* factory = Registry<gpu::GpuFactory>::Instance().Find("native");
    if (!factory) return nullptr;
    return std::make_unique<QueuedDisplayGpu>(host, [&](gpu::Host& queue_host) -> std::unique_ptr<gpu::Gpu> {
        auto inner = (*factory)(queue_host, options);
        if (!inner) return nullptr;
        return std::make_unique<QueuedDisplayTarget>(queue_host, std::move(inner));
    });
}

} // namespace

DisplayMemGpu::DisplayMemGpu(gpu::Host& host, std::unique_ptr<gpu::Gpu> inner)
    : host_(host), inner_(std::move(inner)), scaled_(dynamic_cast<gpu::ScaledScreens*>(inner_.get())),
      scale_(scaled_ ? std::max(1u, scaled_->ScreenScale()) : 1u) {
    RefreshDisplayRegisters();
}

void DisplayMemGpu::RefreshDisplayRegisters() {
    for (u32 screen = 0; screen < kPdcBase.size(); ++screen) {
        for (u32 offset = 0; offset < 0x100; offset += 4) {
            const u32 address = kPdcBase[screen] + offset;
            shadow_.WriteReg(address, inner_->ReadReg(address));
        }
        const u32 fill = kLcdFill[screen];
        shadow_.WriteReg(fill, inner_->ReadReg(fill));
    }
}

void DisplayMemGpu::Execute(const gpu::GxCommand& command) {
    inner_->Execute(command);
}

void DisplayMemGpu::WriteReg(u32 addr, u32 value) {
    inner_->WriteReg(addr, value);
    shadow_.WriteReg(addr, value);
}

u32 DisplayMemGpu::ReadReg(u32 addr) {
    return inner_->ReadReg(addr);
}

void DisplayMemGpu::SetBufferSwap(u32 screen, const gpu::FrameBufferInfo& info) {
    inner_->SetBufferSwap(screen, info);
    if (screen >= kPdcBase.size()) return;
    shadow_.ApplySwap(screen, info, host_.VirtualToPhysical(info.address_left).value_or(0),
                      host_.VirtualToPhysical(info.address_right).value_or(0));
    if (screen == 0) ++top_swaps_;
}

void DisplayMemGpu::SetColorFill(u32 fill) {
    inner_->SetColorFill(fill);
    shadow_.ApplyFill(fill);
}

void DisplayMemGpu::FlushRegion(u32 vaddr, u32 size) {
    inner_->FlushRegion(vaddr, size);
}

void DisplayMemGpu::InvalidateRegion(u32 vaddr, u32 size) {
    inner_->InvalidateRegion(vaddr, size);
}

void DisplayMemGpu::Attach(std::shared_ptr<RuntimeLink> link) {
    link_ = std::move(link);
}

void DisplayMemGpu::AddScreenSink(present::ScreenSink* sink) {
    if (!sink || std::find(sinks_.begin(), sinks_.end(), sink) != sinks_.end()) return;
    sinks_.push_back(sink);
    shared_wanted_ = std::all_of(sinks_.begin(), sinks_.end(),
                                 [](auto* item) { return item->WantsSharedPictures(); });
    direct_wanted_ = std::all_of(sinks_.begin(), sinks_.end(), [](auto* item) { return item->WantsBackendImages(); });
}

void DisplayMemGpu::RemoveScreenSink(present::ScreenSink* sink) {
    std::erase(sinks_, sink);
    shared_wanted_ = !sinks_.empty() && std::all_of(sinks_.begin(), sinks_.end(),
                                                   [](auto* item) { return item->WantsSharedPictures(); });
    direct_wanted_ = !sinks_.empty() && std::all_of(sinks_.begin(), sinks_.end(),
                                                   [](auto* item) { return item->WantsBackendImages(); });
}

void DisplayMemGpu::VBlank(u32 screen) {
    PublishVBlank(screen, nullptr);
}

void DisplayMemGpu::PublishVBlank(u32 screen, const FrameInfo* frame_info) {
    if (screen == 0 && scaled_) {
        const bool shared = shared_wanted_ && !present::SharedPicturesRefused();
        if (shared != shared_asked_) {
            scaled_->SetSharedScreens(shared);
            shared_asked_ = shared;
        }
        static const bool direct_allowed = [] {
            const char* v = std::getenv("MH4N_SCREEN_DIRECT");
            return v == nullptr || *v != '0';
        }();
        const bool direct = direct_allowed && direct_wanted_ && !shared && present::SharedBackend() != nullptr;
        if (direct != direct_asked_) {
            scaled_->SetDirectScreens(direct);
            direct_asked_ = direct;
        }
        const bool hidden = std::any_of(sinks_.begin(), sinks_.end(),
                                        [](auto* sink) { return sink->ScreenHidden(1); });
        if (touch_hidden_ != hidden) {
            scaled_->SetTouchScreenShown(!hidden);
            touch_hidden_ = hidden;
        }
    }

    inner_->VBlank(screen);
    if (screen != 0) return;
    ++counter_vblanks_;
    {
        // A stall (the game thread took long between two VBlanks): say what it was busy with (the first 200)
        const auto now = std::chrono::steady_clock::now();
        if (g_origin_ns.load() == 0) g_origin_ns.store(now.time_since_epoch().count());  // (the first VBlank)
        const auto origin = std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(g_origin_ns.load()));
        g_vblanks.store(counter_vblanks_);
        const double gap = std::chrono::duration<double, std::milli>(now - last_vblank_time_).count();
        const auto [fs_us, fs_bytes] = stall::TakeFsReads();
        if (last_vblank_time_.time_since_epoch().count() != 0 && gap > 30.0 && stall_logs_ < 200) {
            ++stall_logs_;
            MH4N_LOG(Info, kLog, "frame gap {:.1f} ms at t={:.2f} s, vblank {} (file reads {:.1f} ms, {} KB)", gap,
                     std::chrono::duration<double>(now - origin).count(), counter_vblanks_, fs_us / 1000.0, fs_bytes >> 10);
        }
        if (last_vblank_time_.time_since_epoch().count() != 0) {
            // Pacing of the vblanks themselves: how even the game's own beat is (the log shows it every 600)
            static double sum = 0, worst = 0;
            static u32 n = 0, over_1_5x = 0, over_2x = 0, over_12 = 0;
            static double gpu_sum = 0, gpu_worst = 0;  // the device's time a frame (timestamps), when the backend has them
            static u32 gpu_n = 0;
            // (read only in a diagnostic run, C4U_GPU_TIME=1 in the environment: the normal program does not ask the device for it)
#ifdef C4U_GPU_TIME_LOG  // (a test build, not -Release: always read)
            static const bool gpu_time_log = true;
#else
            static const bool gpu_time_log = std::getenv("C4U_GPU_TIME") != nullptr;
#endif
            if (!gpu_time_log) {
            } else if (auto* debug = dynamic_cast<gpu::GpuDebug*>(&Inner())) {
                if (const auto ns = debug->TakeFrameGpuTimeNs()) {
                    const double ms = static_cast<double>(*ns) / 1e6;
                    gpu_sum += ms;
                    gpu_worst = std::max(gpu_worst, ms);
                    ++gpu_n;
                }
            }
            static double prev = 0;
            sum += gap;
            worst = std::max(worst, gap);
            ++n;
            if (prev > 0 && gap > prev * 1.5) ++over_1_5x;
            if (prev > 0 && gap > prev * 2.0) ++over_2x;
            if (gap > 12.0) ++over_12;
            prev = gap;
            if (n == 600) {
                const double avg = sum / n;
                MH4N_LOG(Info, kLog, "vblank pacing: avg {:.2f} ms ({:.0f}/s), worst {:.1f} ms; {} jumps >1.5x the previous, {} >2x, {} over 12 ms (of 600)",
                         avg, 1000.0 / avg, worst, over_1_5x, over_2x, over_12);
                if (gpu_time_log) {
                    MH4N_LOG(Info, kLog, "GPU time a frame: avg {:.2f} ms, worst {:.2f} ms (of {} frames measured)",
                             gpu_n ? gpu_sum / gpu_n : 0.0, gpu_worst, gpu_n);
                }
                gpu_sum = gpu_worst = 0;
                gpu_n = 0;
                sum = worst = 0;
                n = over_1_5x = over_2x = over_12 = 0;
            }
        }
        last_vblank_time_ = now;
    }
    if (sinks_.empty()) return;
    const FrameInfo info = frame_info ? *frame_info : SampleFrame(link_.get());
    present::DisplayFrame frame{};
    frame.sequence = next_sequence_;
    frame.game_frame = info.game_frame;
    frame.tick = info.tick;
    // A big picture (a high internal resolution) can cost more to build than a vblank lasts, and the building runs on the
    // emulation thread: the game would never catch up. Past a few ms every second vblank is skipped (the screens stay as
    // they were published) so the game keeps its time.
    if (build_ms_ > 5.0 && (counter_vblanks_ & 1)) { ++skipped_builds_; return; }
    const auto build_start = std::chrono::steady_clock::now();
    frame.top = BuildScreen(0, info.top_columns, info.game_frame);
    if (!touch_hidden_) frame.bottom = BuildScreen(1, present::kBottomWidth, info.game_frame);
    build_ms_ = 0.9 * build_ms_ + 0.1 * std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - build_start).count();
    ++next_sequence_;
    if (counter_vblanks_ % 600 == 0) {
        MH4N_LOG(Info, kLog, "screens: building both takes {:.2f} ms on the {} thread (average); {} of {} vblanks skipped so far",
                 build_ms_, frame_info ? "renderer" : "game", skipped_builds_, counter_vblanks_);
    }

    pacing::Mark(pacing::kPublish, frame.game_frame);
    pacing::Mark(pacing::kPublishSwap, top_swaps_);
    for (auto* sink : sinks_) sink->Publish(frame);
}

std::optional<present::ScreenImage> DisplayMemGpu::BuildScreen(u32 screen, u32 columns, u32 game_frame) {
    const u32 fill = Fill(shadow_, screen);
    if (fill & kFillEnabled) {
        last_scaled_[screen] = {};
        const auto size = RgbaSize(columns, present::kTopHeight, scale_);
        if (!size) return std::nullopt;
        const gpu_capture::Rgb color{static_cast<u8>(fill), static_cast<u8>(fill >> 8),
                                     static_cast<u8>(fill >> 16)};
        auto pixels = std::make_shared<std::vector<u8>>(*size);
        for (std::size_t index = 0; index < *size; index += 4) Store(pixels->data() + index, color);
        return CpuImage(columns * scale_, present::kTopHeight * scale_, std::move(pixels));
    }
    if (scaled_ && scaled_->HasScreens()) {
        if (shared_asked_) {
            if (auto image = BuildSharedScreen(screen, columns)) return image;
        }
        if (direct_asked_) {
            if (auto image = BuildBackendScreen(screen, columns)) return image;
        }
        if (auto image = BuildScaledScreen(screen, columns)) return image;
    }
    last_scaled_[screen] = {};
    PrepareScreenMemory(screen, columns);
    return BuildMemoryScreen(screen, columns, game_frame);
}

std::optional<present::ScreenImage> DisplayMemGpu::BuildMemoryScreen(u32 screen, u32 width, u32 game_frame) {
    const auto fb = Buffer(shadow_, screen);
    const u32 height = fb.bytes_per_pixel ? fb.stride / fb.bytes_per_pixel : 0;
    const u64 length = u64{fb.stride} * width;
    const auto size = RgbaSize(width, height, scale_);
    const u8* memory = nullptr;
    if (size && fb.address && fb.stride % fb.bytes_per_pixel == 0 && length <= std::numeric_limits<u32>::max()) {
        memory = host_.PhysicalPointer(fb.address, static_cast<u32>(length));
    }
    if (!memory) {
        if (last_error_[screen].empty()) {
            MH4N_LOG(Debug, kLog, "Screen {} framebuffer unavailable at {:#010x}", screen, fb.address);
            last_error_[screen] = "framebuffer_unavailable";
        }
        return std::nullopt;
    }
    last_error_[screen].clear();
    LogMemoryScreen(screen, game_frame);

    const std::array config{fb.address, fb.stride, fb.format, width};
    const u64 key = XXH3_64bits_withSeed(memory, static_cast<std::size_t>(length),
                                         XXH3_64bits(config.data(), sizeof(config)));
    auto& last = last_screen_[screen];
    if (last.valid && last.key == key) return last.image;
    last.image = {};
    if (!last.storage || last.storage.use_count() != 1) last.storage = std::make_shared<std::vector<u8>>();
    last.storage->resize(*size);

    const u32 output_width = width * scale_;
    const u32 output_height = height * scale_;
    const std::size_t row_size = std::size_t{output_width} * 4;
    for (u32 y = 0; y < height; ++y) {
        auto* row = last.storage->data() + std::size_t{y} * scale_ * row_size;
        auto* output = row;
        const u32 source_row = height - 1 - y;
        for (u32 x = 0; x < width; ++x) {
            const auto offset = std::size_t{x} * fb.stride + source_row * fb.bytes_per_pixel;
            const auto color = gpu_capture::DecodeDisplayPixel(fb.format, memory + offset);
            for (u32 i = 0; i < scale_; ++i, output += 4) Store(output, color);
        }
        for (u32 i = 1; i < scale_; ++i) std::copy_n(row, row_size, row + i * row_size);
    }
    last.key = key;
    last.valid = true;
    last.image = CpuImage(output_width, output_height, last.storage);
    return last.image;
}

void DisplayMemGpu::LogMemoryScreen(u32 screen, u32 game_frame) {
    static const bool log = [] {
        const char* value = std::getenv("MH4N_SCALE_LOG");
        return value && std::string_view(value) == "1";
    }();
    if (!log || scale_ == 1 || memory_screen_logs_ >= 20) return;
    if (memory_screen_seen_[screen] && memory_screen_frame_[screen] == game_frame) return;
    memory_screen_seen_[screen] = true;
    memory_screen_frame_[screen] = game_frame;
    ++memory_screen_logs_;
    MH4N_LOG(Info, kLog, "Screen {} frame {} built from guest memory at {}x", screen, game_frame, scale_);
}

std::optional<present::ScreenImage> DisplayMemGpu::BuildScaledScreen(u32 screen, u32 width) {
    const auto fb = Buffer(shadow_, screen);
    if (!fb.address || !fb.bytes_per_pixel) return std::nullopt;
    const u32 height = fb.stride / fb.bytes_per_pixel;
    const auto size = RgbaSize(width, height, scale_);
    if (!size) return std::nullopt;
    auto pixels = std::make_shared<std::vector<u8>>();
    auto& last = last_scaled_[screen];
    u64 sequence = 0;
    const auto read = [&](u32 address, bool wait) {
        return scaled_->ReadScaledScreen(address, fb.format, fb.stride, width, *pixels, wait, &sequence);
    };
    Read status = read(fb.address, false);
    if (status == Read::Pending) {
        constexpr u64 max_repeat = 3;
        const bool recent = last.image && next_sequence_ - last.made <= max_repeat;
        if (fb.other_address && fb.other_address != fb.address &&
            read(fb.other_address, false) == Read::Ready && (!last.image || sequence > last.sequence)) {
            status = Read::Ready;
        } else if (recent) {
            return last.image;
        } else {
            status = read(fb.address, true);
        }
    }
    if (status != Read::Ready || pixels->size() != *size) return std::nullopt;
    last = {CpuImage(width * scale_, height * scale_, std::move(pixels)), sequence, next_sequence_};
    return last.image;
}

std::optional<present::ScreenImage> DisplayMemGpu::BuildBackendScreen(u32 screen, u32 width) {
    const auto fb = Buffer(shadow_, screen);
    if (!fb.address || !fb.bytes_per_pixel) return std::nullopt;
    const u32 height = fb.stride / fb.bytes_per_pixel;
    if (!RgbaSize(width, height, scale_)) return std::nullopt;
    gpu::ScaledScreens::BackendScreen result;
    if (scaled_->ReadBackendScreen(fb.address, fb.format, fb.stride, width, result) != Read::Ready) return std::nullopt;
    if (result.width != width * scale_ || result.height != height * scale_) return std::nullopt;
    present::ScreenImage image{};
    image.image = std::move(result.image);
    image.region = result.region;
    image.scale = 1;
    image.orientation = result.orientation;
    image.timeline_value = result.timeline_value;
    image.width = result.width;
    image.height = result.height;
    if (!image.Valid()) return std::nullopt;
    last_scaled_[screen] = {};
    return image;
}

std::optional<present::ScreenImage> DisplayMemGpu::BuildSharedScreen(u32 screen, u32 width) {
    const auto fb = Buffer(shadow_, screen);
    if (!fb.address || !fb.bytes_per_pixel) return std::nullopt;
    const u32 height = fb.stride / fb.bytes_per_pixel;
    if (!RgbaSize(width, height, scale_)) return std::nullopt;
    gpu::ScaledScreens::SharedScreen result;
    if (scaled_->ReadSharedScreen(fb.address, fb.format, fb.stride, width, result) != Read::Ready) {
        return std::nullopt;
    }
    present::ScreenImage image{};
    image.width = width * scale_;
    image.height = height * scale_;
    image.region = result.region;
    image.shared = std::move(result.picture);
    if (!image.Valid()) return std::nullopt;
    return image;
}

void DisplayMemGpu::PrepareScreenMemory(u32 screen, u32 columns) {
    if (!scaled_ || !scaled_->HasScreens()) return;
    const auto fb = Buffer(shadow_, screen);
    const u64 size = u64{fb.stride} * columns;
    if (fb.address && size && size <= std::numeric_limits<u32>::max()) {
        scaled_->PrepareMemory(fb.address, static_cast<u32>(size));
    }
}

void DisplayMemGpu::ReadScreen(const present::ScreenImage& screen,
                               std::function<void(present::RgbImage)> done) {
    ReadCpuScreen(screen, std::move(done));
}

MH4N_REGISTER(BootStep, "display_mem.attach", BootStep{Phase::Services, AttachRuntime});
MH4N_REGISTER(present::DisplayGpu, "null-display", {});
MH4N_REGISTER(present::DisplayGpu, "native-display", {});
MH4N_REGISTER(present::DisplayGpu, "native-display-queue", {});
MH4N_REGISTER(gpu::GpuFactory, "null-display",
              [](gpu::Host& host, const Options& options) { return MakeDisplay("null", host, options); });
MH4N_REGISTER(gpu::GpuFactory, "native-display",
              [](gpu::Host& host, const Options& options) {
                  return MakeDisplay("native", host, options);
              });
MH4N_REGISTER(gpu::GpuFactory, "native-display-queue", MakeQueuedDisplay);

// MODIFIED for the Windows port: a moment the player marks as it happens, so a log line can be found by its time.
std::string Mark(const char* how) {
    static std::atomic<u32> n{0};
    const auto now = std::chrono::steady_clock::now();
    const std::int64_t o = g_origin_ns.load();
    const double t = o == 0 ? 0.0 : std::chrono::duration<double>(now - std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(o))).count();
    const std::time_t wall = std::time(nullptr);
    char clock[16] = {};
    std::strftime(clock, sizeof clock, "%H:%M:%S", std::localtime(&wall));
    const u32 number = ++n;
    MH4N_LOG(Info, kLog, "[mark] #{} ({}) at {} (clock), t={:.2f} s, vblank {}", number, how, clock, t, g_vblanks.load());
    return "mark #" + std::to_string(number) + " at " + clock;
}

} // namespace mh4n::display_mem
