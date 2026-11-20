#include "meeting_intel/intel_settings.h"

#include <stdexcept>

#include "meeting_intel/detail/paths.h"
#include "pyport/text.h"
#include "settings/keys.h"

namespace fv::meeting_intel {

bool resolve_typesafe_enabled(const Json& settings) {
    return keys::typesafe_enabled.read(settings);
}

bool resolve_typesafe_feature_enabled(const Json& settings, std::string_view name) {
    // Python looks the name up only once the master switch is on.
    if (!resolve_typesafe_enabled(settings)) {
        return false;
    }
    const BoolSetting* key = nullptr;
    if (name == kFeatureCitations) {
        key = &keys::typesafe_citations_enabled;
    } else if (name == kFeatureSemanticSearch) {
        key = &keys::typesafe_semantic_search_enabled;
    } else if (name == kFeatureQuestionRadar) {
        key = &keys::typesafe_question_radar_enabled;
    } else if (name == kFeatureHighlights) {
        key = &keys::typesafe_highlights_enabled;
    } else {
        throw std::out_of_range("unknown Fast judgments feature '" + std::string(name) + "'");
    }
    return key->read(settings);
}

bool resolve_typesafe_topic_shift_enabled(const Json& settings) {
    return resolve_typesafe_enabled(settings) && keys::typesafe_topic_shift_enabled.read(settings);
}

bool resolve_typesafe_voice_commands_enabled(const Json& settings) {
    return resolve_typesafe_enabled(settings) &&
           keys::typesafe_voice_commands_enabled.read(settings);
}

std::vector<std::string> default_voice_command_names(const AppIdentity& defaults_from) {
    return {"note taker", "notetaker", "assistant", std::string(defaults_from.wake_name),
            std::string(defaults_from.wake_name_spaced)};
}

std::vector<std::string> resolve_typesafe_voice_command_names(const Json& settings,
                                                              const AppIdentity& defaults_from) {
    const Json raw = keys::typesafe_voice_command_names.read(settings);
    if (raw.is_null()) {
        return default_voice_command_names(defaults_from);
    }
    std::vector<std::string> names;
    if (raw.is_array()) {
        for (const auto& item : raw) {
            if (!item.is_string()) {
                continue;
            }
            const std::string stripped = py::strip(item.get_ref<const std::string&>());
            if (stripped.empty()) {
                continue;
            }
            std::string joined;
            for (const auto& word : py::split(py::lower(stripped))) {
                if (!joined.empty()) {
                    joined += ' ';
                }
                joined += word;
            }
            names.push_back(std::move(joined));
        }
    }
    return names.empty() ? default_voice_command_names(defaults_from) : names;
}

bool resolve_meeting_past_recall_enabled(const Json& settings) {
    return keys::meeting_past_recall_enabled.read(settings);
}

bool resolve_meeting_context_folder_enabled(const Json& settings) {
    return keys::meeting_context_folder_enabled.read(settings);
}

std::string resolve_meeting_context_folder_path(const Json& settings, const EnvReader& env) {
    const std::string cleaned = py::strip(keys::meeting_context_folder_path.read(settings));
    if (cleaned.empty()) {
        return "";
    }
    std::string expanded = detail::expanduser(cleaned, env);
    if (!detail::isabs(expanded)) {
        expanded = detail::abspath(expanded);
    }
    return detail::normpath(expanded);
}

Json resolve_meeting_insight_review(const Json& settings) {
    const bool enabled = keys::meeting_insight_review.read(settings);
    const bool consent =
        keys::meeting_insight_review_consent.read(settings) == kInsightReviewConsent;
    Json review = Json::object();
    review["enabled"] = enabled && consent && resolve_typesafe_enabled(settings);
    review["consent"] = consent ? std::string(kInsightReviewConsent) : std::string();
    review["sensitivity"] = keys::meeting_insight_review_sensitivity.read(settings);
    return review;
}

} // namespace fv::meeting_intel
