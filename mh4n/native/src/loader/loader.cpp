// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: mh4n/loader.h, mh4n/memory.h, mod_list.h, code_patch.h and loader_test.cpp.

#include "image_fields.h"
#include "mh4n/crc32.h"
#include "mh4n/loader.h"

#include "../common/utf8.h"
#include "../mods/mod_list.h"

#include <array>
#include <cstring>
#include <fmt/format.h>

namespace mh4n {
namespace {

u64 CodeSize(const ExHeader& header) {
    return (u64{header.text.num_pages} + header.ro.num_pages + header.data.num_pages) * 0x1000;
}

}  // namespace

bool CheckExheaderOverride(const ExHeader& game, const ExHeader& mod, std::size_t code_size,
                           std::string* error) {
    using loader_detail::Fail;
    if (code_patch::ModId(game.program_id) != code_patch::ModId(mod.program_id)) {
        return Fail(error, "the exheader is not this game's");
    }
    if (game.text.address != mod.text.address || game.ro.address != mod.ro.address ||
        game.data.address != mod.data.address) {
        return Fail(error, "the exheader's segments are not where the game's are");
    }
    if (CodeSize(mod) != code_size) {
        return Fail(error, "code.bin and exheader.bin are not from the same build");
    }
    return true;
}

std::unique_ptr<Loader> Loader::OpenGameFolder(const std::string& dir, std::string* error) {
    return OpenGameFolder(dir, "", error);
}

std::unique_ptr<Loader> Loader::OpenGameFolder(const std::string& dir, const std::string& load_dir,
                                               std::string* error) {
    using namespace loader_detail;
    const auto folder = utf8::PathFromUtf8(dir);
    const auto exheader_path = folder / "exheader.bin";
    const auto code_path = folder / "code.bin";
    std::vector<u8> exheader, code;
    ExHeader game;
    if (!ReadFile(exheader_path, utf8::PathToUtf8(exheader_path), exheader, error) ||
        !ParseExHeader(exheader.data(), exheader.size(), &game, error) ||
        !ReadFile(code_path, utf8::PathToUtf8(code_path), code, error)) {
        return nullptr;
    }

    const auto name = game.title.empty() ? utf8::PathToUtf8(folder.filename()) : game.title;
    const auto enabled = load_dir.empty() ? std::vector<mods::Mod>{} :
        mods::Enabled(utf8::PathToUtf8(utf8::PathFromUtf8(load_dir) / "mods") + "/", game.program_id);
    const auto mod_folders = mods::CodeFolders(enabled);
    if (mod_folders.empty()) {
        return FromExHeader(game, std::move(code), name, error);
    }

    ExHeader header = game;
    std::string override_folder;
    const auto replacement = code_patch::ExheaderOverrideOf(mod_folders, &override_folder);
    std::string detail;
    if (replacement && !ParseExHeader(replacement->data(), replacement->size(), &header, &detail)) {
        Fail(error, detail);
        return nullptr;
    }

    const u64 bss_size = RoundPage(header.bss_size);
    if (bss_size > kManagedEnd) {
        Fail(error, "invalid exheader BSS size");
        return nullptr;
    }
    const auto outcome = mods::ApplyCode(enabled, code, static_cast<u32>(bss_size / 0x1000));
    if (!outcome.ok) {
        Fail(error, outcome.error);
        return nullptr;
    }

    const auto code_size = code.size() - static_cast<std::size_t>(bss_size);
    if (replacement && !CheckExheaderOverride(game, header, code_size, &detail)) {
        auto path = utf8::PathFromUtf8(override_folder);
        if (path.filename().empty()) {
            path = path.parent_path();
        }
        Fail(error, "mods: " + utf8::PathToUtf8(path.filename()) + "/exheader.bin: " + detail);
        return nullptr;
    }
    if (code_size != CodeSize(header)) {
        Fail(error, "code.bin and exheader.bin sizes differ (were they made from the same title?)");
        return nullptr;
    }

    std::vector<u8> bss(code.begin() + code_size, code.end());
    code.resize(code_size);
    auto loader = FromExHeader(header, std::move(code), name, error);
    if (loader && !bss.empty()) {
        auto& data = loader->pieces_[loader->pieces_.size() - 2];
        data.data_size += bss.size();
        loader->code_.insert(loader->code_.end(), bss.begin(), bss.end());
    }
    return loader;
}

std::unique_ptr<Loader> Loader::FromExHeader(const ExHeader& header, std::vector<u8> code,
                                             std::string name, std::string* error) {
    using namespace loader_detail;
    const auto reject = [error](const char* message) {
        Fail(error, message);
        return nullptr;
    };
    if (header.system_mode > 5 || !FcramRegions::ValidMemoryMode(header.system_mode)) {
        return reject("unsupported Old 3DS memory mode");
    }
    const auto region = header.kernel_caps.MemoryRegion();
    if (region < 1 || region > 3) {
        return reject("invalid FCRAM allocation region");
    }
    if (CodeSize(header) != code.size()) {
        return reject("code.bin size does not match the exheader");
    }
    if (header.stack_size > kStackTop || (header.stack_size & 0xFFF) != 0) {
        return reject("invalid stack size");
    }

    auto loader = std::unique_ptr<Loader>(new Loader);
    auto& image = loader->image_;
    image.name = std::move(name);
    image.program_id = header.program_id;
    image.entry = header.text.address;
    image.text_base = header.text.address;
    image.text_size = header.text.size;
    image.stack_size = header.stack_size == 0 ? kDefaultStackSize : header.stack_size;
    image.main_thread_priority = static_cast<s32>(header.priority);
    image.ideal_processor = header.ideal_processor;
    image.memory_mode = header.system_mode;
    image.kernel_version = header.kernel_caps.kernel_version.value_or(0);
    image.linear_heap_area = LinearHeapAreaForKernelVersion(image.kernel_version);
    image.memory_region = static_cast<FcramRegion>(region - 1);
    image.resource_limit_category = header.resource_limit_category;
    image.apply_app_max_cpu = image.resource_limit_category == 0;
    image.core1_schedule_mode = header.schedule_mode;
    image.core1_max_cpu = header.max_cpu;
    image.svc_mask = header.kernel_caps.svc_mask;
    image.address_mappings = header.kernel_caps.address_mappings;
    image.handle_table_size = header.kernel_caps.handle_table_size;

    const std::array segments{header.text, header.ro, header.data};
    const std::array names{"text", "ro", "data"};
    const std::array perms{MemPerm::ReadExecute, MemPerm::Read, MemPerm::ReadWrite};
    std::size_t offset = 0;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const auto& segment = segments[i];
        const u64 bytes = u64{segment.num_pages} * 0x1000;
        const u64 mapped = bytes + (i == 2 ? RoundPage(header.bss_size) : 0);
        if (segment.size > bytes || u64{segment.address} + mapped > kManagedEnd ||
            (mapped && (segment.address & 0xFFF) != 0) || (i == 0 && !bytes)) {
            return reject("invalid exheader segment");
        }
        if (mapped) {
            const auto size = static_cast<u32>(mapped);
            image.segments.push_back({names[i], segment.address, size, perms[i],
                                      i == 2 ? MemState::Private : MemState::Code});
            loader->pieces_.push_back({segment.address, size, offset, static_cast<std::size_t>(bytes)});
        }
        offset += static_cast<std::size_t>(bytes);
    }
    const u32 stack_base = image.stack_top - image.stack_size;
    image.segments.push_back({"stack", stack_base, image.stack_size, MemPerm::ReadWrite, MemState::Locked});
    loader->pieces_.push_back({stack_base, image.stack_size, 0, 0});
    loader->code_ = std::move(code);
    return loader;
}

bool Loader::Map(GuestMemory& memory, FcramRegions& regions, std::string* error) {
    using loader_detail::Fail;
    if (mapped_) {
        return Fail(error, "process image is already mapped");
    }
    if (regions.MemoryMode() != image_.memory_mode) {
        return Fail(error, "FCRAM memory mode does not match the process image");
    }

    struct Allocation {
        u32 vaddr;
        FcramRange range;
        bool mapped;
    };
    std::vector<Allocation> allocations;
    const auto rollback = [&](const char* message) {
        for (auto it = allocations.rbegin(); it != allocations.rend(); ++it) {
            if (it->mapped) {
                memory.Unmap(it->vaddr, it->range.size);
            }
            regions.Free(image_.memory_region, it->range.offset, it->range.size);
        }
        return Fail(error, message);
    };
    for (std::size_t i = 0; i < pieces_.size(); ++i) {
        const auto& piece = pieces_[i];
        const auto& segment = image_.segments[i];
        const auto ranges = regions.HeapAllocate(image_.memory_region, piece.size);
        if (ranges.empty()) {
            return rollback("not enough FCRAM for the process image");
        }
        const auto first = allocations.size();
        u32 vaddr = piece.vaddr;
        for (const auto& range : ranges) {
            allocations.push_back({vaddr, range, false});
            vaddr += range.size;
        }
        for (std::size_t index = first; index < allocations.size(); ++index) {
            auto& allocation = allocations[index];
            const Result result = memory.Map(allocation.vaddr, allocation.range.size, segment.perm,
                                             segment.state, u64{kFcramPaddr} + allocation.range.offset);
            if (!IsSuccess(result)) {
                return rollback("process segment overlaps an existing mapping or has an invalid address");
            }
            allocation.mapped = true;
        }
        if (piece.data_size != 0) {
            std::memcpy(memory.Base() + piece.vaddr, code_.data() + piece.offset, piece.data_size);
        }
    }
    memory.SetLinearHeapArea(image_.linear_heap_area);
    mapped_ = true;
    return true;
}

std::span<const u8> Loader::TextBytes(const GuestMemory& memory) const {
    if (!mapped_ || !memory.IsMapped(image_.text_base, image_.text_size)) {
        return {};
    }
    return {memory.Base() + image_.text_base, image_.text_size};
}

u32 Loader::TextCrc32(const GuestMemory& memory) const {
    const auto text = TextBytes(memory);
    return Crc32(text.data(), text.size());
}

std::string DescribeProcessImage(const ProcessImage& image) {
    std::string description = fmt::format(
        "name                 : {}\n"
        "program id           : 0x{:016X}\n"
        "entry                : 0x{:08X}\n"
        "stack top            : 0x{:08X}\n"
        "stack size           : 0x{:X}\n"
        "main thread priority : {}\n"
        "ideal processor      : {}\n"
        "main thread fpscr    : 0x{:08X}\n"
        "memory mode          : {}\n"
        "kernel version       : 0x{:X}\n"
        "linear heap area     : 0x{:08X}\n"
        "memory region        : {}\n"
        "resource category    : {}\n"
        "core 1 schedule mode : {} ({}), max cpu {}\n",
        image.name, image.program_id, image.entry, image.stack_top, image.stack_size,
        image.main_thread_priority, image.ideal_processor, image.main_thread_fpscr, image.memory_mode,
        image.kernel_version, image.linear_heap_area, static_cast<u32>(image.memory_region),
        image.resource_limit_category, image.core1_schedule_mode,
        image.core1_schedule_mode == 0 ? "single" : "multi", image.core1_max_cpu);
    for (const auto& segment : image.segments) {
        const unsigned perm = static_cast<unsigned>(segment.perm);
        const char* state = segment.state == MemState::Code ? "Code" :
                            segment.state == MemState::Locked ? "Locked" : "Private";
        description += fmt::format("{:<6} 0x{:08X}-0x{:08X} size 0x{:08X} {}{}{} {}\n",
                                   segment.name, segment.vaddr, segment.vaddr + segment.size, segment.size,
                                   perm & 1 ? 'r' : '-', perm & 2 ? 'w' : '-', perm & 4 ? 'x' : '-', state);
    }
    return description;
}

}  // namespace mh4n
