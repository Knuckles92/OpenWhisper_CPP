#pragma once

// What the custom-report and store-tool-host tests share: OpenWhisper's test meeting
// (tests/test_meeting_custom_report.py), a store of the ended meeting, a repository serving it,
// and a report harness whose text endpoint is an SseProvider on 127.0.0.1 answering with
// canned Chat Completions replies. Nothing leaves the loopback interface.

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../llm/sse_provider.h"
#include "../meeting_state/stub_repository.h"
#include "base/env.h"
#include "base/identity.h"
#include "base/json.h"
#include "base/testing/fake_clock.h"
#include "base/testing/map_env_reader.h"
#include "credentials/dotenv.h"
#include "credentials/store.h"
#include "credentials/testing/memory_backend.h"
#include "http/client.h"
#include "intel_support.h"
#include "llm/credentials.h"
#include "meeting_intel/custom_report.h"
#include "meeting_state/schema.h"
#include "meeting_state/store.h"
#include "meeting_state/testing/fixed_ids.h"
#include "pyport/json.h"
#include "testkit/testkit.h"

namespace fv::meeting_intel::test {

inline constexpr std::string_view kPastId = "m_past";

inline Json report_segment(int index, double start_s, const std::string& text,
                           const std::string& channel = "mic") {
    return Json{{"id", std::format("sg_{}", index)},
                {"meeting_id", kPastId},
                {"chunk_id", 1},
                {"channel", channel},
                {"start_s", start_s},
                {"end_s", start_s + 4.0},
                {"text", text},
                {"speaker_participant_id", nullptr},
                {"speaker_source", "channel"},
                {"speaker_pinned", false}};
}

// SEGMENTS of the Python tests.
inline const std::vector<Json>& report_segments() {
    static const std::vector<Json> rows = {
        report_segment(1, 0, "Lets talk about the vendor renewal and pricing."),
        report_segment(2, 6, "We cannot go below a twelve percent discount.", "loopback"),
        report_segment(3, 14, "Priya will draft the renewal note by Friday."),
        report_segment(4, 24, "Agreed, we hold at twelve and revisit the terms in Q3."),
    };
    return rows;
}

// MEETING of the Python tests.
inline const Json& report_meeting() {
    static const Json row = {{"id", kPastId},
                             {"title", "Vendor renewal"},
                             {"started_at", "2026-09-19T10:00:00Z"},
                             {"cloud_enabled", true}};
    return row;
}

// _ended_state(): an ended meeting whose finalization completed.
inline meeting_state::MeetingState ended_state() {
    meeting_state::MeetingState state;
    state.meeting_id = std::string(kPastId);
    state.title = "Vendor renewal";
    state.status = "ended";
    state.cloud_enabled = true;
    state.finalization =
        meeting_state::FinalizationState::coerce(Json{{"status", "completed"}}, true, "ended");
    return state;
}

// A store with deterministic ids and its clock, which must outlive it.
struct StoreFixture {
    FakeClock clock;
    meeting_state::testing::FixedIds ids;
    std::shared_ptr<meeting_state::MeetingStateStore> store;

    explicit StoreFixture(meeting_state::MeetingState state = ended_state(),
                          std::shared_ptr<meeting_state::MeetingRepository> repository = nullptr)
        : store(std::make_shared<meeting_state::MeetingStateStore>(
              std::move(state), repository, meeting_state::SegmentHandler{}, clock, ids)) {}

    // _request(store, actor, **overrides).
    meeting_state::OpResult request(const std::string& actor = "host",
                                    const std::string& run_id = "run_1") {
        return store
            ->apply(actor, std::nullopt,
                    Json::array({Json{{"op", "request_custom_report"},
                                      {"request", "the commitments"},
                                      {"run_id", run_id}}}))
            .at(0);
    }
    Json reports() const { return store->snapshot().at("custom_reports"); }
};

// TestLifecycle.Repo: the meeting row and SEGMENTS for any id, and segment_exists from a set.
class ReportRepository final : public meeting_state::test_support::StubRepository {
public:
    std::vector<std::string> known_segments;
    bool fail_segment_checks = false;
    std::vector<std::pair<std::string, std::string>> segment_checks;

    std::optional<Json> get_meeting(std::string_view meeting_id) override {
        Json row = report_meeting();
        row["id"] = std::string(meeting_id);
        return row;
    }
    std::vector<Json> get_segments(std::string_view, double, std::optional<std::int64_t>) override {
        return report_segments();
    }
    bool segment_exists(std::string_view meeting_id, std::string_view segment_id) override {
        segment_checks.emplace_back(meeting_id, segment_id);
        if (fail_segment_checks) {
            throw std::runtime_error("database is locked");
        }
        return std::ranges::find(known_segments, segment_id) != known_segments.end();
    }
};

// Runs every posted task at once, on the caller's thread (Python's run_in_thread=False).
class InlineExecutor final : public Executor {
public:
    void post(Task task) override { task(); }
    TimerHandle post_after(std::chrono::milliseconds, Task task) override {
        task();
        return {};
    }
    bool is_current() const override { return true; }
};

// One file of tests/fixtures/llm/reports: a list of Chat Completions replies.
inline Json report_fixture(std::string_view name) {
    const auto path = testkit::source_root() / "tests" / "fixtures" / "llm" / "reports" /
                      base_utf8::path_from_utf8(std::string(name) + ".json");
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("missing fixture " + std::string(name));
    }
    std::ostringstream text;
    text << in.rdbuf();
    return py::loads(text.str());
}

// A Chat Completions reply: the text, the tool calls ({id, name, arguments}), why it stopped
// and its usage (null for none).
inline Json chat_reply(const std::string& text, const Json& calls = Json::array(),
                       const std::string& finish_reason = "stop",
                       const Json& usage = Json{
                           {"prompt_tokens", 10}, {"completion_tokens", 5}, {"total_tokens", 15}}) {
    Json message = Json{{"role", "assistant"}, {"content", text}};
    if (!calls.empty()) {
        Json tool_calls = Json::array();
        for (const auto& call : calls) {
            tool_calls.push_back(Json{{"id", call.at("id")},
                                      {"type", "function"},
                                      {"function", Json{{"name", call.at("name")},
                                                        {"arguments", call.at("arguments")}}}});
        }
        message["tool_calls"] = tool_calls;
    }
    Json reply =
        Json{{"id", "chatcmpl-report"},
             {"object", "chat.completion"},
             {"model", "test-model"},
             {"choices",
              Json::array(
                  {Json{{"index", 0}, {"message", message}, {"finish_reason", finish_reason}}})}};
    if (!usage.is_null()) {
        reply["usage"] = usage;
    }
    return reply;
}

inline Json tool_call(const std::string& id, const std::string& name,
                      const std::string& arguments) {
    return Json{{"id", id}, {"name", name}, {"arguments", arguments}};
}

// generate_report's surroundings: a custom text endpoint on a loopback SseProvider, keys that
// need none, settings with the consent switches, and recall over a stub history.
struct ReportHarness {
    testing::SseProvider provider;
    ReportRepository repository;
    std::shared_ptr<MemoryCredentialBackend> backend = std::make_shared<MemoryCredentialBackend>();
    CredentialStore credentials{backend, identity()};
    MapEnvReader env;
    DotEnv dotenv{std::filesystem::path("fv-meeting-intel-tests-no-such-dir") / ".env"};
    Json settings = Json::object();
    StubHistory history;
    RecallDeps recall{&history, [this] { return settings; }, nullptr, nullptr};
    ReportDeps deps;

    explicit ReportHarness(std::vector<testing::SseReply> replies = {}, bool with_recall = false)
        : provider(std::move(replies)), deps{.repository = repository,
                                             .http = direct_client(),
                                             .keys = llm::KeySources{credentials, env, dotenv},
                                             .settings = [this] { return settings; },
                                             .recall = with_recall ? &recall : nullptr} {}

    ReportHarness(const ReportHarness&) = delete;
    ReportHarness& operator=(const ReportHarness&) = delete;

    static std::shared_ptr<http::Client> direct_client() {
        http::CurlClientOptions options;
        options.proxy = [](std::string_view) {
            return std::optional<std::string>();
        };
        return http::make_curl_client(std::move(options));
    }

    // The consent switches generate_report reads.
    void consent(bool past_recall, bool context_files) {
        settings["meeting_past_recall_enabled"] = past_recall;
        settings["meeting_context_folder_enabled"] = context_files;
    }

    // The meeting's endpoint snapshot: a custom endpoint at the provider that needs no key.
    Json endpoint(const Json& model_metadata = Json()) const {
        Json snapshot = Json{{"profile_id", "custom_report"},
                             {"name", "Report test"},
                             {"kind", "custom"},
                             {"base_url", provider.url("/v1")},
                             {"api_key_env", ""}};
        if (!model_metadata.is_null()) {
            snapshot["model_metadata"] = model_metadata;
        }
        return snapshot;
    }
    ReportRoute route(const std::string& model = "test-model") const {
        return ReportRoute{"custom_report", model, endpoint()};
    }

    // generate_report over the Python tests' meeting.
    Json run(std::string_view request = "every commitment") {
        return generate_report(report_meeting(), ended_state().to_json(), report_segments(),
                               request, route(), deps);
    }

    // The request bodies the provider saw.
    std::vector<Json> bodies() const {
        std::vector<Json> out;
        for (const auto& request : provider.requests()) {
            out.push_back(request.json());
        }
        return out;
    }
};

// The ReportUnavailable sentence `body` throws, or a marker when it throws none.
template <class Body> std::string unavailable(Body body) {
    try {
        body();
    } catch (const ReportUnavailable& error) {
        return error.what();
    }
    return "<no ReportUnavailable>";
}

} // namespace fv::meeting_intel::test
