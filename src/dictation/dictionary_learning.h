#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "base/executor.h"
#include "base/signal.h"
#include "dictation/job.h"
#include "focus/capture_service.h"

namespace fv {
class SettingsStore;
}

// Learning dictionary words from the corrections a user makes after a paste
// (services/dictionary.py set_learned_callback ... _learn). The word diff itself is fv_text's
// (text/dictionary_learning.h). Words are user content: only the fact that one was learned is
// logged.
namespace fv::dictation {

// text::LEARN_DELAY_S: how long after a paste the field is read again.
inline constexpr std::chrono::milliseconds LEARN_DELAY{4000};

// A pasted word the user corrects the same way twice (text::LEARN_SIGHTINGS) becomes a Learned
// dictionary word; a correction that puts back a word AI cleanup had changed is learned at once
// and protected from cleanup. The sightings are kept in memory only.
//
// The delay runs on `ui`; the field is read again through the capture service (its callback runs
// on the service's thread, which never writes a file); the word is saved on `persistence`, which
// also emits term_learned. Settings, the executors and the capture service must outlive the
// learner; destroy it only after `persistence` stopped running its tasks.
// Affinity: thread-safe; schedule_learning on the UI thread (it never blocks).
class DictionaryLearner {
public:
    // `capture` is focus::get_service() at each reread when null.
    DictionaryLearner(SettingsStore& settings, Executor& ui, Executor& persistence,
                      focus::CaptureService* capture = nullptr);
    ~DictionaryLearner();
    DictionaryLearner(const DictionaryLearner&) = delete;
    DictionaryLearner& operator=(const DictionaryLearner&) = delete;

    // dictionary.schedule_learning: looks for a correction of `pasted_text` LEARN_DELAY later, for
    // a job whose start captured the caret text of another app. `before_cleanup` is the
    // transcript before AI cleanup changed it, when it did.
    void schedule_learning(const DictationJob& job, std::string_view pasted_text,
                           std::optional<std::string_view> before_cleanup = std::nullopt);

    // A word was saved: (the saved spelling, protected from cleanup). Emitted on `persistence`.
    Signal<std::string, bool> term_learned;

private:
    struct Shared;
    void reread(const focus::AppIdentity& identity, const std::string& pasted,
                const std::optional<std::string>& before_cleanup);

    SettingsStore& settings_;
    Executor& ui_;
    focus::CaptureService* capture_;
    std::shared_ptr<Shared> shared_;
};

} // namespace fv::dictation
