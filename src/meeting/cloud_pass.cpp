#include "meeting/cloud_pass.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>

#include "meeting/detail/py_value.h"
#include "pyport/text.h"

namespace fv::meeting {
namespace {

constexpr double kTieTolerance = 1e-6;

std::optional<double> time(const Json& value) {
    if (value.is_number()) {
        const auto number = value.get<double>();
        if (std::isfinite(number)) {
            return number;
        }
    } else if (value.is_string()) {
        const auto text = py::strip(value.get<std::string>());
        try {
            std::size_t size = 0;
            const auto number = std::stod(text, &size);
            if (size == text.size() && std::isfinite(number)) {
                return number;
            }
        } catch (const std::exception&) {
            // A malformed row does not make a usable interval.
        }
    }
    return std::nullopt;
}

struct Turn {
    std::string speaker;
    double start;
    double end;
};

std::optional<Turn> turn_from(const Json& value) {
    const auto speaker = py::strip(detail::str_or_empty(detail::get(value, "speaker")));
    const auto start = time(detail::get(value, "start"));
    const auto end = time(detail::get(value, "end"));
    if (speaker.empty() || !start || !end || *start >= *end) {
        return std::nullopt;
    }
    return Turn{speaker, *start, *end};
}

double duration(const Json& row) {
    const auto start = time(detail::get(row, "start_s"));
    const auto end = time(detail::get(row, "end_s"));
    return start && end ? *end - *start : 0;
}

} // namespace

double overlap_duration(double start_a, double end_a, double start_b, double end_b) {
    return std::max(0.0, std::min(end_a, end_b) - std::max(start_a, start_b));
}

NamedSpeakers map_turns_to_segments(const std::vector<Json>& segments,
                                    const std::vector<Json>& turns) {
    std::vector<Turn> usable;
    for (const auto& value : turns) {
        if (auto turn = turn_from(value)) {
            usable.push_back(std::move(*turn));
        }
    }
    NamedSpeakers result;
    for (const auto& row : segments) {
        const auto id = detail::str_or_empty(detail::get(row, "id"));
        const auto start = time(detail::get(row, "start_s"));
        const auto end = time(detail::get(row, "end_s"));
        if (id.empty() || !start || !end || detail::truthy(detail::get(row, "speaker_pinned"))) {
            continue;
        }
        const Turn* best = nullptr;
        double best_overlap = 0;
        for (const auto& turn : usable) {
            const auto overlap = overlap_duration(*start, *end, turn.start, turn.end);
            if (overlap > 0 && (overlap > best_overlap + kTieTolerance ||
                                (std::abs(overlap - best_overlap) <= kTieTolerance &&
                                 (!best || turn.start < best->start)))) {
                best = &turn;
                best_overlap = overlap;
            }
        }
        if (best) {
            result.emplace_back(id, best->speaker);
        }
    }
    return result;
}

SpeakerMap resolve_speaker_participants(const SpeakerGroups& speakers,
                                        const std::map<std::string, Json, std::less<>>& by_id,
                                        const NamedSpeakers& named) {
    SpeakerMap result;
    std::set<std::string> claimed;
    std::set<std::string> matched;
    for (const auto& [speaker, participant] : named) {
        result.emplace_back(speaker, participant);
        claimed.insert(participant);
        matched.insert(speaker);
    }
    struct Vote {
        std::string speaker;
        std::string winner;
        int votes;
        std::size_t coverage;
    };
    std::vector<Vote> pending;
    for (const auto& [speaker, segments] : speakers) {
        if (matched.contains(speaker)) {
            continue;
        }
        std::map<std::string, int> votes;
        for (const auto& id : segments) {
            const auto row = by_id.find(id);
            if (row != by_id.end()) {
                const auto participant =
                    detail::str_or_empty(detail::get(row->second, "speaker_participant_id"));
                if (!participant.empty()) {
                    ++votes[participant];
                }
            }
        }
        if (votes.empty()) {
            result.emplace_back(speaker, std::nullopt);
        } else {
            const auto winner =
                std::max_element(votes.begin(), votes.end(), [](const auto& a, const auto& b) {
                    return std::tie(a.second, a.first) < std::tie(b.second, b.first);
                });
            pending.push_back({speaker, winner->first, winner->second, segments.size()});
        }
    }
    std::sort(pending.begin(), pending.end(), [](const Vote& a, const Vote& b) {
        if (a.votes != b.votes) {
            return a.votes > b.votes;
        }
        if (a.coverage != b.coverage) {
            return a.coverage > b.coverage;
        }
        return a.speaker < b.speaker;
    });
    for (const auto& vote : pending) {
        result.emplace_back(vote.speaker, claimed.insert(vote.winner).second
                                              ? std::optional(vote.winner)
                                              : std::nullopt);
    }
    return result;
}

std::string speaker_slug(std::string_view name) {
    std::string result;
    bool separator = false;
    for (const char ch : name) {
        const bool ascii_alnum =
            (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
        if (ascii_alnum) {
            if (separator && !result.empty()) {
                result += '_';
            }
            result += ch;
            separator = false;
        } else {
            separator = true;
        }
    }
    if (result.empty()) {
        return "speaker";
    }
    return result.substr(0, 32);
}

std::vector<std::pair<Json, Json>> pick_reference_segments(const std::vector<Json>& participants,
                                                           const std::vector<Json>& segments) {
    std::vector<std::pair<Json, Json>> result;
    for (const auto& participant : participants) {
        if (result.size() == kMaxKnownSpeakers) {
            break;
        }
        if (detail::get(participant, "kind") != "others_cluster" ||
            detail::get(participant, "name_source") != "human") {
            continue;
        }
        const Json& id = detail::get(participant, "id");
        if (!id.is_string() || id.get_ref<const std::string&>().empty()) {
            continue;
        }
        const Json* pinned = nullptr;
        const Json* unpinned = nullptr;
        for (const auto& row : segments) {
            if (detail::get(row, "channel") != "loopback" ||
                detail::get(row, "speaker_participant_id") != id) {
                continue;
            }
            const bool pin = detail::truthy(detail::get(row, "speaker_pinned"));
            const auto*& best = pin ? pinned : unpinned;
            if (!best || duration(row) > duration(*best)) {
                best = &row;
            }
        }
        const auto* best = unpinned ? unpinned : pinned;
        if (best && duration(*best) > 0) {
            result.emplace_back(participant, *best);
        }
    }
    return result;
}

std::vector<Json> parse_diarized_result(const Json& result) {
    std::vector<Json> turns;
    const auto& segments = detail::get(result, "segments");
    if (segments.is_array()) {
        for (const auto& value : segments) {
            if (const auto turn = turn_from(value)) {
                turns.push_back(
                    Json{{"speaker", turn->speaker}, {"start", turn->start}, {"end", turn->end}});
            }
        }
    }
    return turns;
}

std::vector<Json> project_window_turns(const Json& turns, double origin, double duration_s) {
    std::vector<Json> projected;
    if (turns.is_array() && std::isfinite(origin) && std::isfinite(duration_s) && duration_s > 0) {
        for (const auto& row : turns) {
            if (const auto turn = turn_from(row)) {
                const auto start = std::clamp(turn->start, 0.0, duration_s);
                const auto end = std::clamp(turn->end, 0.0, duration_s);
                if (end > start) {
                    projected.push_back(Json{{"speaker", turn->speaker},
                                             {"start", origin + start},
                                             {"end", origin + end}});
                }
            }
        }
    }
    return projected;
}

} // namespace fv::meeting
