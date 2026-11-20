#pragma once

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/cancel.h"
#include "base/json.h"
#include "llm/judge.h"

namespace fv::llm {

// A Judge for tests of the code that asks one: scripted answers (or failures, as nullopt),
// optional delays that a cancel cuts short, and every question it was asked.
// Affinity: thread-safe.
class FakeJudge final : public Judge {
public:
    struct Call {
        Json state;
        Json questions;
        std::optional<double> timeout_s;
    };

    // The next ask answers `answers` (nullopt: no judgment) after `delay`.
    void push(std::optional<Json> answers, std::chrono::milliseconds delay = {}) {
        std::lock_guard lock(mutex_);
        script_.push_back({std::move(answers), delay});
    }
    // Answers every ask once the script is empty; without one, an empty script is no judgment.
    void set_handler(
        std::function<std::optional<Json>(const Json& state, const Json& questions)> handler) {
        std::lock_guard lock(mutex_);
        handler_ = std::move(handler);
    }
    void set_model(std::string model) {
        std::lock_guard lock(mutex_);
        model_ = std::move(model);
    }
    void set_local(bool local) {
        std::lock_guard lock(mutex_);
        local_ = local;
    }
    std::vector<Call> calls() const {
        std::lock_guard lock(mutex_);
        return calls_;
    }

    std::optional<Json> ask(const Json& state, const Json& questions,
                            std::optional<double> timeout_s, const CancelToken& cancel) override {
        Step step;
        std::function<std::optional<Json>(const Json&, const Json&)> handler;
        {
            std::lock_guard lock(mutex_);
            calls_.push_back({state, questions, timeout_s});
            if (!script_.empty()) {
                step = std::move(script_.front());
                script_.pop_front();
            } else {
                handler = handler_;
            }
        }
        if (step.delay.count() > 0 && cancel.wait_for(step.delay)) {
            return std::nullopt;
        }
        if (cancel.cancelled()) {
            return std::nullopt;
        }
        if (handler) {
            return handler(state, questions);
        }
        return step.answers;
    }
    std::string model() const override {
        std::lock_guard lock(mutex_);
        return model_;
    }
    bool is_local() const override {
        std::lock_guard lock(mutex_);
        return local_;
    }

    // {"q": {"type": "noul", "noul": p}}, as a judge answers Judge::noul.
    static Json noul_answer(double probability, const std::string& name = "q") {
        return Json{{name, Json{{"type", "noul"}, {"noul", probability}}}};
    }
    // {"q": {"type": "choice", ...}}, as a judge answers Judge::choice.
    static Json choice_answer(const std::string& choice, double confidence,
                              Json probabilities = Json::object(), const std::string& name = "q") {
        return Json{{name, Json{{"type", "choice"},
                                {"choice", choice},
                                {"confidence", confidence},
                                {"probabilities", std::move(probabilities)}}}};
    }

private:
    struct Step {
        std::optional<Json> answers;
        std::chrono::milliseconds delay{0};
    };

    mutable std::mutex mutex_;
    std::deque<Step> script_;
    std::function<std::optional<Json>(const Json&, const Json&)> handler_;
    std::vector<Call> calls_;
    std::string model_ = "fake-judge";
    bool local_ = false;
};

// A JudgeProvider with a settable judge (null: Fast judgments off or no key), route and key
// presence; it records the settings it was asked about.
// Affinity: thread-safe.
class FakeJudgeProvider final : public JudgeProvider {
public:
    void set_judge(std::shared_ptr<Judge> judge) {
        std::lock_guard lock(mutex_);
        judge_ = std::move(judge);
    }
    void set_route(JudgeRoute route) {
        std::lock_guard lock(mutex_);
        route_ = std::move(route);
    }
    void set_key_present(bool present) {
        std::lock_guard lock(mutex_);
        key_present_ = present;
    }
    std::vector<Json> asked() const {
        std::lock_guard lock(mutex_);
        return asked_;
    }

    std::shared_ptr<Judge> judge(const Json& settings) override {
        std::lock_guard lock(mutex_);
        asked_.push_back(settings);
        return judge_;
    }
    JudgeRoute route(const Json& settings) override {
        std::lock_guard lock(mutex_);
        asked_.push_back(settings);
        return route_;
    }
    bool key_present(const Json& settings) override {
        std::lock_guard lock(mutex_);
        asked_.push_back(settings);
        return key_present_;
    }

private:
    mutable std::mutex mutex_;
    std::shared_ptr<Judge> judge_;
    JudgeRoute route_ = typesafe_route();
    bool key_present_ = true;
    std::vector<Json> asked_;
};

} // namespace fv::llm
