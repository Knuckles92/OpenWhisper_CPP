#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <thread>
#include <utility>

#include "../remote_host/host_rig.h"
#include "agentapi/history_store.h"
#include "base/testing/manual_executor.h"
#include "db/database.h"
#include "remote/connection.h"
#include "remote_host/history_broker.h"
#include "testkit/testkit.h"
#include "ui_remote/history_publication.h"

using fv::Json;
using fv::remote_history::publication_reply;

namespace {
Json request(const char* operation, Json params = Json::object()) {
    return {{"type", "history_query"},
            {"id", "history-1"},
            {"operation", operation},
            {"params", std::move(params)}};
}
} // namespace

TEST_CASE("session history replies project only personal records and disclosed fields") {
    fv::testkit::TempDataRoot root;
    const auto path = root.path() / "history.db";
    auto test = std::async(std::launch::async, [path] {
        auto db = fv::Database::open_or_create(path);
        db.write([](fv::sql::Connection& connection) {
            connection.exec(
                "INSERT INTO transcription_history "
                "(id, text, raw_text, timestamp, model, audio_file, source_name, origin_device_id) "
                "VALUES ('local', 'shared text', 'private raw', '2026-04-01T14:00:00', 'test', "
                "'private recording', 'private filename', NULL), "
                "('foreign', 'other device text', NULL, '2026-04-02T14:00:00', 'test', "
                "NULL, NULL, 'another-device')");
        });
        fv::agentapi::HistoryStore store(path);
        const auto listing = publication_reply(store, request("transcriptions", {{"limit", 20}}));
        const auto items = listing.at("result").at("items");
        REQUIRE(items.size() == 1);
        CHECK(items.front().at("id") == "local");
        CHECK(items.front().at("preview") == "shared text");
        const auto detail =
            publication_reply(store, request("transcription", {{"record_id", "local"}}));
        CHECK(detail.at("result").at("text") == "shared text");
        CHECK(detail.dump().find("private") == std::string::npos);
        CHECK(publication_reply(store, request("transcription", {{"record_id", "foreign"}}))
                  .at("error") == "not_found");
        CHECK(publication_reply(store, request("segments", {{"meeting_id", "example"}}))
                  .at("error") == "invalid_query");
        CHECK(publication_reply(store, request("transcriptions", {{"limit", 21}})).at("error") ==
              "invalid_query");
        CHECK(publication_reply(store, request("transcriptions", {{"include_remote", true}}))
                  .at("error") == "invalid_query");
    });
    test.get();
}

TEST_CASE("canceling queued history publication never contacts the paired peer") {
    fv::ManualExecutor executor;
    int peerLookups = 0;
    auto publication = std::make_shared<fv::remote_history::Publication>(
        executor, std::filesystem::path(),
        [&]() -> std::optional<fv::remote::ClientPairing> {
            ++peerLookups;
            return std::nullopt;
        },
        fv::remote_history::Publication::Connect{});
    int ended = 0;
    const auto status = [&](std::string, bool finished) {
        if (finished) {
            ++ended;
        }
    };
    const auto generation = publication->start("initial", status);
    REQUIRE(generation);
    CHECK_FALSE(publication->start("duplicate", status));
    publication->stop(generation);
    executor.run_all();
    CHECK(peerLookups == 0);
    CHECK(ended == 1);
    const auto next = publication->start("another", status);
    REQUIRE(next);
    publication->stop(generation);
    executor.run_all();
    CHECK(peerLookups == 1);
    CHECK(ended == 2);
}

TEST_CASE("session history publication serves only the granted loopback host until stopped") {
    using namespace std::chrono_literals;
    fv::testkit::TempDataRoot root;
    auto test = std::async(std::launch::async, [path = root.path() / "history.db"] {
        auto db = fv::Database::open_or_create(path);
        db.write([](fv::sql::Connection& connection) {
            connection.exec(
                "INSERT INTO transcription_history (id, text, timestamp, model) "
                "VALUES ('hello', 'synthetic dictation', '2026-04-01T14:00:00', 'test')");
        });
        std::unique_ptr<fv::remote_host::HistoryBroker> broker;
        fv::remote_host::rig::HostRig host([&](fv::remote_host::SpeechHostDeps& deps) {
            broker = std::make_unique<fv::remote_host::HistoryBroker>(deps.registry, 2s);
            deps.history = broker.get();
        });
        const auto credentials = host.pair();
        REQUIRE_FALSE(credentials.token.empty());
        fv::remote::ClientPairing pair;
        pair.host = "127.0.0.1";
        pair.port = host.port;
        pair.fingerprint = credentials.fingerprint;
        pair.device_id = credentials.device_id;
        fv::remote::ClientEnv env{host.reactor};
        fv::ThreadPool worker("synthetic-history", 1);
        auto publication = std::make_shared<fv::remote_history::Publication>(
            worker, path, [&] { return std::make_optional(pair); },
            [&](const fv::remote::ClientPairing& peer) {
                return std::make_shared<fv::remote::RemoteConnection>(
                    env, peer.host, peer.port, credentials.token, peer.fingerprint,
                    std::vector<std::string>{}, true);
            });
        std::promise<bool> ready;
        auto online = ready.get_future();
        std::promise<void> ended;
        auto stopped = ended.get_future();
        const auto generation = publication->start(pair.fingerprint, [&](std::string, bool done) {
            if (done) {
                ended.set_value();
            } else {
                ready.set_value(true);
            }
        });
        // Stop before any test assertion can unwind the IO pool.
        struct Stop {
            fv::remote_history::Publication& publication;
            ~Stop() { publication.stop(); }
        } scoped{*publication};
        REQUIRE(generation);
        REQUIRE(online.wait_for(4s) == std::future_status::ready);
        CHECK_THROWS_AS(broker->query(pair.device_id, "transcription", {{"record_id", "hello"}}),
                        fv::remote::ClientUnavailable);
        const auto items = broker->query(pair.device_id, "transcriptions", {{"limit", 20}});
        REQUIRE(items.at("items").size() == 1);
        CHECK(items.at("items").at(0).at("id") == "hello");
        const auto item = broker->query(pair.device_id, "transcription", {{"record_id", "hello"}});
        CHECK(item.at("text") == "synthetic dictation");
        publication->stop(generation);
        REQUIRE(stopped.wait_for(4s) == std::future_status::ready);
        CHECK_THROWS_AS(broker->query(pair.device_id, "transcriptions", {{"limit", 20}}),
                        fv::remote::ClientUnavailable);
    });
    test.get();
}
