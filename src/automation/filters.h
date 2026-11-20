#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/cancel.h"
#include "base/clock.h"
#include "base/events.h"
#include "base/json.h"
#include "dictation/automation_port.h"
#include "dictation/job.h"
#include "http/client.h"
#include "process/run_once.h"

// Text filters: outside programs and services that change dictated text
// (services/automation/filters.py). A filter sees the words of a live dictation or action at one
// stage (before_cleanup or after_cleanup) and answers with new text, no change, or "deliver
// nothing". Saved filters (Settings > Hooks) run first, in their saved order, then filters leased
// over the live API, in the order they were registered; a dictation Route by voice may send to an
// action is routed between the two (StageRun). Words a lease changed are a program's, not the
// user's (program_changed).
//
// Request: {"v":1,"stage","job_id","mode","action_id","text","language","app"} as compact JSON
// (POSTed to an http filter; on stdin, with a newline, for a program filter). Reply: {"text"}
// replaces the text; {}, an empty body or HTTP 204 keeps it; {"cancel": true} or an empty text
// delivers nothing. Anything else keeps the text and publishes filter.failed, without the text.
// A filter has its own time limit and a stage has STAGE_BUDGET_S in all. A cancelled dictation
// stops waiting on its filters at once. Dictated text is never logged.
namespace fv::automation {

namespace filters {
// How long a lease lives unless renewed, in seconds.
inline constexpr double LEASE_TTL_S = 60.0;
// Live leases at most.
inline constexpr std::size_t MAX_LEASES = 10;
// All the filters of one stage, together, in seconds.
inline constexpr double STAGE_BUDGET_S = 3.0;
// A filter is not started with less time than this left, in seconds.
inline constexpr double MIN_SLICE_S = 0.05;
// Job modes filters apply to (never Command Mode, transforms or files).
inline constexpr std::array<std::string_view, 2> MODES = {"dictation", "action"};
// Hosts a leased filter may listen on.
inline constexpr std::array<std::string_view, 3> LOOPBACK_HOSTS = {"127.0.0.1", "::1", "localhost"};
// Most of a reply that is read, in bytes.
inline constexpr std::size_t MAX_REPLY_BYTES = 1024 * 1024;
inline constexpr int REQUEST_VERSION = 1;
// The name filter.failed gives a leased filter.
inline constexpr std::string_view LEASED_NAME = "API filter";
} // namespace filters

// filters.FilterOutcome: the pipeline's FilterResult, field for field.
using FilterOutcome = dictation::FilterResult;

// A filter a program on this computer leased over the live API.
// Affinity: value type.
struct Lease {
    std::string id; // "flt_" and 16 lowercase hex digits
    std::string url;
    std::string stage;
    std::int64_t timeout_ms = 1500;
    std::vector<std::string> modes;
    // Unix seconds it lapses at unless renewed (time.time() + LEASE_TTL_S).
    double expires_at = 0.0;

    // The live API's shape (api.py _lease_dict): {id, url, stage, timeout_ms, modes, expires_at,
    // ttl_s}.
    Json to_json() const;
    friend bool operator==(const Lease&, const Lease&) = default;
};

// The live leases. A lease lapses on the clock's steady time, immune to wall-clock changes,
// while expires_at reports wall-clock time.
// Affinity: thread-safe.
class FilterLeases {
public:
    explicit FilterLeases(const Clock& clock = system_clock(), double ttl_s = filters::LEASE_TTL_S);

    // Leases a filter for the TTL. Throws py::ValueError with the copy for the caller when the
    // URL (stripped) is not http(s) on 127.0.0.1, ::1 or localhost, the stage is not a stage,
    // timeout_ms is not a whole number from 100 to 5000 (a float with an integer value counts;
    // a bool never does), modes is not a non-empty list of dictation and action, or MAX_LEASES
    // are already live; py::TypeError for a list or object among the modes, as Python's
    // dict.fromkeys raises. nullopt arguments are the defaults (1500, both modes).
    Lease register_lease(std::string_view url, std::string_view stage,
                         std::optional<Json> timeout_ms = std::nullopt,
                         std::optional<Json> modes = std::nullopt);
    // Extends a live lease by the TTL from now; nullopt when it lapsed, was dropped or never
    // existed (Python's KeyError).
    std::optional<Lease> renew_lease(std::string_view id);
    // Forgets a lease; false when it was not live.
    bool drop_lease(std::string_view id);
    // Forgets every lease (the live API stopped); how many were live.
    int drop_all_leases();
    // Live leases, oldest registration first.
    std::vector<Lease> leases() const;

private:
    void purge_expired() const; // caller holds mutex_

    const Clock& clock_;
    double ttl_s_;
    mutable std::mutex mutex_;
    mutable std::vector<Lease> leases_;
    mutable std::vector<std::chrono::steady_clock::time_point> deadlines_; // beside leases_
};

// run_once as a program filter calls it; tests replace it.
using FilterRunOnce =
    std::function<RunOnceResult(const std::vector<std::string>& argv, std::string_view input,
                                double timeout_s, const CancelToken& cancel)>;

// What running filters needs besides the settings.
struct FilterContext {
    FilterLeases& leases;
    std::shared_ptr<http::Client> http;
    // filter.failed goes here.
    EventSink& events;
    // Empty: fv::run_once.
    FilterRunOnce run_once = {};
    // Reads the settings when a run was given none (Python's load_all_settings).
    std::function<Json()> load_settings = {};
};

// One stage's filters for one job, in two parts sharing STAGE_BUDGET_S: the user's saved
// filters, then (after routing) the leases, for the job as it is then. Each part returns what
// apply would for its filters alone. A cancel ends the wait on the current filter at once, skips
// the rest and returns the text as it came, without filter.failed. Never throws for a filter's
// sake.
// Affinity: one caller at a time (the jobs strand); each filter waits on a thread of its own.
class StageRun final : public dictation::FilterStageRun {
public:
    StageRun(std::string stage, std::optional<Json> settings, FilterContext context,
             CancelToken cancel = {});

    // The user's saved filters; never program_changed.
    FilterOutcome saved(std::string text, const dictation::DictationJob& job) override;
    // The filters programs leased over the live API.
    FilterOutcome leased(std::string text, const dictation::DictationJob& job) override;
    // Both, in one run.
    FilterOutcome apply(std::string text, const dictation::DictationJob& job) override;

    // What is left of the stage's budget, in seconds.
    double budget_s() const { return budget_s_; }

private:
    struct Step;

    FilterOutcome run(std::string text, const dictation::DictationJob* job, bool saved,
                      bool leased);
    std::vector<Step> steps(const dictation::DictationJob& job, bool saved, bool leased) const;
    FilterOutcome run_steps(const std::vector<Step>& steps, const std::string& text,
                            const dictation::DictationJob& job,
                            std::chrono::steady_clock::time_point deadline);
    void failed(const Step& step, std::string_view reason);

    std::string stage_;
    std::optional<Json> settings_;
    FilterContext context_;
    CancelToken cancel_;
    double budget_s_ = filters::STAGE_BUDGET_S;
};

// filters.apply: the filters of `stage` on a live dictation or action; a worker thread. The text
// comes back at once when none apply: no job, Command Mode, a transform, no words (silence is
// never a cancel), no enabled saved filter for the stage and no live lease (leases also need the
// text filters permission). canceled is true only when a filter answered cancel or turned the
// words into none.
FilterOutcome apply(std::string_view stage, std::string text, const dictation::DictationJob* job,
                    std::optional<Json> settings, FilterContext context,
                    const CancelToken& cancel = {});

} // namespace fv::automation
