// Golden vectors of meeting.time_utils, meeting.state.schema, meeting.state.patches and
// meeting.corrections (tools/parity/cases/<module>.py).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "base/testing/fake_clock.h"
#include "golden/golden.h"
#include "meeting_state/corrections.h"
#include "meeting_state/detail/patches_internal.h"
#include "meeting_state/patches.h"
#include "meeting_state/schema.h"
#include "meeting_state/testing/fixed_ids.h"
#include "meeting_state/time_utils.h"
#include "pyport/datetime.h"
#include "pyport/errors.h"

namespace golden = fv::golden;
using fv::Json;
using namespace fv::meeting_state;

namespace {

// The Python exception's class name, or "" when nothing pyport-shaped was thrown.
template <class Fn> std::string raised(Fn&& fn) {
    try {
        fn();
    } catch (const fv::py::PyError& error) {
        return std::string(error.type_name());
    }
    return "";
}

// Checks a case: the exception Python raised, or the result as JSON.
template <class Fn> void replay(const golden::Case& c, Fn&& fn) {
    INFO(c.label());
    if (c.raises()) {
        CHECK(raised(fn) == c.raises_type());
        return;
    }
    const Json actual = fn();
    INFO("actual " << actual.dump());
    CHECK(golden::equal(c.result<Json>(), actual));
}

Json iso_or_null(const std::optional<fv::py::DateTime>& moment) {
    return moment ? Json(moment->isoformat()) : Json();
}

template <class T> Json optional_json(const std::optional<T>& value) {
    return value ? Json(*value) : Json();
}

// meeting.time_utils pins: now 2026-03-08T07:30:00.250000Z, the zone America/New_York.
const fv::FakeClock& new_york_clock() {
    static const fv::FakeClock clock(std::chrono::sys_days{std::chrono::year{2026} / 3 / 8} +
                                         std::chrono::hours{7} + std::chrono::minutes{30} +
                                         std::chrono::milliseconds{250},
                                     "America/New_York");
    return clock;
}

// The other modules pin now at FakeClock's default, 2026-01-01T00:00:00Z.
const fv::FakeClock& utc_clock() {
    static const fv::FakeClock clock;
    return clock;
}

// {t: r for t, r in pairs}: a repeated term keeps its first position and its last value.
TermRules rules_of(const Json& pairs) {
    TermRules rules;
    for (const auto& pair : pairs) {
        const std::string term = pair.at(0).get<std::string>();
        const std::string replacement = pair.at(1).get<std::string>();
        auto found = std::find_if(rules.begin(), rules.end(),
                                  [&](const auto& rule) { return rule.first == term; });
        if (found != rules.end()) {
            found->second = replacement;
        } else {
            rules.emplace_back(term, replacement);
        }
    }
    return rules;
}

Json pairs_json(const TermRules& rules) {
    Json out = Json::array();
    for (const auto& [term, replacement] : rules) {
        out.push_back(Json::array({term, replacement}));
    }
    return out;
}

ScopedRules scoped_of(const Json& scoped) {
    ScopedRules rules;
    for (const auto& [segment, items] : scoped.items()) {
        auto& list = rules[segment];
        for (const auto& item : items) {
            list.push_back({item.at("selected_text").get<std::string>(),
                            item.at("replacement").get<std::string>(),
                            item.at("occurrence_index").get<std::int64_t>(),
                            item.at("base_fingerprint").get<std::string>()});
        }
    }
    return rules;
}

std::function<bool(std::string_view)> predicate(const Json& ids, bool raises = false) {
    if (ids.is_null()) {
        return {};
    }
    std::set<std::string, std::less<>> known;
    for (const auto& id : ids) {
        known.insert(id.get<std::string>());
    }
    return [known, raises](std::string_view segment_id) {
        if (raises) {
            throw std::runtime_error("lookup failed");
        }
        return known.contains(segment_id);
    };
}

} // namespace

// meeting.time_utils

TEST_CASE("golden meeting.time_utils", "[golden]") {
    const auto& clock = new_york_clock();
    golden::for_each("meeting.time_utils", "utc_now_iso", [&](const golden::Case& c) {
        replay(c, [&] { return Json(utc_now_iso(clock)); });
    });
    golden::for_each("meeting.time_utils", "parse_meeting_time", [&](const golden::Case& c) {
        replay(c, [&] { return iso_or_null(parse_meeting_time(c.args()[0])); });
    });
    golden::for_each("meeting.time_utils", "as_local_time", [&](const golden::Case& c) {
        replay(c, [&] { return iso_or_null(as_local_time(c.args()[0], clock)); });
    });
    golden::for_each("meeting.time_utils", "as_local_time_at_offset", [&](const golden::Case& c) {
        replay(c, [&] {
            const std::chrono::seconds offset = std::chrono::hours{c.arg<int>(1)};
            return iso_or_null(as_local_time(c.args()[0], offset));
        });
    });
    golden::for_each("meeting.time_utils", "elapsed_seconds", [&](const golden::Case& c) {
        replay(c, [&] { return optional_json(elapsed_seconds(c.args()[0], c.args()[1], clock)); });
    });
    golden::for_each("meeting.time_utils", "seconds_since", [&](const golden::Case& c) {
        replay(c, [&] { return optional_json(seconds_since(c.args()[0], clock)); });
    });
    golden::for_each("meeting.time_utils", "format_meeting_started_at", [&](const golden::Case& c) {
        replay(c, [&] { return Json(format_meeting_started_at(c.args()[0], clock)); });
    });
    golden::for_each("meeting.time_utils", "meeting_duration_s", [&](const golden::Case& c) {
        replay(c, [&] {
            return optional_json(meeting_duration_s(c.args()[0], c.arg<bool>(1), clock));
        });
    });
    golden::for_each("meeting.time_utils", "format_meeting_duration", [&](const golden::Case& c) {
        replay(c, [&] { return Json(format_meeting_duration(c.args()[0], clock)); });
    });
    golden::for_each(
        "meeting.time_utils", "format_meeting_identity_meta", [&](const golden::Case& c) {
            replay(c, [&] { return Json(format_meeting_identity_meta(c.args()[0], clock)); });
        });
}

// meeting.state.schema

TEST_CASE("golden meeting.state.schema", "[golden]") {
    const auto& clock = utc_clock();
    golden::for_each("meeting.state.schema", "round_trip", [&](const golden::Case& c) {
        replay(c, [&] { return MeetingState::from_json(c.args()[0], clock).to_json(); });
    });
    golden::for_each("meeting.state.schema", "coerce", [&](const golden::Case& c) {
        replay(c, [&] {
            return FinalizationState::coerce(c.args()[0], c.arg<bool>(1), c.arg<std::string>(2))
                .to_json();
        });
    });
    golden::for_each("meeting.state.schema", "normalize_historical", [&](const golden::Case& c) {
        replay(c, [&] {
            return FinalizationState::normalize_historical(c.args()[0], c.arg<bool>(1),
                                                           c.arg<std::string>(2))
                .to_json();
        });
    });
    golden::for_each("meeting.state.schema", "history_pill", [&](const golden::Case& c) {
        replay(c, [&] {
            const auto pill =
                FinalizationState::coerce(c.args()[0]).history_pill(c.arg<std::string>(1));
            return pill ? Json::array({pill->first, pill->second}) : Json();
        });
    });
    golden::for_each("meeting.state.schema", "default_for_cloud", [&](const golden::Case& c) {
        replay(c, [&] { return FinalizationState::default_for_cloud(c.arg<bool>(0)).to_json(); });
    });
    golden::for_each("meeting.state.schema", "infer_legacy", [&](const golden::Case& c) {
        replay(c, [&] {
            return FinalizationState::infer_legacy(c.arg<bool>(0), c.arg<std::string>(1)).to_json();
        });
    });
    golden::for_each("meeting.state.schema", "parse_state_json", [&](const golden::Case& c) {
        replay(c, [&] { return parse_state_json(c.args()[0]).value_or(Json()); });
    });
    golden::for_each("meeting.state.schema", "coerce_card_deferred", [&](const golden::Case& c) {
        replay(c, [&] { return Json(coerce_card_deferred(c.args()[0])); });
    });
    golden::for_each("meeting.state.schema", "card_protected", [&](const golden::Case& c) {
        replay(c, [&] { return Json(CardItem::from_json(c.args()[0], clock).is_protected()); });
    });
}

// meeting.state.patches

TEST_CASE("golden meeting.state.patches", "[golden]") {
    const auto& clock = utc_clock();
    golden::for_each("meeting.state.patches", "script", [&](const golden::Case& c) {
        replay(c, [&] {
            testing::FixedIds ids;
            const Json& initial = c.args()[0];
            MeetingState state;
            if (initial.is_null()) {
                state.meeting_id = "m_test";
            } else {
                state = MeetingState::from_json(initial, clock);
            }
            Json results = Json::array();
            for (const auto& batch : c.args()[1]) {
                OpContext ctx;
                ctx.actor_type = batch.at(0).get<std::string>();
                if (!batch.at(1).is_null()) {
                    ctx.actor_id = batch.at(1).get<std::string>();
                }
                const Json& pinned = batch.at(3);
                bool raises = false;
                if (pinned.is_array()) {
                    for (const auto& id : pinned) {
                        raises = raises || id == "!raise";
                    }
                }
                ctx.segment_exists = predicate(batch.at(2));
                ctx.segment_pinned = predicate(pinned, raises);
                ctx.clock = &clock;
                ctx.ids = &ids;
                Json batch_results = Json::array();
                for (const auto& result : apply_ops(state, ctx, batch.at(4))) {
                    batch_results.push_back(result.to_json());
                }
                results.push_back(std::move(batch_results));
            }
            return Json{{"results", std::move(results)}, {"state", state.to_json()}};
        });
    });
    golden::for_each("meeting.state.patches", "filter_notes_ops", [&](const golden::Case& c) {
        replay(c, [&] {
            std::optional<std::set<std::string, std::less<>>> known;
            if (!c.args()[1].is_null()) {
                known.emplace();
                for (const auto& id : c.args()[1]) {
                    known->insert(id.get<std::string>());
                }
            }
            return filter_notes_ops(c.args()[0], known);
        });
    });
    golden::for_each("meeting.state.patches", "live_note_ids", [&](const golden::Case& c) {
        replay(c, [&] {
            Json out = Json::array();
            for (const auto& id : live_note_ids(c.args()[0])) {
                out.push_back(id);
            }
            return out;
        });
    });
    golden::for_each("meeting.state.patches", "normalize_item_text", [&](const golden::Case& c) {
        replay(c, [&] { return Json(detail::normalize_item_text(c.arg<std::string>(0))); });
    });
    golden::for_each("meeting.state.patches", "item_text_too_similar", [&](const golden::Case& c) {
        replay(c, [&] {
            return Json(detail::item_text_too_similar(c.arg<std::string>(0), c.arg<std::string>(1),
                                                      c.arg<double>(2), c.arg<double>(3)));
        });
    });
    golden::for_each("meeting.state.patches", "check_data_size", [&](const golden::Case& c) {
        replay(c, [&] { return optional_json(check_data_size(c.args()[0])); });
    });
    golden::for_each("meeting.state.patches", "check_text", [&](const golden::Case& c) {
        replay(c, [&] { return optional_json(check_text(c.args()[0], c.arg<std::size_t>(1))); });
    });
}

// meeting.corrections

TEST_CASE("golden meeting.corrections", "[golden]") {
    golden::for_each("meeting.corrections", "segment_fingerprint", [&](const golden::Case& c) {
        replay(c, [&] { return Json(segment_fingerprint(c.arg<std::string>(0))); });
    });
    golden::for_each("meeting.corrections", "valid_segment_fingerprint",
                     [&](const golden::Case& c) {
                         replay(c, [&] { return Json(valid_segment_fingerprint(c.args()[0])); });
                     });
    golden::for_each("meeting.corrections", "is_correction_author", [&](const golden::Case& c) {
        replay(c, [&] { return Json(is_correction_author(c.args()[0])); });
    });
    golden::for_each("meeting.corrections", "term_rules_from_items", [&](const golden::Case& c) {
        replay(c, [&] { return pairs_json(term_rules_from_items(c.args()[0])); });
    });
    golden::for_each("meeting.corrections", "term_rules", [&](const golden::Case& c) {
        replay(c, [&] { return pairs_json(term_rules(c.args()[0])); });
    });
    golden::for_each("meeting.corrections", "occurrence_rules", [&](const golden::Case& c) {
        replay(c, [&] {
            Json out = Json::object();
            for (const auto& [segment, rules] : occurrence_rules(c.args()[0])) {
                Json list = Json::array();
                for (const auto& rule : rules) {
                    list.push_back({{"selected_text", rule.selected_text},
                                    {"replacement", rule.replacement},
                                    {"occurrence_index", rule.occurrence_index},
                                    {"base_fingerprint", rule.base_fingerprint}});
                }
                out[segment] = std::move(list);
            }
            return out;
        });
    });
    golden::for_each("meeting.corrections", "correct_text", [&](const golden::Case& c) {
        replay(c, [&] { return Json(correct_text(c.arg<std::string>(0), rules_of(c.args()[1]))); });
    });
    golden::for_each("meeting.corrections", "correct_segment_text", [&](const golden::Case& c) {
        replay(c, [&] {
            return Json(correct_segment_text(c.arg<std::string>(0), c.arg<std::string>(1),
                                             rules_of(c.args()[2]), scoped_of(c.args()[3])));
        });
    });
    golden::for_each("meeting.corrections", "vocabulary_terms", [&](const golden::Case& c) {
        replay(c, [&] {
            return Json(vocabulary_terms(rules_of(c.args()[0]), c.arg<std::size_t>(1),
                                         c.arg<std::size_t>(2)));
        });
    });
    golden::for_each("meeting.corrections", "vocabulary_hint", [&](const golden::Case& c) {
        replay(c, [&] { return Json(vocabulary_hint(rules_of(c.args()[0]))); });
    });
}
