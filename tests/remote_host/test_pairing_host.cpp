// Pairing on the host (services/remote_asr/host.py _pair, _pair_tailscale, _pair_request,
// _await_answer; tests/test_remote_engine.py and tests/test_remote_pairing_ux.py): by code, by
// Tailscale, and by the owner's approval with six digits on both screens, over loopback TLS.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "base/hash.h"
#include "base/ids.h"
#include "base/testing/fake_clock.h"
#include "host_rig.h"
#include "remote/discovery_codec.h"
#include "remote/testing/fake_tailscale_cli.h"
#include "remote_host/pairing_host.h"

using Catch::Matchers::ContainsSubstring;
using fv::Json;
using namespace fv::remote_host::rig;
using namespace std::chrono_literals;

namespace {

// request_pairing's exchange (client.py:304-388) by hand: pair_request, pair_commit,
// pair_nonce, pair_reveal (checked against the commitment), then the host's answer, unless the
// person cancels first.
struct ApprovalRequest {
    explicit ApprovalRequest(std::uint16_t port, std::string name = "laptop")
        : thread([this, port, name] { run(port, name); }) {}
    ~ApprovalRequest() {
        canceled = true;
        if (thread.joinable()) {
            thread.join();
        }
    }

    void run(std::uint16_t port, const std::string& name) {
        RawClient client(port);
        if (!client.connected()) {
            failure = "could not connect: " + client.error();
            done = true;
            return;
        }
        client.send(Json{{"type", "pair_request"}, {"protocol", 1}, {"device_name", name}});
        auto commit = client.receive();
        if (!commit || commit->value("type", "") != "pair_commit") {
            answer = commit;
            done = true;
            return;
        }
        std::array<std::byte, 32> nonce{};
        fv::random_bytes(nonce);
        std::array<std::uint8_t, 32> client_nonce{};
        std::memcpy(client_nonce.data(), nonce.data(), nonce.size());
        client.send(Json{{"type", "pair_nonce"}, {"nonce", fv::to_hex(client_nonce)}});
        auto reveal = client.receive();
        if (!reveal || reveal->value("type", "") != "pair_reveal") {
            answer = reveal;
            done = true;
            return;
        }
        const std::string host_hex = reveal->at("nonce").get<std::string>();
        std::vector<std::uint8_t> host_nonce;
        for (std::size_t i = 0; i + 1 < host_hex.size(); i += 2) {
            host_nonce.push_back(
                static_cast<std::uint8_t>(std::stoi(host_hex.substr(i, 2), nullptr, 16)));
        }
        commitment_checks =
            fv::remote::pairing_commitment(host_nonce) == commit->at("commit").get<std::string>();
        {
            std::lock_guard lock(mutex);
            sas = fv::remote::pairing_sas(host_nonce, client_nonce, client.fingerprint());
        }
        while (!canceled) {
            if (auto reply = client.receive(50ms)) {
                answer = reply;
                client.wait_closed();
                close_code = client.close_code();
                done = true;
                return;
            }
            if (client.close_code()) {
                close_code = client.close_code();
                done = true;
                return;
            }
        }
        client.send(Json{{"type", "pair_cancel"}});
        client.close(1000);
        done = true;
    }

    std::string code() {
        std::lock_guard lock(mutex);
        return sas;
    }
    bool finish(std::chrono::milliseconds timeout = 5s) {
        return wait_for([&] { return done.load(); }, timeout);
    }

    std::mutex mutex;
    std::string sas;
    std::atomic<bool> canceled{false};
    std::atomic<bool> done{false};
    bool commitment_checks = false;
    std::optional<Json> answer;
    std::optional<std::uint16_t> close_code;
    std::string failure;
    std::thread thread;
};

constexpr std::string_view kOwner = "owner@example.com";

// test_remote_engine.py's tailnet_host: connections come from 127.0.0.1, so the Tailscale
// identity check is replaced and `accept` decides whether it recognizes the caller.
struct TailnetHost {
    TailnetHost()
        : rig([this](fv::remote_host::SpeechHostDeps& deps) {
              deps.tailscale_owner = [this] {
                  std::lock_guard lock(mutex);
                  return owner;
              };
              deps.addresses = [] {
                  return std::vector<std::string>{"192.0.2.10", "100.64.0.2"};
              };
              deps.own_device =
                  [this](std::string_view remote, std::string_view local,
                         std::string_view login) -> std::optional<fv::remote::PeerIdentity> {
                  std::lock_guard lock(mutex);
                  calls.emplace_back(std::string(remote) + "|" + std::string(local) + "|" +
                                     std::string(login));
                  if (!accept) {
                      return std::nullopt;
                  }
                  return fv::remote::PeerIdentity{"laptop", std::string(login), false};
              };
          }) {}

    std::mutex mutex;
    std::string owner{kOwner};
    bool accept = true;
    std::vector<std::string> calls;
    HostRig rig;
};

} // namespace

// ---- by code ------------------------------------------------------------------------------------

TEST_CASE("test_pairing_issues_a_token_the_host_stores_only_as_a_digest") {
    HostRig rig;
    const auto result = rig.pair();
    REQUIRE(!result.token.empty());
    CHECK(result.host.at("name") == "devbox");
    CHECK(result.fingerprint == test_identity().fingerprint);
    CHECK(result.host.at("fingerprint") == test_identity().fingerprint);
    CHECK(result.close_code == 1000);
    const Json devices = rig.store.devices();
    REQUIRE(devices.size() == 1);
    CHECK(devices[0].at("name") == "laptop");
    CHECK(devices[0].at("token_sha256") == fv::remote::token_digest(result.token));
    CHECK(devices[0].at("via") == "code");
    CHECK(devices.dump().find(result.token) == std::string::npos);
    // The code is single use.
    CHECK_FALSE(rig.host->pairing_status().has_value());
}

TEST_CASE("test_wrong_codes_are_refused_and_close_pairing") {
    HostRig rig;
    std::vector<std::string> events;
    InlineExecutor inline_executor;
    auto subscription =
        rig.host->events.connect(inline_executor, [&](std::string kind, Json detail) {
            events.push_back(kind + detail.dump());
        });
    rig.host->open_pairing();
    for (int i = 0; i < fv::remote_host::MAX_PAIRING_FAILURES - 1; ++i) {
        const auto refused = pair_with_code(rig.port, "000000");
        REQUIRE(refused.error.has_value());
        CHECK_THAT(refused.error->at("message").get<std::string>(),
                   ContainsSubstring("doesn't match"));
        CHECK(refused.close_code == fv::remote::CLOSE_UNAUTHORIZED);
        CHECK(refused.close_reason == "bad_code");
    }
    CHECK(rig.host->pairing_status().has_value());
    const auto last = pair_with_code(rig.port, "000000");
    REQUIRE(last.error.has_value());
    CHECK_THAT(last.error->at("message").get<std::string>(),
               ContainsSubstring("Too many wrong codes"));
    CHECK_FALSE(rig.host->pairing_status().has_value());
    CHECK(rig.store.devices().empty());
    CHECK(events.back() == R"(pairing{"open":false})");
}

TEST_CASE("test_pairing_is_refused_when_not_open") {
    HostRig rig;
    const auto refused = pair_with_code(rig.port, "123456");
    REQUIRE(refused.error.has_value());
    CHECK_THAT(refused.error->at("message").get<std::string>(), ContainsSubstring("isn't open"));
    CHECK(refused.error->at("code") == "pairing_closed");
}

TEST_CASE("an expired code is closed pairing, and the code is compared after stripping") {
    fv::FakeClock clock;
    HostRig rig({}, clock);
    rig.host->open_pairing("246810");
    const auto status = rig.host->pairing_status();
    REQUIRE(status.has_value());
    CHECK(status->first == "246810");
    CHECK(status->second == 300.0);
    // Only surrounding whitespace is stripped: a dashed code is a wrong one.
    CHECK(pair_with_code(rig.port, "24-68-10").error->at("code") == "bad_code");
    CHECK(pair_with_code(rig.port, " 246810 ").error == std::nullopt);
    rig.host->open_pairing("246810");
    clock.advance(301s);
    CHECK(pair_with_code(rig.port, "246810").error->at("code") == "pairing_closed");
    CHECK_FALSE(rig.host->pairing_status().has_value());
}

// ---- by Tailscale
// --------------------------------------------------------------------------------

TEST_CASE("test_owners_tailscale_computer_pairs_without_a_code") {
    TailnetHost fixture;
    const auto result = pair_with_tailscale(fixture.rig.port);
    REQUIRE(!result.token.empty());
    CHECK(result.host.at("addresses") == Json{"192.0.2.10", "100.64.0.2"});
    {
        std::lock_guard lock(fixture.mutex);
        CHECK(fixture.calls == std::vector<std::string>{"127.0.0.1|127.0.0.1|owner@example.com"});
    }
    CHECK(fixture.rig.store.devices()[0].at("via") == "tailscale");
    // The token works like any other.
    auto connection = connect(fixture.rig.port, result.token);
    CHECK(connection.client->request("transcribe", Json::object(), tone(160))
              .at("result")
              .at("text") == "heard 160");
}

TEST_CASE("a Tailscale pairing without a name takes the node's") {
    TailnetHost fixture;
    Json message =
        Json{{"type", "pair"}, {"protocol", 1}, {"device_name", ""}, {"tailscale", true}};
    const auto result = pair_message(fixture.rig.port, message);
    REQUIRE(!result.token.empty());
    CHECK(fixture.rig.store.devices()[0].at("name") == "laptop");
}

TEST_CASE("test_tailscale_pairing_is_refused_for_anyone_else") {
    TailnetHost fixture;
    fixture.accept = false;
    const auto refused = pair_with_tailscale(fixture.rig.port);
    REQUIRE(refused.error.has_value());
    CHECK_THAT(refused.error->at("message").get<std::string>(),
               ContainsSubstring("owner's own Tailscale computers"));
    CHECK(refused.close_reason == "tailscale_refused");
    CHECK(fixture.rig.store.devices().empty());
}

TEST_CASE("test_tailscale_pairing_is_refused_when_the_host_turned_it_off") {
    TailnetHost fixture;
    fixture.owner.clear();
    const auto refused = pair_with_tailscale(fixture.rig.port);
    REQUIRE(refused.error.has_value());
    CHECK_THAT(refused.error->at("message").get<std::string>(),
               ContainsSubstring("doesn't pair over Tailscale without a code"));
    CHECK(fixture.calls.empty());
    CHECK(fixture.rig.store.devices().empty());
}

TEST_CASE("test_code_pairing_still_works_and_carries_the_hosts_addresses") {
    TailnetHost fixture;
    const auto result = pair_with_code(fixture.rig.port, fixture.rig.host->open_pairing());
    REQUIRE(!result.token.empty());
    CHECK(result.host.at("addresses") == Json{"192.0.2.10", "100.64.0.2"});
    CHECK(fixture.rig.store.devices()[0].at("via") == "code");
}

TEST_CASE("a tailnet peer is vouched for by whois when both ends are Tailscale addresses") {
    fv::remote::testing::FakeTailscaleCli cli;
    cli.set_identity("100.101.102.50",
                     fv::remote::PeerIdentity{"laptop-node", "owner@example.com", false});
    HostRig rig([&](fv::remote_host::SpeechHostDeps& deps) {
        deps.tailscale = &cli;
        deps.tailscale_owner = [] {
            return std::string("owner@example.com");
        };
        deps.peer_address = [](std::string_view) {
            return std::string("100.101.102.50");
        };
    });
    // The local end is 127.0.0.1, not the tailnet's: whois is never asked.
    CHECK(pair_with_tailscale(rig.port).error->at("code") == "tailscale_refused");
    CHECK(cli.looked_up().empty());
}

// ---- by approval --------------------------------------------------------------------------------

TEST_CASE("an allowed request pairs with the same digits on both screens") {
    HostRig rig;
    std::vector<std::string> events;
    InlineExecutor inline_executor;
    auto subscription =
        rig.host->events.connect(inline_executor, [&](std::string kind, Json detail) {
            if (kind == "pair_request" || kind == "paired") {
                events.push_back(kind + detail.dump());
            }
        });
    ApprovalRequest request(rig.port);
    REQUIRE(wait_for(
        [&] { return rig.host->pending_request().has_value() && !request.code().empty(); }));
    const auto pending = *rig.host->pending_request();
    CHECK(pending.sas == request.code());
    CHECK(pending.sas.size() == 6);
    CHECK(pending.name == "laptop");
    CHECK(pending.address == "127.0.0.1");
    CHECK(pending.id.size() == 12);
    CHECK(pending.seconds_left > 100);
    CHECK(rig.host->answer_request(pending.id, true));
    REQUIRE(request.finish());
    CHECK(request.commitment_checks);
    REQUIRE(request.answer.has_value());
    CHECK(request.answer->at("type") == "paired");
    CHECK(rig.store.devices()[0].at("via") == "approval");
    CHECK_FALSE(rig.host->answer_request(pending.id, true));
    CHECK(events ==
          std::vector<std::string>{R"(pair_request{"name":"laptop","address":"127.0.0.1"})",
                                   R"(paired{"name":"laptop"})", "pair_request{}"});
}

TEST_CASE("test_a_denied_request_pairs_nothing") {
    HostRig rig;
    ApprovalRequest request(rig.port);
    REQUIRE(wait_for([&] { return rig.host->pending_request().has_value(); }, 5s));
    rig.host->answer_request(rig.host->pending_request()->id, false);
    REQUIRE(request.finish());
    REQUIRE(request.answer.has_value());
    CHECK_THAT(request.answer->at("message").get<std::string>(), ContainsSubstring("declined"));
    CHECK(request.close_code == fv::remote::CLOSE_UNAUTHORIZED);
    CHECK(rig.store.devices().empty());
}

TEST_CASE("test_canceling_here_takes_the_question_off_the_host") {
    HostRig rig;
    ApprovalRequest request(rig.port);
    REQUIRE(wait_for([&] { return rig.host->pending_request().has_value(); }, 5s));
    request.canceled = true;
    REQUIRE(request.finish());
    CHECK_FALSE(request.answer.has_value());
    CHECK(wait_for([&] { return !rig.host->pending_request().has_value(); }));
    CHECK(rig.store.devices().empty());
}

TEST_CASE("test_one_question_at_a_time") {
    HostRig rig;
    ApprovalRequest first(rig.port);
    REQUIRE(wait_for([&] { return rig.host->pending_request().has_value(); }, 5s));
    ApprovalRequest intruder(rig.port, "intruder");
    REQUIRE(intruder.finish());
    REQUIRE(intruder.answer.has_value());
    CHECK_THAT(intruder.answer->at("message").get<std::string>(),
               ContainsSubstring("already asking about another computer"));
    CHECK(intruder.answer->at("code") == "approval_busy");
    first.canceled = true;
}

TEST_CASE("test_an_address_that_keeps_asking_is_turned_away") {
    HostRig rig(
        [](fv::remote_host::SpeechHostDeps& deps) { deps.limits.max_approval_requests = 1; });
    {
        ApprovalRequest asked(rig.port);
        asked.canceled = true; // ask, then give up at once
        REQUIRE(asked.finish());
    }
    CHECK(wait_for([&] { return !rig.host->pending_request().has_value(); }));
    ApprovalRequest again(rig.port);
    REQUIRE(again.finish());
    REQUIRE(again.answer.has_value());
    CHECK_THAT(again.answer->at("message").get<std::string>(),
               ContainsSubstring("Too many pairing requests"));
}

TEST_CASE("test_requests_from_outside_this_network_are_refused") {
    HostRig rig([](fv::remote_host::SpeechHostDeps& deps) {
        deps.peer_address = [](std::string_view) {
            return std::string("8.8.8.8");
        };
    });
    ApprovalRequest request(rig.port);
    REQUIRE(request.finish());
    REQUIRE(request.answer.has_value());
    CHECK_THAT(request.answer->at("message").get<std::string>(),
               ContainsSubstring("only takes pairing requests from its own network"));
    CHECK(request.answer->at("code") == "approval_refused");
}

TEST_CASE("test_nobody_answering_turns_the_request_down") {
    HostRig rig([](fv::remote_host::SpeechHostDeps& deps) { deps.limits.approval = 500ms; });
    ApprovalRequest request(rig.port);
    REQUIRE(request.finish(5s));
    REQUIRE(request.answer.has_value());
    CHECK_THAT(request.answer->at("message").get<std::string>(),
               ContainsSubstring("Nobody answered on devbox"));
    CHECK(request.answer->at("code") == "pair_timeout");
}

TEST_CASE("test_turning_sharing_off_ends_a_waiting_request") {
    HostRig rig;
    ApprovalRequest request(rig.port);
    REQUIRE(wait_for([&] { return rig.host->pending_request().has_value(); }, 5s));
    rig.host->stop();
    REQUIRE(request.finish());
    REQUIRE(request.answer.has_value());
    CHECK_THAT(request.answer->at("message").get<std::string>(),
               ContainsSubstring("stopped sharing"));
}

// Adapted: the server's shutdown closes every connection, and the request's answer still
// arrives first, with its own close, because stop() waits for it (a slowed send cannot be
// injected into the host here).
TEST_CASE("test_a_slow_answer_still_beats_the_shutdown") {
    HostRig rig;
    ApprovalRequest request(rig.port);
    REQUIRE(wait_for([&] { return rig.host->pending_request().has_value(); }, 5s));
    rig.host->stop();
    REQUIRE(request.finish());
    REQUIRE(request.answer.has_value());
    CHECK(request.answer->at("code") == "pair_closed");
    CHECK(request.close_code == fv::remote::CLOSE_UNAUTHORIZED);
}

TEST_CASE("test_a_request_that_never_answers_holds_up_stopping_only_briefly") {
    fv::FakeClock clock;
    fv::remote_host::PairingHost pairing(clock, "devbox", {},
                                         [](std::string_view) { return true; });
    // Nothing is handling this request, so nothing will ever answer it.
    auto admitted = pairing.admit_request("laptop", "127.0.0.1");
    auto request = std::get<std::shared_ptr<fv::remote_host::PairRequest>>(admitted);
    pairing.reveal(request, "123456", {});
    const auto started = std::chrono::steady_clock::now();
    pairing.stop(300ms);
    CHECK(std::chrono::steady_clock::now() - started < 2s);
    CHECK(request->stopped);
    CHECK(request->done);
}

TEST_CASE("a waiting request expires and the window forgets old requests") {
    fv::FakeClock clock;
    fv::remote_host::PairingLimits limits;
    limits.max_approval_requests = 2;
    fv::remote_host::PairingHost pairing(clock, "devbox", limits,
                                         [](std::string_view) { return true; });
    auto first = std::get<std::shared_ptr<fv::remote_host::PairRequest>>(
        pairing.admit_request("laptop", "192.168.1.30"));
    // One question at a time, until the first expires (10 s + 120 s before it is revealed).
    CHECK(std::get<fv::remote_host::PairingRefusal>(pairing.admit_request("x", "192.168.1.31"))
              .code == "approval_busy");
    clock.advance(131s);
    CHECK(pairing.outcome(first, true) == fv::remote_host::ApprovalOutcome::timeout);
    pairing.finish(first);
    auto second = std::get<std::shared_ptr<fv::remote_host::PairRequest>>(
        pairing.admit_request("laptop", "192.168.1.30"));
    pairing.finish(second);
    CHECK(std::get<fv::remote_host::PairingRefusal>(pairing.admit_request("laptop", "192.168.1.30"))
              .code == "approval_limited");
    // Exactly 600 s after the first request it still counts; past that it no longer does.
    clock.advance(469s);
    CHECK(std::holds_alternative<fv::remote_host::PairingRefusal>(
        pairing.admit_request("laptop", "192.168.1.30")));
    clock.advance(1s);
    CHECK(std::holds_alternative<std::shared_ptr<fv::remote_host::PairRequest>>(
        pairing.admit_request("laptop", "192.168.1.30")));
}

TEST_CASE("pairing codes are six digits") {
    for (int i = 0; i < 50; ++i) {
        const std::string code = fv::remote_host::new_pairing_code();
        REQUIRE(code.size() == 6);
        CHECK(code.find_first_not_of("0123456789") == std::string::npos);
    }
}
