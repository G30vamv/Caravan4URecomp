// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
//
// Gamepads that SDL does not give the game (a DualSense or DualShock 4 over Bluetooth that SDL's HIDAPI does not open, or a
// generic HID pad with no SDL mapping): read here straight from the HID device, one thread each. Adapted from the HID reader of
// the author's other port (mh3u hidpad.rs), without the rumble (the 3DS has none).
//
// Which devices: every Sony pad, and any other HID joystick/gamepad that is not an XInput device (its path has no `ig_`). The
// shell decides, per pad, whether to use it: it is left alone while SDL already has a gamepad of the same vendor/product or an
// XInput pad is connected (a real Xbox pad, or the virtual one DSX / DS4Windows / Steam Input make), so nothing is heard twice.
//
// The reports are read with Windows's own HID parser (HidP_*) from the descriptor, so the transport and report mode do not
// matter; Sony's reports that the descriptor does not describe (DualSense 0x31, DualShock 4 0x11 over Bluetooth) are read by
// offset. ini `hid_pads = off` (or env MH4U_HID_PADS=0) turns it off.
#pragma once

#include <windows.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <setupapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace hidpads {

// SDL's gamepad button numbers (what the mapper's events carry) as bits, and its axis numbers.
enum Button : int {
    kSouth = 0, kEast, kWest, kNorth, kBack, kGuide, kStart, kLeftStick, kRightStick, kLeftShoulder, kRightShoulder,
    kDpadUp, kDpadDown, kDpadLeft, kDpadRight
};
enum Axis : int { kLeftX = 0, kLeftY, kRightX, kRightY, kTriggerLeft, kTriggerRight, kAxisCount };

struct Snapshot {
    int id = 0;  // stable for as long as the pad stays connected
    std::uint16_t vendor = 0, product = 0;
    std::string name;
    std::uint32_t buttons = 0;  // bit n = SDL button n held
    std::int16_t axes[kAxisCount] = {};
};

namespace detail {

constexpr std::uint16_t kSony = 0x054c;
constexpr std::uint16_t kDualShock4[3] = {0x05c4, 0x09cc, 0x0ba0};
constexpr std::uint16_t kDualSense[2] = {0x0ce6, 0x0df2};
constexpr float kDeadzone = 0.10f;
constexpr float kTriggerThreshold = 0.12f;
constexpr std::uint16_t kPageGeneric = 0x01, kPageButtons = 0x09, kUsageHat = 0x39;

inline bool In(std::uint16_t v, const std::uint16_t* a, std::size_t n) {
    return std::find(a, a + n, v) != a + n;
}

struct State {
    std::uint32_t buttons = 0;
    std::int16_t axes[kAxisCount] = {};
    bool operator==(const State& o) const {
        return buttons == o.buttons && std::equal(axes, axes + kAxisCount, o.axes);
    }
};

inline std::int16_t ToAxis(float v) {  // -1..1 -> SDL's range, with a small dead zone
    if (std::fabs(v) < kDeadzone) return 0;
    return static_cast<std::int16_t>(std::clamp(v, -1.0f, 1.0f) * 32767.0f);
}
inline std::int16_t ToTrigger(float v) {  // 0..1
    return static_cast<std::int16_t>(std::clamp(v, 0.0f, 1.0f) * 32767.0f);
}
inline float ByteAxis(std::uint8_t raw) { return (static_cast<float>(raw) - 127.5f) / 127.5f; }

inline std::uint32_t Bit(int b) { return 1u << b; }

/// The d-pad buttons of a hat value (0 up, then clockwise by eighths; anything else is centred).
inline std::uint32_t HatButtons(std::uint32_t dir) {
    switch (dir) {
    case 0: return Bit(kDpadUp);
    case 1: return Bit(kDpadUp) | Bit(kDpadRight);
    case 2: return Bit(kDpadRight);
    case 3: return Bit(kDpadDown) | Bit(kDpadRight);
    case 4: return Bit(kDpadDown);
    case 5: return Bit(kDpadDown) | Bit(kDpadLeft);
    case 6: return Bit(kDpadLeft);
    case 7: return Bit(kDpadUp) | Bit(kDpadLeft);
    default: return 0;
    }
}

/// Sony's button bytes (the same in a DualShock 4 and a DualSense): b0 hat and face buttons, b1 shoulders and sticks, b2 PS.
inline State SonyState(const std::uint8_t sticks[4], const std::uint8_t b[3], std::uint8_t l2, std::uint8_t r2) {
    State s;
    s.axes[kLeftX] = ToAxis(ByteAxis(sticks[0]));
    s.axes[kLeftY] = ToAxis(ByteAxis(sticks[1]));
    s.axes[kRightX] = ToAxis(ByteAxis(sticks[2]));
    s.axes[kRightY] = ToAxis(ByteAxis(sticks[3]));
    const bool l2_digital = (b[1] & 0x04) != 0, r2_digital = (b[1] & 0x08) != 0;
    s.axes[kTriggerLeft] = l2_digital && l2 == 0 ? 32767 : ToTrigger(static_cast<float>(l2) / 255.0f);
    s.axes[kTriggerRight] = r2_digital && r2 == 0 ? 32767 : ToTrigger(static_cast<float>(r2) / 255.0f);
    auto add = [&](bool on, int bit) { if (on) s.buttons |= Bit(bit); };
    add(b[0] & 0x10, kWest);   // square
    add(b[0] & 0x20, kSouth);  // cross
    add(b[0] & 0x40, kEast);   // circle
    add(b[0] & 0x80, kNorth);  // triangle
    add(b[1] & 0x01, kLeftShoulder);
    add(b[1] & 0x02, kRightShoulder);
    add(b[1] & 0x10, kBack);   // share / create
    add(b[1] & 0x20, kStart);  // options
    add(b[1] & 0x40, kLeftStick);
    add(b[1] & 0x80, kRightStick);
    add(b[2] & 0x01, kGuide);  // PS
    s.buttons |= HatButtons(b[0] & 0x0f);
    return s;
}

/// A Sony report read by offset, for the reports Windows's parser cannot read (they are not in the descriptor) and as a fallback.
inline bool ParseSonyReport(std::uint16_t product, bool bluetooth, const std::uint8_t* r, std::size_t n, State& out) {
    if (n == 0) return false;
    const bool ds = In(product, kDualSense, 2), ds4 = In(product, kDualShock4, 3);
    if (r[0] == 0x31 && ds && n >= 12) {  // DualSense, Bluetooth full report: id, tag, x y rx ry l2 r2 counter b0 b1 b2
        const std::uint8_t st[4] = {r[2], r[3], r[4], r[5]}, b[3] = {r[9], r[10], r[11]};
        out = SonyState(st, b, r[6], r[7]);
        return true;
    }
    if (r[0] == 0x01 && ds && !bluetooth && n >= 11) {  // DualSense, USB: id, x y rx ry l2 r2 counter b0 b1 b2
        const std::uint8_t st[4] = {r[1], r[2], r[3], r[4]}, b[3] = {r[8], r[9], r[10]};
        out = SonyState(st, b, r[5], r[6]);
        return true;
    }
    if (r[0] == 0x01 && (ds4 || ds) && n >= 10) {  // DualShock 4 (and a DualSense's short Bluetooth report): id, x y rx ry b0 b1 b2 l2 r2
        const std::uint8_t st[4] = {r[1], r[2], r[3], r[4]}, b[3] = {r[5], r[6], r[7]};
        out = SonyState(st, b, r[8], r[9]);
        return true;
    }
    if (r[0] == 0x11 && ds4 && n >= 12) {  // DualShock 4, Bluetooth full report: id, 2 bytes, then x y rx ry b0 b1 b2 l2 r2
        const std::uint8_t st[4] = {r[3], r[4], r[5], r[6]}, b[3] = {r[7], r[8], r[9]};
        out = SonyState(st, b, r[10], r[11]);
        return true;
    }
    return false;
}

struct AxisCap {
    std::uint8_t report;
    std::uint16_t usage;
    long min, max;
    std::uint16_t bits;
};

/// One connected pad: its handle, what its descriptor says and the state its thread last read.
struct Pad {
    int id = 0;
    HANDLE handle = INVALID_HANDLE_VALUE;
    PHIDP_PREPARSED_DATA preparsed = nullptr;
    std::string path, name;
    std::uint16_t vendor = 0, product = 0;
    bool bluetooth = false;
    std::size_t report_len = 0;
    std::vector<AxisCap> axes;
    std::mutex mutex;
    State state;
    std::atomic<bool> alive{true};

    /// The value of an axis in a report, 0 to 1 (a missing axis is -1).
    float Axis(std::uint8_t* report, std::size_t n, std::uint16_t usage) const {
        const AxisCap* a = nullptr;
        for (const AxisCap& c : axes) {
            if (c.usage == usage && c.report == report[0]) { a = &c; break; }
        }
        if (!a || a->max <= a->min) return -1.0f;
        ULONG raw = 0;
        if (HidP_GetUsageValue(HidP_Input, kPageGeneric, 0, usage, &raw, preparsed, reinterpret_cast<PCHAR>(report),
                               static_cast<ULONG>(n)) != HIDP_STATUS_SUCCESS)
            return -1.0f;
        long value = static_cast<long>(raw);
        if (a->min < 0 && a->bits < 32) {
            const unsigned shift = 32u - a->bits;
            value = static_cast<std::int32_t>(raw << shift) >> shift;
        }
        return std::clamp(static_cast<float>(value - a->min) / static_cast<float>(a->max - a->min), 0.0f, 1.0f);
    }

    /// Reads a report with Windows's HID parser; false when the descriptor does not describe it.
    bool Parse(std::uint8_t* report, std::size_t n, State& out) const {
        const bool sony = vendor == kSony;
        const float lx = Axis(report, n, 0x30), ly = Axis(report, n, 0x31);
        if (lx < 0 || ly < 0) return false;
        const float z = Axis(report, n, 0x32), rz = Axis(report, n, 0x35), rx = Axis(report, n, 0x33), ry = Axis(report, n, 0x34);
        // Sony: right stick Z/Rz, triggers Rx/Ry. Others: right stick Z/Rz, or Rx/Ry when there is no Z/Rz
        float right_x = 0.5f, right_y = 0.5f;
        if (sony || z >= 0) { if (z >= 0) right_x = z; if (rz >= 0) right_y = rz; }
        else { if (rx >= 0) right_x = rx; if (ry >= 0) right_y = ry; }
        State s;
        s.axes[kLeftX] = ToAxis(lx * 2 - 1);
        s.axes[kLeftY] = ToAxis(ly * 2 - 1);
        s.axes[kRightX] = ToAxis(right_x * 2 - 1);
        s.axes[kRightY] = ToAxis(right_y * 2 - 1);
        if (sony) {
            if (rx >= 0) s.axes[kTriggerLeft] = ToTrigger(rx);
            if (ry >= 0) s.axes[kTriggerRight] = ToTrigger(ry);
        }
        for (const AxisCap& c : axes) {  // the hat
            if (c.usage != kUsageHat || c.report != report[0]) continue;
            ULONG raw = 0;
            if (HidP_GetUsageValue(HidP_Input, kPageGeneric, 0, kUsageHat, &raw, preparsed, reinterpret_cast<PCHAR>(report),
                                   static_cast<ULONG>(n)) == HIDP_STATUS_SUCCESS) {
                const long dir = static_cast<long>(raw) - c.min;
                if (dir >= 0) s.buttons |= HatButtons(static_cast<std::uint32_t>(dir));
            }
            break;
        }
        USAGE usages[32];
        ULONG count = 32;
        if (HidP_GetUsages(HidP_Input, kPageButtons, 0, usages, &count, preparsed, reinterpret_cast<PCHAR>(report),
                           static_cast<ULONG>(n)) == HIDP_STATUS_SUCCESS) {
            for (ULONG i = 0; i < count; ++i) {
                const int u = usages[i];
                int bit = -1;
                if (sony) {  // Sony's usages in the order of their descriptors
                    static const int kSonyMap[14] = {-1, kWest, kSouth, kEast, kNorth, kLeftShoulder, kRightShoulder, -2, -3,
                                                     kBack, kStart, kLeftStick, kRightStick, kGuide};
                    if (u >= 1 && u <= 13) bit = kSonyMap[u];
                    if (bit == -2) s.axes[kTriggerLeft] = std::max<std::int16_t>(s.axes[kTriggerLeft], 32767);
                    if (bit == -3) s.axes[kTriggerRight] = std::max<std::int16_t>(s.axes[kTriggerRight], 32767);
                } else {  // the usual order of a generic pad's buttons (A B X Y, bumpers, back, start, sticks)
                    static const int kGenericMap[11] = {-1, kSouth, kEast, kWest, kNorth, kLeftShoulder, kRightShoulder, kBack,
                                                        kStart, kLeftStick, kRightStick};
                    if (u >= 1 && u <= 10) bit = kGenericMap[u];
                }
                if (bit >= 0) s.buttons |= Bit(bit);
            }
        }
        out = s;
        return true;
    }
};

inline std::mutex g_mutex;
inline std::vector<std::shared_ptr<Pad>> g_pads;
inline std::atomic<int> g_next_id{0x40000000};  // far from SDL's joystick ids
inline std::atomic<bool> g_started{false};

inline std::vector<std::string> DevicePaths() {
    std::vector<std::string> paths;
    GUID guid;
    HidD_GetHidGuid(&guid);
    HDEVINFO set = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return paths;
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interface_data{};
        interface_data.cbSize = sizeof(interface_data);
        if (!SetupDiEnumDeviceInterfaces(set, nullptr, &guid, index, &interface_data)) break;
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &interface_data, nullptr, 0, &needed, nullptr);
        if (needed == 0) continue;
        std::vector<std::uint64_t> buffer((needed + 7) / 8);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &interface_data, detail, needed, nullptr, nullptr)) continue;
        const int len = WideCharToMultiByte(CP_UTF8, 0, detail->DevicePath, -1, nullptr, 0, nullptr, nullptr);
        if (len <= 1) continue;
        std::string s(static_cast<std::size_t>(len - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, detail->DevicePath, -1, s.data(), len, nullptr, nullptr);
        paths.push_back(std::move(s));
    }
    SetupDiDestroyDeviceInfoList(set);
    return paths;
}

inline std::wstring Wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<std::size_t>(std::max(n, 1)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    w.resize(static_cast<std::size_t>(std::max(n - 1, 0)));
    return w;
}

enum class Open { Pad, Skip, Retry };

inline Open OpenDevice(const std::string& path, std::shared_ptr<Pad>& out, std::set<std::string>& warned) {
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // XInput devices (Xbox pads and everything that pretends to be one) are SDL's / XInput's
    if (lower.find("&ig_") != std::string::npos || lower.find("\\ig_") != std::string::npos) return Open::Skip;
    const bool sony_path = lower.find("054c") != std::string::npos;
    const std::wstring wide = Wide(path);
    HANDLE handle = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        if (sony_path && warned.insert(path).second) {
            std::fprintf(stderr,
                         "[hid] a Sony pad could not be opened (Windows error %lu). HidHide, DS4Windows or Steam is probably hiding it "
                         "from this program: add Caravan4URecomp.exe to HidHide's application list, or run DSX/DS4Windows, which make "
                         "an Xbox pad that works as it is: %s\n",
                         GetLastError(), path.c_str());
        }
        return sony_path ? Open::Retry : Open::Skip;
    }
    HIDD_ATTRIBUTES attributes{};
    attributes.Size = sizeof(attributes);
    PHIDP_PREPARSED_DATA preparsed = nullptr;
    if (!HidD_GetAttributes(handle, &attributes) || !HidD_GetPreparsedData(handle, &preparsed)) {
        CloseHandle(handle);
        return Open::Skip;
    }
    HIDP_CAPS caps{};
    const bool joystick = HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS && caps.UsagePage == kPageGeneric &&
                          (caps.Usage == 0x04 || caps.Usage == 0x05);
    if (!joystick || caps.InputReportByteLength < 2) {
        HidD_FreePreparsedData(preparsed);
        CloseHandle(handle);
        return Open::Skip;
    }
    auto pad = std::make_shared<Pad>();
    pad->handle = handle;
    pad->preparsed = preparsed;
    pad->path = path;
    pad->vendor = attributes.VendorID;
    pad->product = attributes.ProductID;
    pad->report_len = caps.InputReportByteLength;
    pad->bluetooth = lower.find("00001124-0000-1000-8000-00805f9b34fb") != std::string::npos;
    USHORT count = caps.NumberInputValueCaps;
    std::vector<HIDP_VALUE_CAPS> value_caps(count);
    if (count > 0 && HidP_GetValueCaps(HidP_Input, value_caps.data(), &count, preparsed) == HIDP_STATUS_SUCCESS) {
        for (USHORT i = 0; i < count; ++i) {
            const HIDP_VALUE_CAPS& c = value_caps[i];
            if (c.UsagePage != kPageGeneric) continue;
            const USHORT first = c.IsRange ? c.Range.UsageMin : c.NotRange.Usage;
            const USHORT last = c.IsRange ? c.Range.UsageMax : c.NotRange.Usage;
            // 32-bit counter and a cap: a range ending at 0xFFFF made a 16-bit counter loop forever (and fill the memory)
            for (unsigned u = first; u <= last && u < first + 64u; ++u) {
                pad->axes.push_back({static_cast<std::uint8_t>(c.ReportID), static_cast<std::uint16_t>(u), c.LogicalMin, c.LogicalMax, c.BitSize});
            }
        }
    }
    wchar_t name[128] = {};
    if (HidD_GetProductString(handle, name, sizeof(name))) {
        const int len = WideCharToMultiByte(CP_UTF8, 0, name, -1, nullptr, 0, nullptr, nullptr);
        if (len > 1) {
            pad->name.assign(static_cast<std::size_t>(len - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, name, -1, pad->name.data(), len, nullptr, nullptr);
        }
    }
    if (pad->name.empty()) pad->name = "HID gamepad";
    pad->id = g_next_id.fetch_add(1);
    std::fprintf(stderr, "[hid] gamepad read from the device: %s (%04x:%04x, %s)\n", pad->name.c_str(), pad->vendor, pad->product,
                 pad->bluetooth ? "Bluetooth" : "USB");
    out = pad;
    return Open::Pad;
}

inline void ReadLoopBody(const std::shared_ptr<Pad>& pad) {
    std::vector<std::uint8_t> buffer(pad->report_len);
    bool first = true, unread = false;
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(pad->handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) || read == 0) break;
        const std::size_t n = std::min<std::size_t>(read, buffer.size());
        State state;
        bool ok = pad->Parse(buffer.data(), buffer.size(), state);
        if (!ok && pad->vendor == kSony) ok = ParseSonyReport(pad->product, pad->bluetooth, buffer.data(), n, state);
        if (!ok) {
            if (!unread) {
                unread = true;
                std::fprintf(stderr, "[hid] report not understood from %04x:%04x: id 0x%02x, %zu bytes\n", pad->vendor, pad->product,
                             buffer[0], n);
            }
            continue;
        }
        if (first) {
            std::fprintf(stderr, "[hid] first report from %04x:%04x: id 0x%02x, %zu bytes\n", pad->vendor, pad->product, buffer[0], n);
            first = false;
        }
        std::lock_guard<std::mutex> lock(pad->mutex);
        pad->state = state;
    }
    std::fprintf(stderr, "[hid] gamepad %04x:%04x disconnected\n", pad->vendor, pad->product);
    pad->alive.store(false);
}

/// The reader thread: whatever goes wrong with one pad (an exception) ends that pad only, never the program.
inline void ReadLoop(std::shared_ptr<Pad> pad) {
    try {
        ReadLoopBody(pad);
    } catch (...) {
        std::fprintf(stderr, "[hid] the reader of %04x:%04x failed; the pad is dropped\n", pad->vendor, pad->product);
    }
    pad->alive.store(false);
}

}  // namespace detail

/// Whether an XInput pad is connected (a real Xbox pad, or the virtual one DSX, DS4Windows or Steam Input make).
inline bool XInputConnected() {
    using Fn = DWORD(WINAPI*)(DWORD, void*);
    static const Fn fn = [] {
        HMODULE m = LoadLibraryW(L"xinput1_4.dll");
        if (!m) m = LoadLibraryW(L"xinput9_1_0.dll");
        return m ? reinterpret_cast<Fn>(GetProcAddress(m, "XInputGetState")) : nullptr;
    }();
    if (!fn) return false;
    for (DWORD slot = 0; slot < 4; ++slot) {
        alignas(8) unsigned char state[16] = {};  // XINPUT_STATE is 16 bytes
        if (fn(slot, state) == 0) return true;
    }
    return false;
}

/// Starts the thread that finds the pads (a scan of the HID devices every 2 s) and reads each one on a thread of its own.
inline void Start() {
    if (detail::g_started.exchange(true)) return;
    std::thread([] {
      try {
        std::set<std::string> rejected, warned, open;
        for (;;) {
            const std::vector<std::string> paths = detail::DevicePaths();
            const std::set<std::string> present(paths.begin(), paths.end());
            for (auto it = rejected.begin(); it != rejected.end();) it = present.count(*it) ? std::next(it) : rejected.erase(it);
            for (auto it = warned.begin(); it != warned.end();) it = present.count(*it) ? std::next(it) : warned.erase(it);
            {   // the pads whose thread ended (unplugged): their handles are freed and they leave the list
                std::lock_guard<std::mutex> lock(detail::g_mutex);
                for (auto it = detail::g_pads.begin(); it != detail::g_pads.end();) {
                    if ((*it)->alive.load()) { ++it; continue; }
                    open.erase((*it)->path);
                    CloseHandle((*it)->handle);
                    HidD_FreePreparsedData((*it)->preparsed);
                    it = detail::g_pads.erase(it);
                }
            }
            for (const std::string& path : paths) {
                if (rejected.count(path) || open.count(path)) continue;
                std::shared_ptr<detail::Pad> pad;
                switch (detail::OpenDevice(path, pad, warned)) {
                case detail::Open::Pad: {
                    open.insert(path);
                    {
                        std::lock_guard<std::mutex> lock(detail::g_mutex);
                        detail::g_pads.push_back(pad);
                    }
                    std::thread(detail::ReadLoop, pad).detach();
                    break;
                }
                case detail::Open::Skip: rejected.insert(path); break;
                case detail::Open::Retry: break;
                }
            }
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
      } catch (...) {
        std::fprintf(stderr, "[hid] the pad scanner failed and stopped; the HID pads are off for this run\n");
      }
    }).detach();
}

// A start guard. A crash cannot be caught, and the HID code opens every gamepad-like device of the PC, so a marker file lives from
// the moment the HID reading starts until it has run for a while (or the program ends normally). If the marker is still there at the
// next start, that run ended inside it: the HID pads stay off in this one and the game opens as usual (SDL still serves the pads it
// can). Such an end is not always the HID code's fault (a forced close, a power cut), so the start after that one tries again; only
// two in a row leave the HID pads off for good. A run that lasts, or ends normally, removes the marker. MH4U_HID_FORCE=1 tries again.
// The marker is one line, "<state> <ends in a row>", state = armed | retry | off (older versions wrote "hid": read as armed 0).
inline std::filesystem::path GuardPath() {
    std::error_code ec;
    return std::filesystem::temp_directory_path(ec) / "Caravan4URecomp_hid_start.marker";
}
namespace detail {
inline int g_guard_ends = 0;  // the ends inside the HID reading that came right before this run
inline void WriteGuard(const char* state, int ends) { std::ofstream(GuardPath(), std::ios::trunc) << state << ' ' << ends << '\n'; }
}  // namespace detail
/// Asked once at a start, before ArmGuard. True: the HID pads stay off in this run.
inline bool GuardTripped() {
    detail::g_guard_ends = 0;
    const char* force = std::getenv("MH4U_HID_FORCE");
    if (force && std::string(force) == "1") return false;
    std::string state;
    int ends = 0;
    {
        std::ifstream marker(GuardPath());
        if (!marker) return false;
        marker >> state;
        if (!(marker >> ends)) ends = 0;
    }
    if (state == "off") return true;
    if (state == "retry") {
        detail::g_guard_ends = ends;
        return false;
    }
    ++ends;
    detail::WriteGuard(ends >= 2 ? "off" : "retry", ends);
    return true;
}
inline void ArmGuard() { detail::WriteGuard("armed", detail::g_guard_ends); }
inline void DisarmGuard() {
    std::error_code ec;
    std::filesystem::remove(GuardPath(), ec);
}

/// The pads connected now with what each one holds.
inline std::vector<Snapshot> Get() {
    std::vector<Snapshot> out;
    std::lock_guard<std::mutex> lock(detail::g_mutex);
    for (const auto& pad : detail::g_pads) {
        if (!pad->alive.load()) continue;
        Snapshot s;
        s.id = pad->id;
        s.vendor = pad->vendor;
        s.product = pad->product;
        s.name = pad->name;
        {
            std::lock_guard<std::mutex> state_lock(pad->mutex);
            s.buttons = pad->state.buttons;
            std::copy(pad->state.axes, pad->state.axes + kAxisCount, s.axes);
        }
        out.push_back(std::move(s));
    }
    return out;
}

}  // namespace hidpads
