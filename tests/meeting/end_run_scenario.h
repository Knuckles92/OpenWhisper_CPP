#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "base/json.h"
#include "meeting/end_run.h"

// One End as the engine drives it (plan, announce, redecode, identify_speakers, insights), with
// every effect a fake scripted by a scenario. The scenario and the result have the shape of
// tools/parity/cases/meeting.end_run.py, so the unit tests and the golden replay share them.
namespace fv::meeting::testing {

inline const Json& field(const Json& scenario, const char* key) {
    static const Json null_value;
    return scenario.contains(key) ? scenario.at(key) : null_value;
}

template <class T> T value_or(const Json& scenario, const char* key, T fallback) {
    return scenario.contains(key) ? scenario.at(key).get<T>() : fallback;
}

inline Json run_scenario(const Json& s) {
    auto events = std::make_shared<Json>(Json::array());
    EndRun::Effects effects;
    effects.set_finalization = [events](const FinalizationUpdate& update) {
        events->push_back(Json::array({"set_finalization", update.to_json()}));
        return true;
    };
    effects.redecode = [s](const ProgressCallback& progress) {
        const Json spec = s.contains("redecode") ? s.at("redecode") : Json{{"result", true}};
        for (const auto& call : spec.value("progress", Json::array())) {
            progress(call.at(0).get<std::string>(), call.at(1).get<int>(), call.at(2).get<int>());
        }
        if (spec.at("result") == "raise") {
            throw std::runtime_error("redecode failed");
        }
        return spec.at("result").get<bool>();
    };
    effects.redecode_failure = [s] {
        return value_or<std::string>(s, "redecode_failure", "");
    };
    effects.identify_speakers = [s](const ProgressCallback& progress,
                                    const std::function<void()>& on_start) {
        const Json spec = s.contains("speakers") ? s.at("speakers")
                                                 : Json{{"result", {{"ok", true}, {"applied", 1}}}};
        if (spec.value("on_start", false)) {
            on_start();
        }
        for (const auto& call : spec.value("progress", Json::array())) {
            progress(call.at(0).get<std::string>(), call.at(1).get<int>(), call.at(2).get<int>());
        }
        if (spec.at("result") == "raise") {
            throw std::runtime_error("speakers failed");
        }
        return spec.at("result");
    };
    effects.allow_agent_writes = [events, s] {
        events->push_back(Json::array({"allow"}));
        if (value_or(s, "allow_raises", false)) {
            throw std::runtime_error("allow failed");
        }
    };
    effects.revoke_agent_writes = [events] {
        events->push_back(Json::array({"revoke"}));
    };
    effects.core_is_healthy = [s] {
        return value_or(s, "core_healthy", true);
    };
    effects.offline_message = [s] {
        return value_or<std::string>(s, "offline_message", "Meeting intelligence is offline.");
    };
    effects.summary_stats = [s] {
        return s.contains("summary_stats") ? s.at("summary_stats")
                                           : Json{{"segments", 3}, {"words", 12}};
    };
    if (value_or(s, "scheduler", false)) {
        if (s.contains("polish")) {
            effects.polish = [s](double, const ProgressCallback& progress) {
                const Json& spec = s.at("polish");
                for (const auto& call : spec.value("progress", Json::array())) {
                    progress(call.at(0).get<std::string>(), call.at(1).get<int>(),
                             call.at(2).get<int>());
                }
                if (spec.at("result") == "raise") {
                    throw std::runtime_error("polish failed");
                }
                return spec.at("result");
            };
        }
        effects.consolidate = [s](const std::function<void(const std::string&)>& progress) {
            const Json spec = s.contains("consolidate")
                                  ? s.at("consolidate")
                                  : Json{{"result", {{"status", "completed"}}}};
            for (const auto& detail : spec.value("progress", Json::array())) {
                progress(detail.get<std::string>());
            }
            if (spec.at("result") == "raise") {
                throw std::runtime_error("consolidate failed");
            }
            return spec.at("result");
        };
    }

    EndOptions options;
    options.meeting_id = "m_test";
    options.complete = value_or(s, "complete", true);
    options.unfinished = value_or<std::int64_t>(s, "unfinished", 0);
    options.has_scheduler = value_or(s, "scheduler", false);
    options.will_offline = value_or(s, "will_offline", false);
    options.will_speaker_id = value_or(s, "will_speaker_id", false);
    EndRun end(std::move(effects), options);
    end.plan(value_or(s, "cloud_enabled", false), value_or(s, "want_polish", true),
             value_or(s, "want_report", true));
    end.announce();
    end.redecode();
    end.identify_speakers();
    end.insights();
    return {
        {"events", *events},
        {"state",
         {{"run_cloud", end.run_cloud()},
          {"total_steps", end.total_steps()},
          {"offline_ok", end.offline_ok()},
          {"speaker_ok", end.speaker_ok()},
          {"speaker_skipped", end.speaker_skipped()},
          {"speaker_error", end.speaker_error()}}},
    };
}

// The set_finalization payloads of a run, in order.
inline Json finalizations(const Json& run) {
    Json out = Json::array();
    for (const auto& event : run.at("events")) {
        if (event.at(0) == "set_finalization") {
            out.push_back(event.at(1));
        }
    }
    return out;
}

} // namespace fv::meeting::testing
