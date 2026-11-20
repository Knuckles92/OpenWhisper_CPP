#include "dictation/text_preparer.h"

#include "dictation/dictionary_learning.h"
#include "text/dictionary.h"

namespace fv::dictation {

text::Snippet to_text(const Snippet& snippet) {
    return text::Snippet{snippet.id, snippet.trigger, snippet.text, snippet.formatted};
}

Snippet from_text(const text::Snippet& snippet) {
    return Snippet{snippet.id, snippet.trigger, snippet.text, snippet.formatted};
}

text::SnippetPlan to_text(const SnippetPlan& plan) {
    text::SnippetPlan converted;
    converted.text = plan.text;
    if (plan.whole) {
        converted.whole = to_text(*plan.whole);
    }
    for (const auto& [placeholder, snippet] : plan.placeholders) {
        converted.placeholders.emplace_back(placeholder, to_text(snippet));
    }
    return converted;
}

SnippetPlan from_text(const text::SnippetPlan& plan) {
    SnippetPlan converted;
    converted.text = plan.text;
    if (plan.whole) {
        converted.whole = from_text(*plan.whole);
    }
    for (const auto& [placeholder, snippet] : plan.placeholders) {
        converted.placeholders.emplace_back(placeholder, from_text(snippet));
    }
    return converted;
}

std::string DictionaryTextPreparer::apply_replacements(std::string_view text,
                                                       const Json& settings) const {
    return text::apply_replacements(text, text::load_dictionary(settings));
}

SnippetPlan DictionaryTextPreparer::plan_expansion(std::string_view text,
                                                   const Json& settings) const {
    return from_text(text::plan_expansion(text, text::load_snippets(settings)));
}

Expansion DictionaryTextPreparer::expand(std::string_view text, const SnippetPlan& plan) const {
    const text::Expansion expanded = text::expand(text, to_text(plan));
    return Expansion{expanded.plain, expanded.html, expanded.ok};
}

void DictionaryTextPreparer::schedule_learning(
    const DictationJob& job, std::string_view pasted_text,
    std::optional<std::string_view> before_cleanup) const {
    if (learner_ != nullptr) {
        learner_->schedule_learning(job, pasted_text, before_cleanup);
    }
}

const TextPreparer& dictionary_preparer() {
    static const DictionaryTextPreparer preparer;
    return preparer;
}

} // namespace fv::dictation
