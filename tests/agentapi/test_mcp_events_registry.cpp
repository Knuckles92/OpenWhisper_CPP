// MCP Events' saved subscriptions (tests/test_automation_fix_mcp_events_registry.py and
// test_automation_fix_mcp_events_rollback.py): no waiting on slow storage, no expired
// leftovers, and a save that failed leaves the secret as it was.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#include "agentapi/mcp_events.h"
#include "agentapi/mcp_events_registry.h"
#include "automation/catalog.h"
#include "base/clock.h"
#include "base/testing/fake_clock.h"
#include "credentials/store.h"
#include "credentials/testing/memory_backend.h"
#include "events_support.h"
#include "pyport/json.h"
#include "settings/testing/memory_store.h"
#include "testkit/testkit.h"

namespace {

using fv::Json;
using namespace std::chrono_literals;
using namespace fv::agentapi;
using namespace fv::agentapi::events_ext;
using namespace fv::agentapi::test;
namespace catalog = fv::automation::catalog;

const std::string kSecret = "whsec_" + std::string(32, 'A');

// A credential backend whose writes stall for a second while `slow` is set.
class SlowBackend final : public fv::MemoryCredentialBackend {
public:
    void set(std::string_view service, std::string_view name, std::string_view value) override {
        stall();
        MemoryCredentialBackend::set(service, name, value);
    }
    bool remove(std::string_view service, std::string_view name) override {
        stall();
        return MemoryCredentialBackend::remove(service, name);
    }

    std::atomic<bool> slow{false};
    std::atomic<bool> entered{false};

private:
    void stall() {
        if (slow) {
            entered = true;
            std::this_thread::sleep_for(1s);
        }
    }
};

double now_seconds(const fv::Clock& clock = fv::system_clock()) {
    return std::chrono::duration<double>(clock.utc().time_since_epoch()).count();
}

EventSubscription subscription(int n, double expires_in = 3600.0,
                               const fv::Clock& clock = fv::system_clock()) {
    EventSubscription out;
    char id[64];
    std::snprintf(id, sizeof id, "sub_%032x", n);
    out.id = id;
    out.name = std::string(catalog::TRANSCRIPTION_COMPLETED);
    out.url = "http://127.0.0.1:9/" + std::to_string(n);
    out.refresh_before = static_cast<std::int64_t>(now_seconds(clock) + expires_in);
    out.origin = std::string(ORIGIN_LOCAL);
    return out;
}

template <class Fn> auto timed(Fn&& fn) {
    const auto started = std::chrono::steady_clock::now();
    auto result = fn();
    return std::pair{result, std::chrono::steady_clock::now() - started};
}

// McpEvents over a slow credential store, settings with MCP Events on, and its own bus.
struct SlowRig {
    fv::testkit::TempDataRoot root;
    fv::MemorySettingsStore settings{EventsRig::default_settings()};
    std::shared_ptr<SlowBackend> backend = std::make_shared<SlowBackend>();
    fv::CredentialStore store{backend, fv::identity()};
    fv::automation::EventBus bus;
    std::unique_ptr<McpEvents> events;

    explicit SlowRig(const fv::Clock& clock = fv::system_clock()) { restart(clock); }
    ~SlowRig() {
        events.reset();
        bus.close();
    }
    void restart(const fv::Clock& clock = fv::system_clock()) {
        events.reset();
        events = std::make_unique<McpEvents>(settings, store, bus, file(),
                                             fv::http::make_curl_client(), clock);
    }
    std::filesystem::path file() const { return root.path() / "mcp_event_subscriptions.json"; }

    // Gone from the credential store and from the file.
    bool forgotten(const std::string& id) {
        std::ifstream in(file(), std::ios::binary);
        if (!in) {
            // Windows refuses a read while the registry is replacing the file on its own
            // thread. Not forgotten yet as far as this look can tell.
            return false;
        }
        std::ostringstream text;
        text << in.rdbuf();
        return !store.get(secret_name(id)) && text.str().find(id) == std::string::npos;
    }

    // Marks a saved subscription expired, as the file would read after a restart.
    void expire_saved(const std::string& id) {
        std::ifstream in(file(), std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        in.close();
        Json saved = fv::py::loads(text.str());
        for (Json& record : saved.at("subscriptions")) {
            if (record.at("id") == id) {
                record["refresh_before"] = "2020-01-01T00:00:00Z";
            }
        }
        std::ofstream out(file(), std::ios::binary | std::ios::trunc);
        out << fv::py::dumps(saved);
    }

    // One live and one expired subscription, as a restarted app finds them.
    std::pair<EventSubscription, EventSubscription> saved_then_expired() {
        const auto live = subscription(1);
        const auto old = subscription(2);
        events->registry().upsert(live, kSecret);
        events->registry().upsert(old, kSecret);
        expire_saved(old.id);
        restart();
        return {live, old};
    }
};

} // namespace

TEST_CASE("test_slow_secret_removal_blocks_neither_counting_nor_the_bus",
          "[test_automation_fix_mcp_events_registry]") {
    SlowRig rig;
    rig.events->start_delivery();
    auto& registry = rig.events->registry();
    for (int n = 0; n < 3; ++n) {
        registry.upsert(subscription(n), kSecret);
    }
    std::mutex mutex;
    std::condition_variable arrived;
    std::map<std::int64_t, std::chrono::steady_clock::time_point> received;
    auto other = rig.bus.subscribe(
        [&](const fv::automation::Event& event) {
            std::lock_guard lock(mutex);
            received[event.seq] = std::chrono::steady_clock::now();
            arrived.notify_all();
        },
        std::nullopt, "other");

    rig.backend->slow = true;
    std::thread clearing([&] { rig.events->clear_subscriptions(); });
    REQUIRE(eventually([&] { return rig.backend->entered.load(); }));

    const auto [count, elapsed] = timed([&] { return rig.events->subscription_count(); });
    CHECK(elapsed < 300ms);
    CHECK(count == 0); // the clear already took them out of memory

    const auto published_at = std::chrono::steady_clock::now();
    const auto event = rig.bus.publish_event(catalog::TRANSCRIPTION_COMPLETED,
                                             Json{{"job_id", "j"}, {"text", "x"}});
    {
        std::unique_lock lock(mutex);
        REQUIRE(arrived.wait_for(lock, 5s, [&] { return received.contains(event.seq); }));
        CHECK(received.at(event.seq) - published_at < 300ms);
    }
    clearing.join();
    other->close();
}

TEST_CASE("test_a_slow_save_of_a_new_subscription_does_not_block_counting",
          "[test_automation_fix_mcp_events_registry]") {
    SlowRig rig;
    rig.events->start_delivery();
    auto& registry = rig.events->registry();
    registry.upsert(subscription(1), kSecret);
    rig.backend->slow = true;
    std::thread saving([&] { registry.upsert(subscription(2), "whsec_" + std::string(32, 'B')); });
    REQUIRE(eventually([&] { return rig.backend->entered.load(); }));
    const auto [count, elapsed] = timed([&] { return rig.events->subscription_count(); });
    CHECK(elapsed < 300ms);
    CHECK(count == 1);
    saving.join();
    CHECK(rig.events->subscription_count() == 2);
}

TEST_CASE("test_a_slow_fsync_does_not_block_counting",
          "[test_automation_fix_mcp_events_registry]") {
    SlowRig rig;
    rig.events->start_delivery();
    auto& registry = rig.events->registry();
    registry.upsert(subscription(1), kSecret);
    std::atomic<bool> entered{false};
    // The file write stalls as an fsync can.
    registry.set_writer([&](const std::filesystem::path& path, const std::string& bytes) {
        entered = true;
        std::this_thread::sleep_for(1s);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << bytes;
    });
    std::thread saving([&] { registry.remove(subscription(1).id); });
    REQUIRE(eventually([&] { return entered.load(); }));
    const auto [count, elapsed] = timed([&] { return rig.events->subscription_count(); });
    CHECK(elapsed < 300ms);
    CHECK(count == 0);
    saving.join();
}

TEST_CASE("test_starting_delivery_forgets_expired_subscriptions",
          "[test_automation_fix_mcp_events_registry]") {
    SlowRig rig;
    const auto [live, old] = rig.saved_then_expired();
    rig.events->start_delivery();
    CHECK(eventually([&] { return rig.forgotten(old.id); }));
    CHECK(rig.store.get(secret_name(live.id)) == kSecret);
    CHECK(rig.events->subscription_count() == 1);
}

TEST_CASE("test_counting_forgets_expired_subscriptions",
          "[test_automation_fix_mcp_events_registry]") {
    SlowRig rig;
    const auto [live, old] = rig.saved_then_expired();
    CHECK(rig.events->subscription_count() == 1);
    CHECK(eventually([&] { return rig.forgotten(old.id); }));
    CHECK(rig.store.get(secret_name(live.id)) == kSecret);
}

TEST_CASE("test_a_subscription_that_expires_while_running_is_forgotten_when_counted",
          "[test_automation_fix_mcp_events_registry]") {
    // Wall time moves only when the test says, so the expiry needs no sleep.
    fv::FakeClock clock(std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        std::chrono::system_clock::now()));
    SlowRig rig(clock);
    rig.events->start_delivery();
    const auto soon = subscription(3, 1.2, clock); // whole seconds: expires 0.2 to 1.2 s from now
    rig.events->registry().upsert(soon, kSecret);
    CHECK(rig.events->subscription_count() == 1);
    clock.advance(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(
            static_cast<double>(soon.refresh_before) - now_seconds(clock) + 0.05)));
    CHECK(rig.events->subscription_count() == 0);
    CHECK(eventually([&] { return rig.forgotten(soon.id); }));
}

// -- rollback (test_automation_fix_mcp_events_rollback.py) ---------------------------------------

namespace {

const std::string kOld = "whsec_" + std::string(32, 'A');
const std::string kNewer = "whsec_" + std::string(32, 'B');
const std::string kRefused = "whsec_" + std::string(32, 'C');

struct RollbackRig {
    fv::testkit::TempDataRoot root;
    MemoryCredentials credentials;
    Registry registry{root.path() / "subscriptions.json", credentials.store, fv::system_clock()};

    static EventSubscription subscription(double expires_in = 3600.0) {
        EventSubscription out;
        out.id = "sub_" + std::string(32, '1');
        out.name = std::string(catalog::HISTORY_SAVED);
        out.url = "https://example.com/hook";
        out.refresh_before = static_cast<std::int64_t>(now_seconds() + expires_in);
        out.origin = std::string(ORIGIN_LOCAL);
        return out;
    }
    void failing_writes() {
        registry.set_writer([](const std::filesystem::path& path, const std::string&) {
            throw std::filesystem::filesystem_error(
                "disk full", path, std::make_error_code(std::errc::no_space_on_device));
        });
    }
    std::optional<std::string> secret() {
        return credentials.store.get(secret_name(subscription().id));
    }
};

} // namespace

TEST_CASE("test_a_refresh_that_could_not_be_saved_keeps_the_old_secret",
          "[test_automation_fix_mcp_events_rollback]") {
    RollbackRig rig;
    const auto saved = RollbackRig::subscription();
    rig.registry.upsert(saved, kOld);

    rig.failing_writes();
    CHECK_THROWS_AS(rig.registry.upsert(RollbackRig::subscription(7200.0), kRefused),
                    std::system_error);

    CHECK(rig.secret() == kOld);
    CHECK(rig.registry.signing_secrets(saved.id) == std::vector<std::string>{kOld});
    CHECK(rig.registry.get(saved.id) == saved);
}

TEST_CASE("test_a_rotation_in_progress_survives_a_refresh_that_failed",
          "[test_automation_fix_mcp_events_rollback]") {
    RollbackRig rig;
    const auto saved = RollbackRig::subscription();
    rig.registry.upsert(saved, kOld);
    rig.registry.upsert(saved, kNewer); // a rotation: kOld still signs for a while
    CHECK(rig.registry.signing_secrets(saved.id) == std::vector<std::string>{kNewer, kOld});

    rig.failing_writes();
    CHECK_THROWS_AS(rig.registry.upsert(saved, kRefused), std::system_error);

    CHECK(rig.secret() == kNewer);
    CHECK(rig.registry.signing_secrets(saved.id) == std::vector<std::string>{kNewer, kOld});
}

TEST_CASE("test_a_refresh_with_the_same_secret_that_failed_changes_nothing",
          "[test_automation_fix_mcp_events_rollback]") {
    RollbackRig rig;
    const auto saved = RollbackRig::subscription();
    rig.registry.upsert(saved, kOld);
    rig.failing_writes();
    CHECK_THROWS_AS(rig.registry.upsert(RollbackRig::subscription(7200.0), kOld),
                    std::system_error);
    CHECK(rig.secret() == kOld);
    CHECK(rig.registry.signing_secrets(saved.id) == std::vector<std::string>{kOld});
}

TEST_CASE("test_a_new_subscription_that_could_not_be_saved_leaves_no_secret",
          "[test_automation_fix_mcp_events_rollback]") {
    RollbackRig rig;
    rig.failing_writes();
    CHECK_THROWS_AS(rig.registry.upsert(RollbackRig::subscription(), kOld), std::system_error);
    CHECK_FALSE(rig.secret());
    CHECK(rig.registry.active().empty());
}

TEST_CASE("a damaged or missing subscriptions file loads as no subscriptions") {
    fv::testkit::TempDataRoot root;
    MemoryCredentials credentials;
    const auto file = root.path() / "mcp_event_subscriptions.json";
    {
        Registry registry(file, credentials.store, fv::system_clock());
        CHECK(registry.active().empty());
    }
    for (const std::string text :
         {"{", "[]", "\xEF\xBB\xBF{}", "{\"subscriptions\": 5}", "\xff\xfe"}) {
        std::ofstream(file, std::ios::binary | std::ios::trunc) << text;
        Registry registry(file, credentials.store, fv::system_clock());
        CHECK(registry.active().empty());
    }
    // Saved as OpenWhisper writes it: indent 2, non-ASCII kept, a final newline.
    Registry registry(file, credentials.store, fv::system_clock());
    auto saved = RollbackRig::subscription();
    saved.arguments = Json::object();
    saved.url = "https://example.com/caf\xC3\xA9";
    saved.created_at = "2026-01-01T00:00:00Z";
    registry.upsert(saved, kOld);
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    const std::string expected =
        "{\n  \"version\": 1,\n  \"subscriptions\": [\n    {\n      \"id\": \"" + saved.id +
        "\",\n      \"name\": \"history.saved\",\n      \"arguments\": {},\n      \"url\": "
        "\"https://example.com/caf\xC3\xA9\",\n      \"refresh_before\": \"" +
        iso(static_cast<double>(saved.refresh_before)) +
        "\",\n      \"created_at\": \"2026-01-01T00:00:00Z\",\n      \"origin\": \"local\"\n    "
        "}\n  ]\n}\n";
    CHECK(text.str() == expected);
    Registry again(file, credentials.store, fv::system_clock());
    CHECK(again.get(saved.id) == saved);
}
