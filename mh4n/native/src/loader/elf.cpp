// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: ELF32 program header format, mh4n/loader.h and loader_test.cpp.

#include "image_fields.h"
#include "mh4n/loader.h"

#include "../common/utf8.h"

namespace mh4n {

std::unique_ptr<Loader> Loader::OpenElf(const std::string& path, std::string* error) {
    const auto file = utf8::PathFromUtf8(path);
    std::vector<u8> bytes;
    if (!loader_detail::ReadFile(file, path, bytes, error)) {
        return nullptr;
    }
    return FromElf(std::move(bytes), utf8::PathToUtf8(file.filename()), error);
}

std::unique_ptr<Loader> Loader::FromElf(std::vector<u8> bytes, std::string name, std::string* error) {
    using namespace loader_detail;
    const auto reject = [error](const char* message) {
        Fail(error, message);
        return nullptr;
    };
    if (bytes.size() < 4 || bytes[0] != 0x7F || bytes[1] != 'E' || bytes[2] != 'L' || bytes[3] != 'F') {
        return reject("not an ELF file");
    }
    if (bytes.size() < 52 || bytes[4] != 1 || bytes[5] != 1 || bytes[6] != 1 ||
        ReadLe(bytes.data() + 18, 2) != 40 || ReadLe(bytes.data() + 20, 4) != 1) {
        return reject("expected a 32-bit little-endian ARM ELF");
    }
    const u64 type = ReadLe(bytes.data() + 16, 2);
    if (type != 2 && type != 3) {
        return reject("unsupported ELF type");
    }
    const u32 base = type == 3 ? kProcessImageVaddr : 0;
    const u64 entry = base + ReadLe(bytes.data() + 24, 4);
    const u64 table = ReadLe(bytes.data() + 28, 4);
    const u64 stride = ReadLe(bytes.data() + 42, 2);
    const u64 count = ReadLe(bytes.data() + 44, 2);
    if (entry >= kManagedEnd || stride < 32 || table + count * stride > bytes.size()) {
        return reject("invalid ELF entry or program header table");
    }

    auto loader = std::unique_ptr<Loader>(new Loader);
    auto& image = loader->image_;
    image.name = std::move(name);
    image.kind = ProgramKind::Elf;
    image.entry = static_cast<u32>(entry);
    image.kernel_version = kElfKernelVersion;
    image.memory_mode = kElfMemoryMode;
    image.main_thread_priority = kElfMainThreadPriority;
    image.linear_heap_area = LinearHeapAreaForKernelVersion(image.kernel_version);
    image.core1_schedule_mode = 1;
    image.core1_max_cpu = 89;
    image.svc_mask.set();
    image.address_mappings = {{0x1FF50000, 0x8000, true, false}, {0x1FF70000, 0x8000, true, false},
                              {0x1F000000, 0x600000, false, false}};

    struct Segment {
        SegmentMapping mapping;
        Piece piece;
    };
    std::array<std::optional<Segment>, 3> segments;
    for (u64 i = 0; i < count; ++i) {
        const u8* ph = bytes.data() + table + i * stride;
        if (ReadLe(ph, 4) != 1) {
            continue;
        }
        const u64 flags = ReadLe(ph + 24, 4) & 7;
        const std::size_t index = flags == 5 ? 0 : flags == 4 ? 1 : flags == 6 ? 2 : 3;
        if (index == 3 || segments[index]) {
            continue;
        }
        const u64 file_offset = ReadLe(ph + 4, 4);
        const u64 address = base + ReadLe(ph + 8, 4);
        const u64 file_size = ReadLe(ph + 16, 4);
        const u64 mem_size = ReadLe(ph + 20, 4);
        if (file_size > mem_size || file_offset + file_size > bytes.size() ||
            address + mem_size > kManagedEnd) {
            return reject("invalid ELF load segment");
        }
        if (mem_size == 0) {
            continue;
        }

        const u64 page_offset = address & 0xFFF;
        const auto vaddr = static_cast<u32>(address - page_offset);
        const auto size = static_cast<u32>(RoundPage(page_offset + mem_size));
        const std::array names{"text", "ro", "data"};
        const std::array perms{MemPerm::ReadExecute, MemPerm::Read, MemPerm::ReadWrite};
        const auto offset = loader->code_.size();
        loader->code_.resize(offset + static_cast<std::size_t>(page_offset));
        loader->code_.insert(loader->code_.end(), bytes.begin() + file_offset,
                             bytes.begin() + file_offset + file_size);
        segments[index] = Segment{
            {names[index], vaddr, size, perms[index], index == 2 ? MemState::Private : MemState::Code},
            {vaddr, size, offset, static_cast<std::size_t>(page_offset + file_size)}};
        if (index == 0) {
            image.text_base = static_cast<u32>(address);
            image.text_size = static_cast<u32>(file_size);
        }
    }
    if (!segments[0]) {
        return reject("ELF has no executable load segment");
    }
    for (auto& segment : segments) {
        if (segment) {
            image.segments.push_back(std::move(segment->mapping));
            loader->pieces_.push_back(segment->piece);
        }
    }
    const u32 stack_base = image.stack_top - image.stack_size;
    image.segments.push_back({"stack", stack_base, image.stack_size, MemPerm::ReadWrite, MemState::Locked});
    loader->pieces_.push_back({stack_base, image.stack_size, 0, 0});
    return loader;
}

}  // namespace mh4n
