// The download consent coordinator, the download error text and the inventory snapshot: the 26
// tests of OpenWhisper's tests/test_hf_access.py that FioriVoce keeps, and the vectors of
// services.hf_access.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "base/identity.h"
#include "base/testing/fake_clock.h"
#include "base/testing/map_env_reader.h"
#include "download/verified_download.h"
#include "golden/golden.h"
#include "history/format_utils.h"
#include "http/testing/fake_http_client.h"
#include "models/catalog.h"
#include "models/hf_access.h"
#include "models/hf_access_policy.h"
#include "models_test_support.h"
#include "settings/testing/memory_store.h"

using namespace fv::models;
using namespace fv::models::test;
using fv::Json;
namespace golden = fv::golden;

namespace {

constexpr std::string_view kModel = "parakeet-v3";
constexpr std::string_view kOther = "orukeet-v0.1";

// A coordinator for a model missing from the cache (the class-level patches of
// TestEvaluateAccess), with the policy saved in an in-memory settings file.
struct Coordinator {
    fv::MemorySettingsStore settings;
    fv::MapEnvReader env;
    std::set<std::string, std::less<>> cached;
    HuggingFaceAccessCoordinator coordinator{
        settings, [this](std::string_view name) { return cached.contains(name); }, env};

    void set_policy(std::string_view policy) { save_hf_access_policy(settings, policy); }
};

std::vector<std::string> names(std::initializer_list<std::string_view> list) {
    return {list.begin(), list.end()};
}

CachedModelInfo info(std::string repo, std::int64_t size, std::vector<std::string> revisions) {
    CachedModelInfo cached;
    cached.repo_id = std::move(repo);
    cached.size_on_disk = size;
    cached.path = "/models";
    cached.revisions = std::move(revisions);
    return cached;
}

} // namespace

// --- TestHelpers ---

TEST_CASE("TestHelpers::test_format_download_size_known_models", "[test_hf_access]") {
    // from tests/test_hf_access.py::TestHelpers::test_format_download_size_known_models, over the
    // speech models (OpenWhisper's asks Whisper's base and turbo).
    CHECK(format_download_size("parakeet-v3") == "~714 MB");
    CHECK(format_download_size("orukeet-v0.1") == "~714 MB");
}

TEST_CASE("TestHelpers::test_format_download_size_unknown_model", "[test_hf_access]") {
    CHECK_FALSE(format_download_size("some/custom-repo"));
}

// --- TestEvaluateAccess ---

TEST_CASE("TestEvaluateAccess::test_cached_model_always_loads_locally", "[test_hf_access]") {
    Coordinator c;
    c.cached.insert(std::string(kModel));
    for (const std::string_view policy : hf_access_policy::kAll) {
        c.set_policy(policy);
        CHECK(c.coordinator.evaluate_access(kModel) == AccessDecision::LoadCached);
    }
}

TEST_CASE("TestEvaluateAccess::test_ask_policy_needs_consent", "[test_hf_access]") {
    Coordinator c;
    c.set_policy(hf_access_policy::kAsk);
    CHECK(c.coordinator.evaluate_access(kModel) == AccessDecision::NeedsConsent);
}

TEST_CASE("TestEvaluateAccess::test_never_policy_needs_consent", "[test_hf_access]") {
    // "never" still surfaces the dialog, with its Download once override.
    Coordinator c;
    c.set_policy(hf_access_policy::kNever);
    CHECK(c.coordinator.evaluate_access(kModel) == AccessDecision::NeedsConsent);
}

TEST_CASE("TestEvaluateAccess::test_always_policy_allows_download", "[test_hf_access]") {
    Coordinator c;
    c.set_policy(hf_access_policy::kAlways);
    CHECK(c.coordinator.evaluate_access(kModel) == AccessDecision::DownloadAllowed);
}

TEST_CASE("TestEvaluateAccess::test_env_override_blocks_even_with_grant", "[test_hf_access]") {
    Coordinator c;
    c.env.set("HF_HUB_OFFLINE", "1");
    c.set_policy(hf_access_policy::kAlways);
    c.coordinator.grant_once(kModel);
    CHECK(c.coordinator.evaluate_access(kModel) == AccessDecision::BlockedByEnv);
}

TEST_CASE("TestEvaluateAccess::test_one_time_grant_is_consumed_once", "[test_hf_access]") {
    Coordinator c;
    c.set_policy(hf_access_policy::kAsk);
    c.coordinator.grant_once(kModel);
    CHECK(c.coordinator.evaluate_access(kModel) == AccessDecision::DownloadAllowed);
    // The grant is spent: a second request needs fresh consent.
    CHECK(c.coordinator.evaluate_access(kModel) == AccessDecision::NeedsConsent);
}

TEST_CASE("TestEvaluateAccess::test_advisory_check_preserves_grant", "[test_hf_access]") {
    Coordinator c;
    c.set_policy(hf_access_policy::kAsk);
    c.coordinator.grant_once(kModel);
    CHECK(c.coordinator.evaluate_access(kModel, true) == AccessDecision::DownloadAllowed);
    // The grant survives the advisory check and is consumed here.
    CHECK(c.coordinator.evaluate_access(kModel) == AccessDecision::DownloadAllowed);
}

TEST_CASE("TestEvaluateAccess::test_grant_applies_only_to_that_model", "[test_hf_access]") {
    Coordinator c;
    c.set_policy(hf_access_policy::kAsk);
    c.coordinator.grant_once(kModel);
    CHECK(c.coordinator.evaluate_access(kOther) == AccessDecision::NeedsConsent);
}

// --- TestDescribeDownloadError ---

TEST_CASE("TestDescribeDownloadError::test_offline", "[test_hf_access]") {
    // As the download relays them: text only, or an OSError.
    const std::vector<std::vector<ErrorCause>> chains{
        {{"[Errno -3] Temporary failure in name resolution", {}, {}, false}},
        {{"[WinError 11001] getaddrinfo failed", {}, {}, false}},
        {{"Could not reach the download server (timed out).", {}, {}, false}},
        {{"[Errno 101] Network is unreachable", 101, {}, false}},
    };
    for (const auto& chain : chains) {
        const std::string message = describe_model_download_error(kModel, chain);
        CHECK(message.starts_with("Couldn't reach the download server for 'parakeet-v3'."));
        CHECK(message.ends_with(" in Downloads."));
        CHECK(message.find("Errno") == std::string::npos);
    }
}

TEST_CASE("TestDescribeDownloadError::test_disk_full_names_the_size", "[test_hf_access]") {
    const std::vector<std::vector<ErrorCause>> chains{
        {{"[Errno 28] No space left on device", {}, {}, false}},
        {{"[Errno 28] No space left on device", 28, {}, false}},
    };
    for (const auto& chain : chains) {
        const std::string message = describe_model_download_error(kModel, chain);
        CHECK(message.starts_with("Not enough disk space for 'parakeet-v3' (about 714 MB)."));
        CHECK(message.ends_with(" in Downloads."));
    }
    // A real failure from the filesystem reads the same.
    const auto full = std::make_exception_ptr(std::filesystem::filesystem_error(
        "write", std::make_error_code(std::errc::no_space_on_device)));
    CHECK(describe_model_download_error(kModel, full)
              .starts_with("Not enough disk space for 'parakeet-v3' (about 714 MB)."));
}

TEST_CASE("TestDescribeDownloadError::test_anything_else_points_at_the_log", "[test_hf_access]") {
    const std::string message = describe_model_download_error(
        kModel, std::make_exception_ptr(std::runtime_error("HTTP 500")));
    CHECK(message.find(std::string(fv::identity().log_file)) != std::string::npos);
    CHECK(message.find("HTTP 500") == std::string::npos);
}

// --- TestFormatSizeBytes ---

TEST_CASE("TestFormatSizeBytes::test_boundaries", "[test_hf_access]") {
    CHECK(fv::format_size_bytes(512) == "512 B");
    CHECK(fv::format_size_bytes(145'000'000) == "145 MB");
    CHECK(fv::format_size_bytes(1'530'000'000) == "1.53 GB");
    CHECK(fv::format_size_bytes(12'000) == "12 KB");
}

// --- TestScanCachedModels (over the speech-model inventory, not the Hugging Face hub) ---

TEST_CASE("TestScanCachedModels::test_maps_repos_by_repo_id", "[test_hf_access]") {
    CachedModelsSnapshot snapshot([] {
        return std::vector{info("nvidia/parakeet-tdt-0.6b-v3", 714'000'000, {"abc"}),
                           info("oruk/orukeet", 714'456'704, {"d1", "d2"})};
    });
    const auto cached = snapshot.scan();
    REQUIRE(cached.size() == 2);
    CHECK(cached.contains("nvidia/parakeet-tdt-0.6b-v3"));
    CHECK(cached.at("nvidia/parakeet-tdt-0.6b-v3").size_on_disk == 714'000'000);
    CHECK(cached.at("oruk/orukeet").revisions == std::vector<std::string>{"d1", "d2"});
}

TEST_CASE("TestScanCachedModels::test_missing_cache_dir_returns_empty", "[test_hf_access]") {
    TempDir temp;
    fv::http::FakeHttpClient client;
    SpeechModelCache cache(temp.path() / "no-such-folder", client);
    CachedModelsSnapshot snapshot(cache);
    CHECK(snapshot.scan().empty());
}

TEST_CASE("TestScanCachedModels::test_positive_max_age_reuses_snapshot_and_returns_copies",
          "[test_hf_access]") {
    fv::FakeClock clock;
    int scans = 0;
    CachedModelsSnapshot snapshot(
        [&scans] {
            ++scans;
            return std::vector{info("nvidia/parakeet-tdt-0.6b-v3", 10, {"abc"})};
        },
        clock);
    auto first = snapshot.scan(30);
    first.clear();
    const auto second = snapshot.scan(30);
    CHECK(scans == 1);
    CHECK(second.size() == 1);
    REQUIRE(snapshot.peek());
    CHECK(snapshot.peek()->contains("nvidia/parakeet-tdt-0.6b-v3"));
    // Older than the allowed age, the inventory is read again.
    clock.advance(std::chrono::seconds(31));
    snapshot.scan(30);
    CHECK(scans == 2);
    // Without a positive age every call reads it.
    snapshot.scan();
    CHECK(scans == 3);
    snapshot.invalidate();
    CHECK_FALSE(snapshot.peek());
}

TEST_CASE("TestScanCachedModels::test_peek_does_not_wait_for_a_cold_directory_walk",
          "[test_hf_access]") {
    std::mutex mutex;
    std::condition_variable changed;
    bool started = false;
    bool release = false;
    CachedModelsSnapshot snapshot([&] {
        std::unique_lock lock(mutex);
        started = true;
        changed.notify_all();
        changed.wait_for(lock, std::chrono::seconds(5), [&] { return release; });
        return std::vector<CachedModelInfo>{};
    });
    auto worker = std::async(std::launch::async, [&snapshot] { return snapshot.scan(30); });
    {
        std::unique_lock lock(mutex);
        REQUIRE(changed.wait_for(lock, std::chrono::seconds(5), [&] { return started; }));
    }
    const auto before = std::chrono::steady_clock::now();
    CHECK_FALSE(snapshot.peek());
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(100));
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    changed.notify_all();
    REQUIRE(worker.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    CHECK(worker.get().empty());
}

TEST_CASE("concurrent scans share one walk and a raced scan is taken again", "[test_hf_access]") {
    std::mutex mutex;
    std::condition_variable changed;
    int calls = 0;
    bool release = false;
    CachedModelsSnapshot* self = nullptr;
    CachedModelsSnapshot snapshot([&] {
        std::unique_lock lock(mutex);
        const int call = ++calls;
        changed.notify_all();
        if (call == 1) {
            changed.wait_for(lock, std::chrono::seconds(5), [&] { return release; });
            return std::vector{info("stale/repo", 1, {})};
        }
        return std::vector{info("fresh/repo", 2, {})};
    });
    self = &snapshot;
    auto first = std::async(std::launch::async, [self] { return self->scan(30); });
    {
        std::unique_lock lock(mutex);
        REQUIRE(changed.wait_for(lock, std::chrono::seconds(5), [&] { return calls == 1; }));
    }
    auto second = std::async(std::launch::async, [self] { return self->scan(30); });
    // A download finished while the first walk was reading the disk.
    snapshot.invalidate();
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    changed.notify_all();
    REQUIRE(first.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(second.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    CHECK(first.get().contains("fresh/repo"));
    CHECK(second.get().contains("fresh/repo"));
    CHECK(calls == 2);
}

TEST_CASE("a failed inventory read frees the scan for the next caller", "[test_hf_access]") {
    bool fail = true;
    CachedModelsSnapshot snapshot([&fail] {
        if (fail) {
            throw std::runtime_error("disk gone");
        }
        return std::vector{info("nvidia/parakeet-tdt-0.6b-v3", 1, {})};
    });
    CHECK_THROWS_AS(snapshot.scan(), std::runtime_error);
    fail = false;
    CHECK(snapshot.scan().size() == 1);
}

// --- TestRequestDeduplication ---

TEST_CASE("TestRequestDeduplication::test_begin_request_claims_and_rejects_duplicates",
          "[test_hf_access]") {
    Coordinator c;
    CHECK(c.coordinator.begin_request(kModel));
    CHECK_FALSE(c.coordinator.begin_request(kModel));
    // A different model gets its own slot.
    CHECK(c.coordinator.begin_request(kOther));
}

TEST_CASE("TestRequestDeduplication::test_end_request_releases_slot", "[test_hf_access]") {
    Coordinator c;
    CHECK(c.coordinator.begin_request(kModel));
    c.coordinator.end_request(kModel);
    CHECK(c.coordinator.begin_request(kModel));
}

TEST_CASE("TestRequestDeduplication::test_end_request_for_unclaimed_model_is_harmless",
          "[test_hf_access]") {
    Coordinator c;
    c.coordinator.end_request("never-claimed");
    CHECK(c.coordinator.begin_request("never-claimed"));
}

// --- TestClaimBatch ---

TEST_CASE("TestClaimBatch::test_claims_every_downloadable_model", "[test_hf_access]") {
    Coordinator c;
    const auto batch = names({"tiny", "base", "small"});
    CHECK(c.coordinator.claim_batch(batch) == batch);
    // Slots stay held so a concurrent request cannot double-download.
    CHECK_FALSE(c.coordinator.begin_request("base"));
}

TEST_CASE("TestClaimBatch::test_skips_cached_models", "[test_hf_access]") {
    Coordinator c;
    c.cached.insert("tiny");
    CHECK(c.coordinator.claim_batch(names({"tiny", "base"})) == names({"base"}));
}

TEST_CASE("TestClaimBatch::test_skips_duplicates_in_the_request", "[test_hf_access]") {
    Coordinator c;
    CHECK(c.coordinator.claim_batch(names({"tiny", "tiny"})) == names({"tiny"}));
}

TEST_CASE("TestClaimBatch::test_skips_models_already_in_flight", "[test_hf_access]") {
    Coordinator c;
    CHECK(c.coordinator.begin_request("base"));
    CHECK(c.coordinator.claim_batch(names({"base", "tiny"})) == names({"tiny"}));
}

TEST_CASE("TestClaimBatch::test_requests_in_flight_counts_claimed_slots", "[test_hf_access]") {
    Coordinator c;
    CHECK(c.coordinator.requests_in_flight() == 0);
    c.coordinator.claim_batch(names({"tiny", "base"}));
    CHECK(c.coordinator.requests_in_flight() == 2);
    c.coordinator.end_request("tiny");
    CHECK(c.coordinator.requests_in_flight() == 1);
}

// --- FioriVoce's own ---

TEST_CASE("the policy is the saved one and set_policy saves it", "[test_hf_access]") {
    Coordinator c;
    CHECK(c.coordinator.policy() == "ask");
    c.coordinator.set_policy("always");
    CHECK(c.coordinator.policy() == "always");
    CHECK(c.settings.contents().at("hf_access_policy") == "always");
    CHECK_THROWS_AS(c.coordinator.set_policy("sometimes"), std::invalid_argument);
}

TEST_CASE("decision and consent names are OpenWhisper's", "[test_hf_access]") {
    CHECK(access_decision_name(AccessDecision::LoadCached) == "load_cached");
    CHECK(access_decision_name(AccessDecision::DownloadAllowed) == "download_allowed");
    CHECK(access_decision_name(AccessDecision::NeedsConsent) == "needs_consent");
    CHECK(access_decision_name(AccessDecision::BlockedByEnv) == "blocked_by_env");
    CHECK(consent_action_name(ConsentAction::Cancel) == "cancel");
    CHECK(consent_action_name(ConsentAction::DownloadOnce) == "download_once");
    CHECK(consent_action_name(ConsentAction::AlwaysAllow) == "always_allow");
    CHECK(consent_action_name(ConsentAction::OpenSettings) == "open_settings");
}

TEST_CASE("error_chain reads nested exceptions, cancels and download failures",
          "[test_hf_access]") {
    std::exception_ptr nested;
    try {
        try {
            throw std::system_error(std::make_error_code(std::errc::no_space_on_device), "write");
        } catch (...) {
            std::throw_with_nested(std::runtime_error("install failed"));
        }
    } catch (...) {
        nested = std::current_exception();
    }
    const auto chain = error_chain(nested);
    REQUIRE(chain.size() == 2);
    CHECK(chain[0].text == "install failed");
    CHECK(chain[1].errno_value == 28);
    CHECK(describe_model_download_error(kModel, nested).starts_with("Not enough disk space"));

    const auto canceled = std::make_exception_ptr(ComponentCanceled());
    CHECK(describe_model_download_error(kModel, canceled) ==
          "Download of 'parakeet-v3' was canceled.");

    fv::download::NetworkFailure failure;
    failure.kind = fv::download::NetworkFailure::Kind::Url;
    failure.reason = "[WinError 11001] getaddrinfo failed";
    const auto offline = std::make_exception_ptr(fv::download::DownloadError("failed", failure));
    CHECK(describe_model_download_error(kModel, offline)
              .starts_with("Couldn't reach the download server"));

    // At most eight links are read.
    std::vector<ErrorCause> long_chain(8, ErrorCause{"link", {}, {}, false});
    long_chain.push_back({"canceled", {}, {}, false});
    CHECK(describe_model_download_error(kModel, long_chain).starts_with("Couldn't download"));
}

TEST_CASE("concurrent requests get one slot and one grant per model", "[test_hf_access]") {
    // Model loads and consent requests arrive on worker threads while dialogs run on the GUI
    // thread: a claim or a one-time grant must go to exactly one of them.
    Coordinator c;
    c.set_policy(hf_access_policy::kAsk);
    c.coordinator.grant_once(kModel);
    std::atomic<int> slots{0};
    std::atomic<int> allowed{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&] {
            while (!go.load()) {
                std::this_thread::yield();
            }
            for (int k = 0; k < 50; ++k) {
                if (c.coordinator.begin_request(kModel)) {
                    ++slots;
                }
                if (c.coordinator.evaluate_access(kModel) == AccessDecision::DownloadAllowed) {
                    ++allowed;
                }
            }
        });
    }
    go = true;
    for (std::thread& thread : threads) {
        thread.join();
    }
    CHECK(slots == 1);
    CHECK(allowed == 1);
    CHECK(c.coordinator.requests_in_flight() == 1);
}

TEST_CASE("a cancelled download and a full Windows disk read as such", "[test_hf_access]") {
    const auto canceled =
        std::make_exception_ptr(fv::download::DownloadCanceled("The download was canceled."));
    CHECK(describe_model_download_error(kModel, canceled) ==
          "Download of 'parakeet-v3' was canceled.");
    // A disk failure the download relays as text keeps Python's wording.
    fv::download::NetworkFailure full;
    full.reason = "[Errno 28] No space left on device";
    const auto relayed = std::make_exception_ptr(fv::download::DownloadError(full.reason, full));
    CHECK(describe_model_download_error(kModel, relayed)
              .starts_with("Not enough disk space for 'parakeet-v3' (about 714 MB)."));
    CHECK(describe_model_download_error(kModel, std::exception_ptr()) ==
          "Couldn't download 'parakeet-v3' (details in " + std::string(fv::identity().log_file) +
              "). Retry in Downloads.");
#ifdef _WIN32
    // ERROR_DISK_FULL as std::filesystem reports it on Windows: the system error code is the
    // winerror Python's OSError carries.
    const auto disk = std::make_exception_ptr(
        std::system_error(std::error_code(112, std::system_category()), "write"));
    const auto chain = error_chain(disk);
    REQUIRE(chain.size() == 1);
    CHECK(chain[0].winerror == 112);
    CHECK(describe_model_download_error(kModel, disk)
              .starts_with("Not enough disk space for 'parakeet-v3' (about 714 MB)."));
#endif
}

// --- Golden vectors ---

TEST_CASE("services.hf_access.evaluate_access vectors", "[golden]") {
    golden::for_each("services.hf_access", "evaluate_access", [](const golden::Case& c) {
        Coordinator coordinator;
        if (c.arg<bool>(0)) {
            coordinator.cached.insert(std::string(kModel));
        }
        if (!c.args()[1].is_null()) {
            coordinator.env.set("HF_HUB_OFFLINE", c.arg<std::string>(1));
        }
        coordinator.set_policy(c.arg<std::string>(3));
        if (c.arg<bool>(2)) {
            coordinator.coordinator.grant_once(kModel);
        }
        const AccessDecision first =
            coordinator.coordinator.evaluate_access(kModel, c.arg<bool>(4));
        const AccessDecision second = coordinator.coordinator.evaluate_access(kModel);
        CHECK(golden::equal(c.result<Json>(), Json::array({access_decision_name(first),
                                                           access_decision_name(second)})));
    });
}

TEST_CASE("services.hf_access.describe_model_download_error vectors", "[golden]") {
    golden::for_each("services.hf_access", "describe_model_download_error",
                     [](const golden::Case& c) {
                         const Json result = c.result<Json>();
                         std::vector<ErrorCause> chain;
                         for (const Json& link : result.at("chain")) {
                             ErrorCause cause;
                             cause.text = link.at("text").get<std::string>();
                             if (!link.at("errno").is_null()) {
                                 cause.errno_value = link.at("errno").get<int>();
                             }
                             if (!link.at("winerror").is_null()) {
                                 cause.winerror = link.at("winerror").get<int>();
                             }
                             cause.network = link.at("network").get<bool>();
                             chain.push_back(std::move(cause));
                         }
                         CHECK(golden::equal_rebranded(
                             result.at("message").get<std::string>(),
                             describe_model_download_error(c.arg<std::string>(0), chain)));
                     });
}

TEST_CASE("services.hf_access.format_download_size vectors", "[golden]") {
    golden::for_each("services.hf_access", "format_download_size", [](const golden::Case& c) {
        const auto size = format_download_size(c.arg<std::string>(0));
        CHECK(golden::equal(c.result<Json>(), size ? Json(*size) : Json()));
    });
}
