#pragma once

#include <portaudio.h>

#include <memory>
#include <string>

// The platform half of the PortAudio backend: one implementation in win/, one in posix/.
namespace fv::audio::detail {

// A device name as sounddevice decodes it: UTF-8, falling back to the ANSI code page ("mbcs")
// for MME and DirectSound names that are not UTF-8.
std::string device_name(const PaDeviceInfo& info);

// hostApiSpecificStreamInfo for an input stream on `info`: WASAPI's auto-convert settings when
// asked for and the device is on WASAPI (sounddevice's WasapiSettings(auto_convert=True)), else
// null. `storage` owns what the pointer points to.
void* input_stream_info(const PaDeviceInfo& info, bool wasapi_auto_convert,
                        std::shared_ptr<void>& storage);

// Names a thread for debuggers and crash dumps (the threads Python names).
void name_this_thread(const char* name);

} // namespace fv::audio::detail
