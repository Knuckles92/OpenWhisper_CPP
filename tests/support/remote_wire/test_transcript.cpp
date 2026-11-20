// fv_remote_wire_testkit's own tests: compare (normalized values, bindings across frames,
// readable differences), render, the binary split, the committed transcripts, and ScriptedPeer
// round trips with M9.1's ws_connect on 127.0.0.1.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "base/identity.h"
#include "net/reactor.h"
#include "net/tls.h"
#include "net/ws_client.h"
#include "remote_wire/scripted_peer.h"
#include "remote_wire/transcript.h"

using namespace fv::testing::remote_wire;
using namespace std::chrono_literals;

namespace {

const std::string kToken = "Ab0_Ab0-Ab0_Ab0-Ab0_Ab0-Ab0_Ab0-Ab0_Ab0-Ab0"; // 43 characters
const std::string kOtherToken = "Zz9_Zz9-Zz9_Zz9-Zz9_Zz9-Zz9_Zz9-Zz9_Zz9-Zz9";
const std::string kDevice = "0123456789abcdef0123456789abcdef";
const std::string kFingerprint = "00112233445566778899AABBCCDDEEFF00112233445566778899AABBCCDDEEFF";
const std::string kNonce = "ab00ab00ab00ab00ab00ab00ab00ab00ab00ab00ab00ab00ab00ab00ab00ab00";

const fv::net::TlsIdentity& identity_for_tests() {
    static const fv::net::TlsIdentity identity =
        fv::net::generate_self_signed_p256("FioriVoce host test");
    return identity;
}

// A small transcript in the recorder's format: a pairing reply, the hello that uses its token,
// a reply with host_ms, a refusal naming the product, a commit and its reveal, a close.
Transcript synthetic() {
    Json frames = Json::array();
    const auto text = [&](const char* dir, std::string body) {
        frames.push_back({{"step", "s"},
                          {"conn", 0},
                          {"dir", dir},
                          {"kind", "text"},
                          {"text", std::move(body)}});
    };
    text("s2c", R"({"type":"paired","token":"<token#1>","device_id":"<device_id#1>",)"
                R"("host":{"name":"studio","fingerprint":"<fingerprint#1>"}})");
    text("c2s", R"({"type": "hello", "protocol": 1, "token": "<token#1>"})");
    text("s2c", R"({"id":1,"result":{},"host_ms":"<host_ms#1>"})");
    text("s2c", R"({"id":2,"error":"Update OpenWhisper on the host to keep records there."})");
    text("s2c", R"({"type":"pair_commit","commit":"<commit#1>","timeout_s":120})");
    text("s2c", R"({"type":"pair_reveal","nonce":"<nonce#1>"})");
    frames.push_back({{"step", "s"},
                      {"conn", 0},
                      {"dir", "s2c"},
                      {"kind", "close"},
                      {"code", 4401},
                      {"reason", "unauthorized"}});
    frames.push_back({{"step", "t"},
                      {"conn", 1},
                      {"dir", "c2s"},
                      {"kind", "binary"},
                      {"header", R"({"id":1,"op":"transcribe","language":"en"})"},
                      {"payload_hex", "0001ff7f"}});
    frames.push_back({{"step", "t"},
                      {"conn", 1},
                      {"dir", "c2s"},
                      {"kind", "binary"},
                      {"header", R"({"id":2,"op":"transcribe"})"},
                      {"payload_sha256", "0000"},
                      {"payload_len", 70000},
                      {"payload_fill", "00"}});
    frames.push_back({{"step", "u"},
                      {"conn", 2},
                      {"dir", "s2c"},
                      {"kind", "http"},
                      {"status", 404},
                      {"body", "Not here\n"}});
    const Json normalize = Json::parse(R"([
        {"path": "0/token", "as": "token"}, {"path": "0/device_id", "as": "device_id"},
        {"path": "0/host/fingerprint", "as": "fingerprint"}, {"path": "1/token", "as": "token"},
        {"path": "2/host_ms", "as": "host_ms"}, {"path": "3/error", "as": "product"},
        {"path": "4/commit", "as": "commit", "of": "<nonce#1>"},
        {"path": "5/nonce", "as": "nonce"}])");
    return parse_transcript(
        Json{{"scenario", "synthetic"}, {"frames", frames}, {"normalize", normalize}});
}

std::string paired(const std::string& token) {
    return R"({"type":"paired","token":")" + token + R"(","device_id":")" + kDevice +
           R"(","host":{"name":"studio","fingerprint":")" + kFingerprint + R"("}})";
}

std::vector<std::byte> bytes_of(std::string_view text) {
    std::vector<std::byte> out;
    for (char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

} // namespace

TEST_CASE("compare accepts normalized values of the recorded shape and binds them") {
    const Transcript transcript = synthetic();
    Json bindings;
    CHECK(compare(transcript, 0, text_frame("s2c", paired(kToken)), bindings).empty());
    CHECK(bindings["<token#1>"] == kToken);
    CHECK(bindings["<device_id#1>"] == kDevice);
    CHECK(bindings["<fingerprint#1>"] == kFingerprint);
    CHECK(compare(transcript, 1,
                  text_frame("c2s",
                             R"({"type": "hello", "protocol": 1, "token": ")" + kToken + R"("})"),
                  bindings)
              .empty());
}

TEST_CASE("values named alike must agree across frames") {
    const Transcript transcript = synthetic();
    Json bindings;
    REQUIRE(compare(transcript, 0, text_frame("s2c", paired(kToken)), bindings).empty());
    const std::string diff = compare(
        transcript, 1,
        text_frame("c2s", R"({"type": "hello", "protocol": 1, "token": ")" + kOtherToken + "\"}"),
        bindings);
    CHECK(diff.find("<token#1> was " + kToken) != std::string::npos);
    CHECK(diff.find("frame 1") != std::string::npos);
    CHECK(bindings["<token#1>"] == kToken);

    Json prebound = {{"<fingerprint#1>", "FF" + kFingerprint.substr(2)}};
    CHECK_FALSE(compare(transcript, 0, text_frame("s2c", paired(kToken)), prebound).empty());
}

TEST_CASE("a normalized value of another shape is a difference") {
    const Transcript transcript = synthetic();
    Json bindings;
    const std::string diff =
        compare(transcript, 0, text_frame("s2c", paired("short-token")), bindings);
    CHECK(diff.find("<token#1> should be a token") != std::string::npos);
    CHECK(bindings.empty());

    CHECK(compare(transcript, 2, text_frame("s2c", R"({"id":1,"result":{},"host_ms":12.3})"),
                  bindings)
              .empty());
    CHECK_FALSE(compare(transcript, 2, text_frame("s2c", R"({"id":1,"result":{},"host_ms":12.34})"),
                        bindings)
                    .empty());
    CHECK_FALSE(compare(transcript, 2,
                        text_frame("s2c", R"({"id":1,"result":{},"host_ms":"12.3"})"), bindings)
                    .empty());
}

TEST_CASE("a difference names the frame and shows both texts where they part") {
    const Transcript transcript = synthetic();
    Json bindings;
    const std::string diff = compare(
        transcript, 1,
        text_frame("c2s", R"({"type":"hello","protocol":1,"token":")" + kToken + "\"}"), bindings);
    CHECK(diff.find("synthetic frame 1 (step \"s\", conn 0, c2s text)") != std::string::npos);
    CHECK(diff.find("differs at byte 8") != std::string::npos);
    CHECK(diff.find("expected: {\"type\": \"hello\"") != std::string::npos);
    CHECK(diff.find("actual:   {\"type\":\"hello\"") != std::string::npos);
    CHECK_FALSE(compare(transcript, 1, close_frame("c2s", 1000, ""), bindings).empty());
    CHECK(compare(transcript, 99, close_frame("c2s", 1000, ""), bindings).find("past the end") !=
          std::string::npos);
}

TEST_CASE("product names in copy are this product's") {
    const Transcript transcript = synthetic();
    Json bindings;
    const std::string ours = std::string(R"({"id":2,"error":"Update )") +
                             std::string(fv::identity().product) +
                             R"( on the host to keep records there."})";
    const std::string theirs =
        R"({"id":2,"error":"Update OpenWhisper on the host to keep records there."})";
    CHECK(compare(transcript, 3, text_frame("s2c", ours), bindings).empty());
    CHECK_FALSE(compare(transcript, 3, text_frame("s2c", theirs), bindings).empty());
    CHECK(compare(transcript, 3, text_frame("s2c", theirs), bindings, kRecordedProduct).empty());
}

TEST_CASE("product names in copy take the article this product's name needs") {
    CHECK(with_product("The host is preparing an OpenWhisper update.", "FioriVoce") ==
          "The host is preparing a FioriVoce update.");
    CHECK(with_product("An OpenWhisper host", "FioriVoce") == "A FioriVoce host");
    CHECK(with_product("an OpenWhisper update", "Ombra") == "an Ombra update");
    CHECK(with_product("Update OpenWhisper on both computers", "FioriVoce") ==
          "Update FioriVoce on both computers");
    CHECK(with_product("Klan OpenWhisper, OpenWhisper", "FioriVoce") ==
          "Klan FioriVoce, FioriVoce");
    CHECK(with_product("an OpenWhisper update", kRecordedProduct) == "an OpenWhisper update");

    // The update hold of remote_requests, as a FioriVoce host words it (M9.md, the wire).
    const Transcript transcript = load_transcript("requests");
    REQUIRE(transcript.frames.size() > 67);
    REQUIRE(transcript.frames[67].text.find("preparing an OpenWhisper update") !=
            std::string::npos);
    Json bindings;
    const std::string ours = R"({"id":1,"code":"engine_reloading","error":"The host is )"
                             R"(preparing a FioriVoce update. Try again shortly."})";
    CHECK(compare(transcript, 67, text_frame("s2c", ours), bindings, "FioriVoce").empty());
    const std::string wrong_article = R"({"id":1,"code":"engine_reloading","error":"The host )"
                                      R"(is preparing an FioriVoce update. Try again shortly."})";
    CHECK_FALSE(
        compare(transcript, 67, text_frame("s2c", wrong_article), bindings, "FioriVoce").empty());
}

TEST_CASE("a commit must commit to the nonce the transcript names") {
    const Transcript transcript = synthetic();
    REQUIRE(transcript.commit_of.at("<commit#1>") == "<nonce#1>");
    Json bindings;
    const std::string commit = commitment_of(kNonce);
    CHECK(commit.size() == 64);
    REQUIRE(compare(transcript, 4,
                    text_frame("s2c", R"({"type":"pair_commit","commit":")" + commit +
                                          R"(","timeout_s":120})"),
                    bindings)
                .empty());
    CHECK(compare(transcript, 5,
                  text_frame("s2c", R"({"type":"pair_reveal","nonce":")" + kNonce + "\"}"),
                  bindings)
              .empty());

    Json broken;
    REQUIRE(compare(transcript, 4,
                    text_frame("s2c", R"({"type":"pair_commit","commit":")" + std::string(64, 'c') +
                                          R"(","timeout_s":120})"),
                    broken)
                .empty());
    CHECK(compare(transcript, 5,
                  text_frame("s2c", R"({"type":"pair_reveal","nonce":")" + kNonce + "\"}"), broken)
              .find("is not the commitment of <nonce#1>") != std::string::npos);
}

TEST_CASE("binary frames split as the recorder splits them and compare header and payload") {
    const auto split = binary_frame("c2s", bytes_of(std::string("\0\0\0\x02{}\x01", 7)));
    CHECK(split.has_header);
    CHECK(split.text == "{}");
    CHECK(split.payload.size() == 1);
    CHECK_FALSE(binary_frame("c2s", bytes_of(std::string("\0\x01", 2))).has_header);
    CHECK_FALSE(binary_frame("c2s", bytes_of(std::string("\0\0\0\x09{}", 6))).has_header);
    CHECK_FALSE(binary_frame("c2s", bytes_of(std::string("\0\0\0\x01\xff", 5))).has_header);
    CHECK(binary_frame("c2s", bytes_of(std::string("\0\0\0\x02\xc3\xa9", 6))).has_header);
    CHECK_FALSE(binary_frame("c2s", bytes_of(std::string("\0\0\0\x03\xed\xa0\x80", 7))).has_header);

    const Transcript transcript = synthetic();
    Json bindings;
    const std::string header = R"({"id":1,"op":"transcribe","language":"en"})";
    std::string bytes = std::string("\0\0\0", 3) + static_cast<char>(header.size()) + header +
                        std::string("\x00\x01\xff\x7f", 4);
    CHECK(compare(transcript, 7, binary_frame("c2s", bytes_of(bytes)), bindings).empty());
    bytes.back() = '\x7e';
    CHECK(compare(transcript, 7, binary_frame("c2s", bytes_of(bytes)), bindings)
              .find("the payload differs") != std::string::npos);
}

TEST_CASE("render fills placeholders from bindings, new values of their shape otherwise") {
    const Transcript transcript = synthetic();
    Json bindings = {{"<token#1>", kToken}};
    const WireFrame hello = render(transcript, 1, bindings);
    CHECK(hello.text == R"({"type": "hello", "protocol": 1, "token": ")" + kToken + "\"}");

    const WireFrame pairing = render(transcript, 0, bindings);
    CHECK(has_shape("device_id", bindings["<device_id#1>"].get<std::string>()));
    CHECK(has_shape("fingerprint", bindings["<fingerprint#1>"].get<std::string>()));
    Json replay;
    CHECK(compare(transcript, 0, pairing, replay).empty());

    const WireFrame commit = render(transcript, 4, bindings);
    const WireFrame reveal = render(transcript, 5, bindings);
    CHECK(bindings["<commit#1>"] == commitment_of(bindings["<nonce#1>"].get<std::string>()));
    CHECK(compare(transcript, 4, commit, replay).empty());
    CHECK(compare(transcript, 5, reveal, replay).empty());

    const WireFrame timed = render(transcript, 2, bindings);
    CHECK(compare(transcript, 2, timed, replay).empty());

    const WireFrame large = render(transcript, 8, bindings);
    CHECK(large.payload.size() == 70000);
    CHECK(wire_bytes(large).size() == 4 + large.text.size() + 70000);
    CHECK(has_shape("token", fresh_value("token")));
    CHECK(has_shape("host_ms", fresh_value("host_ms")));
    CHECK(has_shape("iso_time", fresh_value("iso_time")));
}

TEST_CASE("http and close frames compare by status, body, code and reason") {
    const Transcript transcript = synthetic();
    Json bindings;
    CHECK(compare(transcript, 9, http_frame("s2c", 404, "Not here\n"), bindings).empty());
    CHECK(compare(transcript, 9, http_frame("s2c", 404, ""), bindings).empty());
    CHECK_FALSE(compare(transcript, 9, http_frame("s2c", 404, "Other\n"), bindings).empty());
    CHECK_FALSE(compare(transcript, 9, http_frame("s2c", 403, ""), bindings).empty());
    CHECK(compare(transcript, 6, close_frame("s2c", 4401, "unauthorized"), bindings).empty());
    CHECK(compare(transcript, 6, close_frame("s2c", 4401, "bad_code"), bindings)
              .find("expected close 4401 \"unauthorized\"") != std::string::npos);
}

TEST_CASE("every committed transcript loads, and slices keep their normalize list") {
    for (const char* name : {"probe", "wrong_path", "protocol", "pair_code", "pair_tailscale",
                             "pair_approval", "hello_ready", "requests", "history"}) {
        INFO(name);
        const Transcript transcript = load_transcript(name);
        CHECK(transcript.scenario == std::string("remote_") + name);
        CHECK_FALSE(transcript.frames.empty());
        CHECK(transcript.setup.contains("steps"));
    }
    const Transcript pairing = load_transcript("remote_pair_code");
    const std::vector<std::string> steps{"right_code"};
    const Transcript right = only_steps(pairing, steps);
    REQUIRE(right.frames.size() == 4);
    REQUIRE(right.normalize.size() == 3);
    CHECK(right.normalize[0]["path"] == "1/token");
    CHECK(connections(right) == std::vector<int>{8});
    CHECK(step_setup(right, "right_code")["client"] == "pair_with_host");
    CHECK_THROWS(load_transcript("no_such_scenario"));
}

TEST_CASE("ScriptedPeer plays every recorded connection of every transcript to ws_connect") {
    for (const char* name : {"probe", "wrong_path", "protocol", "pair_code", "pair_tailscale",
                             "pair_approval", "hello_ready", "requests", "history"}) {
        INFO(name);
        const Transcript transcript = load_transcript(name);
        fv::net::Reactor reactor;
        ScriptedPeer peer(reactor, identity_for_tests(), transcript);
        Json bindings;
        for (int conn : connections(transcript)) {
            INFO("conn " << conn);
            const auto differences = play_client(reactor, peer.port(), transcript, conn, bindings,
                                                 {.product = std::string(kRecordedProduct)});
            CHECK(differences == std::vector<std::string>{});
        }
        CHECK(peer.wait_finished(10s));
        CHECK(peer.mismatches() == std::vector<std::string>{});
        CHECK(peer.connections() == connections(transcript).size());
        for (const auto& item : bindings.items()) {
            if (item.key().starts_with("<fingerprint#")) {
                CHECK(item.value() == peer.fingerprint());
            }
        }
    }
}

namespace {

// A raw ws_connect client that records what it got.
struct Client {
    std::mutex mutex;
    std::condition_variable changed;
    bool opened = false;
    std::optional<std::uint16_t> closed;
    std::optional<fv::net::WsConnectFailure> failed;
    std::vector<std::string> texts;

    std::unique_ptr<fv::net::WsClientConnection>
    connect(fv::net::Reactor& reactor, std::uint16_t port, std::optional<std::string> pin = {}) {
        fv::net::WsClientOptions options;
        options.url = "wss://127.0.0.1:" + std::to_string(port) + std::string(kEnginePath);
        options.pinned_sha256 = std::move(pin);
        options.on_open = [this] {
            set([&] { opened = true; });
        };
        options.on_text = [this](std::string_view text) {
            set([&] { texts.emplace_back(text); });
        };
        options.on_close = [this](std::uint16_t code, std::string_view) {
            set([&] { closed = code; });
        };
        options.on_connect_failed = [this](const fv::net::WsConnectFailure& failure) {
            set([&] { failed = failure; });
        };
        return fv::net::ws_connect(reactor, std::move(options));
    }
    template <class F> void set(F&& change) {
        {
            std::lock_guard lock(mutex);
            change();
        }
        changed.notify_all();
    }
    template <class P> bool wait(P&& done) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 10s, done);
    }
};

} // namespace

TEST_CASE("ScriptedPeer reports a client frame that differs from the record") {
    const std::vector<std::string> steps{"available"};
    const Transcript transcript = only_steps(load_transcript("probe"), steps);
    fv::net::Reactor reactor;
    ScriptedPeer peer(reactor, identity_for_tests(), transcript);
    Client client;
    auto socket = client.connect(reactor, peer.port());
    REQUIRE(client.wait([&] { return client.opened; }));
    socket->send_text(std::make_shared<const std::string>(R"({"type": "probe", "protocol": 2})"));
    REQUIRE(client.wait([&] { return client.closed.has_value(); }));
    CHECK(*client.closed == 1000);
    REQUIRE(client.texts.size() == 1);
    CHECK(client.texts[0].starts_with(R"({"type":"probe","protocol":1,)"));
    REQUIRE(peer.wait_finished(10s));
    const auto mismatches = peer.mismatches();
    REQUIRE(mismatches.size() == 1);
    CHECK(mismatches[0].find("frame 0") != std::string::npos);
    CHECK(mismatches[0].find("differs at byte 30") != std::string::npos);
    const auto received = peer.received();
    REQUIRE_FALSE(received.empty());
    CHECK(received[0].text == R"({"type": "probe", "protocol": 2})");
    CHECK(received[0].conn == 0);
}

TEST_CASE("ScriptedPeer refuses a connection the transcript does not have") {
    const std::vector<std::string> steps{"available"};
    const Transcript transcript = only_steps(load_transcript("probe"), steps);
    fv::net::Reactor reactor;
    ScriptedPeer peer(reactor, identity_for_tests(), transcript);
    Json bindings;
    CHECK(play_client(reactor, peer.port(), transcript, 0, bindings,
                      {.product = std::string(kRecordedProduct)})
              .empty());
    Client extra;
    auto socket = extra.connect(reactor, peer.port());
    REQUIRE(extra.wait([&] { return extra.closed.has_value() || extra.failed.has_value(); }));
    CHECK(extra.closed == std::optional<std::uint16_t>(1011));
    const auto mismatches = peer.mismatches();
    REQUIRE(mismatches.size() == 1);
    CHECK(mismatches[0].find("connection 2 was not recorded") != std::string::npos);
}

TEST_CASE("A host frame ScriptedPeer cannot play is reported, and the connection closed 1011") {
    // A host frame known only by its digest (no payload_fill) cannot be sent again.
    Json frames = Json::array();
    frames.push_back({{"step", "s"},
                      {"conn", 0},
                      {"dir", "c2s"},
                      {"kind", "text"},
                      {"text", R"({"type": "probe", "protocol": 1})"}});
    frames.push_back({{"step", "s"},
                      {"conn", 0},
                      {"dir", "s2c"},
                      {"kind", "binary"},
                      {"payload_sha256", std::string(64, '0')},
                      {"payload_len", 70000}});
    frames.push_back({{"step", "s"},
                      {"conn", 0},
                      {"dir", "s2c"},
                      {"kind", "close"},
                      {"code", 1000},
                      {"reason", ""}});
    const Transcript transcript = parse_transcript(
        Json{{"scenario", "unplayable"}, {"frames", frames}, {"normalize", Json::array()}});
    fv::net::Reactor reactor;
    ScriptedPeer peer(reactor, identity_for_tests(), transcript);
    Client client;
    auto socket = client.connect(reactor, peer.port());
    REQUIRE(client.wait([&] { return client.opened; }));
    socket->send_text(std::make_shared<const std::string>(R"({"type": "probe", "protocol": 1})"));
    REQUIRE(client.wait([&] { return client.closed.has_value(); }));
    CHECK(*client.closed == 1011);
    REQUIRE(peer.wait_finished(10s));
    const auto mismatches = peer.mismatches();
    REQUIRE(mismatches.size() == 1);
    CHECK(mismatches[0].find("unplayable conn 0: frame 1 cannot be played") != std::string::npos);
    CHECK(mismatches[0].find("known only by its digest") != std::string::npos);
}

TEST_CASE("A payload_fill that is not one byte is a malformed transcript") {
    for (const char* fill : {"", "0000"}) {
        INFO(fill);
        const Json frame = {{"step", "s"},          {"conn", 0},
                            {"dir", "c2s"},         {"kind", "binary"},
                            {"payload_sha256", ""}, {"payload_len", 70000},
                            {"payload_fill", fill}};
        CHECK_THROWS_WITH(parse_transcript(Json{{"scenario", "bad"},
                                                {"frames", Json::array({frame})},
                                                {"normalize", Json::array()}}),
                          Catch::Matchers::ContainsSubstring("payload_fill is not one byte"));
    }
}

TEST_CASE("A client pinned to another certificate sends the scripted peer nothing") {
    const std::vector<std::string> steps{"ready_full"};
    const Transcript transcript = only_steps(load_transcript("hello_ready"), steps);
    fv::net::Reactor reactor;
    ScriptedPeer peer(reactor, identity_for_tests(), transcript);
    Client client;
    auto socket = client.connect(reactor, peer.port(), std::string(64, 'A'));
    REQUIRE(client.wait([&] { return client.failed.has_value() || client.opened; }));
    CHECK_FALSE(client.opened);
    CHECK(client.failed->kind == fv::net::WsConnectFailure::Kind::Tls);
    socket.reset();
    CHECK(peer.received().empty());
}

TEST_CASE("A ScriptedPeer told to stop reading leaves later client frames in the socket") {
    const std::vector<std::string> steps{"pair", "transcribe"};
    const Transcript transcript = only_steps(load_transcript("requests"), steps);
    const std::vector<int> conns = connections(transcript);
    REQUIRE(conns.size() == 2);
    fv::net::Reactor reactor;
    ScriptedPeer peer(reactor, identity_for_tests(), transcript, {.stop_reading_after = 1});
    Json bindings;
    REQUIRE(play_client(reactor, peer.port(), transcript, conns[0], bindings,
                        {.product = std::string(kRecordedProduct)})
                .empty());

    Client client;
    auto socket = client.connect(reactor, peer.port());
    REQUIRE(client.wait([&] { return client.opened; }));
    std::vector<std::size_t> sent;
    for (std::size_t index : frames_of(transcript, conns[1])) {
        if (transcript.frames[index].dir == "c2s" && transcript.frames[index].kind != "close") {
            sent.push_back(index);
        }
    }
    REQUIRE(sent.size() == 5); // hello, two transcribes, describe, an unknown op
    socket->send_text(
        std::make_shared<const std::string>(render(transcript, sent[0], bindings).text));
    REQUIRE(client.wait([&] { return client.texts.size() == 1; })); // ready
    for (std::size_t i = 1; i < sent.size(); ++i) {
        socket->send_binary(wire_bytes(render(transcript, sent[i], bindings)));
    }
    // The hello was released; the frames after it are not, so reading stops once three of them
    // wait (websockets' max_queue=2 pauses above 2): the unknown op is never read.
    REQUIRE(client.wait([&] { return client.texts.size() == 4; }));
    std::this_thread::sleep_for(500ms);
    CHECK(client.texts.size() == 4);
    // The first connection's pair and close, then the hello, the two transcribes and describe.
    const auto received = peer.received();
    REQUIRE(received.size() == 6);
    CHECK(received.back().text == R"({"id":3,"op":"describe"})");
    CHECK(peer.mismatches() == std::vector<std::string>{});
}
