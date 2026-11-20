#include "dictation/pipeline.h"

#include <algorithm>
#include <stdexcept>

#include "base/log.h"
#include "focus/catalog.h"
#include "settings/keys.h"
#include "settings/resolve.h"
#include "text/cleanup_prompts.h"

namespace fv::dictation {

namespace {

constexpr log::Logger kLog{"services.dictation_pipeline"};

const Json& or_empty(const Json& settings) {
    static const Json kEmpty = Json::object();
    return settings.is_object() ? settings : kEmpty;
}

// _expand: nullopt when expansion failed.
std::optional<Expansion> expand(const TextPreparer& preparer, std::string_view text,
                                const SnippetPlan& plan) {
    try {
        return preparer.expand(text, plan);
    } catch (...) {
        kLog.warning("Snippet expansion failed");
        return std::nullopt;
    }
}

std::optional<focus::TextContext> caret_context(const DictationJob* job) {
    if (job == nullptr || job->mode != JobMode::Dictation) {
        return std::nullopt;
    }
    const auto snapshot = job->snapshot();
    if (!snapshot || !snapshot->text || !snapshot->text->caret_known) {
        return std::nullopt;
    }
    return snapshot->text;
}

void set_unless_empty(Json& fields, std::string_view key, std::string_view value) {
    if (!value.empty()) {
        fields[std::string(key)] = std::string(value);
    }
}

} // namespace

std::string TextPreparer::apply_replacements(std::string_view text, const Json&) const {
    return std::string(text);
}

SnippetPlan TextPreparer::plan_expansion(std::string_view text, const Json&) const {
    return SnippetPlan{std::string(text), std::nullopt, {}};
}

Expansion TextPreparer::expand(std::string_view text, const SnippetPlan& plan) const {
    if (plan.whole || !plan.placeholders.empty()) {
        throw std::logic_error("Snippets are not available");
    }
    return Expansion{std::string(text), std::string(), true};
}

void TextPreparer::schedule_learning(const DictationJob&, std::string_view,
                                     std::optional<std::string_view>) const {}

const TextPreparer& pass_through_preparer() {
    static const TextPreparer preparer;
    return preparer;
}

std::string apply_dictionary(std::string_view raw, const Json& settings,
                             const TextPreparer& preparer) {
    try {
        return preparer.apply_replacements(raw, or_empty(settings));
    } catch (...) {
        kLog.warning("Dictionary replacements failed; keeping the words as heard");
        return std::string(raw);
    }
}

PreparedText prepare_text(std::string_view raw, const DictationJob* job, const Json& settings,
                          const TextPreparer& preparer, bool replace_dictionary_terms) {
    const Json& values = or_empty(settings);
    const std::string original(raw);
    std::string text =
        replace_dictionary_terms ? apply_dictionary(raw, values, preparer) : original;
    SnippetPlan plan{text, std::nullopt, {}};
    if (job != nullptr &&
        (job->mode == JobMode::Dictation || job->mode == JobMode::Action ||
         job->mode == JobMode::Walkthrough) &&
        keys::snippets_enabled.read(values)) {
        try {
            plan = preparer.plan_expansion(text, values);
        } catch (...) {
            kLog.warning("Snippet planning failed; no snippets expanded");
            plan = SnippetPlan{text, std::nullopt, {}};
        }
    }
    PreparedText prepared;
    prepared.original = original;
    prepared.text = text;
    prepared.cleanup_input = plan.text;
    prepared.skip_cleanup = plan.whole.has_value();
    prepared.plan = std::move(plan);
    return prepared;
}

FinishedText finish_text(std::string_view cleaned, const PreparedText& prepared,
                         const TextPreparer& preparer) {
    auto raw = expand(preparer, prepared.cleanup_input, prepared.plan);
    if (!raw || !raw->ok) {
        // The plan cannot expand even its own text: paste the words as heard.
        raw = Expansion{prepared.text, std::string(), true};
    }
    if (cleaned == prepared.cleanup_input) {
        return FinishedText{raw->plain, raw->html, true, raw->plain};
    }
    const auto result = expand(preparer, cleaned, prepared.plan);
    if (!result || !result->ok) {
        return FinishedText{raw->plain, raw->html, false, raw->plain};
    }
    return FinishedText{result->plain, result->html, true, raw->plain};
}

std::string text_for_paste(std::string_view text, const DictationJob* job, const TextJoin& join) {
    const auto context = caret_context(job);
    if (!context) {
        return std::string(text);
    }
    try {
        return join(text, context);
    } catch (...) {
        kLog.warning("Joining with the text before the caret failed");
        return std::string(text);
    }
}

std::string html_for_paste(std::string_view html, std::string_view text, const DictationJob* job,
                           const HtmlJoin& join) {
    const auto context = html.empty() ? std::nullopt : caret_context(job);
    if (!context) {
        return std::string(html);
    }
    try {
        return join(html, text, context);
    } catch (...) {
        kLog.warning("Joining rich text with the text before the caret failed");
        return std::string(html);
    }
}

std::string_view to_string(PasteTarget target) {
    switch (target) {
    case PasteTarget::Same:
        return "same";
    case PasteTarget::Changed:
        return "changed";
    case PasteTarget::Unknown:
        break;
    }
    return "unknown";
}

PasteTarget paste_target(const DictationJob* job, focus::CaptureService* service) {
    std::shared_ptr<focus::FocusRequest> request;
    if (job != nullptr) {
        request = job->target ? job->target : job->focus;
    }
    const auto value = resolved(request, std::chrono::milliseconds::zero());
    const auto expected = value ? value->identity : std::nullopt;
    if (!expected) {
        return PasteTarget::Unknown;
    }
    std::optional<focus::AppIdentity> current;
    try {
        std::shared_ptr<focus::CaptureService> installed;
        if (service == nullptr) {
            installed = focus::get_service();
            service = installed.get();
        }
        current = service->identity_now(PASTE_CHECK_TIMEOUT);
    } catch (...) {
        kLog.warning("Current focus unavailable");
        return PasteTarget::Unknown;
    }
    if (!current) {
        return PasteTarget::Unknown;
    }
    return focus::same_target(current, expected) ? PasteTarget::Same : PasteTarget::Changed;
}

bool paste_target_ok(const DictationJob* job, focus::CaptureService* service) {
    return paste_target(job, service) == PasteTarget::Same;
}

std::string cleanup_level(const Json& settings, bool has_profile, const CustomPrompt& custom) {
    if (has_profile) {
        return "profile";
    }
    const Json& values = or_empty(settings);
    std::string level;
    try {
        if (!custom(values).empty()) {
            return "custom";
        }
        // "none" while AI cleanup is off.
        level = std::string(text::to_string(text::resolve_level(values)));
    } catch (...) {
        kLog.debug("Cleanup level unavailable");
        return {};
    }
    return level == "none" ? std::string() : level;
}

void after_paste(const DictationJob* job, std::string_view pasted_text,
                 std::optional<std::string_view> before_cleanup, const TextPreparer& preparer) {
    if (job == nullptr || job->mode != JobMode::Dictation) {
        return;
    }
    try {
        preparer.schedule_learning(*job, pasted_text, before_cleanup);
    } catch (...) {
        kLog.warning("Dictionary learning could not be scheduled");
    }
}

std::string category_for(const std::optional<focus::FocusSnapshot>& snapshot,
                         const Json& settings) {
    const auto& identity = snapshot ? snapshot->identity : std::nullopt;
    if (!identity || identity->is_self) {
        return {};
    }
    const auto overrides =
        focus::catalog::parse_overrides(keys::app_style_overrides.read(or_empty(settings)));
    const auto kind = focus::catalog::classify(identity, overrides);
    return kind ? kind->category : std::string();
}

Json history_fields(const DictationJob* job, std::optional<std::string_view> info_level, bool live,
                    const Json& settings) {
    std::string_view kind;
    if (job != nullptr && job->mode != JobMode::Dictation) {
        kind = to_string(job->mode);
    } else {
        kind = live ? to_string(JobMode::Dictation) : FILE_ENTRY_KIND;
    }
    Json fields = Json::object();
    fields["entry_kind"] = std::string(kind);
    const auto snapshot = job != nullptr ? job->snapshot() : std::nullopt;
    if (snapshot && snapshot->identity) {
        set_unless_empty(fields, "app_id", snapshot->identity->app_id);
        set_unless_empty(fields, "app_name", snapshot->identity->name);
        try {
            // Not style_for: stats keep the category with styles turned off.
            set_unless_empty(fields, "app_category", category_for(snapshot, settings));
        } catch (...) {
            kLog.warning("App category unavailable");
        }
    }
    set_unless_empty(fields, "cleanup_level", info_level.value_or(std::string_view()));
    if (job != nullptr && job->recognition) {
        set_unless_empty(fields, "language", job->recognition->language);
    }
    return fields;
}

Json stats_counts(const PreparedText& prepared, const WordCounter& words) {
    try {
        const SnippetPlan& plan = prepared.plan;
        std::vector<const Snippet*> used;
        if (plan.whole) {
            used.push_back(&*plan.whole);
        } else {
            for (const auto& [placeholder, snippet] : plan.placeholders) {
                used.push_back(&snippet);
            }
        }
        std::size_t snippet_words = 0;
        for (const Snippet* snippet : used) {
            const std::size_t typed = words.count_words(snippet->text);
            const std::size_t said = words.count_words(snippet->trigger);
            snippet_words += typed > said ? typed - said : 0;
        }
        const std::size_t corrections = words.words_edited(prepared.original, prepared.text);
        Json counts = Json::object();
        if (!used.empty()) {
            counts["snippets"] = used.size();
        }
        if (snippet_words != 0) {
            counts["snippet_words"] = snippet_words;
        }
        if (corrections != 0) {
            counts["corrections"] = corrections;
        }
        return counts;
    } catch (...) {
        kLog.warning("Could not count snippets and corrections");
        return Json::object();
    }
}

namespace detail {

void log_record_stats_failure() {
    kLog.warning("Could not record dictation stats");
}

} // namespace detail

} // namespace fv::dictation
