// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: 3dbrew DSP Memory Region, libctru NDSP, and boot-village and early MDSP.
#pragma once

#include <array>
#include <functional>
#include <span>
#include <string_view>

#include "mh4n/types.h"

namespace mh4n {

class NativeDsp {
public:
    static constexpr std::size_t kBankBytes = 0x8000;
    static constexpr std::size_t kFrameSamples = 160;
    static constexpr u8 kAudioPipe = 2;
    using Bank = std::span<u8, kBankBytes>;
    using Stereo = std::array<s16, 2>;
    using Frame = std::array<Stereo, kFrameSamples>;
    // Address is the guest physical address from the wave buffer descriptor.
    using ReadMemory = std::function<bool(u32 address, void* dst, u32 size)>;
    // The caller can route warnings to its logger. Each warning is emitted at most once.
    using Warn = std::function<void(std::string_view)>;

    explicit NativeDsp(ReadMemory read, Warn warn = {});
    void Initialize();
    void Shutdown();
    void Sleep();
    void Wakeup();

    // The audio pipe accepts four bytes with a little-endian state word (0 through 3).
    // Initialize and Wakeup publish the structure count and addresses. ReadPipe consumes
    // the whole request when available, or returns zero and leaves the response queued.
    bool WritePipe(u8 pipe, std::span<const u8> bytes);
    std::size_t ReadPipe(u8 pipe, std::span<u8> bytes);
    std::size_t PipeReadable(u8 pipe) const;
    // Register 0 publishes a one-shot acknowledgement for the sleep/shutdown pipe command.
    bool ReceiveDataReady() const;
    u16 ReceiveData();

    // The caller supplies the just-published bank and the opposite bank, with distinct storage.
    // The input holds ARM commands and the returned aux planes. Tick consumes the dirty flags
    // and publishes status, PCM, aux sends and the next counter to the opposite bank.
    // The caller owns the cadence and all notifications. A stopped or sleeping Tick is silent.
    Frame Tick(Bank input, Bank output);

private:
    static constexpr u32 kPhaseBits = 24;
    static constexpr u32 kPhaseOne = 1u << kPhaseBits;

    struct Buffer {
        u32 address{};
        u32 samples{};
        u16 id{};
        u8 predictor{};
        bool has_context{};
        bool loop{};
        s16 y1{};
        s16 y2{};
    };

    using VoiceGains = std::array<std::array<float, 4>, 3>;
    struct Voice {
        std::array<Buffer, 4> queue{};
        std::size_t queue_head{};
        std::size_t queue_size{};
        std::size_t next_slot{};
        Buffer buffer{};
        std::array<s16, 16> coefficients{};
        VoiceGains gain{};
        std::array<s16, 2> simple{};
        std::array<s16, 5> biquad{};
        Stereo simple_y{}, biquad_x1{}, biquad_x2{}, biquad_y1{}, biquad_y2{};
        Stereo older{}, newer{};
        std::array<u8, 8> adpcm_block{};
        s16 adpcm_y1{}, adpcm_y2{};
        u32 step{kPhaseOne};
        u32 phase{};
        u32 next_sample{};
        u32 position{};
        u16 format{};
        u16 sync{};
        u16 filters{};
        u16 previous{};
        u8 interp{};
        u8 selector{2};
        bool enabled{};
        bool changed{};
        bool deferred_start{};
        bool adpcm_block_valid{};
    };

    struct Aux {
        float volume{};
        bool enabled{};
        bool arm{};
        bool delay{};
        bool reverb{};
    };

    enum class State { Off, Running, Sleeping };
    enum class Warning : u8 { Codec, Polyphase, Delay, Reverb, Surround, Count };
    using Mix = std::array<std::array<std::array<s64, 4>, kFrameSamples>, 3>;

    void PublishAddresses();
    void WarnOnce(Warning warning, std::string_view message);
    void ApplyGlobal(u8* config);
    void ApplyVoice(Voice& voice, u8* config, const u8* coefficients);
    void StartBuffer(Voice& voice, Buffer buffer);
    static u32 BufferSamples(const Voice& voice);
    void Advance(Voice& voice);
    Stereo Decode(Voice& voice);
    void RenderVoice(Voice& voice, Mix& mix, std::size_t index, u16 counter,
                     const VoiceGains& previous_gain);

    ReadMemory read_;
    Warn warn_;
    std::array<Voice, 24> voices_{};
    std::array<Aux, 2> aux_{};
    std::array<bool, static_cast<std::size_t>(Warning::Count)> warned_{};
    std::array<u8, 32> pipe_{};
    std::size_t pipe_position_{pipe_.size()};
    State state_{State::Off};
    float master_{1.0f};
    u16 mode_{1};
    bool acknowledge_{};
};

} // namespace mh4n
