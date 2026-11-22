// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: the PICA contracts, gpu_pica_test, the MGPU INIT block layout, and 3dbrew GPU/Internal Registers.

#include "command_processor.h"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>

#include "mh4n/log.h"

namespace mh4n::gpu::pica {
namespace {

constexpr char kLog[] = "GPU.PICA";

class InitReader {
public:
    explicit InitReader(std::span<const u8> bytes) : bytes_(bytes) {}

    u8 Byte() {
        return bytes_[offset_++];
    }

    u32 Word() {
        u32 value = 0;
        for (u32 shift = 0; shift < 32; shift += 8) {
            value |= u32{Byte()} << shift;
        }
        return value;
    }

    void Words(std::span<u32> words) {
        for (u32& word : words) {
            word = Word();
        }
    }

    void Vectors(std::span<Vec4f> vectors) {
        for (Vec4f& vector : vectors) {
            for (float& component : vector.c) {
                component = std::bit_cast<float>(Word());
            }
        }
    }

    void Queue(PackedAttribute& queue) {
        Words(queue.buffer);
        queue.index = Word();
    }

private:
    std::span<const u8> bytes_;
    size_t offset_ = 0;
};

template <typename Decode>
void ReadBlock(const InitState& init, InitBlock kind, size_t size, Decode decode) {
    const auto bytes = init.Block(kind);
    if (bytes.size() == size) {
        InitReader reader(bytes);
        decode(reader);
    } else if (!bytes.empty()) {
        MH4N_LOG(Warning, kLog, "INIT block {} has {} bytes, expected {}",
                 static_cast<u32>(kind), bytes.size(), size);
    }
}

void ReadSetup(InitReader& reader, ShaderSetup& setup) {
    setup.entry_point = reader.Word();
    setup.biggest_program_size = std::min(reader.Word(), kMaxProgramWords);
    setup.biggest_swizzle_size = std::min(reader.Word(), kMaxSwizzleWords);
    reader.Words(setup.program_code);
    reader.Words(setup.swizzle_data);
    reader.Vectors(setup.uniform_f);
    for (bool& value : setup.uniform_b) {
        value = reader.Byte() != 0;
    }
    for (auto& vector : setup.uniform_i) {
        for (u8& component : vector) {
            component = reader.Byte();
        }
    }
    reader.Queue(setup.uniform_queue);
    setup.program_serial = ShaderSetup::NextSerial();
    setup.swizzle_serial = ShaderSetup::NextSerial();
    setup.uniforms_serial = ShaderSetup::NextSerial();
}

void UpdateTable(std::span<u32> table, u32 index, std::span<const u32> words, auto mark_dirty) {
    for (const u32 word : words) {
        u32& entry = table[index++ % table.size()];
        if (entry != word) {
            entry = word;
            mark_dirty();
        }
    }
}

std::span<u32> ProcTable(ProcTex& state, u32 type) {
    switch (type) {
    case kProcTexNoise: return state.noise_table;
    case kProcTexColorMap: return state.color_map_table;
    case kProcTexAlphaMap: return state.alpha_map_table;
    case kProcTexColor: return state.color_table;
    case kProcTexColorDiff: return state.color_diff_table;
    default: return {};
    }
}

u32 ChannelAddress(const PicaState& state, u32 channel) {
    return BitsOf(state.regs[reg::kCommandBufferAddress + channel], 0, 28) << 3;
}

u32 ChannelSize(const PicaState& state, u32 channel) {
    return BitsOf(state.regs[reg::kCommandBufferSize + channel], 0, 20) << 3;
}

} // namespace

CommandProcessor::CommandProcessor(gpu::Host& host, Backend& backend)
    : host_(host), backend_(backend), state_(std::make_unique<PicaState>()) {}

CommandProcessor::~CommandProcessor() = default;

void CommandProcessor::LoadInitState(const gpu::InitState& init) {
    // Allocate on the heap and keep state() references valid across the reset.
    const auto defaults = std::make_unique<PicaState>();
    *state_ = *defaults;
    auto& s = *state_;
    ReadBlock(init, InitBlock::PicaRegs, kNumRegs * sizeof(u32),
              [&](InitReader& r) { r.Words(s.regs); });
    ReadBlock(init, InitBlock::VsSetup, 34368, [&](InitReader& r) { ReadSetup(r, s.vs); });
    ReadBlock(init, InitBlock::GsSetup, 34368, [&](InitReader& r) { ReadSetup(r, s.gs); });
    ReadBlock(init, InitBlock::DefaultAttrs, 256,
              [&](InitReader& r) { r.Vectors(s.default_attributes); });
    ReadBlock(init, InitBlock::LightingLut, 24576, [&](InitReader& r) {
        for (auto& table : s.lighting.luts) {
            r.Words(table);
        }
    });
    ReadBlock(init, InitBlock::FogLut, 512, [&](InitReader& r) { r.Words(s.fog.lut); });
    ReadBlock(init, InitBlock::ProcTex, 3584, [&](InitReader& r) {
        r.Words(s.proctex.noise_table);
        r.Words(s.proctex.color_map_table);
        r.Words(s.proctex.alpha_map_table);
        r.Words(s.proctex.color_table);
        r.Words(s.proctex.color_diff_table);
    });
    ReadBlock(init, InitBlock::Immediate, 284, [&](InitReader& r) {
        r.Vectors(s.immediate.input_vertex);
        s.immediate.current_attribute = r.Word();
        s.immediate.reset_geometry_pipeline = r.Word() != 0;
        r.Queue(s.immediate.queue);
    });
    ReadBlock(init, InitBlock::GeometryPipeline, 288, [&](InitReader& r) {
        auto& g = s.geometry;
        g.mode = static_cast<GeometryMode>(r.Word());
        g.vs_output_num = r.Word();
        g.buffer_index = r.Word();
        g.buffer_size = r.Word();
        g.buffer_offset = r.Word();
        g.need_index = r.Word() != 0;
        g.main_vertex_num = r.Word();
        g.total_vertex_num = r.Word();
        r.Vectors(g.attribute_buffer);
    });
    delay_.Reset();
}

void CommandProcessor::ResetList(u32 paddr, u32 size) {
    list_ = {.paddr = paddr, .length = size / 4, .index = 0,
             .head = size ? host_.PhysicalPointer(paddr, size) : nullptr};
    if (list_.head == nullptr && list_.length != 0 &&
        host_.PhysicalPointer(paddr, 4) == nullptr) {
        list_.length = 0;
    }
}

u32 CommandProcessor::ListWord(u32 index) const {
    const u8* bytes = nullptr;
    if (list_.head != nullptr && index < list_.length) {
        bytes = list_.head + size_t{index} * 4;
    } else {
        const u64 addr = u64{list_.paddr} + u64{index} * 4;
        if (addr <= 0xFFFFFFFCu) {
            bytes = host_.PhysicalPointer(static_cast<u32>(addr), 4);
        }
    }
    u32 value = 0;
    if (bytes != nullptr) {
        std::memcpy(&value, bytes, sizeof(value));
    }
    return value;
}

void CommandProcessor::ProcessCmdList(u32 paddr, u32 size) {
    ++lists_;
    ResetList(paddr, size);
    bool stop_requested = false;
    while (!stop_requested && list_.index < list_.length) {
        list_.index += list_.index & 1;
        const u32 value = ListWord(list_.index++);
        const u32 header = ListWord(list_.index++);
        const u32 id = BitsOf(header, 0, 16);
        const u32 mask = BitsOf(header, 16, 4);
        const u32 extra_count = BitsOf(header, 20, 8);
        const bool sequential = BitsOf(header, 31, 1) != 0;
        const u32 old_address = list_.paddr;
        const u32 old_length = list_.length;
        const u32 start_extras = list_.index;
        delay_.AddCommands(1);
        WriteInternalReg(id, value, mask, stop_requested);
        if (stop_requested || list_.index != start_extras || list_.paddr != old_address ||
            list_.length != old_length || extra_count == 0) {
            continue;
        }
        std::array<u32, 255> words;
        for (u32 i = 0; i < extra_count; ++i) {
            words[i] = ListWord(list_.index++);
        }
        const std::span<const u32> extras(words.data(), extra_count);
        if (sequential) {
            WriteInternalRegSequential(id + 1, extras, mask, stop_requested);
        } else {
            WriteInternalRegBatch(id, extras, mask, stop_requested);
        }
    }
}

void CommandProcessor::SubmitCmdList(u32 index) {
    if (index >= 2 || state_->regs[reg::kCommandBufferTrigger + index] == 0) {
        return;
    }
    ProcessCmdList(ChannelAddress(*state_, index), ChannelSize(*state_, index));
    state_->regs[reg::kCommandBufferTrigger + index] = 0;
}

void CommandProcessor::SubmitGxCmdList(u32 paddr, u32 size) {
    WriteHardwareRegister(reg::kCommandBufferAddress,
                          WithBits(state_->regs[reg::kCommandBufferAddress], 0, 28, paddr >> 3));
    WriteHardwareRegister(reg::kCommandBufferSize,
                          WithBits(state_->regs[reg::kCommandBufferSize], 0, 20, size >> 3));
    WriteHardwareRegister(reg::kCommandBufferTrigger, 1);
}

void CommandProcessor::WriteHardwareRegister(u32 id, u32 value) {
    if (id >= kNumRegs) {
        return;
    }
    state_->regs[id] = value;
    state_->dirty.Set(id);
    if (InRange(id, reg::kCommandBufferTrigger, 2)) {
        SubmitCmdList(id - reg::kCommandBufferTrigger);
    }
}

void CommandProcessor::SetRegisterWriteLog(bool enabled) {
    if (enabled && !log_writes_) {
        writes_taken_ = false;
    }
    log_writes_ = enabled;
}

std::vector<gpu::RegisterWrite> CommandProcessor::TakeRegisterWrites() {
    writes_taken_ = true;
    std::vector<gpu::RegisterWrite> result;
    result.swap(writes_);
    return result;
}

void CommandProcessor::LogWriteSlow(u32 id, u32 mask, u32 value) {
    // Claude fix 2026-11-19 (a 1-2 GB memory leak found in a village run): the log is for whoever takes it (the capture checks);
    // when nobody has taken it and it reaches a million entries, it is turned off and freed, as production does.
    if (!writes_taken_ && writes_.size() >= (1u << 20)) {
        log_writes_ = false;
        std::vector<gpu::RegisterWrite>().swap(writes_);
        return;
    }
    writes_.push_back({static_cast<u16>(id), static_cast<u8>(mask), value});
}

void CommandProcessor::WriteInternalReg(u32 id, u32 value, u32 mask, bool& stop_requested) {
    if (id >= kNumRegs) {
        return;
    }
    LogWrite(id, mask, value);
    const u32 byte_mask = ExpandMask(mask);
    u32& stored = state_->regs[id];
    stored = (stored & ~byte_mask) | (value & byte_mask);
    if (IsSpecialRegister(id)) {
        // Claude fix 2026-11-19 (differential fuzz vs production): the handlers see the register as stored after the byte mask.
        // (the data ports of the tables and shader memories take the written word as it is; the other handlers, e.g. the
        // bool and int uniforms, see the merged register)
        HandleSpecialReg(id, value, stop_requested);
    }
    state_->dirty.Set(id);
}

void CommandProcessor::WriteInternalRegBatch(u32 id, std::span<const u32> values, u32 mask,
                                             bool& stop_requested) {
    if (id >= kNumRegs) {
        return;
    }
    if (IsBatchRegister(id)) {
        for (const u32 word : values) {
            LogWrite(id, mask, word);
        }
        const u32 byte_mask = ExpandMask(mask);
        u32& stored = state_->regs[id];
        stored = (stored & ~byte_mask) | (values.back() & byte_mask);
        HandleSpecialRegBatch(id, values);
        state_->dirty.Set(id);
        return;
    }
    for (const u32 word : values) {
        WriteInternalReg(id, word, mask, stop_requested);
        if (stop_requested || list_.index == 0) {
            return;
        }
    }
}

void CommandProcessor::WriteInternalRegSequential(u32 id, std::span<const u32> values, u32 mask,
                                                  bool& stop_requested) {
    for (const u32 word : values) {
        WriteInternalReg(id++, word, mask, stop_requested);
        if (stop_requested || list_.index == 0) {
            return;
        }
    }
}

void CommandProcessor::HandleSpecialReg(u32 id, u32 value, bool& stop_requested) {
    auto& s = *state_;
    auto& regs = s.regs;
    if (IsBatchRegister(id)) {
        HandleSpecialRegBatch(id, {&value, 1});
        return;
    }

    switch (id) {
    case reg::kIrqRequest:
        for (u32 byte = 0; byte < 4; ++byte) {
            if (BitsOf(regs[reg::kIrqMask], byte, 1) == 0 &&
                BitsOf(value, 8 * byte, 8) == BitsOf(regs[reg::kIrqCompare], 8 * byte, 8)) {
                host_.SignalInterrupt(Interrupt::P3D, delay_.CalculateAndResetDelay());
                stop_requested = regs[reg::kIrqAutostop] != 0;
                break;
            }
        }
        return;
    case reg::kFixedAttribIndex:
        s.immediate.Reset();
        return;
    case reg::kTriangleTopology:
    case reg::kRestartPrimitive:
        // The draw backend sees these writes through the dirty register bits.
        return;
    case reg::kGasLutIndex:
        s.gas.index = BitsOf(regs[id], 0, 16) % Gas::kEntries;
        return;
    case reg::kGasLutData:
        s.gas.lut[s.gas.index] = value;
        s.gas.index = (s.gas.index + 1) % Gas::kEntries;
        s.gas.table_dirty = true;
        ++gas_lut_writes_;
        return;
    case reg::kTriggerDraw:
    case reg::kTriggerDrawIndexed:
        DrawArrays(id == reg::kTriggerDrawIndexed);
        return;
    case reg::kCommandBufferTrigger:
    case reg::kCommandBufferTrigger + 1: {
        const u32 channel = id - reg::kCommandBufferTrigger;
        ResetList(ChannelAddress(s, channel), ChannelSize(s, channel));
        return;
    }
    case reg::kVs + reg::kShaderOutputMask:
        if (GsMirrorsVs()) {
            const u32 output = reg::kGs + reg::kShaderOutputMask;
            regs[output] = WithBits(regs[output], 0, 16, value);  // the written word, not the merged register (differential fuzz)
            s.dirty.Set(output);
        }
        return;
    default:
        break;
    }

    const bool vs = id >= reg::kVs;
    const u32 offset = id - (vs ? reg::kVs : reg::kGs);
    auto write = [&](ShaderSetup& setup) {
        if (offset == reg::kShaderBoolUniforms) {
            setup.WriteUniformBoolReg(regs[id]);  // the merged register (differential fuzz vs production)
        } else if (InRange(offset, reg::kShaderIntUniforms, 4)) {
            std::array<u8, 4> bytes{};
            for (u32 i = 0; i < bytes.size(); ++i) {
                bytes[i] = static_cast<u8>(regs[id] >> (i * 8));
            }
            setup.WriteUniformIntReg(offset - reg::kShaderIntUniforms, bytes);
        }
    };
    if (id >= reg::kGs) {
        write(vs ? s.vs : s.gs);
        if (vs && GsMirrorsVs()) {
            write(s.gs);
        }
    }
}

void CommandProcessor::HandleSpecialRegBatch(u32 id, std::span<const u32> values) {
    auto& s = *state_;
    auto& regs = s.regs;
    const u32 count = static_cast<u32>(values.size());
    if (InRange(id, reg::kFixedAttribData, 3)) {
        for (const u32 word : values) {
            SubmitImmediate(word);
        }
        return;
    }
    if (InRange(id, reg::kLightingLutData, 8)) {
        u32& config = regs[reg::kLightingLutConfig];
        const u32 type = BitsOf(config, 8, 5);
        const u32 index = BitsOf(config, 0, 8);
        if (type < Lighting::kTables) {
            UpdateTable(s.lighting.luts[type], index, values,
                        [&] { s.lighting.lut_dirty |= 1u << type; });
        }
        config = WithBits(config, 0, 8, index + count);
        return;
    }
    if (InRange(id, reg::kFogLutData, 8)) {
        u32& config = regs[reg::kFogLutOffset];
        const u32 index = BitsOf(config, 0, 16);
        UpdateTable(s.fog.lut, index, values, [&] { s.fog.lut_dirty = true; });
        config = WithBits(config, 0, 16, index + count);
        return;
    }
    if (InRange(id, reg::kProcTexLutData, 8)) {
        u32& config = regs[reg::kProcTexLutConfig];
        const u32 type = BitsOf(config, 8, 4);
        const u32 index = BitsOf(config, 0, 8);
        const auto table = ProcTable(s.proctex, type);
        if (!table.empty()) {
            UpdateTable(table, index, values, [&] {
                s.proctex.table_dirty |= static_cast<u8>(1u << type);
            });
        }
        config = WithBits(config, 0, 8, index + count);
        return;
    }

    const bool vs = id >= reg::kVs;
    const bool mirror = vs && GsMirrorsVs();
    const u32 base = vs ? reg::kVs : reg::kGs;
    auto& shader = vs ? s.vs : s.gs;
    const u32 port = id - base;
    if (InRange(port, reg::kShaderUniformData, 8)) {
        u32& config = regs[base + reg::kShaderUniformIndex];
        const auto range = shader.WriteUniformFloatRegRange(config, values);
        if (mirror && range) {
            for (u32 i = range->first_index; i < range->first_index + range->count; ++i) {
                if (s.gs.uniform_f[i] != shader.uniform_f[i]) {
                    s.gs.uniform_f[i] = shader.uniform_f[i];
                    // (experiment 2026-11-19) mirrored float uniforms do not set the dirty flag in production
                    s.gs.uniforms_serial = ShaderSetup::NextSerial();
                }
            }
        }
        return;
    }

    const bool program = InRange(port, reg::kShaderProgramData, 8);
    u32& cursor = regs[base + (program ? reg::kShaderProgramOffset : reg::kShaderSwizzleOffset)];
    const u32 limit = program ? (vs ? 512 : kMaxProgramWords) : kMaxSwizzleWords;
    if (cursor >= limit || count > limit - cursor) {
        return;
    }
    const auto update = program ? &ShaderSetup::UpdateProgramCodeRange :
                                  &ShaderSetup::UpdateSwizzleDataRange;
    (shader.*update)(cursor, values);
    if (mirror) {
        (s.gs.*update)(cursor, values);
    }
    cursor += count;
}

bool CommandProcessor::GsMirrorsVs() const {
    const auto& regs = state_->regs;
    return BitsOf(regs[reg::kGsUnitExclusiveConfig], 0, 1) == 0 &&
           BitsOf(regs[reg::kGeometryStage], 0, 2) == kUseGsNo;
}

void CommandProcessor::SubmitImmediate(u32 word) {
    auto& s = *state_;
    u32& index = s.regs[reg::kFixedAttribIndex];
    if (!s.immediate.queue.Push(word)) {
        return;
    }
    const Vec4f attribute = s.immediate.queue.Get();
    if (index < 15) {
        s.default_attributes[index++] = attribute;
    } else if (index == 15) {
        auto& current = s.immediate.current_attribute;
        const u32 last = BitsOf(s.regs[reg::kMaxInputAttribIndex], 0, 4);
        if (current > last) {
            current = 0;
        }
        s.immediate.input_vertex[current++] = attribute;
        if (current > last) {
            current = 0;
            DrawImmediate();
        }
    }
}

void CommandProcessor::DrawImmediate() {
    auto& s = *state_;
    DrawCall draw{.kind = DrawKind::Immediate, .num_vertices = 1,
                  .geometry_reconfigured = s.immediate.reset_geometry_pipeline};
    if (draw.geometry_reconfigured) {
        ReconfigureGeometryPipeline();
        s.immediate.reset_geometry_pipeline = false;
    }
    IssueDraw(draw);
}

void CommandProcessor::DrawArrays(bool is_indexed) {
    const auto& regs = state_->regs;
    ReconfigureGeometryPipeline();
    DrawCall draw{
        .kind = is_indexed ? DrawKind::Indexed : DrawKind::Arrays,
        .vertex_base_address = BitsOf(regs[reg::kVertexAttributesBase], 1, 28) << 4,
        .num_vertices = regs[reg::kNumVertices],
        .vertex_offset = is_indexed ? 0 : regs[reg::kVertexOffset],
        .index_offset = is_indexed ? BitsOf(regs[reg::kIndexArray], 0, 28) : 0,
        .index_u16 = is_indexed && BitsOf(regs[reg::kIndexArray], 31, 1) != 0,
        .geometry_reconfigured = true,
    };
    IssueDraw(draw);
}

void CommandProcessor::ReconfigureGeometryPipeline() {
    auto& s = *state_;
    const auto& regs = s.regs;
    auto& g = s.geometry;
    g = {};
    // Claude fix 2026-11-19 (differential fuzz vs production, golden/picafuzz): any non-zero use_gs value reconfigures the
    // pipeline, and the GS unit's input-buffer mode is not consulted (production does the same for every register set).
    if (BitsOf(regs[reg::kGeometryStage], 0, 2) == 0) {
        return;
    }

    const u32 config = regs[reg::kGsConfig];
    const u32 input = regs[reg::kGs + reg::kShaderInputBufferConfig];
    g.vs_output_num = BitsOf(regs[reg::kVsOutmapTotalA], 0, 4) + 1;
    switch (BitsOf(config, 0, 8)) {
    case kGsModePoint:
        g.mode = GeometryMode::Point;
        g.buffer_size = BitsOf(input, 0, 4) + 1;
        break;
    case kGsModeVariablePrimitive:
        g.mode = GeometryMode::VariablePrimitive;
        g.need_index = true;
        break;
    case kGsModeFixedPrimitive:
        g.mode = GeometryMode::FixedPrimitive;
        g.buffer_offset = BitsOf(config, 16, 8);
        g.buffer_index = g.buffer_offset;
        // Claude fix 2026-11-19 (differential fuzz): the end of the buffer is vertices x vs outputs, not vertices x stride.
        g.buffer_size = g.buffer_offset + (BitsOf(config, 8, 4) + 1) * g.vs_output_num;
        break;
    default:
        g = {};
        return;
    }
    s.gs.entry_point = BitsOf(regs[reg::kGs + reg::kShaderMainOffset], 0, 16);
}

void CommandProcessor::IssueDraw(DrawCall& call) {
    auto& s = *state_;
    s.vs.entry_point = BitsOf(s.regs[reg::kVs + reg::kShaderMainOffset], 0, 16);
    call.sequence = sequence_++;
    call.state = &s;
    delay_.AddVertices(call.num_vertices, BitsOf(s.regs[reg::kTriangleTopology], 8, 2));
    backend_.Draw(call);
    s.ClearDirty();
}

} // namespace mh4n::gpu::pica
