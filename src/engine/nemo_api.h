#pragma once

#include <filesystem>

#include "engine/shared_library.h"
#include "nemo_speech/asr.h"

namespace fv {

// The nemo_speech_asr_* functions FioriVoce calls, without the prefix. Adding
// one here binds it at load; a runtime that lacks it then fails to load.
#define FV_NEMO_ASR_FUNCTIONS(X)                                                                   \
    X(version)                                                                                     \
    X(last_error)                                                                                  \
    X(create)                                                                                      \
    X(destroy)                                                                                     \
    X(recognition_options_default)                                                                 \
    X(recognize_f32)                                                                               \
    X(result_audio_processed)                                                                      \
    X(result_transcript)                                                                           \
    X(result_word_count)                                                                           \
    X(result_word_text)                                                                            \
    X(result_word_start_time)                                                                      \
    X(result_word_end_time)                                                                        \
    X(result_destroy)

// The NeMo-Speech.cpp ASR C ABI, bound at run time. The runtime is chosen by
// the user (CPU or a GPU build) and may not be installed at all, so it is
// loaded by path instead of linked. Each pointer has the exact type the
// vendored header declares.
struct NemoApi {
    SharedLibrary library;

#define FV_DECLARE(name) decltype(&::nemo_speech_asr_##name) name = nullptr;
    FV_NEMO_ASR_FUNCTIONS(FV_DECLARE)
#undef FV_DECLARE

    // The API of the runtime library at `library_path`, loading and binding it
    // on first use. Throws EngineError when the library cannot load or lacks a
    // function.
    //
    // A loaded runtime stays loaded until the process exits, and the reference
    // stays valid that long. Unloading is not safe: with NeMo-Speech.cpp 0.1.0
    // on Windows, FreeLibrary on the runtime crashes with an access violation
    // even after every recognizer is destroyed.
    static const NemoApi& get(const std::filesystem::path& library_path);
};

} // namespace fv
