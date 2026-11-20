#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "automation/filters.h"
#include "base/cancel.h"

// What one filter said, and how a filter is waited on (filters.py _Verdict, _interpret, _await,
// _program_verdict).
namespace fv::automation::detail {

enum class VerdictKind { Keep, Replace, Cancel, Failed, Canceled };

// Affinity: value type.
struct Verdict {
    VerdictKind kind = VerdictKind::Keep;
    std::string text;   // Replace
    std::string reason; // Failed: fixed copy, never the text

    friend bool operator==(const Verdict&, const Verdict&) = default;
};

// _interpret: a reply body as keep, replace, cancel or failed (with the reason). Throws
// py::RecursionError for JSON nested deeper than Python reads, as json.loads raises it.
Verdict interpret(std::string_view raw);

// _await: `work` on a thread of its own, waited on until it answers, `timeout_s` passes (failed,
// "No answer within N s") or `cancel` fires (Canceled), whichever is first; a cancel wins over an
// answer that came with it. `work` gets a token that fires when it is abandoned, so a request
// can end early; a `work` that throws is failed "<failed_copy> (<type>)". Never throws.
Verdict await_verdict(std::function<Verdict(const CancelToken& abandoned)> work,
                      std::optional<double> timeout_s, const CancelToken& cancel,
                      std::string_view failed_copy);

// _program_verdict: runs the filter program once with `body` and a newline on stdin, stopped at
// `timeout_s` or by `cancel`, and reads its answer from stdout.
Verdict program_verdict(const FilterRunOnce& run_once, const std::vector<std::string>& argv,
                        std::string_view body, double timeout_s, const CancelToken& cancel);

} // namespace fv::automation::detail
