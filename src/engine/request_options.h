#pragma once

#include <string>

#include "nemo_speech/asr.h"

namespace fv {

// The language_code a request sends: null (the model detects the language) for "" and "auto",
// else `language`'s characters, which must outlive the request.
const char* native_language_code(const std::string& language);

// The options of every decode (services/local_asr/nvidia.py _options): the runtime's defaults
// with word time offsets, automatic punctuation and interim results on, and the language of
// native_language_code(language).
nemo_speech_asr_recognition_options request_options(nemo_speech_asr_recognition_options defaults,
                                                    const std::string& language);

} // namespace fv
