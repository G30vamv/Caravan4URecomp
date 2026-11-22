// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: tool.h and layeredfs_test.cpp.

#include "tool.h"
#include "layeredfs.h"

#include "../common/utf8.h"

#include <charconv>
#include <fstream>
#include <optional>
#include <string_view>

namespace mh4n::layeredfs {
namespace {

std::optional<u64> ReadNumber(std::string_view text) {
    int base = 10;
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2);
        base = 16;
    }
    u64 value = 0;
    const auto end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value, base);
    if (result.ec != std::errc{} || result.ptr != end) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

int RunTool(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    if (args.size() != 5) {
        err << "usage: mh4n_layeredfs BASE OFFSET SIZE MODS_DIR OUT\n";
        return 1;
    }
    const auto offset = ReadNumber(args[1]);
    const auto size = ReadNumber(args[2]);
    if (!offset || !size) {
        err << "OFFSET and SIZE must be unsigned decimal or hexadecimal numbers\n";
        return 1;
    }
    std::string error;
    auto base = fs::PositionedFile::Open(args[0], &error);
    if (!base) {
        err << error << '\n';
        return 2;
    }
    if (*offset > base->Size()) {
        err << "OFFSET is past the end of BASE\n";
        return 2;
    }

    const auto mods = utf8::PathFromUtf8(args[3]);
    const u64 length = *size == 0 ? base->Size() - *offset : *size;
    auto image = Build(std::move(base), *offset, length, utf8::PathToUtf8(mods / "romfs"),
                       utf8::PathToUtf8(mods / "romfs_ext"), &error);
    if (!image) {
        err << error << '\n';
        return 2;
    }

    std::ofstream output(utf8::PathFromUtf8(args[4]), std::ios::binary);
    if (!output) {
        err << "cannot open " << args[4] << '\n';
        return 2;
    }
    std::vector<u8> buffer(64 * 1024);
    for (u64 pos = 0; pos < image->GetSize();) {
        std::size_t read = 0;
        if (!IsSuccess(image->Read(pos, buffer.size(), buffer.data(), &read)) || read == 0 ||
            !output.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(read))) {
            err << "cannot write " << args[4] << '\n';
            return 2;
        }
        pos += read;
    }
    output.close();
    if (!output) {
        err << "cannot finish writing " << args[4] << '\n';
        return 2;
    }
    out << "Wrote " << image->GetSize() << " bytes\n";
    return 0;
}

}  // namespace mh4n::layeredfs
