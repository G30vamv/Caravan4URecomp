// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: 3dbrew DSP Memory Region, libctru NDSP, and boot-village, early and long MDSP.
#include "dsp_native.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <utility>

namespace mh4n {
namespace {

constexpr std::array<u16, 15> kPipeAddresses{
    0xbfff, 0x9e92, 0x8680, 0xa792, 0x9430, 0x8400, 0x8540, 0x9492,
    0x8710, 0x8410, 0xa912, 0xaa12, 0xaad2, 0xac52, 0xac5c,
};
constexpr std::size_t BankOffset(std::size_t structure) {
    return 2u * (kPipeAddresses[structure] - 0x8000u);
}
u16 Load16(const u8* p) {
    return static_cast<u16>(p[0] | (p[1] << 8));
}
u32 Load32(const u8* p) {
    return static_cast<u32>(Load16(p)) | (static_cast<u32>(Load16(p + 2)) << 16);
}
s16 LoadSigned16(const u8* p) {
    return std::bit_cast<s16>(Load16(p));
}
float LoadFloat(const u8* p) {
    const float value = std::bit_cast<float>(Load32(p));
    return std::isfinite(value) ? value : 0.0f;
}
void Store16(u8* p, u16 value) {
    p[0] = static_cast<u8>(value);
    p[1] = static_cast<u8>(value >> 8);
}
void Store32(u8* p, u32 value) {
    Store16(p, static_cast<u16>(value));
    Store16(p + 2, static_cast<u16>(value >> 16));
}
s16 Saturate16(s64 value) {
    return static_cast<s16>(std::clamp<s64>(value, -32768, 32767));
}
s32 Saturate32(s64 value) {
    return static_cast<s32>(std::clamp<s64>(value, INT32_MIN, INT32_MAX));
}
s32 Scale(s64 sample, float gain) {
    const float value = static_cast<float>(sample) * gain;
    if (value >= 2147483648.0f) {
        return INT32_MAX;
    }
    if (value <= -2147483648.0f) {
        return INT32_MIN;
    }
    return static_cast<s32>(value);
}

} // namespace

NativeDsp::NativeDsp(ReadMemory read, Warn warn)
    : read_(std::move(read)), warn_(std::move(warn)) {}

void NativeDsp::Initialize() {
    voices_ = {};
    aux_ = {};
    master_ = 1.0f;
    mode_ = 1;
    state_ = State::Running;
    acknowledge_ = false;
    PublishAddresses();
}

void NativeDsp::Shutdown() {
    state_ = State::Off;
    pipe_position_ = pipe_.size();
    acknowledge_ = false;
}

void NativeDsp::Sleep() {
    if (state_ == State::Running) {
        state_ = State::Sleeping;
        pipe_position_ = pipe_.size();
    }
}

void NativeDsp::Wakeup() {
    if (state_ == State::Sleeping) {
        state_ = State::Running;
        acknowledge_ = false;
        PublishAddresses();
    }
}

void NativeDsp::PublishAddresses() {
    Store16(pipe_.data(), static_cast<u16>(kPipeAddresses.size()));
    for (std::size_t i = 0; i < kPipeAddresses.size(); ++i) {
        Store16(pipe_.data() + 2 + i * 2, kPipeAddresses[i]);
    }
    pipe_position_ = 0;
}

bool NativeDsp::WritePipe(u8 pipe, std::span<const u8> bytes) {
    if (pipe != kAudioPipe || bytes.size() != 4) {
        return false;
    }
    switch (Load16(bytes.data())) {
    case 0: Initialize(); return true;
    case 1: Shutdown(); acknowledge_ = true; return true;
    case 2: Wakeup(); return true;
    case 3: Sleep(); acknowledge_ = true; return true;
    default: return false;
    }
}

std::size_t NativeDsp::ReadPipe(u8 pipe, std::span<u8> bytes) {
    if (bytes.size() > PipeReadable(pipe)) {
        return 0;
    }
    std::copy_n(pipe_.data() + pipe_position_, bytes.size(), bytes.data());
    pipe_position_ += bytes.size();
    return bytes.size();
}

std::size_t NativeDsp::PipeReadable(u8 pipe) const {
    return pipe == kAudioPipe ? pipe_.size() - pipe_position_ : 0;
}

bool NativeDsp::ReceiveDataReady() const {
    return acknowledge_;
}

u16 NativeDsp::ReceiveData() {
    return std::exchange(acknowledge_, false) ? 1 : 0;
}

void NativeDsp::WarnOnce(Warning warning, std::string_view message) {
    if (!std::exchange(warned_[static_cast<std::size_t>(warning)], true) && warn_) {
        warn_(message);
    }
}

void NativeDsp::ApplyGlobal(u8* config) {
    const u32 flags = Load32(config);
    Store32(config, 0);
    if (flags & 0x10000) {
        master_ = LoadFloat(config + 4);
    }
    if (flags & 0x04000000) {
        mode_ = Load16(config + 0x16);
    }
    if (mode_ > 1) {
        WarnOnce(Warning::Surround, "NativeDsp: surround, compressor and surround biquads are unsupported");
    }
    for (std::size_t bus = 0; bus < aux_.size(); ++bus) {
        auto& aux = aux_[bus];
        if (flags & (0x40u << bus)) {
            aux.enabled = Load16(config + 0x24 + bus * 2) != 0;
        }
        if (flags & (0x100u << bus)) {
            aux.arm = Load16(config + 0x28 + bus * 2) != 0;
        }
        if (flags & (0x1000000u << bus)) {
            aux.volume = LoadFloat(config + 8 + bus * 4);
        }
        // UNVERIFIED: silence the effect return when an unsupported DSP effect is enabled.
        const u8* delay = config + 0x2c + bus * 0x14;
        const u8* reverb = config + 0x54 + bus * 0x34;
        if ((flags & (0x400u << bus)) && (Load16(delay) & 1)) {
            aux.delay = Load16(delay + 2) != 0;
            if (aux.delay) {
                WarnOnce(Warning::Delay, "NativeDsp: DSP delay is unsupported");
            }
        }
        if ((flags & (0x1000u << bus)) && (Load16(reverb) & 1)) {
            aux.reverb = Load16(reverb + 2) != 0;
            if (aux.reverb) {
                WarnOnce(Warning::Reverb, "NativeDsp: DSP reverb is unsupported");
            }
        }
    }
}

void NativeDsp::StartBuffer(Voice& voice, Buffer buffer) {
    voice.buffer = buffer;
    voice.position = 0;
    voice.next_sample = 0;
    voice.adpcm_block_valid = false;
    if (buffer.has_context) {
        voice.adpcm_y1 = buffer.y1;
        voice.adpcm_y2 = buffer.y2;
    }
}

u32 NativeDsp::BufferSamples(const Voice& voice) {
    // ADPCM playback exposes both nibbles of the last byte to the resampler.
    const u32 count = voice.buffer.samples;
    return (voice.format & 0x0c) == 8 && count != UINT32_MAX ? (count + 1) & ~1u : count;
}

void NativeDsp::ApplyVoice(Voice& voice, u8* config, const u8* coefficients) {
    const u32 flags = Load32(config);
    Store32(config, 0);
    voice.changed = false;
    if (flags & 0x20000000) {
        voice = {};
    }
    if (flags & 0x10) {
        voice.queue_head = voice.queue_size = voice.next_slot = 0;
    }
    if (flags & 0x10000000) {
        voice.sync = Load16(config + 0xa2);
    }
    if (flags & 0x10000) {
        voice.enabled = config[0xa0] != 0;
    }
    for (std::size_t bus = 0; bus < voice.gain.size(); ++bus) {
        if (flags & (0x02000000u << bus)) {
            for (std::size_t channel = 0; channel < 4; ++channel) {
                voice.gain[bus][channel] = LoadFloat(config + 4 + bus * 16 + channel * 4);
            }
        }
    }
    if (flags & 0x40000) {
        const float rate = std::clamp(LoadFloat(config + 0x34), 0.0f, 255.0f);
        voice.step = static_cast<u32>(rate * kPhaseOne);
    }
    if (flags & 0x20000) {
        voice.interp = config[0x38];
        voice.selector = config[0x39];
    }
    if (flags & 4) {
        for (std::size_t i = 0; i < voice.coefficients.size(); ++i) {
            voice.coefficients[i] = LoadSigned16(coefficients + i * 2);
        }
    }
    // UNVERIFIED: the Q1.15 and Q2.14 filter coefficients follow the public ABI.
    if (flags & 0x400000) {
        voice.filters = Load16(config + 0x3a);
    }
    if (flags & 0x800000) {
        for (std::size_t i = 0; i < voice.simple.size(); ++i) {
            voice.simple[i] = LoadSigned16(config + 0x3c + i * 2);
        }
    }
    if (flags & 0x1000000) {
        for (std::size_t i = 0; i < voice.biquad.size(); ++i) {
            voice.biquad[i] = LoadSigned16(config + 0x40 + i * 2);
        }
    }
    if (flags & 0x40200000) {
        const u16 options = Load16(config + 0xbc);
        voice.format = Load16(config + 0xb4);
        voice.older = voice.newer = {};
        voice.simple_y = voice.biquad_x1 = voice.biquad_x2 = voice.biquad_y1 = voice.biquad_y2 = {};
        voice.adpcm_y1 = voice.adpcm_y2 = 0;
        voice.phase = 0;
        voice.previous = 0;
        StartBuffer(voice, {std::rotl(Load32(config + 0xac), 16),
            std::rotl(Load32(config + 0xb0), 16), Load16(config + 0xbe), config[0xb6],
            (options & 1) != 0, (options & 2) != 0,
            LoadSigned16(config + 0xb8), LoadSigned16(config + 0xba)});
        const u32 position = std::min(std::rotl(Load32(config + 0xa4), 16), BufferSamples(voice));
        voice.deferred_start = position == 0;
        if (voice.buffer.address != 0) {
            while (voice.next_sample < position) {
                voice.older = voice.newer;
                voice.newer = Decode(voice);
                ++voice.next_sample;
            }
        }
        voice.next_sample = position;
        voice.position = position == 0 ? 0 : position - 1;
    } else if ((flags & 8) && voice.buffer.address == std::rotl(Load32(config + 0xac), 16)) {
        voice.buffer.samples = std::rotl(Load32(config + 0xb0), 16);
        voice.buffer.id = Load16(config + 0xbe);
        voice.buffer.loop = (Load16(config + 0xbc) & 2) != 0;
    }
    if (flags & 0x80000) {
        const u16 dirty = Load16(config + 0x4a);
        Store16(config + 0x4a, 0);
        const std::size_t first = voice.next_slot;
        for (std::size_t i = 0; i < voice.queue.size(); ++i) {
            const std::size_t slot = (first + i) % voice.queue.size();
            if (!(dirty & (1u << slot)) || voice.queue_size == voice.queue.size()) {
                continue;
            }
            const u8* b = config + 0x4c + slot * 0x14;
            voice.queue[(voice.queue_head + voice.queue_size) % voice.queue.size()] = {
                std::rotl(Load32(b), 16), std::rotl(Load32(b + 4), 16), Load16(b + 0x10),
                b[8], b[0xe] != 0, b[0xf] != 0, LoadSigned16(b + 0xa), LoadSigned16(b + 0xc)};
            ++voice.queue_size;
            voice.next_slot = (slot + 1) % voice.queue.size();
        }
    }
}

NativeDsp::Stereo NativeDsp::Decode(Voice& voice) {
    const u32 sample = voice.next_sample;
    const u8 codec = static_cast<u8>((voice.format >> 2) & 3);
    const bool stereo = (voice.format & 3) == 2;
    const auto read = [&](u64 offset, u8* dst, u32 size) {
        const u64 address = static_cast<u64>(voice.buffer.address) + offset;
        return voice.buffer.address != 0 && address + size <= (1ull << 32) &&
               read_ && read_(static_cast<u32>(address), dst, size);
    };
    if (codec == 2 && !stereo) {
        const u32 nibble = sample % 14;
        if (nibble == 0) {
            voice.adpcm_block_valid = read(static_cast<u64>(sample / 14) * 8,
                                           voice.adpcm_block.data(), 8);
        }
        if (!voice.adpcm_block_valid) {
            voice.adpcm_y1 = voice.adpcm_y2 = 0;
            return {};
        }
        const u8 header = sample < 14 && voice.buffer.has_context
            ? voice.buffer.predictor : voice.adpcm_block[0];
        const u8 packed = voice.adpcm_block[1 + nibble / 2];
        const s32 raw = (packed >> ((nibble & 1) ? 0 : 4)) & 15;
        const s32 delta = raw < 8 ? raw : raw - 16;
        const std::size_t pair = ((header >> 4) & 7) * 2;
        const s64 prediction = static_cast<s64>(voice.coefficients[pair]) * voice.adpcm_y1 +
                               static_cast<s64>(voice.coefficients[pair + 1]) * voice.adpcm_y2;
        const s64 residual = static_cast<s64>(delta) * (1u << (11 + (header & 15)));
        const s16 value = Saturate16((prediction + residual + 1024) >> 11);
        voice.adpcm_y2 = voice.adpcm_y1;
        voice.adpcm_y1 = value;
        return {value, value};
    }
    // UNVERIFIED: PCM8 signed bytes and PCM16 little endian, interleaved for stereo.
    if (codec <= 1) {
        const u32 channels = stereo ? 2 : 1;
        const u32 width = codec + 1;
        std::array<u8, 4> bytes{};
        if (read(static_cast<u64>(sample) * channels * width, bytes.data(), channels * width)) {
            Stereo result{};
            for (std::size_t ch = 0; ch < channels; ++ch) {
                result[ch] = codec == 0 ? static_cast<s16>(std::bit_cast<s8>(bytes[ch]) * 256)
                                        : LoadSigned16(bytes.data() + ch * 2);
            }
            return {result[0], stereo ? result[1] : result[0]};
        }
        return {};
    }
    // UNVERIFIED: a single descriptor does not specify two ADPCM predictor histories.
    WarnOnce(Warning::Codec, "NativeDsp: stereo ADPCM or reserved sample codec is unsupported");
    return {};
}

void NativeDsp::Advance(Voice& voice) {
    while (voice.next_sample >= BufferSamples(voice)) {
        // UNVERIFIED: loop the descriptor and reload its initial ADPCM context.
        if (voice.buffer.loop && voice.buffer.samples != 0) {
            StartBuffer(voice, voice.buffer);
        } else if (voice.queue_size != 0) {
            const Buffer buffer = voice.queue[voice.queue_head];
            voice.queue_head = (voice.queue_head + 1) % voice.queue.size();
            --voice.queue_size;
            StartBuffer(voice, buffer);
            voice.changed = true;
        } else {
            voice.older = voice.newer;
            voice.newer = {};
            return;
        }
    }
    voice.older = voice.newer;
    voice.newer = Decode(voice);
    ++voice.next_sample;
}

void NativeDsp::RenderVoice(Voice& voice, Mix& mix, std::size_t index, u16 counter,
                            const VoiceGains& previous_gain) {
    if (!voice.enabled || std::exchange(voice.deferred_start, false)) {
        return;
    }
    const auto ends_after_current = [&] {
        return voice.queue_size == 0 && (!voice.buffer.loop || voice.buffer.samples == 0);
    };
    if (voice.next_sample >= BufferSamples(voice) && voice.position >= BufferSamples(voice) &&
        ends_after_current()) {
        voice.changed = true;
        voice.enabled = false;
        voice.previous = voice.buffer.id;
        voice.buffer.id = 0;
        return;
    }
    // libctru identifies modes 0/1/2, and only uses the selector in mode 0.
    // The reference PCM in long MDSP uses the same two-sample path for selector 0
    // at the observed PCM16 stereo rate. Other selector 0 inputs are UNVERIFIED.
    // MODIFIED (2026-11-20): selector 1 (and any other polyphase filter) played silent before: a monster roar uses it.
    // The polyphase filters are not modelled; they are heard as the two-sample (linear) path, as selector 0 and 2 are.
    const bool supported = voice.interp <= 2;
    const bool ramp_gain = previous_gain != voice.gain;
    if (voice.interp == 0 && voice.selector == 1 && !warned_[static_cast<std::size_t>(Warning::Polyphase)]) {
        WarnOnce(Warning::Polyphase,
                 std::format("NativeDsp: polyphase filter selector {} is played as linear interpolation (mode {}, voice {}, frame counter {})",
                             voice.selector, voice.interp, index, counter));
    }
    for (std::size_t i = 0; i < kFrameSamples; ++i) {
        // The voice publishes the source index used for the last output sample.
        voice.position = voice.next_sample;
        const bool exhausted = voice.next_sample >= BufferSamples(voice) && ends_after_current();
        Stereo sample{};
        for (std::size_t ch = 0; ch < 2; ++ch) {
            if (supported && !exhausted && voice.interp <= 1) {
                const s32 difference = voice.newer[ch] - voice.older[ch];
                // early MDSP frame 751 saturates the difference for selector 2.
                // UNVERIFIED on hardware: explicit linear mode uses the full difference.
                const s32 delta = voice.interp == 0 ? Saturate16(difference) : difference;
                sample[ch] = static_cast<s16>(voice.older[ch] +
                                              (static_cast<s64>(delta) * voice.phase >> kPhaseBits));
            } else if (supported && !exhausted) {
                // UNVERIFIED on hardware: hold the latest decoded sample.
                sample[ch] = voice.newer[ch];
            }
            if (voice.filters & 1) {
                const s64 y = static_cast<s64>(voice.simple[0]) * sample[ch] +
                              static_cast<s64>(voice.simple[1]) * voice.simple_y[ch];
                sample[ch] = voice.simple_y[ch] = Saturate16(y >> 15);
            }
            if (voice.filters & 2) {
                const s64 y = static_cast<s64>(voice.biquad[4]) * sample[ch] +
                              static_cast<s64>(voice.biquad[3]) * voice.biquad_x1[ch] +
                              static_cast<s64>(voice.biquad[2]) * voice.biquad_x2[ch] +
                              static_cast<s64>(voice.biquad[1]) * voice.biquad_y1[ch] +
                              static_cast<s64>(voice.biquad[0]) * voice.biquad_y2[ch];
                voice.biquad_x2[ch] = voice.biquad_x1[ch];
                voice.biquad_x1[ch] = sample[ch];
                voice.biquad_y2[ch] = voice.biquad_y1[ch];
                sample[ch] = voice.biquad_y1[ch] = Saturate16(y >> 14);
            }
        }
        if (supported) {
            // long MDSP frames 6490 and 8135 match the old gain at sample 0
            // and the newly published gain at sample 159.
            const float fraction = static_cast<float>(i) / static_cast<float>(kFrameSamples - 1);
            for (std::size_t bus = 0; bus < mix.size(); ++bus) {
                for (std::size_t ch = 0; ch < 4; ++ch) {
                    // UNVERIFIED: rear gains and aux 1 use the same per-plane gain.
                    const float target = voice.gain[bus][ch];
                    const float gain = ramp_gain ? std::lerp(previous_gain[bus][ch], target, fraction) : target;
                    mix[bus][i][ch] += Scale(sample[ch % 2], gain);
                }
            }
        }
        const u64 next_phase = static_cast<u64>(voice.phase) + voice.step;
        voice.phase = static_cast<u32>(next_phase % kPhaseOne);
        for (u32 step = 0; step < next_phase / kPhaseOne; ++step) {
            Advance(voice);
        }
    }
}

NativeDsp::Frame NativeDsp::Tick(Bank input, Bank output) {
    Frame pcm{};
    if (state_ != State::Running) {
        return pcm;
    }
    Mix mix{};
    const u16 counter = Load16(input.data() + BankOffset(0));
    ApplyGlobal(input.data() + BankOffset(4));
    for (std::size_t i = 0; i < voices_.size(); ++i) {
        auto& voice = voices_[i];
        u8* config = input.data() + BankOffset(1) + i * 0xc0;
        const bool ramp_gain = voice.enabled && (Load32(config) & 0x20000000) == 0;
        const auto previous_gain = voice.gain;
        ApplyVoice(voice, config, input.data() + BankOffset(3) + i * 0x20);
        RenderVoice(voice, mix, i, counter, ramp_gain ? previous_gain : voice.gain);
        u8* status = output.data() + BankOffset(2) + i * 0xc;
        status[0] = voice.enabled;
        status[1] = voice.changed;
        Store16(status + 2, voice.sync);
        Store32(status + 4, std::rotl(voice.position, 16));
        Store16(status + 8, voice.buffer.id);
        Store16(status + 0xa, voice.previous);
    }
    for (std::size_t i = 0; i < kFrameSamples; ++i) {
        auto main = mix[0][i];
        for (std::size_t bus = 0; bus < aux_.size(); ++bus) {
            const auto& aux = aux_[bus];
            for (std::size_t ch = 0; ch < 4; ++ch) {
                const std::size_t offset = BankOffset(7) + 0xa00 * bus + 4 * (ch * kFrameSamples + i);
                const s32 send = aux.enabled ? Saturate32(mix[bus + 1][i][ch]) : 0;
                const s32 returned = aux.arm ? std::bit_cast<s32>(Load32(input.data() + offset)) : send;
                Store32(output.data() + offset, static_cast<u32>(send));
                if (aux.enabled && !aux.delay && !aux.reverb) {
                    main[ch] += Scale(returned, aux.volume);
                }
            }
        }
        if (mode_ == 0) {
            // UNVERIFIED on hardware: downmix the front pair after aux and before clipping.
            pcm[i].fill(Saturate16(Scale((main[0] + main[1]) / 2, master_)));
        } else if (mode_ == 1) {
            pcm[i] = {Saturate16(Scale(main[0], master_)), Saturate16(Scale(main[1], master_))};
        }
        for (std::size_t ch = 0; ch < 2; ++ch) {
            Store16(output.data() + BankOffset(6) + (i * 2 + ch) * 2,
                    static_cast<u16>(pcm[i][ch]));
        }
    }
    Store16(output.data() + BankOffset(5) + 2, 0);
    Store16(output.data() + BankOffset(0), counter == 0xffff ? 2 : static_cast<u16>(counter + 1));
    return pcm;
}

} // namespace mh4n
