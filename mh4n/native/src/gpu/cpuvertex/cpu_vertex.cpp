// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: cpuvertex, PICA and vertex loader contracts, gpu_cpuvertex tests and 3dbrew GPU/Internal Registers.

#include "cpu_vertex.h"

#include "geometry_pipeline.h"
#include "native_programs.h"
#include "shader_interpreter.h"
#include "mh4n/log.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <unordered_set>
#include <utility>

namespace mh4n::gpu::cpuvertex {
namespace {

namespace reg = pica::reg;

constexpr char kLog[] = "CpuVertex";
constexpr u32 kOutmapTotal = 0x4F;
constexpr u32 kOutmapBase = 0x50;

u32 UseGeometry(const pica::PicaState& state) {
    return state.regs[reg::kGeometryStage] & 3;
}

ShaderInputConfig VertexInputConfig(const pica::PicaState& state) {
    return {
        state.regs[reg::kVs + reg::kShaderInputBufferConfig] & 15,
        state.regs[reg::kVs + 0x0B] | (static_cast<u64>(state.regs[reg::kVs + 0x0C]) << 32),
    };
}

u32 MainOffset(const pica::PicaState& state, u32 block) {
    return state.regs[block + reg::kShaderMainOffset] & 0xFFFF;
}

Vec4 ToShaderVector(const pica::Vec4f& vector) {
    return {{f24::FromFloat32(vector.c[0]), f24::FromFloat32(vector.c[1]),
             f24::FromFloat32(vector.c[2]), f24::FromFloat32(vector.c[3])}};
}

Uniforms VertexUniforms(const pica::ShaderSetup& source) {
    Uniforms uniforms;
    std::transform(source.uniform_f.begin(), source.uniform_f.end(), uniforms.f.begin(), ToShaderVector);
    uniforms.b = source.uniform_b;
    uniforms.i = source.uniform_i;
    return uniforms;
}

class OutputMapping final : public VertexSink {
public:
    explicit OutputMapping(OutputStream& output) : output_(output) {}

    bool SetMap(const pica::PicaState& state, bool validate) {
        count_ = (state.regs[kOutmapTotal] & 7u) * 4;
        for (u32 i = 0; i < count_; ++i) {
            const u32 semantic = (state.regs[kOutmapBase + i / 4] >> (8 * (i % 4))) & 31;
            if (validate && semantic >= 24 && semantic < 31) {
                return false;
            }
            slots_[i] = semantic;
        }
        return true;
    }

    void SubmitVertex(const AttributeBuffer& values) override {
        std::array<float, 24> fields;
        fields.fill(1.0f);
        for (u32 i = 0; i < count_; ++i) {
            if (slots_[i] < fields.size()) {
                fields[slots_[i]] = values[i / 4].c[i % 4].ToFloat32();
            }
        }
        for (u32 semantic = 8; semantic < 12; ++semantic) {
            fields[semantic] = std::min(std::fabs(fields[semantic]), 1.0f);
        }
        output_.vertices.push_back(std::bit_cast<OutputVertex>(fields));
    }

    void SetWinding() override {
        output_.winding_before.push_back(static_cast<u32>(output_.vertices.size()));
    }

private:
    OutputStream& output_;
    u32 count_ = 0;
    std::array<u32, 28> slots_{};
};

template <typename T>
T ReadScalar(const u8* data) {
    T value;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

template <typename T>
void DecodeLanes(const u8* data, u32 count, Vec4& vector) {
    for (u32 lane = 0; lane < count; ++lane) {
        vector.c[lane] = f24::FromFloat32(static_cast<float>(ReadScalar<T>(data + lane * sizeof(T))));
    }
}

bool ReadInputs(const vertex::LoadedDraw& loaded, const ShaderInputConfig& config,
                u32 vertex, ShaderUnit& shader) {
    for (u32 attribute = 0; attribute <= config.max_input_attribute_index; ++attribute) {
        const u32 location = config.GetRegisterForAttribute(attribute);
        const auto& attr = loaded.attributes[location];
        if (attr.binding >= loaded.buffers.size()) {
            return false;
        }
        const auto& buf = loaded.buffers[attr.binding];
        const u32 index = buf.per_instance ? 0 : vertex;
        if (index >= buf.vertex_count || attr.components > 4) {
            return false;
        }
        const auto info = vertex::GetFormatInfo(attr.format);
        const size_t stride = buf.source ? buf.source_stride : buf.stride;
        const size_t offset = stride * index + attr.offset;
        const size_t bytes = attr.components * (info.size_bytes / info.lanes);
        if (buf.source ? offset + bytes > stride * buf.vertex_count : offset + bytes > buf.data.size()) {
            return false;
        }

        Vec4 value{{f24::Zero(), f24::Zero(), f24::Zero(), f24::One()}};
        const u8* data = (buf.source ? buf.source : buf.data.data()) + offset;
        switch (info.kind) {
        case vertex::ComponentKind::S8: DecodeLanes<s8>(data, attr.components, value); break;
        case vertex::ComponentKind::U8: DecodeLanes<u8>(data, attr.components, value); break;
        case vertex::ComponentKind::S16: DecodeLanes<s16>(data, attr.components, value); break;
        case vertex::ComponentKind::F32: DecodeLanes<float>(data, attr.components, value); break;
        }
        shader.input[location] = value;
    }
    return true;
}

const char* ErrorName(Unhandled kind) {
    switch (kind) {
    case Unhandled::Instruction: return "unsupported shader instruction";
    case Unhandled::CompareMode: return "unsupported compare mode";
    case Unhandled::EmitOnVertexShader: return "geometry emission in a vertex shader";
    case Unhandled::EmitVertexId: return "invalid emitted vertex id";
    case Unhandled::EntryPoint: return "shader entry point outside program memory";
    case Unhandled::InstructionBudget: return "shader instruction budget exceeded";
    case Unhandled::GeometryConfig: return "inconsistent geometry configuration";
    case Unhandled::GeometryNeedsIndex: return "variable geometry requires indices";
    case Unhandled::GeometryOverflow: return "geometry inputs exceed buffer size";
    case Unhandled::LoaderBuffer: return "vertex missing from loader buffer";
    case Unhandled::OutputSemantic: return "invalid rasterizer output semantic";
    default: return "unknown shader error";
    }
}

struct ReportedError {
    u64 code;
    u64 descriptors;
    u32 entry;
    Unhandled kind;
    bool geometry;
    bool operator==(const ReportedError&) const = default;
};

struct ReportedErrorHash {
    size_t operator()(const ReportedError& value) const noexcept {
        return static_cast<size_t>(value.code ^ (value.descriptors << 1) ^
                                   (u64{value.entry} << 16) ^
                                   (static_cast<u64>(value.kind) << 1) ^ value.geometry);
    }
};

struct CachedVertex {
    u32 source_index = ~u32{0};
    AttributeBuffer outputs{};
};

} // namespace

const char* ToString(CpuReason reason) {
    switch (reason) {
    case CpuReason::GeometryShader: return "geometry";
    case CpuReason::Immediate: return "immediate";
    case CpuReason::Refused: return "refused by accelerate_draw";
    default: return "none";
    }
}

CpuReason CpuPathReason(const pica::DrawCall& draw, bool assembler_empty) {
    const auto& state = *draw.state;
    if (UseGeometry(state) == pica::kUseGsYes) {
        return CpuReason::GeometryShader;
    }
    if (draw.kind == pica::DrawKind::Immediate) {
        return CpuReason::Immediate;
    }
    const u32 topology = (state.regs[reg::kTriangleTopology] >> 8) & 3;
    if (!assembler_empty ||
        ((topology == pica::kTopologyList || topology == pica::kTopologyShader) && draw.num_vertices % 3 != 0)) {
        return CpuReason::Refused;
    }
    return CpuReason::None;
}

struct CpuVertexRunner::Impl {
    Impl() {
        gs_unit.SetVertexSink(&mapping);
        geometry.SetVertexSink(&mapping);
    }

    void SyncGeometryUniforms(const pica::PicaState& state) {
        const auto& source = state.gs;
        if (!have_gs_uniforms || source.uniform_b != uploaded_b ||
            state.dirty.Test(reg::kGs + reg::kShaderBoolUniforms)) {
            gs_uniforms.b = source.uniform_b;
            uploaded_b = source.uniform_b;
        }
        gs_uniforms.i = source.uniform_i;

        // Geometry writes into the live constants. Apply guest changes against its previous snapshot.
        const bool float_data_written = [&] {
            for (u32 i = 0; i < 8; ++i) {
                if (state.dirty.Test(reg::kGs + reg::kShaderUniformData + i)) {
                    return true;
                }
            }
            return false;
        }();
        const u32 next = state.regs[reg::kGs + reg::kShaderUniformIndex] & 127;
        const bool finished_upload = float_data_written && source.uniform_queue.index == 0 && next > 0;
        for (u32 i = 0; i < uploaded_f.size(); ++i) {
            if (!have_gs_uniforms || !(source.uniform_f[i] == uploaded_f[i]) ||
                (finished_upload && i == next - 1)) {
                gs_uniforms.f[i] = ToShaderVector(source.uniform_f[i]);
                uploaded_f[i] = source.uniform_f[i];
            }
        }
        have_gs_uniforms = true;
    }

    void ClearOutput() {
        stream.vertices.clear();
        stream.winding_before.clear();
    }

    RunStatus Fail(const pica::PicaState& state, RunError error, bool from_geometry, bool full) {
        ClearOutput();
        ++totals.unhandled;
        last = ErrorName(error.kind);
        const auto& shader = from_geometry ? state.gs : state.vs;
        const auto block = from_geometry ? reg::kGs : reg::kVs;
        const ReportedError key{
            pica_native_hash(shader.program_code.data(), pica::kMaxProgramWords),
            pica_native_hash(shader.swizzle_data.data(), 128),
            MainOffset(state, block), error.kind, from_geometry,
        };
        if (reported.insert(key).second) {
            MH4N_LOG(Warning, kLog, "{}", last);
        }
        if (full) {
            geometry.Reset();
            geometry_configured = false;
        }
        return RunStatus::Unhandled;
    }

    RunStatus Run(gpu::Host& host, const pica::DrawCall& draw, RunMode mode) {
        ++totals.draws;
        ClearOutput();
        last.clear();

        const auto& state = *draw.state;
        const bool full = mode == RunMode::Full;
        const bool immediate = draw.kind == pica::DrawKind::Immediate;
        const bool indexed = draw.kind == pica::DrawKind::Indexed;
        if (full) {
            SyncGeometryUniforms(state);
            geometry.SetState(state, &engine);
            geometry.SetNative(programs.Find(state.gs, MainOffset(state, reg::kGs), true), check_native);
            if (!geometry_configured || draw.geometry_reconfigured) {
                if (!geometry.Reconfigure(!geometry_configured && !draw.geometry_reconfigured)) {
                    return Fail(state, geometry.error(), true, true);
                }
                geometry_configured = true;
            }
            if (!indexed && (geometry.NeedIndexInput() || state.geometry.mode == pica::GeometryMode::VariablePrimitive)) {
                return Fail(state, {Unhandled::GeometryNeedsIndex}, true, true);
            }
        }
        if (!mapping.SetMap(state, full && !immediate)) {
            return Fail(state, {Unhandled::OutputSemantic}, false, full);
        }

        u32 count = 1;
        if (!immediate) {
            loader.LoadInto(host, draw, loaded, false, indexed);
            if (loaded.status == vertex::LoadStatus::NoVertices) {
                return RunStatus::Ok;
            }
            if (loaded.status != vertex::LoadStatus::Ok) {
                ++totals.load_failed;
                last = vertex::ToString(loaded.status);
                if (full) {
                    geometry.Reset();
                    geometry_configured = false;
                }
                return RunStatus::LoadFailed;
            }
            count = loaded.count;
        }

        const auto config = VertexInputConfig(state);
        const auto uniforms = VertexUniforms(state.vs);
        const u32 entry = MainOffset(state, reg::kVs);
        const ShaderSetup setup{
            state.vs.program_code.data(), state.vs.swizzle_data.data(), &uniforms, entry,
            programs.Find(state.vs, entry, false), check_native,
        };
        const u32 mask = state.regs[reg::kVs + reg::kShaderOutputMask];
        ShaderUnit shader;
        if (immediate) {
            AttributeBuffer attributes;
            std::transform(state.immediate.input_vertex.begin(), state.immediate.input_vertex.end(),
                           attributes.begin(), ToShaderVector);
            shader.LoadInput(config, attributes);
        }

        std::array<CachedVertex, 32> cache;
        size_t next_cache_slot = 0;
        AttributeBuffer outputs{};
        RunError error;
        for (u32 i = 0; i < count; ++i) {
            const u32 index = indexed ? loaded.list[i] : i;
            if (full && geometry.NeedIndexInput()) {
                if (!geometry.SubmitIndex(index)) {
                    return Fail(state, geometry.error(), true, true);
                }
                continue;
            }

            ++totals.vertices;
            auto hit = cache.end();
            if (indexed) {
                hit = std::find_if(cache.begin(), cache.end(),
                                   [&](const CachedVertex& value) { return value.source_index == index; });
            }
            const AttributeBuffer* vertex = &outputs;
            if (hit == cache.end()) {
                if (!immediate &&
                    ((indexed && index < loaded.first_vertex) ||
                     !ReadInputs(loaded, config, indexed ? index - loaded.first_vertex : index, shader))) {
                    return Fail(state, {Unhandled::LoaderBuffer}, false, full);
                }
                ++totals.vs_invocations;
                if (!engine.Run(setup, shader, error)) {
                    return Fail(state, error, false, full);
                }
                shader.WriteOutput(mask, outputs);
                if (indexed) {
                    cache[next_cache_slot] = {index, outputs};
                    next_cache_slot = (next_cache_slot + 1) % cache.size();
                }
            } else {
                vertex = &hit->outputs;
            }

            if (full) {
                const u64 before = geometry.invocations();
                const bool ok = geometry.SubmitVertex(*vertex);
                totals.gs_invocations += geometry.invocations() - before;
                if (!ok) {
                    return Fail(state, geometry.error(), true, true);
                }
            } else {
                mapping.SubmitVertex(*vertex);
            }
        }
        totals.vertices_out += stream.vertices.size();
        return RunStatus::Ok;
    }

    OutputStream stream;
    OutputMapping mapping{stream};
    Counters totals;
    std::string last;
    vertex::VertexLoader loader{vertex::VertexCaps{}};
    vertex::LoadedDraw loaded;
    InterpreterEngine engine;
    GeometryShaderUnit gs_unit;
    Uniforms gs_uniforms;
    GeometryPipeline geometry{gs_unit, gs_uniforms};
    bool geometry_configured = false;
    bool have_gs_uniforms = false;
    std::array<pica::Vec4f, pica::kNumFloatUniforms> uploaded_f{};
    std::array<bool, 16> uploaded_b{};
    std::unordered_set<ReportedError, ReportedErrorHash> reported;
    NativePrograms programs;
    bool check_native = false;
};

CpuVertexRunner::CpuVertexRunner() : impl_(std::make_unique<Impl>()) {}
CpuVertexRunner::~CpuVertexRunner() = default;

RunStatus CpuVertexRunner::Run(gpu::Host& host, const pica::DrawCall& draw, RunMode mode) {
    return impl_->Run(host, draw, mode);
}

void CpuVertexRunner::SetNativePrograms(const PicaNativeEntry* entries, u32 count, bool check) {
    impl_->programs.SetTable(entries, count);
    impl_->check_native = check;
}

void CpuVertexRunner::NoteDraw(const pica::DrawCall& draw) {
    impl_->SyncGeometryUniforms(*draw.state);
}

void CpuVertexRunner::Reset() {
    auto next = std::make_unique<Impl>();
    next->programs = std::move(impl_->programs);
    next->check_native = impl_->check_native;
    impl_ = std::move(next);
}

const OutputStream& CpuVertexRunner::output() const { return impl_->stream; }
const Counters& CpuVertexRunner::counters() const { return impl_->totals; }
const std::string& CpuVertexRunner::last_error() const { return impl_->last; }
const vertex::VertexCounters& CpuVertexRunner::loader_counters() const { return impl_->loader.counters(); }

} // namespace mh4n::gpu::cpuvertex
