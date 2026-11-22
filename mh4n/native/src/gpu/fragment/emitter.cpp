// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: emitter.h, the FragmentKey layout and gpu_tev_test's emitter probe.

#include "emitter.h"

#include <algorithm>
#include <cstring>

namespace mh4n::gpu::fragment {

Emitter::Emitter(Mode mode, const FragmentKey& applied_key, const DeviceProfile& profile, ShaderPass pass)
    : mode_(mode), profile_(profile), pass_(pass) {
    if (specialised()) {
        std::memcpy(words_, &applied_key, sizeof(words_));
    }
}

Value Emitter::Field(size_t word, u32 first, u32 count) const {
    if (specialised()) {
        return Slice(Const(KnownWord(word)), first, count);
    }
    return Slice({false, 0, fmt::format("cfg[{}][{}]", word / 4, word % 4)}, first, count);
}

Value Emitter::Slice(const Value& value, u32 first, u32 count) const {
    if (count == 0 || first >= 32) {
        return Const(0);
    }
    const u32 available = 32 - first;
    const u32 mask = 0xFFFFFFFFu >> (32 - std::min(count, available));
    if (value.known) {
        return Const((value.literal >> first) & mask);
    }
    std::string expression = value.glsl;
    if (first != 0) {
        expression = fmt::format("(({}) >> {}u)", expression, first);
    }
    if (count < available) {
        expression = fmt::format("(({}) & {}u)", expression, mask);
    }
    return {false, 0, std::move(expression)};
}

Value Emitter::Select(const Cond& condition, const Value& yes, const Value& no) const {
    if (condition.known) {
        return condition.value ? yes : no;
    }
    if (yes.known && no.known && yes.literal == no.literal) {
        return yes;
    }
    return {false, 0, fmt::format("({} ? {} : {})", condition.glsl, yes.glsl, no.glsl)};
}

Cond Emitter::Eq(const Value& value, u32 number) const {
    return Eq(value, Const(number));
}

Cond Emitter::Ne(const Value& value, u32 number) const {
    return Not(Eq(value, number));
}

Cond Emitter::Eq(const Value& a, const Value& b) const {
    if (a.known && b.known) {
        return Cond::Known(a.literal == b.literal);
    }
    return {false, false, fmt::format("(({}) == ({}))", a.glsl, b.glsl)};
}

Cond Emitter::Not(const Cond& condition) const {
    if (condition.known) {
        return Cond::Known(!condition.value);
    }
    return {false, false, fmt::format("(!({}))", condition.glsl)};
}

Cond Emitter::And(const Cond& a, const Cond& b) const {
    if (a.known) {
        return a.value ? b : a;
    }
    if (b.known) {
        return b.value ? a : b;
    }
    return {false, false, fmt::format("(({}) && ({}))", a.glsl, b.glsl)};
}

Cond Emitter::Or(const Cond& a, const Cond& b) const {
    if (a.known) {
        return a.value ? a : b;
    }
    if (b.known) {
        return b.value ? b : a;
    }
    return {false, false, fmt::format("(({}) || ({}))", a.glsl, b.glsl)};
}

void Emitter::If(const Cond& condition, const Body& then, const Body& otherwise) {
    if (condition.known) {
        const Body& chosen = condition.value ? then : otherwise;
        if (chosen) {
            chosen();
        }
        return;
    }

    Line("if ({}) {{", condition.glsl);
    then();
    if (otherwise) {
        Raw("} else {\n");
        otherwise();
    }
    Raw("}\n");
}

void Emitter::Switch(const Value& value, std::initializer_list<std::pair<u32, Body>> cases,
                     const Body& otherwise) {
    if (value.known) {
        for (const auto& [number, body] : cases) {
            if (value.literal == number) {
                body();
                return;
            }
        }
        if (otherwise) {
            otherwise();
        }
        return;
    }

    Line("switch ({}) {{", value.glsl);
    for (const auto& [number, body] : cases) {
        Line("case {}u: {{", number);
        body();
        Raw("} break;\n");
    }
    if (otherwise) {
        Raw("default: {\n");
        otherwise();
        Raw("} break;\n");
    }
    Raw("}\n");
}

void Emitter::ForEach(const Value& count, u32 max, std::string_view variable,
                      const std::function<void(const std::string& index)>& body) {
    if (count.known) {
        for (u32 i = 0; i < std::min(count.literal, max); ++i) {
            Raw("{\n");
            body(Const(i).glsl);
            Raw("}\n");
        }
        return;
    }

    Line("for (uint {0} = 0u; {0} < min({1}, {2}u); ++{0}) {{", variable, count.glsl, max);
    body(std::string(variable));
    Raw("}\n");
}

} // namespace mh4n::gpu::fragment
