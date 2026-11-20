// The client's errors (services/remote_asr/client.py, remote_history/channel.py), the engine
// card's data (transcriber/remote_backend.py), the helpers of the UI contracts
// (services/remote_asr/service.py's TailnetHost and NearbyHost) and the fakes later packages
// build their pages and runtimes on.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/executor.h"
#include "base/testing/manual_executor.h"
#include "remote/client_history_source.h"
#include "remote/control.h"
#include "remote/errors.h"
#include "remote/link.h"
#include "remote/record_sync_control.h"
#include "remote/testing/fake_client_history_source.h"
#include "remote/testing/fake_record_sync.h"
#include "remote/testing/fake_remote_engine.h"

using namespace fv;
using namespace fv::remote;

// ---- errors ----

TEST_CASE("retry hints are seconds from a non-negative number of milliseconds", "[errors]") {
    CHECK(retry_after_seconds(1000) == 1.0);
    CHECK(retry_after_seconds(250.0) == 0.25);
    CHECK(retry_after_seconds(0) == 0.0);
    CHECK_FALSE(retry_after_seconds(-1));
    CHECK_FALSE(retry_after_seconds(true)); // a bool is not a number here
    CHECK_FALSE(retry_after_seconds("1000"));
    CHECK_FALSE(retry_after_seconds(nullptr));
    CHECK_FALSE(retry_after_seconds(std::numeric_limits<double>::quiet_NaN()));
    CHECK(std::isinf(*retry_after_seconds(std::numeric_limits<double>::infinity())));
}

TEST_CASE("a busy host and a refused request say whether to retry", "[errors]") {
    const RemoteHostBusy busy("The host is busy. Try again shortly.", 1000);
    CHECK(busy.retryable());
    CHECK(busy.retry_after_s == 1.0);
    CHECK(std::string(busy.what()) == "The host is busy. Try again shortly.");
    const RemoteEngineError& base = busy;
    CHECK(dynamic_cast<const RemoteHostBusy*>(&base) != nullptr);

    const RemoteRequestError request("The host is busy. Try again shortly.", "busy", 1000);
    CHECK(request.retryable());
    CHECK(request.code == "busy");
    CHECK(request.retry_after_s == 1.0);
    const RemoteRequestError reloading("The host is loading its speech engine.",
                                       "engine_reloading");
    CHECK_FALSE(reloading.retryable());
    const RemoteRequestError plain("Unknown operation: 'x'", 5, "soon");
    CHECK_FALSE(plain.code);
    CHECK_FALSE(plain.retry_after_s);
    // Not a RemoteEngineError, as in Python (a RuntimeError).
    const std::runtime_error& runtime = plain;
    CHECK(dynamic_cast<const RemoteEngineError*>(&runtime) == nullptr);
}

TEST_CASE("lost connections, refusals and cancels keep what they carry", "[errors]") {
    const RemoteConnectionLost lost("Lost the connection to devbox:47821.", true);
    CHECK(lost.sent);
    const RemoteUnreachable refused("Couldn't reach devbox:47821: nothing there is accepting "
                                    "connections on that port.",
                                    true);
    CHECK(refused.refused);
    CHECK_FALSE(RemoteUnreachable("Couldn't find a computer named devbox.").refused);
    const PairingCanceled canceled("Pairing canceled.");
    CHECK(dynamic_cast<const RemoteEngineError*>(&canceled) != nullptr);
    const RemoteDecodeNotStarted not_started("x");
    CHECK(dynamic_cast<const RemoteEngineError*>(&not_started) != nullptr);
}

TEST_CASE("client history errors keep a known code and its sentence", "[errors]") {
    const auto [code, kept, message] = GENERATE(table<std::string, std::string, std::string>({
        {"unavailable", "unavailable", "Client history is unavailable. The client may be offline."},
        {"sharing_disabled", "sharing_disabled", "History sharing is disabled on this client."},
        {"not_found", "not_found", "Record not found on this client."},
        {"invalid_query", "invalid_query", "Invalid client history query."},
        {"snapshot_unavailable", "snapshot_unavailable",
         "This client's saved insights are unavailable."},
        {"unsupported", "unsupported", "Direct client queries are not available on this server."},
        {"bogus", "unavailable", "Client history is unavailable. The client may be offline."},
    }));
    const ClientUnavailable error(code);
    CHECK(error.code() == kept);
    CHECK(std::string(error.what()) == message);
    CHECK(ClientUnavailable().code() == "unavailable");
    CHECK(ClientUnavailable(Json(7)).code() == "unavailable");
    CHECK(ClientUnavailable(Json("not_found")).code() == "not_found");
}

// ---- the engine card's data ----

TEST_CASE("host models come from a list of objects with a family and a model", "[link]") {
    CHECK_FALSE(parse_host_models(nullptr));
    CHECK_FALSE(parse_host_models(Json::object()));
    CHECK(parse_host_models(Json::array())->empty());
    const Json raw = Json::parse(R"([
        {"family": "parakeet", "model": "parakeet-v3", "label": "Parakeet v3"},
        {"family": "parakeet", "model": "parakeet-v2"},
        {"family": "parakeet", "model": "x", "label": ""},
        {"family": "parakeet", "model": "y", "label": 5},
        {"family": "", "model": "z"}, {"family": "w", "model": 3}, "not a dict", null
    ])");
    const auto models = parse_host_models(raw);
    REQUIRE(models);
    REQUIRE(models->size() == 4);
    CHECK((*models)[0] == HostModel{"parakeet", "parakeet-v3", "Parakeet v3"});
    CHECK((*models)[1].label == "parakeet-v2");
    CHECK((*models)[2].label == "x");
    CHECK((*models)[3].label == "5");
    CHECK((*models)[0].key() == std::pair<std::string, std::string>("parakeet", "parakeet-v3"));
}

TEST_CASE("network time is what the host did not spend", "[link]") {
    CHECK(RemoteTiming{"devbox", 2, 1.5, 0.5}.network_s() == 1.0);
    CHECK(RemoteTiming{"devbox", 1, 0.2, 0.5}.network_s() == 0.0);
    CHECK_FALSE(RemoteTiming{"devbox", 1, 0.2, std::nullopt}.network_s());
    RemoteLink a;
    RemoteLink b;
    CHECK(a == b);
    b.beat = 1;
    CHECK_FALSE(a == b);
    CHECK(a.state == "unpaired");
}

// ---- the UI contracts' helpers ----

TEST_CASE("a nearby host pairs over the tailnet only without a code", "[control]") {
    LanHost lan;
    lan.address = "192.168.1.20";
    lan.port = 47821;
    TailnetHost tailnet;
    tailnet.peer.name = "devbox";
    tailnet.peer.address = "100.64.0.2";
    tailnet.peer.mine = true;
    tailnet.port = 47821;
    tailnet.compatible = true;
    tailnet.tailscale_pairing = true;
    CHECK(tailnet.address() == "100.64.0.2:47821");
    CHECK(tailnet.can_pair_without_code());

    NearbyHost both{"DEVBOX", std::string(64, 'A'), Json::object(), true, true, lan, tailnet};
    CHECK(both.name() == "devbox");
    CHECK(both.can_pair_without_code());
    CHECK(both.address() == "100.64.0.2:47821");

    tailnet.peer.mine = false;
    NearbyHost theirs{"DEVBOX", "", Json::object(), true, true, lan, tailnet};
    CHECK_FALSE(theirs.can_pair_without_code());
    CHECK(theirs.address() == "192.168.1.20:47821");

    NearbyHost lan_only{"DEVBOX", "", Json::object(), true, true, lan, std::nullopt};
    CHECK(lan_only.name() == "DEVBOX");
    CHECK(lan_only.address() == "192.168.1.20:47821");
    NearbyHost neither{"DEVBOX", "", Json::object(), true, true, std::nullopt, std::nullopt};
    CHECK(neither.address().empty());
}

// ---- the fakes ----

TEST_CASE("the fake engine answers from its script and records calls", "[fakes]") {
    testing::FakeRemoteEngine engine;
    ManualExecutor executor;
    std::vector<std::string> events;
    auto subscription =
        engine.changed.connect(executor, [&](const std::string& kind) { events.push_back(kind); });

    engine.state.port = 5000;
    CHECK(engine.host_state().port == 5000);
    engine.set_host_enabled(true);
    CHECK(engine.host_state().running);
    CHECK(engine.open_pairing() == "123456");
    CHECK(engine.host_state().pairing == std::pair<std::string, double>("123456", 300.0));

    engine.state.pair_request = PairRequestInfo{"r1", "laptop", "192.168.1.30", "123456", 120};
    CHECK_FALSE(engine.answer_pair_request("other", true));
    CHECK(engine.answer_pair_request("r1", true));
    CHECK_FALSE(engine.pending_pair_request());

    const ClientPairing paired = engine.pair("192.168.1.20:5000", "123456", false);
    CHECK(paired.host == "192.168.1.20");
    CHECK(paired.port == 5000);
    CHECK(engine.client_pairing() == paired);
    const auto renamed = engine.rename_host(" Studio PC ");
    REQUIRE(renamed);
    CHECK(renamed->host_name == "Studio PC");
    CHECK(renamed->paired_name == "192.168.1.20");

    engine.fail_next(std::make_exception_ptr(RemoteUnreachable("Couldn't reach it.", true)));
    CHECK_THROWS_AS(engine.pair("devbox", "1", false), RemoteUnreachable);
    CHECK_NOTHROW(engine.pair("devbox", "1", false));

    std::vector<std::string> shown;
    CancelSource cancel;
    cancel.cancel();
    CHECK_THROWS_AS(engine.request_pairing(
                        "devbox", [&](std::string sas) { shown.push_back(sas); }, cancel.token()),
                    PairingCanceled);
    CHECK(shown == std::vector<std::string>{"123456"});

    engine.emit_changed("activity");
    executor.run_all();
    CHECK(events == std::vector<std::string>{"state", "pairing", "pair_request", "client",
                                             "host_renamed", "client", "activity"});
    const auto calls = engine.calls();
    CHECK(calls.front() == "host_state()");
    CHECK(std::find(calls.begin(), calls.end(), "set_host_enabled(true)") != calls.end());
    CHECK(std::find(calls.begin(), calls.end(), "pair(192.168.1.20:5000,123456,false)") !=
          calls.end());
}

TEST_CASE("the fake record sync queues, lists and pauses", "[fakes]") {
    testing::FakeRecordSync sync;
    RecordSync& history_side = sync;
    history_side.record_saved("dictation", "r1");
    history_side.cleared("dictation");
    const std::vector<std::string> keep = {"m1", "m2"};
    sync.cleared("meeting", keep);
    CHECK(sync.calls() == std::vector<std::string>{"record_saved(dictation,r1)",
                                                   "cleared(dictation)",
                                                   "cleared(meeting,[m1,m2])"});

    CHECK_THROWS_WITH(sync.set_location("cloud"), "Unknown storage location: 'cloud'");
    sync.set_location("both");
    CHECK(sync.location() == "both");
    CHECK(sync.status().location == "both");

    sync.listing["dictation"] = {Json{{"id", "h1"}, {"text", "hello"}}};
    CHECK(sync.list_remote("dictation", "", 100).size() == 1);
    CHECK(sync.remote_item("dictation", "h1")->at("text") == "hello");
    CHECK_FALSE(sync.remote_item("meeting", "h1"));

    sync.fail_next(
        std::make_exception_ptr(RecordsUnavailable("devbox isn't reachable right now.")));
    CHECK_THROWS_AS(sync.list_remote("dictation", "", 100), RecordsUnavailable);

    {
        SyncPause pause = sync.paused_for_backup();
        CHECK(sync.pauses() == 1);
        SyncPause moved = std::move(pause);
        CHECK(sync.pauses() == 1);
    }
    CHECK(sync.pauses() == 0);

    sync.progress_lines = {"Bringing back 1…", "Bringing back 2…"};
    sync.bring_back_result = 2;
    std::vector<std::string> progress;
    CHECK(sync.bring_back([&](std::string line) { progress.push_back(line); }) == 2);
    CHECK(progress == sync.progress_lines);
}

TEST_CASE("the fake client history answers, refuses and records", "[fakes]") {
    testing::FakeClientHistorySource source;
    source.set_clients({{"d1", "laptop", "online"}, {"d2", "desk", "unavailable"}});
    CHECK(source.clients().size() == 2);
    source.answer("d1", "list", Json{{"entries", Json::array()}});
    CHECK((source.query("d1", "list", Json::object()) == Json{{"entries", Json::array()}}));
    try {
        (void)source.query("d2", "list", Json::object());
        FAIL("expected ClientUnavailable");
    } catch (const ClientUnavailable& error) {
        CHECK(error.code() == "unavailable");
    }
    source.fail("d1", "sharing_disabled");
    try {
        (void)source.query("d1", "list", Json::object());
        FAIL("expected ClientUnavailable");
    } catch (const ClientUnavailable& error) {
        CHECK(error.code() == "sharing_disabled");
    }
    CHECK(source.queries().size() == 3);
}
