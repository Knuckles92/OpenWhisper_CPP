// Ports tests/test_personalize_s1_service.py, tests/test_personalize_fix_f3_reread_exclusion.py
// and the capture part of tests/test_personalize_fix_f2_selection_blocked.py. The platform is
// FakeFocusPlatform (its reads run on the service's real text thread); deadlines run on a
// ManualExecutor over a FakeClock, so "the deadline passes" is clock.advance() rather than a
// sleep, and the wedge check reads the same clock.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "focus/capture_service.h"
#include "focus/focus_capture.h"
#include "focus/foreground.h"
#include "focus/testing/fake_platform.h"
#include "focus_support.h"
#include "log/metrics_capture.h"
#include "settings/store.h"
#include "testkit/testkit.h"

using namespace fv;
using namespace fv::focus;
using namespace std::chrono_literals;

namespace {

constexpr auto kWait = 5000ms;

AppIdentity identity(std::string app_id, std::string name, std::optional<std::uint32_t> pid,
                     std::string window = "", std::string title_hint = "",
                     std::string platform = "") {
    return AppIdentity{.app_id = std::move(app_id),
                       .name = std::move(name),
                       .pid = pid,
                       .window = std::move(window),
                       .title_hint = std::move(title_hint),
                       .platform = std::move(platform)};
}

const AppIdentity kOutlook = identity("outlook.exe", "Outlook", 10, "0x1");
const AppIdentity kSlack = identity("slack.exe", "Slack", 11, "0x2");
const AppIdentity kTerminal = identity("windowsterminal.exe", "Windows Terminal", 14);
const AppIdentity kSelf{.app_id = "openwhisper", .name = "OpenWhisper", .pid = 1, .is_self = true};
const TextContext kContext = FakeFocusPlatform::default_context();

// The test's own copy of wait_for in OpenWhisper's tests: polls in real time.
template <class Predicate>
bool wait_until(Predicate predicate, std::chrono::milliseconds timeout = kWait) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

FocusSnapshot snapshot(const std::shared_ptr<FocusRequest>& request) {
    const auto resolved = request->wait_for(kWait);
    REQUIRE(resolved);
    return *resolved;
}

FocusSnapshot resolved_now(const std::shared_ptr<FocusRequest>& request) {
    const auto resolved = request->try_get();
    REQUIRE(resolved);
    return *resolved;
}

FocusSnapshot only(std::optional<AppIdentity> identity) {
    return FocusSnapshot{std::move(identity), std::nullopt};
}

FocusSnapshot with(std::optional<AppIdentity> identity, TextContext context) {
    return FocusSnapshot{std::move(identity), std::move(context)};
}

struct Options {
    std::chrono::milliseconds deadline = 1000ms;
    std::chrono::milliseconds wedged_after = 10000ms;
    std::vector<std::string> excluded;
};

struct Harness {
    explicit Harness(std::shared_ptr<FakeFocusPlatform> fake = nullptr, Options options = {})
        : platform(fake ? std::move(fake) : std::make_shared<FakeFocusPlatform>(kOutlook)) {
        for (const auto& app : options.excluded) {
            excluded.push_back(app);
        }
        FocusCaptureOptions settings;
        settings.deadline = options.deadline;
        settings.wedged_after = options.wedged_after;
        settings.clock = &clock;
        settings.timers = &timers;
        settings.settings = [this] {
            std::lock_guard lock(mutex);
            if (settings_broken) {
                ++callouts_after_shutdown;
                throw std::runtime_error("settings locked");
            }
            return std::make_shared<const Json>(Json{{"app_context_excluded_apps", excluded}});
        };
        settings.metrics = [this](const CaptureSample& sample) {
            std::lock_guard lock(mutex);
            if (metrics_broken) {
                ++callouts_after_shutdown;
                throw std::invalid_argument("metrics");
            }
            samples.push_back(sample);
        };
        service = std::make_unique<FocusCapture>(platform, std::move(settings));
    }

    ~Harness() {
        platform->open_all_gates();
        service->shutdown();
        service.reset();
        // Every reader closes once its thread has finished its call.
        for (const auto& reader : platform->readers()) {
            reader->closed.wait(kWait);
        }
    }

    std::vector<CaptureSample> metrics() {
        std::lock_guard lock(mutex);
        return samples;
    }

    void exclude(const std::string& app) {
        std::lock_guard lock(mutex);
        excluded.push_back(app);
    }

    FakeClock clock;
    ManualExecutor timers{clock};
    std::shared_ptr<FakeFocusPlatform> platform;
    std::mutex mutex;
    Json excluded = Json::array();
    bool settings_broken = false;
    bool metrics_broken = false;
    // Calls made while settings_broken or metrics_broken was set.
    int callouts_after_shutdown = 0;
    std::vector<CaptureSample> samples;
    std::unique_ptr<FocusCapture> service;
};

std::vector<std::string> read_apps(const FakeFocusPlatform& platform) {
    std::vector<std::string> apps;
    for (const auto& read : platform.reads()) {
        apps.push_back(read.app_id);
    }
    return apps;
}

// Collects reread answers and the thread each arrived on.
struct Answers {
    void operator()(std::optional<TextContext> context) {
        {
            std::lock_guard lock(mutex);
            values.push_back(std::move(context));
            threads.push_back(std::this_thread::get_id());
        }
        done.set();
    }
    std::vector<std::optional<TextContext>> all() {
        std::lock_guard lock(mutex);
        return values;
    }
    std::function<void(std::optional<TextContext>)> callback() {
        return [this](std::optional<TextContext> context) {
            (*this)(std::move(context));
        };
    }

    std::mutex mutex;
    std::vector<std::optional<TextContext>> values;
    std::vector<std::thread::id> threads;
    FakeEvent done;
};

std::optional<TextContext> reread(CaptureService& service, const AppIdentity& target) {
    Answers answers;
    service.reread(target, answers.callback());
    REQUIRE(answers.done.wait(kWait));
    return answers.all().front();
}

} // namespace

// from tests/test_personalize_s1_service.py::test_identity_only_resolves_at_once_without_a_thread
TEST_CASE("identity only resolves at once without a thread", "[test_personalize_s1_service]") {
    Harness h;

    const auto request = h.service->request(false, false);

    CHECK(request->ready());
    CHECK(resolved_now(request) == only(kOutlook));
    CHECK_FALSE(h.service->state().has_worker);
    CHECK(h.platform->readers().empty());
    CHECK(h.metrics().empty());
}

// from tests/test_personalize_s1_service.py::test_text_is_read_on_its_own_thread
TEST_CASE("text is read on its own thread", "[test_personalize_s1_service]") {
    Harness h;

    CHECK(snapshot(h.service->request(true, false)) == with(kOutlook, kContext));

    const auto reads = h.platform->reads();
    REQUIRE(reads.size() == 1);
    CHECK(reads[0].same_call("outlook.exe", true, false));
    CHECK(reads[0].thread != std::this_thread::get_id());
    CHECK(h.platform->readers()[0]->thread == reads[0].thread);
    // The sample is recorded on the text thread right after the request resolves.
    REQUIRE(wait_until([&] { return !h.metrics().empty(); }));
    const auto metrics = h.metrics();
    REQUIRE(metrics.size() == 1);
    CHECK(metrics[0].context_capture_timeouts == 0);
    CHECK(metrics[0].context_capture_ms >= 0);
    CHECK(metrics[0].context_capture_ms < 5000);

    const auto selection = snapshot(h.service->request(false, true));
    CHECK(selection.text == kContext);
    CHECK(h.platform->reads().back().same_call("outlook.exe", false, true));
    CHECK(h.platform->readers().size() == 1);
}

// from tests/test_personalize_s1_service.py::test_the_future_resolves_by_the_deadline_with_identity
TEST_CASE("the future resolves by the deadline with identity", "[test_personalize_s1_service]") {
    Harness h(nullptr, {.deadline = 50ms});
    h.platform->gate("outlook.exe");

    const auto request = h.service->request(true, false);
    REQUIRE(h.platform->blocked.wait(kWait));
    CHECK_FALSE(request->ready());
    h.timers.advance(50ms);

    CHECK(resolved_now(request) == only(kOutlook));
    CHECK(h.metrics() == std::vector<CaptureSample>{{50.0, 1}});
    h.platform->gate("outlook.exe")->set();
    CHECK(wait_until([&] { return !h.service->state().worker_busy; }));
    CHECK(h.metrics().size() == 1);
}

// from tests/test_personalize_s1_service.py::test_a_busy_reader_means_identity_only_never_a_queue
TEST_CASE("a busy reader means identity only never a queue", "[test_personalize_s1_service]") {
    Harness h;
    h.platform->gate("outlook.exe");
    const auto first = h.service->request(true, false);
    REQUIRE(h.platform->blocked.wait(kWait));

    h.platform->set_current(kSlack);
    const auto second = h.service->request(true, false);

    CHECK(resolved_now(second) == only(kSlack));
    h.platform->gate("outlook.exe")->set();
    CHECK(snapshot(first) == with(kOutlook, kContext));
    CHECK(read_apps(*h.platform) == std::vector<std::string>{"outlook.exe"});
}

// from
// tests/test_personalize_s1_service.py::test_a_wedged_reader_is_replaced_and_its_app_left_alone
TEST_CASE("a wedged reader is replaced and its app left alone", "[test_personalize_s1_service]") {
    Harness h(nullptr, {.deadline = 50ms, .wedged_after = 50ms});
    h.platform->gate("outlook.exe");
    const auto first = h.service->request(true, false);
    REQUIRE(h.platform->blocked.wait(kWait));
    h.timers.advance(50ms);
    CHECK(resolved_now(first) == only(kOutlook));
    h.timers.advance(100ms);
    h.service->set_deadline(kWait);

    h.platform->set_current(kSlack);
    CHECK(snapshot(h.service->request(true, false)) == with(kSlack, kContext));
    const auto state = h.service->state();
    CHECK(state.replacements == 1);
    CHECK(state.workers_started == 2);
    CHECK(h.platform->readers().size() == 2);

    h.platform->set_current(kOutlook);
    const auto again = h.service->request(true, false);
    CHECK(resolved_now(again) == only(kOutlook));
    CHECK(read_apps(*h.platform) == std::vector<std::string>{"outlook.exe", "slack.exe"});

    // The abandoned thread finishes its call, closes its reader and exits.
    h.platform->gate("outlook.exe")->set();
    CHECK(h.platform->readers()[0]->closed.wait(kWait));
    CHECK_FALSE(h.platform->readers()[1]->closed.is_set());
}

// from tests/test_personalize_s1_service.py::test_text_capture_stops_after_a_few_replacements
TEST_CASE("text capture stops after a few replacements", "[test_personalize_s1_service]") {
    Harness h(nullptr, {.deadline = 20ms, .wedged_after = 20ms});
    std::vector<AppIdentity> apps;
    for (std::uint32_t index = 0; index < 6; ++index) {
        apps.push_back(identity("app" + std::to_string(index) + ".exe",
                                "App " + std::to_string(index), index));
        h.platform->gate(apps.back().app_id);
    }

    for (int index = 0; index < MAX_WORKER_REPLACEMENTS + 2; ++index) {
        h.platform->set_current(apps[index]);
        const auto request = h.service->request(true, false);
        h.timers.advance(20ms);
        CHECK(snapshot(request) == only(apps[index]));
        h.timers.advance(50ms);
    }

    CHECK(h.service->state().text_off);
    CHECK(wait_until([&] {
        return h.platform->readers().size() ==
               static_cast<std::size_t>(MAX_WORKER_REPLACEMENTS + 1);
    }));
    h.platform->set_current(apps.back());
    const auto request = h.service->request(true, false);
    CHECK(resolved_now(request) == only(apps.back()));
    CHECK(resolved_now(h.service->request(false, false)) == only(apps.back()));
}

// from tests/test_personalize_s1_service.py::test_repeated_misses_stop_reading_one_app
TEST_CASE("repeated misses stop reading one app", "[test_personalize_s1_service]") {
    Harness h(nullptr, {.deadline = 20ms});
    const auto gate = h.platform->gate("outlook.exe");

    for (int index = 0; index < MAX_CONSECUTIVE_MISSES; ++index) {
        const auto request = h.service->request(true, false);
        h.timers.advance(20ms);
        CHECK(snapshot(request) == only(kOutlook));
        gate->set();
        CHECK(wait_until([&] { return !h.service->state().worker_busy; }));
        gate->clear();
    }

    CHECK(h.service->state().disabled.contains("outlook.exe"));
    int timeouts = 0;
    for (const auto& sample : h.metrics()) {
        timeouts += sample.context_capture_timeouts;
    }
    CHECK(timeouts == MAX_CONSECUTIVE_MISSES);
    const auto reads = h.platform->reads().size();
    CHECK(resolved_now(h.service->request(true, false)) == only(kOutlook));
    CHECK(h.platform->reads().size() == reads);

    h.platform->set_current(kSlack);
    CHECK(snapshot(h.service->request(true, false)) == with(kSlack, kContext));
}

// from tests/test_personalize_s1_service.py::test_a_fast_read_resets_the_miss_count
TEST_CASE("a fast read resets the miss count", "[test_personalize_s1_service]") {
    // The deadlines run on the fake clock; what is left on real time are failure bounds, long
    // enough here that a loaded machine can't reach them: a gated read must never time out on
    // its own and answer fast, nor a slow worker be taken for a stuck one.
    constexpr auto kLoaded = 60s;
    Harness h(nullptr, {.deadline = 20ms});
    h.platform->gate_timeout = kLoaded;
    const auto gate = h.platform->gate("outlook.exe");
    for (int index = 0; index < MAX_CONSECUTIVE_MISSES - 1; ++index) {
        const auto request = h.service->request(true, false);
        h.timers.advance(20ms);
        REQUIRE(request->wait_for(kLoaded));
        gate->set();
        CHECK(wait_until([&] { return !h.service->state().worker_busy; }, kLoaded));
        gate->clear();
    }
    CHECK(h.service->state().misses.at("outlook.exe") == MAX_CONSECUTIVE_MISSES - 1);
    h.platform->remove_gate("outlook.exe");
    h.service->set_deadline(1000ms);
    const auto fast = h.service->request(true, false)->wait_for(kLoaded);
    REQUIRE(fast);
    CHECK(fast->text == kContext);
    CHECK_FALSE(h.service->state().misses.contains("outlook.exe"));
}

// from tests/test_personalize_s1_service.py::test_no_text_is_read_for_unknown_self_or_terminal_apps
TEST_CASE("no text is read for unknown self or terminal apps", "[test_personalize_s1_service]") {
    const auto target =
        GENERATE(std::optional<AppIdentity>(), std::optional<AppIdentity>(kSelf),
                 std::optional<AppIdentity>(kTerminal), std::optional<AppIdentity>(AppIdentity{}));
    CAPTURE(test::describe(target));
    Harness h(std::make_shared<FakeFocusPlatform>(target));

    const auto request = h.service->request(true, true);

    CHECK(resolved_now(request) == only(target));
    CHECK(h.platform->reads().empty());
    CHECK_FALSE(h.service->state().has_worker);
}

// from tests/test_personalize_s1_service.py::test_excluded_apps_are_never_read
TEST_CASE("excluded apps are never read", "[test_personalize_s1_service]") {
    const std::string excluded = GENERATE(as<std::string>{}, "Outlook", "OUTLOOK.EXE", "outlook");
    CAPTURE(excluded);
    Harness h(nullptr, {.excluded = {excluded}});

    CHECK(snapshot(h.service->request(true, false)) == only(kOutlook));
    // A selection read is told it was refused, so Command Mode won't copy it.
    CHECK(snapshot(h.service->request(true, true)) ==
          with(kOutlook, TextContext{.source = std::string(EXCLUDED_SOURCE), .blocked = true}));
    CHECK(h.platform->reads().empty());
}

// from tests/test_personalize_s1_service.py::test_unreadable_settings_fail_closed
TEST_CASE("unreadable settings fail closed", "[test_personalize_s1_service]") {
    Harness h;
    {
        std::lock_guard lock(h.mutex);
        h.settings_broken = true;
    }

    CHECK(snapshot(h.service->request(true, false)) == only(kOutlook));
    CHECK(h.platform->reads().empty());
}

// from tests/test_personalize_s1_service.py::test_nothing_raises
TEST_CASE("nothing raises", "[test_personalize_s1_service]") {
    Harness h;
    h.platform->set_identity_error(true);
    CHECK(resolved_now(h.service->request(true, false)) == FocusSnapshot{});
    CHECK_FALSE(h.service->current_identity());

    h.platform->set_identity_error(false);
    h.platform->set_read_error(true);
    CHECK(snapshot(h.service->request(true, false)) == only(kOutlook));

    h.platform->set_read_error(false);
    {
        std::lock_guard lock(h.mutex);
        h.metrics_broken = true;
    }
    CHECK(snapshot(h.service->request(true, false)) == with(kOutlook, kContext));
}

// from tests/test_personalize_s1_service.py::test_a_reader_that_cannot_start_turns_text_off
TEST_CASE("a reader that cannot start turns text off", "[test_personalize_s1_service]") {
    Harness h;
    h.platform->set_reader_error(true);

    CHECK(snapshot(h.service->request(true, false)) == only(kOutlook));
    CHECK(wait_until([&] { return h.service->state().text_off; }));
    const auto request = h.service->request(true, false);
    CHECK(resolved_now(request) == only(kOutlook));
}

// from tests/test_personalize_s1_service.py::test_platforms_without_text_only_name_the_app
TEST_CASE("platforms without text only name the app", "[test_personalize_s1_service]") {
    Harness h(std::make_shared<FakeFocusPlatform>(kOutlook, true, false));

    const auto request = h.service->request(true, true);

    CHECK(resolved_now(request) == only(kOutlook));
    CHECK_FALSE(h.service->state().has_worker);
}

// from
// tests/test_personalize_s1_service.py::test_async_identity_resolves_on_its_own_thread_within_the_deadline
TEST_CASE("async identity resolves on its own thread within the deadline",
          "[test_personalize_s1_service]") {
    Harness h(std::make_shared<FakeFocusPlatform>(kOutlook, false, false), {.deadline = 300ms});

    CHECK(snapshot(h.service->request(false, false)) == only(kOutlook));
    const auto threads = h.platform->identity_threads_seen();
    REQUIRE_FALSE(threads.empty());
    CHECK(threads[0] != std::this_thread::get_id());
    CHECK_FALSE(h.service->current_identity());

    h.platform->set_identity_delay(1500ms);
    const auto started = std::chrono::steady_clock::now();
    const auto request = h.service->request(false, false);
    h.timers.advance(300ms);
    CHECK(snapshot(request) == FocusSnapshot{});
    CHECK(std::chrono::steady_clock::now() - started < 1200ms);
}

// from tests/test_personalize_s1_service.py::test_current_identity_is_synchronous_where_identity_is
TEST_CASE("current identity is synchronous where identity is", "[test_personalize_s1_service]") {
    Harness h;
    CHECK(h.service->current_identity() == kOutlook);
    h.platform->set_current(kSlack);
    CHECK(h.service->current_identity() == kSlack);
}

// from
// tests/test_personalize_s1_service.py::test_reread_reads_again_only_while_the_same_app_has_focus
TEST_CASE("reread reads again only while the same app has focus", "[test_personalize_s1_service]") {
    Harness h;
    Answers answers;

    h.service->reread(kOutlook, answers.callback());
    REQUIRE(answers.done.wait(kWait));
    CHECK(answers.all()[0] == kContext);
    REQUIRE(h.platform->reads().size() == 1);
    CHECK(answers.threads[0] == h.platform->reads()[0].thread);
    CHECK(h.platform->reads()[0].same_call("outlook.exe", true, true));

    answers.done.clear();
    h.platform->set_current(identity("outlook.exe", "Outlook", 10, "0x9"));
    h.service->reread(kOutlook, answers.callback());
    REQUIRE(answers.done.wait(kWait));
    CHECK_FALSE(answers.all().back());
    CHECK(h.platform->reads().size() == 1);
}

// from tests/test_personalize_s1_service.py::test_reread_answers_none_when_it_cannot_read
TEST_CASE("reread answers none when it cannot read", "[test_personalize_s1_service]") {
    Harness h;
    Answers answers;
    h.service->reread(kTerminal, answers.callback());
    h.service->reread(std::nullopt, answers.callback());

    h.platform->gate("outlook.exe");
    const auto busy = h.service->request(true, false);
    REQUIRE(h.platform->blocked.wait(kWait));
    h.service->reread(kOutlook, answers.callback());
    CHECK(answers.all() == std::vector<std::optional<TextContext>>(3));

    CHECK_NOTHROW(h.service->reread(
        kTerminal, [](std::optional<TextContext>) { throw std::runtime_error("callback"); }));

    Harness async(std::make_shared<FakeFocusPlatform>(kOutlook, false));
    async.service->reread(kOutlook, answers.callback());
    CHECK(answers.all() == std::vector<std::optional<TextContext>>(4));
}

// from
// tests/test_personalize_s1_service.py::test_shutdown_stops_the_thread_and_empties_later_snapshots
TEST_CASE("shutdown stops the thread and empties later snapshots",
          "[test_personalize_s1_service]") {
    Harness h;
    snapshot(h.service->request(true, false));

    h.service->shutdown();

    CHECK(h.platform->readers()[0]->closed.wait(kWait));
    CHECK(resolved_now(h.service->request(true, false)) == FocusSnapshot{});
    CHECK_FALSE(h.service->current_identity());
    Answers answers;
    h.service->reread(kOutlook, answers.callback());
    CHECK(answers.all() == std::vector<std::optional<TextContext>>(1));
}

// from
// tests/test_personalize_s1_service.py::test_recent_apps_are_newest_first_without_repeats_or_self
TEST_CASE("recent apps are newest first without repeats or self", "[test_personalize_s1_service]") {
    auto h = std::make_shared<Harness>();
    const AppIdentity gmail{.app_id = "chrome.exe", .name = "Chrome", .title_hint = "Gmail"};
    for (const auto& current :
         {std::optional<AppIdentity>(kOutlook), std::optional<AppIdentity>(kSlack),
          std::optional<AppIdentity>(kSelf), std::optional<AppIdentity>(gmail),
          std::optional<AppIdentity>(kOutlook), std::optional<AppIdentity>()}) {
        h->platform->set_current(current);
        h->service->request(false, false);
    }

    const std::vector<AppIdentity> expected{kOutlook, gmail, kSlack};
    CHECK(h->service->recent_apps() == expected);
    // The process-wide slot holds a shared service; this one lives as long as the harness.
    set_service(std::shared_ptr<CaptureService>(h, h->service.get()));
    CHECK(recent_apps() == expected);
    set_service(std::make_shared<NullCaptureService>());
    CHECK(recent_apps().empty());
    set_service(nullptr);
}

// from
// tests/test_personalize_s1_service.py::test_the_default_service_is_the_real_one_for_this_platform
// Reads the real foreground window.
TEST_CASE("the default service is the real one for this platform",
          "[test_personalize_s1_service][interactive]") {
    testkit::TempDataRoot root;
    SettingsStore settings(root.path() / "settings.json");
    set_service(make_capture_service(settings));
    const auto service = get_service();
#ifdef _WIN32
    CHECK(dynamic_cast<FocusCapture*>(service.get()) != nullptr);
#endif
    CHECK(service->request(false, false)->wait_for(kWait));
    shutdown_service();
    set_service(std::make_shared<NullCaptureService>());
}

// from tests/test_personalize_s1_service.py::test_capture_metrics_are_allowlisted
TEST_CASE("capture metrics are allowlisted", "[test_personalize_s1_service]") {
    for (std::string_view name : {"context_capture_ms", "context_capture_timeouts"}) {
        CAPTURE(name);
        CHECK(std::ranges::find(log::kMetricNames, name) != log::kMetricNames.end());
    }
}

// from tests/test_personalize_s1_service.py::test_settling_an_already_resolved_future_reports_false
TEST_CASE("settling an already resolved future reports false", "[test_personalize_s1_service]") {
    FocusRequest request;
    CHECK(request.settle(only(kOutlook)));
    CHECK_FALSE(request.settle(FocusSnapshot{}));
    CHECK(request.try_get() == only(kOutlook));
}

// from tests/test_personalize_s1_service.py::test_identity_now_gives_up_after_the_timeout
TEST_CASE("identity now gives up after the timeout", "[test_personalize_s1_service]") {
    Harness h(std::make_shared<FakeFocusPlatform>(kOutlook, false, false));
    h.platform->set_identity_delay(1000ms);

    const auto started = std::chrono::steady_clock::now();
    CHECK_FALSE(h.service->identity_now(50ms));
    CHECK(std::chrono::steady_clock::now() - started < 900ms);
}

TEST_CASE("a request calls back once it resolves", "[test_personalize_s1_service]") {
    FocusRequest request;
    std::vector<FocusSnapshot> seen;
    request.on_ready([&](const FocusSnapshot& value) { seen.push_back(value); });
    CHECK(seen.empty());
    request.settle(only(kSlack));
    request.on_ready([&](const FocusSnapshot& value) { seen.push_back(value); });
    CHECK(seen == std::vector<FocusSnapshot>{only(kSlack), only(kSlack)});
    CHECK(FocusRequest::resolved(FocusSnapshot{})->ready());
}

TEST_CASE("the null service knows nothing", "[test_personalize_s1_service]") {
    NullCaptureService service;
    CHECK(resolved_now(service.request(true, true)) == FocusSnapshot{});
    CHECK_FALSE(service.current_identity());
    CHECK_FALSE(service.identity_now(10ms));
    Answers answers;
    service.reread(kOutlook, answers.callback());
    CHECK(answers.all() == std::vector<std::optional<TextContext>>(1));
    CHECK(service.recent_apps().empty());
    set_service(nullptr);
    CHECK(dynamic_cast<NullCaptureService*>(get_service().get()) != nullptr);
    shutdown_service();
}

TEST_CASE("the service's own deadline timers resolve a stuck read",
          "[test_personalize_s1_service]") {
    // The production wiring: the system clock and the service's own deadline pool.
    auto platform = std::make_shared<FakeFocusPlatform>(kOutlook);
    platform->gate("outlook.exe");
    FocusCaptureOptions options;
    options.deadline = 50ms;
    options.metrics = [](const CaptureSample&) {
    };
    {
        FocusCapture service(platform, std::move(options));
        const auto started = std::chrono::steady_clock::now();
        CHECK(snapshot(service.request(true, false)) == only(kOutlook));
        CHECK(std::chrono::steady_clock::now() - started < 2000ms);
    }
    platform->open_all_gates();
    CHECK(platform->readers()[0]->closed.wait(kWait));
}

TEST_CASE("nothing reaches settings or metrics after shutdown", "[test_personalize_s1_service]") {
    // The text thread outlives shutdown() while a read runs; what the providers refer to (the
    // SettingsStore, a metrics sink) may be gone by the time it returns.
    Harness h;
    h.platform->gate("outlook.exe");
    const auto request = h.service->request(true, false);
    REQUIRE(h.platform->blocked.wait(kWait));

    h.service->shutdown();
    {
        std::lock_guard lock(h.mutex);
        h.settings_broken = true;
        h.metrics_broken = true;
    }
    h.platform->gate("outlook.exe")->set();

    // The read in flight still answers its request, then the thread ends.
    CHECK(snapshot(request) == with(kOutlook, kContext));
    REQUIRE(h.platform->readers()[0]->closed.wait(kWait));
    h.timers.advance(5000ms);
    CHECK(h.metrics().empty());
    CHECK(h.callouts_after_shutdown == 0);
}

// --- tests/test_personalize_fix_f2_selection_blocked.py ---------------------------------------

namespace {

const AppIdentity kKeepass = identity("keepassxc.exe", "KeePassXC", 21, "0x21");
const AppIdentity kNotepad = identity("notepad.exe", "Notepad", 4, "0x4");

} // namespace

// from
// tests/test_personalize_fix_f2_selection_blocked.py::test_a_selection_request_in_an_excluded_app_comes_back_refused
TEST_CASE("a selection request in an excluded app comes back refused",
          "[test_personalize_fix_f2_selection_blocked]") {
    const bool text_supported = GENERATE(true, false);
    CAPTURE(text_supported);
    Harness h(std::make_shared<FakeFocusPlatform>(kKeepass, true, text_supported),
              {.excluded = {"KeePassXC"}});

    const auto selection = snapshot(h.service->request(false, true));
    const auto dictation = snapshot(h.service->request(true, false));

    CHECK(selection.identity == kKeepass);
    REQUIRE(selection.text);
    CHECK(selection.text->blocked);
    CHECK_FALSE(selection.text->selection_known);
    CHECK_FALSE(selection.text->caret_known);
    // Dictation just gets no text, as before.
    CHECK(dictation == only(kKeepass));
    CHECK(h.platform->reads().empty());
}

// from tests/test_personalize_fix_f2_selection_blocked.py::test_other_apps_are_still_read
TEST_CASE("other apps are still read", "[test_personalize_fix_f2_selection_blocked]") {
    Harness h(std::make_shared<FakeFocusPlatform>(kNotepad), {.excluded = {"KeePassXC"}});

    const auto selection = snapshot(h.service->request(false, true));

    REQUIRE(selection.text);
    CHECK_FALSE(selection.text->blocked);
    const auto reads = h.platform->reads();
    REQUIRE(reads.size() == 1);
    CHECK(reads[0].same_call("notepad.exe", false, true));
}

// --- tests/test_personalize_fix_f3_reread_exclusion.py ----------------------------------------

namespace {

// Browser tabs share the window and the browser's pid; only the site differs.
const AppIdentity kGmail = identity("chrome.exe", "Chrome", 20, "0x5", "Gmail", "windows");

AppIdentity tab(const std::string& site) {
    AppIdentity other = kGmail;
    other.title_hint = site;
    return other;
}

} // namespace

// from
// tests/test_personalize_fix_f3_reread_exclusion.py::test_a_tab_switch_to_an_excluded_site_reads_nothing
TEST_CASE("a tab switch to an excluded site reads nothing",
          "[test_personalize_fix_f3_reread_exclusion]") {
    Harness h(std::make_shared<FakeFocusPlatform>(tab("WhatsApp")), {.excluded = {"WhatsApp"}});

    CHECK_FALSE(reread(*h.service, kGmail));
    CHECK(h.platform->reads().empty());
}

// from
// tests/test_personalize_fix_f3_reread_exclusion.py::test_a_tab_switch_to_another_site_reads_nothing
TEST_CASE("a tab switch to another site reads nothing",
          "[test_personalize_fix_f3_reread_exclusion]") {
    Harness h(std::make_shared<FakeFocusPlatform>(tab("GitHub")));

    CHECK_FALSE(reread(*h.service, kGmail));
    CHECK(h.platform->reads().empty());
}

// from
// tests/test_personalize_fix_f3_reread_exclusion.py::test_a_site_excluded_after_the_dictation_is_not_reread
TEST_CASE("a site excluded after the dictation is not reread",
          "[test_personalize_fix_f3_reread_exclusion]") {
    Harness h(std::make_shared<FakeFocusPlatform>(kGmail));
    CHECK(reread(*h.service, kGmail) == kContext);

    h.exclude("Gmail");

    CHECK_FALSE(reread(*h.service, kGmail));
    CHECK(h.platform->reads().size() == 1);
}

// from tests/test_personalize_fix_f3_reread_exclusion.py::test_the_same_tab_is_still_reread
TEST_CASE("the same tab is still reread", "[test_personalize_fix_f3_reread_exclusion]") {
    Harness h(std::make_shared<FakeFocusPlatform>(kGmail), {.excluded = {"WhatsApp"}});

    CHECK(reread(*h.service, kGmail) == kContext);
    const auto reads = h.platform->reads();
    REQUIRE(reads.size() == 1);
    CHECK(reads[0].same_call("chrome.exe", true, true));
}
