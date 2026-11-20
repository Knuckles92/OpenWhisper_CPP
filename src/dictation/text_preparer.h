#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "base/json.h"
#include "dictation/pipeline.h"
#include "text/snippets.h"

// The dictionary and snippets behind the pipeline's text stages (services/dictation_pipeline.py
// prepare_text, _expand and after_paste over services/dictionary.py and services/snippets.py).
namespace fv::dictation {

class DictionaryLearner;

// fv_text keeps its own snippet types, since it cannot include fv_dictation; these convert field
// by field.
text::Snippet to_text(const Snippet& snippet);
Snippet from_text(const text::Snippet& snippet);
text::SnippetPlan to_text(const SnippetPlan& plan);
SnippetPlan from_text(const text::SnippetPlan& plan);

// The real TextPreparer: the user's dictionary replaces "sounds like" variants, saved snippets
// are planned and expanded, and a pasted dictation is handed to `learner` (none: no learning).
// Affinity: thread-safe (stateless; the learner is thread-safe).
class DictionaryTextPreparer final : public TextPreparer {
public:
    explicit DictionaryTextPreparer(DictionaryLearner* learner = nullptr) : learner_(learner) {}

    std::string apply_replacements(std::string_view text, const Json& settings) const override;
    SnippetPlan plan_expansion(std::string_view text, const Json& settings) const override;
    Expansion expand(std::string_view text, const SnippetPlan& plan) const override;
    void schedule_learning(const DictationJob& job, std::string_view pasted_text,
                           std::optional<std::string_view> before_cleanup) const override;

private:
    DictionaryLearner* learner_;
};

// The preparer a runtime uses until it is given one: the dictionary and snippets, no learning.
const TextPreparer& dictionary_preparer();

} // namespace fv::dictation
