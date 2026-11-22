// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: 3dbrew GPU/Shader Instruction Set, shader contract headers, gpu_shader_test.cpp test cases and golden/gpu.

#include "glsl_decompiler.h"

#include <algorithm>
#include <array>
#include <deque>
#include <map>
#include <string_view>
#include <utility>

#include "pica_isa.h"

namespace mh4n::gpu::shader {
namespace {

using isa::Instruction;
using isa::Opcode;

constexpr std::string_view kLanes = "xyzw";
constexpr int kNone = -1;

constexpr std::string_view kPrelude = R"glsl(
// MAX and MIN as the PICA does them (src1 > src2 ? src1 : src2): a NaN second operand stays; the branch keeps drivers from using fmax/fmin,
// which return the other operand
float mh4n_fmax(float a, float b) { if (b != b) return b; return a > b ? a : b; }
float mh4n_fmin(float a, float b) { if (b != b) return b; return a < b ? a : b; }
vec4 mh4n_max(vec4 a, vec4 b) { return vec4(mh4n_fmax(a.x, b.x), mh4n_fmax(a.y, b.y), mh4n_fmax(a.z, b.z), mh4n_fmax(a.w, b.w)); }
vec4 mh4n_min(vec4 a, vec4 b) { return vec4(mh4n_fmin(a.x, b.x), mh4n_fmin(a.y, b.y), mh4n_fmin(a.z, b.z), mh4n_fmin(a.w, b.w)); }
vec4 pica_read(int base, int offset) {
    int index = (base + (offset >= -128 && offset <= 127 ? offset : 0)) & 127;
    return index < 96 ? uniforms.f[index] : vec4(1.0);
}

vec4 pica_mul(vec4 left, vec4 right) {
    uvec4 a = floatBitsToUint(left) & uvec4(0x7fffffffu);
    uvec4 b = floatBitsToUint(right) & uvec4(0x7fffffffu);
    bvec4 zero = bvec4((a.x == 0u && b.x == 0x7f800000u) || (b.x == 0u && a.x == 0x7f800000u),
                      (a.y == 0u && b.y == 0x7f800000u) || (b.y == 0u && a.y == 0x7f800000u),
                      (a.z == 0u && b.z == 0x7f800000u) || (b.z == 0u && a.z == 0x7f800000u),
                      (a.w == 0u && b.w == 0x7f800000u) || (b.w == 0u && a.w == 0x7f800000u));
    precise vec4 product = left * right;
    return mix(product, vec4(0.0), zero);
}

int pica_address(float value) {
    // Only signed-byte offsets affect relative reads.
    return value > -129.0 && value < 128.0 ? int(value) : 0;
}

)glsl";

// For a normalized binary32 significand m, the reciprocal significand is 2^47 / m.
// A square root uses sqrt(m * 2^(23 + parity)), with parity taken from the unbiased exponent.
// Compare exact integer midpoints to round both operations without a float64 capability.
constexpr std::string_view kExactScalars = R"glsl(
uvec2 pica_product(uint a, uint b) {
    uvec2 product;
    umulExtended(a, b, product.y, product.x);
    return product;
}

bool pica_less(uvec2 a, uvec2 b) {
    return a.y == b.y ? a.x < b.x : a.y < b.y;
}

bool pica_round_up(uint m, uvec2 target, uint lower, bool root) {
    // For sqrt, compare 4*m*2^(23+parity) with (2*lower+1)^2.
    // For division, compare twice the numerator with m*(2*lower+1).
    uint midpoint = 2u * lower + 1u;
    uvec2 limit = pica_product(midpoint, root ? midpoint : m);
    return pica_less(limit, target) || (all(equal(limit, target)) && (lower & 1u) != 0u);
}

uint pica_nearest(uint m, uvec2 target, uint guess, bool root) {
    uint lo = max(guess, 4u) - 4u;
    uint hi = guess + 4u;
    if (pica_round_up(m, target, hi, root) ||
        (lo > 0u && !pica_round_up(m, target, lo - 1u, root))) {
        lo = 0u;
        hi = 0x1000000u;
    }
    while (lo < hi) {
        uint mid = (lo + hi) / 2u;
        if (pica_round_up(m, target, mid, root)) lo = mid + 1u;
        else hi = mid;
    }
    return lo;
}

float pica_rcp(float value) {
    uint bits = floatBitsToUint(value);
    uint sign_bit = bits & 0x80000000u;
    uint magnitude = bits ^ sign_bit;
    uint exponent = magnitude >> 23;
    if (magnitude == 0u) return uintBitsToFloat(sign_bit | 0x7f800000u);
    if (exponent == 255u) return magnitude == 0x7f800000u ? uintBitsToFloat(sign_bit) : value;
    uint m = (magnitude & 0x7fffffu) | (exponent == 0u ? 0u : 0x800000u);
    int e = int(exponent);
    if (exponent == 0u) {
        int shift = 23 - findMSB(m);
        m <<= shift;
        e = 1 - shift;
    }
    if (e < -1) return uintBitsToFloat(sign_bit | 0x7f800000u);
    int shift = max(e - 252, 0);
    float normalized = uintBitsToFloat(0x3f000000u | (m & 0x7fffffu));
    uint guess = uint((1.0 / normalized) * float(0x800000u >> shift));
    uint rounded = pica_nearest(m, uvec2(0u, 0x10000u >> shift), guess, false);
    uint base = uint(max(252 - e, 0)) << 23;
    return uintBitsToFloat(sign_bit | (base + rounded));
}

float pica_rsq(float value) {
    uint bits = floatBitsToUint(value);
    uint magnitude = bits & 0x7fffffffu;
    if (magnitude == 0u) return pica_rcp(value);
    if (bits != magnitude) return uintBitsToFloat(0x7fc00000u);
    int exponent = int(magnitude >> 23);
    if (exponent == 255) return magnitude == 0x7f800000u ? 0.0 : value;
    uint m = (magnitude & 0x7fffffu) | (exponent == 0 ? 0u : 0x800000u);
    if (exponent == 0) {
        int shift = 23 - findMSB(m);
        m <<= shift;
        exponent = 1 - shift;
    }
    int power = exponent - 127;
    int parity = power & 1;
    float normalized = uintBitsToFloat((uint(127 + parity) << 23) | (m & 0x7fffffu));
    uint guess = uint(sqrt(normalized) * 8388608.0);
    uint shift = uint(25 + parity);
    uvec2 target = uvec2(m << shift, m >> (32u - shift));
    uint rounded = pica_nearest(m, target, guess, true);
    uint base = uint(126 + (power - parity) / 2) << 23;
    return pica_rcp(uintBitsToFloat(base + rounded));
}
)glsl";

enum class ScopeKind { Main, Subroutine, Then, Else, Loop };

struct Scope {
    ScopeKind kind;
    u32 begin;
    u32 end;
    int parent = kNone;
    int exit = kNone;
    int break_target = kNone;
    bool has_else = false;
};

struct Node {
    int scope = kNone;
    u32 word = 0;
    std::string code;
    std::string condition;
    int next = kNone;
    int taken = kNone;
    int callee = kNone;
    int result = kNone;
    bool backwards = false;
    bool reachable = false;
};

struct Procedure {
    u32 begin;
    u32 end;
    std::vector<Scope> scopes;
    std::vector<Node> nodes;
    std::map<std::pair<int, u32>, int> positions;
    std::vector<int> callees;
    std::vector<int> loops;
    int entry = kNone;
    bool guarded = false;
};

struct Refusal {
    const char* category;
    const char* message;
    int word;
};

std::string ProcedureName(int id) {
    return "pica_body_" + std::to_string(id);
}

class Translator {
public:
    Translator(const DecompileInput& input, const RegGetter& inputs, const RegGetter& outputs)
        : input_(input), inputs_(inputs), outputs_(outputs) {}

    DecompileResult Run();

private:
    int AddProcedure(u32 begin, u32 end);
    int AddScope(Procedure& proc, Scope scope);
    int AddNode(Procedure& proc, Node node);
    int Position(Procedure& proc, int scope, u32 pc);
    int ScopeExit(Procedure& proc, int scope, u32 pc);
    int FindLoop(const Procedure& proc, int scope) const;
    void Decode(Procedure& proc, Node& node);
    void Arithmetic(Node& node, Instruction instruction);
    std::string Source(isa::SourceRegister reg, u32 address, isa::Swizzle swizzle, u32 operand);
    void CheckCallGraph() const;
    void EmitProcedure(std::string& glsl, const Procedure& proc, int id) const;
    void EmitNode(std::string& glsl, const Node& node, bool dispatch) const;

    const DecompileInput& input_;
    const RegGetter& inputs_;
    const RegGetter& outputs_;
    std::deque<Procedure> procedures_;
    std::map<std::pair<u32, u32>, int> entries_;
    DecompileResult result_;
    u32 instruction_count_ = 0;
    bool ignored_geometry_ = false;
    bool uses_scalar_rounding_ = false;
};

std::string Condition(Instruction instruction) {
    const std::string x = instruction.refx() ? "conditional_code.x" : "!conditional_code.x";
    const std::string y = instruction.refy() ? "conditional_code.y" : "!conditional_code.y";
    switch (instruction.condition_op()) {
    case isa::ConditionOp::Or: return x + " || " + y;
    case isa::ConditionOp::And: return x + " && " + y;
    case isa::ConditionOp::JustX: return x;
    case isa::ConditionOp::JustY: return y;
    }
    return {};
}

std::string UniformCondition(Instruction instruction, bool inverted = false) {
    return "(uniforms.b & " + std::to_string(1u << instruction.bool_uniform_id()) + "u) " +
           (inverted ? "== 0u" : "!= 0u");
}

std::string Translator::Source(isa::SourceRegister reg, u32 address, isa::Swizzle swizzle, u32 operand) {
    std::string expression;
    const std::string index = std::to_string(reg.index());
    switch (reg.kind()) {
    case isa::SourceKind::Input: expression = inputs_(reg.index()); break;
    case isa::SourceKind::Temporary: expression = "reg_tmp" + index; break;
    case isa::SourceKind::FloatUniform:
        if (address != 0) {
            constexpr std::string_view offsets[] = {"address_registers.x", "address_registers.y", "address_registers.z"};
            expression = "pica_read(" + index + ", " + std::string(offsets[address - 1]) + ")";
        } else {
            expression = "uniforms.f[" + index + "]";
        }
        break;
    }
    std::string selection;
    for (u32 component = 0; component < 4; ++component) {
        selection += kLanes[swizzle.selector(operand, component)];
    }
    return (swizzle.negate(operand) ? "-(" : "(") + expression + ")." + selection;
}

void Translator::Arithmetic(Node& node, Instruction instruction) {
    const Opcode opcode = instruction.Effective();
    const bool mad = instruction.info().format == isa::Format::MultiplyAdd;
    const bool inverted = instruction.info().src_inversed;
    const u32 descriptor = mad ? instruction.mad_operand_desc_id() : instruction.operand_desc_id();
    const isa::Swizzle swizzle{descriptor < input_.swizzle.size() ? input_.swizzle[descriptor] : 0};
    std::string mask;
    for (u32 component = 0; component < 4; ++component) {
        if (swizzle.DestEnabled(component)) {
            mask += kLanes[component];
        }
    }
    const bool lit = opcode == Opcode::LIT;
    if (lit || opcode == Opcode::DST || opcode == Opcode::DSTI) {
        result_.uses_unverified_instructions = true;
    }
    std::string destination;
    if (opcode != Opcode::MOVA && opcode != Opcode::CMP) {
        const isa::DestRegister dest = mad ? instruction.mad_dest() : instruction.dest();
        if (dest.is_output()) {
            destination = outputs_(dest.index());
        } else if (dest.is_temporary()) {
            destination = "reg_tmp" + std::to_string(dest.index());
        }
        if (!lit && (destination.empty() || mask.empty())) {
            return;
        }
    }
    const u32 address = mad ? instruction.mad_address_register_index() : instruction.address_register_index();
    const auto first = mad ? instruction.mad_src1(inverted) : instruction.src1(inverted);
    const auto second = mad ? instruction.mad_src2(inverted) : instruction.src2(inverted);
    const bool unary = opcode == Opcode::MOV || opcode == Opcode::MOVA || opcode == Opcode::FLR || lit ||
                       opcode == Opcode::EX2 || opcode == Opcode::LG2 || opcode == Opcode::RCP || opcode == Opcode::RSQ;
    node.code += "{\nvec4 a = " + Source(first, mad || inverted ? 0 : address, swizzle, 1) + ";\n";
    if (!unary) {
        node.code += "vec4 b = " + Source(second, (mad != inverted) ? address : 0, swizzle, 2) + ";\n";
    }
    std::string expr;
    switch (opcode) {
    case Opcode::MOV: expr = "a"; break;
    case Opcode::FLR: expr = "floor(a)"; break;
    case Opcode::ADD: expr = "a + b"; break;
    case Opcode::MUL: expr = "pica_mul(a, b)"; break;
    case Opcode::MAD:
    case Opcode::MADI:
        node.code += "vec4 c = " + Source(instruction.mad_src3(inverted), inverted ? address : 0, swizzle, 3) + ";\n";
        expr = "pica_mul(a, b) + c";
        break;
    case Opcode::DPH:
    case Opcode::DPHI:
        node.code += "a.w = 1.0;\n";
        [[fallthrough]];
    case Opcode::DP3:
    case Opcode::DP4: {
        const bool three = opcode == Opcode::DP3;
        node.code += "vec4 product = pica_mul(a, b);\n";
        if (input_.exact_arithmetic) {
            expr = three ? "vec4((product.x + product.y) + product.z)"
                         : "vec4(((product.x + product.y) + product.z) + product.w)";
        } else {
            expr = three ? "vec4(dot(product.xyz, vec3(1.0)))" : "vec4(dot(product, vec4(1.0)))";
        }
        break;
    }
    case Opcode::DST:
    case Opcode::DSTI: expr = "vec4(1.0, pica_mul(a, b).y, a.z, b.w)"; break;
    case Opcode::LIT:
        node.code += "conditional_code = greaterThanEqual(a.xw, vec2(0.0));\n";
        expr = "vec4(1.0, max(a.x, 0.0), a.x > 0.0 && a.y > 0.0 ? pow(a.y, clamp(a.w, -128.0, 128.0)) : 0.0, 1.0)";
        break;
    case Opcode::MAX: expr = "mh4n_max(a, b)"; break;
    case Opcode::MIN: expr = "mh4n_min(a, b)"; break;
    case Opcode::SGE:
    case Opcode::SGEI: expr = "vec4(greaterThanEqual(a, b))"; break;
    case Opcode::SLT:
    case Opcode::SLTI: expr = "vec4(lessThan(a, b))"; break;
    case Opcode::EX2: expr = "vec4(exp2(a.x))"; break;
    case Opcode::LG2: expr = "vec4(log2(a.x))"; break;
    case Opcode::RCP:
    case Opcode::RSQ:
        uses_scalar_rounding_ |= input_.exact_arithmetic;
        if (opcode == Opcode::RCP) {
            expr = input_.exact_arithmetic ? "vec4(pica_rcp(a.x))" : "vec4(1.0 / a.x)";
        } else {
            expr = input_.exact_arithmetic ? "vec4(pica_rsq(a.x))" : "vec4(inversesqrt(a.x))";
        }
        break;
    case Opcode::MOVA:
        for (u32 component = 0; component < 2; ++component) {
            if (swizzle.DestEnabled(component)) {
                const std::string lane(1, kLanes[component]);
                node.code += "address_registers." + lane + " = pica_address(a." + lane + ");\n";
            }
        }
        break;
    case Opcode::CMP: {
        constexpr std::string_view ops[] = {"==", "!=", "<", "<=", ">", ">="};
        const auto x = static_cast<u32>(instruction.compare_x());
        const auto y = static_cast<u32>(instruction.compare_y());
        if (x < std::size(ops)) node.code += "conditional_code.x = a.x " + std::string(ops[x]) + " b.x;\n";
        if (y < std::size(ops)) node.code += "conditional_code.y = a.y " + std::string(ops[y]) + " b.y;\n";
        break;
    }
    default: throw Refusal{"unhandled-instruction", "Unhandled instruction", static_cast<int>(node.word)};
    }
    if (!expr.empty() && !destination.empty() && !mask.empty()) {
        node.code += "precise vec4 value = " + expr + ";\n" + destination + "." + mask + " = value." + mask + ";\n";
    }
    node.code += "}\n";
}

int Translator::AddProcedure(u32 begin, u32 end) {
    const int id = static_cast<int>(procedures_.size());
    const auto [entry, inserted] = entries_.try_emplace({begin, end}, id);
    if (inserted) {
        auto& proc = procedures_.emplace_back();
        proc.begin = begin;
        proc.end = end;
    }
    return entry->second;
}

int Translator::AddScope(Procedure& proc, Scope scope) {
    proc.scopes.push_back(scope);
    return static_cast<int>(proc.scopes.size()) - 1;
}

int Translator::AddNode(Procedure& proc, Node node) {
    proc.nodes.push_back(std::move(node));
    return static_cast<int>(proc.nodes.size()) - 1;
}

int Translator::Position(Procedure& proc, int scope, u32 pc) {
    const Scope& region = proc.scopes[scope];
    if (pc == region.end && region.exit != kNone) {
        return region.exit;
    }
    const auto [position, inserted] = proc.positions.try_emplace({scope, pc}, static_cast<int>(proc.nodes.size()));
    if (inserted) {
        Node node;
        node.scope = scope;
        node.word = pc;
        AddNode(proc, std::move(node));
    }
    return position->second;
}

int Translator::ScopeExit(Procedure& proc, int scope, u32 pc) {
    for (int current = scope; current != kNone; current = proc.scopes[current].parent) {
        const Scope& region = proc.scopes[current];
        if (pc >= region.begin && pc <= region.end) {
            return Position(proc, current, pc);
        }
        if (region.kind == ScopeKind::Loop) {
            break;
        }
    }
    return kNone;
}

int Translator::FindLoop(const Procedure& proc, int scope) const {
    for (int current = scope; current != kNone; current = proc.scopes[current].parent) {
        if (proc.scopes[current].kind == ScopeKind::Loop) {
            return proc.scopes[current].break_target;
        }
    }
    return kNone;
}

void Translator::Decode(Procedure& proc, Node& node) {
    const u32 pc = node.word;
    const int scope = node.scope;
    const Scope region = proc.scopes[scope];
    const int at = static_cast<int>(pc);
    if (pc >= region.end || pc >= input_.program.size()) {
        throw Refusal{"loaded-code", "Execution ran beyond the loaded program", at};
    }
    if (++instruction_count_ > kMaxCompiledInstructions) {
        throw Refusal{"program-size", "Vertex program exceeds the compiled instruction limit", at};
    }
    const Instruction instruction{input_.program[pc]};
    const Opcode opcode = instruction.Effective();
    const u32 target = instruction.dest_offset();
    const u32 length = instruction.num_instructions();
    const auto next = [&] { return Position(proc, scope, pc + 1); };
    const bool tail_sensitive = region.kind == ScopeKind::Loop || region.kind == ScopeKind::Subroutine ||
                                (region.kind == ScopeKind::Then && region.has_else);
    node.code = "// PICA " + std::to_string(pc) + " " + instruction.info().name + "\n";

    switch (opcode) {
    case Opcode::END:
        node.result = 1;
        return;
    case Opcode::NOP:
        break;
    case Opcode::EMIT:
    case Opcode::SETEMIT:
        ignored_geometry_ = true;
        break;
    case Opcode::BREAK:
    case Opcode::BREAKC: {
        const int local_loop = FindLoop(proc, scope);
        if (local_loop != kNone) {
            if (opcode == Opcode::BREAK) {
                node.next = local_loop;
                return;
            }
            node.condition = Condition(instruction);
            node.taken = local_loop;
        } else {
            node.code += "if (loop_depth > 0";
            if (opcode == Opcode::BREAKC) {
                node.code += " && (" + Condition(instruction) + ")";
            }
            node.code += ") return 2;\n";
        }
        break;
    }
    case Opcode::IFC:
    case Opcode::IFU: {
        const u32 end = target + length;
        if (target < pc + 1 || end > region.end) {
            throw Refusal{"loaded-code", "IF branches extend outside their loaded region", at};
        }
        result_.differs_from_interpreter |= tail_sensitive && end == region.end;
        const int join = Position(proc, scope, end);
        const int then_scope = AddScope(proc, {ScopeKind::Then, pc + 1, target, scope, join, kNone, length != 0});
        node.taken = Position(proc, then_scope, pc + 1);
        if (length == 0) {
            node.next = join;
        } else {
            const int else_scope = AddScope(proc, {ScopeKind::Else, target, end, scope, join});
            node.next = Position(proc, else_scope, target);
        }
        node.condition = opcode == Opcode::IFU ? UniformCondition(instruction) : Condition(instruction);
        return;
    }
    case Opcode::LOOP: {
        const u32 end = target + 1;
        if (target < pc + 1 || end > region.end) {
            throw Refusal{"loaded-code", "LOOP body extends outside its loaded region", at};
        }
        result_.differs_from_interpreter |= region.kind == ScopeKind::Loop && end == region.end;
        const int after = Position(proc, scope, end);
        const int step = AddNode(proc, {});
        const int leave = AddNode(proc, {});
        const int loop_scope = AddScope(proc, {ScopeKind::Loop, pc + 1, end, scope, step, leave});
        const int body = Position(proc, loop_scope, pc + 1);
        const std::string suffix = std::to_string(step);
        const std::string counter = "pica_count_" + suffix;
        const std::string uniform = "uniforms.i[" + std::to_string(instruction.int_uniform_id()) + "]";
        const std::string initial = "address_registers.z = int(" + uniform + ".y);\n";
        const std::string increment = "address_registers.z += int(" + uniform + ".z);\n";
        proc.loops.push_back(step);

        node.code += counter + " = " + uniform + ".x + 1u;\n" + initial + "++loop_depth;\n";
        node.next = body;
        proc.nodes[step].code = increment;
        proc.nodes[step].condition = "--" + counter + " != 0u";
        proc.nodes[step].taken = body;
        proc.nodes[leave].code = "if (--loop_depth > 0) " + initial;
        // Both exits use the same restore. BREAK still advances aL once.
        proc.nodes[step].next = leave;
        proc.nodes[leave].next = after;
        Node break_node;
        break_node.code = increment;
        break_node.next = leave;
        proc.scopes[loop_scope].break_target = AddNode(proc, std::move(break_node));
        return;
    }
    case Opcode::CALL:
    case Opcode::CALLC:
    case Opcode::CALLU: {
        if (target >= input_.program.size() || target + length > input_.program.size()) {
            throw Refusal{"loaded-code", "CALL target extends outside the loaded program", at};
        }
        if (length == 0) {
            break;
        }
        result_.differs_from_interpreter |= (tail_sensitive || region.kind == ScopeKind::Then) && pc + 1 == region.end;
        node.callee = AddProcedure(target, target + length);
        proc.callees.push_back(node.callee);
        node.taken = FindLoop(proc, scope);
        if (opcode == Opcode::CALLC) node.condition = Condition(instruction);
        if (opcode == Opcode::CALLU) node.condition = UniformCondition(instruction);
        // The call's break edge exists only when a loop is active in this function.
        break;
    }
    case Opcode::JMPC:
    case Opcode::JMPU:
        if (target >= input_.program.size()) {
            throw Refusal{"loaded-code", "Jump target is outside the loaded program", at};
        }
        node.taken = ScopeExit(proc, scope, target);
        if (node.taken == kNone) {
            throw Refusal{"jump-scope", "Jump target is outside the current procedure or loop", at};
        }
        result_.differs_from_interpreter |= (tail_sensitive || region.kind == ScopeKind::Then) && pc + 1 == region.end;
        node.backwards = target <= pc;
        proc.guarded |= node.backwards;
        node.condition = opcode == Opcode::JMPC ? Condition(instruction) : UniformCondition(instruction, length & 1u);
        break;
    default:
        if (instruction.info().format == isa::Format::Unknown) {
            throw Refusal{"unhandled-instruction", "Unhandled instruction", at};
        }
        Arithmetic(node, instruction);
        break;
    }
    node.next = next();
}

void Translator::CheckCallGraph() const {
    std::vector<u32> incoming(procedures_.size());
    for (const auto& proc : procedures_) {
        for (const int callee : proc.callees) {
            ++incoming[callee];
        }
    }
    std::deque<int> ready;
    for (size_t id = 0; id < incoming.size(); ++id) {
        if (incoming[id] == 0) ready.push_back(static_cast<int>(id));
    }
    while (!ready.empty()) {
        const int id = ready.front();
        ready.pop_front();
        for (const int callee : procedures_[id].callees) {
            if (--incoming[callee] == 0) ready.push_back(callee);
        }
    }
    const auto cycle = std::find_if(incoming.begin(), incoming.end(), [](u32 count) { return count != 0; });
    if (cycle != incoming.end()) {
        throw Refusal{"recursive-call", "Recursive vertex shader call", static_cast<int>(procedures_[cycle - incoming.begin()].begin)};
    }
}

void Translator::EmitNode(std::string& glsl, const Node& node, bool dispatch) const {
    glsl += node.code;
    if (node.callee != kNone) {
        const bool conditional = !node.condition.empty();
        glsl += conditional ? "if (" + node.condition + ") {\n" : "{\n";
        glsl += "int outcome = " + ProcedureName(node.callee) + "();\n";
        if (node.taken != kNone) {
            glsl += "if (outcome == 2) { pc = " + std::to_string(node.taken) + "; break; }\n";
        }
        glsl += "if (outcome != 0) return outcome;\n}\n";
    } else if (!node.condition.empty()) {
        glsl += "if (" + node.condition + ") {\n";
        if (node.backwards) {
            glsl += "if (!(jmp_guard < " + std::to_string(kMaxJumpIterations) + "u)) return 1;\n++jmp_guard;\n";
        }
        glsl += "pc = " + std::to_string(node.taken) + "; break;\n}\n";
    }
    if (node.result != kNone) {
        glsl += "return " + std::to_string(node.result) + ";\n";
    } else if (dispatch) {
        glsl += "pc = " + std::to_string(node.next) + "; break;\n";
    }
}

void Translator::EmitProcedure(std::string& glsl, const Procedure& proc, int id) const {
    std::vector<u32> incoming(proc.nodes.size());
    std::vector<bool> is_head(proc.nodes.size());
    bool dispatch = false;
    is_head[proc.entry] = true;
    for (const Node& node : proc.nodes) {
        if (!node.reachable) continue;
        if (node.next != kNone) ++incoming[node.next];
        if (node.taken != kNone) {
            dispatch = true;
            ++incoming[node.taken];
            is_head[node.taken] = true;
        }
    }
    for (size_t n = 0; n < proc.nodes.size(); ++n) {
        is_head[n] = is_head[n] || incoming[n] > 1;
    }
    dispatch |= std::count(is_head.begin(), is_head.end(), true) > 1;

    glsl += "int " + ProcedureName(id) + "() {\n";
    for (const int loop : proc.loops) {
        const std::string n = std::to_string(loop);
        glsl += "uint pica_count_" + n + " = 0u;\n";
    }
    if (dispatch) {
        glsl += "int pc = " + std::to_string(proc.entry) + ";\n";
        if (proc.guarded) glsl += "uint jmp_guard = 0u;\n";
        glsl += "for (;;) {\nswitch (pc) {\n";
    }
    for (size_t head = 0; head < proc.nodes.size(); ++head) {
        if (!is_head[head]) continue;
        if (dispatch) glsl += "case " + std::to_string(head) + ": {\n";
        for (int current = static_cast<int>(head); current != kNone;) {
            const Node& node = proc.nodes[current];
            const bool ends_block = node.next == kNone || is_head[node.next];
            EmitNode(glsl, node, dispatch && ends_block);
            current = ends_block ? kNone : node.next;
        }
        if (dispatch) glsl += "}\n";
    }
    if (dispatch) glsl += "default: return 1;\n}\n}\n";
    glsl += "}\n";
}

DecompileResult Translator::Run() {
    try {
        if (input_.program.size() > kMaxProgramWords || input_.swizzle.size() > kMaxSwizzleWords) {
            throw Refusal{"program-size", "Loaded shader data exceeds PICA memory", -1};
        }
        if (input_.main_offset >= input_.program.size()) {
            throw Refusal{"entry-point", "Vertex shader entry point is outside the loaded program", -1};
        }
        AddProcedure(input_.main_offset, static_cast<u32>(input_.program.size()));
        for (size_t id = 0; id < procedures_.size(); ++id) {
            auto& proc = procedures_[id];
            int return_node = kNone;
            if (id != 0) {
                Node end;
                end.result = 0;
                return_node = AddNode(proc, std::move(end));
            }
            const ScopeKind kind = id == 0 ? ScopeKind::Main : ScopeKind::Subroutine;
            const int scope = AddScope(proc, {kind, id == 0 ? 0u : proc.begin, proc.end, kNone, return_node});
            proc.entry = Position(proc, scope, proc.begin);
            std::deque<int> pending{proc.entry};
            while (!pending.empty()) {
                const int current = pending.front();
                pending.pop_front();
                if (proc.nodes[current].reachable) continue;
                Node node = std::move(proc.nodes[current]);
                if (node.scope != kNone) Decode(proc, node);
                if (node.next != kNone) pending.push_back(node.next);
                if (node.taken != kNone) pending.push_back(node.taken);
                node.reachable = true;
                proc.nodes[current] = std::move(node);
            }
        }
        CheckCallGraph();

        std::string& glsl = result_.source;
        glsl += kPrelude;
        if (uses_scalar_rounding_) glsl += kExactScalars;
        // The vertex and quaternion wrappers also reset these globals.
        glsl += "ivec3 address_registers;\nbvec2 conditional_code;\nint loop_depth;\nbool loop_break;\n";
        std::string reset;
        for (int reg = 0; reg < 16; ++reg) {
            const std::string name = "reg_tmp" + std::to_string(reg);
            glsl += "vec4 " + name + ";\n";
            reset += name + " = vec4(0.0, 0.0, 0.0, 1.0);\n";
        }
        for (size_t id = 0; id < procedures_.size(); ++id) {
            glsl += "int " + ProcedureName(static_cast<int>(id)) + "();\n";
        }
        for (size_t id = 0; id < procedures_.size(); ++id) {
            EmitProcedure(glsl, procedures_[id], static_cast<int>(id));
        }
        glsl += "bool exec_shader() {\n" + reset +
                "address_registers = ivec3(0);\nconditional_code = bvec2(false);\nloop_depth = 0;\n"
                "return pica_body_0() == 1;\n}\n";
        if (std::any_of(procedures_.begin(), procedures_.end(), [](const auto& proc) { return proc.guarded; })) {
            result_.notes.emplace_back("A backwards jump is bounded to 65536 taken branches per procedure invocation");
        }
        if (ignored_geometry_) {
            result_.notes.emplace_back("EMIT and SETEMIT have no effect in the vertex stage");
        }
        result_.ok = true;
    } catch (const Refusal& error) {
        result_.error = error.message;
        result_.error_category = error.category;
        result_.error_word = error.word;
    }
    return std::move(result_);
}

} // namespace

DecompileResult DecompileProgram(const DecompileInput& input, const RegGetter& input_reg,
                                 const RegGetter& output_reg) {
    return Translator(input, input_reg, output_reg).Run();
}

} // namespace mh4n::gpu::shader
