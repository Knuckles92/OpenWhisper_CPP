#pragma once

#include <optional>

#include "base/json.h"
#include "dictation/job.h"

namespace fv::dictation {

// dictation_pipeline.recognition_for: the language and vocabulary of a final pass starting now.
// The language is speech::job_language's ("" for the engine's own setting); the phrases are the
// dictionary's recognition phrases (starred first) while dictionary_steer_recognition is on, else
// none. `job` is unused, as in Python. Never throws: a failure is logged and gives the defaults.
// Affinity: thread-safe.
RecognitionContext recognition_for(const DictationJob* job, const Json& settings);

// recognition_context.transcribe's seam: the context a decode is given. A backend that does not
// take one (`supports_recognition` false), or an empty context, decodes exactly as without a
// context: nullopt. Affinity: thread-safe.
std::optional<RecognitionContext>
context_for_backend(const std::optional<RecognitionContext>& recognition,
                    bool supports_recognition);

} // namespace fv::dictation
