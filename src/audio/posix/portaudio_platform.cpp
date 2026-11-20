#include "audio/detail/portaudio_platform.h"

#include <pthread.h>

#include <string>

namespace fv::audio::detail {

std::string device_name(const PaDeviceInfo& info) {
    // Every non-Windows host API names devices in UTF-8.
    return info.name == nullptr ? std::string{} : std::string(info.name);
}

void* input_stream_info(const PaDeviceInfo& /*info*/, bool /*wasapi_auto_convert*/,
                        std::shared_ptr<void>& /*storage*/) {
    return nullptr; // WASAPI exists only on Windows.
}

void name_this_thread(const char* name) {
#if defined(__APPLE__)
    pthread_setname_np(name);
#else
    // Linux limits the name to 15 bytes.
    const std::string shortened = std::string(name).substr(0, 15);
    pthread_setname_np(pthread_self(), shortened.c_str());
#endif
}

} // namespace fv::audio::detail
