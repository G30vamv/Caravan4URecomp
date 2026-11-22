// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: cheat_engine.h, gateway_cheat.h and cheats_test.cpp.

#include "cheat_engine.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

namespace mh4n::cheats {
namespace {

std::mutex current_mutex;
std::mutex save_mutex;
std::shared_ptr<CheatEngine> current_engine;

bool Fail(std::string* message, const char* reason) {
    if (message) *message = reason;
    return false;
}

void AppendBigEndian(std::vector<u8>& out, u32 value, unsigned width) {
    while (width > 0) {
        out.push_back(static_cast<u8>(value >> (8 * --width)));
    }
}

} // namespace

std::string CheatFilePath(const std::string& user_folder, u64 title_id) {
    if (user_folder.empty()) return {};
    std::ostringstream filename;
    filename << std::uppercase << std::hex << std::setfill('0') << std::setw(16) << title_id << ".txt";
    return (std::filesystem::path(user_folder) / "cheats" / filename.str()).generic_string();
}

CheatEngine::CheatEngine(std::string path) : path_(std::move(path)) {}

std::size_t CheatEngine::Load() {
    auto next = GatewayCheat::LoadFile(path_);
    std::unique_lock lock(cheats_list_mutex_);
    cheats_list_.swap(next);
    Changed();
    return cheats_list_.size();
}

bool CheatEngine::Save(std::string* error) const {
    if (path_.empty()) return Fail(error, "no_path");
    std::lock_guard save_lock(save_mutex);
    std::string text;
    {
        std::shared_lock lock(cheats_list_mutex_);
        for (const auto& cheat : cheats_list_) text += cheat->ToString();
    }
    const auto parent = std::filesystem::path(path_).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) return Fail(error, "unwritable");
    }
    auto pending = std::filesystem::path(path_);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    pending += ".tmp." + std::to_string(stamp);
    std::ofstream file(pending, std::ios::binary | std::ios::trunc);
    file << text;
    file.close();
    std::error_code ec;
    if (file) {
        std::filesystem::rename(pending, std::filesystem::path(path_), ec);
        if (!ec) {
            if (error) error->clear();
            return true;
        }
    }
    std::filesystem::remove(pending, ec);
    return Fail(error, "unwritable");
}

std::vector<CheatInfo> CheatEngine::List() const {
    std::shared_lock lock(cheats_list_mutex_);
    std::vector<CheatInfo> entries;
    entries.reserve(cheats_list_.size());
    const u32 begin = code_begin_.load(std::memory_order_relaxed);
    const u32 end = code_end_.load(std::memory_order_relaxed);
    for (const auto& cheat : cheats_list_) {
        entries.push_back({cheat->GetName(), cheat->GetComments(), cheat->GetCode(), cheat->IsEnabled(),
                           cheat->InvalidLines(), cheat->TouchesCode(begin, end)});
    }
    return entries;
}

std::size_t CheatEngine::Count() const {
    std::shared_lock lock(cheats_list_mutex_);
    return cheats_list_.size();
}

std::size_t CheatEngine::EnabledCount() const {
    std::shared_lock lock(cheats_list_mutex_);
    return static_cast<std::size_t>(std::count_if(cheats_list_.begin(), cheats_list_.end(),
                                                  [](const auto& cheat) { return cheat->IsEnabled(); }));
}

u64 CheatEngine::Revision() const {
    return revision_.load(std::memory_order_relaxed);
}

bool CheatEngine::SetEnabled(std::size_t index, bool enabled) {
    std::unique_lock lock(cheats_list_mutex_);
    if (index >= cheats_list_.size()) return false;
    auto& cheat = cheats_list_[index];
    if (cheat->IsEnabled() != enabled) {
        cheat->SetEnabled(enabled);
        Changed();
    }
    return true;
}

bool CheatEngine::SetEnabled(const std::string& name, bool enabled) {
    std::unique_lock lock(cheats_list_mutex_);
    const auto found = std::find_if(cheats_list_.begin(), cheats_list_.end(),
                                    [&](const auto& cheat) { return cheat->GetName() == name; });
    if (found == cheats_list_.end()) return false;
    if ((*found)->IsEnabled() != enabled) {
        (*found)->SetEnabled(enabled);
        Changed();
    }
    return true;
}

bool CheatEngine::Remove(std::size_t index) {
    std::unique_lock lock(cheats_list_mutex_);
    if (index >= cheats_list_.size()) return false;
    cheats_list_.erase(cheats_list_.begin() + static_cast<std::ptrdiff_t>(index));
    Changed();
    return true;
}

int CheatEngine::Import(const std::string& path, std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        Fail(error, "unreadable");
        return -1;
    }
    std::ostringstream text;
    text << input.rdbuf();
    auto imports = GatewayCheat::Parse(text.str());
    if (imports.empty()) {
        Fail(error, "no_cheats");
        return -1;
    }

    std::unique_lock lock(cheats_list_mutex_);
    for (auto& cheat : imports) {
        const auto found = std::find_if(cheats_list_.begin(), cheats_list_.end(),
                                        [&](const auto& entry) { return entry->GetName() == cheat->GetName(); });
        cheat->SetEnabled(false);
        if (found == cheats_list_.end()) cheats_list_.push_back(std::move(cheat));
        else *found = std::move(cheat);
    }
    Changed();
    if (error) error->clear();
    return static_cast<int>(imports.size());
}

void CheatEngine::SetCodeRange(u32 begin, u32 end) {
    std::unique_lock lock(cheats_list_mutex_);
    code_begin_.store(begin, std::memory_order_relaxed);
    code_end_.store(end, std::memory_order_relaxed);
    Changed();
}

bool CheatEngine::CodePatch(std::size_t index, std::vector<u8>* ips, std::string* why) const {
    std::shared_ptr<const GatewayCheat> cheat;
    u32 begin;
    u32 end;
    {
        std::shared_lock lock(cheats_list_mutex_);
        begin = code_begin_.load(std::memory_order_relaxed);
        end = code_end_.load(std::memory_order_relaxed);
        if (begin >= end) return Fail(why, "no_code_range");
        if (index >= cheats_list_.size()) return Fail(why, "none");
        cheat = cheats_list_[index];
    }
    std::map<u32, u8> bytes;
    if (!cheat->CodeWrites(begin, end, &bytes, why)) return false;

    std::vector<u8> patch{'P', 'A', 'T', 'C', 'H'};
    for (auto it = bytes.cbegin(); it != bytes.cend();) {
        const u32 start = it->first;
        const u32 offset = start - begin;
        if (offset == 0x454F46) return Fail(why, "ips_eof");
        if (offset > 0xFFFFFF) return Fail(why, "ips_range");

        auto next = it;
        u32 length = 0;
        do {
            ++length;
            ++next;
        } while (next != bytes.cend() && next->first - start == length && length < 0xFFFF);
        AppendBigEndian(patch, offset, 3);
        AppendBigEndian(patch, length, 2);
        for (; it != next; ++it) patch.push_back(it->second);
    }
    patch.insert(patch.end(), {'E', 'O', 'F'});
    if (ips) *ips = std::move(patch);
    if (why) why->clear();
    return true;
}

void CheatEngine::Run(CheatTarget& target) const {
    std::shared_lock lock(cheats_list_mutex_);
    for (const auto& cheat : cheats_list_) {
        if (cheat->IsEnabled()) cheat->Execute(target);
    }
}

std::shared_ptr<CheatEngine> Current() {
    std::lock_guard lock(current_mutex);
    return current_engine;
}

void SetCurrent(std::shared_ptr<CheatEngine> engine) {
    std::lock_guard lock(current_mutex);
    current_engine.swap(engine);
}

} // namespace mh4n::cheats
