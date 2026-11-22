// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: gateway_cheat.h, cheats_test.cpp and the Gateway 3DS release notes (Cheat Code Format, mirrored on WikiTemp).

#include "gateway_cheat.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>
#include <string_view>
#include <utility>

namespace mh4n::cheats {
namespace {

using Type = GatewayCheat::CheatType;
using Line = GatewayCheat::CheatLine;

std::string_view Trim(std::string_view line) {
    constexpr std::string_view whitespace = " \t\r\n\v\f";
    const auto first = line.find_first_not_of(whitespace);
    if (first == line.npos) return {};
    return line.substr(first, line.find_last_not_of(whitespace) - first + 1);
}

bool Hex(std::string_view word, u32& value) {
    const auto [end, error] = std::from_chars(word.data(), word.data() + word.size(), value, 16);
    return error == std::errc{} && end == word.data() + word.size();
}

std::string_view StripInlineComment(std::string_view line) {
    const auto comment = std::min({line.find("//"), line.find('#'), line.find(';')});
    return Trim(line.substr(0, comment));
}

std::vector<Line> ReadLines(std::istream& input) {
    std::vector<Line> lines;
    for (std::string line; std::getline(input, line);) {
        const auto text = Trim(line);
        if (!text.empty()) lines.emplace_back(std::string(text));
    }
    return lines;
}

u32 WriteWidth(Type type) {
    switch (type) {
    case Type::Write32: case Type::IncrementiveWrite32: return 4;
    case Type::Write16: case Type::IncrementiveWrite16: return 2;
    case Type::Write8: case Type::IncrementiveWrite8: return 1;
    default: return 0;
    }
}

bool IsCondition(Type type) {
    return (type >= Type::GreaterThan32 && type <= Type::NotEqualTo16WithMask) || type == Type::Joker;
}

bool Condition(const Line& line, u32 offset, CheatTarget& target) {
    if (line.type == Type::Joker) return (target.PadState() & line.value) == line.value;
    const u32 address = offset + line.address;
    const bool narrow = line.type >= Type::GreaterThan16WithMask;
    const u32 expected = narrow ? line.value & 0xFFFF : line.value;
    const u32 actual = narrow ? (target.Read16(address) & ~(line.value >> 16)) : target.Read32(address);
    switch (line.type) {
    case Type::GreaterThan32: case Type::GreaterThan16WithMask: return expected > actual;
    case Type::LessThan32: case Type::LessThan16WithMask: return expected < actual;
    case Type::EqualTo32: case Type::EqualTo16WithMask: return expected == actual;
    default: return expected != actual;
    }
}

u32 Read(CheatTarget& target, u32 address, u32 width) {
    switch (width) {
    case 1: return target.Read8(address);
    case 2: return target.Read16(address);
    default: return target.Read32(address);
    }
}

void Write(CheatTarget& target, u32 address, u32 value, u32 width) {
    switch (width) {
    case 1:
        if (target.Read8(address) != static_cast<u8>(value)) target.Write8(address, static_cast<u8>(value));
        break;
    case 2:
        if (target.Read16(address) != static_cast<u16>(value)) target.Write16(address, static_cast<u16>(value));
        break;
    case 4:
        if (target.Read32(address) != value) target.Write32(address, value);
        break;
    }
}

std::size_t PatchLines(const std::vector<Line>& lines, std::size_t op) {
    const u64 requested = (u64{lines[op].value} + 7) / 8;
    return static_cast<std::size_t>(std::min<u64>(requested, lines.size() - op - 1));
}

template <typename Emit>
void EmitPatch(const std::vector<Line>& lines, std::size_t op, u32 address, Emit emit) {
    u32 remaining = lines[op].value;
    const auto count = PatchLines(lines, op);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& payload = lines[op + 1 + i];
        if (!payload.valid) return;
        for (u32 word : {payload.first, payload.value}) {
            const u32 size = std::min(remaining, 4u);
            if (size == 0) break;
            emit(address, word, size);
            address += size;
            remaining -= size;
        }
    }
}

bool InRange(u32 address, u32 width, u32 begin, u32 end) {
    return width != 0 && begin < end &&
           ((address < end && u64{address} + width > begin) ||
            u64{address} + width > (u64{1} << 32) + begin);
}

bool ScanWrites(const std::vector<Line>& lines, u32 begin, u32 end,
                std::map<u32, u8>* bytes, std::string* why) {
    const auto fail = [why](const char* reason) {
        if (why) *why = reason;
        return false;
    };
    std::map<u32, u8> result;
    u32 offset = 0;
    bool loaded = false;
    bool uncertain_offset = false;
    u32 conditions = 0;
    bool in_loop = false;
    bool saw_loop = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto& line = lines[i];
        if (!line.valid) continue;
        const Type type = line.type;
        if (IsCondition(type)) {
            ++conditions;
            continue;
        }
        if (type == Type::Terminator) {
            if (conditions) --conditions;
            continue;
        }
        if (type == Type::Loop) {
            in_loop = saw_loop = true;
            continue;
        }
        if (type == Type::LoopExecuteVariant || type == Type::FullTerminator) {
            in_loop = false;
            if (type == Type::FullTerminator) {
                offset = 0;
                loaded = uncertain_offset = saw_loop = false;
                conditions = 0;
            }
            continue;
        }
        if (type == Type::SetOffset || type == Type::AddOffset || type == Type::LoadOffset) {
            const bool conditional = conditions || in_loop;
            if (type == Type::LoadOffset) {
                loaded = true;
            } else if (type == Type::SetOffset) {
                offset = line.value;
                if (!conditional) loaded = uncertain_offset = false;
            } else {
                offset += line.value;
            }
            uncertain_offset |= conditional;
            continue;
        }

        const bool patch = type == Type::Patch;
        const bool from_register = type >= Type::IncrementiveWrite32 && type <= Type::IncrementiveWrite8;
        const auto width = patch ? static_cast<u32>(std::min<u64>(line.value, 8ull * PatchLines(lines, i)))
                                 : WriteWidth(type);
        if (width == 0) continue;
        const u32 address = offset + (from_register ? line.value : line.address);
        if (bytes && loaded) return fail("loaded_offset");
        if (bytes && uncertain_offset) return fail("conditional");
        if (!loaded && InRange(address, width, begin, end)) {
            if (!bytes) return true;
            if (conditions || in_loop) return fail("conditional");
            if (from_register) return fail("register");
            const auto emit = [&](u32 at, u32 value, u32 size) {
                for (u32 byte = 0; byte < size; ++byte, ++at) {
                    if (at >= begin && at < end) result[at] = static_cast<u8>(value >> (8 * byte));
                }
            };
            if (patch) EmitPatch(lines, i, address, emit);
            else emit(address, line.value, width);
        }
        if (patch) i += PatchLines(lines, i);
        if (from_register) {
            offset += width;
            uncertain_offset |= conditions != 0 || saw_loop;
        }
    }
    if (result.empty()) return fail("none");
    *bytes = std::move(result);
    if (why) why->clear();
    return true;
}

} // namespace

GatewayCheat::CheatLine::CheatLine(const std::string& line) : cheat_line(Trim(line)) {
    const auto text = StripInlineComment(cheat_line);
    const auto left = text.substr(0, 8);
    const auto right = Trim(text.substr(left.size()));
    valid = left.size() == 8 && right.size() == 8 && Hex(left, first) && Hex(right, value);
    if (!valid) return;

    cheat_line = std::string(left) + " " + std::string(right);
    const u32 primary = first >> 28;
    type = static_cast<Type>(primary == 0xD ? first >> 24 : primary);
    address = first & 0x0FFF'FFFF;
}

GatewayCheat::GatewayCheat(std::string name, std::vector<CheatLine> cheat_lines, std::string comments)
    : name_(std::move(name)), cheat_lines_(std::move(cheat_lines)), comments_(std::move(comments)) {}

GatewayCheat::GatewayCheat(std::string name, const std::string& code, std::string comments)
    : name_(std::move(name)), comments_(std::move(comments)) {
    std::istringstream input(code);
    cheat_lines_ = ReadLines(input);
}

void GatewayCheat::Execute(CheatTarget& target) const {
    u32 offset = 0;
    u32 data = 0;
    u32 skipped = 0;
    std::size_t loop_start = 0;
    u32 repeats = 0;
    for (std::size_t i = 0; i < cheat_lines_.size(); ++i) {
        const auto& line = cheat_lines_[i];
        if (!line.valid) continue;
        const Type type = line.type;
        if (type == Type::FullTerminator || (type == Type::LoopExecuteVariant && !skipped)) {
            if (repeats) {
                --repeats;
                i = loop_start;
                skipped = 0;
            } else if (type == Type::FullTerminator) {
                offset = data = skipped = 0;
            }
            continue;
        }
        if (IsCondition(type)) {
            if (skipped || !Condition(line, offset, target)) ++skipped;
            continue;
        }
        if (type == Type::Terminator) {
            if (skipped) --skipped;
            continue;
        }
        if (type == Type::Patch) {
            if (!skipped) {
                EmitPatch(cheat_lines_, i, line.address + offset, [&](u32 at, u32 word, u32 size) {
                    if (size == 3) {
                        Write(target, at, word, 2);
                        Write(target, at + 2, word >> 16, 1);
                    } else {
                        Write(target, at, word, size);
                    }
                });
            }
            i += PatchLines(cheat_lines_, i);
            continue;
        }
        if (skipped) continue;

        switch (type) {
        case Type::Write32: case Type::Write16: case Type::Write8:
            Write(target, offset + line.address, line.value, WriteWidth(type));
            break;
        case Type::LoadOffset: offset = target.Read32(offset + line.address); break;
        case Type::Loop: loop_start = i; repeats = line.value; break;
        case Type::SetOffset: offset = line.value; break;
        case Type::AddOffset: offset += line.value; break;
        case Type::AddValue: data += line.value; break;
        case Type::SetValue: data = line.value; break;
        case Type::IncrementiveWrite32: case Type::IncrementiveWrite16: case Type::IncrementiveWrite8: {
            const u32 width = WriteWidth(type);
            Write(target, offset + line.value, data, width);
            offset += width;
            break;
        }
        case Type::Load32: data = Read(target, offset + line.value, 4); break;
        case Type::Load16: data = Read(target, offset + line.value, 2); break;
        case Type::Load8: data = Read(target, offset + line.value, 1); break;
        default: break;
        }
    }
}

std::string GatewayCheat::GetCode() const {
    std::string code;
    for (const auto& line : cheat_lines_) {
        code += line.cheat_line;
        code += '\n';
    }
    return code;
}

std::size_t GatewayCheat::InvalidLines() const {
    return static_cast<std::size_t>(std::count_if(cheat_lines_.begin(), cheat_lines_.end(),
                                                  [](const auto& line) { return !line.valid; }));
}

bool GatewayCheat::TouchesCode(u32 begin, u32 end) const {
    return ScanWrites(cheat_lines_, begin, end, nullptr, nullptr);
}

bool GatewayCheat::CodeWrites(u32 begin, u32 end, std::map<u32, u8>* bytes, std::string* why) const {
    std::map<u32, u8> discarded;
    return ScanWrites(cheat_lines_, begin, end, bytes ? bytes : &discarded, why);
}

std::string GatewayCheat::ToString() const {
    std::string text = "[" + name_ + "]\n";
    if (IsEnabled()) text += std::string(kEnabledText) + '\n';
    std::istringstream comments(comments_);
    for (std::string line; std::getline(comments, line);) text += '*' + line + '\n';
    return text + GetCode() + '\n';
}

std::vector<std::shared_ptr<GatewayCheat>> GatewayCheat::Parse(const std::string& text) {
    constexpr std::string_view bom = "\xEF\xBB\xBF";
    std::istringstream input(text.starts_with(bom) ? text.substr(bom.size()) : text);
    std::vector<std::shared_ptr<GatewayCheat>> cheats;
    std::string name;
    std::string comments;
    std::vector<Line> lines;
    bool enabled = false;
    bool have_name = false;
    const auto finish = [&] {
        if (have_name && !lines.empty()) {
            auto cheat = std::make_shared<GatewayCheat>(name, std::move(lines), std::move(comments));
            cheat->SetEnabled(enabled);
            cheats.push_back(std::move(cheat));
        }
        lines.clear();
        comments.clear();
        enabled = false;
    };
    for (std::string raw; std::getline(input, raw);) {
        auto line = Trim(raw);
        if (line.empty()) continue;
        if (line.front() == '[') {
            if (const auto end = line.find(']'); end != line.npos) {
                finish();
                name = line.substr(1, end - 1);
                have_name = true;
                continue;
            }
        }
        if (!have_name) continue;
        if (line == kEnabledText) {
            enabled = true;
        } else if (line.starts_with('*') || line.starts_with('#') || line.starts_with("//") ||
                   (line.starts_with('{') && line.ends_with('}'))) {
            if (line.starts_with('{')) {
                line = Trim(line.substr(1, line.size() - 2));
            } else if (line.starts_with('*')) {
                line.remove_prefix(1);
            } else {
                line = Trim(line.substr(line.starts_with("//") ? 2 : 1));
            }
            comments.append(line);
            comments += '\n';
        } else {
            lines.emplace_back(std::string(line));
        }
    }
    finish();
    return cheats;
}

std::vector<std::shared_ptr<GatewayCheat>> GatewayCheat::LoadFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::ostringstream text;
    text << input.rdbuf();
    return Parse(text.str());
}

} // namespace mh4n::cheats
