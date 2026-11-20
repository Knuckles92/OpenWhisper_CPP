#include "dictation/dictionary_learning.h"

#include <exception>
#include <utility>

#include "base/log.h"
#include "pyport/text.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "text/dictionary.h"
#include "text/dictionary_learning.h"

namespace fv::dictation {

namespace {
constexpr log::Logger kLog{"services.dictionary"};
} // namespace

// What the delayed steps share with the learner; they hold it weakly and do nothing once the
// learner is gone.
struct DictionaryLearner::Shared {
    Shared(DictionaryLearner& learner, SettingsStore& settings, Executor& persistence)
        : owner(&learner), settings(settings), persistence(persistence) {}

    // Held while a step runs, so the learner's destructor waits for it.
    std::recursive_mutex owner_mutex;
    DictionaryLearner* owner;
    SettingsStore& settings;
    Executor& persistence;

    std::mutex sightings_mutex;
    // Casefolded word -> sightings so far, below text::LEARN_SIGHTINGS.
    std::map<std::string, int> sightings;

    // _on_reread: the capture service's callback; keeps that thread free of file writes.
    static void on_reread(const std::weak_ptr<Shared>& weak,
                          const std::optional<focus::TextContext>& context,
                          const std::string& pasted,
                          const std::optional<std::string>& before_cleanup) {
        try {
            const auto shared = weak.lock();
            if (!shared || !context || !context->caret_known) {
                return;
            }
            const std::string field = context->before + context->selected + context->after;
            const auto found = text::learned_correction(pasted, field);
            if (!found) {
                return;
            }
            const auto& [replaced, word] = *found;
            const std::optional<std::string_view> spoken =
                before_cleanup ? std::optional<std::string_view>(*before_cleanup) : std::nullopt;
            if (text::reverted_cleanup(spoken, replaced, word)) {
                // Undoing an AI edit is deliberate; one sighting is enough.
                shared->learn_later(word, true);
                return;
            }
            const std::string key = py::casefold(word);
            {
                std::lock_guard lock(shared->sightings_mutex);
                const int count = shared->sightings[key] + 1;
                if (count < text::LEARN_SIGHTINGS) {
                    shared->sightings[key] = count;
                    return;
                }
                shared->sightings.erase(key);
            }
            shared->learn_later(word, false);
        } catch (...) {
            kLog.warning("Dictionary learning failed");
        }
    }

    void learn_later(std::string word, bool is_protected) {
        std::weak_ptr<Shared> weak = self;
        persistence.post([weak, word = std::move(word), is_protected] {
            if (const auto shared = weak.lock()) {
                shared->learn(word, is_protected);
            }
        });
    }

    // _learn: the settings write, on the persistence strand.
    void learn(const std::string& word, bool is_protected) {
        std::lock_guard owner_lock(owner_mutex);
        if (owner == nullptr) {
            return;
        }
        std::optional<text::DictionaryTerm> learned;
        try {
            settings.mutate(
                [&](Json& values) { learned = text::add_learned(values, word, is_protected); });
        } catch (...) {
            kLog.warning("Could not save a learned dictionary word");
            return;
        }
        if (!learned) {
            return;
        }
        kLog.info("Dictionary learned a word from a correction");
        try {
            owner->term_learned.emit(learned->term, learned->is_protected);
        } catch (...) {
            kLog.warning("Learned-word listener raised");
        }
    }

    std::weak_ptr<Shared> self;
};

DictionaryLearner::DictionaryLearner(SettingsStore& settings, Executor& ui, Executor& persistence,
                                     focus::CaptureService* capture)
    : settings_(settings), ui_(ui), capture_(capture),
      shared_(std::make_shared<Shared>(*this, settings, persistence)) {
    shared_->self = shared_;
}

DictionaryLearner::~DictionaryLearner() {
    std::lock_guard lock(shared_->owner_mutex);
    shared_->owner = nullptr;
}

void DictionaryLearner::schedule_learning(const DictationJob& job, std::string_view pasted_text,
                                          std::optional<std::string_view> before_cleanup) {
    if (pasted_text.empty() || py::strip(pasted_text).empty()) {
        return;
    }
    const auto snapshot = job.snapshot();
    if (!snapshot || !snapshot->identity || snapshot->identity->is_self || !snapshot->text ||
        !snapshot->text->caret_known) {
        return;
    }
    std::weak_ptr<Shared> weak = shared_;
    std::optional<std::string> before;
    if (before_cleanup) {
        before = std::string(*before_cleanup);
    }
    // Fire and forget, as threading.Timer: the learner's destruction stops the step through
    // `weak`, and the executor's own shutdown cancels the timer.
    ui_.post_after(LEARN_DELAY,
                   [this, weak, identity = *snapshot->identity, pasted = std::string(pasted_text),
                    before = std::move(before)] {
                       const auto shared = weak.lock();
                       if (!shared) {
                           return;
                       }
                       std::lock_guard lock(shared->owner_mutex);
                       if (shared->owner != nullptr) {
                           reread(identity, pasted, before);
                       }
                   })
        .detach();
}

void DictionaryLearner::reread(const focus::AppIdentity& identity, const std::string& pasted,
                               const std::optional<std::string>& before_cleanup) {
    try {
        const auto settings = settings_.snapshot();
        if (!keys::dictionary_learn_enabled.read(*settings) ||
            !keys::app_context_read_text.read(*settings)) {
            return;
        }
        std::shared_ptr<focus::CaptureService> installed;
        focus::CaptureService* service = capture_;
        if (service == nullptr) {
            installed = focus::get_service();
            service = installed.get();
        }
        std::weak_ptr<Shared> weak = shared_;
        service->reread(identity,
                        [weak, pasted, before_cleanup](std::optional<focus::TextContext> context) {
                            Shared::on_reread(weak, context, pasted, before_cleanup);
                        });
    } catch (...) {
        kLog.warning("Dictionary learning could not read the field again");
    }
}

} // namespace fv::dictation
