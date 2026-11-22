// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: present.h, scaled_screens.h, shared_images.h and the display memory and present tests.
#include "renderer.h"
#include "screen_picture.h"

#include <algorithm>
#include <atomic>
#include <mutex>

#include "gpu/fragment/generator.h"
#include "gpu/shader/vertex_shader.h"
#include "mh4n/log.h"

namespace mh4n::gpu::native {
namespace {

constexpr auto kLog = "NativeGpu";
std::atomic<u64> next_image_id{1};

const char* kVertex = R"glsl(#version 450
layout(location = 0) flat out uint flags;
void main() {
    vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
    flags = uint(gl_InstanceIndex);
}
)glsl";

const char* kFragment = R"glsl(#version 450
#extension GL_EXT_samplerless_texture_functions : require
layout(set = 0, binding = 0) uniform texture2D source;
layout(location = 0) flat in uint flags;
layout(location = 0) out vec4 out_color;
uint expand8(uint value, uint bits) {
    return (value << (8u - bits)) | (value >> (2u * bits - 8u));
}
void main() {
    ivec2 size = textureSize(source, 0);
    ivec2 dst = ivec2(gl_FragCoord.xy);
    int row = (flags & 1u) != 0u ? dst.x : size.y - 1 - dst.x;
    vec3 rgb = texelFetch(source, ivec2(size.x - 1 - dst.y, row), 0).rgb;
    if (flags > 1u) {
        for (int channel = 0; channel < 3; ++channel) {
            uint input_bits = 8u - ((flags >> (1 + channel * 3)) & 7u);
            uint output_bits = 8u - ((flags >> (10 + channel * 3)) & 7u);
            uint packed = uint(round(rgb[channel] * float((1u << input_bits) - 1u)));
            uint value = expand8(packed, input_bits);
            rgb[channel] = float(expand8(value >> (8u - output_bits), output_bits)) / 255.0;
        }
    }
    out_color = vec4(rgb, 1.0);
}
)glsl";

using Bits = std::array<u32, 3>;

Bits InputBits(rhi::Format format) {
    using F = rhi::Format;
    switch (format) {
    case F::R5G6B5:
    case F::B5G6R5: return {5, 6, 5};
    case F::R5G5B5A1:
    case F::B5G5R5A1:
    case F::A1R5G5B5: return {5, 5, 5};
    case F::R4G4B4A4:
    case F::B4G4R4A4: return {4, 4, 4};
    default: return {8, 8, 8};
    }
}

u32 PictureFlags(rhi::Format storage, u32 format, bool flip) {
    constexpr std::array<Bits, 5> output_bits{{{8, 8, 8}, {8, 8, 8}, {5, 6, 5}, {5, 5, 5}, {4, 4, 4}}};
    const auto input = InputBits(storage);
    u32 flags = flip;
    for (u32 i = 0; i < input.size(); ++i) {
        flags |= (8 - input[i]) << (1 + 3 * i);
        flags |= (8 - output_bits[format][i]) << (10 + 3 * i);
    }
    return flags;
}

rhi::BindingSetDesc PictureBindings() {
    return {{{0, rhi::BindingType::SampledImage}}};
}

class Picture final : public present::SharedPicture {
public:
    Picture(std::shared_ptr<rhi::Backend> backend, std::shared_ptr<rhi::Image> image,
            std::shared_ptr<rhi::SharedImage> shared, std::shared_ptr<rhi::SyncFile> fence, rhi::TimelineValue ready)
        : backend_(std::move(backend)), image_(std::move(image)), shared_(std::move(shared)),
          fence_(std::move(fence)), ready_(ready),
          id_(shared_ ? shared_->Id() : next_image_id.fetch_add(1, std::memory_order_relaxed)),
          width_(shared_ ? shared_->Width() : image_->Width()), height_(shared_ ? shared_->Height() : image_->Height()),
          producer_(std::this_thread::get_id()) {}

    void* HardwareBuffer() const override { return shared_ ? shared_->BufferRef().get() : nullptr; }
    u64 BufferId() const override { return id_; }
    u32 BufferWidth() const override { return width_; }
    u32 BufferHeight() const override { return height_; }
    bool Ready() const override { return backend_->CompletedValue() >= ready_; }
    int TakeSyncFd() const override { return fence_ && !fence_->Signaled() ? fence_->Dup() : -1; }
    bool WaitReady(u64 timeout_ns) const override {
        return rhi::SharedImages::WaitFor(*backend_, ready_, timeout_ns);
    }

    bool HandTo(const rhi::Backend& backend, std::thread::id reader) const {
        if (&backend != backend_.get() || !image_ || reader == std::thread::id{}) return false;
        std::lock_guard lock(reader_mutex_);
        if (reader_ == std::thread::id{}) {
            if (std::this_thread::get_id() != producer_) return false;
            if (reader != producer_) rhi::HandOver(*image_, reader, {ready_, rhi::Stage::FragmentShader});
            reader_ = reader;
        }
        return reader_ == reader;
    }

    const rhi::Image* Image() const { return image_.get(); }
    rhi::TimelineValue Timeline() const { return ready_; }

private:
    std::shared_ptr<rhi::Backend> backend_;
    std::shared_ptr<rhi::Image> image_;
    std::shared_ptr<rhi::SharedImage> shared_;
    std::shared_ptr<rhi::SyncFile> fence_;
    rhi::TimelineValue ready_;
    u64 id_;
    u32 width_, height_;
    std::thread::id producer_;
    mutable std::mutex reader_mutex_;
    mutable std::thread::id reader_;
};

} // namespace

std::optional<present::ScreenImage> VulkanScreenImage(const present::ScreenImage& screen, const rhi::Backend& backend,
                                                     std::thread::id reader) {
    if (!screen.IsShared() || !screen.Valid()) return std::nullopt;
    const auto* picture = dynamic_cast<const Picture*>(screen.shared.get());
    if (!picture || !picture->HandTo(backend, reader)) return std::nullopt;
    present::ScreenImage image = screen;
    image.image = std::shared_ptr<const void>{screen.shared, picture->Image()};
    image.shared.reset();
    image.timeline_value = picture->Timeline();
    return image;
}

bool ScreenPictures::Init() {
    if (initialized_) return pipeline_ != nullptr;
    initialized_ = true;
#if defined(__ANDROID__)
    shared_ = rhi::SharedImages::Create(backend_, nullptr);
    if (!shared_) return false;
#endif
    std::vector<u32> vertex, fragment_code;
    std::string error;
    if (!shader::CompileGlslToSpirv(kVertex, vertex, error) ||
        !fragment::CompileFragmentGlslToSpirv(kFragment, fragment_code, error)) {
        MH4N_LOG(Warning, kLog, "Display shader: {}", error);
        return false;
    }
    rhi::GraphicsPipelineDesc desc{};
    desc.vertex.spirv = vertex;
    desc.fragment.spirv = fragment_code;
    desc.sets = {PictureBindings()};
    desc.color_format = rhi::Format::Rgba8Unorm;
    desc.name = "PICA upright screen";
    auto pipeline = backend_->CreateGraphicsPipeline(desc);
    if (!pipeline) {
        MH4N_LOG(Warning, kLog, "Display pipeline: {}", pipeline.error());
        return false;
    }
    pipeline_ = std::move(*pipeline);
    bindings_ = std::make_unique<rhi::BindingArena>(backend_);
    return true;
}

std::shared_ptr<const present::SharedPicture> ScreenPictures::Make(const std::shared_ptr<rhi::Image>& source,
                                                                   u32 format, bool flip) {
    if (!Init()) return {};
    const u32 width = source->Height(), height = source->Width();
    std::shared_ptr<rhi::SharedImage> external;
    std::shared_ptr<rhi::Image> local;
    if (shared_) {
        const auto spare = std::ranges::find_if(shared_pool_, [&](const auto& image) {
            return image.use_count() == 1 && image->Width() == width && image->Height() == height;
        });
        if (spare != shared_pool_.end()) {
            external = *spare;
        } else {
            auto image = shared_->Make(width, height);
            if (!image) return {};
            external = std::move(*image);
            if (shared_pool_.size() < 8) shared_pool_.push_back(external);
        }
    } else {
        auto image = backend_->CreateImage({.format = rhi::Format::Rgba8Unorm, .width = width, .height = height,
                                            .name = "PICA upright screen"});
        if (!image) return {};
        local = std::move(*image);
    }
    rhi::Image& target = external ? external->GetImage() : *local;
    auto recorder = backend_->BeginRecording();
    auto set = bindings_->Acquire(PictureBindings());
    if (!set) {
        MH4N_LOG(Warning, kLog, "Display bindings: {}", set.error());
        return {};
    }
    const auto sampled = (*set)->SetSampledImage(0, *source, rhi::ImageView::Sampled);
    if (!sampled) {
        MH4N_LOG(Warning, kLog, "Display source: {}", sampled.error());
        return {};
    }
    if (external) shared_->Acquire(recorder, *external);
    rhi::TargetPassDesc pass{.color = &target, .color_load = rhi::LoadOp::DontCare, .area = {0, 0, width, height}};
    recorder.BeginTargetPass(pass);
    recorder.SetPipeline(*pipeline_);
    recorder.Bind(**set, 0);
    recorder.Draw(3, 1, 0, PictureFlags(source->GetFormat(), format, flip));
    recorder.EndPass();
    if (external) shared_->Release(recorder, *external);
    const auto ready = backend_->Submit(std::move(recorder));
    backend_->Retire(source, ready);
    backend_->Retire(pipeline_, ready);
    if (external) backend_->Retire(external, ready);
    if (local) backend_->Retire(local, ready);
    auto fence = external ? shared_->ExportFence() : nullptr;
    return std::make_shared<Picture>(backend_, std::move(local), std::move(external), std::move(fence), ready);
}

void ScreenPictures::Reset() {
    shared_pool_.clear();
}

} // namespace mh4n::gpu::native
