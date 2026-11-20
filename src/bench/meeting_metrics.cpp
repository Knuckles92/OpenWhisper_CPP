#include "bench/meeting_metrics.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <tuple>

#include "bench/detail/nfkc.h"
#include "pyport/regex.h"
#include "pyport/text.h"

namespace fv::bench {

namespace {

const py::Regex& token_pattern() {
    static const py::Regex pattern(R"([\w]+(?:'[\w]+)?)");
    return pattern;
}

std::string replace_all(std::string text, std::string_view from, std::string_view to) {
    for (auto at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
    return text;
}

double number(const Json& value) {
    // float(x or 0.0): a missing, null, zero or empty value is 0.
    if (value.is_number()) {
        return value.get<double>();
    }
    if (value.is_string() && !value.get<std::string>().empty()) {
        return std::stod(value.get<std::string>());
    }
    return 0.0;
}

Json counts_json(const EditCounts& counts) {
    return Json{{"substitutions", counts.substitutions},
                {"deletions", counts.deletions},
                {"insertions", counts.insertions}};
}

double ratio(std::size_t part, std::size_t whole) {
    return static_cast<double>(part) / static_cast<double>(whole);
}

} // namespace

EditCounts& EditCounts::operator+=(const EditCounts& other) {
    substitutions += other.substitutions;
    deletions += other.deletions;
    insertions += other.insertions;
    return *this;
}

std::vector<std::string> normalize_tokens(std::string_view text) {
    std::string normalized = py::lower(detail::nfkc(text));
    normalized = replace_all(std::move(normalized), "\u2019", "'");
    normalized = replace_all(std::move(normalized), "`", "'");
    // AMI writes spoken acronyms as A_S_R_ and inflected forms as C_D_s; the recognizer writes
    // asr and cds, so the separators are not lexical characters for scoring.
    normalized = replace_all(std::move(normalized), "_", "");
    return token_pattern().findall(normalized);
}

EditCounts edit_counts(std::span<const std::string> reference,
                       std::span<const std::string> hypothesis) {
    // Cell: total errors, substitutions, deletions, insertions; compared as Python's tuples.
    using Cell = std::tuple<std::size_t, std::size_t, std::size_t, std::size_t>;
    std::vector<Cell> previous(reference.size() + 1);
    for (std::size_t i = 0; i <= reference.size(); ++i) {
        previous[i] = {i, 0, i, 0};
    }
    std::vector<Cell> current;
    for (std::size_t h = 1; h <= hypothesis.size(); ++h) {
        current.assign(1, Cell{h, 0, 0, h});
        for (std::size_t r = 1; r <= reference.size(); ++r) {
            if (reference[r - 1] == hypothesis[h - 1]) {
                current.push_back(previous[r - 1]);
                continue;
            }
            const auto& [dt, ds, dd, di] = previous[r - 1];
            const auto& [lt, ls, ld, li] = current[r - 1];
            const auto& [ut, us, ud, ui] = previous[r];
            using Candidate = std::tuple<std::size_t, std::size_t, std::size_t, std::size_t, int>;
            const Candidate best = std::min({Candidate{dt + 1, ds + 1, dd, di, 0},
                                             Candidate{lt + 1, ls, ld + 1, li, 1},
                                             Candidate{ut + 1, us, ud, ui + 1, 2}});
            current.emplace_back(std::get<0>(best), std::get<1>(best), std::get<2>(best),
                                 std::get<3>(best));
        }
        previous.swap(current);
    }
    const auto& [total, substitutions, deletions, insertions] = previous.back();
    (void)total;
    return EditCounts{substitutions, deletions, insertions};
}

Json TextScore::to_json() const {
    Json out = counts_json(counts);
    out["words"] = words;
    out["hypothesis_words"] = hypothesis_words;
    out["errors"] = errors();
    out["wer"] = wer ? Json(*wer) : Json(nullptr);
    return out;
}

TextScore score_text(std::string_view reference, std::string_view hypothesis) {
    const auto ref = normalize_tokens(reference);
    const auto hyp = normalize_tokens(hypothesis);
    TextScore score;
    score.counts = edit_counts(ref, hyp);
    score.words = ref.size();
    score.hypothesis_words = hyp.size();
    if (!ref.empty()) {
        score.wer = ratio(score.errors(), ref.size());
    }
    return score;
}

Json reference_overlap_stats(std::span<const ReferenceWord> reference_words) {
    std::vector<ReferenceWord> ordered(reference_words.begin(), reference_words.end());
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const ReferenceWord& a, const ReferenceWord& b) {
                         return std::tie(a.start_s, a.end_s) < std::tie(b.start_s, b.end_s);
                     });
    std::set<std::size_t> overlapping;
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        const ReferenceWord& word = ordered[i];
        if (word.end_s <= word.start_s) {
            continue;
        }
        for (std::size_t j = i + 1; j < ordered.size(); ++j) {
            const ReferenceWord& other = ordered[j];
            if (other.start_s >= word.end_s) {
                break;
            }
            if (other.speaker != word.speaker && other.end_s > other.start_s &&
                other.end_s > word.start_s) {
                overlapping.insert(i);
                overlapping.insert(j);
            }
        }
    }
    const std::size_t total = ordered.size();
    return Json{{"reference_words", total},
                {"overlapping_reference_words", overlapping.size()},
                {"overlap_rate", total != 0 ? ratio(overlapping.size(), total) : 0.0}};
}

std::vector<TimedText> timed_texts(const Json& segments) {
    std::vector<TimedText> out;
    for (const Json& segment : segments) {
        TimedText item;
        item.start_s = number(segment.value("start_s", Json(nullptr)));
        item.end_s = number(segment.value("end_s", Json(nullptr)));
        const Json text = segment.value("text", Json(""));
        item.text =
            text.is_string() ? text.get<std::string>() : (text.is_null() ? "" : text.dump());
        out.push_back(std::move(item));
    }
    return out;
}

Json score_timed_transcript(std::span<const ReferenceWord> reference_words,
                            std::span<const TimedText> hypothesis_segments, double window_s) {
    if (window_s <= 0) {
        throw std::invalid_argument("window_s must be positive");
    }
    double evaluation_start_s = 0.0;
    double evaluation_end_s = 0.0;
    if (!reference_words.empty()) {
        evaluation_start_s = reference_words.front().start_s;
        evaluation_end_s = reference_words.front().end_s;
        for (const ReferenceWord& word : reference_words) {
            evaluation_start_s = std::min(evaluation_start_s, word.start_s);
            evaluation_end_s = std::max(evaluation_end_s, word.end_s);
        }
    }
    const auto window_count = std::max<std::int64_t>(
        1, static_cast<std::int64_t>(std::floor(evaluation_end_s / window_s)) + 1);
    std::vector<const TimedText*> in_scope;
    std::size_t ignored_hypothesis_words = 0;
    for (const TimedText& segment : hypothesis_segments) {
        if (segment.end_s > evaluation_start_s && segment.start_s < evaluation_end_s) {
            in_scope.push_back(&segment);
        } else {
            ignored_hypothesis_words += normalize_tokens(segment.text).size();
        }
    }

    EditCounts totals;
    std::size_t total_reference = 0;
    std::size_t total_hypothesis = 0;
    Json windows = Json::array();
    for (std::int64_t index = 0; index < window_count; ++index) {
        const double start_s = static_cast<double>(index) * window_s;
        const double end_s = start_s + window_s;
        std::vector<std::string> reference;
        for (const ReferenceWord& word : reference_words) {
            if (start_s <= word.start_s && word.start_s < end_s) {
                for (auto& token : normalize_tokens(word.text)) {
                    reference.push_back(std::move(token));
                }
            }
        }
        std::vector<std::string> hypothesis;
        for (const TimedText* segment : in_scope) {
            const double at = std::max(evaluation_start_s, segment->start_s);
            if (start_s <= at && at < end_s) {
                for (auto& token : normalize_tokens(segment->text)) {
                    hypothesis.push_back(std::move(token));
                }
            }
        }
        const EditCounts counts = edit_counts(reference, hypothesis);
        totals += counts;
        total_reference += reference.size();
        total_hypothesis += hypothesis.size();
        Json window{{"start_s", start_s},
                    {"end_s", end_s},
                    {"reference_words", reference.size()},
                    {"hypothesis_words", hypothesis.size()}};
        window.update(counts_json(counts));
        window["wer"] = !reference.empty()   ? ratio(counts.errors(), reference.size())
                        : hypothesis.empty() ? 0.0
                                             : 1.0;
        windows.push_back(std::move(window));
    }

    const std::size_t denominator = std::max<std::size_t>(1, total_reference);
    Json out{{"metric", "tcWER-" + std::to_string(static_cast<std::int64_t>(window_s)) + "s"},
             {"evaluation_start_s", evaluation_start_s},
             {"evaluation_end_s", evaluation_end_s},
             {"out_of_reference_hypothesis_words", ignored_hypothesis_words},
             {"reference_words", total_reference},
             {"hypothesis_words", total_hypothesis}};
    out.update(counts_json(totals));
    out["wer"] = ratio(totals.errors(), denominator);
    out["substitution_rate"] = ratio(totals.substitutions, denominator);
    out["deletion_rate"] = ratio(totals.deletions, denominator);
    out["insertion_rate"] = ratio(totals.insertions, denominator);
    out["windows"] = std::move(windows);
    return out;
}

Json aggregate_scores(std::span<const Json> scores) {
    std::size_t reference_words = 0;
    std::size_t hypothesis_words = 0;
    std::size_t substitutions = 0;
    std::size_t deletions = 0;
    std::size_t insertions = 0;
    double wer_sum = 0.0;
    for (const Json& item : scores) {
        reference_words += item.at("reference_words").get<std::size_t>();
        hypothesis_words += item.at("hypothesis_words").get<std::size_t>();
        substitutions += item.at("substitutions").get<std::size_t>();
        deletions += item.at("deletions").get<std::size_t>();
        insertions += item.at("insertions").get<std::size_t>();
        wer_sum += item.at("wer").get<double>();
    }
    const std::size_t errors = substitutions + deletions + insertions;
    const std::size_t denominator = std::max<std::size_t>(1, reference_words);
    return Json{{"meetings", scores.size()},
                {"reference_words", reference_words},
                {"hypothesis_words", hypothesis_words},
                {"substitutions", substitutions},
                {"deletions", deletions},
                {"insertions", insertions},
                {"wer", ratio(errors, denominator)},
                {"substitution_rate", ratio(substitutions, denominator)},
                {"deletion_rate", ratio(deletions, denominator)},
                {"insertion_rate", ratio(insertions, denominator)},
                {"macro_wer", scores.empty() ? 0.0 : wer_sum / static_cast<double>(scores.size())}};
}

} // namespace fv::bench
