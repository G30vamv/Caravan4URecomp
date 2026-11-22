// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by
// khorne-hub (https://github.com/khorne-hub/4u-native); see ADDITIONAL-TERMS and THIRD_PARTY_NOTICES.
//
// Discord Rich Presence ("Playing Monster Hunter 4 Ultimate" with the time played). It talks to the Discord desktop app over
// its local named pipe (\\.\pipe\discord-ipc-N) with the documented IPC framing; no Discord library and no network. It only
// sends the game's name and the start time: never the nickname, addresses or paths. If Discord is not running it tries
// again every 15 seconds in the background and never blocks or fails the game. ini: discord_presence = on|off (default on).
#pragma once

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>

namespace discord {

constexpr const char* kApplicationId = "1573193173826801775";

class Presence {
public:
    Presence() = default;
    Presence(const Presence&) = delete;
    Presence& operator=(const Presence&) = delete;
    ~Presence() { Stop(); }

    void Start(std::string details, std::string state) {
        if (thread_.joinable()) return;
        details_ = std::move(details);
        state_ = std::move(state);
        started_at_ = static_cast<std::int64_t>(std::time(nullptr));
        stop_ = false;
        thread_ = std::thread([this] { Run(); });
    }

    void Stop() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
    }

private:
    static constexpr std::uint32_t kHandshake = 0, kFrame = 1;

    static std::string Escape(const std::string& text) {
        std::string out;
        for (const char c : text) {
            if (c == '"' || c == '\\') out += '\\';
            if (static_cast<unsigned char>(c) >= 0x20) out += c;
        }
        return out;
    }

    static bool WriteAll(HANDLE pipe, const void* data, DWORD size) {
        const auto* bytes = static_cast<const char*>(data);
        while (size > 0) {
            DWORD written = 0;
            if (!WriteFile(pipe, bytes, size, &written, nullptr) || written == 0) return false;
            bytes += written;
            size -= written;
        }
        return true;
    }

    static bool Send(HANDLE pipe, std::uint32_t opcode, const std::string& json) {
        std::string packet(8, '\0');
        const std::uint32_t length = static_cast<std::uint32_t>(json.size());
        std::memcpy(packet.data(), &opcode, 4);
        std::memcpy(packet.data() + 4, &length, 4);
        packet += json;
        return WriteAll(pipe, packet.data(), static_cast<DWORD>(packet.size()));
    }

    // Reads and drops whatever Discord answered (it must not pile up); false when the pipe is broken.
    static bool Drain(HANDLE pipe) {
        for (;;) {
            DWORD available = 0;
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return false;
            if (available == 0) return true;
            char buffer[4096];
            DWORD got = 0;
            if (!ReadFile(pipe, buffer, sizeof(buffer), &got, nullptr)) return false;
        }
    }

    static HANDLE Connect() {
        for (int index = 0; index < 10; ++index) {
            const std::string name = "\\\\.\\pipe\\discord-ipc-" + std::to_string(index);
            const HANDLE pipe = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe != INVALID_HANDLE_VALUE) return pipe;
        }
        return INVALID_HANDLE_VALUE;
    }

    bool Session(HANDLE pipe) {
        if (!Send(pipe, kHandshake, std::string("{\"v\":1,\"client_id\":\"") + kApplicationId + "\"}")) return false;
        // The first answer (READY) comes back; give Discord a moment, then send the activity.
        for (int i = 0; i < 20 && !stop_; ++i) {
            DWORD available = 0;
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return false;
            if (available > 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!Drain(pipe)) return false;
        const std::string activity =
            "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(GetCurrentProcessId()) +
            ",\"activity\":{\"details\":\"" + Escape(details_) + "\",\"state\":\"" + Escape(state_) +
            "\",\"timestamps\":{\"start\":" + std::to_string(started_at_) + "}}},\"nonce\":\"1\"}";
        if (!Send(pipe, kFrame, activity)) return false;
        while (!stop_) {
            if (!Drain(pipe)) return false;  // Discord closed: connect again later
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        return true;
    }

    void Run() {
        while (!stop_) {
            const HANDLE pipe = Connect();
            if (pipe != INVALID_HANDLE_VALUE) {
                Session(pipe);
                CloseHandle(pipe);
            }
            for (int i = 0; i < 150 && !stop_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::string details_, state_;
    std::int64_t started_at_ = 0;
};

}  // namespace discord
