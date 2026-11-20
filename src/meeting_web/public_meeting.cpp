#include "meeting_web/public_meeting.h"

#include <algorithm>
#include <string>
#include <tuple>
#include <vector>

#include "base/log.h"
#include "meeting_state/content.h"
#include "meeting_state/schema.h"
#include "meeting_state/stored.h"
#include "meeting_state/time_utils.h"
#include "meeting_web/detail/py_value.h"
#include "pyport/errors.h"

namespace fv::meeting_web {

namespace {

constexpr log::Logger logger("meeting.web.api");

using detail::get;
using detail::py_or;
using detail::py_str;
using detail::truthy;

bool is_text(const Json& value, std::string_view text) {
    return value.is_string() && value.get_ref<const std::string&>() == text;
}

std::int64_t live_count(const Json& cards, std::string_view key) {
    const Json& items = get(cards, key);
    if (!items.is_array()) {
        return 0;
    }
    return std::count_if(items.begin(), items.end(), [](const Json& item) {
        return item.is_object() && !is_text(get(item, "status"), "removed");
    });
}

Json build_digest(const Json& state) {
    static const Json no_cards = Json::object();
    const Json& cards = get(state, "cards").is_object() ? get(state, "cards") : no_cards;

    std::vector<Json> questions;
    const Json& raw_questions = get(state, "questions");
    if (raw_questions.is_object() || raw_questions.is_array()) {
        for (const auto& question : raw_questions) {
            questions.push_back(question);
        }
    }
    const auto open_questions =
        std::count_if(questions.begin(), questions.end(), [](const Json& question) {
            return question.is_object() && is_text(get(question, "status"), "open");
        });

    std::vector<const Json*> people;
    const Json& raw_people = get(state, "participants");
    if (raw_people.is_object()) {
        for (const auto& person : raw_people) {
            if (person.is_object() && truthy(get(person, "id"))) {
                people.push_back(&person);
            }
        }
    }
    // "Me" first, then join order, so avatar colours match the live view.
    auto key = [](const Json* person) {
        return std::make_tuple(!is_text(get(*person, "kind"), "me"),
                               py_str(py_or(get(*person, "created_at"), Json(""))),
                               py_str(get(*person, "id")));
    };
    std::stable_sort(people.begin(), people.end(),
                     [&](const Json* left, const Json* right) { return key(left) < key(right); });

    Json shown = Json::array();
    for (std::size_t i = 0; i < people.size() && i < kDigestParticipants; ++i) {
        const Json& person = *people[i];
        shown.push_back(Json{
            {"id", py_str(get(person, "id"))},
            {"display_name", py_str(py_or(get(person, "display_name"), Json("")))},
            {"kind", py_str(py_or(get(person, "kind"), Json("")))},
            {"created_at", py_str(py_or(get(person, "created_at"), Json("")))},
        });
    }
    return Json{
        {"decisions", live_count(cards, "decisions")},
        {"action_items", live_count(cards, "action_items")},
        {"risks", live_count(cards, "risks")},
        {"open_questions", open_questions},
        {"participant_count", people.size()},
        {"participants", std::move(shown)},
    };
}

} // namespace

std::optional<Json> meeting_digest(const Json& meeting) {
    const auto state = meeting_state::parse_state_json(get(meeting, "state_json"));
    if (!state) {
        return std::nullopt;
    }
    try {
        return std::optional<Json>(build_digest(*state));
    } catch (const std::exception&) {
        logger.debug("Could not build History digest for {}", py_str(get(meeting, "id")));
        return std::nullopt;
    }
}

Json public_meeting(const Json& meeting, meeting_state::MeetingRepository* repository,
                    const Clock& clock) {
    if (!truthy(meeting)) {
        return Json::object();
    }
    Json out = Json::object();
    for (const auto key : kPublicMeetingKeys) {
        out[std::string(key)] = get(meeting, key);
    }
    out["display_title"] = meeting_state::meeting_display_title(meeting);
    const std::string status = py_str(py_or(get(meeting, "status"), Json("")));
    const bool running = std::find(kRunningStatuses.begin(), kRunningStatuses.end(), status) !=
                         kRunningStatuses.end();
    const auto duration = meeting_state::meeting_duration_s(meeting, running, clock);
    out["duration_s"] = duration ? Json(*duration) : Json();
    if (auto digest = meeting_digest(meeting)) {
        out["digest"] = std::move(*digest);
    }
    const auto finalization = meeting_state::compact_finalization_list_fields(meeting);
    for (const auto& [key, value] : finalization.items()) {
        out[key] = value;
    }
    Json summary = get(meeting, "content_summary");
    if (summary.is_object() || repository != nullptr) {
        if (!summary.is_object()) {
            summary = meeting_state::summarize_meeting_content(
                *repository, py_str(py_or(get(meeting, "id"), Json(""))));
        }
        out["content_summary"] = summary;
        for (const char* flag : {"has_audio", "has_transcript", "can_rerun_speakers"}) {
            if (!summary.contains(flag)) {
                throw py::PyError("KeyError", std::string("'") + flag + "'");
            }
            out[flag] = summary.at(flag);
        }
    }
    return out;
}

} // namespace fv::meeting_web
