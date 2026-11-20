#include "dictation/recognition.h"

#include "base/log.h"
#include "speech/dictation_language.h"
#include "text/dictionary.h"

namespace fv::dictation {

namespace {
constexpr log::Logger kLog{"services.dictation_pipeline"};
} // namespace

RecognitionContext recognition_for(const DictationJob*, const Json& settings) {
    static const Json kEmpty = Json::object();
    const Json& values = settings.is_object() ? settings : kEmpty;
    try {
        RecognitionContext context;
        // Empty while dictionary_steer_recognition is off.
        context.phrases = text::recognition_phrases(values);
        context.language = speech::job_language(values);
        return context;
    } catch (...) {
        kLog.warning("Recognition context unavailable; using defaults");
        return RecognitionContext{};
    }
}

std::optional<RecognitionContext>
context_for_backend(const std::optional<RecognitionContext>& recognition,
                    bool supports_recognition) {
    if (!recognition || !supports_recognition ||
        (recognition->language.empty() && recognition->phrases.empty())) {
        return std::nullopt;
    }
    return recognition;
}

} // namespace fv::dictation
