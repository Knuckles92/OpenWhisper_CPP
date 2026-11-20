#include "engine/request_options.h"

namespace fv {

const char* native_language_code(const std::string& language) {
    return language.empty() || language == "auto" ? nullptr : language.c_str();
}

nemo_speech_asr_recognition_options request_options(nemo_speech_asr_recognition_options defaults,
                                                    const std::string& language) {
    nemo_speech_asr_recognition_options options = defaults;
    options.language_code = native_language_code(language);
    options.enable_word_time_offsets = true;
    options.enable_automatic_punctuation = true;
    options.interim_results = true;
    return options;
}

} // namespace fv
