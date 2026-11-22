// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: pack.h, json.h and hdpack_test.cpp pack fixtures.

#include "pack.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>

#include "json.h"

namespace mh4n::hdpack {
namespace {

namespace fs = std::filesystem;

std::string_view Basename(std::string_view name) {
    const auto separator = name.find_last_of("/\\");
    return separator == std::string_view::npos ? name : name.substr(separator + 1);
}

FileFormat Extension(std::string_view extension) {
    if (extension == "png") {
        return FileFormat::Png;
    }
    if (extension == "dds") {
        return FileFormat::Dds;
    }
    if (extension == "ktx") {
        return FileFormat::Ktx;
    }
    return FileFormat::None;
}

void AddFile(Pack& pack, const fs::path& root, const fs::directory_entry& entry,
             const std::map<std::string, std::vector<u64>>& mapped_hashes) {
    const auto name = entry.path().filename().string();
    const auto first_dot = name.find('.');
    const auto last_dot = name.rfind('.');
    const auto format = first_dot == std::string::npos
                            ? FileFormat::None
                            : Extension(std::string_view(name).substr(last_dot + 1));
    if (format == FileFormat::None || std::count(name.begin(), name.end(), '.') > 2) {
        ++pack.stats.ignored_names;
        return;
    }
    if (format == FileFormat::Dds && pack.options.skip_mipmap) {
        ++pack.stats.ignored_dds;
        return;
    }

    PackFile file;
    file.path = entry.path().lexically_relative(root).generic_string();
    file.format = format;
    if (first_dot != last_dot && std::string_view(name).substr(first_dot + 1, last_dot - first_dot - 1) == "norm") {
        file.type = MapType::Normal;
    }
    const auto mapped = mapped_hashes.find(name);
    if (mapped != mapped_hashes.end()) {
        file.hashes = mapped->second;
    }
    if (auto stem = ParseTextureStem(std::string_view(name).substr(0, first_dot))) {
        if (std::ranges::find(file.hashes, stem->hash) == file.hashes.end()) {
            file.hashes.push_back(stem->hash);
        }
    }
    if (file.hashes.empty()) {
        ++pack.stats.files_without_hash;
        return;
    }

    std::error_code ec;
    const auto size = entry.file_size(ec);
    if (!ec) {
        file.size = size;
    }
    const auto time = entry.last_write_time(ec);
    if (!ec) {
        file.mtime = std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
    }
    pack.files.push_back(std::move(file));
}

}  // namespace

std::optional<TextureStem> ParseTextureStem(std::string_view name) {
    const std::string stem(name);
    TextureStem result;
    if (std::sscanf(stem.c_str(), "tex1_%" SCNu32 "x%" SCNu32 "_%" SCNx64 "_%" SCNu32,
                    &result.width, &result.height, &result.hash, &result.format) == 4) {
        return result;
    }
    return std::nullopt;
}

std::optional<PackJson> ParsePackJson(const std::string& text, std::string* error) {
    const auto document = ParseJson(text, error);
    if (!document) {
        return std::nullopt;
    }
    const auto* options = document->Find("options");
    if (!options || !options->IsObject()) {
        if (error) {
            *error = "pack.json requires an options object";
        }
        return std::nullopt;
    }

    PackJson pack;
    pack.options.has_pack_json = true;
    for (const auto& [name, field] : {
             std::pair{"flip_png_files", &PackOptions::flip_png_files},
             std::pair{"skip_mipmap", &PackOptions::skip_mipmap},
             std::pair{"use_new_hash", &PackOptions::use_new_hash},
         }) {
        const auto* option = options->Find(name);
        if (!option || !option->IsBool()) {
            if (error) {
                *error = std::string("pack.json requires a boolean option ") + name;
            }
            return std::nullopt;
        }
        pack.options.*field = option->boolean;
    }

    const auto* textures = document->Find("textures");
    if (textures && textures->IsObject()) {
        for (const auto& [key, value] : textures->object) {
            u64 hash = 0;
            std::string_view digits(key);
            if (digits.starts_with("0x") || digits.starts_with("0X")) {
                digits.remove_prefix(2);
            }
            const auto end = digits.data() + digits.size();
            const auto [ptr, ec] = std::from_chars(digits.data(), end, hash, 16);
            if (ec != std::errc{} || ptr != end) {
                continue;
            }
            const auto add_name = [&](const Json& path) {
                if (path.IsString()) {
                    auto& hashes = pack.hashes_by_file[std::string(Basename(path.string))];
                    if (std::ranges::find(hashes, hash) == hashes.end()) {
                        hashes.push_back(hash);
                    }
                }
            };
            if (value.IsArray()) {
                for (const auto& path : value.array) {
                    add_name(path);
                }
            } else {
                add_name(value);
            }
        }
    }
    return pack;
}

std::optional<Pack> ScanPack(const std::string& dir, std::string* error) {
    const fs::path root(dir);
    const auto fail = [&](std::string message) -> std::optional<Pack> {
        if (error) {
            *error = std::move(message);
        }
        return std::nullopt;
    };

    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        return fail("not a pack directory: " + dir);
    }

    Pack pack;
    PackJson settings;
    const auto config = root / "pack.json";
    const bool has_config = fs::exists(config, ec);
    if (ec) {
        return fail("cannot inspect pack.json: " + ec.message());
    }
    if (has_config) {
        std::ifstream input(config, std::ios::binary);
        if (!input) {
            return fail("cannot open pack.json");
        }
        const std::string contents{std::istreambuf_iterator<char>(input), {}};
        std::string parse_error;
        auto parsed = ParsePackJson(contents, &parse_error);
        if (!parsed) {
            return fail("pack.json: " + parse_error);
        }
        settings = std::move(*parsed);
    }
    pack.options = settings.options;

    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it.depth() >= 63) {
            it.disable_recursion_pending();
        }
        std::error_code file_ec;
        if (it->path() != config && it->is_regular_file(file_ec)) {
            AddFile(pack, root, *it, settings.hashes_by_file);
        }
    }
    if (ec) {
        return fail("cannot scan pack: " + ec.message());
    }

    std::ranges::sort(pack.files, {}, &PackFile::path);
    for (std::size_t index = 0; index < pack.files.size(); ++index) {
        const auto& file = pack.files[index];
        for (const u64 hash : file.hashes) {
            int& slot = pack.materials[hash].file[static_cast<std::size_t>(file.type)];
            if (slot < 0) {
                slot = static_cast<int>(index);
            } else {
                ++pack.stats.duplicate_maps;
            }
        }
    }
    return pack;
}

}  // namespace mh4n::hdpack
