// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: vertex_loader.h, gpu/pica contracts, gpu_vertex_test.cpp cases, 3dbrew GPU/Internal Registers and GPU/Pitfalls.

#include "vertex_loader.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace mh4n::gpu::vertex {
namespace {

constexpr u32 kArrayCount = 12;
constexpr u32 kInputCount = 16;
constexpr u32 kFloat4Bytes = 4 * sizeof(float);
constexpr u32 kMaxArraysVertices = 1u << 24;

// The rows follow the PICA's two-bit type field and the columns its size field.
struct AttributeType {
    ComponentKind kind;
    u32 element_bytes;
    Conversion conversion;
    std::array<VertexFormat, 4> formats;
};

constexpr std::array<AttributeType, 4> kTypes{{
    {ComponentKind::S8, 1, Conversion::FromSint,
     {VertexFormat::R8_SINT, VertexFormat::R8G8_SINT, VertexFormat::R8G8B8A8_SINT,
      VertexFormat::R8G8B8A8_SINT}},
    {ComponentKind::U8, 1, Conversion::FromUint,
     {VertexFormat::R8_UINT, VertexFormat::R8G8_UINT, VertexFormat::R8G8B8A8_UINT,
      VertexFormat::R8G8B8A8_UINT}},
    {ComponentKind::S16, 2, Conversion::FromSint,
     {VertexFormat::R16_SINT, VertexFormat::R16G16_SINT, VertexFormat::R16G16B16A16_SINT,
      VertexFormat::R16G16B16A16_SINT}},
    {ComponentKind::F32, 4, Conversion::None,
     {VertexFormat::R32_FLOAT, VertexFormat::R32G32_FLOAT, VertexFormat::R32G32B32_FLOAT,
      VertexFormat::R32G32B32A32_FLOAT}},
}};

constexpr auto kFormats = [] {
    std::array<FormatInfo, static_cast<size_t>(VertexFormat::R32G32B32A32_FLOAT) + 1> formats{};
    for (const auto& type : kTypes) {
        for (u32 i = 0; i < type.formats.size(); ++i) {
            const u32 lanes = i + 1;
            formats[static_cast<size_t>(type.formats[i])] =
                {type.kind, lanes, lanes * type.element_bytes};
        }
    }
    return formats;
}();

constexpr u32 AlignUp(u32 size, u32 alignment) {
    const u32 remainder = size % alignment;
    return remainder == 0 ? size : size + alignment - remainder;
}

constexpr u32 Nibble(u32 word, u32 slot) {
    return (word >> (4 * slot)) & 0xf;
}

const u8* MapRange(Host& host, u64 address, u64 bytes) {
    constexpr u64 address_space = u64{1} << 32;
    if (address >= address_space || bytes >= address_space || bytes > address_space - address) {
        return nullptr;
    }
    return host.PhysicalPointer(static_cast<u32>(address), static_cast<u32>(bytes));
}

struct ArrayLayout {
    u32 offset = 0;
    u32 stride = 0;
    const u8* data = nullptr;
};

struct AttributeLayout {
    u32 array = kArrayCount;
    u32 offset = 0;
    const AttributeType* type = nullptr;
    u32 components = 0;
};

bool LoadAttributes(Host& host, const pica::DrawCall& draw, LoadedDraw& out, u32 vertex_count,
                    u32 alignment, bool copy_vertices) {
    const auto& state = *draw.state;
    const auto& regs = state.regs;
    const bool immediate = draw.kind == pica::DrawKind::Immediate;
    const u32 format_reg = pica::reg::kVertexAttributesBase + 1;
    const u32 format_high = regs[format_reg + 1];
    const u32 fixed_mask = (format_high >> 16) & 0xfff;
    const u32 shader_inputs = (regs[pica::reg::kVs + pica::reg::kShaderInputBufferConfig] & 0xf) + 1;
    const u32 input_map_reg = pica::reg::kVs + 0xb;
    const u32 attribute_count = immediate ? shader_inputs : std::min(shader_inputs, (format_high >> 28) + 1);

    std::array<ArrayLayout, kArrayCount> arrays{};
    std::array<AttributeLayout, kArrayCount> layout{};
    if (!immediate) {
        for (u32 array = 0; array < kArrayCount; ++array) {
            const u32 first_reg = format_reg + 2 + 3 * array;
            const u32 config = regs[first_reg + 2];
            const u32 stride = (config >> 16) & 0xff;
            if (stride == 0) {
                continue;
            }
            arrays[array] = {regs[first_reg] & 0x0fffffff, stride};
            const u32 slots = std::min(config >> 28, kArrayCount);
            u32 offset = 0;
            for (u32 slot = 0; slot < slots; ++slot) {
                const u32 attribute = Nibble(regs[first_reg + 1 + slot / 8], slot % 8);
                if (attribute >= kArrayCount) {
                    offset = AlignUp(offset, 4) + 4 * (attribute - kArrayCount + 1);
                    continue;
                }
                const u32 format = Nibble(regs[format_reg + attribute / 8], attribute % 8);
                const AttributeType& type = kTypes[format & 3];
                const u32 components = (format >> 2) + 1;
                offset = AlignUp(offset, type.element_bytes);
                layout[attribute] = {array, offset, &type, components};
                offset += components * type.element_bytes;
            }
        }
    }

    // Resolve register collisions before touching guest memory. An overwritten array may be unmapped.
    std::array<u32, kInputCount> register_attributes;
    register_attributes.fill(kInputCount);
    for (u32 attribute = 0; attribute < attribute_count; ++attribute) {
        const u32 location = Nibble(regs[input_map_reg + attribute / 8], attribute % 8);
        register_attributes[location] = attribute;
    }

    std::array<bool, kArrayCount> used{};
    std::array<const pica::Vec4f*, kInputCount> constants{};
    for (u32 location = 0; location < kInputCount; ++location) {
        const u32 attribute = register_attributes[location];
        auto& input = out.attributes[location];
        input.location = location;
        input.binding = kArrayCount;
        if (attribute == kInputCount) {
            continue;
        }
        if (immediate) {
            constants[location] = &state.immediate.input_vertex[attribute];
        } else if (attribute >= kArrayCount || ((fixed_mask >> attribute) & 1) != 0) {
            constants[location] = &state.default_attributes[attribute];
        } else if (const auto& entry = layout[attribute]; entry.type != nullptr) {
            used[entry.array] = true;
            input.binding = entry.array;
            input.offset = entry.offset;
            input.format = entry.type->formats[entry.components - 1];
            input.components = entry.components;
            input.conversion = entry.type->conversion;
            input.alias_w = GetFormatInfo(input.format).lanes > entry.components;
        }
    }

    std::array<u32, kArrayCount + 1> bindings{};
    u32 buffer_count = 0;
    for (u32 array = 0; array < kArrayCount; ++array) {
        if (!used[array]) {
            continue;
        }
        auto& src = arrays[array];
        const u64 address = u64{draw.vertex_base_address} + src.offset + u64{out.first_vertex} * src.stride;
        src.data = MapRange(host, address, u64{vertex_count} * src.stride);
        if (src.data == nullptr) {
            return false;
        }
        bindings[array] = buffer_count++;
    }

    bindings[kArrayCount] = buffer_count;
    out.buffers.resize(buffer_count + 1);
    for (u32 array = 0; array < kArrayCount; ++array) {
        if (!used[array]) {
            continue;
        }
        const auto& src = arrays[array];
        auto& buffer = out.buffers[bindings[array]];
        buffer.stride = AlignUp(src.stride, alignment);
        buffer.vertex_count = vertex_count;
        buffer.per_instance = false;
        buffer.source = src.data;
        buffer.source_stride = src.stride;
        if (copy_vertices) {
            buffer.data.resize(buffer.Bytes());
            buffer.CopyTo(buffer.data.data());
            buffer.source = nullptr;
            buffer.source_stride = 0;
        } else {
            buffer.data.clear();
        }
    }

    u32 constant_bytes = kFloat4Bytes;
    for (auto& input : out.attributes) {
        input.binding = bindings[input.binding];
        if (constants[input.location] != nullptr) {
            input.offset = constant_bytes;
            constant_bytes += kFloat4Bytes;
        }
    }
    auto& constant_buffer = out.buffers.back();
    constant_buffer.stride = AlignUp(constant_bytes, alignment);
    constant_buffer.vertex_count = 1;
    constant_buffer.per_instance = true;
    constant_buffer.source = nullptr;
    constant_buffer.source_stride = 0;
    constant_buffer.data.assign(static_cast<size_t>(constant_buffer.stride) + kVertexTailPadding, 0);
    constexpr pica::Vec4f missing{{0.0f, 0.0f, 0.0f, 1.0f}};
    std::memcpy(constant_buffer.data.data(), missing.c, kFloat4Bytes);
    for (const auto& input : out.attributes) {
        if (const auto* value = constants[input.location]) {
            std::memcpy(constant_buffer.data.data() + input.offset, value->c, kFloat4Bytes);
        }
    }
    return true;
}

} // namespace

FormatInfo GetFormatInfo(VertexFormat format) {
    const size_t slot = static_cast<size_t>(format);
    return slot < kFormats.size() ? kFormats[slot] : FormatInfo{};
}

const char* ToString(LoadStatus status) {
    switch (status) {
    case LoadStatus::Ok: return "ok";
    case LoadStatus::NoVertices: return "no vertices";
    case LoadStatus::Unmapped: return "unmapped";
    case LoadStatus::TooLarge: return "more vertices than any draw has";
    }
    return "unknown";
}

void VertexBuffer::CopyTo(u8* out) const {
    if (source == nullptr) {
        if (!data.empty()) {
            std::memcpy(out, data.data(), data.size());
        }
        return;
    }

    if (stride == source_stride) {
        const size_t bytes = static_cast<size_t>(stride) * vertex_count;
        std::memcpy(out, source, bytes);
        out += bytes;
    } else {
        const u8* vertex = source;
        for (u32 i = 0; i < vertex_count; ++i) {
            std::memcpy(out, vertex, source_stride);
            std::memset(out + source_stride, 0, stride - source_stride);
            vertex += source_stride;
            out += stride;
        }
    }
    std::memset(out, 0, kVertexTailPadding);
}

LoadedDraw VertexLoader::Load(Host& host, const pica::DrawCall& draw) {
    LoadedDraw out;
    LoadInto(host, draw, out);
    return out;
}

void VertexLoader::LoadInto(Host& host, const pica::DrawCall& draw, LoadedDraw& out,
                            bool copy_vertices, bool defer_indices) {
    // Keep the caller's buffer and index storage when resetting the draw description.
    LoadedDraw next;
    next.buffers.swap(out.buffers);
    next.index_data.swap(out.index_data);
    out = std::move(next);
    out.index_data.clear();

    const auto drop = [&](LoadStatus status) {
        out.status = status;
        out.buffers.clear();
        out.attributes = {};
        out.first_vertex = 0;
        out.list_low = 0;
        out.list_high = 0;
        ++counters_.draws_dropped;
    };

    const bool indexed = draw.kind == pica::DrawKind::Indexed;
    const bool immediate = draw.kind == pica::DrawKind::Immediate;
    const u32 count = immediate ? 1 : draw.num_vertices;
    if (count == 0) {
        drop(LoadStatus::NoVertices);
        return;
    }
    if (draw.kind == pica::DrawKind::Arrays &&
        (count > kMaxArraysVertices || u64{draw.vertex_offset} + count > (u64{1} << 32))) {
        drop(LoadStatus::TooLarge);
        return;
    }

    constexpr std::array topologies{Topology::TriangleList, Topology::TriangleStrip,
                                    Topology::TriangleFan, Topology::Shader};
    const u32 topology_reg = draw.state->regs[pica::reg::kTriangleTopology];
    out.topology = topologies[(topology_reg >> 8) & 3];
    const bool convert_fan = !immediate && out.topology == Topology::TriangleFan &&
                             (!caps_.triangle_fans || caps_.apple);
    if (convert_fan && count >= 3 && u64{count - 2} * 3 > std::numeric_limits<u32>::max()) {
        drop(LoadStatus::TooLarge);
        return;
    }

    const u8* index_source = nullptr;
    u32 vertex_count = count;
    if (indexed) {
        const u32 width = draw.index_u16 ? 2 : 1;
        index_source = MapRange(host, u64{draw.vertex_base_address} + draw.index_offset, u64{count} * width);
        if (index_source == nullptr) {
            drop(LoadStatus::Unmapped);
            return;
        }
        indices_.resize(count);
        u32 low = std::numeric_limits<u32>::max();
        u32 high = 0;
        const u8* read = index_source;
        for (u32& value : indices_) {
            value = *read++;
            if (draw.index_u16) {
                value |= static_cast<u32>(*read++) << 8;
            }
            low = std::min(low, value);
            high = std::max(high, value);
        }
        out.first_vertex = low;
        out.list_low = low;
        out.list_high = high;
        vertex_count = high - low + 1;
    } else if (!immediate) {
        out.first_vertex = draw.vertex_offset;
    }

    if (!LoadAttributes(host, draw, out, vertex_count, std::max(caps_.stride_alignment, 1u), copy_vertices)) {
        drop(LoadStatus::Unmapped);
        return;
    }

    out.count = count;
    const bool native_u8 = caps_.index_uint8 && !caps_.apple;
    if (indexed) {
        out.base_vertex = -static_cast<s32>(out.first_vertex);
        out.list = indices_;
        out.index_source = index_source;
        out.index_source_u16 = draw.index_u16;
    }

    if (convert_fan) {
        out.topology = Topology::TriangleList;
        out.fan_converted = true;
        ++counters_.fans_converted;
        triangles_.clear();
        if (count >= 3) {
            triangles_.reserve(static_cast<size_t>(count - 2) * 3);
            const u32 first = indexed ? indices_.front() : 0;
            for (u32 i = 1; i < count - 1; ++i) {
                triangles_.push_back(first);
                triangles_.push_back(indexed ? indices_[i] : i);
                triangles_.push_back(indexed ? indices_[i + 1] : i + 1);
            }
        }
        out.list = triangles_;
        if (!indexed) {
            out.list_high = count - 1;
        }
        PackList(out, indexed, draw.index_u16, native_u8);
    } else if (indexed && defer_indices) {
        out.indices_deferred = true;
        deferred_source_u16_ = draw.index_u16;
        deferred_native_u8_ = native_u8;
    } else if (indexed) {
        PackList(out, true, draw.index_u16, native_u8);
    }
    ++counters_.draws_loaded;
}

void VertexLoader::PackIndices(LoadedDraw& out) {
    if (out.indices_deferred) {
        PackList(out, true, deferred_source_u16_, deferred_native_u8_);
    }
}

void VertexLoader::PackList(LoadedDraw& out, bool indexed, bool source_u16, bool native_u8) {
    const auto values = out.list;
    out.list = {};
    out.indices_deferred = false;
    out.count = static_cast<u32>(values.size());
    if (values.empty()) {
        return;
    }

    const bool source_u8 = indexed && !source_u16;
    u32 width = 2;
    if (source_u8 && native_u8) {
        width = 1;
    } else if (out.list_high > 0xffff) {
        width = 4;
    }
    if (caps_.primitive_restart_always_on &&
        ((width == 1 && out.list_high == 0xff) || (width == 2 && out.list_high == 0xffff))) {
        width *= 2;
        out.restart_widened = true;
        ++counters_.restart_widened;
    }

    out.indices_widened_u8 = source_u8 && width > 1;
    constexpr std::array types{IndexType::None, IndexType::U8, IndexType::U16, IndexType::None, IndexType::U32};
    out.index_type = types[width];
    out.index_data.resize(values.size() * width);
    u8* write = out.index_data.data();
    for (u32 value : values) {
        for (u32 byte = 0; byte < width; ++byte) {
            *write++ = static_cast<u8>(value);
            value >>= 8;
        }
    }
}

} // namespace mh4n::gpu::vertex
