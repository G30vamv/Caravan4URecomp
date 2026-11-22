// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: swkbd.h, mh4n/applet_frontend.h, apt_test.cpp and applet_frontend_test.cpp.

#include "swkbd.h"

#include <algorithm>
#include <cstring>

#include "frontend_hub.h"
#include "mh4n/cfg.h"
#include "mh4n/log.h"

namespace mh4n::apt {
namespace {

constexpr const char* kLog = "Service.APT";
constexpr u32 kNoButton = static_cast<u32>(SoftwareKeyboardButtonConfig::NoButton);
constexpr char32_t kReplacement = 0xFFFD;

bool HighSurrogate(char16_t unit) {
    return unit >= 0xD800 && unit <= 0xDBFF;
}

bool LowSurrogate(char16_t unit) {
    return unit >= 0xDC00 && unit <= 0xDFFF;
}

void Truncate(std::u16string& input, std::size_t max_units) {
    if (input.size() > max_units) {
        if (max_units != 0 && HighSurrogate(input[max_units - 1])) {
            --max_units;
        }
        input.resize(max_units);
    }
}

template <std::size_t N>
std::u16string ReadString(const u16 (&input)[N]) {
    return {input, std::find(input, input + N, u16{0})};
}

std::vector<u8> EncodeConfig(const SoftwareKeyboardConfig& config) {
    const auto* bytes = reinterpret_cast<const u8*>(&config);
    return {bytes, bytes + sizeof(config)};
}

struct TextSelection {
    std::size_t next = 0;
};

}  // namespace

std::u16string Utf8ToUtf16(const std::string& text) {
    std::u16string result;
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<u8>(text[i]);
        unsigned count = 1;
        char32_t codepoint = kReplacement;
        char32_t minimum = 0;
        if (lead <= 0x7F) {
            codepoint = lead;
        } else if (lead >= 0xC2 && lead <= 0xDF) {
            count = 2;
            minimum = 0x80;
            codepoint = lead & 0x1Fu;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            count = 3;
            minimum = 0x800;
            codepoint = lead & 0x0Fu;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            count = 4;
            minimum = 0x10000;
            codepoint = lead & 7u;
        }

        bool valid = count <= text.size() - i;
        for (unsigned j = 1; valid && j < count; ++j) {
            const auto byte = static_cast<u8>(text[i + j]);
            valid = (byte & 0xC0) == 0x80;
            codepoint = (codepoint << 6) | (byte & 0x3Fu);
        }
        if (!valid || codepoint < minimum || codepoint > 0x10FFFF ||
            (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
            codepoint = kReplacement;
            count = 1;
        }
        if (codepoint <= 0xFFFF) {
            result.push_back(static_cast<char16_t>(codepoint));
        } else {
            codepoint -= 0x10000;
            result.push_back(static_cast<char16_t>(0xD800 + (codepoint >> 10)));
            result.push_back(static_cast<char16_t>(0xDC00 + (codepoint & 0x3FF)));
        }
        i += count;
    }
    return result;
}

std::string Utf16ToUtf8(const std::u16string& text) {
    std::string result;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char32_t codepoint = text[i];
        if (HighSurrogate(text[i]) && i + 1 < text.size() && LowSurrogate(text[i + 1])) {
            const char32_t high = codepoint - 0xD800;
            const char32_t low = char32_t{text[++i]} - 0xDC00;
            codepoint = 0x10000 + (high << 10) + low;
        } else if (HighSurrogate(text[i]) || LowSurrogate(text[i])) {
            codepoint = kReplacement;
        }

        const auto append = [&result](char32_t byte) {
            result.push_back(static_cast<char>(byte));
        };
        if (codepoint < 0x80) {
            append(codepoint);
            continue;
        }
        if (codepoint >= 0x10000) {
            append(0xF0 | (codepoint >> 18));
            append(0x80 | ((codepoint >> 12) & 0x3F));
        } else if (codepoint >= 0x800) {
            append(0xE0 | (codepoint >> 12));
        } else {
            append(0xC0 | (codepoint >> 6));
        }
        if (codepoint >= 0x800) {
            append(0x80 | ((codepoint >> 6) & 0x3F));
        }
        append(0x80 | (codepoint & 0x3F));
    }
    return result;
}

KeyboardRequest MakeKeyboardRequest(const SoftwareKeyboardConfig& config, SharedMemory* text_memory) {
    KeyboardRequest request;
    request.type = static_cast<KeyboardType>(config.type);
    request.buttons = static_cast<KeyboardButtons>(config.num_buttons_m1);
    for (std::size_t i = 0; i < request.button_text.size(); ++i) {
        request.button_text[i] = ReadString(config.button_text[i]);
    }
    request.hint = ReadString(config.hint_text);
    request.max_text_length = config.max_text_length;
    request.accept = static_cast<KeyboardAccept>(config.valid_input);
    request.password = static_cast<KeyboardPassword>(config.password_mode);
    request.multiline = config.multiline != 0;
    request.max_digits = config.max_digits;
    const u32 flags = config.filter_flags;
    request.filters = {
        (flags & kFilterDigits) != 0,
        (flags & kFilterAt) != 0 && config.skip_at_check == 0,
        (flags & kFilterPercent) != 0,
        (flags & kFilterBackslash) != 0,
        (flags & kFilterProfanity) != 0,
        (flags & kFilterCallback) != 0,
    };
    const u32 offset = config.initial_text_offset;
    if (text_memory && offset < text_memory->Size()) {
        const auto* input = text_memory->GetPointer(offset);
        const auto units = std::min<u32>((text_memory->Size() - offset) / 2, config.max_text_length);
        for (u32 i = 0; input && i < units; ++i) {
            const auto unit = static_cast<char16_t>(input[2 * i] | (u32{input[2 * i + 1]} << 8));
            if (!unit) {
                break;
            }
            request.initial_text.push_back(unit);
        }
    }
    return request;
}

KeyboardConfig ToFrontendConfig(const KeyboardRequest& request) {
    return {static_cast<u32>(request.buttons), static_cast<u32>(request.accept), request.multiline,
            request.max_text_length, request.max_digits, request.filters.digits, request.filters.at,
            request.filters.percent, request.filters.backslash, request.filters.profanity,
            request.filters.callback};
}

KeyboardConfig ToFrontendConfig(const SoftwareKeyboardConfig& config) {
    return ToFrontendConfig(MakeKeyboardRequest(config, nullptr));
}

ValidationError CheckFilters(const KeyboardConfig& config, const std::u16string& input) {
    if (config.prevent_digit &&
        std::ranges::count_if(input, [](char16_t unit) { return unit >= u'0' && unit <= u'9'; }) >
            config.max_digits) {
        return ValidationError::MaxDigitsExceeded;
    }
    if (config.prevent_at && input.find(u'@') != std::u16string::npos) {
        return ValidationError::AtSignNotAllowed;
    }
    if (config.prevent_percent && input.find(u'%') != std::u16string::npos) {
        return ValidationError::PercentNotAllowed;
    }
    if (config.prevent_backslash && input.find(u'\\') != std::u16string::npos) {
        return ValidationError::BackslashNotAllowed;
    }
    return ValidationError::None;
}

ValidationError CheckInput(const KeyboardConfig& config, const std::u16string& input) {
    const auto mode = static_cast<SoftwareKeyboardValidInput>(config.accept_mode);
    if (mode == SoftwareKeyboardValidInput::FixedLen && input.size() != config.max_text_length) {
        return ValidationError::FixedLengthRequired;
    }
    if (input.size() > config.max_text_length) {
        return ValidationError::MaxLengthExceeded;
    }
    if ((mode == SoftwareKeyboardValidInput::NotBlank ||
         mode == SoftwareKeyboardValidInput::NotEmptyNotBlank) &&
        std::ranges::all_of(input, [](char16_t unit) {
            return unit == u' ' || (unit >= u'\t' && unit <= u'\r');
        })) {
        return ValidationError::BlankInputNotAllowed;
    }
    if (mode == SoftwareKeyboardValidInput::NotEmpty && input.empty()) {
        return ValidationError::EmptyInputNotAllowed;
    }
    if (config.accept_mode > static_cast<u32>(SoftwareKeyboardValidInput::FixedLen)) {
        return ValidationError::UnknownConfiguration;
    }
    return ValidationError::None;
}

ValidationError CheckButton(const KeyboardConfig& config, u8 button) {
    if (config.button_config > kNoButton) {
        return ValidationError::UnknownConfiguration;
    }
    const u32 max_button = config.button_config == kNoButton ? 0 : config.button_config;
    return button <= max_button ? ValidationError::None : ValidationError::ButtonOutOfRange;
}

ValidationError CheckFinalize(const KeyboardConfig& config, const std::u16string& text, u8 button) {
    if (config.button_config > kNoButton) {
        return ValidationError::UnknownConfiguration;
    }
    if (button != config.button_config || config.button_config == kNoButton) {
        return ValidationError::None;
    }
    const auto input_result = CheckInput(config, text);
    return input_result == ValidationError::None ? CheckFilters(config, text) : input_result;
}

void DefaultKeyboard::Open() {
    const auto state = context_.State<TextSelection>("swkbd.text");
    const auto texts = context_.GetOptions().All("--swkbd-text");
    if (state->next < texts.size()) {
        text_ = Utf8ToUtf16(texts[state->next++]);
    } else {
        text_ = cfg::GetUserName(context_);
    }
}

void DefaultKeyboard::Execute(const KeyboardConfig& config) {
    Configure(config);
    if (config.button_config > kNoButton) {
        MH4N_LOG(Warning, kLog, "Unknown button config {}", config.button_config);
        return;
    }
    const auto button = static_cast<u8>(config.button_config == kNoButton ? 0 : config.button_config);
    auto text = text_;
    Truncate(text, config.max_text_length);
    const auto validation = Finalize(text, button);
    if (validation == ValidationError::UnknownConfiguration) {
        MH4N_LOG(Warning, kLog, "unknown validation method {}", config.accept_mode);
    } else if (validation != ValidationError::None) {
        MH4N_LOG(Warning, kLog, "the default keyboard's text is not accepted");
    }
}

void DefaultKeyboard::ShowError(const std::u16string& error) {
    MH4N_LOG(Warning, kLog, "Default keyboard text is unaccepted! error: {}", Utf16ToUtf8(error));
}

ValidationError DefaultKeyboard::ValidateFilters(const std::u16string& input) const {
    return CheckFilters(config_, input);
}

ValidationError DefaultKeyboard::ValidateInput(const std::u16string& input) const {
    return CheckInput(config_, input);
}

ValidationError DefaultKeyboard::ValidateButton(u8 button) const {
    return CheckButton(config_, button);
}

ValidationError DefaultKeyboard::Finalize(const std::u16string& text, u8 button) {
    const auto result = CheckFinalize(config_, text, button);
    data_ready_ = result == ValidationError::None;
    if (data_ready_) {
        data_ = {text, button};
    }
    return result;
}

SoftwareKeyboard::SoftwareKeyboard(AppletId id, AppletId parent, std::weak_ptr<AppletHost> host,
                                   ServiceContext& context)
    : Applet(id, parent, std::move(host)), context_(context), keyboard_(context) {}

Result SoftwareKeyboard::Start(const MessageParameter& parameter) {
    if (parameter.buffer.size() != sizeof(config_)) {
        MH4N_LOG(Warning, kLog, "The size of the parameter (SoftwareKeyboardConfig) is wrong: {} bytes",
                 parameter.buffer.size());
        return kResultUnknown;
    }
    std::memcpy(&config_, parameter.buffer.data(), sizeof(config_));
    text_memory_ = ObjectCast<SharedMemory>(parameter.object);
    frontend_ = FindFrontendHub(context_);
    if (!frontend_ || !frontend_->Keyboard()) {
        frontend_.reset();
        MH4N_LOG(Info, kLog, "no keyboard frontend was set");
        keyboard_.Open();
    }
    is_active_ = true;
    Execute();
    return kResultSuccess;
}

void SoftwareKeyboard::Execute() {
    if (auto frontend = frontend_ ? frontend_->Keyboard() : nullptr) {
        const auto request = MakeKeyboardRequest(config_, text_memory_.get());
        keyboard_.Configure(ToFrontendConfig(request));
        frontend_->Drop();
        awaiting_answer_ = true;
        MH4N_LOG(Info, kLog, "the app's keyboard is shown");
        frontend->Execute(request);
    } else {
        keyboard_.Execute(ToFrontendConfig(config_));
    }
}

void SoftwareKeyboard::ShowError(const std::u16string& message) {
    if (auto frontend = frontend_ ? frontend_->Keyboard() : nullptr) {
        frontend->ShowError(message);
    } else {
        keyboard_.ShowError(message);
    }
}

Result SoftwareKeyboard::ReceiveParameterImpl(const MessageParameter& parameter) {
    if (is_active_ && parameter.signal == SignalType::Message &&
        (config_.filter_flags & kFilterCallback) != 0 && parameter.buffer.size() == sizeof(config_)) {
        SoftwareKeyboardConfig verdict{};
        std::memcpy(&verdict, parameter.buffer.data(), sizeof(verdict));
        switch (static_cast<SoftwareKeyboardCallbackResult>(verdict.callback_result)) {
        case SoftwareKeyboardCallbackResult::OK:
            return Finalize();
        case SoftwareKeyboardCallbackResult::Close:
            ShowError(ReadString(verdict.callback_msg));
            config_.text_length = 0;
            config_.return_code = static_cast<s32>(SoftwareKeyboardResult::BannedInput);
            return Finalize();
        case SoftwareKeyboardCallbackResult::Continue:
            ShowError(ReadString(verdict.callback_msg));
            Execute();
            return kResultSuccess;
        }
        return kResultUnknown;
    }
    return AnswerFramebufferRequest(context_, parameter, framebuffer_memory_, "SoftwareKeyboard");
}

void SoftwareKeyboard::Update() {
    if (awaiting_answer_) {
        const auto answer = frontend_->Take();
        if (!answer) {
            return;
        }
        awaiting_answer_ = false;
        if (keyboard_.ValidateButton(answer->button) != ValidationError::None ||
            keyboard_.Finalize(answer->text, answer->button) != ValidationError::None) {
            MH4N_LOG(Warning, kLog, "the app's keyboard answer was rejected, it is asked again");
            Execute();
            return;
        }
    }
    if (!keyboard_.DataReady()) {
        return;
    }

    auto [text, button] = keyboard_.ReceiveData();
    constexpr std::array<s32, 4> first_code{0, 1, 3, -1};
    const auto buttons = config_.num_buttons_m1;
    config_.return_code = first_code[buttons] + button;
    config_.text_offset = 0;

    if (text_memory_ && text_memory_->Size() >= 2) {
        const std::size_t capacity = std::min<std::size_t>(text_memory_->Size() / 2 - 1, 0xFFFF);
        if (text.size() > capacity) {
            MH4N_LOG(Warning, kLog, "the keyboard text does not fit the shared memory block");
            Truncate(text, capacity);
        }
        if (auto* destination = text_memory_->GetPointer()) {
            for (std::size_t i = 0; i < text.size(); ++i) {
                destination[2 * i] = static_cast<u8>(text[i]);
                destination[2 * i + 1] = static_cast<u8>(text[i] >> 8);
            }
            destination[2 * text.size()] = 0;
            destination[2 * text.size() + 1] = 0;
        }
    } else {
        text.clear();
    }

    config_.text_length = static_cast<u16>(text.size());
    if ((config_.filter_flags & kFilterCallback) != 0 && button == buttons) {
        SendParameter({id_, parent_, SignalType::Message, {}, EncodeConfig(config_)});
    } else {
        Finalize();
    }
}

Result SoftwareKeyboard::Finalize() {
    if (frontend_) {
        frontend_->Drop();
    }
    awaiting_answer_ = false;
    CloseApplet(nullptr, EncodeConfig(config_));
    return kResultSuccess;
}

MH4N_OPTION("runner", "--swkbd-text", "TEXT", "answer a default software keyboard with TEXT");
MH4N_OPTION("ipc-replay", "--swkbd-text", "TEXT", "answer a default software keyboard with TEXT");

}  // namespace mh4n::apt
