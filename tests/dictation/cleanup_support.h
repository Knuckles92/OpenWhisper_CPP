#pragma once

// The fakes OpenWhisper's cleanup tests monkeypatch into place (tests/test_personalize_*.py
// _cleanup_on and the runtime fixture's cleaner): a cleaner that records what it is sent and
// answers as the test says, a preparer that records the learning it is asked for, and jobs whose
// focus capture has already resolved.

#include <chrono>
#include <condition_variable>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "base/identity.h"
#include "base/testing/map_env_reader.h"
#include "credentials/dotenv.h"
#include "credentials/store.h"
#include "credentials/testing/memory_backend.h"
#include "dictation/cleaner_port.h"
#include "dictation/job.h"
#include "dictation/text_preparer.h"
#include "focus/capture_service.h"
#include "focus/snapshot.h"
#include "http/client.h"
#include "http/testing/fake_http_client.h"
#include "llm/credentials.h"
#include "llm/transcript_cleanup.h"
#include "pyport/json.h"
#include "speech/testing/fake_backend.h"
#include "text/cleanup_prompts.h"

namespace fv::dictation::testing {

// _cleanup_on: an available cleaner of "openai" / "gpt" whose answer `result` makes from the
// text it is sent. Affinity: thread-safe.
class FakeCleaner final : public TranscriptCleaner {
public:
    struct Call {
        std::string text;
        std::string system_prompt;
        std::optional<double> timeout_s;
    };

    explicit FakeCleaner(std::function<std::string(const std::string&)> answer =
                             [](const std::string& text) { return text; })
        : answer_(std::move(answer)) {}

    bool is_available(const Json& settings) override {
        std::lock_guard lock(mutex_);
        availability_checks_.push_back(settings);
        return available;
    }
    CleanupResult cleanup(const CleanupRequest& request) override {
        {
            std::lock_guard lock(mutex_);
            calls_.push_back({request.text, request.system_prompt, request.timeout_s});
        }
        if (during) {
            during();
        }
        CleanupResult result;
        result.provider = "openai";
        result.model = "gpt";
        if (error) {
            result.text = request.text;
            result.error = error;
            return result;
        }
        result.text = answer_(request.text);
        return result;
    }
    void answer(std::function<std::string(const std::string&)> fn) {
        std::lock_guard lock(mutex_);
        answer_ = std::move(fn);
    }
    std::vector<Call> calls() const {
        std::lock_guard lock(mutex_);
        return calls_;
    }
    std::vector<Json> availability_checks() const {
        std::lock_guard lock(mutex_);
        return availability_checks_;
    }

    bool available = true;
    // Every pass fails with it and hands its text back.
    std::optional<std::string> error;
    // Runs inside each pass, before it answers (a Cancel pressed during cleanup).
    std::function<void()> during;

private:
    mutable std::mutex mutex_;
    std::function<std::string(const std::string&)> answer_;
    std::vector<Call> calls_;
    std::vector<Json> availability_checks_;
};

// The dictionary and snippets as DictionaryTextPreparer applies them, recording each
// schedule_learning (dictionary.schedule_learning monkeypatched). Affinity: thread-safe.
class RecordingPreparer final : public TextPreparer {
public:
    struct Learning {
        JobMode mode;
        std::string pasted;
        std::optional<std::string> before_cleanup;
        bool operator==(const Learning&) const = default;
    };

    std::string apply_replacements(std::string_view text, const Json& settings) const override {
        return real_.apply_replacements(text, settings);
    }
    SnippetPlan plan_expansion(std::string_view text, const Json& settings) const override {
        return real_.plan_expansion(text, settings);
    }
    Expansion expand(std::string_view text, const SnippetPlan& plan) const override {
        return real_.expand(text, plan);
    }
    void schedule_learning(const DictationJob& job, std::string_view pasted_text,
                           std::optional<std::string_view> before_cleanup) const override {
        if (fail) {
            throw std::runtime_error("learning broke");
        }
        std::lock_guard lock(mutex_);
        learned_.push_back(
            {job.mode, std::string(pasted_text),
             before_cleanup ? std::optional<std::string>(*before_cleanup) : std::nullopt});
    }
    std::vector<Learning> learned() const {
        std::lock_guard lock(mutex_);
        return learned_;
    }

    bool fail = false;

private:
    DictionaryTextPreparer real_;
    mutable std::mutex mutex_;
    mutable std::vector<Learning> learned_;
};

inline focus::AppIdentity app(std::string app_id, std::string name, std::uint32_t pid = 0,
                              std::string window = {}, std::string title_hint = {}) {
    focus::AppIdentity identity;
    identity.app_id = std::move(app_id);
    identity.name = std::move(name);
    if (pid != 0) {
        identity.pid = pid;
    }
    identity.window = std::move(window);
    identity.title_hint = std::move(title_hint);
    return identity;
}

inline focus::TextContext caret(std::string before, std::string after = {},
                                bool caret_known = true) {
    focus::TextContext context;
    context.before = std::move(before);
    context.after = std::move(after);
    context.caret_known = caret_known;
    context.selection_known = true;
    context.source = "uia";
    return context;
}

// A job whose focus capture resolved to (identity, text); `captured` false is a job without a
// capture (app awareness off).
inline DictationJob job_in(std::optional<focus::AppIdentity> identity,
                           std::optional<focus::TextContext> text = std::nullopt,
                           JobMode mode = JobMode::Dictation, bool captured = true) {
    DictationJob job;
    job.mode = mode;
    if (captured) {
        job.focus = focus::FocusRequest::resolved(
            focus::FocusSnapshot{std::move(identity), std::move(text)});
    }
    return job;
}

inline std::shared_ptr<const DictationJob> shared(DictationJob job) {
    return std::make_shared<const DictationJob>(std::move(job));
}

// The settings value of a snippet, as Snippet.to_dict writes it.
inline Json snippet_json(std::string id, std::string trigger, std::string text,
                         bool formatted = false) {
    return Json{{"id", std::move(id)},
                {"trigger", std::move(trigger)},
                {"text", std::move(text)},
                {"formatted", formatted}};
}

// Keys from an in-memory store and a map environment holding an OpenRouter and an OpenAI key;
// no .env. Nothing here reads the owner's keys.
struct CleanupKeys {
    std::shared_ptr<MemoryCredentialBackend> backend = std::make_shared<MemoryCredentialBackend>();
    CredentialStore store{backend, identity()};
    MapEnvReader env{{"OPENROUTER_API_KEY", "test-openrouter-key"},
                     {"OPENAI_API_KEY", "test-openai-key"}};
    DotEnv dotenv{std::filesystem::path("fv-dictation-tests-no-such-dir") / ".env"};
    llm::KeySources sources{store, env, dotenv};
};

// The TranscriptCleanup the composition builds (deferred client), over `http`, reading the
// settings of `settings`, on `clock` (the system clock when null).
template <class Store>
std::shared_ptr<llm::TranscriptCleanup> make_cleanup(std::shared_ptr<http::Client> http,
                                                     CleanupKeys& keys, Store& settings,
                                                     const Clock* clock = nullptr) {
    llm::CleanupOptions options;
    options.clock = clock;
    options.default_prompt = text::legacy_default_prompt();
    return std::make_shared<llm::TranscriptCleanup>(
        std::move(http), keys.sources, [&settings] { return *settings.snapshot(); }, "", "", "",
        true, std::move(options));
}

// A chat endpoint that answers `text` to every request.
inline http::FakeHttpClient::Handler chat_answer(std::string text) {
    return [text = std::move(text)](const http::Request&) {
        http::ScriptedResponse reply;
        reply.status = 200;
        reply.reason = "OK";
        reply.headers = {{"Content-Type", "application/json"}};
        reply.body = py::dumps(Json{
            {"choices", Json::array({Json{{"message", {{"role", "assistant"}, {"content", text}}},
                                          {"finish_reason", "stop"}}})}});
        return reply;
    };
}

// The chat messages a recorded request sent.
inline Json sent_messages(const http::Request& request) {
    const auto* body = std::get_if<std::string>(&request.body);
    if (body == nullptr) {
        return Json::array();
    }
    return py::loads(*body).value("messages", Json::array());
}

// A provider that takes the request and never answers until its cancel or release().
// Affinity: thread-safe.
class StallingClient final : public http::Client {
public:
    std::expected<http::Response, http::Error> send(const http::Request&, const CancelToken& cancel,
                                                    http::BodySink*) override {
        {
            std::lock_guard lock(mutex_);
            ++started_;
        }
        changed_.notify_all();
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, std::chrono::seconds(10),
                          [&] { return released_ || cancel.cancelled(); });
        return std::unexpected(http::Error{http::ErrorKind::Cancelled, "canceled"});
    }
    bool wait_started(std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return started_ > 0; });
    }
    void release() {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    int started_ = 0;
    bool released_ = false;
};

// The engine hears `text` from now on.
inline void say(speech::FakeEngine& engine, std::string text) {
    speech::FakeScript script = engine.script();
    script.texts = {std::move(text)};
    engine.set_script(std::move(script));
}

} // namespace fv::dictation::testing
