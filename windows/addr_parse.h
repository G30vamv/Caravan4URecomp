// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. GPL-3.0-or-later.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace netaddr {

// HOST:PORT out of a text (a pasted address, with spaces or a line around it); false when it is not one.
inline bool ParseHostPort(const std::string& text, std::string& host, unsigned& port, unsigned default_port) {
    std::string t;
    for (char c : text) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!t.empty()) break;  // the first word only
            continue;
        }
        t.push_back(c);
    }
    if (t.empty() || t.size() > 255) return false;
    host = t;
    port = default_port;
    const auto colon = t.rfind(':');
    if (colon != std::string::npos) {
        host = t.substr(0, colon);
        const std::string digits = t.substr(colon + 1);
        if (digits.empty() || digits.size() > 5 || !std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) return false;
        port = static_cast<unsigned>(std::atoi(digits.c_str()));
    }
    if (host.empty() || port == 0 || port > 65535) return false;
    for (unsigned char c : host) {
        if (!(std::isalnum(c) || c == '.' || c == '-')) return false;
    }
    return true;
}

}  // namespace netaddr
