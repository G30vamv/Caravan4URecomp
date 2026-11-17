// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. GPL-3.0-or-later.
//
// The addresses of this PC that a friend can reach a room on, found with the adapters of Windows: the virtual networks
// first (Radmin VPN, Hamachi, ZeroTier, Tailscale: no router to open), then the local network. Used by the menu's
// "Host a room" so that the player gets an address to send, with no ipconfig and no guessing.
#pragma once

#include "addr_parse.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace netaddr {

struct Address {
    std::string ip;
    std::string where;  // "Radmin VPN", "Hamachi", "ZeroTier", "Tailscale", "local network"
    bool vpn = false;
};

inline std::string Narrow(const wchar_t* w) {
    if (w == nullptr) return {};
    std::string out;
    for (; *w != 0; ++w) out.push_back(*w < 128 ? static_cast<char>(*w) : '?');
    return out;
}

inline bool Contains(std::string haystack, const char* needle) {
    std::transform(haystack.begin(), haystack.end(), haystack.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return haystack.find(needle) != std::string::npos;
}

// The IPv4 addresses worth sharing, the virtual networks first. Empty when there is no network.
inline std::vector<Address> Find() {
    std::vector<Address> out;
    ULONG size = 0;
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    GetAdaptersAddresses(AF_INET, flags, nullptr, nullptr, &size);
    if (size == 0) return out;
    std::vector<unsigned char> buffer(size);
    auto* first = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, first, &size) != NO_ERROR) return out;
    for (const IP_ADAPTER_ADDRESSES* a = first; a != nullptr; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        const std::string name = Narrow(a->FriendlyName) + " " + Narrow(a->Description);
        for (const IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u != nullptr; u = u->Next) {
            if (u->Address.lpSockaddr == nullptr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            const auto* sin = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
            const std::uint32_t host = ntohl(sin->sin_addr.S_un.S_addr);
            const unsigned b1 = host >> 24, b2 = (host >> 16) & 255;
            if (b1 == 127 || (b1 == 169 && b2 == 254) || b1 == 0) continue;  // loopback, "no address yet"
            Address address;
            char text[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &sin->sin_addr, text, sizeof text);
            address.ip = text;
            if (Contains(name, "radmin")) address.where = "Radmin VPN", address.vpn = true;
            else if (Contains(name, "hamachi")) address.where = "Hamachi", address.vpn = true;
            else if (Contains(name, "zerotier")) address.where = "ZeroTier", address.vpn = true;
            else if (Contains(name, "tailscale") || (b1 == 100 && b2 >= 64 && b2 <= 127)) address.where = "Tailscale", address.vpn = true;
            else if (b1 == 10 || (b1 == 192 && b2 == 168) || (b1 == 172 && b2 >= 16 && b2 <= 31)) address.where = "local network";
            else continue;  // a public address on an adapter: the player's own router's job, not guessed here
            out.push_back(std::move(address));
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Address& x, const Address& y) { return x.vpn && !y.vpn; });
    return out;
}

// "26.1.2.3:24872 (Radmin VPN)"
inline std::string Describe(const Address& a, unsigned port) {
    return a.ip + ":" + std::to_string(port) + " (" + a.where + ")";
}

}  // namespace netaddr
