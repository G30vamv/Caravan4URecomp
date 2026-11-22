// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: 3dbrew Config Savegame, Flash Filesystem, SecureInfo_A and Cfg:GenHashConsoleUnique, golden boot-village.ipc.trace and FIPS 180-4.

#include "cfg_save.h"

#include <array>
#include <bit>
#include <fstream>
#include <string_view>
#include <utility>

namespace mh4n::wp11 {
namespace {

constexpr std::size_t kConfigSize = 0x8000;
constexpr std::size_t kEntrySize = 12;
constexpr std::size_t kHeaderSize = 4;
constexpr u16 kPublicFlags = kUserRead | kSystemRead | 0x4;

constexpr u64 ReadLittleEndian(std::span<const u8> bytes) {
    u64 value = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        value |= u64{bytes[i]} << (8 * i);
    }
    return value;
}

std::filesystem::path FindConfig(const std::filesystem::path& nand) {
    const auto data = nand / "data";
    const auto save = std::filesystem::path("sysdata/00010017/00000000/config");
    const auto default_path = data / "00000000000000000000000000000000" / save;
    std::error_code error;
    if (std::filesystem::is_regular_file(default_path, error)) {
        return default_path;
    }

    // A decrypted NAND export can have a real ID0. Avoid selecting between multiple profiles.
    std::filesystem::path found;
    std::filesystem::directory_iterator entry(data, error);
    for (; !error && entry != std::filesystem::directory_iterator{}; entry.increment(error)) {
        const auto id = entry->path().filename().native();
        if (id.size() != 32 || id.find_first_not_of(
                std::filesystem::path("0123456789abcdefABCDEF").native()) != id.npos) {
            continue;
        }
        auto candidate = entry->path() / save;
        std::error_code status_error;
        if (std::filesystem::is_regular_file(candidate, status_error)) {
            if (!found.empty()) {
                return {};
            }
            found = std::move(candidate);
        }
    }
    return error ? std::filesystem::path{} : found;
}

u8 ReadRegion(const std::filesystem::path& nand) {
    for (const char* name : {"SecureInfo_A", "SecureInfo_B"}) {
        std::ifstream file(nand / "rw" / "sys" / name, std::ios::binary);
        file.seekg(0x100);
        const int region = file.get();
        if (region >= 0 && region <= 6) {
            return static_cast<u8>(region);
        }
    }
    return 1;  // USA
}

constexpr u32 ReverseBytes(u32 word) {
    return (word << 24) | ((word & 0x0000FF00) << 8) |
           ((word & 0x00FF0000) >> 8) | (word >> 24);
}

// SHA-256 needs exactly one padded block for the u64 console ID and u32 salt.
u64 HashTransferableId(u64 console_id, u32 salt) {
    constexpr std::array<u32, 64> round_constants{
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };
    std::array<u32, 64> words{};
    words[0] = ReverseBytes(static_cast<u32>(console_id));
    words[1] = ReverseBytes(static_cast<u32>(console_id >> 32));
    words[2] = ReverseBytes(salt);
    words[3] = 0x80000000;
    words[15] = 96;

    for (std::size_t i = 16; i < words.size(); ++i) {
        const u32 x = words[i - 15];
        const u32 y = words[i - 2];
        const u32 sigma0 = std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3);
        const u32 sigma1 = std::rotr(y, 17) ^ std::rotr(y, 19) ^ (y >> 10);
        words[i] = words[i - 16] + sigma0 + words[i - 7] + sigma1;
    }

    constexpr std::array<u32, 8> initial{
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    auto [a, b, c, d, e, f, g, h] = initial;
    for (std::size_t i = 0; i < words.size(); ++i) {
        const u32 sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const u32 choose = (e & f) ^ (~e & g);
        const u32 temp1 = h + sum1 + choose + round_constants[i] + words[i];
        const u32 sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const u32 majority = (a & b) ^ (a & c) ^ (b & c);
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + sum0 + majority;
    }

    return ReverseBytes(initial[6] + g) | (u64{ReverseBytes(initial[7] + h)} << 32);
}

}  // namespace

ConfigSave::ConfigSave(const std::filesystem::path& user_folder) {
    const auto nand = user_folder / "nand";
    region_ = ReadRegion(nand);
    std::ifstream file(FindConfig(nand), std::ios::binary);
    std::array<u8, kConfigSize> bytes{};
    if (file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()) &&
        file.peek() == std::char_traits<char>::eof()) {
        const std::span<const u8> data = bytes;
        const auto count = ReadLittleEndian(data.first(2));
        const auto data_offset = ReadLittleEndian(data.subspan(2, 2));
        const auto table_end = kHeaderSize + count * kEntrySize;
        if (table_end <= data_offset && data_offset <= data.size()) {
            for (std::size_t i = 0; i < count; ++i) {
                const auto entry = data.subspan(kHeaderSize + i * kEntrySize, kEntrySize);
                const auto id = static_cast<u32>(ReadLittleEndian(entry.first(4)));
                const auto flags = static_cast<u16>(ReadLittleEndian(entry.last(2)));
                const auto size = ReadLittleEndian(entry.subspan(8, 2));
                auto offset = kHeaderSize + i * kEntrySize + 4;
                if (size > 4) {
                    offset = ReadLittleEndian(entry.subspan(4, 4));
                    if (offset < data_offset || offset > data.size() || size > data.size() - offset) {
                        continue;
                    }
                }
                if (size != 0) {
                    const auto block = data.subspan(offset, size);
                    blocks_.try_emplace(id, Block{flags, {block.begin(), block.end()}});
                }
            }
        }
    }

    // Fill absent blocks for a fresh profile. The NAND save remains read-only.
    constexpr std::array<float, 8> stereo{62, 289, 76.80f, 46.08f, 10, 5, 55.58f, 21.57f};
    std::vector<u8> stereo_bytes;
    stereo_bytes.reserve(32);
    for (const auto value : stereo) {
        const u32 word = std::bit_cast<u32>(value);
        for (unsigned i = 0; i < 4; ++i) {
            stereo_bytes.push_back(static_cast<u8>(word >> (8 * i)));
        }
    }
    blocks_.try_emplace(0x00050005, Block{kPublicFlags, std::move(stereo_bytes)});
    blocks_.try_emplace(0x00070001, Block{kPublicFlags, {1}});
    blocks_.try_emplace(kConsoleIdBlock, Block{kPublicFlags, std::vector<u8>(8)});
    std::vector<u8> user_name(0x1C);
    constexpr std::string_view default_name = "Player";
    for (std::size_t i = 0; i < default_name.size(); ++i) {
        user_name[2 * i] = static_cast<u8>(default_name[i]);
    }
    blocks_.try_emplace(0x000A0000, Block{kPublicFlags, std::move(user_name)});
    blocks_.try_emplace(0x000A0001, Block{kPublicFlags, {1, 1}});
    blocks_.try_emplace(0x000A0002, Block{kPublicFlags, {1}});
    blocks_.try_emplace(0x000B0000, Block{kPublicFlags, {0, 0, 2, 49}});
    blocks_.try_emplace(0x000C0000, Block{kPublicFlags, std::vector<u8>(0xC0)});
    blocks_.try_emplace(0x000D0000, Block{kPublicFlags, {0x7F, 0x7F, 0, 0}});
    blocks_.try_emplace(kModelBlock, Block{kSystemRead | 0x4, {4, 0, 0, 0}});
}

std::span<const u8> ConfigSave::ReadBlock(u32 id, u16 access) const {
    const auto entry = blocks_.find(id);
    if (entry == blocks_.end() || (entry->second.flags & access) != access) {
        return {};
    }
    return entry->second.bytes;
}

u64 TransferableId(std::span<const u8, 8> console_id, u32 salt) {
    return HashTransferableId(ReadLittleEndian(console_id), salt & 0xFFFFF);
}

}  // namespace mh4n::wp11
