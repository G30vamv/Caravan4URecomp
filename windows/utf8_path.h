// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by
// khorne-hub (https://github.com/khorne-hub/4u-native); see ADDITIONAL-TERMS and THIRD_PARTY_NOTICES.
//
// Paths as UTF-8 text and back. path::string() must not be used here: it converts to the ANSI code page of Windows, so a
// letter such as an accented one comes out in a coding the rest of the program does not use, and a character that code
// page does not have (Japanese, a symbol) throws, which closed the program. path(std::string) reads ANSI the same way.
#pragma once

#include <filesystem>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace u8p {

inline std::string Utf8(const std::filesystem::path& path) {
#ifdef _WIN32
    const std::wstring& wide = path.native();
    if (wide.empty()) return {};
    const int length = static_cast<int>(wide.size());
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), length, nullptr, 0, nullptr, nullptr);
    std::string text(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, wide.data(), length, text.data(), n, nullptr, nullptr);
    return text;
#else
    return path.string();
#endif
}

inline std::filesystem::path PathOf(const std::string& utf8) {
#ifdef _WIN32
    if (utf8.empty()) return {};
    const int length = static_cast<int>(utf8.size());
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), length, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), length, wide.data(), n);
    return std::filesystem::path(wide);
#else
    return std::filesystem::path(utf8);
#endif
}

}  // namespace u8p
