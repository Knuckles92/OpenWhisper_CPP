#include "audio/detail/portaudio_platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <pa_win_wasapi.h>

#include <cstring>
#include <string_view>

namespace fv::audio::detail {

namespace {

bool valid_utf8(std::string_view text) {
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                               static_cast<int>(text.size()), nullptr, 0) > 0 ||
           text.empty();
}

std::string ansi_to_utf8(std::string_view text) {
    const int wide =
        MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring buffer(static_cast<std::size_t>(wide), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), buffer.data(), wide);
    const int narrow =
        WideCharToMultiByte(CP_UTF8, 0, buffer.data(), wide, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(narrow), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buffer.data(), wide, out.data(), narrow, nullptr, nullptr);
    return out;
}

} // namespace

std::string device_name(const PaDeviceInfo& info) {
    const std::string_view raw = info.name == nullptr ? std::string_view{} : info.name;
    if (valid_utf8(raw)) {
        return std::string(raw);
    }
    // MME and DirectSound may hand over ANSI names. sounddevice raises for any other host API;
    // reading those as ANSI as well keeps the device listed.
    return ansi_to_utf8(raw);
}

void* input_stream_info(const PaDeviceInfo& info, bool wasapi_auto_convert,
                        std::shared_ptr<void>& storage) {
    const PaHostApiInfo* host = Pa_GetHostApiInfo(info.hostApi);
    if (!wasapi_auto_convert || host == nullptr || host->type != paWASAPI) {
        return nullptr;
    }
    auto settings = std::make_shared<PaWasapiStreamInfo>();
    std::memset(settings.get(), 0, sizeof(PaWasapiStreamInfo));
    settings->size = sizeof(PaWasapiStreamInfo);
    settings->hostApiType = paWASAPI;
    settings->version = 1;
    settings->flags = paWinWasapiAutoConvert;
    storage = settings;
    return settings.get();
}

void name_this_thread(const char* name) {
    const int count = MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wide.data(), count);
    SetThreadDescription(GetCurrentThread(), wide.c_str());
}

} // namespace fv::audio::detail
