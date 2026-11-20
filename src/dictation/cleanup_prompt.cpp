#include "dictation/cleanup_prompt.h"

#include <utility>

#include "base/log.h"
#include "dictation/batch.h"
#include "dictation/text_preparer.h"
#include "focus/prompt_block.h"
#include "llm/cleanup_settings.h"
#include "settings/keys.h"
#include "text/cleanup_prompts.h"
#include "text/dictionary.h"
#include "text/snippets.h"

namespace fv::dictation {

namespace {

constexpr log::Logger kLog{"services.dictation_pipeline"};

const Json& or_empty(const Json& settings) {
    static const Json kEmpty = Json::object();
    return settings.is_object() ? settings : kEmpty;
}

// _block: what `build` makes, or "" when it throws.
template <class Build> std::string block(std::string_view name, Build&& build) {
    try {
        return std::forward<Build>(build)();
    } catch (...) {
        kLog.warning("Cleanup prompt block {} failed; left out", name);
        return {};
    }
}

// _base_cleanup_prompt.
std::string base_cleanup_prompt(const Json& settings, const text::CleanupProfile* profile,
                                const std::vector<std::string>& rules) {
    if (profile != nullptr) {
        return text::compose_profile_prompt(*profile, rules);
    }
    std::string base;
    try {
        base = text::base_prompt(settings, text::resolve_level(settings));
    } catch (...) {
        kLog.warning("Cleanup preset unavailable; using the saved prompt");
        base = text::resolve_transcript_cleanup_prompt(settings);
    }
    return text::compose_transcript_cleanup_prompt(base, rules);
}

std::optional<focus::FocusSnapshot> job_snapshot(const DictationJob& job) {
    return job.snapshot(CONTEXT_CAPTURE_DEADLINE);
}

} // namespace

std::string compose_cleanup_prompt(const Json& settings, const DictationJob* job,
                                   const text::CleanupProfile* profile,
                                   const std::vector<std::string>& rules,
                                   const PreparedText& prepared,
                                   std::optional<std::string_view> batch_context,
                                   const CleanupPromptBlocks& blocks) {
    const Json& values = or_empty(settings);
    const bool agent =
        llm::is_installed_cleanup_agent(llm::resolve_transcript_cleanup_provider(values));
    std::string prompt = agent ? base_cleanup_prompt(values, profile, {})
                               : base_cleanup_prompt(values, profile, rules);
    if (agent) {
        // Expand snippets after cleanup. The CLI needs only the placeholder spellings.
        const auto guard = text::prompt_guard(to_text(prepared.plan));
        if (!guard.empty()) {
            prompt += "\n\n" + guard;
        }
        return prompt;
    }

    const bool use_style = profile == nullptr && keys::app_styles_enabled.read(values);
    const bool use_caret_text = keys::app_context_read_text.read(values);
    std::optional<focus::FocusSnapshot> snapshot;
    if (job != nullptr && (use_style || use_caret_text)) {
        snapshot = job_snapshot(*job);
    }

    std::vector<std::string> parts;
    std::optional<text::app_styles::AppStyle> style;
    if (use_style) {
        // _app_style: an unavailable style is no style.
        try {
            style = blocks.style_for ? blocks.style_for(snapshot, values)
                                     : text::app_styles::style_for(snapshot, values);
        } catch (...) {
            kLog.warning("App style unavailable");
            style.reset();
        }
        parts.push_back(block("style", [&] {
            return blocks.style_block ? blocks.style_block(style)
                                      : text::app_styles::prompt_block(style);
        }));
    }
    // The style block already tells a terminal to stay on one line.
    const bool terminal_covered = style && style->surface == text::app_styles::SURFACE_TERMINAL;
    if (profile == nullptr && job != nullptr && job->mode == JobMode::Dictation &&
        !terminal_covered) {
        parts.push_back(block("lists", [&] {
            const auto captured = job_snapshot(*job);
            return blocks.lists_block ? blocks.lists_block(values, captured)
                                      : text::inline_lists_block(values, captured);
        }));
    }
    parts.push_back(block("dictionary", [&] {
        return blocks.dictionary_block ? blocks.dictionary_block(values)
                                       : text::prompt_block(text::load_dictionary(values));
    }));
    parts.push_back(block("snippets", [&] {
        return blocks.snippet_guard ? blocks.snippet_guard(prepared.plan)
                                    : text::prompt_guard(to_text(prepared.plan));
    }));
    if (use_caret_text) {
        parts.push_back(block("context", [&] {
            return blocks.context_block ? blocks.context_block(snapshot)
                                        : focus::prompt_block(snapshot);
        }));
    }
    for (const std::string& part : parts) {
        if (!part.empty()) {
            prompt += "\n\n";
            prompt += part;
        }
    }
    if (batch_context && !batch_context->empty()) {
        prompt = compose_batch_cleanup_prompt(prompt, *batch_context);
    }
    return prompt;
}

} // namespace fv::dictation
