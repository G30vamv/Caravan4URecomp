// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: WP71-pica-vs-interp, cpuvertex contracts, gpu_cpuvertex tests and 3dbrew GPU/Shader Instruction Set.

#include "shader_interpreter.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <vector>

namespace mh4n::gpu::cpuvertex {
namespace {

using isa::CompareOp;
using isa::FlowOp;
using isa::Instruction;
using isa::Opcode;
using isa::SwizzlePattern;

std::atomic<u64> checked_runs{0};
std::atomic<u64> checked_differences{0};

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

f24 Bit(bool value) {
    return value ? f24::One() : f24::Zero();
}

const Vec4& Read(const ShaderSetup& setup, const ShaderUnit& unit, isa::SourceRegister reg,
                 u32 address = 0) {
    const u32 index = static_cast<u32>(reg.GetIndex());
    switch (reg.GetRegisterType()) {
    case isa::RegisterType::Input:
        return unit.input[index];
    case isa::RegisterType::Temporary:
        return unit.temporary[index];
    case isa::RegisterType::FloatUniform:
        break;
    }

    const s32 offset = address ? unit.address_registers[address - 1] : 0;
    const u32 relative = (index + static_cast<u32>(offset >= -128 && offset <= 127 ? offset : 0)) & 127;
    static const Vec4 ones{{f24::One(), f24::One(), f24::One(), f24::One()}};
    return relative < setup.uniforms->f.size() ? setup.uniforms->f[relative] : ones;
}

template <auto Selector, auto Negate>
Vec4 Swizzle(const Vec4& value, SwizzlePattern pattern) {
    Vec4 result;
    const bool negate = (pattern.*Negate)();
    for (u32 lane = 0; lane < 4; ++lane) {
        const f24 component = value.c[(pattern.*Selector)(lane)];
        result.c[lane] = negate ? -component : component;
    }
    return result;
}

s32 ToAddress(float value) {
    if (value >= -2147483648.0f && value < 2147483648.0f) {
        return static_cast<s32>(value);
    }
    return std::numeric_limits<s32>::min();
}

bool Test(Instruction instruction, const ShaderUnit& unit) {
    const bool x = unit.conditional_code[0] == instruction.flow_refx();
    const bool y = unit.conditional_code[1] == instruction.flow_refy();
    switch (instruction.flow_op()) {
    case FlowOp::Or: return x || y;
    case FlowOp::And: return x && y;
    case FlowOp::JustX: return x;
    case FlowOp::JustY: return y;
    }
    return false;
}

bool Compare(CompareOp op, f24 a, f24 b) {
    switch (op) {
    case CompareOp::Equal: return a == b;
    case CompareOp::NotEqual: return a != b;
    case CompareOp::LessThan: return a < b;
    case CompareOp::LessEqual: return a <= b;
    case CompareOp::GreaterThan: return a > b;
    case CompareOp::GreaterEqual: return a >= b;
    default: return false;
    }
}

void Write(ShaderUnit& unit, isa::DestRegister reg, SwizzlePattern pattern, const Vec4& value) {
    const u32 index = static_cast<u32>(reg.GetIndex());
    auto& destination = reg.IsOutput() ? unit.output[unit.output_bank][index] : unit.temporary[index];
    for (u32 lane = 0; lane < 4; ++lane) {
        if (pattern.DestComponentEnabled(lane)) {
            destination.c[lane] = value.c[lane];
        }
    }
}

f24 LaneOperation(Opcode op, f24 a, f24 b) {
    switch (op) {
    case Opcode::ADD: return a + b;
    case Opcode::MUL: return a * b;
    case Opcode::SGE:
    case Opcode::SGEI: return Bit(a >= b);
    case Opcode::SLT:
    case Opcode::SLTI: return Bit(a < b);
    case Opcode::FLR: return f24::FromFloat32(std::floor(a.ToFloat32()));
    case Opcode::MAX: return a > b ? a : b;
    case Opcode::MIN: return a < b ? a : b;
    default: return a; // MOV
    }
}

float Exp2(float value) { return std::exp2(value); }
float Log2(float value) { return std::log2(value); }

Unhandled ExecuteAlu(const ShaderSetup& setup, ShaderUnit& unit, Instruction instruction) {
    const Opcode op = instruction.EffectiveOpCode();
    const bool inverted = instruction.GetInfo().src_inversed;
    const bool mad = instruction.GetInfo().type == isa::Type::MultiplyAdd;
    const SwizzlePattern pattern{
        setup.swizzle_data[mad ? instruction.mad_operand_desc_id() : instruction.common_operand_desc_id()]};
    const u32 index = mad ? instruction.mad_address_register_index() : instruction.common_address_register_index();
    const auto read1 = [&](isa::SourceRegister reg, u32 address) {
        return Swizzle<&SwizzlePattern::selector_src1, &SwizzlePattern::negate_src1>(
            Read(setup, unit, reg, address), pattern);
    };
    const auto read2 = [&](isa::SourceRegister reg, u32 address) {
        return Swizzle<&SwizzlePattern::selector_src2, &SwizzlePattern::negate_src2>(
            Read(setup, unit, reg, address), pattern);
    };

    Vec4 result;
    if (mad) {
        const Vec4 a = read1(instruction.mad_src1(inverted), 0);
        const Vec4 b = read2(instruction.mad_src2(inverted), inverted ? 0 : index);
        const Vec4 c = Swizzle<&SwizzlePattern::selector_src3, &SwizzlePattern::negate_src3>(
            Read(setup, unit, instruction.mad_src3(inverted), inverted ? index : 0), pattern);
        for (u32 lane = 0; lane < 4; ++lane) {
            result.c[lane] = a.c[lane] * b.c[lane] + c.c[lane];
        }
        Write(unit, instruction.mad_dest(), pattern, result);
        return Unhandled::None;
    }

    const Vec4 a = read1(instruction.common_src1(inverted), inverted ? 0 : index);
    const Vec4 b = read2(instruction.common_src2(inverted), inverted ? index : 0);
    f24 scalar;
    switch (op) {
    case Opcode::DP3:
    case Opcode::DP4:
    case Opcode::DPH:
    case Opcode::DPHI:
        for (u32 lane = 0; lane < 3; ++lane) {
            scalar = scalar + a.c[lane] * b.c[lane];
        }
        if (op != Opcode::DP3) {
            scalar = scalar + (op == Opcode::DP4 ? a.c[3] : f24::One()) * b.c[3];
        }
        break;
    case Opcode::EX2: scalar = f24::FromFloat32(Exp2(a.c[0].ToFloat32())); break;
    case Opcode::LG2: scalar = f24::FromFloat32(Log2(a.c[0].ToFloat32())); break;
    case Opcode::RCP: scalar = f24::One() / a.c[0]; break;
    case Opcode::RSQ: scalar = f24::One() / f24::FromFloat32(std::sqrt(a.c[0].ToFloat32())); break;
    case Opcode::MOVA:
        for (u32 lane = 0; lane < 2; ++lane) {
            if (pattern.DestComponentEnabled(lane)) {
                unit.address_registers[lane] = ToAddress(a.c[lane].ToFloat32());
            }
        }
        return Unhandled::None;
    case Opcode::CMP: {
        const CompareOp x = instruction.common_compare_x();
        const CompareOp y = instruction.common_compare_y();
        if (x > CompareOp::GreaterEqual || y > CompareOp::GreaterEqual) {
            return Unhandled::CompareMode;
        }
        unit.conditional_code[0] = Compare(x, a.c[0], b.c[0]);
        unit.conditional_code[1] = Compare(y, a.c[1], b.c[1]);
        return Unhandled::None;
    }
    case Opcode::ADD:
    case Opcode::MUL:
    case Opcode::SGE:
    case Opcode::SGEI:
    case Opcode::SLT:
    case Opcode::SLTI:
    case Opcode::FLR:
    case Opcode::MAX:
    case Opcode::MIN:
    case Opcode::MOV:
        for (u32 lane = 0; lane < 4; ++lane) {
            result.c[lane] = LaneOperation(op, a.c[lane], b.c[lane]);
        }
        Write(unit, instruction.common_dest(), pattern, result);
        return Unhandled::None;
    default:
        return Unhandled::Instruction;
    }

    std::fill_n(result.c, 4, scalar);
    Write(unit, instruction.common_dest(), pattern, result);
    return Unhandled::None;
}

template <typename Frame, size_t Capacity>
struct FlowStack {
    std::array<Frame, Capacity> frames{};
    size_t size = 0;

    Frame& Top() { return frames[size - 1]; }
    void Pop() { --size; }
    void Push(Frame frame) {
        if (size == Capacity) {
            for (size_t i = 1; i < Capacity; ++i) {
                frames[i - 1] = frames[i];
            }
            --size;
        }
        frames[size++] = frame;
    }
};

struct FlowFrame {
    u32 end;
    u32 resume;
};

struct LoopFrame {
    u32 end;
    u32 start;
    u32 remaining;
    s32 increment;
    s32 initial_address;
};

template <size_t Count>
struct NativeVectors {
    float values[Count][4];
};

struct NativeOutputs {
    float values[2][16][4];
};

struct NativeIntegers {
    unsigned char values[4][4];
};

struct NativeCall {
    GeometryEmitter* emitter;
    NativeOutputs output;
    unsigned char bank;
    Unhandled failure = Unhandled::Instruction;

    static int Emit(void* context) {
        auto& call = *static_cast<NativeCall*>(context);
        auto* emitter = call.emitter;
        if (!emitter || emitter->emit_state.vertex_id >= emitter->buffer.size()) {
            call.failure = emitter ? Unhandled::EmitVertexId : Unhandled::EmitOnVertexShader;
            return 0;
        }
        emitter->Emit(std::bit_cast<AttributeBuffer>(call.output.values[call.bank != 0]));
        return 1;
    }

    static int SetEmit(void* context, unsigned id, unsigned primitive, unsigned winding) {
        auto& call = *static_cast<NativeCall*>(context);
        if (!call.emitter) {
            call.failure = Unhandled::EmitOnVertexShader;
            return 0;
        }
        call.emitter->emit_state = {winding != 0, primitive != 0, static_cast<u8>(id)};
        return 1;
    }
};

struct Emission {
    bool winding = false;
    AttributeBuffer output{};
};

class RecordingSink final : public VertexSink {
public:
    explicit RecordingSink(VertexSink* sink = nullptr) : sink_(sink) {}

    void SubmitVertex(const AttributeBuffer& output) override {
        events.push_back({false, output});
        if (sink_) {
            sink_->SubmitVertex(output);
        }
    }

    void SetWinding() override {
        events.push_back({true, {}});
        if (sink_) {
            sink_->SetWinding();
        }
    }

    std::vector<Emission> events;

private:
    VertexSink* sink_;
};

template <typename T>
bool SameVectorBits(const T& a, const T& b) {
    return std::memcmp(&a, &b, sizeof(T)) == 0;
}

bool SameUnit(const ShaderUnit& a, const ShaderUnit& b) {
    if (a.output_bank != b.output_bank ||
        !std::equal(std::begin(a.address_registers), std::end(a.address_registers), b.address_registers) ||
        !std::equal(std::begin(a.conditional_code), std::end(a.conditional_code), b.conditional_code) ||
        !SameVectorBits(a.input, b.input) || !SameVectorBits(a.temporary, b.temporary) ||
        !SameVectorBits(a.output, b.output)) {
        return false;
    }
    if (!a.emitter_ptr) {
        return true;
    }

    const auto& x = *a.emitter_ptr;
    const auto& y = *b.emitter_ptr;
    return x.emit_state.winding == y.emit_state.winding &&
           x.emit_state.prim_emit == y.emit_state.prim_emit &&
           x.emit_state.vertex_id == y.emit_state.vertex_id &&
           SameVectorBits(x.buffer, y.buffer);
}

} // namespace

void ShaderUnit::LoadInput(const ShaderInputConfig& config, const AttributeBuffer& attributes) {
    for (u32 i = 0; i <= config.max_input_attribute_index && i < input.size(); ++i) {
        input[config.GetRegisterForAttribute(i)] = attributes[i];
    }
}

void ShaderUnit::WriteOutput(u32 output_mask, AttributeBuffer& buffer) const {
    u32 next = 0;
    for (u32 i = 0; i < output[0].size(); ++i) {
        if (output_mask & (1u << i)) {
            buffer[next++] = output[output_bank][i];
        }
    }
}

void GeometryEmitter::Emit(std::span<const Vec4, 16> output_regs) {
    if (emit_state.vertex_id >= buffer.size()) {
        return;
    }
    auto& vertex = buffer[emit_state.vertex_id];
    u32 next = 0;
    for (u32 i = 0; i < output_regs.size(); ++i) {
        if (output_mask & (1u << i)) {
            vertex[next++] = output_regs[i];
        }
    }

    if (emit_state.prim_emit && sink) {
        if (emit_state.winding) {
            sink->SetWinding();
        }
        for (const auto& buffered : buffer) {
            sink->SubmitVertex(buffered);
        }
    }
}

void GeometryShaderUnit::Reset() {
    VertexSink* sink = emitter.sink;
    emitter = {};
    emitter.sink = sink;
    static_cast<ShaderUnit&>(*this) = ShaderUnit(&emitter);
}

NativeCheckCounts GetNativeCheckCounts() {
    return {checked_runs.load(std::memory_order_relaxed), checked_differences.load(std::memory_order_relaxed)};
}

bool InterpreterEngine::Run(const ShaderSetup& setup, ShaderUnit& unit, RunError& error) const {
    error = {};
    if (setup.entry_point >= kMaxProgramCodeLength) {
        error = {Unhandled::EntryPoint, setup.entry_point, 0};
        return false;
    }
    if (!setup.native) {
        return Interpret(setup, unit, error);
    }
    return setup.check_native ? RunChecked(setup, unit, error) : RunNative(setup, unit, error);
}

bool InterpreterEngine::Interpret(const ShaderSetup& setup, ShaderUnit& unit, RunError& error) const {
    FlowStack<FlowFrame, 4> calls;
    FlowStack<FlowFrame, 8> branches;
    FlowStack<LoopFrame, 4> loops;
    u32 pc = setup.entry_point;

    for (u32 step = 0; step < kMaxInstructions; ++step) {
        if (pc >= kMaxProgramCodeLength) {
            error = {Unhandled::EntryPoint, pc, 0};
            return false;
        }
        if (pc == kMaxProgramCodeLength - 1) {
            return true;
        }

        const Instruction instruction{setup.program_code[pc]};
        const Opcode op = instruction.EffectiveOpCode();
        const u32 next = pc + 1;
        u32 target = next;
        bool break_loop = false;
        Unhandled failure = Unhandled::None;
        const auto& info = instruction.GetInfo();
        if (info.type == isa::Type::Arithmetic || info.type == isa::Type::MultiplyAdd) {
            failure = ExecuteAlu(setup, unit, instruction);
        } else {
            const u32 dest = instruction.flow_dest_offset();
            const u32 length = instruction.flow_num_instructions();
            const u32 uniform = instruction.flow_bool_uniform_id();
            switch (op) {
            case Opcode::END: return true;
            case Opcode::NOP: break;
            case Opcode::BREAK:
            case Opcode::BREAKC:
                break_loop = op == Opcode::BREAK || Test(instruction, unit);
                break;
            case Opcode::CALL:
            case Opcode::CALLC:
            case Opcode::CALLU:
                if (length && (op == Opcode::CALL ||
                               (op == Opcode::CALLC ? Test(instruction, unit) : setup.uniforms->b[uniform]))) {
                    calls.Push({dest + length, next});
                    target = dest;
                }
                break;
            case Opcode::IFC:
            case Opcode::IFU:
                if (op == Opcode::IFC ? Test(instruction, unit) : setup.uniforms->b[uniform]) {
                    branches.Push({dest, dest + length});
                } else {
                    target = dest;
                }
                break;
            case Opcode::LOOP: {
                const auto& parameters = setup.uniforms->i[instruction.flow_int_uniform_id()];
                unit.address_registers[2] = parameters[1];
                // The renderer restores the inner loop's start value on return to an outer loop.
                loops.Push({dest + 1, next, 1u + parameters[0], parameters[2], parameters[1]});
                break;
            }
            case Opcode::JMPC:
                if (Test(instruction, unit)) {
                    target = dest;
                }
                break;
            case Opcode::JMPU:
                if (setup.uniforms->b[uniform] != ((length & 1) != 0)) {
                    target = dest;
                }
                break;
            case Opcode::EMIT:
                if (!unit.emitter_ptr) {
                    failure = Unhandled::EmitOnVertexShader;
                } else if (unit.emitter_ptr->emit_state.vertex_id >= unit.emitter_ptr->buffer.size()) {
                    failure = Unhandled::EmitVertexId;
                } else {
                    unit.emitter_ptr->Emit(unit.output[unit.output_bank]);
                    unit.output_bank = !unit.output_bank;
                }
                break;
            case Opcode::SETEMIT:
                if (!unit.emitter_ptr) {
                    failure = Unhandled::EmitOnVertexShader;
                } else {
                    unit.emitter_ptr->emit_state = {instruction.setemit_winding(), instruction.setemit_prim_emit(),
                                                    static_cast<u8>(instruction.setemit_vertex_id())};
                }
                break;
            default:
                failure = Unhandled::Instruction;
                break;
            }
        }
        if (failure != Unhandled::None || (break_loop && !loops.size)) {
            error = {failure == Unhandled::None ? Unhandled::Instruction : failure, pc, instruction.hex};
            return false;
        }

        // All scopes inspect the sequential PC. A matching loop overrides a branch or call.
        u32 returned = next;
        for (u32 popped = 0; calls.size && calls.Top().end == returned; ++popped) {
            if (popped < 3) {
                returned = calls.Top().resume;
            }
            calls.Pop();
            target = returned;
        }
        if (branches.size && branches.Top().end == next) {
            target = branches.Top().resume;
            branches.Pop();
        }
        if (loops.size && (break_loop || loops.Top().end == next)) {
            auto& loop = loops.Top();
            unit.address_registers[2] += loop.increment;
            if (break_loop || --loop.remaining == 0) {
                target = loop.end;
                if (loops.size > 1) {
                    unit.address_registers[2] = loop.initial_address;
                }
                loops.Pop();
            } else {
                target = loop.start;
            }
        }
        pc = target;
    }

    error = {Unhandled::InstructionBudget, pc, pc < kMaxProgramCodeLength ? setup.program_code[pc] : 0};
    return false;
}

bool InterpreterEngine::RunNative(const ShaderSetup& setup, ShaderUnit& unit, RunError& error) const {
    static_assert(sizeof(bool) == sizeof(unsigned char));
    auto input = std::bit_cast<NativeVectors<16>>(unit.input);
    auto temporary = std::bit_cast<NativeVectors<16>>(unit.temporary);
    const auto uniform_f = std::bit_cast<NativeVectors<96>>(setup.uniforms->f);
    const auto uniform_i = std::bit_cast<NativeIntegers>(setup.uniforms->i);
    NativeCall call{unit.emitter_ptr, std::bit_cast<NativeOutputs>(unit.output),
                    static_cast<unsigned char>(unit.output_bank)};
    PicaNativeState state{
        input.values,
        temporary.values,
        call.output.values,
        &call.bank,
        unit.address_registers,
        reinterpret_cast<unsigned char*>(unit.conditional_code),
        uniform_f.values,
        reinterpret_cast<const unsigned char*>(setup.uniforms->b.data()),
        uniform_i.values,
        NativeCall::Emit,
        NativeCall::SetEmit,
        Exp2,
        Log2,
        &call,
    };
    const int result = setup.native(&state);
    unit.input = std::bit_cast<AttributeBuffer>(input);
    unit.temporary = std::bit_cast<AttributeBuffer>(temporary);
    unit.output = std::bit_cast<decltype(unit.output)>(call.output);
    unit.output_bank = call.bank != 0;
    if (result == 1) {
        return true;
    }
    const u32 pc = result < 0 ? static_cast<u32>(-(result + 1)) : setup.entry_point;
    error = {call.failure, pc, pc < kMaxProgramCodeLength ? setup.program_code[pc] : 0};
    return false;
}

bool InterpreterEngine::RunChecked(const ShaderSetup& setup, ShaderUnit& unit, RunError& error) const {
    ShaderUnit native_unit = unit;
    GeometryEmitter native_emitter;
    GeometryEmitter* emitter = unit.emitter_ptr;
    VertexSink* sink = emitter ? emitter->sink : nullptr;
    RecordingSink interpreted_events(sink);
    RecordingSink native_events;
    if (emitter) {
        native_emitter = *emitter;
        native_emitter.sink = &native_events;
        native_unit.emitter_ptr = &native_emitter;
        emitter->sink = &interpreted_events;
    }
    const bool interpreted_ok = Interpret(setup, unit, error);
    if (emitter) {
        emitter->sink = sink;
    }
    RunError native_error;
    const bool native_ok = RunNative(setup, native_unit, native_error);
    const bool same_events = std::equal(
        interpreted_events.events.begin(), interpreted_events.events.end(),
        native_events.events.begin(), native_events.events.end(),
        [](const Emission& a, const Emission& b) {
            return a.winding == b.winding && SameVectorBits(a.output, b.output);
        });
    checked_runs.fetch_add(1, std::memory_order_relaxed);
    if (interpreted_ok != native_ok || error.kind != native_error.kind ||
        error.program_counter != native_error.program_counter || error.instruction != native_error.instruction ||
        !SameUnit(unit, native_unit) || !same_events) {
        checked_differences.fetch_add(1, std::memory_order_relaxed);
    }
    return interpreted_ok;
}

} // namespace mh4n::gpu::cpuvertex
