// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: pica_isa.h and 3dbrew GPU/Shader Instruction Set.

#include "pica_isa.h"

#include <array>

namespace mh4n::gpu::shader::isa {
namespace {

constexpr auto MakeOpTable() {
    std::array<OpInfo, 64> table{};
    table.fill({Format::Unknown, false, "unknown"});
    constexpr const char* arithmetic[] = {
        "add", "dp3", "dp4", "dph", "dst", "ex2", "lg2", "lit", "mul", "sge", "slt", "flr", "max", "min", "rcp", "rsq",
    };
    for (u32 i = 0; i < std::size(arithmetic); ++i) {
        table[i] = {Format::Arithmetic, false, arithmetic[i]};
    }
    constexpr const char* inverted[] = {"dphi", "dsti", "sgei", "slti"};
    for (u32 i = 0; i < std::size(inverted); ++i) {
        table[0x18 + i] = {Format::Arithmetic, true, inverted[i]};
    }
    table[0x12] = {Format::Arithmetic, false, "mova"};
    table[0x13] = {Format::Arithmetic, false, "mov"};
    table[0x20] = {Format::Trivial, false, "break"};
    table[0x21] = {Format::Trivial, false, "nop"};
    table[0x22] = {Format::Trivial, false, "end"};
    table[0x23] = {Format::Conditional, false, "breakc"};
    table[0x24] = {Format::Conditional, false, "call"};
    table[0x25] = {Format::Conditional, false, "callc"};
    table[0x26] = {Format::UniformFlowControl, false, "callu"};
    table[0x27] = {Format::UniformFlowControl, false, "ifu"};
    table[0x28] = {Format::Conditional, false, "ifc"};
    table[0x29] = {Format::UniformFlowControl, false, "loop"};
    table[0x2A] = {Format::Trivial, false, "emit"};
    table[0x2B] = {Format::SetEmit, false, "setemit"};
    table[0x2C] = {Format::Conditional, false, "jmpc"};
    table[0x2D] = {Format::UniformFlowControl, false, "jmpu"};
    table[0x2E] = table[0x2F] = {Format::Arithmetic, false, "cmp"};
    for (u32 i = 0x30; i < 0x38; ++i) {
        table[i] = {Format::MultiplyAdd, true, "madi"};
        table[i + 8] = {Format::MultiplyAdd, false, "mad"};
    }
    return table;
}

constexpr auto kOpTable = MakeOpTable();
constexpr OpInfo kUnknown{Format::Unknown, false, "unknown"};

} // namespace

const OpInfo& GetOpInfo(u32 opcode6) {
    return opcode6 < kOpTable.size() ? kOpTable[opcode6] : kUnknown;
}

} // namespace mh4n::gpu::shader::isa
