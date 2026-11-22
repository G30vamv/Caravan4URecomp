// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: files.h, provider_host.h, gpu_pipeline_test and record files emitted by the shipped cache.

#include "files.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <limits>

#include "cache_bytes.h"
#include "gpu/native/provider_host.h"

namespace mh4n::gpu::pipeline {
namespace {

constexpr std::array<u8, 8> kMagic{'M', 'H', '4', 'N', 'P', 'I', 'P', 'E'};
constexpr size_t kHeaderSize = kMagic.size() + sizeof(u32) * 2 + sizeof(u64) * 2;

u64 Checksum(std::span<const u8> bytes) {
    return native::detail::HashBytes(bytes.data(), bytes.size(), 0x46494c45ull);
}

bool Fail(std::string* error, std::string message) {
    if (error) {
        *error = std::move(message);
    }
    return false;
}

}  // namespace

const char* ToString(ReadStatus status) {
    switch (status) {
    case ReadStatus::Ok: return "ok";
    case ReadStatus::Missing: return "missing";
    case ReadStatus::Stale: return "stale";
    case ReadStatus::Damaged: return "damaged";
    }
    return "unknown";
}

bool WriteBytes(const std::filesystem::path& path, std::span<const u8> bytes, std::string* error) {
    if (error) {
        error->clear();
    }
    if (bytes.size() > static_cast<u64>(std::numeric_limits<std::streamsize>::max())) {
        return Fail(error, "file too large");
    }

    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            return Fail(error, ec.message());
        }
    }

    static std::atomic<u64> sequence{0};
    auto temporary = path;
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    temporary += ".tmp-" + std::to_string(ticks) + "-" + std::to_string(sequence.fetch_add(1));
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) {
        return Fail(error, "could not open temporary file");
    }
    if (!bytes.empty()) {
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    file.close();
    if (file) {
        std::filesystem::rename(temporary, path, ec);
        if (!ec) {
            return true;
        }
    }
    const auto message = ec ? ec.message() : "could not write temporary file";
    std::filesystem::remove(temporary, ec);
    return Fail(error, message);
}

std::optional<std::vector<u8>> ReadBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return std::nullopt;
    }
    const std::streamoff size = file.tellg();
    if (size < 0 || static_cast<u64>(size) > std::vector<u8>().max_size() ||
        size > std::numeric_limits<std::streamsize>::max()) {
        return std::nullopt;
    }
    std::vector<u8> bytes(static_cast<size_t>(size));
    file.seekg(0);
    if (size != 0 && !file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) {
        return std::nullopt;
    }
    return bytes;
}

bool WriteRecords(const std::filesystem::path& path, u32 kind, u64 version_a, u64 version_b,
                  const Records& records, std::string* error) {
    constexpr auto kMaxWord = std::numeric_limits<u32>::max();
    if (records.size() > kMaxWord) {
        return Fail(error, "too many records");
    }
    std::vector<u8> bytes(kMagic.begin(), kMagic.end());
    disk::Append(bytes, kind);
    disk::Append(bytes, version_a);
    disk::Append(bytes, version_b);
    disk::Append(bytes, static_cast<u32>(records.size()));
    for (const auto& record : records) {
        if (record.size() > kMaxWord) {
            return Fail(error, "record too large");
        }
        disk::Append(bytes, static_cast<u32>(record.size()));
        bytes.insert(bytes.end(), record.begin(), record.end());
    }
    disk::Append(bytes, Checksum(bytes));
    return WriteBytes(path, bytes, error);
}

ReadStatus ReadRecords(const std::filesystem::path& path, u32 kind, u64 version_a, u64 version_b, Records& records) {
    records.clear();
    const auto bytes = ReadBytes(path);
    if (!bytes) {
        std::error_code ec;
        const bool exists = std::filesystem::exists(path, ec);
        return !exists && !ec ? ReadStatus::Missing : ReadStatus::Damaged;
    }
    if (bytes->size() < kHeaderSize + sizeof(u64)) {
        return ReadStatus::Damaged;
    }
    const auto contents = std::span(*bytes).first(bytes->size() - sizeof(u64));
    disk::Reader footer(std::span(*bytes).last(sizeof(u64)));
    disk::Reader reader(contents);
    if (!std::ranges::equal(reader.Take(kMagic.size()), kMagic) || footer.Read<u64>() != Checksum(contents)) {
        return ReadStatus::Damaged;
    }
    const u32 file_kind = reader.Read<u32>();
    const u64 file_version_a = reader.Read<u64>();
    const u64 file_version_b = reader.Read<u64>();
    if (file_kind != kind || file_version_a != version_a || file_version_b != version_b) {
        return ReadStatus::Stale;
    }

    const u32 count = reader.Read<u32>();
    if (count > reader.Remaining() / sizeof(u32)) {
        return ReadStatus::Damaged;
    }
    Records decoded;
    decoded.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        if (reader.Remaining() < sizeof(u32)) {
            return ReadStatus::Damaged;
        }
        const u32 size = reader.Read<u32>();
        const auto record = reader.Take(size);
        if (record.size() != size) {
            return ReadStatus::Damaged;
        }
        decoded.emplace_back(record.begin(), record.end());
    }
    if (!reader.Done()) {
        return ReadStatus::Damaged;
    }
    records = std::move(decoded);
    return ReadStatus::Ok;
}

}  // namespace mh4n::gpu::pipeline
