// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: native_gpu.h, 3dbrew GPU/Internal Registers, the vertex and fragment contracts, and gpu_tev_test.
#include "renderer.h"
#include "render_robust.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <type_traits>

#include "gpu/backend/state/state.h"
#include "gpu/backend/vulkan_access.h"
#include "gpu/fbops/fixed_function.h"
#include "gpu/fragment/lighting/fixup.h"
#include "gpu/fragment/profile.h"
#include "mh4n/log.h"
#include "mh4n/present.h"
#include "provider_select.h"
#include "vertex_input.h"

namespace mh4n::gpu::native {
namespace {

constexpr u32 kViewportHalfWidth = 0x041;
constexpr u32 kViewportHalfHeight = 0x043;
constexpr u32 kViewportCorner = 0x068;
constexpr u32 kDepthFormat = 0x116;
constexpr u32 kColorFormat = 0x117;
constexpr u32 kDepthAddress = 0x11C;
constexpr u32 kColorAddress = 0x11D;
constexpr u32 kBufferDimensions = 0x11E;
constexpr MemoryRange kAllMemory{0, u64{1} << 32};
constexpr u64 kVertexRingBytes = 16 * kMiB;
constexpr auto kVertexRingUsage = rhi::BufferUsage::Vertex | rhi::BufferUsage::Index | rhi::BufferUsage::Storage;

using P = rhi::PicaFormat;

struct Coordinates {
    rhi::Viewport viewport;
    rhi::Rect area;
    fragment::UniformContext uniforms;
    bool flip_y = false;
};

u32 SurfaceAddress(u32 word) {
    return (word & 0x0fffffff) * 8;
}

bool IntegratedGpu(const rhi::Backend& backend) {
    const auto handles = rhi::Handles(backend);
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(handles.physical_device, &properties);
    return properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
}

bool ValidTarget(const SurfaceKey& key) {
    return key.address && key.Range().End() <= kAllMemory.End();
}

auto CreateUploadRing(rhi::Backend& backend, const char* name, u64 bytes, u64 minimum, rhi::BufferUsage usage) {
    // RHI rings always use the host-visible Upload heap.
    return CreateSmallerRing(name, bytes, minimum, [&](u64 size) { return backend.CreateRing(size, usage, name); });
}

u64 StagingBytes(u32 scale) {
    return std::clamp(scale * scale * (3 * kMiB), 16 * kMiB, 512 * kMiB);
}

s32 Coordinate(u32 word) {
    const s32 low = static_cast<s32>(word & 1023);
    return low >= 512 ? low - 1024 : low;
}

std::optional<P> ColorFormat(u32 word) {
    constexpr std::array formats{P::Rgba8, P::Rgb8, P::Rgb5A1, P::Rgb565, P::Rgba4};
    const u32 format = (word >> 16) & 7;
    if (format >= formats.size()) return std::nullopt;
    return formats[format];
}

std::optional<P> DepthFormat(u32 word) {
    switch (word & 3) {
    case 0: return P::D16;
    case 2: return P::D24;
    case 3: return P::D24S8;
    default: return std::nullopt;
    }
}

Coordinates DrawCoordinates(const pica::PicaState& state, u32 width, u32 height, u32 scale) {
    const auto& regs = state.regs;
    const bool flip = !(regs[kBufferDimensions] & (1u << 24));
    const float factor = static_cast<float>(scale);
    const u32 corner = regs[kViewportCorner];
    const float x = static_cast<float>(Coordinate(corner)) * factor;
    const float y = static_cast<float>(Coordinate(corner >> 16)) * factor;
    const float w = 2 * factor * pica::Float24ToFloat(regs[kViewportHalfWidth]);
    const float h = 2 * factor * pica::Float24ToFloat(regs[kViewportHalfHeight]);
    const float surface_height = static_cast<float>(height * scale);
    const float top = flip ? surface_height - y - h : y;

    Coordinates draw{};
    draw.flip_y = flip;
    draw.uniforms.framebuffer_scale = static_cast<int>(scale);
    draw.viewport = {x, top, w, h, 0, 1};
    namespace reg = fragment::freg;
    const u32 start = regs[reg::kScissorXY1], end = regs[reg::kScissorXY2];
    const int ix = static_cast<int>(scale);
    int left = static_cast<int>(start & 1023) * ix;
    int upper = static_cast<int>((start >> 16) & 1023) * ix;
    int right = (static_cast<int>(end & 1023) + 1) * ix;
    int lower = (static_cast<int>((end >> 16) & 1023) + 1) * ix;
    if (flip) {
        const int buffer_height = static_cast<int>(height * scale);
        const int former_upper = upper;
        upper = buffer_height - lower;
        lower = buffer_height - former_upper;
    }
    draw.uniforms.scissor = fragment::UniformContext::Scissor{left, upper, right, lower};

    if (!(w > 0 && h > 0 && std::isfinite(w) && std::isfinite(h))) return draw;
    const auto bound = [](float value, u32 limit) {
        return static_cast<int>(std::clamp(value, 0.0f, static_cast<float>(limit)));
    };
    if ((regs[reg::kScissorMode] & 3) != static_cast<u32>(fragment::re::ScissorMode::Include)) {
        left = upper = 0;
        right = static_cast<int>(width * scale);
        lower = static_cast<int>(height * scale);
    }
    left = std::max(left, bound(std::floor(x), width * scale));
    upper = std::max(upper, bound(std::floor(top), height * scale));
    right = std::min(right, bound(std::ceil(x + w), width * scale));
    lower = std::min(lower, bound(std::ceil(top + h), height * scale));
    draw.area = {left, upper, static_cast<u32>(std::max(0, right - left)),
                 static_cast<u32>(std::max(0, lower - upper))};
    return draw;
}

VertexStageKey CpuStage() {
    VertexStageKey key{};
    key.kind = VertexStageKind::Trivial;
    const auto layout = cpuvertex::MakeBatchVertexLayout();
    key.input.binding_count = static_cast<u8>(layout.bindings.size());
    key.input.attribute_count = static_cast<u8>(layout.attributes.size());
    for (size_t i = 0; i < layout.bindings.size(); ++i) {
        const auto& binding = layout.bindings[i];
        key.input.bindings[i] = {binding.stride, static_cast<u8>(binding.per_instance)};
    }
    for (size_t i = 0; i < layout.attributes.size(); ++i) {
        const auto& attr = layout.attributes[i];
        key.input.attributes[i] = {static_cast<u8>(attr.location), static_cast<u8>(attr.binding),
                                   attr.format, attr.offset};
    }
    return key;
}

template <typename T>
bool UploadUniform(rhi::Ring& ring, const T& data, u32 alignment, UniformRange& range) {
    auto upload = ring.Map(sizeof(T), alignment);
    if (!upload) return false;
    std::memcpy(upload->data.data(), &data, sizeof(T));
    if (!ring.Commit(sizeof(T))) return false;
    range = {ring.GetBuffer().get(), upload->offset, sizeof(T)};
    return true;
}

vertex::VertexCaps VertexCapabilities(const rhi::Capabilities& caps) {
    return {caps.triangle_fans, caps.index_uint8, caps.apple, caps.stride_alignment,
            caps.primitive_restart_always_on};
}

bool SelectGeometryFixup(const NativeOptions& options, const rhi::Capabilities& caps) {
    const char* setting = std::getenv("MH4N_LIGHTING_FIXUP");
    const std::string_view mode = setting ? std::string_view{setting} : options.lighting_fixup;
    return (mode == "gs" || (mode == "auto" && caps.vendor_id != 0x13B5)) &&
           caps.geometry_shader && caps.clip_distance;
}

bool LightingFixupEnabled(const NativeOptions& options) {
    const char* setting = std::getenv("MH4N_LIGHTING_FIXUP");
    const std::string_view mode = setting ? std::string_view{setting} : options.lighting_fixup;
    return mode != "off" && mode != "0";
}

} // namespace

std::unique_ptr<Renderer> Renderer::Create(Host& host, const NativeOptions& options, CounterMap& counters) {
    auto backend = std::static_pointer_cast<rhi::Backend>(present::SharedBackend());
    if (!backend) {
        auto instance = rhi::CreateInstance();
        if (!instance) {
            PublishGpuInitFailure(counters[kGpuInitFailed], "Vulkan instance", instance.error());
            return {};
        }
        auto created = rhi::CreateBackend(*instance);
        if (!created) {
            PublishGpuInitFailure(counters[kGpuInitFailed], "Vulkan device", created.error());
            return {};
        }
        backend = std::move(*created);
    }
    auto renderer = std::unique_ptr<Renderer>(new Renderer(host, options, counters, std::move(backend), options.scale));
    if (renderer->Init()) return renderer;
    return {};
}

Renderer::Renderer(Host& host, const NativeOptions& options, CounterMap& counters,
                   std::shared_ptr<rhi::Backend> backend, u32 scale)
    : host_(host), vertex_host_(host, [this](MemoryRange range) { PrepareDrawMemory(range); }),
      options_(options), counters_(counters), backend_(std::move(backend)), scale_(scale),
      builder_(backend_, fragment::FeatureRegistry::Global()), fragment_(fragment::ProfileOf(backend_->Caps())),
      surfaces_(host_, backend_, scale_, counters_), textures_(host_, backend_, surfaces_, options_, counters_),
      loader_(VertexCapabilities(backend_->Caps())), screen_pictures_(backend_) {
    gpu_time_total_ = backend_->FrameGpuTimeTotalNs();
}

Renderer::~Renderer() {
    SubmitDraws();
    if (last_submission_) backend_->WaitIdle(last_submission_);
}

bool Renderer::Init() {
    const auto fail = [this](std::string_view resource, std::string_view cause) {
        PublishGpuInitFailure(counters_[kGpuInitFailed], resource, cause);
        return false;
    };
    geometry_fixup_ = SelectGeometryFixup(options_, backend_->Caps());
    pull_fixup_ = !geometry_fixup_ && LightingFixupEnabled(options_);
    if (geometry_fixup_) {
        static std::once_flag once;
        std::call_once(once, [] {
            FixupGeneratorRegistry::Global().Set([](const VertexStageOutputs& stage) {
                fragment::lighting::FixupStageInputs inputs{};
                inputs.num_outputs = stage.num_outputs;
                inputs.clip_distance = true;
                const size_t count = std::min(inputs.semantic_map.size(), stage.semantic_map.size());
                for (size_t i = 0; i < count; ++i) {
                    inputs.semantic_map[i] = {stage.semantic_map[i].attribute, stage.semantic_map[i].component};
                }
                return fragment::lighting::GenerateFixupGeometrySpirv(inputs);
            });
        });
    }
    provider_ = MakePipelineProvider(backend_, builder_, options_.options, options_.force_ubershader);
    auto composer = DrawComposer::Create(backend_, *provider_);
    if (!composer) return fail("draw composer", composer.error());

    constexpr fragment::TableRingsDesc defaults{};
    auto tables = CreateSmallerRing("PICA lighting/fog lookup table", defaults.lf_bytes, kMiB, [&](u64 bytes) {
        const u64 general = defaults.general_bytes * bytes / defaults.lf_bytes;
        auto result = fragment::TableRings::Create(*backend_, {bytes, general});
        if (result && general != defaults.general_bytes) {
            MH4N_LOG(Warning, "NativeGpu", "PICA procedural lookup table: using {} bytes after allocation failure",
                     (*result)->Buffer(fragment::TableRing::General).Size());
        }
        return result;
    });
    if (!tables) return fail("PICA lookup tables", tables.error());

    auto fragment_uniforms = CreateSmallerRing("PICA fragment uniforms", kMiB, kMiB / 4, [this](u64 bytes) {
        return fragment::FragmentUniformRing::Create(*backend_, bytes);
    });
    if (!fragment_uniforms) return fail("PICA fragment uniforms", fragment_uniforms.error());

    auto uniforms = CreateUploadRing(*backend_, "PICA vertex uniforms", 2 * kMiB, kMiB,
                                     rhi::BufferUsage::Uniform | rhi::BufferUsage::Storage);
    if (!uniforms) return fail("PICA vertex uniforms", uniforms.error());
    auto vertices = CreateUploadRing(*backend_, "PICA vertices", kVertexRingBytes, kMiB, kVertexRingUsage);
    if (!vertices) return fail("PICA vertices", vertices.error());

    // A surface at a high internal resolution is tens of MiB. Large uploads can also use the existing staging fallback.
    auto staging = CreateUploadRing(*backend_, "PICA image upload", StagingBytes(scale_), kMiB,
                                    rhi::BufferUsage::Transfer);
    if (!staging) return fail("PICA image upload", staging.error());
    composer_ = std::move(*composer);
    tables_ = std::move(*tables);
    fragment_uniforms_ = std::move(*fragment_uniforms);
    uniforms_ = std::move(*uniforms);
    vertices_ = std::move(*vertices);
    staging_ = std::move(*staging);
    if (pull_fixup_) fixup_sets_ = std::make_unique<rhi::BindingArena>(backend_);
    const auto native_table = reinterpret_cast<PicaNativeTableFunction>(host_.CodeSymbol(PICA_NATIVE_TABLE_SYMBOL));
    if (native_table) {
        unsigned count = 0, version = 0;
        const auto* programs = native_table(&count, &version);
        if (programs && version == PICA_NATIVE_VERSION) cpu_.SetNativePrograms(programs, count, false);
    }
    last_submission_ = backend_->LastSubmittedValue();
    return true;
}

void Renderer::Drop(std::string_view reason) {
    ++counters_["native_draws_dropped"];
    if (!reason.empty() && logged_drops_++ < 8) MH4N_LOG(Warning, "NativeGpu", "Draw dropped: {}", reason);
}

rhi::Recorder& Renderer::Recording() {
    if (!recording_) {
        recording_.emplace(backend_->BeginRecording());
        recording_->MarkFrame(rhi::FrameMark::Begin);
        composer_->BeginRecording(*recording_);
        fragment_uniforms_->BeginRecording();
    }
    return *recording_;
}

void Renderer::SubmitDraws() {
    if (!recording_) return;
    composer_->EndPass(*recording_);
    if (recorded_draws_) recording_->MarkFrame(rhi::FrameMark::End);
    last_submission_ = backend_->Submit(std::move(*recording_));
    recording_.reset();
    composer_->Submitted(true);
    textures_.Submitted(last_submission_);
    for (auto& pipeline : recorded_quaternions_) backend_->Retire(std::move(pipeline), last_submission_);
    recorded_quaternions_.clear();
    attempted_draws_ = recorded_draws_ = 0;
    vertex_bytes_ = 0;
}

void Renderer::ReserveVertexSpace(u64 bytes) {
    if (vertex_bytes_ && vertex_bytes_ + bytes > vertices_->Size() / 2) SubmitDraws();
    // On a reduced ring, a single draw may need more than the whole ring. Try just what it needs.
    if (bytes > vertices_->Size() && vertices_->Size() < kVertexRingBytes && bytes <= kVertexRingBytes) {
        auto larger = backend_->CreateRing(bytes, kVertexRingUsage, "PICA vertices for a large draw");
        if (larger) {
            SubmitDraws();
            backend_->Retire(vertices_->GetBuffer(), last_submission_);
            vertices_ = std::move(*larger);
            MH4N_LOG(Info, "NativeGpu", "PICA vertices: using {} bytes for a large draw", bytes);
        }
    }
    vertex_bytes_ += bytes;
    Recording();
}

bool Renderer::UploadUniforms(DrawDesc& draw, const pica::PicaState& state, bool flip_y, bool changed) {
    const auto alignment = backend_->Caps().uniform_buffer_offset_alignment;
    if (draw.vertex.kind == VertexStageKind::Translated && !draw.vs_pica_uniforms.buffer &&
        !UploadUniform(*uniforms_, shader::MakePicaUniformData(state.vs), alignment, draw.vs_pica_uniforms)) {
        return false;
    }
    if (!UploadUniform(*uniforms_, shader::MakeVertexUniformData(state, flip_y), alignment, draw.vs_uniforms)) {
        return false;
    }
    auto fragment_uniforms = fragment_uniforms_->Write(fragment_.block(), changed);
    if (!fragment_uniforms) return false;
    draw.fragment_uniforms = *fragment_uniforms;
    draw.tables = tables_.get();
    return true;
}

bool Renderer::UploadVertices(DrawDesc& draw, const vertex::LoadedDraw& loaded,
                              std::array<rhi::VertexBufferBinding, rhi::kMaxVertexBindings>& buffers) {
    if (loaded.buffers.size() > buffers.size()) return false;
    const auto* buffer = vertices_->GetBuffer().get();
    const u32 alignment = std::max(4u, backend_->Caps().stride_alignment);
    for (size_t i = 0; i < loaded.buffers.size(); ++i) {
        const auto& source = loaded.buffers[i];
        const auto bytes = source.Bytes();
        auto dest = vertices_->Map(bytes, alignment);
        if (!dest) return false;
        source.CopyTo(dest->data.data());
        if (!vertices_->Commit(bytes)) return false;
        buffers[i] = {buffer, dest->offset};
    }
    draw.vertex_buffers = std::span{buffers}.first(loaded.buffers.size());
    draw.count = loaded.count;
    draw.base_vertex = loaded.base_vertex;
    draw.first = 0;
    if (loaded.index_type == vertex::IndexType::None) return true;

    const auto bytes = loaded.index_data.size();
    const auto padded = (bytes + 3) & ~size_t{3};
    auto dest = vertices_->Map(padded, 4);
    if (!dest) return false;
    std::memcpy(dest->data.data(), loaded.index_data.data(), bytes);
    std::memset(dest->data.data() + bytes, 0, padded - bytes);
    if (!vertices_->Commit(padded)) return false;
    draw.index_buffer = buffer;
    draw.index_offset = dest->offset;
    draw.first = 0;
    switch (loaded.index_type) {
    case vertex::IndexType::U8: draw.index_type = rhi::IndexType::U8; break;
    case vertex::IndexType::U16: draw.index_type = rhi::IndexType::U16; break;
    case vertex::IndexType::U32: draw.index_type = rhi::IndexType::U32; break;
    default: return false;
    }
    return true;
}

void Renderer::RecordDraw(const pica::DrawCall& source, DrawDesc& draw, Surface* color, Surface* depth,
                           bool flip_y, bool uniforms_changed) {
    const pica::PicaState& state = *source.state;
    const auto area = draw.target.area;
    if (!area.width || !area.height) {
        cpu_.NoteDraw(source);
        Drop("empty framebuffer or viewport");
        return;
    }

    if ((!color && (!depth || draw.needs.shadow_write_pass)) ||
        (draw.needs.shadow_write_pass && !Shadows())) {
        cpu_.NoteDraw(source);
        if (Shadows() || !draw.needs.shadow_write_pass) Drop("framebuffer unavailable");
        return;
    }
    if (color && color->key.shadow && !Shadows() && area.x == 0 && area.y == 0 &&
        area.width == color->key.width * scale_ && area.height == color->key.height * scale_) {
        cpu_.NoteDraw(source);
        return;
    }

    draw.state = fbops::MakeFixedFunctionState(state);
    if (draw.needs.shadow_write_pass) {
        draw.shadow_target = color->image.get();
    } else {
        draw.target.color = color ? color->image.get() : nullptr;
        draw.target.depth = depth ? depth->image.get() : nullptr;
        draw.color_format = color ? surfaces_.ColorFormat(*color) : rhi::Format::Undefined;
        draw.depth_format = depth ? surfaces_.DepthFormat(*depth) : rhi::Format::Undefined;
        if (!depth) {
            draw.state.depth_test = draw.state.depth_write = draw.state.stencil_test = false;
        }
    }

    const auto plan = rhi::state::PlanPipeline(draw.state, backend_->Caps());
    if (!plan) {
        cpu_.NoteDraw(source);
        Drop(plan.error());
        return;
    }
    draw.needs.dst_copy |= plan->needs_destination_copy;

    bool cpu_path = cpuvertex::NeedsCpuPath(source, assembler_.IsEmpty());
    bool vertex_only = false;
    const bool pull = !cpu_path && draw.needs.fixup_gs && pull_fixup_;
    draw.needs.fixup_gs = draw.needs.fixup_gs && geometry_fixup_;

    std::array<rhi::VertexBufferBinding, rhi::kMaxVertexBindings> buffers{};
    if (!cpu_path) {
        loader_.LoadInto(vertex_host_, source, loaded_, false, false);
        if (loaded_.status != vertex::LoadStatus::Ok) {
            cpu_.NoteDraw(source);
            Drop(vertex::ToString(loaded_.status));
            return;
        }
        counters_["vertex_restart_widened"] += loaded_.restart_widened;
        draw.topology = pull ? rhi::Topology::TriangleList : ToRhiTopology(loaded_.topology);
        const auto config = shader::MakeVertexShaderConfig(
            state, &loaded_, {.use_clip_planes = backend_->Caps().clip_distance,
                              .geometry_stage = draw.needs.fixup_gs, .pull_fixup = pull});
        draw.vertex.program = config.Hash();
        if (!pull) draw.vertex.input = MakeVertexInput(loaded_);
        builder_.AddVertexSource(config, state.vs, draw.vertex.program);
        DrawDesc fallback = draw;
        fallback.vertex = CpuStage();
        fallback.topology = rhi::Topology::TriangleList;
        fallback.fallback_only = true;
        const auto readiness = provider_->Prepare(composer_->KeyOf(draw), composer_->KeyOf(fallback));
        if (readiness == PipelineReadiness::Pending) {
            pending_draw_ = true;
            cpu_.NoteDraw(source);
            Drop({});
            return;
        }
        cpu_path = readiness != PipelineReadiness::Ready;
        if (cpu_path) {
            vertex_only = true;
            draw.fallback_only = readiness != PipelineReadiness::Failed;
            counters_["translate_failures"] += readiness == PipelineReadiness::Failed;
        } else {
            u64 needed = loaded_.index_data.size() + 4;
            for (const auto& input : loaded_.buffers) needed += input.Bytes() + backend_->Caps().stride_alignment + 4;
            ReserveVertexSpace(pull ? std::max(vertices_->Size(), needed) : needed);
            if (!UploadVertices(draw, loaded_, buffers)) {
                cpu_.NoteDraw(source);
                Drop("vertex ring exhausted");
                return;
            }
            if (pull) {
                cpu_path = !UploadUniform(*uniforms_, shader::MakePicaUniformData(state.vs),
                                          backend_->Caps().uniform_buffer_offset_alignment, draw.vs_pica_uniforms) ||
                           !PreparePull(draw, source, Recording());
                vertex_only = cpu_path;
            }
        }
    }

    if (cpu_path) {
        const auto before = cpu_.counters();
        const auto status = cpu_.Run(vertex_host_, source, vertex_only ? cpuvertex::RunMode::VertexOnly : cpuvertex::RunMode::Full);
        const auto& after = cpu_.counters();
        counters_["cpu_path_draws"] += after.draws - before.draws;
        counters_["cpu_path_vertices"] += after.vertices - before.vertices;
        counters_["cpu_path_unhandled"] += after.unhandled - before.unhandled;
        counters_["cpu_path_load_failed"] += after.load_failed - before.load_failed;
        if (status != cpuvertex::RunStatus::Ok) {
            Drop(cpu_.last_error());
            return;
        }
        batch_.Clear();
        const auto add = [this](const auto& a, const auto& b, const auto& c) { batch_.AddTriangle(a, b, c); };
        if (vertex_only) {
            cpuvertex::PrimitiveAssembler independent(assembler_.GetTopology());
            independent.SubmitStream(cpu_.output(), add);
        } else {
            assembler_.SubmitStream(cpu_.output(), add);
        }
        if (batch_.Empty()) return;
        ReserveVertexSpace(batch_.VertexCount() * cpuvertex::kHardwareVertexStride + 16);
        auto batch = cpuvertex::UploadBatch(*vertices_, batch_);
        if (!batch) {
            Drop(batch.error());
            return;
        }
        draw.vertex = CpuStage();
        draw.topology = rhi::Topology::TriangleList;
        draw.count = batch->vertex_count;
        draw.index_buffer = nullptr;
        draw.vs_pica_uniforms = {};
        buffers[0] = {vertices_->GetBuffer().get(), batch->offset};
        draw.vertex_buffers = std::span{buffers}.first(1);
    } else {
        cpu_.NoteDraw(source);
    }

    auto& recorder = Recording();
    if (!UploadUniforms(draw, state, flip_y, uniforms_changed)) {
        Drop("uniform ring exhausted");
        return;
    }
    const auto outside_pass = [&] { composer_->EndPass(recorder); };
    if ((color && !surfaces_.Upload(*color, area, recorder, *staging_, outside_pass)) ||
        (depth && !surfaces_.Upload(*depth, area, recorder, *staging_, outside_pass)) ||
        !textures_.Bind(state, draw, color, recorder, *staging_, outside_pass)) {
        Drop("framebuffer or texture upload failed");
        return;
    }

    const auto dynamic = fbops::MakeDynamicState(state);
    fbops::ApplyDynamicState(recorder, dynamic, plan->constants_from_alpha);
    const auto result = composer_->Draw(recorder, draw);
    if (!result || result->skipped) {
        pending_draw_ |= result && result->skipped;
        Drop(result ? std::string_view{} : std::string_view{result.error()});
        return;
    }
    ++counters_["native_draws"];
    ++recorded_draws_;
    if (color && !color->key.shadow && present::RendererCannotDraw()) {
        const auto failure = counters_.find(kGpuTargetFailed);
        if (failure != counters_.end() && failure->second) {
            failure->second = 0;
            present::SetRendererCannotDraw(false);
        }
    }
    counters_["fixup_draws"] += result->fixup;
    if (draw.pull_words) {
        ++counters_["pull_draws"];
        counters_["pull_vertices"] += draw.count;
    }
    if (color && (draw.state.color_write_mask || draw.needs.shadow_write_pass)) {
        surfaces_.Written(*color, area);
        textures_.Notify(color->key.Range());
        InvalidateScreens(color->key.Range());
    }
    if (depth && (draw.state.depth_write || draw.state.stencil_test)) surfaces_.Written(*depth, area);
}

void Renderer::Draw(const pica::DrawCall& source, bool touch_shown) {
    const auto& state = *source.state;
    assembler_.ApplyRegisterWrites(state);
    const u32 dimensions = state.regs[kBufferDimensions];
    const u32 width = dimensions & 2047;
    const u32 height = ((dimensions >> 12) & 1023) + 1;
    auto coordinates = DrawCoordinates(state, width, height, scale_);
    // Prepare fragment tables on every draw. The PICA clears its dirty bits after this call, even on a drop.
    const auto table_draws = [this](fragment::TableRing ring, u64 worst_upload) {
        // Leave room for a wrap that cannot reuse bytes still in this recorder.
        return std::max<u64>(1, tables_->Buffer(ring).Size() / (2 * (worst_upload + fragment::kTableAlignment)));
    };
    const u64 draw_limit = std::min({u64{32}, table_draws(fragment::TableRing::Lf, fragment::kLfRingMaxUpload),
                                    table_draws(fragment::TableRing::General, fragment::kGeneralRingMaxUpload)});
    if (attempted_draws_ >= draw_limit) SubmitDraws();
    const auto close_for_read = [this] { SubmitDraws(); };
    const auto prepare_fragment = [&] {
        Recording();
        auto prepared = fragment_.Prepare(state, coordinates.uniforms, tables_.get());
        if (!prepared) {
            fragment_.Invalidate();
            cpu_.NoteDraw(source);
            Drop(prepared.error());
            SubmitDraws();
        }
        return prepared;
    };
    auto prepared = prepare_fragment();
    if (!prepared) return;
    Surface* color = nullptr;
    Surface* depth = nullptr;
    // A readback needs its own recorder. Finish the table upload only when there is an actual readback.
    while (coordinates.area.width && coordinates.area.height) {
        const auto color_format = ColorFormat(state.regs[kColorFormat]);
        const auto depth_format = DepthFormat(state.regs[kDepthFormat]);
        const bool shadow = fragment_.needs().shadow_write_pass;
        bool failed_color = false;
        if (color_format) {
            SurfaceKey key{SurfaceAddress(state.regs[kColorAddress]), width, height, *color_format, true};
            if (!touch_shown && !shadow && !cpuvertex::NeedsCpuPath(source, assembler_.IsEmpty())) {
                key.shadow = false;
                if (SkipTouchDraw(key, coordinates.area)) {
                    cpu_.NoteDraw(source);
                    ++counters_["touch_draws_skipped"];
                    if (draw_limit < 32) ++attempted_draws_;
                    return;
                }
                key.shadow = true;
            }
            color = shadow ? surfaces_.Get(key) : surfaces_.Find(key);
            if (!color && !shadow) {
                key.shadow = false;
                color = surfaces_.Get(key);
            }
            failed_color = !color && ValidTarget(key) && (!shadow || Shadows());
        }
        bool failed_depth = false;
        if (depth_format && !shadow) {
            const SurfaceKey key{SurfaceAddress(state.regs[kDepthAddress]), width, height, *depth_format};
            depth = surfaces_.Get(key);
            failed_depth = !depth && ValidTarget(key);
        }

        const u32 smaller_scale = (failed_color || failed_depth) && scale_ > 1
            ? LowerTargetScale(scale_, IntegratedGpu(*backend_)) : 0;
        if (!smaller_scale) {
            if (!shadow && !color && !depth && (failed_color || failed_depth)) {
                auto& failure = counters_[kGpuTargetFailed];
                if (!failure) {
                    failure = 1;
                    PublishGpuFailure("Renderer cannot draw", failed_color ? "PICA color target" : "PICA depth target",
                                      "target allocation failed at scale " + std::to_string(scale_));
                }
            }
            break;
        }

        SubmitDraws();
        // Deferred display transfers depend on the scale of their source image.
        bool materialized = true;
        for (auto& [address, screen] : screens_) {
            if (screen->owed && !Materialize(*screen, {})) {
                materialized = false;
                break;
            }
        }
        if (!materialized) break;
        surfaces_.WriteBack(kAllMemory);
        if (last_submission_) backend_->WaitIdle(last_submission_);
        screens_.clear();
        complete_screens_ = {};
        screen_jobs_.clear();
        readback_pool_.clear();
        screen_pictures_.Reset();
        top_targets_ = {};
        touch_target_.reset();
        holding_screens_ = false;
        last_vblank_sequence_ = screen_sequence_;
        textures_.Reset();

        // Rebuild at the same address so TextureBindings retains its reference to the surface cache.
        Surfaces smaller_surfaces(host_, backend_, smaller_scale, counters_);
        static_assert(std::is_nothrow_move_constructible_v<Surfaces>);
        std::destroy_at(&surfaces_);
        std::construct_at(&surfaces_, std::move(smaller_surfaces));
        color = depth = nullptr;
        if (const u64 bytes = StagingBytes(smaller_scale); staging_->Size() > bytes) {
            auto staging = CreateUploadRing(*backend_, "PICA image upload", bytes, kMiB, rhi::BufferUsage::Transfer);
            if (staging) {
                staging_ = std::move(*staging);
                MH4N_LOG(Info, "NativeGpu", "PICA image upload: using {} bytes at scale {}", staging_->Size(), smaller_scale);
            }
        }
        MH4N_LOG(Warning, "NativeGpu", "PICA {} target allocation failed at scale {}; trying scale {}",
                 failed_color ? "color" : "depth", scale_, smaller_scale);
        scale_ = smaller_scale;
        coordinates = DrawCoordinates(state, width, height, scale_);
        prepared = prepare_fragment();
        if (!prepared) return;
    }

    if (const auto area = coordinates.area; area.width && area.height) {
        if (color) MaterializeScreens(color->key.Range(), false, close_for_read);
        if (depth) MaterializeScreens(depth->key.Range(), false, close_for_read);
        surfaces_.PrepareDraw(color, depth, area, close_for_read);
        textures_.Prepare(state, fragment_.needs(), [&](MemoryRange range, const Surface* target) {
            MaterializeScreens(range, false, close_for_read);
            surfaces_.WriteBack(range, target, false, close_for_read, true);
        });
    }
    DrawDesc draw{};
    draw.fragment = &fragment_.key();
    draw.needs = fragment_.needs();
    draw.target.area = coordinates.area;
    draw.viewport = coordinates.viewport;
    draw.scissor = coordinates.area;
    std::array<const char*, fragment::kFeatureNames.size()> stubbed{};
    const size_t missing = fragment::StubbedFeatures(*draw.fragment, fragment::FeatureRegistry::Global(), stubbed);
    for (const char* name : std::span{stubbed}.first(missing)) {
        const std::string key = std::string_view{name}.starts_with("stubbed:") ? name : std::string("stubbed:") + name;
        ++counters_[key];
    }
    RecordDraw(source, draw, color, depth, coordinates.flip_y, prepared->block_changed);
    ++attempted_draws_;
}

void Renderer::NotifyWrite(MemoryRange range, bool cpu_flush) {
    SubmitDraws();
    if (cpu_flush) {
        MaterializeScreens(range, true);
        surfaces_.WriteBack(range, nullptr, true);
    }
    surfaces_.Invalidate(range);
    textures_.Notify(range);
    InvalidateScreens(range);
}

void Renderer::InvalidateGuestRange(MemoryRange range) {
    PrepareMemory(range);
    surfaces_.Invalidate(range);
    textures_.Notify(range);
}

void Renderer::WriteBack() {
    PrepareMemory(kAllMemory);
}

void Renderer::Reset() {
    SubmitDraws();
    if (last_submission_) backend_->WaitIdle(last_submission_);
    surfaces_.Clear();
    textures_.Reset();
    screens_.clear();
    complete_screens_ = {};
    top_targets_ = {};
    touch_target_.reset();
    display_buffers_ = {};
    pending_draw_ = holding_screens_ = false;
    last_vblank_sequence_ = screen_sequence_;
    screen_pictures_.Reset();
    screen_jobs_.clear();
    readback_pool_.clear();
    fragment_.Invalidate();
    assembler_.Reset();
    cpu_.Reset();
}

void Renderer::FrameTick() {
    textures_.NextFrame();
    if (++frame_ticks_ % 1800 != 0) return;
    const auto memory = backend_->Memory();
    u64 images = 0, bytes = 0;
    textures_.Stats(images, bytes);
    MH4N_LOG(Info, "Memory", "[mem] the renderer: GPU allocator {} MB in {} blocks ({} MB in {} allocations); textures: {} images {} MB ({} let go so far)",
             memory.block_bytes >> 20, memory.blocks, memory.allocation_bytes >> 20, memory.allocations, images, bytes >> 20,
             counters_["textures_let_go"]);
}

void Renderer::FinishFrame(std::vector<Counter>& counters) {
    SubmitDraws();
    const auto add = [&](std::vector<FrameCounter> source) {
        for (auto& item : source) counters.push_back({std::move(item.name), item.value});
    };
    add(provider_->TakeFrameCounters());
    auto composition = composer_->TakeFrameCounters();
    const u64 texture_feedback = surfaces_.TakeFeedbackCopies();
    for (auto& item : composition) {
        if (item.name == "dst_copies") item.value += texture_feedback;
    }
    add(std::move(composition));
    const auto memory = backend_->Memory();
    counters_["vma_block_bytes"] = memory.block_bytes;
    counters_["vma_allocation_bytes"] = memory.allocation_bytes;
    counters_["vma_blocks"] = memory.blocks;
    counters_["vma_allocations"] = memory.allocations;
    for (auto& counter : counters) {
        const auto it = counters_.find(counter.name);
        if (counter.name.starts_with("vma_") && it != counters_.end()) counter.value = it->second;
    }
    // (with the GPU on its own thread FrameTick already ages the textures at each VBlank: not twice a frame)
    if (options_.options.GetString("--gpu", "") != "native-display-queue") textures_.NextFrame();
}

std::optional<u64> Renderer::TakeGpuTime() {
    const u64 total = backend_->FrameGpuTimeTotalNs();
    const u64 elapsed = total - gpu_time_total_;
    gpu_time_total_ = total;
    if (!elapsed) return std::nullopt;
    return elapsed;
}

} // namespace mh4n::gpu::native
