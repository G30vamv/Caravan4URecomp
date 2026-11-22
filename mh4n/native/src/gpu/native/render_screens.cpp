// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: scaled_screens.h, present.h, gpu_gx_test and the display memory and capture player tests.
#include "renderer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <array>
#include <cstring>

#include "mh4n/log.h"

namespace mh4n::gpu::native {
namespace {

using F = rhi::Format;

u8 Expand(u32 bits, u32 count) {
    if (count == 8) return static_cast<u8>(bits);
    return static_cast<u8>((bits << (8 - count)) | (bits >> (2 * count - 8)));
}

std::array<u8, 3> ReadRgb(const u8* pixel, F format) {
    if (format == F::Rgba8Unorm) return {pixel[0], pixel[1], pixel[2]};
    const u32 word = pixel[0] | (u32{pixel[1]} << 8);
    switch (format) {
    case F::R5G6B5: return {Expand(word >> 11, 5), Expand((word >> 5) & 63, 6), Expand(word & 31, 5)};
    case F::B5G6R5: return {Expand(word & 31, 5), Expand((word >> 5) & 63, 6), Expand(word >> 11, 5)};
    case F::R5G5B5A1: return {Expand(word >> 11, 5), Expand((word >> 6) & 31, 5), Expand((word >> 1) & 31, 5)};
    case F::B5G5R5A1: return {Expand((word >> 1) & 31, 5), Expand((word >> 6) & 31, 5), Expand(word >> 11, 5)};
    case F::A1R5G5B5: return {Expand((word >> 10) & 31, 5), Expand((word >> 5) & 31, 5), Expand(word & 31, 5)};
    case F::R4G4B4A4: return {Expand(word >> 12, 4), Expand((word >> 8) & 15, 4), Expand((word >> 4) & 15, 4)};
    case F::B4G4R4A4: return {Expand((word >> 4) & 15, 4), Expand((word >> 8) & 15, 4), Expand(word >> 12, 4)};
    default: return {};
    }
}

std::array<u32, 3> ComponentDepth(u32 format) {
    constexpr std::array<std::array<u32, 3>, 5> depth{{{8, 8, 8}, {8, 8, 8}, {5, 6, 5}, {5, 5, 5}, {4, 4, 4}}};
    return depth.at(format);
}

// A few threads that stay (the scaled pictures are converted at every vblank: threads made and joined each time cost more)
class TaskPool {
public:
    static TaskPool& Get() {
        static TaskPool pool;
        return pool;
    }
    u32 Size() const { return static_cast<u32>(threads_.size()); }
    void Submit(std::function<void()> task) {
        {
            std::lock_guard lock(mutex_);
            queue_.push_back(std::move(task));
        }
        wake_.notify_one();
    }
    ~TaskPool() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        for (auto& t : threads_) t.join();
    }

private:
    TaskPool() {
        const u32 n = std::clamp(std::thread::hardware_concurrency() / 2, 2u, 4u);
        for (u32 i = 0; i < n; ++i) threads_.emplace_back([this] { Run(); });
    }
    void Run() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
                if (stop_) return;
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            task();
        }
    }
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> queue_;
    std::vector<std::thread> threads_;
    bool stop_ = false;
};

// A display readback buffer is millions of bytes and one is wanted for every screen of every frame: the ones no screen
// holds any more (the pool is the last owner) and the GPU is through with are used again, not made again.
std::shared_ptr<rhi::Buffer> MakeReadback(rhi::Backend& backend, const rhi::Image& image,
                                          std::vector<std::shared_ptr<rhi::Buffer>>& pool) {
    const u32 bpp = image.GetFormat() == F::Rgba8Unorm ? 4 : 2;
    const u64 size = u64{image.Width()} * image.Height() * bpp;
    for (auto& kept : pool) {
        if (kept.use_count() == 1 && kept->Size() == size) return kept;
    }
    auto buffer = backend.CreateBuffer({size, rhi::Heap::Readback, rhi::BufferUsage::Transfer, "PICA display readback"});
    if (!buffer) return nullptr;
    std::shared_ptr<rhi::Buffer> made = std::move(*buffer);
    if (pool.size() < 16) pool.push_back(made);
    return made;
}

void CopyScreenToBuffer(rhi::Recorder& recorder, const rhi::Image& image, rhi::Buffer& buffer, bool keep_general = false) {
    const rhi::BufferImageRegion region{.image_rect = {0, 0, image.Width(), image.Height()}};
    recorder.CopyImageToBuffer(image, buffer, std::span{&region, 1}, keep_general);
}

class TransferMemory final : public Host {
public:
    TransferMemory(Host& host, u32 address, std::span<u8> source)
        : host_(host), address_(address), source_(source) {}

    u8* PhysicalPointer(u32 address, u32 size) override {
        if (address >= address_ && u64{address - address_} + size <= source_.size()) {
            return source_.data() + (address - address_);
        }
        return host_.PhysicalPointer(address, size);
    }
    std::optional<u32> VirtualToPhysical(u32 address) override { return host_.VirtualToPhysical(address); }
    void SignalInterrupt(Interrupt id, u64 delay_ns) override { host_.SignalInterrupt(id, delay_ns); }
    void Post(std::function<void()> work) override { host_.Post(std::move(work)); }

private:
    Host& host_;
    u32 address_;
    std::span<u8> source_;
};

} // namespace

void Renderer::PrepareMemory(MemoryRange range) {
    SubmitDraws();
    MaterializeScreens(range);
    surfaces_.WriteBack(range);
}

void Renderer::PrepareDrawMemory(MemoryRange range) {
    const auto submit = [this] { SubmitDraws(); };
    MaterializeScreens(range, false, submit);
    surfaces_.WriteBack(range, nullptr, false, submit);
}

bool Renderer::Materialize(Screen& screen, MemoryRange preserve) {
    gx::TransferRegs transfer = *screen.owed;
    const auto range = screen.range;
    auto* dest = host_.PhysicalPointer(range.address, static_cast<u32>(range.size));
    if (!dest) return false;

    const auto bpp = gx::BytesPerPixel(transfer.InputFormat());
    std::vector<u8> source(size_t{screen.row_pixels} * screen.rows * bpp);
    auto read = backend_->ReadbackToGuest(*screen.image, {0, 0, screen.row_pixels, screen.rows},
                                          screen.source.key.format, source, scale_);
    if (!read) {
        MH4N_LOG(Warning, "NativeGpu", "display transfer readback: {}", read.error());
        return false;
    }

    std::vector<u8> saved;
    u64 offset = 0;
    if (range.Overlaps(preserve)) {
        offset = std::max(preserve.address, range.address) - range.address;
        const u64 size = std::min(preserve.End(), range.End()) - (range.address + offset);
        saved.assign(dest + offset, dest + offset + size);
    }
    transfer.input_size = screen.row_pixels | (screen.rows << 16);
    TransferMemory replay(host_, transfer.InputAddress(), source);
    gx::DisplayTransfer(replay, transfer);
    if (!saved.empty()) std::memcpy(dest + offset, saved.data(), saved.size());
    screen.owed.reset();
    return true;
}

void Renderer::MaterializeScreens(MemoryRange range, bool preserve_guest, const std::function<void()>& before_read) {
    for (auto& [address, screen] : screens_) {
        if (!screen->owed || !screen->range.Overlaps(range)) continue;
        if (before_read) before_read();
        Materialize(*screen, preserve_guest ? range : MemoryRange{});
    }
}

void Renderer::InvalidateScreens(MemoryRange range) {
    for (auto it = screens_.begin(); it != screens_.end();) {
        if (it->second->range.Overlaps(range)) {
            it = screens_.erase(it);
        } else {
            ++it;
        }
    }
}

bool Renderer::CaptureTransfer(const gx::TransferRegs& transfer, MemoryRange output) {
    const MemoryRange input{transfer.InputAddress(), u64{transfer.InputWidth()} * transfer.InputHeight() *
                                                        gx::BytesPerPixel(transfer.InputFormat())};
    const auto prepare_output = [&](bool overwrite) {
        if (!output.size) return;
        if (overwrite) {
            for (auto& [address, screen] : screens_) {
                if (output.address <= address && output.End() >= screen->range.End()) screen->owed.reset();
            }
        }
        PrepareMemory(output);
        NotifyWrite(output);
    };
    const auto use_cpu = [&] {
        if (input.size) PrepareMemory(input);
        prepare_output(false);
        return false;
    };
    if (scale_ == 1 && !options_.async_screens && !shared_screens_ && !direct_screens_ && !display_buffers_[0].selected &&
        !display_buffers_[1].selected) return use_cpu();
    const u32 width = transfer.OutputWidth(), rows = transfer.OutputHeight();
    const u32 bpp = gx::BytesPerPixel(transfer.OutputFormat());
    if (transfer.IsTextureCopy() || transfer.InputLinear() || transfer.DontSwizzle() ||
        transfer.Scaling() || (transfer.flags & (1u << 16)) || !bpp || !width || !rows ||
        width > transfer.InputWidth() || rows > transfer.InputHeight() ||
        (transfer.FlipVertically() && transfer.CropInputLines() && width != transfer.InputWidth())) {
        return use_cpu();
    }
    if (!output.address || !output.size || input.Overlaps(output)) return use_cpu();
    SubmitDraws();
    MaterializeScreens(input);
    u32 first_row = 0;
    Surface* surface = surfaces_.FindTransferSource(transfer, first_row);
    if (!surface) return use_cpu();
    rhi::ImageDesc desc{};
    desc.width = width * scale_;
    desc.height = rows * scale_;
    desc.format = surface->image->GetFormat();
    desc.name = "PICA display transfer";
    auto copy = backend_->CreateImage(desc);
    if (!copy) return use_cpu();
    auto readback = (shared_screens_ || direct_screens_) ? nullptr : MakeReadback(*backend_, **copy, readback_pool_);
    auto recorder = backend_->BeginRecording();
    rhi::ImageCopyRegion region{};
    region.src_y = surface->image->Height() - (first_row + rows) * scale_;
    region.width = desc.width;
    region.height = desc.height;
    recorder.CopyImage(*surface->image, **copy, std::span{&region, 1});
    if (readback) CopyScreenToBuffer(recorder, **copy, *readback);
    last_submission_ = backend_->Submit(std::move(recorder));
    backend_->Retire(*copy, last_submission_);
    if (readback) backend_->Retire(readback, last_submission_);
    auto screen = std::make_shared<Screen>();
    screen->range = output;
    screen->format = transfer.OutputFormat();
    screen->row_pixels = width;
    screen->rows = rows;
    screen->flip = transfer.FlipVertically();
    screen->sequence = ++screen_sequence_;
    screen->ready = last_submission_;
    screen->image = std::move(*copy);
    screen->source = {surface->key, first_row, width, rows};
    screen->readback = std::move(readback);
    const bool defer = (options_.async_screens || shared_screens_ || direct_screens_) && width % 8 == 0 && rows % 8 == 0 &&
        output.size <= UINT32_MAX && host_.PhysicalPointer(output.address, static_cast<u32>(output.size)) &&
        input.size <= UINT32_MAX && host_.PhysicalPointer(input.address, static_cast<u32>(input.size));
    if (defer) {
        prepare_output(true);
        screen->owed = transfer;
    } else {
        use_cpu();
    }
    screens_.emplace(output.address, std::move(screen));
    return defer;
}

void Renderer::LearnDisplayTargets(const std::array<DisplayBuffers, 2>& buffers) {
    const auto target = [&](u32 address, const DisplayBuffers& buffer) -> std::optional<TransferTarget> {
        const auto it = screens_.find(address);
        if (it == screens_.end()) return std::nullopt;
        const Screen& screen = *it->second;
        if (screen.format != buffer.format || screen.row_pixels * gx::BytesPerPixel(screen.format) != buffer.stride) {
            return std::nullopt;
        }
        return screen.source;
    };
    const auto& top = buffers[0];
    const auto& touch = buffers[1];
    top_targets_ = {target(top.selected, top), target(top.alternate, top),
                    target(top.right[0], top), target(top.right[1], top)};
    touch_target_ = target(touch.selected, touch);
    if (!touch_target_) touch_target_ = target(touch.alternate, touch);
    if (touch_target_ && touch_target_->rows != present::kBottomWidth) touch_target_.reset();
}

bool Renderer::SkipTouchDraw(const SurfaceKey& key, const rhi::Rect& area) const {
    if (!touch_target_ || key != touch_target_->key || (!top_targets_[0] && !top_targets_[1])) return false;
    const auto& touch = *touch_target_;
    const u32 start_y = (key.StorageHeight() - touch.first_row - touch.rows) * scale_;
    const u32 x = static_cast<u32>(area.x), y = static_cast<u32>(area.y);
    if (x + area.width > touch.width * scale_ || y < start_y || y + area.height > start_y + touch.rows * scale_) {
        return false;
    }
    return std::ranges::none_of(top_targets_, [&](const auto& top) {
        return top && top->key.Range().Overlaps(key.Range());
    });
}

bool Renderer::RecordScreenReadback(Screen& screen) {
    if (screen.readback) return true;
    auto buffer = MakeReadback(*backend_, *screen.image, readback_pool_);
    if (!buffer) return false;
    SubmitDraws();
    auto recorder = backend_->BeginRecording();
    CopyScreenToBuffer(recorder, *screen.image, *buffer, screen.published_direct);
    last_submission_ = backend_->Submit(std::move(recorder));
    screen.ready = last_submission_;
    backend_->Retire(screen.image, screen.ready);
    backend_->Retire(buffer, screen.ready);
    screen.readback = std::move(buffer);
    return true;
}

void Renderer::EndDisplayFrame(const std::array<DisplayBuffers, 2>& buffers, bool touch_shown) {
    display_buffers_ = buffers;
    LearnDisplayTargets(buffers);
    if (pending_draw_) {
        for (auto& [address, screen] : screens_) {
            if (screen->sequence > last_vblank_sequence_) screen->complete = false;
        }
        if (screen_sequence_ != last_vblank_sequence_) pending_draw_ = false;
        holding_screens_ = true;
    } else {
        std::array<std::shared_ptr<Screen>, 2> current{};
        bool complete = true;
        for (size_t i = 0; i < buffers.size(); ++i) {
            const auto& buffer = buffers[i];
            if (buffer.fill || (i == 1 && !touch_shown)) continue;
            const auto it = screens_.find(buffer.selected);
            if (it != screens_.end()) {
                current[i] = it->second;
                complete &= current[i]->complete;
            } else if (holding_screens_ && complete_screens_[i] && buffer.selected) {
                complete = false;
            }
        }
        holding_screens_ = !complete;
        if (complete) complete_screens_ = std::move(current);
    }
    last_vblank_sequence_ = screen_sequence_;
}

Renderer::Screen* Renderer::FindScreen(u32 address, u32 format, u32 stride, u32 rows) {
    const auto matches = [&](const Screen& screen) {
        return screen.complete && screen.format == format && screen.rows == rows &&
               screen.row_pixels * gx::BytesPerPixel(format) == stride;
    };
    if (holding_screens_) {
        for (size_t i = 0; i < display_buffers_.size(); ++i) {
            if (display_buffers_[i].Contains(address)) {
                const auto& last = complete_screens_[i];
                return last && matches(*last) ? last.get() : nullptr;
            }
        }
    }
    const auto it = screens_.find(address);
    return it != screens_.end() && matches(*it->second) ? it->second.get() : nullptr;
}

ScaledScreens::ScreenRead Renderer::ReadSharedScreen(u32 address, u32 format, u32 stride, u32 rows,
                                                      ScaledScreens::SharedScreen& out) {
    using Read = ScaledScreens::ScreenRead;
    if (!shared_screens_ || present::SharedPicturesRefused()) return Read::None;
    auto* screen = FindScreen(address, format, stride, rows);
    if (!screen) return Read::None;
    if (!screen->picture) {
        SubmitDraws();
        screen->picture = screen_pictures_.Make(screen->image, format, screen->flip);
        if (screen->picture) last_submission_ = backend_->LastSubmittedValue();
    }
    if (!screen->picture) return Read::None;
    out = {screen->picture, {0, 0, rows * scale_, screen->row_pixels * scale_}, screen->sequence};
    return Read::Ready;
}

// One screen conversion: the GPU raw pixels (the picture columns as rows, as the transfer wrote them) to upright RGBA8.
struct Renderer::ScreenJob : std::enable_shared_from_this<Renderer::ScreenJob> {
    const u8* raw = nullptr;  // the readback buffer memory (held by `hold`, which the game thread releases)
    u32 width = 0, height = 0, bpp = 0, format = 0;
    F storage = F::Rgba8Unorm;
    bool flip = false;
    u64 sequence = 0;
    std::shared_ptr<rhi::Buffer> hold;
    std::vector<u8> out;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::atomic<u32> remaining{0};
    std::mutex mutex;
    std::condition_variable done_cv;
    std::array<std::array<u8, 256>, 3> table{};
    bool plain = false;

    void Convert(u32 first_tile_row, u32 last_tile_row) {
        constexpr u32 kTile = 32;
        for (u32 ty = first_tile_row * kTile; ty < std::min(last_tile_row * kTile, height); ty += kTile) {
            const u32 y_end = std::min(ty + kTile, height);
            for (u32 tx = 0; tx < width; tx += kTile) {
                const u32 x_end = std::min(tx + kTile, width);
                for (u32 y = ty; y < y_end; ++y) {
                    const u32 source_x = height - 1 - y;
                    u8* o = out.data() + (size_t{y} * width + tx) * 4;
                    for (u32 x = tx; x < x_end; ++x, o += 4) {
                        const u32 source_y = flip ? x : width - 1 - x;
                        const u8* in = raw + (size_t{source_y} * height + source_x) * bpp;
                        if (plain) {
                            o[0] = in[0];
                            o[1] = in[1];
                            o[2] = in[2];
                        } else {
                            const auto color = ReadRgb(in, storage);
                            o[0] = table[0][color[0]];
                            o[1] = table[1][color[1]];
                            o[2] = table[2][color[2]];
                        }
                        o[3] = 255;
                    }
                }
            }
        }
    }
    void Start() {
        const auto bits = ComponentDepth(format);
        for (u32 i = 0; i < 3; ++i) {
            for (u32 v = 0; v < 256; ++v) table[i][v] = Expand(v >> (8 - bits[i]), bits[i]);
        }
        plain = storage == F::Rgba8Unorm && bits[0] == 8 && bits[1] == 8 && bits[2] == 8;
        out.resize(size_t{width} * height * 4);
        constexpr u32 kTile = 32;
        const u32 tile_rows = (height + kTile - 1) / kTile;
        auto& pool = TaskPool::Get();
        const u32 parts = size_t{width} * height >= (1u << 18) ? std::min(pool.Size(), tile_rows) : 1u;
        remaining.store(parts);
        for (u32 p = 0; p < parts; ++p) {
            pool.Submit([self = shared_from_this(), first = tile_rows * p / parts, last = tile_rows * (p + 1) / parts] {
                self->Convert(first, last);
                if (self->remaining.fetch_sub(1) == 1) {
                    std::lock_guard lock(self->mutex);
                    self->done_cv.notify_all();
                }
            });
        }
    }
    void Wait() {
        std::unique_lock lock(mutex);
        done_cv.wait(lock, [this] { return remaining.load() == 0; });
    }
};

ScaledScreens::ScreenRead Renderer::ReadBackendScreen(u32 address, u32 format, u32 stride, u32 rows,
                                                        ScaledScreens::BackendScreen& out) {
    using Read = ScaledScreens::ScreenRead;
    if (!direct_screens_) return Read::None;
    auto* screen = FindScreen(address, format, stride, rows);
    if (!screen || !screen->image) return Read::None;
    // The display cuts the colours to the framebuffer's depth (RGB565 and the like); the direct picture is the target's own
    // colours, up to 4/255 smoother. MH4U draws into such targets, so refusing them (WP89 M2) sent every frame back through
    // the CPU: 3.4 ms a frame at 8x and half the frames never shown. MH4N_SCREEN_DIRECT_STRICT=1 brings the refusal back.
    static const bool strict = [] {
        const char* v = std::getenv("MH4N_SCREEN_DIRECT_STRICT");
        return v != nullptr && *v == '1';
    }();
    if (strict && (screen->image->GetFormat() != F::Rgba8Unorm || (format != 0 && format != 1))) return Read::None;
    SubmitDraws();
    if (screen->ready > backend_->LastSubmittedValue()) return Read::None;
    screen->published_direct = true;
    out.image = screen->image;
    out.width = screen->image->Height();   // the upright picture: framebuffer rows across
    out.height = screen->image->Width();
    out.region = {0, 0, screen->image->Width(), screen->image->Height()};
    out.orientation = screen->flip ? present::Orientation::Rotated90 : present::Orientation::Rotated90Mirrored;
    out.timeline_value = screen->ready;
    out.sequence = screen->sequence;
    return Read::Ready;
}

ScaledScreens::ScreenRead Renderer::ReadScreen(u32 address, u32 format, u32 stride, u32 rows, std::vector<u8>& rgba,
                                                bool wait, u64* sequence) {
    using Read = ScaledScreens::ScreenRead;
    auto* candidate = FindScreen(address, format, stride, rows);
    if (!candidate || !RecordScreenReadback(*candidate)) return Read::None;
    const Screen& screen = *candidate;
    if (!wait && backend_->CompletedValue() < screen.ready) return Read::Pending;
    if (wait) {
        u8 one = 0;
        if (!backend_->ReadBuffer(*screen.readback, 0, std::span<u8>{&one, 1}, screen.ready)) return Read::None;
    }
    const auto start_job = [&] {
        auto job = std::make_shared<ScreenJob>();
        job->height = screen.image->Width();
        job->width = screen.image->Height();
        job->storage = screen.image->GetFormat();
        job->bpp = job->storage == F::Rgba8Unorm ? 4 : 2;
        job->format = format;
        job->flip = screen.flip;
        job->sequence = screen.sequence;
        job->hold = screen.readback;
        screen.readback->Invalidate(0, u64{job->width} * job->height * job->bpp);
        job->raw = screen.readback->Mapped().data();
        job->Start();
        return job;
    };
    static const bool pipelined = [] {
        const char* v = std::getenv("MH4N_SCREEN_PIPELINE");
        return v == nullptr || *v != '0';
    }();
    // At a high internal scale the conversion is milliseconds a vblank: it runs on worker threads and the game thread
    // takes the previous vblank picture (one vblank old), so it never waits for it.
    const u64 slot_key = (u64{stride} << 36) | (u64{rows} << 4) | (format & 0xF);
    if (!pipelined || wait || scale_ < 3) {
        auto job = start_job();
        job->Wait();
        rgba = std::move(job->out);
        if (sequence) *sequence = job->sequence;
        return Read::Ready;
    }
    {
        // (the picture behind is not finished yet: the caller shows the last one again rather than wait for it)
        const auto behind = screen_jobs_.find(slot_key);
        if (behind != screen_jobs_.end() && behind->second && behind->second->remaining.load() != 0 &&
            std::chrono::steady_clock::now() - behind->second->started < std::chrono::milliseconds(60)) {
            return Read::Pending;
        }
    }
    std::shared_ptr<ScreenJob> previous = std::move(screen_jobs_[slot_key]);
    if (previous && std::chrono::steady_clock::now() - previous->started > std::chrono::milliseconds(60)) previous.reset();
    auto current = start_job();
    if (!previous) {
        // (no picture behind yet, or a stale one: this one is waited for, and one more is started to be the next)
        current->Wait();
        rgba = std::move(current->out);
        if (sequence) *sequence = current->sequence;
        screen_jobs_[slot_key] = start_job();
        return Read::Ready;
    }
    screen_jobs_[slot_key] = current;
    previous->Wait();
    rgba = std::move(previous->out);
    if (sequence) *sequence = previous->sequence;
    return Read::Ready;
}

} // namespace mh4n::gpu::native
