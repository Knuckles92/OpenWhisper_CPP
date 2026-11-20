#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "automation/event_bus.h"
#include "automation/hook_http.h"
#include "automation/hooks_model.h"
#include "automation/secrets.h"
#include "automation/signing.h"
#include "automation/testing/fake_hook_engines.h"
#include "automation/webhooks.h"
#include "base/identity.h"
#include "base/testing/fake_clock.h"
#include "credentials/testing/memory_backend.h"
#include "http/testing/fake_http_client.h"
#include "http/user_agent.h"
#include "pyport/text.h"
#include "settings/testing/memory_store.h"
#include "support/hook_support.h"
#include "support/loopback_receiver.h"

// tests/test_automation_hooks_webhooks.py, test_automation_fix_hooks_webhook_lanes.py and
// test_automation_fix_hooks_webhook_secret.py: delivery against a real HTTP server on
// 127.0.0.1. Saved webhooks come from an in-memory settings store, events from a fresh bus,
// secrets from an in-memory credential store; the receiver is a real socket server, so signing,
// ordering, retries and redirects are checked on the wire.

using namespace fv::automation;
using fv::Json;
using test::Answer;
using test::Gate;
using test::ReceivedRequest;
using test::Receiver;
using test::wait_until;
using namespace std::chrono_literals;

namespace {

WebhookOptions fast() {
    WebhookOptions options;
    options.retry_delays_s = {0.05, 0.1, 0.15};
    return options;
}

Json hook(const std::string& id, const std::string& url, const Json& extra = Json::object()) {
    Json row = {{"id", id}, {"name", "Hook " + id}, {"url", url}};
    for (const auto& [key, value] : extra.items()) {
        row[key] = value;
    }
    return row;
}

// A backend the store cannot use, as a missing or locked keyring.
class BrokenBackend final : public fv::MemoryCredentialBackend {
public:
    std::optional<std::string> get(std::string_view, std::string_view) override {
        throw fv::CredentialStoreError("no keyring here");
    }
    void set(std::string_view, std::string_view, std::string_view) override {
        throw fv::CredentialStoreError("no keyring here");
    }
    bool remove(std::string_view, std::string_view) override {
        throw fv::CredentialStoreError("no keyring here");
    }
    fv::CredentialStatus probe() override { return {false, "broken", "no keyring here"}; }
};

signing::Received received(const ReceivedRequest& request) {
    return {request.headers.begin(), request.headers.end()};
}

struct Hooks {
    explicit Hooks(WebhookOptions options = fast(),
                   std::shared_ptr<fv::CredentialBackend> backend =
                       std::make_shared<fv::MemoryCredentialBackend>(),
                   std::shared_ptr<fv::http::Client> client = test::loopback_client())
        : credentials(std::move(backend), fv::identity()), http(std::move(client)),
          engine(std::make_unique<WebhookEngine>(settings, credentials, http, bus,
                                                 std::move(options))) {}
    ~Hooks() {
        engine.reset();
        bus.close();
        receiver.close();
    }
    Hooks(const Hooks&) = delete;
    Hooks& operator=(const Hooks&) = delete;

    // save_hooks: each row enabled and following recording.started unless it says otherwise.
    void save(const std::vector<Json>& hooks) {
        Json rows = Json::array();
        for (const Json& hook : hooks) {
            Json row = {{"enabled", true}, {"events", {"recording.started"}}};
            for (const auto& [key, value] : hook.items()) {
                row[key] = value;
            }
            rows.push_back(row);
        }
        settings.update(Json{{"automation_webhooks", rows}});
    }
    Event publish(const std::string& type, Json data) {
        return bus.publish_event(type, std::move(data));
    }
    std::size_t records(const std::string& id) const { return engine->deliveries(id).size(); }
    DeliveryRecord send_test(const std::string& id) {
        auto answer = std::make_shared<std::promise<DeliveryRecord>>();
        auto future = answer->get_future();
        engine->send_test(id, [answer](DeliveryRecord record) { answer->set_value(record); });
        REQUIRE(future.wait_for(10s) == std::future_status::ready);
        return future.get();
    }
    std::string secret_stored(const std::string& id) {
        return credentials.get(hooks::webhook_secret_name(id)).value_or("");
    }

    Receiver receiver;
    fv::MemorySettingsStore settings;
    fv::CredentialStore credentials;
    EventBus bus;
    std::shared_ptr<fv::http::Client> http;
    std::unique_ptr<WebhookEngine> engine;
};

std::vector<std::string> ids_of(const std::vector<ReceivedRequest>& requests) {
    std::vector<std::string> ids;
    for (const auto& request : requests) {
        ids.push_back(request.json()["id"].get<std::string>());
    }
    return ids;
}

std::vector<std::string> types_of(const std::vector<ReceivedRequest>& requests) {
    std::vector<std::string> types;
    for (const auto& request : requests) {
        types.push_back(request.json()["type"].get<std::string>());
    }
    return types;
}

std::size_t dropped_count(const std::vector<DeliveryRecord>& records) {
    return static_cast<std::size_t>(std::ranges::count_if(records, [](const auto& record) {
        if (record.error.find("Dropped") == std::string::npos) {
            return false;
        }
        CHECK(record.error == "Dropped: too many deliveries were waiting."); // A19-38
        return true;
    }));
}

} // namespace

// -- delivery ------------------------------------------------------------------------------------

TEST_CASE("test_delivers_signed_wire_json_in_order", "[test_automation_hooks_webhooks][platform]") {
    Hooks f;
    f.save({hook("a", f.receiver.url("/a"))});
    f.engine->start();
    std::vector<Event> published;
    for (int i = 0; i < 5; ++i) {
        published.push_back(f.publish("recording.started", {{"job_id", std::format("job{}", i)}}));
    }

    const auto got = f.receiver.wait_for(5, "/a");
    const std::string secret = f.engine->ensure_secret("a");
    REQUIRE(got.size() == 5);
    for (std::size_t i = 0; i < got.size(); ++i) {
        CHECK(got[i].json() == published[i].to_dict());
        CHECK(signing::verify(secret, received(got[i]), got[i].body));
        CHECK(got[i].header("webhook-id") == published[i].id);
        CHECK(got[i].header("content-type") == "application/json");
        CHECK(got[i].header("user-agent") == fv::http::user_agent());
    }
    REQUIRE(wait_until([&] { return f.records("a") == 5; }));
    const auto records = f.engine->deliveries("a");
    for (std::size_t i = 0; i < records.size(); ++i) {
        CHECK(records[i].event_id == published[published.size() - 1 - i].id);
        CHECK((records[i].ok && records[i].status == 200 && records[i].attempt == 1 &&
               records[i].error.empty()));
    }
}

TEST_CASE("test_only_enabled_webhooks_get_the_types_they_chose",
          "[test_automation_hooks_webhooks][platform]") {
    Hooks f;
    f.save(
        {hook("a", f.receiver.url("/a"), {{"events", {"recording.stopped"}}}),
         hook("b", f.receiver.url("/b"), {{"events", {"recording.started", "recording.stopped"}}}),
         hook("off", f.receiver.url("/off"), {{"enabled", false}})});
    f.engine->start();
    CHECK(f.bus.subscribers("webhooks") == 1);

    f.publish("recording.started", {{"job_id", "1"}});
    f.publish("recording.canceled", {{"job_id", "1"}});
    f.publish("recording.stopped", {{"job_id", "1"}, {"duration_s", 1.0}});

    CHECK(types_of(f.receiver.wait_for(2, "/b")) ==
          std::vector<std::string>{"recording.started", "recording.stopped"});
    CHECK(types_of(f.receiver.wait_for(1, "/a")) == std::vector<std::string>{"recording.stopped"});
    test::sleep_ms(200);
    CHECK(f.receiver.at("/off").empty());
    CHECK(f.receiver.at("/a").size() == 1);
}

TEST_CASE("test_start_is_idempotent_and_stop_unsubscribes",
          "[test_automation_hooks_webhooks][platform]") {
    Hooks f;
    f.save({hook("a", f.receiver.url("/a"))});
    f.engine->start();
    f.engine->start();
    CHECK(f.bus.subscribers("webhooks") == 1);
    f.engine->stop();
    f.engine->stop();
    CHECK(f.bus.subscribers("webhooks") == 0);
    f.publish("recording.started", {{"job_id", "1"}});
    test::sleep_ms(200);
    CHECK(f.receiver.requests().empty());
}

TEST_CASE("test_no_webhooks_means_no_subscription", "[test_automation_hooks_webhooks][platform]") {
    Hooks f;
    f.save({});
    f.engine->start();
    CHECK(f.bus.subscribers("webhooks") == 0);
}

TEST_CASE("test_reload_follows_saved_changes", "[test_automation_hooks_webhooks][platform]") {
    Hooks f;
    f.save({hook("a", f.receiver.url("/one"))});
    f.engine->start();
    f.publish("recording.started", {{"job_id", "1"}});
    f.receiver.wait_for(1, "/one");

    f.save({hook("a", f.receiver.url("/two"), {{"events", {"recording.canceled"}}})});
    f.engine->reload();
    f.publish("recording.started", {{"job_id", "2"}});
    f.publish("recording.canceled", {{"job_id", "2"}});
    CHECK(types_of(f.receiver.wait_for(1, "/two")) ==
          std::vector<std::string>{"recording.canceled"});

    f.save({hook("a", f.receiver.url("/two"), {{"enabled", false}})});
    f.engine->reload();
    CHECK(f.bus.subscribers("webhooks") == 0);
    f.publish("recording.canceled", {{"job_id", "3"}});
    test::sleep_ms(200);
    CHECK(f.receiver.at("/two").size() == 1);
    CHECK(f.receiver.at("/one").size() == 1);
}

TEST_CASE("test_reload_while_publishing_neither_loses_nor_repeats_events",
          "[test_automation_hooks_webhooks][platform]") {
    Hooks f;
    f.save({hook("a", f.receiver.url("/a"))});
    f.engine->start();
    constexpr int count = 150;

    std::thread publisher([&] {
        for (int i = 0; i < count; ++i) {
            f.publish("recording.started", {{"n", i}});
        }
    });
    for (int flip = 0; flip < 30; ++flip) {
        // Changing the types replaces the subscription each time.
        Json types = {"recording.started"};
        if (flip % 2 != 0) {
            types.push_back("recording.stopped");
        }
        f.save({hook("a", f.receiver.url("/a"), {{"events", types}})});
        f.engine->reload();
    }
    publisher.join();

    const auto got = f.receiver.wait_for(count, "/a", 30s);
    test::sleep_ms(300);
    std::vector<int> numbers;
    for (const auto& request : f.receiver.at("/a")) {
        numbers.push_back(request.json()["data"]["n"].get<int>());
    }
    std::vector<int> expected(count);
    for (int i = 0; i < count; ++i) {
        expected[static_cast<std::size_t>(i)] = i;
    }
    CHECK(numbers == expected);
    CHECK(got.size() == static_cast<std::size_t>(count));
}

// -- retries
// ---------------------------------------------------------------------------------------

TEST_CASE("test_5xx_is_retried_with_backoff_and_keeps_the_webhooks_order",
          "[test_automation_hooks_webhooks][platform]") {
    Hooks f;
    f.receiver.answer("/a", {Answer{.status = 500}, Answer{.status = 503}, Answer{.status = 200}});
    f.save({hook("a", f.receiver.url("/a"))});
    f.engine->start();
    const Event first = f.publish("recording.started", {{"job_id", "1"}});
    const Event second = f.publish("recording.started", {{"job_id", "2"}});

    const auto got = f.receiver.wait_for(4, "/a");
    CHECK(ids_of(got) == std::vector<std::string>{first.id, first.id, first.id, second.id});
    CHECK(test::seconds_between(got[0].at, got[1].at) >= 0.05);
    CHECK(test::seconds_between(got[1].at, got[2].at) >= 0.1);
    REQUIRE(wait_until([&] { return f.records("a") == 4; }));
    auto records = f.engine->deliveries("a");
    std::ranges::reverse(records);
    CHECK((records[0].event_id == first.id && records[0].attempt == 1 && records[0].status == 500 &&
           !records[0].ok));
    CHECK((records[1].event_id == first.id && records[1].attempt == 2 && records[1].status == 503 &&
           !records[1].ok));
    CHECK((records[2].event_id == first.id && records[2].attempt == 3 && records[2].status == 200 &&
           records[2].ok));
    CHECK((records[3].event_id == second.id && records[3].attempt == 1 &&
           records[3].status == 200 && records[3].ok));
}

TEST_CASE("test_gives_up_after_three_retries", "[test_automation_hooks_webhooks][platform]") {
    Hooks f;
    f.receiver.answer("/a", {Answer{.status = 502}});
    f.save({hook("a", f.receiver.url("/a"))});
    f.engine->start();
    f.publish("recording.started", {{"job_id", "1"}});

    f.receiver.wait_for(4, "/a");
    test::sleep_ms(400);
    CHECK(f.receiver.at("/a").size() == 4);
    REQUIRE(wait_until([&] { return f.records("a") == 4; }));
    const auto newest = f.engine->deliveries("a").front();
    CHECK(newest.attempt == 4);
    CHECK(newest.status == 502);
    CHECK_FALSE(newest.ok);
    CHECK(newest.error == "HTTP 502");
}

TEST_CASE("test_408_and_429_are_retried", "[test_automation_hooks_webhooks][platform]") {
    const int status = GENERATE(408, 429);
    Hooks f;
    f.receiver.answer("/a", {Answer{.status = status}, Answer{.status = 200}});
    f.save({hook("a", f.receiver.url("/a"))});
    f.engine->start();
    f.publish("recording.started", {{"job_id", "1"}});
    CHECK(f.receiver.wait_for(2, "/a").size() == 2);
}

TEST_CASE("test_other_4xx_is_never_retried", "[test_automation_hooks_webhooks][platform]") {
    const int status = GENERATE(400, 401, 404, 410, 422);
    Hooks f;
    f.receiver.answer("/a", {Answer{.status = status}, Answer{.status = 200}});
    f.save({hook("a", f.receiver.url("/a"))});
    f.engine->start();
    f.publish("recording.started", {{"job_id", "1"}});
    f.receiver.wait_for(1, "/a");
    test::sleep_ms(400);
    CHECK(f.receiver.at("/a").size() == 1);
    REQUIRE(wait_until([&] { return f.records("a") == 1; }));
    const auto record = f.engine->deliveries("a").front();
    CHECK_FALSE(record.ok);
    CHECK(record.status == status);
    CHECK(record.error == std::format("HTTP {}", status));
}

TEST_CASE("test_retry_after_is_honoured_up_to_the_cap",
          "[test_automation_hooks_webhooks][platform]") {
    WebhookOptions options = fast();
    options.retry_after_max_s = 0.6;
    Hooks f(options);
    f.receiver.answer(
        "/a", {Answer{.status = 429, .headers = {{"Retry-After", "120"}}}, Answer{.status = 200}});
    f.save({hook("a", f.receiver.url("/a"))});
    f.engine->start();
    f.publish("recording.started", {{"job_id", "1"}});

    const auto got = f.receiver.wait_for(2, "/a");
    const double waited = test::seconds_between(got[0].at, got[1].at);
    CHECK(waited >= 0.55);
    CHECK(waited < 5);
}

TEST_CASE("test_retry_after_parses_seconds_and_dates",
          "[test_automation_hooks_webhooks][platform]") {
    const auto now = std::chrono::system_clock::now();
    CHECK(hook_http::retry_after("3", now) == 3.0);
    CHECK(hook_http::retry_after(" 0 ", now) == 0.0);
    CHECK(hook_http::retry_after("", now) == std::nullopt);
    CHECK(hook_http::retry_after(std::nullopt, now) == std::nullopt);
    CHECK(hook_http::retry_after("soon", now) == std::nullopt);
    CHECK(hook_http::retry_after("Wed, 21 Oct 2015 07:28:00 GMT", now) == 0.0);
    const auto later = std::chrono::floor<std::chrono::seconds>(now + 30s);
    const auto when =
        hook_http::retry_after(std::format("{:%a, %d %b %Y %H:%M:%S} GMT", later), now);
    REQUIRE(when);
    CHECK(*when >= 25);
    CHECK(*when <= 31);
}

TEST_CASE("test_a_redirect_is_a_failure_and_is_not_followed",
          "[test_automation_hooks_webhooks][platform]") {
    Hooks f;
    f.receiver.answer(
        "/a", {Answer{.status = 307, .headers = {{"Location", f.receiver.url("/elsewhere")}}}});
    f.save({hook("a", f.receiver.url("/a"))});
    f.engine->start();
    f.publish("recording.started", {{"job_id", "1"}});

    f.receiver.wait_for(1, "/a");
    test::sleep_ms(400);
    CHECK(f.receiver.at("/elsewhere").empty());
    CHECK(f.receiver.at("/a").size() == 1);
    REQUIRE(wait_until([&] { return f.records("a") == 1; }));
    const auto record = f.engine->deliveries("a").front();
    CHECK_FALSE(record.ok);
    CHECK(record.status == 307);
    CHECK(record.error.find("redirect") != std::string::npos);
}

TEST_CASE("test_network_errors_are_retried", "[test_automation_hooks_webhooks][platform]") {
    test::HangUpServer hang_up;
    Hooks f;
    f.save({hook("a", hang_up.url("/a"))});
    f.engine->start();
    f.publish("recording.started", {{"job_id", "1"}});

    REQUIRE(wait_until([&] { return f.records("a") == 4; }));
    test::sleep_ms(300);
    const auto records = f.engine->deliveries("a");
    std::vector<int> attempts;
    for (const auto& record : records) {
        attempts.push_back(record.attempt);
        CHECK_FALSE(record.status);
        // A reset socket or a missing answer, whichever the platform reports.
        CHECK((record.error == "The server closed the connection" ||
               record.error == "The connection broke"));
    }
    CHECK(attempts == std::vector<int>{4, 3, 2, 1});
    CHECK(hang_up.accepted() == 4);
}

TEST_CASE("test_a_waiting_retry_does_not_hold_up_other_webhooks",
          "[test_automation_hooks_webhooks][platform]") {
    WebhookOptions options;
    options.retry_delays_s = {3.0, 3.0, 3.0};
    Hooks f(options);
    f.receiver.answer("/failing", {Answer{.status = 500}});
    f.save({hook("a", f.receiver.url("/failing")), hook("b", f.receiver.url("/fine"))});
    f.engine->start();
    f.publish("recording.started", {{"job_id", "1"}});
    f.publish("recording.started", {{"job_id", "2"}});

    const auto started = std::chrono::steady_clock::now();
    f.receiver.wait_for(2, "/fine", 2500ms);
    CHECK(test::seconds_since(started) < 2.5);
    // The failing webhook's second event waits behind its first one's retry (each webhook has
    // its own lane, so its first answer may come later).
    f.receiver.wait_for(1, "/failing", 2500ms);
    CHECK(f.receiver.at("/failing").size() == 1);
}

TEST_CASE("test_a_full_queue_drops_the_oldest_waiting_delivery",
          "[test_automation_hooks_webhooks][platform]") {
    WebhookOptions options = fast();
    options.max_queued = 3;
    Hooks f(options);
    auto release = std::make_shared<Gate>();
    f.receiver.answer("/a", {Answer{.hold = release}, Answer{.status = 200}});
    f.save({hook("a", f.receiver.url("/a"))});
    f.engine->start();
    const Event first = f.publish("recording.started", {{"n", 0}});
    REQUIRE(wait_until([&] { return f.receiver.arrivals() == std::vector<std::string>{"/a"}; }));
    std::vector<Event> later;
    for (int i = 1; i < 7; ++i) {
        later.push_back(f.publish("recording.started", {{"n", i}}));
    }
    REQUIRE(wait_until([&] { return dropped_count(f.engine->deliveries("a")) >= 3; }));
    release->set();

    const auto got = f.receiver.wait_for(4, "/a");
    CHECK(ids_of(got) == std::vector<std::string>{first.id, later[3].id, later[4].id, later[5].id});
    std::vector<std::string> dropped;
    for (const auto& record : f.engine->deliveries("a")) {
        if (record.error.find("Dropped") != std::string::npos) {
            dropped.push_back(record.event_id);
        }
    }
    std::ranges::sort(dropped);
    std::vector<std::string> expected{later[0].id, later[1].id, later[2].id};
    std::ranges::sort(expected);
    CHECK(dropped == expected);
}

TEST_CASE("test_stop_returns_quickly_with_a_delivery_in_flight",
          "[test_automation_hooks_webhooks][platform]") {
    Hooks f;
    f.receiver.answer("/a", {Answer{.delay = 4s}});
    // Its own id: the attempt in flight still records its outcome later.
    f.save({hook("slow", f.receiver.url("/a"))});
    f.engine->start();
    f.publish("recording.started", {{"job_id", "1"}});
    REQUIRE(wait_until([&] { return f.receiver.arrivals() == std::vector<std::string>{"/a"}; }));
    const auto started = std::chrono::steady_clock::now();
    f.engine->stop();
    CHECK(test::seconds_since(started) < 1.5);
}

// The retry schedule itself, on a clock that moves only when told: 1, 5 and 25 s, and a
// Retry-After honoured up to 60 s.
TEST_CASE("retries wait 1, 5 and 25 seconds on the engine's clock", "[webhooks][platform]") {
    fv::FakeClock clock;
    WebhookOptions options;
    options.clock = &clock;
    Hooks f(options);
    f.receiver.answer("/a", {Answer{.status = 500}, Answer{.status = 503}, Answer{.status = 504},
                             Answer{.status = 200}});
    f.save({hook("a", f.receiver.url("/a"))});
    f.engine->start();
    f.publish("recording.started", {{"job_id", "1"}});

    const std::vector<double> waits{1.0, 5.0, 25.0};
    for (std::size_t attempt = 1; attempt <= waits.size(); ++attempt) {
        // The failed attempt is recorded and waits at the head of its lane.
        REQUIRE(
            wait_until([&] { return f.records("a") == attempt && f.engine->queued("a") == 1; }));
        clock.advance(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::duration<double>(waits[attempt - 1] - 0.1)));
        test::sleep_ms(150);
        CHECK(f.receiver.at("/a").size() == attempt);
        clock.advance(100ms);
        f.receiver.wait_for(attempt + 1, "/a");
    }
    REQUIRE(wait_until([&] { return f.records("a") == 4; }));
    CHECK(f.engine->deliveries("a").front().ok);
}

TEST_CASE("a Retry-After holds a retry back for at most 60 seconds", "[webhooks][platform]") {
    fv::FakeClock clock;
    WebhookOptions options;
    options.clock = &clock;
    Hooks f(options);
    f.receiver.answer(
        "/a", {Answer{.status = 429, .headers = {{"Retry-After", "120"}}}, Answer{.status = 200}});
    f.save({hook("a", f.receiver.url("/a"))});
    f.engine->start();
    f.publish("recording.started", {{"job_id", "1"}});

    REQUIRE(wait_until([&] { return f.records("a") == 1 && f.engine->queued("a") == 1; }));
    clock.advance(59s);
    test::sleep_ms(150);
    CHECK(f.receiver.at("/a").size() == 1);
    clock.advance(1s);
    CHECK(f.receiver.wait_for(2, "/a").size() == 2);
}

TEST_CASE("one attempt says how long to wait before the next", "[webhooks][platform]") {
    Hooks f(WebhookOptions{});
    f.save({hook("a", f.receiver.url("/a"))});
    const auto saved = hooks::find_webhook(*f.settings.snapshot(), "a");
    REQUIRE(saved);
    const Event event = synthetic_event("recording.started", {{"job_id", "1"}});

    f.receiver.answer("/a", {Answer{.status = 500}});
    for (int attempt = 1; attempt <= 3; ++attempt) {
        CHECK(f.engine->deliver(*saved, event, attempt).second ==
              std::vector<double>{1.0, 5.0, 25.0}[static_cast<std::size_t>(attempt - 1)]);
    }
    CHECK(f.engine->deliver(*saved, event, 4).second == std::nullopt);

    f.receiver.answer("/a", {Answer{.status = 503, .headers = {{"Retry-After", "7"}}}});
    CHECK(f.engine->deliver(*saved, event, 1).second == 7.0);
    CHECK(f.engine->deliver(*saved, event, 3).second == 25.0);
    f.receiver.answer("/a", {Answer{.status = 503, .headers = {{"Retry-After", "600"}}}});
    CHECK(f.engine->deliver(*saved, event, 1).second == 60.0);
    f.receiver.answer("/a", {Answer{.status = 400, .headers = {{"Retry-After", "7"}}}});
    CHECK(f.engine->deliver(*saved, event, 1).second == std::nullopt);
}

// -- proxies
// -----------------------------------------------------------------------------------------

TEST_CASE("test_private_addresses_never_go_through_a_proxy",
          "[test_automation_hooks_webhooks][platform]") {
    const std::string dead_proxy = std::format("http://127.0.0.1:{}", test::closed_port());
    auto client = fv::http::make_curl_client(fv::http::CurlClientOptions{
        .proxy = [dead_proxy](std::string_view) -> std::optional<std::string> {
            return dead_proxy;
        }});
    Hooks f(fast(), std::make_shared<fv::MemoryCredentialBackend>(), client);
    f.save({hook("a", f.receiver.url("/a"))});
    f.engine->start();
    f.publish("recording.started", {{"job_id", "1"}});
    CHECK(f.receiver.wait_for(1, "/a").size() == 1);
}

TEST_CASE("test_public_addresses_follow_the_environment",
          "[test_automation_hooks_webhooks][platform]") {
    fv::http::FakeHttpClient client;
    client.set_handler([](const fv::http::Request&) { return fv::http::ScriptedResponse{}; });
    const std::string secret = signing::new_secret();
    CHECK(post_signed(client, "https://hooks.example.com/x", Json::object(), secret).ok);
    CHECK(post_signed(client, "http://192.168.1.20:8080/hook", Json::object(), secret).ok);
    const auto requests = client.requests();
    REQUIRE(requests.size() == 2);
    CHECK_FALSE(requests[0].bypass_proxy);
    CHECK(requests[1].bypass_proxy);
    CHECK_FALSE(requests[0].follow_redirects);
    CHECK_FALSE(requests[1].follow_redirects);
    CHECK(hook_http::is_private("http://192.168.1.20:8080/hook"));
    CHECK(hook_http::is_private("http://localhost:9/x"));
    CHECK_FALSE(hook_http::is_private("https://hooks.example.com/x"));
}

// -- test sends
// ----------------------------------------------------------------------------------------

TEST_CASE("test_send_test_delivers_one_signed_test_event_even_when_disabled",
          "[test_automation_hooks_webhooks][platform]") {
    Hooks f;
    f.save({hook("a", f.receiver.url("/a"), {{"enabled", false}})});
    const DeliveryRecord record = f.send_test("a");

    CHECK(record.ok);
    CHECK(record.status == 200);
    CHECK(record.event_type == "webhook.test");
    const auto request = f.receiver.wait_for(1, "/a")[0];
    const Json body = request.json();
    CHECK(body["type"] == "webhook.test");
    CHECK(body["id"] == record.event_id);
    CHECK(body["data"]["webhook_id"] == "a");
    // A19-39: the test event's own words.
    CHECK(body["data"]["message"] ==
          std::format("This is a test delivery from {}.", fv::identity().product));
    CHECK(signing::verify(f.engine->ensure_secret("a"), received(request), request.body));
    CHECK(f.engine->deliveries("a") == std::vector<DeliveryRecord>{record});
}

TEST_CASE("test_send_test_does_not_retry", "[test_automation_hooks_webhooks][platform]") {
    Hooks f;
    f.receiver.answer("/a", {Answer{.status = 500}, Answer{.status = 200}});
    f.save({hook("a", f.receiver.url("/a"))});
    const DeliveryRecord record = f.send_test("a");
    test::sleep_ms(300);
    CHECK_FALSE(record.ok);
    CHECK(record.status == 500);
    CHECK(record.attempt == 1);
    CHECK(f.receiver.at("/a").size() == 1);
}

TEST_CASE("test_send_test_runs_off_the_callers_thread",
          "[test_automation_hooks_webhooks][platform]") {
    Hooks f;
    f.receiver.answer("/a", {Answer{.delay = 500ms}});
    f.save({hook("a", f.receiver.url("/a"))});
    auto answer = std::make_shared<std::promise<DeliveryRecord>>();
    auto future = answer->get_future();
    const auto started = std::chrono::steady_clock::now();
    f.engine->send_test("a", [answer](DeliveryRecord record) { answer->set_value(record); });
    CHECK(test::seconds_since(started) < 0.3);
    REQUIRE(future.wait_for(10s) == std::future_status::ready);
    CHECK(future.get().ok);
}

TEST_CASE("test_send_test_for_an_unsaved_webhook_says_so",
          "[test_automation_hooks_webhooks][platform]") {
    Hooks f;
    const DeliveryRecord record = f.send_test("missing");
    CHECK_FALSE(record.ok);
    CHECK(record.error == "Save the webhook first.");
}

// -- secrets
// -------------------------------------------------------------------------------------------

TEST_CASE("test_secrets_are_created_once_rotated_and_deleted",
          "[test_automation_hooks_webhooks][platform]") {
    Hooks f;
    const std::string first = f.engine->ensure_secret("a");
    CHECK(first.starts_with("whsec_"));
    CHECK(f.engine->ensure_secret("a") == first);
    CHECK(f.secret_stored("a") == first);
    const std::string rotated = f.engine->rotate_secret("a");
    CHECK(rotated != first);
    CHECK(f.engine->ensure_secret("a") == rotated);
    f.engine->delete_secret("a");
    CHECK_FALSE(f.credentials.get(hooks::webhook_secret_name("a")));
    f.engine->delete_secret("a"); // nothing stored: still quiet
}

TEST_CASE("test_deliveries_use_the_rotated_secret", "[test_automation_hooks_webhooks][platform]") {
    Hooks f;
    f.save({hook("a", f.receiver.url("/a"))});
    f.engine->start();
    const std::string old = f.engine->ensure_secret("a");
    const std::string fresh = f.engine->rotate_secret("a");
    f.publish("recording.started", {{"job_id", "1"}});
    const auto request = f.receiver.wait_for(1, "/a")[0];
    CHECK(signing::verify(fresh, received(request), request.body));
    CHECK_FALSE(signing::verify(old, received(request), request.body));
}

TEST_CASE("test_an_unavailable_credential_store_is_a_clear_message_not_a_crash",
          "[test_automation_hooks_webhooks][platform]") {
    Hooks f(fast(), std::make_shared<BrokenBackend>());
    try {
        f.engine->ensure_secret("a");
        FAIL("ensure_secret did not throw");
    } catch (const SecretUnavailable& error) {
        CHECK(std::string(error.what()).find("signing secret") != std::string::npos);
    }
    CHECK_THROWS_AS(f.engine->rotate_secret("a"), SecretUnavailable);
    CHECK_NOTHROW(f.engine->delete_secret("a"));

    f.save({hook("a", f.receiver.url("/a"))});
    f.engine->start();
    f.publish("recording.started", {{"job_id", "1"}});
    REQUIRE(wait_until([&] { return f.records("a") > 0; }));
    test::sleep_ms(300);
    const auto records = f.engine->deliveries("a");
    REQUIRE(records.size() == 1);
    CHECK_FALSE(records[0].ok);
    CHECK(records[0].error.find("signing secret") != std::string::npos);
    CHECK(f.receiver.requests().empty());
}

TEST_CASE("test_a_delivery_after_the_webhook_was_deleted_does_not_recreate_its_secret",
          "[test_automation_fix_hooks_webhook_secret][platform]") {
    Hooks f;
    f.save({hook("a", f.receiver.url("/a"))});
    const auto saved = hooks::find_webhook(*f.settings.snapshot(), "a");
    REQUIRE(saved);
    f.engine->ensure_secret("a");
    // Settings removes the webhook first, then forgets its secret.
    f.save({});
    f.engine->delete_secret("a");

    // A delivery that was already on its way when the webhook was deleted.
    const auto record =
        f.engine->deliver(*saved, synthetic_event("recording.started", {{"job_id", "1"}}), 1).first;

    CHECK_FALSE(f.credentials.get(hooks::webhook_secret_name("a")));
    CHECK_FALSE(record.ok);
    CHECK(record.error == "The signing secret is missing.");
    CHECK(f.receiver.requests().empty());
}

TEST_CASE("test_a_saved_webhooks_first_delivery_still_makes_its_secret",
          "[test_automation_fix_hooks_webhook_secret][platform]") {
    Hooks f;
    f.save({hook("a", f.receiver.url("/a"))});
    const auto saved = hooks::find_webhook(*f.settings.snapshot(), "a");
    REQUIRE(saved);
    CHECK_FALSE(f.credentials.get(hooks::webhook_secret_name("a")));

    const auto record =
        f.engine->deliver(*saved, synthetic_event("recording.started", {{"job_id", "1"}}), 1).first;

    CHECK(record.ok);
    const std::string secret = f.secret_stored("a");
    CHECK(secret.starts_with("whsec_"));
    CHECK(f.engine->ensure_secret("a") == secret);
}

// -- one-off signed posts
// --------------------------------------------------------------------------------

TEST_CASE("test_post_signed_signs_with_the_given_secret",
          "[test_automation_hooks_webhooks][platform]") {
    Receiver receiver;
    auto client = test::loopback_client();
    const std::string secret = signing::new_secret();
    const Json payload = {{"id", "msg_123"}, {"type", "action.run"}, {"text", "h\xc3\xa9llo"}};
    const DeliveryRecord record = post_signed(*client, receiver.url("/x"), payload, secret);

    CHECK(record.ok);
    CHECK(record.status == 200);
    CHECK(record.event_id == "msg_123");
    CHECK(record.event_type == "action.run");
    const auto request = receiver.wait_for(1, "/x")[0];
    CHECK(request.json() == payload);
    CHECK(request.header("webhook-id") == "msg_123");
    CHECK(signing::verify(secret, received(request), request.body));
}

TEST_CASE("test_post_signed_never_raises", "[test_automation_hooks_webhooks][platform]") {
    auto client = test::loopback_client();
    const std::string secret = signing::new_secret();
    CHECK(post_signed(*client, "ftp://x/y", Json::object(), secret).error.find("http") !=
          std::string::npos);
    CHECK_FALSE(post_signed(*client, "http://127.0.0.1:9/x", Json::object(), "not-a-secret").ok);
    // Python's {"bad": object()} cannot be built as JSON here; a lone surrogate is the payload
    // that does not encode.
    CHECK(
        post_signed(*client, "http://127.0.0.1:9/x", Json{{"bad", "\xed\xa0\xbd"}}, secret).error ==
        "The message isn't valid JSON.");
    const DeliveryRecord record = post_signed(
        *client, std::format("http://127.0.0.1:{}/x", test::closed_port()), Json::object(), secret);
    CHECK_FALSE(record.ok);
    CHECK(record.error == "Couldn't connect");
    CHECK(record.event_id.starts_with("msg_"));
}

TEST_CASE("test_post_signed_reply_returns_the_reply_body",
          "[test_automation_hooks_webhooks][platform]") {
    Receiver receiver;
    auto client = test::loopback_client();
    const std::string secret = signing::new_secret();
    const std::string answer = Json{{"text", "R\xc3\xa9ponse"}}.dump();
    receiver.answer("/text",
                    {Answer{.headers = {{"Content-Type", "application/json"}}, .body = answer}});
    receiver.answer("/plain", {Answer{.body = "not json"}});
    receiver.answer("/empty", {Answer{.status = 204}});
    receiver.answer("/fail", {Answer{.status = 500, .body = R"({"text": "no"})"}});
    receiver.answer("/moved", {Answer{.status = 301, .headers = {{"Location", "/text"}}}});

    auto [record, body] = post_signed_reply(*client, receiver.url("/text"), {{"x", 1}}, secret);
    CHECK(record.ok);
    CHECK(body == answer);
    const auto request = receiver.at("/text").at(0);
    CHECK(signing::verify(secret, received(request), request.body));
    CHECK(post_signed_reply(*client, receiver.url("/plain"), Json::object(), secret).second ==
          "not json");
    CHECK(post_signed_reply(*client, receiver.url("/empty"), Json::object(), secret).second == "");
    std::tie(record, body) =
        post_signed_reply(*client, receiver.url("/fail"), Json::object(), secret);
    CHECK_FALSE(record.ok);
    CHECK(record.status == 500);
    CHECK_FALSE(body);
    std::tie(record, body) =
        post_signed_reply(*client, receiver.url("/moved"), Json::object(), secret);
    CHECK_FALSE(record.ok);
    CHECK(record.status == 301);
    CHECK_FALSE(body);
    CHECK(receiver.at("/text").size() == 1);
}

TEST_CASE("test_post_signed_reply_refuses_a_reply_over_max_bytes",
          "[test_automation_hooks_webhooks][platform]") {
    Receiver receiver;
    auto client = test::loopback_client();
    receiver.answer("/big", {Answer{.body = "{\"text\": \"" + std::string(500, 'x') + "\"}"}});
    auto [record, body] = post_signed_reply(*client, receiver.url("/big"), Json::object(),
                                            signing::new_secret(), SignedPost{.max_bytes = 100});
    CHECK_FALSE(body);
    CHECK_FALSE(record.ok);
    CHECK(record.error == "The reply was too large");
    std::tie(record, body) =
        post_signed_reply(*client, receiver.url("/big"), Json::object(), signing::new_secret());
    CHECK(record.ok);
    REQUIRE(body);
    CHECK(body->size() == 512);
}

TEST_CASE("test_post_signed_reply_never_raises", "[test_automation_hooks_webhooks][platform]") {
    auto client = test::loopback_client();
    auto [record, body] =
        post_signed_reply(*client, "not a url", Json::object(), signing::new_secret());
    CHECK_FALSE(record.ok);
    CHECK_FALSE(body);
    // Python's max_bytes="lots" cannot be passed here; nothing listening is the other failure.
    std::tie(record, body) =
        post_signed_reply(*client, std::format("http://127.0.0.1:{}/x", test::closed_port()),
                          Json::object(), signing::new_secret(), SignedPost{.max_bytes = 0});
    CHECK_FALSE(record.ok);
    CHECK_FALSE(body);
}

TEST_CASE("test_post_signed_takes_a_message_id_and_extra_headers",
          "[test_automation_hooks_webhooks][platform]") {
    Receiver receiver;
    auto client = test::loopback_client();
    const std::string secret = signing::new_secret();
    const DeliveryRecord record = post_signed(
        *client, receiver.url("/x"), {{"id", "evt_payload"}, {"type", "meeting.started"}}, secret,
        SignedPost{.msg_id = "msg_chosen",
                   .headers = {{"X-MCP-Subscription-Id", "sub_1"},
                               {"webhook-signature", "v1,forged"},
                               {"Content-Type", "text/plain"}}});
    CHECK(record.ok);
    CHECK(record.event_id == "msg_chosen");
    auto request = receiver.wait_for(1, "/x")[0];
    CHECK(request.header("webhook-id") == "msg_chosen");
    CHECK(request.header("x-mcp-subscription-id") == "sub_1");
    CHECK(request.header("content-type") == "application/json");
    CHECK(signing::verify(secret, received(request), request.body));

    post_signed_reply(*client, receiver.url("/y"), Json::object(), secret,
                      SignedPost{.msg_id = "msg_two", .headers = {{"X-Extra", "1"}}});
    request = receiver.wait_for(1, "/y")[0];
    CHECK(request.header("webhook-id") == "msg_two");
    CHECK(request.header("x-extra") == "1");
}

TEST_CASE("a caller's own User-Agent replaces the default one", "[webhooks]") {
    // httpx merges a request's headers over the client's, so the hook client's User-Agent is a
    // default an extra header replaces, never a second header beside it.
    fv::http::FakeHttpClient client;
    client.set_handler([](const fv::http::Request&) { return fv::http::ScriptedResponse{}; });
    const std::string secret = signing::new_secret();
    CHECK(post_signed(client, "http://127.0.0.1:9/x", Json::object(), secret).ok);
    CHECK(post_signed(client, "http://127.0.0.1:9/x", Json::object(), secret,
                      SignedPost{.headers = {{"user-agent", "custom/1"}}})
              .ok);
    const auto requests = client.requests();
    REQUIRE(requests.size() == 2);
    const auto agents = [](const fv::http::Request& request) {
        std::vector<std::string> found;
        for (const auto& [name, value] : request.headers) {
            if (fv::py::lower(name) == "user-agent") {
                found.push_back(value);
            }
        }
        return found;
    };
    CHECK(agents(requests[0]) == std::vector<std::string>{fv::http::user_agent()});
    CHECK(agents(requests[1]) == std::vector<std::string>{"custom/1"});
}

TEST_CASE("the fake webhook control answers held tests, a missing callback too", "[webhooks]") {
    FakeWebhookControl fake;
    DeliveryRecord answer{"evt_x", "webhook.test", "t", false, 500, 9, "HTTP 500", 1};
    fake.set_test_answer(answer, true);
    std::vector<DeliveryRecord> answers;
    fake.send_test("a", [&](DeliveryRecord record) { answers.push_back(std::move(record)); });
    fake.send_test("b", {});
    CHECK(answers.empty());
    fake.release_tests();
    CHECK(answers == std::vector<DeliveryRecord>{answer});
    CHECK(fake.tests_sent() == std::vector<std::string>{"a", "b"});
}

// -- privacy
// -----------------------------------------------------------------------------------------------

TEST_CASE("test_logs_never_carry_urls_event_data_or_secrets",
          "[test_automation_hooks_webhooks][platform]") {
    test::ScopedLogCapture capture;
    Hooks f;
    f.receiver.answer("/a", {Answer{.status = 500}});
    f.save({hook("a", f.receiver.url("/a?token=sekrit"))});
    f.engine->start();
    f.publish("recording.started", {{"job_id", "dictated words here"}});
    REQUIRE(wait_until([&] { return f.records("a") == 4; }));
    const std::string secret = f.engine->ensure_secret("a");
    const std::string text = capture.text();
    for (const std::string& needle :
         {std::string("sekrit"), std::string("dictated words"), secret,
          std::format(":{}", f.receiver.port()), std::format("port={}", f.receiver.port())}) {
        CHECK(text.find(needle) == std::string::npos);
    }
}

TEST_CASE("test_other_requests_keep_their_log_lines",
          "[test_automation_hooks_webhooks][platform]") {
    // fv_http's request line goes to "httpx" at INFO, which only the app's logging setup holds
    // back; the hook engines install nothing that would hide another caller's line.
    test::ScopedLogCapture capture;
    fv::log::set_logger_level("httpx", fv::log::Level::Info);
    Receiver receiver;
    auto client = test::loopback_client();
    fv::http::Request request;
    request.method = "POST";
    request.url = receiver.url("/other");
    request.body = std::string();
    REQUIRE(client->send(request, {}).has_value());
    CHECK(capture.text().find("/other") != std::string::npos);
}

// -- lanes (test_automation_fix_hooks_webhook_lanes.py)
// ------------------------------------------------------

TEST_CASE("test_a_slow_webhook_does_not_delay_the_others",
          "[test_automation_fix_hooks_webhook_lanes][platform]") {
    Hooks f;
    // Answers after 3 s, inside the 5 s timeout: a slow receiver, not a failing one.
    f.receiver.answer("/slow", {Answer{.delay = 3s}});
    f.save({hook("slow", f.receiver.url("/slow")), hook("fast", f.receiver.url("/fast"))});
    f.engine->start();
    const auto started = std::chrono::steady_clock::now();
    std::vector<Event> published;
    for (int i = 0; i < 3; ++i) {
        published.push_back(f.publish("recording.started", {{"n", i}}));
    }

    const auto got = f.receiver.wait_for(3, "/fast", 2500ms);
    CHECK(test::seconds_since(started) < 2.5);
    CHECK(ids_of(got) ==
          std::vector<std::string>{published[0].id, published[1].id, published[2].id});
    // The slow one gets its events in order, one answer at a time.
    const auto slow = f.receiver.wait_for(1, "/slow", 10s);
    CHECK(slow[0].json()["id"] == published[0].id);
}

TEST_CASE("test_a_full_queue_only_drops_that_webhooks_own_deliveries",
          "[test_automation_fix_hooks_webhook_lanes][platform]") {
    WebhookOptions options = fast();
    options.max_queued = 3;
    Hooks f(options);
    auto release = std::make_shared<Gate>();
    f.receiver.answer("/stuck", {Answer{.hold = release}, Answer{.status = 200}});
    f.save({hook("stuck", f.receiver.url("/stuck")), hook("fine", f.receiver.url("/fine"))});
    f.engine->start();
    const Event first = f.publish("recording.started", {{"n", 0}});
    REQUIRE(wait_until([&] { return f.receiver.arrived_at("/stuck"); }));
    std::vector<Event> later;
    for (int i = 1; i < 9; ++i) {
        // The other webhook keeps up, one at a time, while the stuck one backs up.
        later.push_back(f.publish("recording.started", {{"n", i}}));
        f.receiver.wait_for(static_cast<std::size_t>(i) + 1, "/fine");
    }

    // The other webhook gets all nine and never loses one to the stuck one's backlog.
    const auto fine = f.receiver.wait_for(9, "/fine");
    std::vector<std::string> expected{first.id};
    for (const auto& event : later) {
        expected.push_back(event.id);
    }
    CHECK(ids_of(fine) == expected);
    CHECK(dropped_count(f.engine->deliveries("fine")) == 0);

    REQUIRE(wait_until([&] { return dropped_count(f.engine->deliveries("stuck")) == 5; }));
    release->set();
    const auto stuck = f.receiver.wait_for(4, "/stuck");
    CHECK(ids_of(stuck) ==
          std::vector<std::string>{first.id, later[5].id, later[6].id, later[7].id});
}

TEST_CASE("test_an_idle_lane_lets_its_thread_go",
          "[test_automation_fix_hooks_webhook_lanes][platform]") {
    WebhookOptions options = fast();
    options.lane_idle_s = 0.1;
    Hooks f(options);
    f.save({hook("a", f.receiver.url("/a")), hook("b", f.receiver.url("/b"))});
    f.engine->start();
    f.publish("recording.started", {{"n", 0}});
    f.receiver.wait_for(1, "/a");
    f.receiver.wait_for(1, "/b");

    CHECK(wait_until([&] { return f.engine->lane_threads() == 0; }, 5s));
    // A later event starts a lane again.
    f.publish("recording.started", {{"n", 1}});
    f.receiver.wait_for(2, "/a");
    f.receiver.wait_for(2, "/b");
}

TEST_CASE("an idle lane ends on the engine's clock", "[webhooks][platform]") {
    fv::FakeClock clock;
    WebhookOptions options;
    options.clock = &clock;
    Hooks f(options);
    f.save({hook("a", f.receiver.url("/a"))});
    f.engine->start();
    f.publish("recording.started", {{"n", 0}});
    f.receiver.wait_for(1, "/a");
    test::sleep_ms(100);
    CHECK(f.engine->lane_threads() == 1);
    clock.advance(29s);
    test::sleep_ms(100);
    CHECK(f.engine->lane_threads() == 1);
    clock.advance(2s);
    CHECK(wait_until([&] { return f.engine->lane_threads() == 0; }, 5s));
}
