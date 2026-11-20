#pragma once

#include <optional>

#include "hotkeys/grammar.h"

namespace fv::hotkeys::testing {

// Makes platform_vocabulary() answer `vocabulary` (nullopt: the build's own again). Tests of
// wording that differs by backend name the one they test rather than depend on the machine.
// Affinity: thread-safe; process-wide.
void force_vocabulary(std::optional<Vocabulary> vocabulary);

// force_vocabulary for one scope.
class ScopedVocabulary {
public:
    explicit ScopedVocabulary(Vocabulary vocabulary) { force_vocabulary(vocabulary); }
    ~ScopedVocabulary() { force_vocabulary(std::nullopt); }
    ScopedVocabulary(const ScopedVocabulary&) = delete;
    ScopedVocabulary& operator=(const ScopedVocabulary&) = delete;
};

} // namespace fv::hotkeys::testing
